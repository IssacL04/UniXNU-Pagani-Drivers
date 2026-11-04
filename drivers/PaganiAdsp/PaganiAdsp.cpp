// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The waits, the interrupt and the client entry points follow UniXNU-sheng package/AdspProbe.cpp
// (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e).
//
// Kernel side of PaganiAdsp (README.md): boot argument pagani-adsp=1 starts one kernel thread, "pagani-adsp", that maps the SoC
// windows, runs the bring-up of AdspCore.c and then serves the DSP's doorbells and the clients' sends until reboot. Off (no argument,
// or any other value) nothing is allocated, mapped or started, and every client call reports PaganiAdspOff.
//
// Design (as sheng's): the thread owns every protocol state. The IPCC interrupt only reads and clears its signals, records which
// doorbell rang and wakes the thread; a storm (more than 20000 a second, or signals that never drain) switches the line off and the
// thread polls instead. Nothing panics: every failure is a stage and a code in the status and a "[adsp]" ramoops line
// (BootProgress boot_progress_trace, a weak default when BootProgress is absent).
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
extern "C" {
#include "AdspCore.h"
#include "AdspPlatform.h"
#include "AdspUtil.h"
}

// The statically linked kernel refuses undefined weak references: providers that may be absent get weak defaults here.
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // the word UfsLog.c's writer stops on while it is not 0, bit by bit
__attribute__((weak)) int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) {return 0;}
__attribute__((weak)) int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) {return 0;}
__attribute__((weak)) int native_ufs_part_status(void) {return -1;}
__attribute__((weak)) IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) {return kIOReturnUnsupported;}
__attribute__((weak)) IOReturn armcpu_spi_disable(uint32_t) {return kIOReturnUnsupported;}
void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
extern uint64_t gPhysBase, gPhysSize;
}

