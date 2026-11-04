// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Kernel side of PaganiTorch (PaganiTorch.h, README.md), in the manner of PaganiHaptics.cpp. pagani_torch_start (Stage7Probe, after the
// vibrator start) reads its boot arguments and returns at once without them. Otherwise it allocates the core, registers the sysctl, starts
// one kernel thread, "pagani-torch", and returns. The thread maps the SPMI arbiter's core, ownership and EE 0 observer windows (read only
// uses), runs the core's discovery/identity/dump and, in mode 1, its initialization (TorchCore.c), which maps exactly one RW channel page
// (the flash module's APID, chnls + 0x1000 x APID) after the identity and ownership checks. Then it serves requests and, while the LED is
// on, polls the fault registers and the deadline. All register access is on that thread. Nothing waits on the boot; a dry run, a refusal
// or a fault releases the windows and ends the thread, and every call then returns -19. Every step is a "[torch]" line in the boot log
// (BootProgress ramoops U records, pagani-ramoops=1) and a word in pagani_torch_status.
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
#include "TorchCore.h"
#include "TorchRegs.h"
#include "TorchText.h"
#include "TorchIos.h"
#include "PaganiTorch.h"
#include "PaganiTorchBoard.h"
void pagani_torch_sysctl_register(void);   // PaganiTorchSysctl.c
}

// The statically linked kernel refuses undefined weak references: a provider that may be absent gets a weak definition here.
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
volatile int32_t pagani_torch_status[16];
}

namespace {
enum : uint32_t {
    kIdleMs = 60000,    // nothing to poll while off: requests wake the thread
    kAnswerMs = 200,    // a caller waits this long for the thread
    kObsSize = PGT_ARB_OBS_EE_STRIDE,   // this EE's observer channels
    kPage = PGT_ARB_CHNL_STRIDE,
};

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint32_t size;
};
Window gCoreWin, gCnfgWin, gObsWin, gChanWin;
PgtCore *gTorch;
PgtConfig gConfig;
PgtSpmiIo gIo;
PgtOps gOps;
IOLock *gCaller;
uint32_t gTestMs;
volatile uint32_t gStarted, gReady, gPending;
// The one request in flight: gCaller's holder writes it, the thread answers with its sequence number.
volatile uint32_t gLevel, gRequest, gAnswered;
const char *volatile gWho;
volatile int32_t gAnswer;

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
    unmapOne(gChanWin);
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

// ---- the core's SPMI windows (TorchSpmi.h) ----
uint32_t ioCore(void *, uint32_t at) {return rd32(gCoreWin, at);}
uint32_t ioCnfg(void *, uint32_t at) {return rd32(gCnfgWin, at);}
uint32_t ioObsRead(void *, uint32_t at) {return rd32(gObsWin, at);}
void ioObsWrite(void *, uint32_t at, uint32_t value) {wr32(gObsWin, at, value);}
int ioMapChannel(void *, unsigned periph, unsigned apid)
{
    if (periph != PGT_FLASH || apid >= 1024 || gChanWin.va) return -22;
    return mapOne(gChanWin, PAGANI_TORCH_SPMI_CHNLS + uint64_t(kPage) * apid, kPage) ? 0 : -12;
}
uint32_t ioChanRead(void *, unsigned periph, uint32_t at) {return periph == PGT_FLASH ? rd32(gChanWin, at) : 0xeu;}
void ioChanWrite(void *, unsigned periph, uint32_t at, uint32_t value)
{
    if (periph == PGT_FLASH) wr32(gChanWin, at, value);
}
void ioDelay(void *, unsigned us) {IODelay(us);}

// ---- the core's clock and log ----
uint64_t opNow(void *) {return nowUs();}
void opTrace(void *, const char *line) {trace(line);}

// ---- the thread ----
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

void exportStatus()
{
    int32_t words[16];
    pgt_core_export(gTorch, words);
    for (unsigned i = 0; i < 16; ++i) pagani_torch_status[i] = words[i];
}

void serveRequest(uint32_t &served)
{
    const uint32_t seq = __atomic_load_n(&gRequest, __ATOMIC_ACQUIRE);
    if (seq == served) return;
    served = seq;
    const int rc = pgt_core_set(gTorch, __atomic_load_n(&gLevel, __ATOMIC_RELAXED), 0, gWho);
    __atomic_store_n(&gAnswer, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&gAnswered, seq, __ATOMIC_RELEASE);
}

void torchThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-torch");
    if (!mapOne(gCoreWin, PAGANI_TORCH_SPMI_CORE, PAGANI_TORCH_SPMI_CORE_SZ) ||
        !mapOne(gCnfgWin, PAGANI_TORCH_SPMI_CNFG, PAGANI_TORCH_SPMI_CNFG_SZ) ||
        !mapOne(gObsWin, PAGANI_TORCH_SPMI_OBS + uint64_t(kObsSize) * PAGANI_TORCH_SPMI_EE, kObsSize)) {
        unmapAll();
        pagani_torch_status[0] = -8;
        trace("[torch] FAIL spmi windows not mapped: torch off");
        return;
    }
    const int rc = pgt_core_init(gTorch, &gConfig, &gOps, &gIo);
    exportStatus();
    if (rc) {   // 1: the dry run's log is out; negative: refused or failed (logged by the core)
        unmapAll();
        trace(rc == 1 ? "[torch] dry run done: nothing written, torch off" : "[torch] off");
        return;
    }
    __atomic_store_n(&gReady, 1u, __ATOMIC_RELEASE);
    if (gTestMs) pgt_core_set(gTorch, PGT_LEVEL_MAX, gTestMs, "test");
    uint32_t served = 0;
    uint32_t sleep = pgt_core_poll(gTorch);
    exportStatus();
    for (;;) {
        if (gTorch->state == PGT_ST_FAULT) {   // stopped by the core; off until reboot
            __atomic_store_n(&gReady, 0u, __ATOMIC_RELEASE);
            serveRequest(served);              // a caller still waiting gets the fault, not the timeout
            exportStatus();
            unmapAll();
            trace("[torch] off after a fault");
            return;
        }
        waitMs(sleep ? sleep : kIdleMs);
        serveRequest(served);
        sleep = pgt_core_poll(gTorch);
        exportStatus();
    }
}
}  // namespace

extern "C" void pagani_torch_start(void)
{
    const unsigned mode = argument("pagani-torch", 0);
    // The iOS switch first (TorchIos.h, PaganiTorchIos.cpp): it only plans here, the nodes come just before launchd.
    pagani_torch_ios_plan(argument("pagani-torch-ios", 0), mode);
    if (!pgt_config_make(&gConfig, mode, PAGANI_TORCH_SPMI_EE, uint16_t(PAGANI_TORCH_SID << 8 | PAGANI_TORCH_PID), PAGANI_TORCH_CHANNELS,
                         PAGANI_TORCH_HARD_MA, argument("pagani-torch-max-ma", 0), argument("pagani-torch-log", 1)))
        return;
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    pagani_torch_status[0] = 1;
    gTestMs = mode == PGT_MODE_ON ? pgt_test_ms(argument("pagani-torch-test", 0)) : 0;
    gCaller = IOLockAlloc();
    gTorch = static_cast<PgtCore *>(IOMallocZero(sizeof(PgtCore)));
    if (!gCaller || !gTorch) {
        pagani_torch_status[0] = -7;
        trace("[torch] FAIL no memory: torch off");
        return;
    }
    gIo = {nullptr, ioCore, ioCnfg, ioObsRead, ioObsWrite, ioMapChannel, ioChanRead, ioChanWrite, ioDelay};
    gOps = {nullptr, opNow, opTrace};
    char line[160];
    PgtText t;
    pgt_text_start(&t, line, sizeof line);
    pgt_text_str(&t, "[torch] start");
    pgt_text_kd(&t, "mode", mode);
    pgt_text_kd(&t, "max_ma", gConfig.max_ma);
    pgt_text_kd(&t, "hard_ma", gConfig.hard_ma);
    pgt_text_kx(&t, "chans", gConfig.chan_mask);
    pgt_text_kd(&t, "log", gConfig.log);
    pgt_text_kd(&t, "test_ms", gTestMs);
    trace(line);
    if (mode == PGT_MODE_ON) pagani_torch_sysctl_register();
    thread_t thread = nullptr;
    if (kernel_thread_start(torchThread, nullptr, &thread) != KERN_SUCCESS) {
        pagani_torch_status[0] = -6;
        trace("[torch] FAIL thread: torch off");
        return;
    }
    thread_deallocate(thread);
}

extern "C" int pagani_torch_request(uint32_t level, const char *who)
{
    if (level > PGT_LEVEL_MAX) return -22;
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    IOLockLock(gCaller);
    __atomic_store_n(&gLevel, level, __ATOMIC_RELAXED);
    gWho = who;
    const uint32_t seq = __atomic_load_n(&gRequest, __ATOMIC_RELAXED) + 1;
    __atomic_store_n(&gRequest, seq, __ATOMIC_RELEASE);
    wake();
    int rc = -110;
    for (uint32_t ms = 0; ms <= kAnswerMs; ++ms) {
        if (__atomic_load_n(&gAnswered, __ATOMIC_ACQUIRE) == seq) {
            rc = __atomic_load_n(&gAnswer, __ATOMIC_RELAXED);
            break;
        }
        if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE) && ms) {   // the thread ended before it saw this request
            rc = -19;
            break;
        }
        IOSleep(1);
    }
    IOLockUnlock(gCaller);
    return rc;
}

extern "C" int pagani_torch_set(uint32_t level) {return pagani_torch_request(level, "api");}

extern "C" int pagani_torch_level(void)
{
    return __atomic_load_n(&gReady, __ATOMIC_ACQUIRE) ? pagani_torch_status[2] : -19;
}
