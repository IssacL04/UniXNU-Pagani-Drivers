// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The start (class registration of the service file, then the service) follows UniXNU-sheng package/BatteryProbe.cpp, and the DSP
// client callbacks (copy into a small ring, wake the owner) package/ShengBattery.cpp (https://github.com/UniXNU/UniXNU-sheng, commit
// 08cde20e); the thread, the gauge bus and the two sources are the 13T's.
//
// Kernel side of PaganiBattery (PaganiBattery.h, README.md). pagani_battery_start (Stage7Probe, after the GPU and the ADSP start) reads
// its boot arguments and returns at once without bit 0 of pagani-battery (pagani-adsp=1 does not imply it: every step of a staged bring-up
// names what it turns on). Otherwise it registers the class of PaganiBatteryService.cpp, creates the service
// (not yet visible) and starts one kernel thread, "pagani-battery", which
//   1 brings up the gauge's I2C (BatteryI2c.c: GCC votes, the engine, the pins) and asks the gauge its device type,
//   2 registers the battery manager channel "PMIC_RTR_ADSP_APPS" with PaganiAdsp (pagani-adsp=1; Android's 5 intents of 1 KiB),
//   3 then runs BatteryCore until reboot: gauge passes, battery manager requests, the charging policy, publication, the log.
// The DSP's callbacks only copy into a 16-message ring and wake the thread. Nothing panics or waits on the boot: every failure is a
// stage in pagani_battery_status[0] and a "[battery]" line in the boot log (BootProgress ramoops U records, pagani-ramoops=1).
// With the other bits of pagani-battery the same thread reads the charger PMIC's state (bits 2, 3) and, 120 s after it started, runs the
// USB probe one step at a time between its other work (bit 1, BatteryUsb.h): one thread, so the observer channels have one user.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BatteryCore.h"
#include "BatteryI2c.h"
#include "BatterySpmi.h"
#include "BatteryText.h"
#include "BatteryUsb.h"
#include "PaganiBattery.h"
#ifndef PAGANI_BATTERY_NO_ADSP
// PaganiAdsp's interface (drivers/PaganiAdsp, on the include path from the build); its weak defaults let the kernel link without it.
#define PAGANI_ADSP_WEAK_DEFAULTS
#include "PaganiAdsp.h"
#endif
}

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void pagani_battery_service_static_init(void) __asm__("__GLOBAL__sub_I_PaganiBatteryService.cpp");
extern "C" void *pagani_battery_service_create(void);
extern "C" void pagani_battery_service_update(void *service, const PbatSnapshot *snapshot, int first);

// The statically linked kernel refuses undefined weak references: providers that may be absent get weak defaults here. The GCC branch
// vote lock word is laid out like the DRV-2 word (PaganiInput Drv2Lock.h): 0 free, else the owner, weak so that every user shares one:
// the touch and the speaker amplifiers take it through Sm8750Platform's sm8750_gcc_vote_set, PaganiWifi and this driver with the same
// protocol (BatteryI2c.h). A holder that overstays gives one unlocked update there, which the check before every transfer here repairs.
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) volatile uint32_t sm8750_gcc_vote_owner;
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // the word UfsLog.c's writer stops on while it is not 0 (BatteryUsb.h)
volatile int32_t pagani_battery_status[16];
}

namespace {
enum : uint32_t {
    kEvMessage = 1, kEvChannel = 2,
    kRing = 16,
    kVoteOwner = 0x42415454,     // 'BATT'
    kVoteSteps = 5000,           // 50 ms in 10 us steps
};

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
};
Window gWindow[PbiWinCount];
// In the order of the PbiWin* windows.
const struct {uint64_t base; uint32_t size;} kWindows[PbiWinCount] = {
    {PBI_GCC_VOTE_BASE, PBI_GCC_PAGE}, {PBI_GCC_SE_BASE, PBI_GCC_PAGE}, {PBI_GCC_AHB_BASE, PBI_GCC_PAGE},
    {PBI_TLMM_SDA, PBI_TLMM_PAGE}, {PBI_TLMM_SCL, PBI_TLMM_PAGE}, {PBI_SE_BASE, PBI_SE_SIZE},
};
static_assert(PbiWinGccVote == 0 && PbiWinGccSe == 1 && PbiWinGccAhb == 2 && PbiWinSda == 3 && PbiWinScl == 4 && PbiWinSe == 5,
              "kWindows follows the PbiWin order");