namespace {
enum : uint32_t {
    kOff = 0, kInitializing = 1, kReady = 2, kDisabled = 3,
    kIpccIntid = 32 + PAGANI_ADSP_IPCC_SPI,
    kIpccSend = 0x0c, kIpccRecvId = 0x10, kIpccRecvEnable = 0x14, kIpccRecvDisable = 0x18, kIpccRecvClear = 0x1c, kIpccNone = 0xffffffffu,
    kStormPerSecond = 20000,
};
static_assert(PaganiAdspErrScmParked == PaganiAdspPlatScm - static_cast<int>(Sm8750ScmTimeout), "the core's name for a parked secure call");

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t size;
};
bool mapWindow(Window &w, uint64_t base, uint64_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = size;
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

volatile uint32_t gState, gThreadStarted;
IOLock *gLock;
PaganiAdspCore *gCore;
PaganiAdspPlatform *gPlatform;
Sm8750CmdDb *gDb;
Window gSmem, gMutex, gIpcc, gAoss, gRsc, gCmdDb;
volatile uint32_t gPending;
volatile uint32_t gIrqAttached, gIrqOff, gIrqStorms;
uint64_t gIrqWindow;
uint32_t gIrqInWindow;
char gPartition[24] = "modem_a";
const uint32_t kRetryMs[] = {1000, 2000, 4000, 8000, 15000, 30000, 30000};

uint64_t nowNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

// ---- events: the interrupt records and wakes, the thread waits ----
void wake(uint32_t bits)
{
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
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
void ipccIrq(void *)
{
    uint32_t bits = 0;
    unsigned n = 0;
    for (; n < 32; ++n) {
        const uint32_t id = rd(gIpcc, kIpccRecvId);
        if (id == kIpccNone) break;
        wr(gIpcc, kIpccRecvClear, id);
        if (id == (PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalSmp2p)) bits |= PaganiAdspEvSmp2p;
        else if (id == (PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalGlink)) bits |= PaganiAdspEvGlink;
    }
    if (gCore) __atomic_fetch_add(&gCore->status.ipcc_interrupts, 1u, __ATOMIC_RELAXED);
    // Storm guard: signals that never drain, or more interrupts in a second than the DSP ever sends: the line goes off, the thread
    // polls (it is told through EvPoll).
    const uint64_t now = mach_absolute_time();
    uint64_t second = 0;
    nanoseconds_to_absolutetime(1000000000ull, &second);
    if (now - gIrqWindow > second) {gIrqWindow = now; gIrqInWindow = 0;}
    if (n == 32 || ++gIrqInWindow > kStormPerSecond) {
        armcpu_spi_disable(kIpccIntid);
        __atomic_store_n(&gIrqOff, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&gIrqStorms, 1u, __ATOMIC_RELAXED);
        bits |= PaganiAdspEvPoll;
    }
    if (bits) wake(bits);
}

// ---- the PaganiAdspOps of the kernel ----
uint64_t opNow(void *) {return nowNs() / 1000000;}
uint32_t opWait(void *, uint32_t ms)
{
    const bool polling = !gCore->irq_ok || __atomic_load_n(&gIrqOff, __ATOMIC_ACQUIRE);
    if (polling) gCore->irq_ok = 0;
    uint32_t events = waitEvents(polling && ms > 5 ? 5 : ms);
    if (polling) events |= PaganiAdspEvPoll;
    return events;
}
void opWake(void *) {wake(PaganiAdspEvSend);}
void opSleep(void *, uint32_t ms) {IOSleep(ms);}
void opTrace(void *, const char *line) {trace(line);}
void opLock(void *) {IOLockLock(gLock);}
void opUnlock(void *) {IOLockUnlock(gLock);}
void *opAlloc(void *, size_t bytes)
{
    void *memory = IOMallocData(bytes);
    if (memory) bzero(memory, bytes);
    return memory;
}
void opFree(void *, void *memory, size_t bytes) {if (memory) IOFreeData(memory, bytes);}
int opHwlock(void *, int take)
{
    // TCSR mutex 3: write our id (apps = 1), it is ours when it reads back (Linux qcom_hwspinlock); held only for one allocation.
    if (!take) {
        wr(gMutex, 0, 0);
        return 1;
    }
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return 1;
        IODelay(10);
    }
    return 0;
}
void opRing(void *, uint32_t signal) {wr(gIpcc, kIpccSend, PaganiAdspIpccClientLpass << 16 | signal);}
int opDoorbells(void *)
{
    IOReturn rc = kIOReturnNotReady;
    uint32_t readback = 0;
    uint64_t router = 0;
    for (unsigned tries = 0;; ++tries) {   // the GIC driver may come up after this thread
        rc = armcpu_spi_attach(kIpccIntid, false, ipccIrq, nullptr, &readback, &router);
        if (rc != kIOReturnNotReady || tries == 40) break;
        IOSleep(250);
    }
    char line[160];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] ipcc intid=");
    pagani_text_udec(&t, kIpccIntid);
    pagani_text_kx(&t, "attach", rc);
    pagani_text_kx(&t, "readback", readback);
    pagani_text_kx(&t, "router", router);
    trace(line);
    if (rc != kIOReturnSuccess) return -19;
    // Only the LPASS SMP2P and GLINK signals: the AOP's receive signal stays off (the GPU driver polls its QMP replies).
    wr(gIpcc, kIpccRecvEnable, PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalSmp2p);
    wr(gIpcc, kIpccRecvEnable, PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalGlink);
    __atomic_store_n(&gIrqAttached, 1u, __ATOMIC_RELEASE);
    return 0;
}
// A bring-up that ended: what opDoorbells switched on goes off again (no signal of a stopped DSP keeps the line up).
void quietDoorbells()
{
    if (!__atomic_exchange_n(&gIrqAttached, 0u, __ATOMIC_ACQ_REL)) return;
    wr(gIpcc, kIpccRecvDisable, PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalSmp2p);
    wr(gIpcc, kIpccRecvDisable, PaganiAdspIpccClientLpass << 16 | PaganiAdspSignalGlink);
    armcpu_spi_disable(kIpccIntid);
}
void opSurvey(void *, PaganiAdspSurvey *out)
{
    out->aoss_magic = rd(gAoss, Sm8750AossQmpDescMagic);
    out->aoss_version = rd(gAoss, Sm8750AossQmpDescVersion);
    const uint32_t words[4] = {Sm8750AossQmpMcoreLinkState, Sm8750AossQmpMcoreLinkStateAck, Sm8750AossQmpMcoreChState,
                               Sm8750AossQmpUcoreChState};
    for (unsigned i = 0; i < 4; ++i) if (rd(gAoss, words[i]) == Sm8750AossQmpStateUp) out->aoss_links |= 1u << i;
    out->rsc_id = rd(gRsc, 0);
}
void scmLine(const char *what, uint32_t pas, int rc)
{
    const Sm8750SmcResult &r = gPlatform->last;
    char line[200];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] scm ");
    pagani_text_str(&t, what);
    pagani_text_kx(&t, "pas", pas);
    pagani_text_kd(&t, "rc", rc);
    pagani_text_kx(&t, "fid", r.fid);
    pagani_text_kd(&t, "x0", r.x0);
    pagani_text_kx(&t, "x1", r.x1);
    pagani_text_kd(&t, "calls", r.calls);
    pagani_text_kd(&t, "intr", r.interrupted);
    pagani_text_kd(&t, "waitq", r.waitq);
    pagani_text_kd(&t, "busy", r.busy);
    pagani_text_kd(&t, "us", r.elapsed_us);
    pagani_text_kd(&t, "lock_waits", gPlatform->scm_waits);
    trace(line);
}
int opQmp(void *, const char *message)
{
    const int rc = pagani_adsp_platform_qmp(gPlatform, message);
    char line[160];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] qmp");
    pagani_text_kd(&t, "rc", rc);
    pagani_text_kd(&t, "reused", gPlatform->qmp.reused);
    pagani_text_kd(&t, "step", gPlatform->qmp.step);
    pagani_text_kd(&t, "kicks", gPlatform->qmp.kicks);
    pagani_text_kd(&t, "lock_waits", gPlatform->qmp_waits);
    if (rc) {
        pagani_text_kx(&t, "reg", gPlatform->qmp.reg);
        pagani_text_kx(&t, "value", gPlatform->qmp.value);
    }
    trace(line);
    return rc;
}
int opRails(void *, int on)
{
    const int rc = pagani_adsp_platform_rails(gPlatform, on);
    const Sm8750RpmhResult &r = gPlatform->rpmh;
    char line[200];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, on ? "[adsp] rails on" : "[adsp] rails off");
    pagani_text_kd(&t, "rc", rc);
    pagani_text_kx(&t, "lcx", gPlatform->lcx.addr);
    pagani_text_kd(&t, "hlvl", on ? gPlatform->lcx_hlvl : 0);
    pagani_text_kd(&t, "vlvl", on && gPlatform->lcx_hlvl < Sm8750RpmhArcMaxLevels ? gPlatform->lcx.vlvl[gPlatform->lcx_hlvl] : 0);
    pagani_text_kx(&t, "lmx", gPlatform->lmx.addr);
    pagani_text_kd(&t, "hlvl", on ? gPlatform->lmx_hlvl : 0);
    pagani_text_kd(&t, "tcs", (int32_t)r.tcs);
    pagani_text_kd(&t, "step", r.step);
    pagani_text_kd(&t, "polls", r.polls);
    pagani_text_kd(&t, "lock_waits", r.lock_waits);
    if (gPlatform->rails_lookup) pagani_text_kd(&t, "cmd_db", gPlatform->rails_lookup);
    trace(line);
    return rc;
}
int opPasInit(void *, uint32_t pas, const uint8_t *metadata, uint64_t bytes)
{
    const int rc = pagani_adsp_platform_pas_init(gPlatform, pas, metadata, bytes);
    scmLine("init_image", pas, rc);
    return rc;
}
int opPasSetup(void *, uint32_t pas, uint64_t base, uint64_t bytes)
{
    const int rc = pagani_adsp_platform_pas_mem_setup(gPlatform, pas, base, bytes);
    scmLine("mem_setup", pas, rc);
    return rc;
}
int opPasAuth(void *, uint32_t pas)
{
    const int rc = pagani_adsp_platform_pas_auth(gPlatform, pas);
    scmLine("auth_and_reset", pas, rc);
    return rc;
}
int opPasShutdown(void *, uint32_t pas)
{
    const int rc = pagani_adsp_platform_pas_shutdown(gPlatform, pas);
    scmLine("shutdown", pas, rc);
    return rc;
}
void opPasRelease(void *, uint32_t pas, int authenticated) {pagani_adsp_platform_pas_release(gPlatform, pas, authenticated);}
void opPasResult(void *, int64_t *x0, uint64_t *x1)
{
    *x0 = gPlatform->last.x0;
    *x1 = gPlatform->last.x1;
}

