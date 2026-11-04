// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Kernel side of PaganiHaptics (PaganiHaptics.h, README.md), in the manner of PaganiBattery.cpp. pagani_haptics_start (Stage7Probe,
// after the battery start) reads its boot arguments and returns at once without them. Otherwise it allocates the core, starts one
// kernel thread, "pagani-haptics", and returns. The thread maps the SPMI arbiter's core, ownership and EE 0 observer windows (read only
// uses), runs the core's discovery/identity/dump and, in mode 1, its initialization (HapticsCore.c), which maps exactly two RW channel
// pages (the HAPTICS_CFG and HAPTICS_PATTERN APIDs, chnls + 0x1000 x APID) after the identity and ownership checks. Then it executes
// queued commands and polls the hardware (FIFO refill, ends, faults) until reboot. Nothing waits on the boot; a dry run, a refusal or
// a fault releases the windows and ends the thread, and every API call then returns -19. Every step is a "[haptics]" line in the boot
// log (BootProgress ramoops U records, pagani-ramoops=1) and a word in pagani_haptics_status.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#if __has_include(<PaganiPeripherals.h>)
#include <PaganiPeripherals.h>
#endif
extern "C" {
#include "HapticsCore.h"
#include "HapticsIos.h"
#include "HapticsRegs.h"
#include "HapticsText.h"
#include "PaganiHaptics.h"
#include "PaganiHapticsBoard.h"
}

// The statically linked kernel refuses undefined weak references: providers that may be absent get weak definitions here. The battery's
// status (PaganiBattery.h) gives the low-battery rule its percent ([1], shown once [0] is 6) and temperature ([4], 0.01 C).
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) volatile int32_t pagani_battery_status[16];
volatile int32_t pagani_haptics_status[16];
}

namespace {
enum : uint32_t {
    kQueue = 16,
    kIdleMs = 1000,
    kObsSize = PGH_ARB_OBS_EE_STRIDE,   // this EE's observer channels
    kPage = PGH_ARB_CHNL_STRIDE,
};

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint32_t size;
};
Window gCoreWin, gCnfgWin, gObsWin, gChanWin[2];
PghCore *gHaptics;
PghConfig gConfig;
PghSpmiIo gIo;
PghOps gOps;
IOSimpleLock *gQueueLock;
IOLock *gProducer;
PghCmd gQueue[kQueue];
uint32_t gHead, gCount, gTestMs;
volatile uint32_t gStarted, gReady, gPending;

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

uint64_t nowUs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000u;
}

bool mapOne(Window &w, uint64_t base, uint32_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = w.va ? size : 0;
    return w.va != nullptr;
}
void unmapOne(Window &w)
{
    w.va = nullptr;
    w.size = 0;
    OSSafeReleaseNULL(w.map);
}
void unmapAll()
{
    unmapOne(gChanWin[0]);
    unmapOne(gChanWin[1]);
    unmapOne(gObsWin);
    unmapOne(gCnfgWin);
    unmapOne(gCoreWin);
}