volatile uint32_t gStarted, gPending, gChannelUp, gChannelChanged;
#ifndef PAGANI_BATTERY_NO_ADSP
volatile uint32_t gDropped, gBig;   // messages the ring had no room for; messages longer than the codec reads
#endif
IOSimpleLock *gRingLock;
struct Slot {
    uint32_t length;
    uint8_t data[PBM_MESSAGE_MAX];
} gRing[kRing];
uint32_t gHead, gCount;
PbatCore *gCore;
PbatConfig gConfig;
PbiBus gBus;
PbiReport gReport;
void *gService;
uint32_t gGaugeId = 1, gPmic = 1, gMode;
#ifndef PAGANI_BATTERY_NO_ADSP
PaganiAdspChannel *gChannel;
#endif

void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}

uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}

// ---- events: the DSP thread records and wakes, this thread waits ----
#ifndef PAGANI_BATTERY_NO_ADSP
void wake(uint32_t bits)
{
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
#endif
uint32_t waitEvents(uint32_t ms)
{
    uint32_t bits = __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
    if (bits) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return bits;
    }
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
}

#ifndef PAGANI_BATTERY_NO_ADSP
void dspState(void *, int up)
{
    __atomic_store_n(&gChannelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gChannelChanged, 1u, __ATOMIC_RELEASE);
    wake(kEvChannel);
}
void dspReceive(void *, const uint8_t *data, uint32_t length)
{
    if (!data || !length) return;
    if (length > PBM_MESSAGE_MAX) {      // not a battery manager answer or notification this driver reads
        __atomic_fetch_add(&gBig, 1u, __ATOMIC_RELAXED);
        return;
    }
    IOSimpleLockLock(gRingLock);
    if (gCount < kRing) {
        Slot &slot = gRing[(gHead + gCount) % kRing];
        memcpy(slot.data, data, length);
        slot.length = length;
        gCount++;
    } else {
        __atomic_fetch_add(&gDropped, 1u, __ATOMIC_RELAXED);
    }
    IOSimpleLockUnlock(gRingLock);
    wake(kEvMessage);
}
const PaganiAdspClient kClient = {dspState, dspReceive};
#endif