// The carveout: reserved no-map memory outside XNU's DRAM, mapped write-combined for the segment writes and unmapped before the
// secure world takes it (as the GPU driver maps its ZAP region).
struct Region {
    IOMemoryDescriptor *descriptor;
    IOMemoryMap *map;
} gRegion;
uint8_t *opMap(void *, uint64_t base, uint64_t bytes)
{
    if (gRegion.map) return nullptr;
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withAddressRange(base, bytes,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone), TASK_NULL);
    if (!descriptor) return nullptr;
    IOMemoryMap *map = descriptor->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
    if (!map) {
        descriptor->release();
        return nullptr;
    }
    gRegion = {descriptor, map};
    return reinterpret_cast<uint8_t *>(map->getVirtualAddress());
}
void opUnmap(void *, uint8_t *, uint64_t)
{
    __asm__ volatile("dsb sy" ::: "memory");   // every segment write has reached memory
    if (gRegion.map) gRegion.map->release();
    if (gRegion.descriptor) gRegion.descriptor->release();
    gRegion = {};
}

// Storage: NativeUfs's read-only partition access (weak defaults above when it is older). A refusal is logged with NativeUfs's
// reason (PartitionTable.h: -1 host not up, -3 I/O, -4 no GPT, -5 not found, -6 duplicate, -7 outside the LUN).
uint32_t gDiskFailures;
void diskLine(const char *what, const char *name, uint64_t a, uint64_t b)
{
    if (gDiskFailures++ >= 8) return;
    char line[160];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] ufs ");
    pagani_text_str(&t, what);
    if (name) pagani_text_ks(&t, "name", name);
    pagani_text_kx(&t, "a", a);
    pagani_text_kx(&t, "b", b);
    pagani_text_kd(&t, "status", native_ufs_part_status());
    trace(line);
}
int diskFind(void *, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    const int found = native_ufs_part_find(name, lun, first, blocks);
    if (!found) diskLine("find refused", name, 0, 0);
    return found;
}
int diskRead(void *, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
    const int ok = native_ufs_part_read(lun, lba, blocks, dst);
    if (!ok) diskLine("read refused", nullptr, static_cast<uint64_t>(lun) << 32 | blocks, lba);
    return ok;
}

