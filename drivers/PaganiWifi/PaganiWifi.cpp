// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng package/WifiProbe.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e): the thread and its
// stages, the chip window accessors (chipRead/chipWrite with peach's window register), chipPowerUp (now a bit mask), the endpoint's
// MSI set-up, the iMSI interrupt on SPI 141, the stack's platform callbacks (lx_platform, nw_board: pNow, pLog, hAlloc, pDmaAlloc,
// pFwGet, pCfgRead, pMsiTarget, ...), publishMac and netStart. The 13T's facts (PaganiWifiTables.h) replace the SM8550/WCN7850 ones;
// the power-up sequence is WifiBringup.cpp, the MHI loader WifiMhiLoad.c, the firmware sources WifiFirmware.c, the DMA window
// WifiDma.c (low memory outside XNU's window on a pass-through SMMU stream, where sheng has a 32-bit stage 1 window), the PMIC
// accesses WifiPmic.c; the RPMh votes go through
// the Adreno830 SM8750 platform layer (Sm8750Rpmh, its DRV-2 lock word) like PaganiAdsp's.
//
// Kernel side of PaganiWifi (README.md). pagani_wifi_start (Stage7Probe, after the ADSP and battery starts) reads pagani-wifi=N and
// returns at once; with N in 1..6 it starts one kernel thread, "pagani-wifi", which runs the stages up to N:
//   1 survey (reads only)  2 power  3 link  4 endpoint (DMA window, SMMU stream, BAR0, MHI registers)
//   5 firmware and its own MHI boot to mission mode (a diagnostic path, the stack is not started)
//   6 the Ath12kWifi stack (it boots the chip itself) and, once it has a station interface, the network service (PaganiWifiNet.cpp)
// Nothing panics: every failure is a step and an error in pagani_wifi_status and a "[wifi]" line in the boot log (BootProgress
// ramoops U records, pagani-ramoops=1); the thread then parks. Nothing is ever written to storage.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <vm/memory_types.h>
#include <stdarg.h>
extern "C" {
#include "MhiBoot.h"
#include "PaganiWifi.h"
#include "Sm8750AossQmp.h"
#include "Sm8750Rpmh.h"
#include "WifiDma.h"
#include "WifiFirmware.h"
#include "WifiMhiLoad.h"
#include "WifiPmic.h"
#include "WifiRddm.h"
#include "WifiUtil.h"
#include "nw_abi.h"
}
#include "PaganiWifiTables.h"
#include "WifiBringup.h"
#define NATIVE_UFS_SM8750 1
#include "SmmuTakeover.h"

// The statically linked kernel refuses undefined weak references: providers that may be absent get weak defaults here (the same
// ones PaganiAdsp carries), and so does the Ath12kWifi stack, so that stages 1 to 5 link and run without it. The stack's own
// nw_wifi_where is zero until it runs: this default's first word is how a kernel without the stack is told (stackLinked).
#define PW_NO_STACK 0x4e4f5f535441434bull   /* "NO_STACK" */
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) {return 0;}
__attribute__((weak)) int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) {return 0;}
__attribute__((weak)) int native_ufs_part_status(void) {return -1;}
__attribute__((weak)) int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -78;}
__attribute__((weak)) IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) {return kIOReturnUnsupported;}
__attribute__((weak)) IOReturn armcpu_spi_disable(uint32_t) {return kIOReturnUnsupported;}
__attribute__((weak)) volatile uint32_t sm8750_gcc_vote_owner;
__attribute__((weak)) volatile uint32_t sm8750_aoss_qmp_owner;
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // the word UfsLog.c's writer stops on while it is not 0, bit by bit (kQuietBit)
__attribute__((weak)) struct nw_wifi_stats nw_wifi_stats;
__attribute__((weak)) volatile uint64_t nw_wifi_where[4] = {PW_NO_STACK, 0, 0, 0};
__attribute__((weak)) int nw_wifi_start(const struct lx_platform *, const struct nw_board *) {return -78;}
__attribute__((weak)) int nw_wifi_step(void) {return 0;}
__attribute__((weak)) void nw_wifi_set_events(const struct nw_wifi_events *) {}
__attribute__((weak)) int nw_wifi_scan(const uint8_t *, size_t) {return -78;}
__attribute__((weak)) int nw_wifi_bss_count(void) {return 0;}
__attribute__((weak)) int nw_wifi_bss_get(int, struct nw_bss_info *) {return -1;}
__attribute__((weak)) int nw_wifi_connect(const uint8_t *, size_t, const uint8_t *, const char *, size_t) {return -78;}
__attribute__((weak)) void nw_wifi_disconnect(void) {}
__attribute__((weak)) int nw_wifi_tx(const uint8_t *, size_t) {return -78;}
__attribute__((weak)) void nw_wifi_status_get(struct nw_wifi_status *st) {if (st) bzero(st, sizeof *st);}
__attribute__((weak)) void nw_wifi_tx_reasons(char *text, int size) {if (text && size > 0) text[0] = 0;}
void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
boolean_t ml_set_interrupts_enabled(boolean_t enable);
volatile int32_t pagani_wifi_status[32];
}
extern uint64_t gPhysBase, gPhysSize;
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void pagani_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_PaganiWifiNet.cpp");

using namespace pagani_wifi;

static_assert(int(PaganiWifiFileNotYet) == int(PwFwNotYet), "PaganiWifi.h's copy of the firmware layer's value");

namespace {
enum : uint32_t {
    kOwner = 0x57494649,            // 'WIFI' in the shared lock words
    kVoteSteps = 5000,              // 50 ms in 10 us steps
    kQmpLockMs = 3000,
    kMsiIntid = 32 + kSpiMsi0,
    kSeedWaitSeconds = 300,         // how long stage 5/6 waits for amss.bin to appear (the Data volume mounts late)
    kDataWaitSeconds = 60,          // how long the board data and the regulatory database wait for that volume, then modem_a's
    kStatusWords = 32,
    kLogDefault = 6,                // pagani-wifi-log: without it, what went to the boot log before the switch existed
    kLogLevelMask = 7, kLogDump = 8,
    kPeriodDefaultMs = 3000,        // UfsLog.h: pagani-ufslog-ms, default 3 s
    kCrumbExtraMs = 500,            // the writer looks at its rings this often, at the longest
    kStackHoldMs = 2000,            // the writer stays held this long past the stack's start, unless the firmware answers first: the first MHI reads, not the QMI lines the rounds are for
};

volatile int32_t *const st = pagani_wifi_status;
Bringup gB;
uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
uint64_t gStartMs;
unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}
void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}
void traceKv(const char *what, const char *key1, int64_t v1, const char *key2 = nullptr, int64_t v2 = 0)
{
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] ");
    pw_text_str(&t, what);
    if (key1) pw_text_kx(&t, key1, uint64_t(v1));
    if (key2) pw_text_kx(&t, key2, uint64_t(v2));
    trace(line);
}
void setStep(int step, int error = 0)
{
    st[0] = step;
    st[1] = error;
}
int fail(int step, int error, const char *what)
{
    setStep(-step, error);
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] FAIL");
    pw_text_kd(&t, "step", step);
    pw_text_kd(&t, "err", error);
    pw_text_char(&t, ' ');
    pw_text_str(&t, what);
    trace(line);
    letGo(gB);
    return error ? error : -1;
}

// pagani-wifi-stop for the steps past WifiBringup.cpp's (those stop by themselves): records the step, and true when it is the last.
int gStopAfter;
bool stopHere(int step)
{
    setStep(step);
    if (gStopAfter != step) return false;
    letGo(gB);
    traceKv("stopped as asked", "step", step);
    return true;
}

