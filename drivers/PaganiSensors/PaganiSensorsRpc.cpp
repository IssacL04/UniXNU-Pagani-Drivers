// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Kernel side of the FastRPC reverse file service (SensorsRpc.h): the GLINK channel, the arena the DSP reads and writes, the sensors PD's
// stream ids in pass-through, the pack on the RAM Data volume, one thread ("pagani-sensors-rpc"). pagani_sensors_rpc_start runs inside
// pagani_sensors_start and only registers and starts; it never waits. Nothing panics: a failure is a stage in the status and a line.
//
// The arena (SSR_ARENA_BYTES at 0xd0100000, pagani-sensors-rpc-pa moves it) is conventional memory below 4 GiB outside XNU's window, right
// after the speaker's period buffer (0xd0000000, 64 KiB; the same reasoning: PaganiSpeaker/SpeakerBoard.h): the DSP takes 32-bit addresses
// and XNU's memory starts at 0x8c0000000. Write-combined, so no cache maintenance: the compute bank's dma-coherent needs a translated
// stream's attributes, and these streams are in pass-through.
//
// The streams: Android maps the sensors PD's context bank (pagani-sensors-rpc-bank moves it) to SIDs 0x1004 (mask 0x80) and 0x1044
// (mask 0x20); the planner of the SoC kit
// (the one UFS, touch and speaker use) maps exact ids, one bypass bank each, so 0x1004 is required, 0x1084, 0x1044 and 0x1064 taken when
// a bank is free (eight banks at most are tried from the top: with the speaker running, the last of these may find none; that is a
// logged line, and only a fault would show that the DSP needs it). The planner has no lock: this runs on the service's thread just before
// the first attach, long after UFS and touch; the speaker's streams come up about then too, so a boot with both is the one to watch.
//
// The pack is /private/var/ngpu/sensors/pack.bin on the Data volume, which launchd mounts long after this thread starts: until
// /private/var/ngpu exists the size answers "not yet", as PaganiWifi's firmware reader does.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
extern "C" {
#include "PaganiAdsp.h"
#include "SensorsKernel.h"
#include "SensorsRpc.h"
#include "SensorsText.h"
extern uint64_t gPhysBase, gPhysSize;
extern void boot_progress_trace(const char *);
}
#include <SmmuTakeover.h>   // SoC kit: the pass-through planner