// The word UfsLog.c's writer stops on while it is not 0: shared bit by bit, so never a plain store.
void opQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, PAGANI_ADSP_QUIET_BIT, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~uint32_t(PAGANI_ADSP_QUIET_BIT), __ATOMIC_RELEASE);
}

const PaganiAdspOps kOps = {
    nullptr, opNow, opWait, opWake, opSleep, opTrace, opLock, opUnlock, opAlloc, opFree, opHwlock, opRing, opDoorbells, opSurvey,
    opQmp, opRails, opPasInit, opPasSetup, opPasAuth, opPasShutdown, opPasRelease, opPasResult, opMap, opUnmap,
    {nullptr, diskFind, diskRead, opAlloc, opFree}, opQuiet,
};

// ---- the platform layer's callbacks ----
uint32_t ioRead(void *context, uint32_t offset) {return rd(*static_cast<Window *>(context), offset);}
void ioWrite(void *context, uint32_t offset, uint32_t value) {wr(*static_cast<Window *>(context), offset, value);}
void ioDelay(void *, unsigned microseconds)
{
    if (microseconds >= 1000) IOSleep(microseconds / 1000);
    else IODelay(microseconds);
}
const Sm8750AossQmpIo kQmpRam = {&gAoss, ioRead, ioWrite, ioDelay}, kQmpIpcc = {&gIpcc, ioRead, ioWrite, ioDelay};
const Sm8750RpmhIo kDrv2 = {&gRsc, ioRead, ioWrite, ioDelay}, kCmdDb = {&gCmdDb, ioRead, ioWrite, ioDelay};
uint64_t platNow(void *) {return nowNs() / 1000;}
void platSleep(void *, uint32_t microseconds) {ioDelay(nullptr, microseconds);}
int platSmc(void *, uint64_t regs[8]) {return Sm8750ScmSmcInstruction(nullptr, regs);}
void cleanToPoc(const void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
// The PAS metadata: physically contiguous, 4 KiB aligned, cleaned to memory and switched to write-combined in every mapping the pmap
// tracks before the secure world takes it, so no cache line of it can be written back over it (the GPU driver's ZAP page does the
// same). After a successful authentication it goes back cacheable and is freed; otherwise it is never touched again.
struct Meta {
    IOBufferMemoryDescriptor *buffer;
    uint64_t phys, bytes;
};
void setPages(uint64_t phys, uint64_t bytes, unsigned int mode)
{
    for (uint64_t at = 0; at < bytes; at += PAGE_SIZE) pmap_set_cache_attributes(static_cast<ppnum_t>((phys + at) >> PAGE_SHIFT), mode);
}
int metaAlloc(void *, const uint8_t *data, uint64_t bytes, uint64_t *phys, void **handle)
{
    const uint64_t size = (bytes + PAGE_SIZE - 1) & ~static_cast<uint64_t>(PAGE_SIZE - 1);
    Meta *meta = static_cast<Meta *>(opAlloc(nullptr, sizeof(Meta)));
    if (!meta) return -12;
    meta->buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), size,
        0x000000fffffff000ull);
    if (!meta->buffer || meta->buffer->prepare() != kIOReturnSuccess) {
        OSSafeReleaseNULL(meta->buffer);
        opFree(nullptr, meta, sizeof(Meta));
        return -12;
    }
    uint8_t *va = static_cast<uint8_t *>(meta->buffer->getBytesNoCopy());
    IOByteCount contiguous = 0;
    const uint64_t pa = meta->buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    if (!va || !pa || (pa & 0xfff) || contiguous < size || pa < gPhysBase || pa + size - gPhysBase > gPhysSize) {
        meta->buffer->complete();
        meta->buffer->release();
        opFree(nullptr, meta, sizeof(Meta));
        return -14;
    }
    memcpy(va, data, static_cast<size_t>(bytes));
    bzero(va + bytes, static_cast<size_t>(size - bytes));
    cleanToPoc(va, static_cast<size_t>(size));
    setPages(pa, size, VM_WIMG_WCOMB);
    meta->phys = pa;
    meta->bytes = size;
    *phys = pa;
    *handle = meta;
    char line[120];
    PaganiText t;
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] metadata");
    pagani_text_kx(&t, "phys", pa);
    pagani_text_kd(&t, "bytes", static_cast<int64_t>(bytes));
    trace(line);
    return 0;
}
void metaFree(void *, void *handle)
{
    Meta *meta = static_cast<Meta *>(handle);
    if (!meta) return;
    setPages(meta->phys, meta->bytes, VM_WIMG_USE_DEFAULT);
    meta->buffer->complete();
    meta->buffer->release();
    opFree(nullptr, meta, sizeof(Meta));
}

unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}
// How long a line takes to reach the UFS log: its period (pagani-ufslog-ms, default 3 s, UfsLog.h) and a look of the writer, 500 ms at the
// longest. No log, nothing writes: the holds and their waits are left out.
uint32_t crumbMs()
{
    if (argument("sheng-log", 0) != 1) return 0;
    const uint32_t period = argument("pagani-ufslog-ms", 3000);
    return (period >= 1000 && period <= 3000 ? period : 3000) + 500;
}

// The core and the client registry exist from the first call on (pagani_adsp_start or a client), when pagani-adsp=1.
bool ensureCore()
{
    for (;;) {
        uint32_t state = __atomic_load_n(&gState, __ATOMIC_ACQUIRE);
        if (state == kReady) return true;
        if (state == kDisabled) return false;
        if (state == kInitializing) {IOSleep(1); continue;}
        if (!__atomic_compare_exchange_n(&gState, &state, kInitializing, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        if (argument("pagani-adsp", 0) != 1) {
            __atomic_store_n(&gState, kDisabled, __ATOMIC_RELEASE);
            return false;
        }
        char partition[sizeof gPartition] = {};
        if (PE_parse_boot_argn("pagani-adsp-part", partition, sizeof partition) && partition[0])
            pagani_strcpy(gPartition, sizeof gPartition, partition);
        gLock = IOLockAlloc();
        gCore = static_cast<PaganiAdspCore *>(opAlloc(nullptr, sizeof(PaganiAdspCore)));
        if (!gLock || !gCore) {
            trace("[adsp] FAIL st=-1 err=-12 no memory for the core");
            __atomic_store_n(&gState, kDisabled, __ATOMIC_RELEASE);
            return false;
        }
        static const char *const fallback = "modem_b";
        PaganiAdspConfig config = {};
        config.crumb_ms = crumbMs();
        config.stop_after = argument("pagani-adsp-stop", 0);
        config.ready_ms = argument("pagani-adsp-ready-ms", 5000);
        config.partitions[0] = gPartition;
        config.partitions[1] = fallback;
        config.partition_count = pagani_streq(gPartition, "modem_a") ? 2 : 1;
        config.directory = "image";
        config.retry_ms = kRetryMs;
        config.retries = sizeof kRetryMs / sizeof kRetryMs[0];
        pagani_adsp_core_init(gCore, &kOps, &config, nullptr, 0);
        __atomic_store_n(&gState, kReady, __ATOMIC_RELEASE);
        return true;
    }
}

bool mapAll()
{
    return mapWindow(gSmem, PAGANI_ADSP_SMEM_BASE, PAGANI_ADSP_SMEM_SIZE) && mapWindow(gMutex, PAGANI_ADSP_TCSR_MUTEX, 0x1000) &&
           mapWindow(gIpcc, PAGANI_ADSP_IPCC_BASE, 0x1000) && mapWindow(gAoss, PAGANI_ADSP_AOSS_BASE, 0x1000) &&
           mapWindow(gRsc, PAGANI_ADSP_RSC_DRV2, PAGANI_ADSP_RSC_SIZE) && mapWindow(gCmdDb, PAGANI_ADSP_CMD_DB, PAGANI_ADSP_CMD_DB_SIZE);
}

void adspThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-adsp");
    gPlatform = static_cast<PaganiAdspPlatform *>(opAlloc(nullptr, sizeof(PaganiAdspPlatform)));
    gDb = static_cast<Sm8750CmdDb *>(opAlloc(nullptr, sizeof(Sm8750CmdDb)));
    if (!gPlatform || !gDb || !mapAll()) {
        gCore->status.stage = -PaganiAdspStMapped;
        gCore->status.error = PaganiAdspErrNoMemory;
        gCore->dead = 1;
        trace("[adsp] FAIL st=-1 err=-12 windows or memory");
        return;
    }
    PaganiAdspPlatformConfig platform = {};
    platform.now_us = platNow;
    platform.sleep_us = platSleep;
    platform.smc = platSmc;
    platform.meta_alloc = metaAlloc;
    platform.meta_free = metaFree;
    platform.qmp_ram = &kQmpRam;
    platform.ipcc = &kQmpIpcc;
    platform.drv2 = &kDrv2;
    platform.cmd_db = &kCmdDb;
    platform.db = gDb;
    platform.rail_level = argument("pagani-adsp-rail", PaganiAdspRailLevel);
    pagani_adsp_platform_init(gPlatform, &platform);
    gCore->smem.base = gSmem.va;
    gCore->smem.size = PAGANI_ADSP_SMEM_SIZE;
    if (!pagani_adsp_core_boot(gCore)) {
        quietDoorbells();
        if (gDb) {opFree(nullptr, gDb, sizeof(Sm8750CmdDb)); gDb = nullptr;}
        return;   // the status and the log say why; the windows stay mapped (a few pages of VA)
    }
    // Served until reboot: doorbells and client sends; with nothing pending the thread sleeps (a minute, or a second while a send
    // waits for an intent, or 5 ms polls without the interrupt); a wait that runs out looks at everything once (a lost doorbell).
    for (;;) {
        uint32_t events = opWait(nullptr, pagani_adsp_core_idle_ms(gCore));
        pagani_adsp_core_service(gCore, events);
    }
}
}  // namespace