// ---- windows ----
struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t size;
};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = w.va ? size : 0;
    return w.va != nullptr;
}
uint32_t rd(const Window &w, uint32_t at)
{
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr(const Window &w, uint32_t at, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
Window gWin[kWinCount], gSmmu, gBar, gRsc, gCmdDbWin, gAoss, gIpcc, gSpmiCore, gSpmiCnfg, gSpmiObs, gSpmiChan;
unsigned gSpmiChanApid = ~0u;
uint32_t gBadAccess;

// ---- WifiBringup's hardware ----
uint32_t hwRead(void *, unsigned win, uint32_t off)
{
    if (win >= kWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {++gBadAccess; return 0xffffffffu;}
    return rd(gWin[win], off);
}
void hwWrite(void *, unsigned win, uint32_t off, uint32_t value)
{
    if (win >= kWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {++gBadAccess; return;}
    wr(gWin[win], off, value);
}
void hwUdelay(void *, unsigned us)
{
    if (us >= 1000) IOSleep(us / 1000);
    else IODelay(us);
}
void hwMsleep(void *, unsigned ms) {IOSleep(ms);}
uint64_t hwNow(void *) {return nowMs();}
// Held for two read-modify-writes, with interrupts off: the lock word orders every driver of 0x52008 and the GPU's GPLL0 votes in 0x52000
// (Sm8750GpuPower takes it since integrate/adreno830-shared-locks.diff went in), and a thread preempted between its read and its write
// would keep them spinning. The other holders wait at most 50 ms; these interrupts stay off for a few register accesses.
boolean_t gVoteInterrupts;
int hwVoteLock(void *, int take)
{
    if (!take) {
        uint32_t expected = kOwner;
        __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        ml_set_interrupts_enabled(gVoteInterrupts);
        return 0;
    }
    for (uint32_t step = 0; step < kVoteSteps; ++step) {
        const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kOwner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            gVoteInterrupts = enabled;
            return 0;
        }
        ml_set_interrupts_enabled(enabled);
        IODelay(10);
    }
    return -16;
}

// RPMh through the Adreno830 SM8750 platform layer: one command per request, waited for; DRV-2 is serialized by its own lock word.
uint32_t ioRead(void *context, uint32_t offset) {return rd(*static_cast<Window *>(context), offset);}
void ioWrite(void *context, uint32_t offset, uint32_t value) {wr(*static_cast<Window *>(context), offset, value);}
void ioDelay(void *, unsigned us) {hwUdelay(nullptr, us);}
const Sm8750RpmhIo kDrv2 = {&gRsc, ioRead, ioWrite, ioDelay}, kCmdDbIo = {&gCmdDbWin, ioRead, ioWrite, ioDelay};
const Sm8750AossQmpIo kQmpRam = {&gAoss, ioRead, ioWrite, ioDelay}, kQmpIpcc = {&gIpcc, ioRead, ioWrite, ioDelay};
Sm8750CmdDb *gDb;
int gDbState = 1;                   // 1 not read, 0 read, negative the copy's error
int cmdDb()
{
    if (gDbState <= 0) return gDbState;
    gDb = static_cast<Sm8750CmdDb *>(IOMallocData(sizeof(Sm8750CmdDb)));
    if (!gDb) return gDbState = -12;
    if (!mapWindow(gCmdDbWin, kCmdDb, kCmdDbBytes) || !mapWindow(gRsc, kRscDrv2, 0x10000)) return gDbState = -14;
    gDbState = Sm8750CmdDbCopy(&kCmdDbIo, gDb);
    traceKv("cmd-db copied", "rc", gDbState, "drv2", rd(gRsc, 0));
    return gDbState;
}
int send(uint32_t address, uint32_t data, const char *what)
{
    Sm8750RpmhCmd cmd = {address, data};
    Sm8750RpmhResult result = {};
    const int rc = Sm8750RpmhWriteActive(&kDrv2, &cmd, 1, 1, &result);
    if (rc) {
        char line[160];
        PwText t;
        pw_text_start(&t, line, sizeof line);
        pw_text_str(&t, "[wifi] rpmh ");
        pw_text_str(&t, what);
        pw_text_kx(&t, "addr", address);
        pw_text_kx(&t, "data", data);
        pw_text_kd(&t, "rc", rc);
        pw_text_kd(&t, "step", result.step);
        pw_text_kd(&t, "tcs", int32_t(result.tcs));
        pw_text_kd(&t, "lock_waits", result.lock_waits);
        trace(line);
    }
    return rc;
}
int hwVrm(void *, const char *resource, uint32_t reg, uint32_t value, uint32_t *address)
{
    int rc = cmdDb();
    if (rc) return rc;
    uint32_t base = 0;
    if ((rc = Sm8750CmdDbVrm(gDb, resource, &base))) return rc;
    if (address) *address = base;
    return send(base + reg, value, resource);
}
int hwBcm(void *, const char *name, uint32_t avg, uint32_t peak, uint32_t buswidth, uint32_t *vote)
{
    int rc = cmdDb();
    if (rc) return rc;
    uint32_t address = 0, unit = 0;
    uint16_t width = 0;
    uint8_t vcd = 0;
    if ((rc = Sm8750CmdDbBcm(gDb, name, &address, &unit, &width, &vcd))) return rc;
    const uint32_t data = Sm8750BcmCommand(1, 1, bcmVote(avg, width, buswidth, unit), bcmVote(peak, width, buswidth, unit));
    if (vote) *vote = data;
    return send(address, data, name);
}

// The PMIC through the SPMI arbiter: observer reads, and the one channel page the sleep clock pin needs.
uint32_t spmiCore(void *, uint32_t off) {return gSpmiCore.va && off + 4 <= gSpmiCore.size ? rd(gSpmiCore, off) : 0;}
uint32_t spmiCnfg(void *, uint32_t off) {return gSpmiCnfg.va && off + 4 <= gSpmiCnfg.size ? rd(gSpmiCnfg, off) : 7;}
uint32_t spmiObsRead(void *, uint32_t off) {return gSpmiObs.va && off + 4 <= gSpmiObs.size ? rd(gSpmiObs, off) : 0xe;}
void spmiObsWrite(void *, uint32_t off, uint32_t value) {if (gSpmiObs.va && off + 4 <= gSpmiObs.size) wr(gSpmiObs, off, value);}
bool spmiChannel(unsigned apid)
{
    if (gSpmiChanApid == apid && gSpmiChan.va) return true;
    if (gSpmiChan.map) {gSpmiChan.map->release(); gSpmiChan = {};}
    gSpmiChanApid = apid;
    return mapWindow(gSpmiChan, PW_SPMI_CHAN + uint64_t(apid) * PW_SPMI_CHAN_STRIDE, PW_SPMI_CHAN_STRIDE);
}
uint32_t spmiChanRead(void *, unsigned apid, uint32_t off) {return spmiChannel(apid) && off + 4 <= PW_SPMI_CHAN_STRIDE ? rd(gSpmiChan, off) : 0xe;}
void spmiChanWrite(void *, unsigned apid, uint32_t off, uint32_t value)
{
    if (spmiChannel(apid) && off + 4 <= PW_SPMI_CHAN_STRIDE) wr(gSpmiChan, off, value);
}
void spmiDelay(void *, unsigned us) {IODelay(us);}
const PwSpmiIo kSpmi = {nullptr, spmiCore, spmiCnfg, spmiObsRead, spmiObsWrite, spmiChanRead, spmiChanWrite, spmiDelay};
bool spmiMapped()
{
    return mapWindow(gSpmiCore, PW_SPMI_CORE, PW_SPMI_CORE_BYTES) && mapWindow(gSpmiCnfg, PW_SPMI_CNFG, PW_SPMI_CNFG_BYTES) &&
           mapWindow(gSpmiObs, PW_SPMI_OBS, PW_SPMI_OBS_BYTES);
}
int hwPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    if (!spmiMapped()) return -14;
    return pw_pmic_rail(&kSpmi, ppid, status, enable, mode, mv);
}
int hwSleepClock(void *, int write)
{
    if (!spmiMapped()) return -14;
    PwSleepClock s;
    const int rc = pw_pmic_sleep_clock(&kSpmi, write, &s);
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] pmk8550 gpio3");
    pw_text_kd(&t, "apid", s.apid);
    pw_text_kd(&t, "owner", s.owner);
    pw_text_kx(&t, "mode", s.regs[2]);
    pw_text_kx(&t, "src", s.regs[6]);
    pw_text_kx(&t, "en", s.regs[8]);
    pw_text_kd(&t, "written", s.written);
    pw_text_kd(&t, "rc", rc);
    trace(line);
    return rc;
}
// One AOSS QMP message under the QMP lock word (the GPU driver's channel, reused as it is).
Sm8750AossQmp gQmp;
bool gQmpReady;
int hwPdc(void *, const char *message)
{
    if (!gQmpReady) {
        if (!mapWindow(gAoss, 0xc300000, 0x400) || !mapWindow(gIpcc, 0x406000, 0x1000)) return -14;
        Sm8750AossQmpInit(&gQmp, &kQmpRam, Sm8750AossQmpMsgramBytes, &kQmpIpcc);
        gQmpReady = true;
    }
    for (uint32_t waited = 0;; ++waited) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_aoss_qmp_owner, &expected, kOwner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (waited >= kQmpLockMs) return -16;
        IOSleep(1);
    }
    const int rc = Sm8750AossQmpSend(&gQmp, message);
    uint32_t expected = kOwner;
    __atomic_compare_exchange_n(&sm8750_aoss_qmp_owner, &expected, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] qmp ");
    pw_text_str(&t, message);
    pw_text_kd(&t, "rc", rc);
    trace(line);
    return rc;
}
void hwLog(void *, const char *line) {trace(line);}
void hwQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, kQuietBit, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~kQuietBit, __ATOMIC_RELEASE);
}
const Hw kHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwVrm, hwBcm, hwPmic, hwSleepClock, hwPdc, hwLog, hwQuiet};
// How long a line takes to reach the UFS log: its period and a look of the writer. No log, nothing writes: the holds and their waits are left out.
unsigned crumbMs()
{
    if (argument("sheng-log", 0) != 1) return 0;
    const unsigned period = argument("pagani-ufslog-ms", kPeriodDefaultMs);
    return (period >= 1000 && period <= kPeriodDefaultMs ? period : unsigned(kPeriodDefaultMs)) + kCrumbExtraMs;
}