// ---- registers ----
bool mapWindows()
{
    for (unsigned i = 0; i < PbiWinCount; ++i) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kWindows[i].base, kWindows[i].size);
        gWindow[i].map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        gWindow[i].va = gWindow[i].map ? reinterpret_cast<volatile uint8_t *>(gWindow[i].map->getVirtualAddress()) : nullptr;
        if (!gWindow[i].va) return false;
    }
    return true;
}
uint32_t ioRead(void *, unsigned window, uint32_t at)
{
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gWindow[window].va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void ioWrite(void *, unsigned window, uint32_t at, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(gWindow[window].va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void ioDelay(void *, unsigned us) {IODelay(us);}
int voteLock(void *, int take)
{
    if (!take) {
        __atomic_store_n(&sm8750_gcc_vote_owner, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    for (uint32_t step = 0; step < kVoteSteps; ++step) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kVoteOwner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        IODelay(10);
    }
    return -16;
}
const PbiIo kIo = {nullptr, ioRead, ioWrite, ioDelay, voteLock};

// ---- the core's operations ----
uint64_t opNow(void *) {return nowMs();}
int opSend(void *, const uint8_t *data, uint32_t length)
{
#ifndef PAGANI_BATTERY_NO_ADSP
    return pagani_adsp_send(gChannel, data, length);
#else
    (void)data; (void)length;
    return -19;
#endif
}
void opPublish(void *, const PbatSnapshot *snapshot, int first)
{
    pagani_battery_service_update(gService, snapshot, first);
    if (first) pagani_battery_status[0] = 6;
}
void opTrace(void *, const char *line) {trace(line);}
// Every transfer to the gauge passes pagani_gauge_transfer_allowed (BatteryGauge.h): register reads and the one DEVICE_TYPE subcommand,
// nothing that could reach the pack's protection switches. A refusal is a bug, counted and logged by the thread (-1, nothing sent).
volatile uint32_t gGaugeRefused;
int gaugeTransfer(const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    if (!pagani_gauge_transfer_allowed(tx, txn, rxn)) {
        __atomic_fetch_add(&gGaugeRefused, 1u, __ATOMIC_RELAXED);
        return -1;
    }
    return pagani_bi2c_xfer(&gBus, PGAUGE_ADDRESS, tx, txn, rx, rxn);
}
int gaugeRead16(void *, uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0, 0};
    const int rc = gaugeTransfer(&reg, 1, data, 2);
    if (!rc) *value = static_cast<uint16_t>(data[0] | data[1] << 8);
    return rc;
}
int gaugeXfer(void *, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    return gaugeTransfer(tx, txn, rx, rxn);
}
void sleepMs(void *, unsigned ms) {IOSleep(ms);}
PbatOps gOps = {nullptr, opNow, opSend, opPublish, opTrace, {nullptr, nullptr, gaugeXfer, sleepMs}, nullptr};

void bringupLine(int rc)
{
    char line[224];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    pbat_text_str(&t, rc ? "[battery] FAIL i2c" : "[battery] i2c up");
    pbat_text_kd(&t, "st", gReport.stage);
    pbat_text_kd(&t, "err", gReport.error);
    pbat_text_kx(&t, "vote", gReport.vote_before);
    pbat_text_kx(&t, "after", gReport.vote_after);
    pbat_text_kd(&t, "voted", gReport.voted);
    pbat_text_kx(&t, "cbcr", gReport.cbcr[0]);
    pbat_text_kx(&t, "core", gReport.cbcr[1]);
    pbat_text_kx(&t, "s1", gReport.cbcr[2]);
    pbat_text_kx(&t, "rcg", gReport.rcg_cfg);
    pbat_text_kx(&t, "fw", gReport.fw);
    pbat_text_kx(&t, "clk", gReport.clk_before);
    pbat_text_kx(&t, "scl", gReport.scl_before);
    pbat_text_kx(&t, "sda", gReport.pin_before[0]);
    pbat_text_kx(&t, "sclpin", gReport.pin_before[1]);
    trace(line);
}

void gaugeIdentity()
{
    char line[160];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    uint16_t type = 0;
    const int rc = pagani_gauge_device_type(&gOps.gauge, &type);
    pbat_text_str(&t, "[battery] gauge");
    pbat_text_kx(&t, "addr", PGAUGE_ADDRESS);
    pbat_text_kd(&t, "rc", rc);
    if (!rc) {
        pbat_text_kx(&t, "type", type);
        pbat_text_str(&t, type == PGAUGE_DEVICE_NFG8011B ? " NFG8011B" : " (not the NFG8011B: values are still range checked)");
    }
    pbat_text_kd(&t, "nack", gBus.nacks);
    pbat_text_kd(&t, "to", gBus.timeouts);
    trace(line);
}

// ---- the charger PMIC's owners (BatterySpmi.h): read once, the windows released after ----
struct Spmi {
    Window core, cnfg, obs;
};
bool mapOne(Window &w, uint64_t base, uint32_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmapOne(Window &w)
{
    OSSafeReleaseNULL(w.map);
    w.va = nullptr;
}
uint32_t spmiCore(void *ctx, uint32_t at) {return *reinterpret_cast<volatile uint32_t *>(static_cast<Spmi *>(ctx)->core.va + at);}
uint32_t spmiCnfg(void *ctx, uint32_t at) {return *reinterpret_cast<volatile uint32_t *>(static_cast<Spmi *>(ctx)->cnfg.va + at);}
// The apps (EE 0) observer channel of an APID: a read command, then its status (bit 0 done, bits 3:1 errors) and the data byte.
int spmiRead8(void *ctx, int apid, uint8_t offset, uint8_t *value)
{
    volatile uint8_t *ch = static_cast<Spmi *>(ctx)->obs.va + static_cast<uint32_t>(apid) * 0x20u;
    *reinterpret_cast<volatile uint32_t *>(ch) = 1u << 27 | static_cast<uint32_t>(offset) << 4;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned n = 0; n < 1000; ++n) {
        const uint32_t status = *reinterpret_cast<volatile uint32_t *>(ch + 8);
        if (status & 0xeu) return -5;
        if (status & 1u) {
            *value = static_cast<uint8_t>(*reinterpret_cast<volatile uint32_t *>(ch + 0x18));
            return 0;
        }
        IODelay(1);
    }
    return -110;
}
void pmicSurvey()
{
    Spmi spmi = {};
    const bool types = gPmic >= 2;
    if (!mapOne(spmi.core, PBS_CORE_BASE, PBS_CORE_SIZE) || !mapOne(spmi.cnfg, PBS_CNFG_BASE, PBS_CNFG_SIZE) ||
        (types && !mapOne(spmi.obs, PBS_OBS_BASE, PBS_OBS_SIZE))) {
        trace("[battery] spmi windows not mapped: no PMIC survey");
    } else {
        PbsSurvey *survey = static_cast<PbsSurvey *>(IOMallocZero(sizeof(PbsSurvey)));
        const PbsIo io = {&spmi, spmiCore, spmiCnfg, types ? spmiRead8 : nullptr};
        if (survey) {
            const int rc = pagani_spmi_survey(&io, 7, types, survey);
            char line[200];
            if (rc) {
                PbatText t;
                pbat_text_start(&t, line, sizeof line);
                pbat_text_str(&t, "[battery] spmi survey refused");
                pbat_text_kd(&t, "rc", rc);
                pbat_text_kx(&t, "v", survey->version);
                trace(line);
            } else {
                unsigned next = 0;
                do {
                    pagani_spmi_line(survey, 7, &next, line, sizeof line);
                    trace(line);
                } while (next);
            }
            IOFree(survey, sizeof(PbsSurvey));
        }
    }
    unmapOne(spmi.obs);
    unmapOne(spmi.cnfg);
    unmapOne(spmi.core);
}

// The observer channels of the peripherals BatterySpmi.h lists: the channel table is read once, the observer window stays mapped.
Spmi gChannels;
int gApid[PBS_PERIPHERALS];
int pmicRead(void *, unsigned ppid, uint8_t offset, uint8_t *value)
{
    const int index = pagani_spmi_index(ppid);
    if (index < 0) return index;
    return gApid[index] < 0 ? gApid[index] : spmiRead8(&gChannels, gApid[index], offset, value);
}
bool pmicOpen()
{
    const PbsIo io = {&gChannels, spmiCore, spmiCnfg, nullptr};
    const bool mapped = mapOne(gChannels.core, PBS_CORE_BASE, PBS_CORE_SIZE) && mapOne(gChannels.cnfg, PBS_CNFG_BASE, PBS_CNFG_SIZE) &&
                        mapOne(gChannels.obs, PBS_OBS_BASE, PBS_OBS_SIZE);
    const int rc = mapped ? pagani_spmi_map(&io, gApid) : -12;
    unmapOne(gChannels.cnfg);
    unmapOne(gChannels.core);
    if (!rc) return true;
    unmapOne(gChannels.obs);
    char line[96];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    pbat_text_str(&t, "[battery] pmic channels not found");
    pbat_text_kd(&t, "rc", rc);
    trace(line);
    return false;
}

// ---- the USB probe (BatteryUsb.h): its windows exist while it runs ----
Window gUsbWindow[PbuWinCount];
PbuRun gUsb;
uint64_t gUsbDue;   // when its next step runs; 0: none
uint32_t usbRead(void *, unsigned window, uint32_t at)
{
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gUsbWindow[window].va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void usbQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, PBU_QUIET_BIT, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~PBU_QUIET_BIT, __ATOMIC_RELEASE);
}
const PbuIo kUsbIo = {nullptr, usbRead, pmicRead, opTrace, usbQuiet};
void usbUnmap()
{
    for (unsigned i = 0; i < PbuWinCount; ++i) unmapOne(gUsbWindow[i]);
}
// Mapping reads nothing: the probe's gates decide what is touched, and the GCC is the only window read before it runs.
void usbOpen()
{
    for (unsigned i = 0; i < PbuWinCount; ++i) {
        if (mapOne(gUsbWindow[i], pagani_usb_windows[i].base, pagani_usb_windows[i].bytes)) continue;
        trace("[usbp] windows not mapped: nothing read");
        usbUnmap();
        return;
    }
    pagani_usb_init(&gUsb, &kUsbIo);
    pagani_usb_entry(&gUsb);
    gUsbDue = nowMs() + PBU_DELAY_MS;
}
void usbStep()
{
    const uint32_t wait = pagani_usb_step(&gUsb);
    gUsbDue = wait ? nowMs() + wait : 0;
    if (!wait) usbUnmap();
}

void batteryThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-battery");
    if (gPmic) pmicSurvey();
    if ((gConfig.pmic || gMode & PBAT_MODE_USB_PROBE) && pmicOpen()) {
        if (gConfig.pmic) gOps.pmic = pmicRead;
        if (gMode & PBAT_MODE_USB_PROBE) usbOpen();
    }
    bool gauge = false;
    if (gConfig.use_gauge) {
        if (!mapWindows()) {
            trace("[battery] FAIL gauge windows not mapped: the gauge is not used");
        } else {
            const int rc = pagani_bi2c_bringup(&gBus, &kIo, &gReport);
            bringupLine(rc);
            if (!rc) {
                gOps.gauge.read16 = gaugeRead16;
                if (gGaugeId) gaugeIdentity();
                gauge = true;
                pagani_battery_status[0] = 5;
            }
        }
    }
    pagani_battery_core_gauge(gCore, gauge);
    bool channel = false;
#ifndef PAGANI_BATTERY_NO_ADSP
    if (gConfig.use_adsp) {
        gChannel = pagani_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 5, 0, &kClient, nullptr);
        channel = gChannel != nullptr;
        trace(channel ? "[battery] dsp channel PMIC_RTR_ADSP_APPS registered (5 x 1024)"
                      : "[battery] dsp channel not registered (PaganiAdsp absent or off): the gauge only");
    }
#endif
    if (!gauge && !channel) {
        pagani_battery_status[0] = -7;
        trace("[battery] FAIL no source (gauge down, no DSP channel): no battery is shown");
        while (gUsbDue) {
            const uint64_t now = nowMs();
            if (now < gUsbDue) IOSleep(static_cast<uint32_t>(gUsbDue - now));
            else usbStep();
        }
        return;
    }
    if (pagani_battery_status[0] < 5) pagani_battery_status[0] = 5;
    uint32_t sleep = 1, revotes = 0, revoteLines = 0, refusedSeen = 0, refusedLines = 0;
#ifndef PAGANI_BATTERY_NO_ADSP
    uint32_t dropped = 0, droppedLines = 0;
#endif
    for (;;) {
        waitEvents(sleep);
#ifndef PAGANI_BATTERY_NO_ADSP
        const uint32_t lost = __atomic_load_n(&gDropped, __ATOMIC_RELAXED);
        if (lost != dropped && droppedLines < 4) {
            dropped = lost;
            droppedLines++;
            char line[96];
            PbatText t;
            pbat_text_start(&t, line, sizeof line);
            pbat_text_str(&t, "[battery] dsp ring full");
            pbat_text_kd(&t, "dropped", lost);
            pbat_text_kd(&t, "long", __atomic_load_n(&gBig, __ATOMIC_RELAXED));
            trace(line);
        }
#endif
        if (__atomic_exchange_n(&gChannelChanged, 0u, __ATOMIC_ACQ_REL))
            pagani_battery_core_channel(gCore, __atomic_load_n(&gChannelUp, __ATOMIC_ACQUIRE) != 0);
        for (;;) {
            Slot message;
            message.length = 0;
            IOSimpleLockLock(gRingLock);
            if (gCount) {
                message = gRing[gHead];
                gHead = (gHead + 1) % kRing;
                gCount--;
            }
            IOSimpleLockUnlock(gRingLock);
            if (!message.length) break;
            pagani_battery_core_message(gCore, message.data, message.length);
        }
        sleep = pagani_battery_core_run(gCore);
        const uint32_t refused = __atomic_load_n(&gGaugeRefused, __ATOMIC_RELAXED);
        if (refused != refusedSeen && refusedLines < 2) {
            refusedSeen = refused;
            refusedLines++;
            char line[96];
            PbatText t;
            pbat_text_start(&t, line, sizeof line);
            pbat_text_str(&t, "[battery] FAIL gauge transfer refused (not a read or DEVICE_TYPE), nothing sent");
            pbat_text_kd(&t, "n", refused);
            trace(line);
        }
        // A transfer found the gauge bus's clock votes dropped by another driver's unlocked update of 0x52008 (BatteryI2c.h).
        if (gBus.revotes != revotes && revoteLines < 4) {
            revotes = gBus.revotes;
            revoteLines++;
            char line[160];
            PbatText t;
            pbat_text_start(&t, line, sizeof line);
            pbat_text_str(&t, gBus.revote_error ? "[battery] i2c clock votes dropped by another driver, not back"
                                                : "[battery] i2c clock votes dropped by another driver, voted again");
            pbat_text_kd(&t, "n", revotes);
            pbat_text_kx(&t, "vote", gBus.dropped_vote);
            pbat_text_kd(&t, "rc", gBus.revote_error);
            trace(line);
        }
        int32_t words[16];
        pagani_battery_core_export(gCore, words);
        for (unsigned i = 1; i < 16; ++i) pagani_battery_status[i] = words[i];
        if (gUsbDue) {
            if (nowMs() >= gUsbDue) usbStep();
            const uint64_t now = nowMs();
            if (gUsbDue) sleep = gUsbDue <= now ? 1 : gUsbDue - now < sleep ? static_cast<uint32_t>(gUsbDue - now) : sleep;
        }
    }
}