// ---- entry points ----
// Stage7Probe calls this after the GPU start (its weak default does nothing when PaganiAdsp is not linked).
extern "C" void pagani_adsp_start(void)
{
    if (!ensureCore()) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gThreadStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(adspThread, nullptr, &thread) != KERN_SUCCESS) {
        gCore->status.stage = -PaganiAdspStMapped;
        gCore->status.error = -7;
        gCore->dead = 1;
        trace("[adsp] FAIL st=-1 err=-7 no thread");
        return;
    }
    thread_deallocate(thread);
}

extern "C" PaganiAdspChannel *pagani_adsp_channel_config(const PaganiAdspChannelConfig *config, const PaganiAdspClient *client,
                                                         void *context)
{
    return ensureCore() ? pagani_adsp_core_channel(gCore, config, client, context) : nullptr;
}

extern "C" PaganiAdspChannel *pagani_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                                  const PaganiAdspClient *client, void *context)
{
    const PaganiAdspChannelConfig config = {name, intent_size, intent_count, initiator, 0, 0};
    return pagani_adsp_channel_config(&config, client, context);
}

extern "C" int pagani_adsp_send(PaganiAdspChannel *channel, const void *data, uint32_t length)
{
    if (__atomic_load_n(&gState, __ATOMIC_ACQUIRE) != kReady) return PaganiAdspOff;
    return pagani_adsp_core_send(gCore, channel, data, length);
}

extern "C" int pagani_adsp_channel_up(const PaganiAdspChannel *channel)
{
    return channel && __atomic_load_n(&channel->up, __ATOMIC_ACQUIRE) ? 1 : 0;
}

extern "C" int pagani_adsp_status(PaganiAdspStatus *out)
{
    if (!out || __atomic_load_n(&gState, __ATOMIC_ACQUIRE) != kReady) return PaganiAdspOff;
    pagani_adsp_core_status(gCore, out);
    return PaganiAdspOk;
}