// ---- the DMA window. Everything the endpoint reaches lies in it. By default low memory outside XNU's window (PaganiWifiTables.h
// kDmaLowBase says why the addresses stay below 4 GiB), mapped write-combined, as the speaker's period buffer; with pagani-wifi-dmapa=0
// XNU memory (above 4 GiB, physically contiguous, its pages switched to write-combined in every mapping the pmap tracks). Either way
// the CPU and the endpoint see the same bytes whatever the bypass stream does to coherency ----
PwDma *gDma;
IOBufferMemoryDescriptor *gDmaBuffer;
Window gDmaLow;
void cleanToPoc(const void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
int dmaLow(uint64_t pa, unsigned mib)
{
    const uint64_t bytes = uint64_t(mib) << 20, end = pa + bytes;
    if ((pa & 0x1fffff) || end > 0x100000000ull || (pa < gPhysBase + gPhysSize && end > gPhysBase)) return -22;
    if (!mapWindow(gDmaLow, pa, bytes, kIOMapWriteCombineCache)) return -14;
    uint8_t *va = const_cast<uint8_t *>(gDmaLow.va);
    cleanToPoc(va, size_t(bytes));          // what UEFI's heap left in the caches must not be evicted over the endpoint's data later
    if (pw_dma_init(gDma, va, pa, uint32_t(bytes))) return -22;
    st[10] = int32_t(mib);
    st[11] = int32_t(pa >> 12);
    traceKv("dma window, low memory", "bus", int64_t(pa), "bytes", int64_t(bytes));
    return 0;
}
int dmaWindow()
{
    if (gDma) return 0;
    gDma = static_cast<PwDma *>(IOMallocData(sizeof(PwDma)));
    if (!gDma) return -12;
    const unsigned asked = argument(bootarg::kDmaMiB, 48);
    const unsigned first = asked >= 8 && asked <= 128 ? asked : 48u;
    const uint64_t low = argument(bootarg::kDmaPa, unsigned(kDmaLowBase));
    if (low) return dmaLow(low, first);
    const unsigned tries[4] = {first, 32, 24, 16};
    for (unsigned mib : tries) {
        const uint64_t bytes = uint64_t(mib) << 20;
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), bytes,
            0x0000000fffe00000ull);   // 36-bit (ath12k's streaming mask and cnss2's), 2 MiB aligned
        if (!buffer) continue;
        if (buffer->prepare() != kIOReturnSuccess) {buffer->release(); continue;}
        IOByteCount contiguous = 0;
        const uint64_t pa = buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        uint8_t *va = static_cast<uint8_t *>(buffer->getBytesNoCopy());
        if (!va || !pa || contiguous < bytes || pa + bytes > (1ull << 36)) {
            buffer->complete();
            buffer->release();
            continue;
        }
        cleanToPoc(va, size_t(bytes));
        for (uint64_t at = 0; at < bytes; at += PAGE_SIZE) pmap_set_cache_attributes(ppnum_t((pa + at) >> PAGE_SHIFT), VM_WIMG_WCOMB);
        if (pw_dma_init(gDma, va, pa, uint32_t(bytes))) {buffer->complete(); buffer->release(); continue;}
        gDmaBuffer = buffer;
        st[10] = int32_t(mib);
        st[11] = int32_t(pa >> 12);
        traceKv("dma window, XNU memory above 4 GiB", "bus", int64_t(pa), "bytes", int64_t(bytes));
        return 0;
    }
    return -12;
}

// ---- the endpoint's stream in the apps SMMU: a pass-through bank, as NativeUfs gives the UFS stream (Sm8750Platform) ----
uint32_t gSmmuBankPage;             // the stream's context bank, in pages from the SMMU's base (0: none yet)
int smmuStream()
{
    if (!mapWindow(gSmmu, kAppsSmmu, kUfsSmmuMapBytes)) return fail(PaganiWifiStepSmmu, -14, "SMMU not mapped");
    auto read = [](unsigned offset) {return rd(gSmmu, offset);};
    auto write = [](unsigned offset, uint32_t value) {wr(gSmmu, offset, value);};
    const UfsSmmuPlan plan = planUfsSmmu(kSidEndpoint, gDma->bus, gDma->bytes, read);
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] smmu sid");
    pw_text_kx(&t, "stream", kSidEndpoint);
    pw_text_kd(&t, "slot", int32_t(plan.slot));
    pw_text_kd(&t, "bank", plan.bank);
    pw_text_kd(&t, "reuse", plan.reuse);
    pw_text_kx(&t, "smr", plan.smr);
    pw_text_kx(&t, "s2cr", plan.s2cr);
    if (plan.error) {pw_text_char(&t, ' '); pw_text_str(&t, plan.error);}
    trace(line);
    if (plan.error) return fail(PaganiWifiStepSmmu, -1, plan.error);
    const char *error = configureUfsSmmu(plan, kSidEndpoint, read, write);
    if (error) return fail(PaganiWifiStepSmmu, -5, error);
    gSmmuBankPage = plan.pages + plan.bank;
    uint64_t deadline = 0;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    uint32_t status = 0;
    if (!syncUfsSmmu(read, write, [&]() {return mach_absolute_time() >= deadline;}, status))
        return fail(PaganiWifiStepSmmu, -110, "SMMU TLB sync");
    st[9] = int32_t(plan.slot | plan.bank << 16 | 0x80000000u);
    setStep(PaganiWifiStepSmmu);
    return 0;
}

// ---- the chip through BAR0: registers above 0x80000 through peach's remap window (0x3278) ----
uint32_t gChipWindow = 0xffffffffu;
// A register offset can come from the device: all ones while it does not answer. Refused here, so that it neither wraps past the
// bound nor makes an unaligned access to Device memory, which is a kernel panic.
uint32_t barRd(uint32_t off) {return pw_reg_ok(off, gBar.size) ? rd(gBar, off) : 0xffffffffu;}
void barWr(uint32_t off, uint32_t value) {if (pw_reg_ok(off, gBar.size)) wr(gBar, off, value);}
uint32_t chipAt(uint32_t off)
{
    if (off < chip::kWindowStart) return off;
    const uint32_t window = (off >> chip::kWindowShift) & chip::kWindowValueMask;
    if (window != gChipWindow) {
        barWr(chip::kWindowReg, chip::kWindowEnable | window);
        (void)barRd(chip::kWindowReg);
        gChipWindow = window;
    }
    return chip::kWindowStart + (off & chip::kWindowRange);
}
uint32_t chipRead(uint32_t off) {return barRd(chipAt(off));}
void chipWrite(uint32_t off, uint32_t value) {barWr(chipAt(off), value);}