bool registerClasses()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    pagani_battery_service_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}

void fail(int32_t stage, const char *what)
{
    pagani_battery_status[0] = -stage;
    char line[96];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    pbat_text_str(&t, "[battery] FAIL ");
    pbat_text_str(&t, what);
    pbat_text_kd(&t, "st", -stage);
    trace(line);
}
}  // namespace

extern "C" void pagani_battery_start(void)
{
    const unsigned adsp = argument("pagani-adsp", 0);
    gMode = pagani_battery_mode(argument("pagani-battery", 0));
    if (!gMode) return;
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    pagani_battery_status[0] = 1;
    gConfig.use_gauge = argument("pagani-battery-gauge", 1) != 0;
    gConfig.use_adsp = adsp == 1 && argument("pagani-battery-adsp", 1) != 0;
    gConfig.charge = argument("pagani-battery-charge", 0);
    gConfig.fcc_ma = argument("pagani-battery-fcc-ma", 2000);
    gConfig.sdp_ma = argument("pagani-battery-sdp-ma", 900);
    gConfig.design_mah = argument("pagani-battery-design-mah", 6260);
    gConfig.log = argument("pagani-battery-log", 1);
    gConfig.gauge_ms = argument("pagani-battery-gauge-ms", 0);
    if (gConfig.gauge_ms && gConfig.gauge_ms < 2000) gConfig.gauge_ms = 2000;
    gGaugeId = argument("pagani-battery-gauge-id", 1);
    gPmic = argument("pagani-battery-pmic", 1);
    gConfig.pmic = pagani_battery_mode_pmic(gMode);
    gRingLock = IOSimpleLockAlloc();
    gCore = static_cast<PbatCore *>(IOMallocZero(sizeof(PbatCore)));
    if (!gRingLock || !gCore) return fail(1, "no memory");
    if (!registerClasses()) return fail(2, "class registration");
    pagani_battery_status[0] = 2;
    gService = pagani_battery_service_create();
    if (!gService) return fail(3, "service attach");
    pagani_battery_status[0] = 3;
    pagani_battery_core_init(gCore, &gOps, &gConfig);
    char line[160];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    pbat_text_str(&t, "[battery] start");
    pbat_text_kd(&t, "gauge", gConfig.use_gauge);
    pbat_text_kd(&t, "adsp", gConfig.use_adsp);
    pbat_text_kd(&t, "charge", gCore->cfg.charge);
    if (gCore->cfg.charge) pbat_text_kd(&t, "fcc_ma", gCore->cfg.fcc_ma);
    pbat_text_kd(&t, "design", gCore->cfg.design_mah);
    pbat_text_kd(&t, "log", gConfig.log);
    pbat_text_kd(&t, "pmic", gPmic);
    pbat_text_kd(&t, "mode", gMode);
    trace(line);
    // The stage before the thread starts: the thread moves it on (5, 6) and may run before this function returns.
    pagani_battery_status[0] = 4;
    thread_t thread = nullptr;
    if (kernel_thread_start(batteryThread, nullptr, &thread) != KERN_SUCCESS) return fail(4, "thread");
    thread_deallocate(thread);
}