namespace {
constexpr uint64_t kSmmuBase = 0x15000000ull;      // apps SMMU (PKX110 live devicetree apps-smmu@15000000)
constexpr uint32_t kSmmuSize = 0x100000u;
constexpr uint64_t kArenaPa = 0xd0100000ull;
constexpr uint32_t kProcess = 4242;                // our process id on the DSP: any number, the same in every message
const char kPackDirectory[] = "/private/var/ngpu";
const char kPackPath[] = "/private/var/ngpu/sensors/pack.bin";

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
};
bool mapWindow(Window &w, uint64_t base, uint32_t size, IOOptionBits cache)
{
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
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

SsrCore *gCore;
PaganiAdspChannel *gChannel;
Window gSmmu, gArena;
volatile uint32_t gPending;

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
void wake()
{
    __atomic_store_n(&gPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

// ---- the DSP thread ----
void channelState(void *, int up)
{
    ssr_core_channel(gCore, up);
    wake();
}
void channelReceive(void *, const uint8_t *data, uint32_t length)
{
    ssr_core_receive(gCore, data, length);
    wake();
}

// ---- the operations ----
uint64_t opNow(void *) {return nowNs() / 1000000ull;}
int opSend(void *, const uint8_t *message, uint32_t length) {return pagani_adsp_send(gChannel, message, length) == PaganiAdspOk ? 0 : -1;}
void opWait(void *, uint32_t ms)
{
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&gPending, 0u, __ATOMIC_RELEASE);
}
void opTrace(void *, const char *line) {trace(line);}
void opPublish(void *) {__asm__ volatile("dsb sy" ::: "memory");}

bool dataMounted(vfs_context_t context)
{
    vnode_t root = vfs_rootvnode();
    if (!root) return false;
    vnode_put(root);
    vnode_t directory = NULLVP;
    if (vnode_lookup(kPackDirectory, 0, &directory, context)) return false;
    vnode_put(directory);
    return true;
}
int opPackSize(void *, uint64_t *size)
{
    vfs_context_t context = vfs_context_create(nullptr);
    if (!context) return SsrPackError;
    int rc = SsrPackNotYet;
    if (dataMounted(context)) {
        vnode_t vp = NULLVP;
        const int error = vnode_open(kPackPath, FREAD, 0, 0, &vp, context);
        if (error) {
            rc = error == ENOENT || error == ENOTDIR ? SsrPackAbsent : SsrPackError;
        } else {
            struct vnode_attr va;
            VATTR_INIT(&va);
            VATTR_WANTED(&va, va_data_size);
            rc = vnode_getattr(vp, &va, context) || !VATTR_IS_SUPPORTED(&va, va_data_size) ? SsrPackError : SsrPackOk;
            if (rc == SsrPackOk) *size = va.va_data_size;
            vnode_close(vp, FREAD, context);
        }
    }
    vfs_context_rele(context);
    return rc;
}
int opPackRead(void *, uint64_t offset, void *to, uint32_t length)
{
    vfs_context_t context = vfs_context_create(nullptr);
    if (!context) return -1;
    vnode_t vp = NULLVP;
    int rc = vnode_open(kPackPath, FREAD, 0, 0, &vp, context) ? -1 : 0;
    for (uint32_t done = 0; !rc && done < length;) {
        int resid = 0;
        const int chunk = int(length - done);
        if (vn_rdwr(UIO_READ, vp, static_cast<caddr_t>(to) + done, chunk, off_t(offset + done), UIO_SYSSPACE, IO_NOAUTH,
                    vfs_context_ucred(context), &resid, vfs_context_proc(context)) || resid == chunk)
            rc = -1;
        done += uint32_t(chunk - resid);
    }
    if (vp) vnode_close(vp, FREAD, context);
    vfs_context_rele(context);
    return rc;
}
void *opAlloc(void *, uint32_t bytes) {return IOMallocData(bytes);}

// ---- the streams ----
int smmuCode(const char *e)
{
    if (!__builtin_strcmp(e, "conflicting UFS stream mapping") || !__builtin_strcmp(e, "reserved context bank already used") ||
        !__builtin_strcmp(e, "no free context bank") || !__builtin_strcmp(e, "no free stream-match group")) return -16;
    if (!__builtin_strcmp(e, "reserved context bank not bypass")) return -1;
    size_t n = 0;
    while (e[n]) ++n;
    if (n >= 8 && !__builtin_strcmp(e + n - 8, "readback")) return -5;
    return -19;
}
int passThrough(uint16_t sid, uint64_t pa, uint32_t bytes, uint32_t *detail)
{
    auto read = [](unsigned at) {return rd(gSmmu, at);};
    auto write = [](unsigned at, uint32_t value) {wr(gSmmu, at, value);};
    const UfsSmmuPlan plan = planUfsSmmu(sid, pa, bytes, read, kSmmuSize);
    *detail = (plan.slot & 0xffu) | (plan.bank & 0xffu) << 8 | (plan.reuse ? 1u << 16 : 0u);
    const char *error = configureUfsSmmu(plan, sid, read, write);
    if (error) {
        *detail |= (plan.s2cr & 0xffu) << 24;
        return smmuCode(error);
    }
    unsigned polls = 0;
    uint32_t status = 0;
    if (!syncUfsSmmu(read, write, [&] {IODelay(10); return ++polls >= 10000;}, status)) return -110;
    return 0;
}
uint64_t gArenaPa;
uint32_t gBank = SSR_CONTEXT_BANK;
int opPrepare(void *)
{
    const uint16_t base = uint16_t(0x1000 + gBank), sids[4] = {base, uint16_t(base + 0x80), uint16_t(base + 0x40), uint16_t(base + 0x60)};
    char line[160];
    SntText t;
    int required = -19;
    snt_start(&t, line, sizeof line);
    snt_str(&t, "[sensors] rpc smmu");
    if (mapWindow(gSmmu, kSmmuBase, kSmmuSize, kIOMapInhibitCache)) {
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t detail = 0;
            const int rc = passThrough(sids[i], gArenaPa, SSR_ARENA_BYTES, &detail);
            if (!i) required = rc;
            snt_kx(&t, "sid", sids[i]);
            snt_kd(&t, "rc", rc);
            snt_kx(&t, "at", detail);
        }
    } else {
        required = -12;
        snt_str(&t, " no window");
    }
    trace(line);
    return required;
}

const SsrOps kOps = {nullptr, opNow, opSend, opWait, opTrace, opPublish, opPrepare, opPackSize, opPackRead, opAlloc};

void rpcThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-sensors-rpc");
    while (ssr_core_step(gCore)) {}
}
}  // namespace

extern "C" int pagani_sensors_rpc_start(unsigned log)
{
    uint64_t pa = kArenaPa;
    PE_parse_boot_argn("pagani-sensors-rpc-pa", &pa, sizeof pa);
    const uint64_t end = pa + SSR_ARENA_BYTES;
    if ((pa & 0xfff) || end > 0x100000000ull || (pa < gPhysBase + gPhysSize && end > gPhysBase)) return -22;
    PE_parse_boot_argn("pagani-sensors-rpc-bank", &gBank, sizeof gBank);
    if (gBank < 3 || gBank > 8) return -22;      // the devicetree's six compute banks
    gCore = static_cast<SsrCore *>(IOMallocZeroData(sizeof(SsrCore)));
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(SSR_BOUNCE));
    if (!gCore || !bounce || !mapWindow(gArena, pa, SSR_ARENA_BYTES, kIOMapWriteCombineCache)) return -12;
    gArenaPa = pa;
    const SsrConfig config = {const_cast<uint8_t *>(gArena.va), pa, SSR_ARENA_BYTES, bounce, kProcess, gBank, log};
    ssr_core_init(gCore, &kOps, &config);
    static const PaganiAdspClient client = {channelState, channelReceive};
    const PaganiAdspChannelConfig channel = {SSR_CHANNEL, SSR_INTENT_SIZE, SSR_INTENT_COUNT, 0, 64, 4};
    gChannel = pagani_adsp_channel_config(&channel, &client, nullptr);
    if (!gChannel) return -19;                   // PaganiAdsp off or not linked
    thread_t thread = nullptr;
    if (kernel_thread_start(rpcThread, nullptr, &thread) != KERN_SUCCESS) return -7;
    thread_deallocate(thread);
    return 0;
}

extern "C" int pagani_sensors_rpc_status(SsrStatus *out)
{
    if (!gCore || !out) return -19;
    *out = gCore->status;                        // a snapshot for a log line: a torn counter does no harm
    return 0;
}