// ath12k_pci_power_up's writes on the chip (facts: PaganiWifiTables.h chip::), each one a bit of pagani-wifi-chipreset. cnss2 makes
// none of them on this phone, on a chip WLAN_EN has just power-cycled, as here, so by default none is made (0x79 is ath12k's set
// without its hot reset, whose address for peach is not known: two candidates). The reads below are only for the log.
enum : uint32_t {
    kResetLtssm = 1u << 0, kResetHotWcn7850 = 1u << 1, kResetHotQcc2072 = 1u << 2, kResetIntClear = 1u << 3,
    kResetQfprom = 1u << 4, kResetWarmEntry = 1u << 5, kResetGlobal = 1u << 6,
    kResetDefault = 0,
};
void chipPowerUp(uint32_t mask)
{
    gChipWindow = 0xffffffffu;
    uint32_t ltssm = chipRead(chip::kParfLtssm);
    if (mask & kResetLtssm)
        for (unsigned i = 0; ltssm != chip::kParfLtssmValue && i < 5; ++i) {
            if (ltssm == 0xffffffffu) IOSleep(5);
            chipWrite(chip::kParfLtssm, chip::kParfLtssmValue);
            ltssm = chipRead(chip::kParfLtssm);
        }
    uint32_t hot = 0;
    if (mask & (kResetHotWcn7850 | kResetHotQcc2072)) {
        const uint32_t reg = mask & kResetHotQcc2072 ? chip::kHotResetQcc2072 : chip::kHotResetWcn7850;
        chipWrite(reg, chipRead(reg) | chip::kHotResetValue);
        hot = chipRead(reg);
        IOSleep(5);
    }
    if (mask & kResetIntClear) chipWrite(chip::kIntAllClear, 0xffffffffu);
    if (mask & kResetQfprom) chipWrite(chip::kQfpromPwrCtrl, chipRead(chip::kQfpromPwrCtrl) & ~chip::kQfpromVdd4Blow);
    const uint32_t cookie = chipRead(chip::kQ6Cookie), warm = chipRead(chip::kWarmSwEntry);
    if (mask & kResetWarmEntry) {
        IOSleep(10);
        chipWrite(chip::kWarmSwEntry, 0);
        IOSleep(10);
    }
    const uint32_t cause = chipRead(chip::kResetCause);   // read-clear
    uint32_t global = 0;
    if (mask & kResetGlobal) {
        const uint32_t v = chipRead(chip::kSocGlobalReset);
        chipWrite(chip::kSocGlobalReset, v | chip::kSocGlobalResetBit);
        (void)chipRead(chip::kSocGlobalReset);
        IOSleep(10);
        chipWrite(chip::kSocGlobalReset, v & ~chip::kSocGlobalResetBit);
        (void)chipRead(chip::kSocGlobalReset);
        IOSleep(10);
        global = chipRead(chip::kSocGlobalReset);
        // the reset clears the window register: put the cached window back; MHICTRL reset clears the SYSERR the reset leaves
        barWr(chip::kWindowReg, chip::kWindowEnable | (gChipWindow == 0xffffffffu ? 0u : gChipWindow));
        (void)barRd(chip::kWindowReg);
        barWr(MHI_CTRL, MHI_CTRL_RESET);
        IOSleep(10);
    }
    char line[160];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] chip power-up");
    pw_text_kx(&t, "mask", mask);
    pw_text_kx(&t, "ltssm", ltssm);
    pw_text_kx(&t, "hot", hot);
    pw_text_kx(&t, "cookie", cookie);
    pw_text_kx(&t, "warm", warm);
    pw_text_kx(&t, "cause", cause);
    pw_text_kx(&t, "global", global);
    pw_text_kx(&t, "mhistatus", barRd(MHI_STATUS));
    trace(line);
}