// Register access, bounds-checked against each window (an offset outside reads as an error status and is never written).
uint32_t rd32(const Window &w, uint32_t at)
{
    if (!w.va || at + 4u > w.size) return 0xeu;
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr32(const Window &w, uint32_t at, uint32_t value)
{
    if (!w.va || at + 4u > w.size) return;
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- the core's SPMI windows (HapticsSpmi.h) ----
uint32_t ioCore(void *, uint32_t at) {return rd32(gCoreWin, at);}
uint32_t ioCnfg(void *, uint32_t at) {return rd32(gCnfgWin, at);}
uint32_t ioObsRead(void *, uint32_t at) {return rd32(gObsWin, at);}
void ioObsWrite(void *, uint32_t at, uint32_t value) {wr32(gObsWin, at, value);}
int ioMapChannel(void *, unsigned periph, unsigned apid)
{
    if (periph > PGH_PTN || apid >= 1024 || gChanWin[periph].va) return -22;
    return mapOne(gChanWin[periph], PAGANI_HAPTICS_SPMI_CHNLS + uint64_t(kPage) * apid, kPage) ? 0 : -12;
}
uint32_t ioChanRead(void *, unsigned periph, uint32_t at) {return periph <= PGH_PTN ? rd32(gChanWin[periph], at) : 0xeu;}
void ioChanWrite(void *, unsigned periph, uint32_t at, uint32_t value)
{
    if (periph <= PGH_PTN) wr32(gChanWin[periph], at, value);
}
void ioDelay(void *, unsigned us) {IODelay(us);}

// ---- the core's clock, waits, log, battery ----
uint64_t opNow(void *) {return nowUs();}
void opWait(void *, unsigned us)
{
    if (us >= 2000u) {   // the brake waits (half an LRA period): whole ms asleep on this thread, the rest spun
        IOSleep(us / 1000u);
        us %= 1000u;
    }
    if (us) IODelay(us);
}
void opTrace(void *, const char *line) {trace(line);}
void opBattery(void *, int *percent, int *deci_c)
{
    *percent = -1;
    *deci_c = 250;
    if (pagani_battery_status[0] == 6 && pagani_battery_status[1] >= 0) {
        *percent = pagani_battery_status[1];
        *deci_c = pagani_battery_status[4] / 10;
    }
}

// ---- commands: any thread queues, the driver's thread executes ----
void wake()
{
    __atomic_fetch_or(&gPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
void waitMs(uint32_t ms)
{
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
}
int post(const PghCmd &cmd)
{
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    IOSimpleLockLock(gQueueLock);
    if (gCount == kQueue) {
        IOSimpleLockUnlock(gQueueLock);
        return -16;
    }
    gQueue[(gHead + gCount) % kQueue] = cmd;
    gCount++;
    IOSimpleLockUnlock(gQueueLock);
    wake();
    return 0;
}
bool dequeue(PghCmd *cmd)
{
    bool got = false;
    IOSimpleLockLock(gQueueLock);
    if (gCount) {
        *cmd = gQueue[gHead];
        gHead = (gHead + 1) % kQueue;
        gCount--;
        got = true;
    }
    IOSimpleLockUnlock(gQueueLock);
    return got;
}

void exportStatus()
{
    int32_t words[16];
    pgh_core_export(gHaptics, words);
    for (unsigned i = 0; i < 16; ++i) pagani_haptics_status[i] = words[i];
}

void hapticsThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-haptics");
    if (!mapOne(gCoreWin, PAGANI_HAPTICS_SPMI_CORE, PAGANI_HAPTICS_SPMI_CORE_SZ) ||
        !mapOne(gCnfgWin, PAGANI_HAPTICS_SPMI_CNFG, PAGANI_HAPTICS_SPMI_CNFG_SZ) ||
        !mapOne(gObsWin, PAGANI_HAPTICS_SPMI_OBS + uint64_t(kObsSize) * PAGANI_HAPTICS_SPMI_EE, kObsSize)) {
        unmapAll();
        pagani_haptics_status[0] = -8;
        trace("[haptics] FAIL spmi windows not mapped: haptics off");
        return;
    }
    const int rc = pgh_core_init(gHaptics, &gConfig, &gOps, &gIo);
    exportStatus();
    if (rc) {   // 1: the dry run's log is out; negative: refused or failed (logged by the core)
        unmapAll();
        trace(rc == 1 ? "[haptics] dry run done: nothing written, haptics off" : "[haptics] off");
        return;
    }
    __atomic_store_n(&gReady, 1u, __ATOMIC_RELEASE);
    if (gTestMs) {
        const PghCmd test = {PGH_CMD_CONSTANT, 1000, gTestMs, 0};
        post(test);
    }
    uint32_t sleep = 0;
    for (;;) {
        waitMs(sleep ? sleep : kIdleMs);
        PghCmd cmd;
        while (dequeue(&cmd)) pgh_core_command(gHaptics, &cmd);
        sleep = pgh_core_poll(gHaptics);
        exportStatus();
        if (gHaptics->state == PGH_ST_FAULT) {   // stopped by the core; off until reboot
            __atomic_store_n(&gReady, 0u, __ATOMIC_RELEASE);
            unmapAll();
            trace("[haptics] off after a fault");
            return;
        }
    }
}

void configure(unsigned mode)
{
    PghConfig &c = gConfig;
    pgh_zero(&c, sizeof c);
    c.mode = mode;
    c.ee = PAGANI_HAPTICS_SPMI_EE;
    c.ppid[PGH_CFG] = uint16_t(PAGANI_HAPTICS_SID << 8 | PAGANI_HAPTICS_PID_CFG);
    c.ppid[PGH_PTN] = uint16_t(PAGANI_HAPTICS_SID << 8 | PAGANI_HAPTICS_PID_PTN);
    c.ppid[PGH_HBST] = uint16_t(PAGANI_HAPTICS_SID << 8 | PAGANI_HAPTICS_PID_HBST);
    c.ppid[PGH_SDAM] = uint16_t(PAGANI_HAPTICS_SDAM_SID << 8 | PAGANI_HAPTICS_PID_SDAM);
    c.t_lra_us = PAGANI_HAPTICS_T_LRA_US;
    c.vmax_mv = PAGANI_HAPTICS_VMAX_MV;
    c.effect_mv = PAGANI_HAPTICS_EFFECT_MV;
    c.fifo_mv = PAGANI_HAPTICS_FIFO_MV;
    const uint8_t brake[8] = PAGANI_HAPTICS_BRAKE;
    for (unsigned i = 0; i < 8; ++i) c.brake[i] = brake[i];
    c.brake_len = PAGANI_HAPTICS_BRAKE_LEN;
    c.brake_mode = PAGANI_HAPTICS_BRAKE_MODE;
    c.brake_gain = PAGANI_HAPTICS_BRAKE_GAIN;
    c.drv_shape = PAGANI_HAPTICS_DRV_SHAPE;
    c.brake_shape = PAGANI_HAPTICS_BRAKE_SHAPE;
    c.drv_duty = PAGANI_HAPTICS_DRV_DUTY;
    c.autores_cfg = PAGANI_HAPTICS_AUTORES;
    c.f0_min_dhz = PAGANI_HAPTICS_F0_MIN_DHZ;
    c.f0_max_dhz = PAGANI_HAPTICS_F0_MAX_DHZ;
    const uint8_t cycles[PAGANI_HAPTICS_EFFECTS] = PAGANI_HAPTICS_EFFECT_CYCLES;
    static_assert(PAGANI_HAPTICS_EFFECTS <= PGH_EFFECTS_MAX, "effects fit the core's table");
    for (unsigned i = 0; i < PAGANI_HAPTICS_EFFECTS; ++i) c.effect_cycles[i] = cycles[i];
    c.effects = PAGANI_HAPTICS_EFFECTS;
    c.vbat_low_mv = PAGANI_HAPTICS_VBAT_LOW_MV;
    c.vbat_low_soc = PAGANI_HAPTICS_VBAT_LOW_SOC;
    c.vbat_cold_soc = PAGANI_HAPTICS_VBAT_COLD_SOC;
    c.vbat_cold_dc = PAGANI_HAPTICS_VBAT_COLD_DC;
#ifdef PAGANI_HAPTICS_PTN_ID   // 0xTTSS once the dry run has shown it (README: pinning)
    c.ptn_type = uint8_t(PAGANI_HAPTICS_PTN_ID >> 8);
    c.ptn_subtype = uint8_t(PAGANI_HAPTICS_PTN_ID & 0xff);
#endif
#ifdef PAGANI_HAPTICS_HBST_ID
    c.hbst_type = uint8_t(PAGANI_HAPTICS_HBST_ID >> 8);
    c.hbst_subtype = uint8_t(PAGANI_HAPTICS_HBST_ID & 0xff);
#endif
    c.hard_mv = PAGANI_HAPTICS_FIFO_MV;
    c.cap_mv = argument("pagani-haptics-vmax-mv", PAGANI_HAPTICS_EFFECT_MV);
    const unsigned play = argument("pagani-haptics-play-ms", 2000);
    c.max_play_ms = play < 10000 ? play : 10000;
    c.max_stream_ms = 30000;
    c.budget_ms = 5000;          // 5 s of drive at once, then 50 % duty
    c.refill_permille = 500;
    c.autores = uint8_t(argument("pagani-haptics-autores", 1) ? 1 : 0);
    c.log = uint8_t(argument("pagani-haptics-log", 1));
    c.spmi_retry = (argument("pagani-audio-srzl", 0) & 4u) ? 1 : 0;   // bit 2 of the audio path's repairs: no argument of its own (the command line is full)
    const unsigned test = argument("pagani-haptics-test", 0);
    gTestMs = test < 500 ? test : 500;
}
}  // namespace

extern "C" void pagani_haptics_start(void)
{
    const unsigned mode = argument("pagani-haptics", 0);
    // The iOS haptics switch first (HapticsIos.h, PaganiHapticsIos.cpp): it only plans /product/haptics here, which load_init_program has
    // made just before launchd; with pagani-haptics-ios absent or 0 it does nothing at all.
    pagani_haptics_ios_publish(argument("pagani-haptics-ios", 0), mode);
    if (mode != PGH_MODE_ON && mode != PGH_MODE_DRY) return;
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    pagani_haptics_status[0] = 1;
    configure(mode);
    gQueueLock = IOSimpleLockAlloc();
    gProducer = IOLockAlloc();
    gHaptics = static_cast<PghCore *>(IOMallocZero(sizeof(PghCore)));
    if (!gQueueLock || !gProducer || !gHaptics) {
        pagani_haptics_status[0] = -7;
        trace("[haptics] FAIL no memory: haptics off");
        return;
    }
    gIo = {nullptr, ioCore, ioCnfg, ioObsRead, ioObsWrite, ioMapChannel, ioChanRead, ioChanWrite, ioDelay};
    gOps = {nullptr, opNow, opWait, opTrace, opBattery};
    char line[160];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] start");
    pgh_text_kd(&t, "mode", mode);
    pgh_text_kd(&t, "cap_mv", gConfig.cap_mv);
    pgh_text_kd(&t, "hard_mv", gConfig.hard_mv);
    pgh_text_kd(&t, "play_ms", gConfig.max_play_ms);
    pgh_text_kd(&t, "autores", gConfig.autores);
    pgh_text_kd(&t, "log", gConfig.log);
    pgh_text_kd(&t, "test_ms", gTestMs);
    trace(line);
    thread_t thread = nullptr;
    if (kernel_thread_start(hapticsThread, nullptr, &thread) != KERN_SUCCESS) {
        pagani_haptics_status[0] = -6;
        trace("[haptics] FAIL thread: haptics off");
        return;
    }
    thread_deallocate(thread);
}

extern "C" int pagani_haptics_constant(uint32_t amplitude_permille, uint32_t duration_ms, uint32_t vmax_mv)
{
    if (!amplitude_permille || amplitude_permille > 1000 || !duration_ms) return -22;
    const PghCmd cmd = {PGH_CMD_CONSTANT, amplitude_permille, duration_ms, vmax_mv};
    return post(cmd);
}

extern "C" int pagani_haptics_effect(uint32_t effect, uint32_t strength_permille)
{
    if (effect >= PAGANI_HAPTICS_EFFECTS || !strength_permille || strength_permille > 1000) return -22;
    const PghCmd cmd = {PGH_CMD_EFFECT, effect, strength_permille, 0};
    return post(cmd);
}

extern "C" int pagani_haptics_stream_begin(uint32_t rate_hz, uint32_t vmax_mv)
{
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    IOLockLock(gProducer);
    const PghCmd cmd = {PGH_CMD_STREAM_BEGIN, rate_hz, vmax_mv, pgh_ring_head(&gHaptics->ring)};
    const int rc = post(cmd);
    IOLockUnlock(gProducer);
    return rc;
}

extern "C" int pagani_haptics_stream_write(const int8_t *samples, uint32_t count)
{
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    if (!samples && count) return -22;
    IOLockLock(gProducer);
    const unsigned taken = pgh_ring_push(&gHaptics->ring, samples, count);
    IOLockUnlock(gProducer);
    return int(taken);
}

extern "C" int pagani_haptics_stream_end(int drain)
{
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    IOLockLock(gProducer);
    const PghCmd cmd = {PGH_CMD_STREAM_END, drain ? 1u : 0u, pgh_ring_head(&gHaptics->ring), 0};
    const int rc = post(cmd);
    IOLockUnlock(gProducer);
    return rc;
}

extern "C" int pagani_haptics_stop(void)
{
    const PghCmd cmd = {PGH_CMD_STOP, 0, 0, 0};
    return post(cmd);
}

extern "C" int pagani_haptics_info(PaganiHapticsInfo *out)
{
    if (!out) return -22;
    pgh_zero(out, sizeof *out);
    out->stage = pagani_haptics_status[0];
    out->state = pagani_haptics_status[1];
    if (!gHaptics) return -19;
    out->f0_dhz = gHaptics->f0_dhz;
    out->t_lra_us = gHaptics->cl_t_lra_us ? gHaptics->cl_t_lra_us : gConfig.t_lra_us;
    out->cap_mv = gHaptics->cfg.cap_mv;
    out->hard_mv = gHaptics->cfg.hard_mv;
    out->fifo_len = gHaptics->fifo_len;
    out->ring_free = PGH_RING - (pgh_ring_head(&gHaptics->ring) - __atomic_load_n(&gHaptics->ring.tail, __ATOMIC_ACQUIRE));
    return __atomic_load_n(&gReady, __ATOMIC_ACQUIRE) ? 0 : -19;
}