// ---- configuration space of the endpoint (the 1 MiB window iATU region 0 maps to bus 1) ----
uint32_t gPcieCap, gMsiCap;
uint32_t cfgRd(uint32_t off) {return pw_reg_ok(off & ~3u, gWin[kWinCfg].size) ? rd(gWin[kWinCfg], off & ~3u) : 0xffffffffu;}
void cfgWr(uint32_t off, uint32_t value) {if (pw_reg_ok(off & ~3u, gWin[kWinCfg].size)) wr(gWin[kWinCfg], off & ~3u, value);}
bool findCaps()
{
    uint8_t cap = uint8_t(cfgRd(0x34) & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = cfgRd(cap);
        if (uint8_t(w) == 0x10) gPcieCap = cap;
        if (uint8_t(w) == 0x05) gMsiCap = cap;
        cap = uint8_t(w >> 8);
    }
    return gPcieCap && gMsiCap;
}
int pCfgRead(int off, int size, uint32_t *v)
{
    uint32_t o = uint32_t(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    const uint32_t w = cfgRd(o & ~3u);
    *v = size == 4 ? w : size == 2 ? (w >> ((o & 2) * 8)) & 0xffff : (w >> ((o & 3) * 8)) & 0xff;
    return 0;
}
int pCfgWrite(int off, int size, uint32_t v)
{
    uint32_t o = uint32_t(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    if (size == 4) {cfgWr(o, v); return 0;}
    const unsigned shift = (size == 2 ? (o & 2) : (o & 3)) * 8;
    const uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;
    cfgWr(o & ~3u, (cfgRd(o & ~3u) & ~mask) | ((v << shift) & mask));
    return 0;
}
int pMsiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data)
{
    *lo = uint32_t(gDma->bus);
    *hi = uint32_t(gDma->bus >> 32);
    *data = 0;
    return 0;
}
// A 32-bit MSI capability cannot name a window above 4 GiB: its writes would go to the truncated address, a stray DMA write through
// the bypass stream. Such an endpoint gets no MSI enabled.
bool msiReachable() {return gMsiCap && (((cfgRd(gMsiCap) >> 16) & 0x80) || !(gDma->bus >> 32));}
// Stage 5 without the stack: no ASPM while the image loads, and the endpoint's MSI aimed at the iMSI receiver's address (one vector).
void endpointMsi()
{
    if (gPcieCap) cfgWr(gPcieCap + 0x10, cfgRd(gPcieCap + 0x10) & ~3u);
    if (!msiReachable()) {traceKv("endpoint MSI not enabled", "cap", gMsiCap, "bus", int64_t(gDma->bus)); return;}
    const uint32_t w = cfgRd(gMsiCap);
    const bool is64 = (w >> 16) & 0x80;
    cfgWr(gMsiCap + 4, uint32_t(gDma->bus));
    if (is64) {cfgWr(gMsiCap + 8, uint32_t(gDma->bus >> 32)); cfgWr(gMsiCap + 0xc, 0);}
    else cfgWr(gMsiCap + 8, 0);
    cfgWr(gMsiCap, (w & 0xffffu) | ((((w >> 16) & ~0x70u) | 1u) << 16));
}

// ---- the endpoint's MSIs as an interrupt: the root complex's iMSI receiver takes writes to MSI_ADDR (the DMA window's first page)
// and raises SPI 141 (level); the handler clears the status and wakes the thread ----
volatile uint32_t gMsiCount, gMsiBits, gMsiWake, gMsiEmpty;
volatile bool gMsiAttached;
void msiIrq(void *)
{
    const Window &d = gWin[kWinDbi];
    const uint32_t status = rd(d, kMsiIntrStatus);
    if (status) wr(d, kMsiIntrStatus, status);
    gMsiCount = gMsiCount + 1;
    gMsiBits = gMsiBits | status;
    gMsiEmpty = status ? 0 : gMsiEmpty + 1;
    if (gMsiEmpty > 100000) {   // a line with nothing to clear would storm: off, the thread polls
        wr(d, kMsiIntrEnable, 0);
        armcpu_spi_disable(kMsiIntid);
        gMsiAttached = false;
    }
    gMsiWake = 1;
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)));
}
int msiAttach()
{
    if (!argument(bootarg::kMsi, 1)) return -1;
    const Window &d = gWin[kWinDbi];
    wr(d, kMsiAddrLo, uint32_t(gDma->bus));
    wr(d, kMsiAddrHi, uint32_t(gDma->bus >> 32));
    wr(d, kMsiIntrMask, 0);
    wr(d, kMsiIntrEnable, 0xffffffffu);
    uint32_t readback = 0;
    uint64_t router = 0;
    IOReturn rc = kIOReturnNotReady;
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(kMsiIntid, false, msiIrq, nullptr, &readback, &router);
        if (rc != kIOReturnNotReady || tries == 40) break;
        IOSleep(250);
    }
    gMsiAttached = rc == kIOReturnSuccess;
    traceKv("msi attach", "rc", int64_t(rc), "router", int64_t(router));
    return gMsiAttached ? 0 : int(rc);
}
// Sleeps until an MSI, a frame to send (pagani_wifi_wake) or ms milliseconds pass.
void msiWait(unsigned ms)
{
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)), THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) {clear_wait(current_thread(), THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
    gMsiWake = 0;
}

// ---- firmware: the partition in place (NativeUfs), then the RAM Data seed (the VFS, once Data is mounted) ----
PwFw *gFw;
IOLock *gFwLock;
int diskFind(void *, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    return native_ufs_part_find(name, lun, first, blocks);
}
int diskRead(void *, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) {return native_ufs_part_read(lun, lba, blocks, dst);}
// The seed lives on the Data volume, which launchd mounts long after this thread starts: until /private/var/ngpu exists every
// lookup answers "not yet".
bool dataMounted(vfs_context_t ctx)
{
    vnode_t root = vfs_rootvnode();
    if (!root) return false;
    vnode_put(root);
    vnode_t dir = NULLVP;
    if (vnode_lookup("/private/var/ngpu", 0, &dir, ctx)) return false;
    vnode_put(dir);
    return true;
}
int seedSize(void *, const char *path, uint64_t *size)
{
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return PwFwIoError;
    int rc = PwFwNotYet;
    if (dataMounted(ctx)) {
        vnode_t vp = NULLVP;
        const int err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
        if (err) {
            rc = err == ENOENT || err == ENOTDIR ? PwFwAbsent : PwFwIoError;
        } else {
            struct vnode_attr va;
            VATTR_INIT(&va);
            VATTR_WANTED(&va, va_data_size);
            rc = vnode_getattr(vp, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_data_size) ? PwFwIoError : PwFwOk;
            if (!rc) *size = va.va_data_size;
            vnode_close(vp, FREAD, ctx);
        }
    }
    vfs_context_rele(ctx);
    return rc;
}
int seedRead(void *, const char *path, uint64_t offset, void *dst, size_t length)
{
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return PwFwIoError;
    vnode_t vp = NULLVP;
    int rc = vnode_open(path, FREAD, 0, 0, &vp, ctx) ? PwFwIoError : PwFwOk;
    for (size_t done = 0; !rc && done < length;) {
        const int chunk = int(length - done < (1u << 20) ? length - done : (1u << 20));
        int resid = 0;
        if (vn_rdwr(UIO_READ, vp, static_cast<caddr_t>(dst) + done, chunk, off_t(offset + done), UIO_SYSSPACE, IO_NOAUTH,
                    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) || resid == chunk)
            rc = PwFwIoError;
        done += size_t(chunk - resid);
    }
    if (vp) vnode_close(vp, FREAD, ctx);
    vfs_context_rele(ctx);
    return rc;
}
void *fwAlloc(void *, size_t bytes) {return IOMallocData(bytes);}
void fwFree(void *, void *memory, size_t bytes) {if (memory) IOFreeData(memory, bytes);}
const PwFwIo kFwIo = {nullptr, diskFind, diskRead, seedSize, seedRead, fwAlloc, fwFree};
char gFwPart[24] = "modem_a", gFwDir[48] = "image/peach", gBdf[32] = "";
bool firmwareInit()
{
    if (gFw) return true;
    gFw = static_cast<PwFw *>(IOMallocData(sizeof(PwFw)));
    gFwLock = IOLockAlloc();
    if (!gFw || !gFwLock) return false;
    PE_parse_boot_argn(bootarg::kFwPartition, gFwPart, sizeof gFwPart);
    PE_parse_boot_argn(bootarg::kFwDir, gFwDir, sizeof gFwDir);
    PE_parse_boot_argn(bootarg::kBdf, gBdf, sizeof gBdf);
    PwFwConfig config = {};
    config.partitions[0] = gFwPart;
    config.partition_count = 1;
    if (pw_streq(gFwPart, "modem_a")) config.partitions[config.partition_count++] = "modem_b";
    if (pw_streq(gFwPart, "none")) config.partition_count = 0;
    config.dirs[0] = gFwDir;
    config.dir_count = 1;
    if (pw_streq(gFwDir, "image/peach")) config.dirs[config.dir_count++] = "image";
    config.seed_dir = PW_FW_SEED_DIR;
    config.bdf = gBdf[0] ? gBdf : nullptr;
    config.m3 = argument(bootarg::kM3, 1);
    pw_fw_init(gFw, &kFwIo, &config);
    return true;
}
int fwGet(const char *name, const uint8_t **data, uint32_t *size)
{
    if (!firmwareInit()) return PwFwNoMemory;
    IOLockLock(gFwLock);
    const int rc = pw_fw_get(gFw, name, data, size);
    const PwFwFile *f = rc ? nullptr : pw_fw_loaded(gFw, name);
    IOLockUnlock(gFwLock);
    static uint32_t said;
    if ((rc && rc != PwFwAbsent && said++ < 32) || (f && f->size == *size && said++ < 64)) {
        char line[200];
        PwText t;
        pw_text_start(&t, line, sizeof line);
        pw_text_str(&t, "[wifi] firmware ");
        pw_text_str(&t, name);
        pw_text_kd(&t, "rc", rc);
        if (!rc) {
            pw_text_kd(&t, "bytes", *size);
            pw_text_ks(&t, "from", f ? f->where : "?");
        } else {
            pw_text_ks(&t, "last", gFw->last_path);
            pw_text_kd(&t, "ufs", native_ufs_part_status());
        }
        trace(line);
    }
    return rc;
}
// The radio's address: from /chosen/unique-chip-id (FNV-1a, locally administered, unicast), the same at every boot (sheng's rule;
// the phone's own NV MAC is not readable here).
bool boardMac(uint8_t m[6])
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = uint8_t(h >> (8 * i));
    m[0] = uint8_t((m[0] & 0xfe) | 0x02);
    return true;
}
// The system's record of the Wi-Fi address, as a boot loader leaves it in the device tree (MobileGestalt WifiAddressData; the
// wireless daemon derives every network's private address from it).
void publishMac()
{
    uint8_t m[6];
    if (!boardMac(m)) return;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-wifi0")) chosen->setProperty("mac-address-wifi0", m, sizeof m);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        if (!wlan->getProperty("local-mac-address")) wlan->setProperty("local-mac-address", m, sizeof m);
        wlan->release();
    }
}

// ---- the stack's platform (lx_platform) and board (nw_board) ----
uint64_t pNow() {return nowMs();}
void pSleep(unsigned ms) {IOSleep(ms);}
void pPoll() {}
// Each line of the stack goes to a ring of its own; notices, warnings and errors (level 6 and below) also to the boot log, at most
// PW_LOG_LINES of them (then only errors), pagani-wifi-log=7 adds the debug lines (WifiUtil.h PwLogGate), and a line equal to one of the
// last sixteen is only counted.
char gText[256][112];
volatile uint32_t gTextN;
uint32_t gLogRepeats, gLogOption;   // counters for the log; a race only loses a count
PwLogGate gLogGate;
void watchFaults(const char *text);
bool has(const char *s, const char *w)
{
    for (; *s; ++s) {
        size_t k = 0;
        while (w[k] && s[k] == w[k]) ++k;
        if (!w[k]) return true;
    }
    return false;
}
void pLog(int level, const char *text)
{
    if (has(text, "chan scan list") || has(text, "mac channel [")) {__atomic_fetch_add(&gLogRepeats, 1u, __ATOMIC_RELAXED); return;}
    if (level >= 7 && (has(text, "htc rx completion") || has(text, "rx ce pipe") || has(text, "ext irq:") || has(text, "event mgmt rx") ||
                       has(text, "mgmt rx event") || has(text, "chan info vdev_id") || has(text, "scan event foreign channel"))) {
        __atomic_fetch_add(&gLogRepeats, 1u, __ATOMIC_RELAXED);
        return;
    }
    static uint32_t recent[16];
    uint32_t h = 2166136261u;
    for (const char *c = text; *c; ++c) h = (h ^ uint8_t(*c)) * 16777619u;
    h ^= uint32_t(level);
    for (uint32_t r : recent) if (r == h) {__atomic_fetch_add(&gLogRepeats, 1u, __ATOMIC_RELAXED); return;}
    memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
    recent[0] = h;
    const uint32_t i = __atomic_fetch_add(const_cast<uint32_t *>(&gTextN), 1u, __ATOMIC_RELAXED) % 256;
    snprintf(gText[i], sizeof gText[i], "%u %d %.100s", uint32_t(nowMs() - gStartMs), level, text);
    if (pw_log_gate(&gLogGate, level, nowMs())) {
        char line[140];
        snprintf(line, sizeof line, "[wifi] ath%d %.120s", level, text);
        boot_progress_trace(line);
    }
    watchFaults(text);
}

// ---- when the firmware dies or stops answering: what the root port saw, the stream's bank in the SMMU, and what the firmware left in the
// RAM dump (WifiRddm.c). Each of the three signs once; the reader waits and sleeps, on the Wi-Fi thread that logged the sign. The root port's
// offsets and lines are sheng's pLog (package/WifiProbe.cpp, commit 08cde20e), the same DesignWare core; sheng reads the SMMU through
// NativeUfs's bank, here the stream has its own (ARM SMMUv2: sGFSR 0x48, sGFSYNR0 0x50; bank: SCTLR 0, FSR 0x58, FAR 0x60, FSYNR0 0x68) ----
void portState(const char *why)
{
    const Window &d = gWin[kWinDbi];
    if (!d.va) return;
    char text[112];
    snprintf(text, sizeof text, "root port %s: devctl/sta %08x link %08x ue %08x mask %08x ce %08x rootsta %08x src %08x", why, rd(d, 0x78),
             rd(d, dbi::kLinkControlStatus), rd(d, 0x104), rd(d, 0x108), rd(d, 0x110), rd(d, 0x130), rd(d, 0x134));
    pLog(3, text);
    snprintf(text, sizeof text, "root port %s: aer header %08x %08x %08x %08x ltssm-debug %08x", why, rd(d, 0x11c), rd(d, 0x120), rd(d, 0x124),
             rd(d, 0x128), rd(d, dbi::kPortDebug0));
    pLog(3, text);
}
void smmuState(const char *why)
{
    if (!gSmmu.va || !gSmmuBankPage) return;
    const uint32_t bank = gSmmuBankPage * 4096;
    char text[112];
    snprintf(text, sizeof text, "smmu %s: gfsr %08x gfsynr0 %08x cb-fsr %08x far %08x%08x fsynr0 %08x sctlr %08x", why, rd(gSmmu, 0x48),
             rd(gSmmu, 0x50), rd(gSmmu, bank + 0x58), rd(gSmmu, bank + 0x64), rd(gSmmu, bank + 0x60), rd(gSmmu, bank + 0x68), rd(gSmmu, bank));
    pLog(3, text);
}
uint32_t rddmRead(void *, uint32_t reg) {return barRd(reg);}
const uint8_t *rddmVa(void *, uint64_t bus, uint64_t bytes) {return pw_dma_va(gDma, bus, bytes);}
void rddmSleep(void *, unsigned ms) {IOSleep(ms);}
void rddmLog(void *, const char *line) {pLog(3, line);}
// The whole dump goes to the UFS log's export area with pagani-wifi-log's bit 3 (read back by read-ufslog.py as export/table.bin, segNN.bin).
// Its generation has the top bit set, which keeps a later export from replacing it.
void rddmExport(const PwRddmIo &io, const PwRddmResult &result)
{
    const uint64_t bytes = pw_rddm_export_bytes(&io, &result);
    uint8_t *blob = bytes ? static_cast<uint8_t *>(IOMallocData(bytes)) : nullptr;
    if (!blob) {pLog(3, "rddm export: nothing to export or no memory"); return;}
    const uint32_t generation = 0x80000000u | uint32_t(nowMs() & 0x7fffffff);
    const int rc = pw_rddm_export_fill(&io, &result, blob, bytes) ? boot_log_export_put(generation, uint32_t(bytes), 0, blob, uint32_t(bytes)) : -100;
    IOFreeData(blob, bytes);
    char text[64];
    snprintf(text, sizeof text, "rddm export: %llu bytes, result %d", static_cast<unsigned long long>(bytes), rc);
    pLog(3, text);
}
void rddmDump()
{
    if (!gBar.va || !gDma) return;
    static PwRddmResult result;
    const PwRddmIo io = {nullptr, rddmRead, rddmVa, rddmSleep, rddmLog};
    const int rc = pw_rddm_scan(&io, &result);
    st[27] = rc ? rc : int32_t(result.messages + 1);
    if (!rc && (gLogOption & kLogDump)) rddmExport(io, result);
}
void watchFaults(const char *text)
{
    enum : uint32_t {kStuck = 1, kSysErr = 2, kRddm = 4};
    static uint32_t shown;
    const uint32_t sign = has(text, "MHI_CB_EE_RDDM") ? kRddm : has(text, "MHI_CB_SYS_ERROR") ? kSysErr : has(text, "stuck: wmi credits") ? kStuck : 0;
    if (!sign || (shown & sign)) return;
    shown |= sign;
    letGo(gB);              // the chip answers, and an export of the dump needs the writer
    const char *why = sign == kRddm ? "at rddm" : sign == kSysErr ? "at syserr" : "at stuck";
    portState(why);
    smmuState(why);
    if (sign == kRddm) rddmDump();
}
void pHalt()
{
    letGo(gB);
    trace("[wifi] the stack halted: the thread parks");
    for (;;) IOSleep(60000);
}
void *hAlloc(size_t n)
{
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16;
    return p + 16;
}
void hFree(void *q)
{
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}
void *pDmaAlloc(size_t n, uint64_t *bus)
{
    void *p = pw_dma_alloc(gDma, n, bus);
    if (!p) {
        static uint32_t said;
        uint32_t total = 0, largest = 0;
        pw_dma_free_bytes(gDma, &total, &largest);
        if (said++ < 8) traceKv("dma window full", "want", int64_t(n), "largest", int64_t(largest));
    }
    return p;
}
void pDmaFree(size_t, void *p, uint64_t) {pw_dma_free(gDma, p);}
uint64_t pDmaMap(void *p, size_t, int) {return pw_dma_bus(gDma, p);}
void pDmaSync(uint64_t bus, size_t n, int)
{
    // the window is mapped non-cacheable: only the ordering is needed
    (void)bus;
    (void)n;
    __asm__ volatile("dsb sy" ::: "memory");
}
int macFile(const uint8_t **data, size_t *size)
{
    static char text[40];
    static size_t len;
    if (!len) {
        uint8_t m[6];
        if (!boardMac(m)) return -1;
        len = size_t(snprintf(text, sizeof text, "address=%02x%02x%02x%02x%02x%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]));
    }
    *data = reinterpret_cast<const uint8_t *>(text);
    *size = len;
    return 0;
}
int pFwGet(const char *path, const uint8_t **data, size_t *size)
{
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (pw_streq(base, "wlan_mac.bin")) return macFile(data, size);
    uint32_t bytes = 0;
    const int rc = fwGet(base, data, &bytes);
    *size = bytes;
    return rc ? -1 : 0;
}

// ---- the network side: its classes are registered with the kernel's module like the other late classes, then en0 is made ----
bool gNetUp, gNetTried;
void netStart()
{
    struct nw_wifi_status status;
    nw_wifi_status_get(&status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {fail(PaganiWifiStepNet, -1, "no module handle for the network classes"); return;}
    pagani_wifi_net_static_init();
    const OSReturn r = OSMetaClass::postModLoad(handle);
    if (r) {fail(PaganiWifiStepNet, int(r), "network classes not registered"); return;}
    pagani_wifi_net_start(status.mac);
    gNetUp = true;
    st[23] = 1;
    setStep(PaganiWifiStepNet);
}

// ---- the stages ----
bool stackLinked() {return nw_wifi_where[0] != PW_NO_STACK;}
const uint32_t kBarMapMax = 16u << 20;

int stage4()
{
    guard(gB, "dma");       // the first touch of the low memory outside XNU's window: on Android it is RAM, here the hypervisor has the last word
    if (int rc = dmaWindow()) return fail(PaganiWifiStepDma, rc, "no DMA window");
    if (stopHere(PaganiWifiStepDma)) return kErrStopped;
    if (int rc = smmuStream()) return rc;
    if (stopHere(PaganiWifiStepSmmu)) return kErrStopped;
    if (int rc = endpoint(gB)) {     // WifiBringup.cpp has logged it
        setStep(gB.step, gB.error);
        return rc;
    }
    st[7] = int32_t(gB.barAddress);
    st[8] = int32_t(gB.barBytes);
    if (!mapWindow(gBar, gB.barAddress, gB.barBytes < kBarMapMax ? gB.barBytes : kBarMapMax))
        return fail(PaganiWifiStepBarMapped, -14, "BAR0 not mapped");
    if (stopHere(PaganiWifiStepBarMapped)) return kErrStopped;
    guard(gB, "bar0");      // the first reads through the endpoint's BAR, then the first one that goes through its window
    if (!findCaps()) traceKv("capabilities incomplete", "pcie", gPcieCap, "msi", gMsiCap);
    static const uint32_t regs[7] = {0x00, 0x08, 0x10, 0x28, 0x2c, 0x38, 0x48};
    uint32_t v[7];
    for (unsigned i = 0; i < 7; ++i) v[i] = barRd(regs[i]);
    const uint32_t bhi = v[3];
    const uint32_t ee = bhi >= 0x100 && uint64_t(bhi) + 0x40 <= gBar.size ? barRd(bhi + MHI_BHI_EXECENV) : 0xffffffffu;
    st[25] = int32_t(ee);
    st[26] = int32_t(v[6]);
    char line[200];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "[wifi] mhi regs");
    pw_text_kx(&t, "reglen", v[0]);
    pw_text_kx(&t, "ver", v[1]);
    pw_text_kx(&t, "cfg", v[2]);
    pw_text_kx(&t, "bhi", v[3]);
    pw_text_kx(&t, "bhie", v[4]);
    pw_text_kx(&t, "ctrl", v[5]);
    pw_text_kx(&t, "status", v[6]);
    pw_text_kx(&t, "ee", ee);
    trace(line);
    if (v[0] == 0xffffffffu && v[3] == 0xffffffffu && v[6] == 0xffffffffu) return fail(PaganiWifiStepMhiRegs, -5, "BAR0 reads all ones");
    // After the line above: pagani-linux's peach entry skips this read, so nothing shows it is harmless at this point, and should it
    // ever hang, the log ends with the registers that were read. pagani-wifi-stop=405 leaves the MHI registers and this read out.
    gChipWindow = 0xffffffffu;
    const uint32_t version = chipRead(chip::kTcsrSocHwVersion);
    st[24] = int32_t(version);
    traceKv("soc hw version", "raw", version, "major", (version >> 8) & 0xf);
    letGo(gB);
    return stopHere(PaganiWifiStepMhiRegs) ? int(kErrStopped) : 0;
}

// The board data and the regulatory database Android uses sit on /odm, out of reach here: a seed that carries them goes before modem_a's
// (WifiFirmware.h), and whether it does is known only once the Data volume is up, seconds after this thread starts.
void dataWait()
{
    for (unsigned waited = 0; waited < kDataWaitSeconds; ++waited) {
        vfs_context_t ctx = vfs_context_create(nullptr);
        const bool mounted = ctx && dataMounted(ctx);
        if (ctx) vfs_context_rele(ctx);
        if (mounted) return;
        if (!waited) traceKv("waiting for the Data volume", "seconds", kDataWaitSeconds);
        IOSleep(1000);
    }
    IOLockLock(gFwLock);
    pw_fw_seed_gone(gFw);
    IOLockUnlock(gFwLock);
    traceKv("no Data volume: board data from the partition", "seconds", kDataWaitSeconds);
}

// The required firmware, waited for (the Data volume mounts late), the optional files tried once.
int firmwareReady()
{
    const uint8_t *data = nullptr;
    uint32_t size = 0;
    int rc = PwFwAbsent;
    for (unsigned waited = 0; waited <= kSeedWaitSeconds; waited += 2) {
        rc = fwGet("amss.bin", &data, &size);
        if (!rc) break;
        if (waited % 30 == 0) traceKv("waiting for amss.bin", "rc", rc, "seconds", waited);
        IOSleep(2000);
    }
    if (rc) return fail(PaganiWifiStepFirmware, rc, "amss.bin not found (partition or Data seed)");
    const PwFwFile *f = pw_fw_loaded(gFw, "amss.bin");
    st[16] = int32_t(size);
    st[17] = f ? f->source : 0;
    dataWait();
    static const char *const optional[] = {"board.bin", "regdb.bin", "tmel_peach_20.elf", "aux_ucode.bin", "qdss_trace_config_v2.cfg",
                                           "m3.bin"};
    for (const char *name : optional) {
        const uint8_t *d = nullptr;
        uint32_t n = 0;
        const int r = fwGet(name, &d, &n);
        if (r) traceKv(name, "absent_rc", r);
    }
    setStep(PaganiWifiStepFirmware);
    return 0;
}

uint32_t mhiRead(void *, uint32_t reg) {return barRd(reg);}
void mhiWrite(void *, uint32_t reg, uint32_t value) {barWr(reg, value);}
void mhiSleep(void *, unsigned ms) {IOSleep(ms);}
uint64_t mhiNow(void *) {return nowMs();}
void mhiClean(void *, const void *va, size_t bytes) {cleanToPoc(va, bytes);}
void mhiLog(void *, const char *line) {trace(line);}
// sheng's linkRecover: the endpoint's PCIe core may restart while its boot loader comes up. BAR0 then reads all ones; when configuration
// space answers again with BAR0 or the decode cleared, they are put back. 1 when something was restored.
unsigned mhiLinkCheck(void *)
{
    if (cfgRd(0) != kEndpointVendorDevice) return 0;
    if ((cfgRd(0x10) & ~0xfu) == (gB.barValue & ~0xfu) && (cfgRd(0x04) & 6u) == 6u) return 0;
    cfgWr(0x10, gB.barValue);
    if ((gB.barMask & 6u) == 4u) cfgWr(0x14, 0);
    cfgWr(0x04, (cfgRd(0x04) & 0xffffu) | 6u);
    endpointMsi();
    gChipWindow = 0xffffffffu;
    return 1;
}

int stage5()
{
    if (int rc = firmwareReady()) return rc;
    if (gB.opt.stopAfter == PaganiWifiStepFirmware) {traceKv("stopped as asked", "step", PaganiWifiStepFirmware); return kErrStopped;}
    guard(gB, "chip");
    chipPowerUp(argument(bootarg::kChipReset, kResetDefault));
    letGo(gB);
    if (stopHere(PaganiWifiStepChipReset)) return kErrStopped;
    st[12] = msiAttach();
    endpointMsi();
    static PwMhiLoad load;
    const uint8_t *image = nullptr;
    uint32_t bytes = 0;
    fwGet("amss.bin", &image, &bytes);
    const PwMhiIo io = {nullptr, mhiRead, mhiWrite, mhiSleep, mhiNow, mhiClean, mhiLog, mhiLinkCheck};
    PwMhiConfig config = {};
    config.image = image;
    config.image_bytes = bytes;
    config.bar_bytes = uint32_t(gBar.size < 0x20000 ? gBar.size : 0x20000);
    config.phase_ms = mhi::kTimeoutMsCnss * 2;
    config.step_ms = 5;
    config.seq = uint32_t(mach_absolute_time()) | 1u;
    setStep(PaganiWifiStepMhiOpen);
    guard(gB, "mhi");       // the chip's own PCIe core restarts during this one (mhiLinkCheck)
    const int rc = pw_mhi_load(&load, gDma, &io, &config);
    letGo(gB);
    st[13] = int32_t(gMsiCount);
    st[14] = int32_t(load.boot.phase | load.boot.error << 8 | load.boot.failed_phase << 16);
    st[15] = int32_t(load.boot.ee);
    if (load.link_restores) traceKv("BAR0 put back during the boot", "times", load.link_restores);
    if (rc) {
        portState("at the MHI failure");
        smmuState("at the MHI failure");
        return fail(load.boot.failed_phase >= MhiWaitSbl ? PaganiWifiStepContexts : PaganiWifiStepSbl, rc, "MHI boot");
    }
    setStep(PaganiWifiStepMission);
    traceKv("mission mode (stage 5 ends here; pagani-wifi=6 runs the stack)", "ms", load.elapsed_ms, "msis", gMsiCount);
    return 0;
}

int stage6()
{
    // the stack programs the endpoint's MSI capability and reads the PCI Express one through these offsets
    if (!gMsiCap || !gPcieCap) return fail(PaganiWifiStepStack, -19, "the endpoint lacks its MSI or PCI Express capability");
    if (!msiReachable()) return fail(PaganiWifiStepStack, -22, "32-bit MSI, DMA window above 4 GiB");
    if (int rc = firmwareReady()) return rc;
    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet};
    static struct nw_board board;
    board.bar = const_cast<uint8_t *>(gBar.va);
    board.bar_len = size_t(gBar.size);
    board.device_id = uint16_t(gB.endpointId >> 16);
    board.msi_cap = uint16_t(gMsiCap);
    board.irq_base = 1000;
    board.cfg_read = pCfgRead;
    board.cfg_write = pCfgWrite;
    board.msi_target = pMsiTarget;
    board.map_phys = nullptr;
    st[12] = msiAttach();
    setStep(PaganiWifiStepStack);
    guard(gB, "stack");     // the stack's own chip resets are in nw_wifi_start
    const int r = nw_wifi_start(&plat, &board);
    if (r) return fail(PaganiWifiStepStack, r, "the stack did not start");
    const uint64_t holdEnds = nowMs() + kStackHoldMs;
    setStep(PaganiWifiStepStackUp);
    traceKv(gMsiAttached ? "stack started, MSI on SPI 141" : "stack started, polling", "msi", st[12]);
    // Runs on interrupts once the MSI line is attached: sleeps until the endpoint signals, a frame is queued or 10 ms pass; goes
    // round again at once while frames move; without the line it polls every millisecond.
    uint64_t nextReport = 0;
    for (uint64_t n = 0;; ++n) {
        const int busy = nw_wifi_step();
        if (gB.held && (nw_wifi_stats.stage >= 3 || nowMs() >= holdEnds)) letGo(gB);
        if (!gNetTried && nw_wifi_stats.stage >= 4) {   // once: a failed registration is reported, not retried every round
            gNetTried = true;
            netStart();
        }
        if (!busy) {
            if (gMsiAttached) msiWait(10);
            else IOSleep(1);
        }
        if ((n & 63) == 0) {
            st[3] = int32_t((nowMs() - gStartMs) / 1000);
            st[13] = int32_t(gMsiCount);
            st[18] = int32_t(nw_wifi_stats.stage);
            st[19] = int32_t(nw_wifi_stats.scans);
            st[20] = int32_t(nw_wifi_stats.bss_seen);
            st[21] = int32_t(nw_wifi_stats.frames);
            st[22] = nw_wifi_stats.last_err;
        }
        const uint64_t now = nowMs();
        if (now >= nextReport) {
            nextReport = now + 30000;
            char line[160];
            snprintf(line, sizeof line, "[wifi] run stage %u scans %u bss %u frames %u err %d msi %u bits %08x repeats %u held %u", nw_wifi_stats.stage,
                     nw_wifi_stats.scans, nw_wifi_stats.bss_seen, nw_wifi_stats.frames, nw_wifi_stats.last_err, gMsiCount, gMsiBits,
                     gLogRepeats, gLogGate.held_back);
            trace(line);
        }
    }
}

void wifiThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-wifi");
    gStartMs = nowMs();
    const unsigned stage = argument(bootarg::kStage, 0);
    st[2] = int32_t(stage);
    setStep(PaganiWifiStepThread);
    traceKv("thread started", "stage", stage);
    gLogOption = argument(bootarg::kLog, kLogDefault);
    pw_log_gate_init(&gLogGate, gLogOption & kLogLevelMask ? gLogOption & kLogLevelMask : uint32_t(kLogDefault), gStartMs);
    if (stage >= 6 && !stackLinked()) {
        fail(PaganiWifiStepStack, -78, "the Ath12kWifi stack is not linked: nothing was powered");
        return;
    }
    for (unsigned w = 0; w < kWinCount; ++w)
        if (!mapWindow(gWin[w], kWinBase[w], kWinBytes[w])) {fail(PaganiWifiStepMapped, -14, "a register window was not mapped"); return;}
    setStep(PaganiWifiStepMapped);
    gB.hw = &kHw;
    gB.opt.stage = stage;
    gB.opt.maxGen = argument(bootarg::kMaxGen, 2);
    gB.opt.pdc = argument(bootarg::kPdc, 0);
    gB.opt.sleepClock = argument(bootarg::kSleepClk, 1);
    gB.opt.s7iVote = argument("pagani-wifi-s7i", 0);
    gB.opt.stopAfter = gStopAfter = int(argument(bootarg::kStopAfter, 0));
    gB.opt.crumbMs = crumbMs();
    int rc = stage == 1 ? survey(gB) : stage == 2 ? powerOn(gB) : linkUp(gB);
    st[4] = int32_t(gB.endpointId);
    st[5] = int32_t(gB.linkStatus);
    st[6] = int32_t(gB.linkMs);
    setStep(gB.step, gB.error);
    if (rc == kErrStopped) {traceKv("stopped as asked", "step", gB.step); return;}
    if (rc) {traceKv("stage ended", "step", gB.step, "err", gB.error); return;}
    if (gBadAccess) traceKv("WARN accesses outside the windows", "count", gBadAccess);
    if (stage >= 4) rc = stage4();
    if (!rc && stage == 5) rc = stage5();
    if (!rc && stage >= 6) rc = stage6();
    letGo(gB);
    traceKv("thread ends", "step", st[0], "err", st[1]);
}

volatile uint32_t gStarted;
}  // namespace

// ---- entry points ----
extern "C" void pagani_wifi_start(void)
{
    const unsigned stage = argument(bootarg::kStage, 0);
    if (stage < 1 || stage > 6) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    // Here and not on the thread: launchd and MobileGestalt look at the device tree once, long before the stack comes up.
    if (stage >= 6 && stackLinked()) publishMac();
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, nullptr, &thread) != KERN_SUCCESS) {
        fail(PaganiWifiStepThread, -7, "no thread");
        return;
    }
    thread_deallocate(thread);
}

extern "C" int pagani_wifi_file(const char *name, const uint8_t **data, size_t *size)
{
    if (!gStarted || !name || !data || !size) return -1;
    uint32_t bytes = 0;
    const int rc = fwGet(name, data, &bytes);
    *size = bytes;
    return rc;
}

extern "C" void pagani_wifi_wake(void)
{
    gMsiWake = 1;
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)));
}

extern "C" void pagani_wifi_logf(int level, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pLog(level, line);
}

extern "C" int nw_board_option(const char *name, int fallback)
{
    if (!name || pw_strlen(name, 16) >= 16) return fallback;
    char key[32];
    PwText t;
    pw_text_start(&t, key, sizeof key);
    pw_text_str(&t, "pagani-wifi-");
    pw_text_str(&t, name);
    return int(argument(key, unsigned(fallback)));
}
