/* SPDX-License-Identifier: MIT */
#ifndef A830_IRQ_H
#define A830_IRQ_H
#include "A830Sched.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Interrupts of the A830 driver (ARCHITECTURE 4.12, PLAN M6/M10/M13):
 *   - the platform SPI layer: PD/drivers/PaganiArmCpu's C exports, reached through A830IrqPlatform;
 *   - A830IrqGuard: the generic storm guard every level handler of the driver uses (332 here; 710/712 in A830Smmu, 336/337 in
 *     A830Gmu, 112 here): N consecutive entries without progress mask the source on the device side, then
 *     armcpu_spi_disable(), set the irq_off bit (reason 23) and the owner falls back to polling;
 *   - A830Irq: the GPU interrupt 332 (kgsl_3d0_irq, the only hard dependency) with its top half A830Irq::primary (touches
 *     only RBBM_INT_0_STATUS 0x6A, RBBM_INT_CLEAR_CMD 0x61 and RBBM_INT_0_MASK 0x62 while IFPC is off), the bottom halves
 *     faultWork and retireWork on thread calls, the optional cx_host_irq 112, and the global-loss path (snapshot, then the
 *     fault sink: A830Recovery from M10, else the device is marked lost). */

class A830Fence;

/* ---- Platform SPI layer (ARCHITECTURE 4.12.1) ----
 * PaganiArmCpu exports these. The static kernel link refuses undefined weak references, so A830Irq.cpp carries weak
 * defaults that answer kIOReturnUnsupported (count: 0): a board without an export behaves as "missing". */
extern "C" {
IOReturn armcpu_spi_attach(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router);
uint64_t armcpu_spi_count(uint32_t intid);
IOReturn armcpu_spi_disable(uint32_t intid);   /* GICD_ICENABLER + RWP; only an attached INTID; the slot stays */
IOReturn armcpu_spi_enable(uint32_t intid);    /* GICD_ISENABLER; only an attached INTID */
}
struct A830IrqPlatform {
    IOReturn (*attach)(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router);
    IOReturn (*disable)(uint32_t intid);   /* null or kIOReturnUnsupported: missing, the device-side mask stands alone */
    IOReturn (*enable)(uint32_t intid);
    uint64_t (*count)(uint32_t intid);
};
/* The PaganiArmCpu exports (through the weak defaults above). */
const A830IrqPlatform *a830IrqArmCpu(void);
/* GIC readback of armcpu_spi_attach: bit0 Group1, bit1 enabled, bit2 edge, bits[15:8] priority 0xA0. */
enum : uint32_t {A830IrqReadbackMask=0xFF07u,A830IrqReadbackLevel=0xA003u,A830IrqReadbackEdge=0xA007u};

/* ---- MMIO windows (32-bit accesses at byte offsets of a Device-nGnRE mapping) ---- */
struct A830Mmio {
    void *cookie;
    uint32_t (*read32)(void *cookie,uint32_t offset);
    void (*write32)(void *cookie,uint32_t offset,uint32_t value);
    bool valid() const {return read32 && write32;}
};
/* A window the kernel mapped (cookie = its base address). */
A830Mmio a830MmioDirect(volatile void *base);

/* ---- The driver's SPIs, in irq_off bit order (ARCHITECTURE 8.2; A830IrqOff* of Adreno830Status.h) ---- */
enum A830IrqLine : uint8_t {
    A830IrqLineGpu,A830IrqLineHfi,A830IrqLineGmu,A830IrqLineSmmuCb0,A830IrqLineSmmuCb2,A830IrqLineCxHost,A830IrqLineScmWaitq,
    A830IrqLineCount
};

/* ---- Storm guard of one SPI (ARCHITECTURE 4.12.1) ----
 * Every level handler, after clearing its source, reads the status again and calls note(entry, reread): no progress is an
 * entry with status 0, or a re-read that still holds every bit just cleared (332: the error bits only, every bit while
 * A830IrqConfig::own_mask is set). threshold consecutive entries without progress
 * (A830_IRQ_STORM_GPU 1000 for 332, A830_IRQ_STORM_OTHER 64 for the others) trip the guard: the device-side mask callback,
 * then armcpu_spi_disable (when the export exists), then the irq_off bit (shown with reason 23); the owner polls from then
 * on (off() is true). rearm() after a recovery re-initialized the device side: armcpu_spi_enable and the irq_off bit clear.
 * note() and trip() run in the primary interrupt context: no lock, no sleep. Edge lines are attached here too (994) but
 * never trip. */
class A830IrqGuard final {
    const A830IrqPlatform *platform_=nullptr;
    void (*mask_)(void *)=nullptr;
    void *mask_context_=nullptr;
    uint32_t intid_=0,off_bit_=0,threshold_=0,readback_=0;
    uint64_t router_=0;
    uint32_t stalls_=0,trips_=0;
    uint64_t entries_=0;
    bool attached_=false,edge_=false,off_=false,platform_disabled_=false;
public:
    A830IrqGuard()=default;
    A830IrqGuard(const A830IrqGuard &)=delete;
    A830IrqGuard &operator=(const A830IrqGuard &)=delete;
    /* Attaches the SPI (Group1, priority 0xA0, routed to the boot CPU, enabled). The readback is recorded; for a level line
     * it must read 0xA003 under 0xFF07 (edge 0xA007) when check_readback is set, else the line is disabled again (best
     * effort) and reported off. Any failure (no export, attach error, readback) sets the irq_off bit and returns the
     * error: the caller polls. mask(mask_context) masks the source on the device side (never null for a level line). */
    IOReturn attach(const A830IrqPlatform *platform,uint32_t intid,bool edge,uint32_t off_bit,uint32_t threshold,
                    void (*handler)(void *),void *context,void (*mask)(void *),void *mask_context,bool check_readback);
    /* Generic progress rule; returns true when this entry tripped the guard (the handler returns at once). */
    bool note(uint32_t entry_status,uint32_t reread_status);
    /* The handler's own verdict (332 applies the error-bit rule of ARCHITECTURE 4.12.3). */
    bool noteProgress(bool progressed);
    /* Turn the line off now (device-side mask + SPI disable + irq_off): storm, or a caller that stops using it. */
    void trip();
    /* After the device side was re-initialized (recovery, slumber exit): enable the SPI again, clear irq_off. */
    IOReturn rearm();
    /* Slumber: disable the SPI without counting a storm (no irq_off bit); resume() undoes it. */
    IOReturn suspend();
    IOReturn resume();
    bool attached() const {return attached_;}
    bool off() const {return __atomic_load_n(&off_,__ATOMIC_ACQUIRE);}
    uint32_t intid() const {return intid_;}
    uint32_t readback() const {return readback_;}
    uint64_t router() const {return router_;}
    uint64_t entries() const {return __atomic_load_n(&entries_,__ATOMIC_RELAXED);}
    uint32_t trips() const {return trips_;}
    uint32_t stalls() const {return stalls_;}
    /* The platform's own count of handled entries (armcpu_spi_count), 0 without it. */
    uint64_t platformCount() const;
};

/* ---- Global faults (ARCHITECTURE 4.13.4) ---- */
enum A830FaultKind : uint8_t {
    A830IrqFaultNone,
    A830IrqFaultHang,         /* RBBM_INT_0_STATUS HANG_DETECT (bit23) */
    A830IrqFaultCpHwError,    /* RBBM_INT_0_STATUS CP_HW_ERROR (bit9) */
    A830IrqFaultWatchdog,     /* software watchdog: 2 s of work in flight without progress (A830Fence) */
    A830IrqFaultGmu,          /* GMU fatal: CM3 fault (GMU2HOST bit15) or WDOG_BITE (A830Gmu reports it here) */
    A830IrqFaultWptr,         /* the fenced WPTR write still dropped after 200 tries (NativeAdrenoRing reports it here) */
    A830IrqFaultAhbFence,     /* IFPC: the AHB fence never returned to ALLOW in the top half (KGSL gen8_irq_poll_fence) */
    A830IrqFaultSelfTest,     /* an S8 coherence probe (V9b/V9c) did not complete within 100 ms */
    A830IrqFaultKindCount
};
/* A830PowerState.h owns the A830Fault* names of its fault causes; these two have no counterpart there. */
static constexpr A830FaultKind A830FaultWptr=A830IrqFaultWptr,A830FaultAhbFence=A830IrqFaultAhbFence;
/* What the bottom half (or the reporter) saw: written to a830_fault_last and handed to the sink. */
struct A830FaultReport {
    uint8_t kind;            /* A830FaultKind */
    uint8_t reserved8;
    uint16_t reason;         /* A830Reason: 18 hang/watchdog/self-test, 17 CP fault, 14 GMU, 6 WPTR/AHB fence */
    uint32_t int_status;     /* RBBM_INT_0_STATUS bits that led here (0 for watchdog/GMU/WPTR) */
    uint32_t rbbm_status,rbbm_status1;   /* 0x12, 0x13 */
    uint32_t rptr,wptr;      /* CP_RB_RPTR 0x806, CP_RB_WPTR 0x807 */
    uint32_t ib1_rem,ib2_rem;   /* BR CP_IB1_REM_SIZE 0x902, CP_IB2_REM_SIZE 0x906 */
    uint64_t ib1_base,ib2_base; /* BR CP_IB1_BASE 0x900/0x901, CP_IB2_BASE 0x904/0x905 */
    uint32_t cp_global;      /* CP_INTERRUPT_STATUS_GLOBAL 0x8B5 */
    uint32_t pipe_hw[2],pipe_sw[2],pipe_protect[2],pipe_opcode[2];   /* BR, BV: 0x8B3, 0x8B7, 0x84F, SQE_STAT[1] */
    uint32_t rl_error[2];    /* CP_RL_ERROR_DETAILS_0/1 0x840/0x841 (AHB error) */
    uint32_t unmasked;       /* GMUAO RBBM_INT_UNMASKED_STATUS_SHADOW 0x23B15 (when the GMU window is given) */
    uint32_t fence,submitted;   /* memptrs fence and the newest submitted seqno (A830Fence) */
    uint32_t smmu_fsr;       /* CB0 FSR and FAR (the snapshot hook, A830Smmu) */
    uint64_t smmu_far;
    uint64_t detail;         /* watchdog: stalled ms; GMU: GMU2HOST_INTR_INFO | AO status << 32; WPTR: retries */
    uint64_t when;           /* a830Now() */
    bool gx_read;            /* the GX registers above were read (GX was on) */
};
extern "C" A830FaultReport a830_fault_last;   /* the latest global fault (host tools, registry snapshot) */
/* The latest non-fatal error interrupt (recorded only, as Linux and KGSL only log them). */
struct A830IrqErrorRecord {
    uint32_t bits;           /* RBBM_INT_0_STATUS error bits of the batch */
    uint32_t fuse;           /* RBBM_SW_FUSE_INT_STATUS (SW_FUSE_VIOLATION; the fuse interrupt is then masked) */
    uint64_t tsb;            /* RBBM_SECVID_TSB_STATUS (TSB_WRITE_ERROR) */
    uint32_t rl_error[2];    /* CP_RL_ERROR_DETAILS_0/1 (CP_AHB_ERROR) */
    uint32_t batches;        /* error batches handled so far */
    uint32_t reserved;
    uint64_t when;
};
extern "C" A830IrqErrorRecord a830_irq_error_last;

/* What A830Irq needs from the rest of the driver; every member optional. Wired by the orchestrator. */
struct A830IrqHooks {
    void *cookie;
    /* A830Cp's MMIO/aperture lock: held around every CP_APERTURE_CNTL_HOST selection (never in the top half). Without it
     * A830Irq serializes its own aperture uses only. */
    void (*lockMmio)(void *cookie);
    void (*unlockMmio)(void *cookie);
    /* retireWork: release the jobs whose seqno <= completed (the submit path / publish shell, ARCHITECTURE 5.4). */
    void (*retire)(void *cookie,uint32_t completed);
    /* A global loss (thread context): the recovery entry (A830Recovery, M10) or the publish shell's device-lost handling.
     * Without it A830Irq marks the fence lost and publishes "GPU LOST" itself. */
    void (*deviceLost)(void *cookie,const A830FaultReport *report);
    /* CB0 FSR/FAR for the snapshot (A830Smmu). */
    void (*smmuSnapshot)(void *cookie,uint32_t *fsr,uint64_t *far);
    /* A non-fatal error interrupt was recorded (the status bits): the owner may republish the status page. */
    void (*errorNoted)(void *cookie,uint32_t status);
};

struct A830IrqConfig {
    A830Mmio gpu;        /* GPU window 0x03D00000 (offset = 4 x dword); required */
    A830Mmio gmu;        /* GMU window 0x03D37000 (A830_GMU_REG_BYTE): IFPC keepalive, AHB fence, unmasked shadow; optional */
    A830Mmio cx_misc;    /* CX_MISC window 0x03D9E000 (A830_CX_MISC_REG_BYTE): cx_host_irq 112; optional */
    const A830IrqPlatform *platform;   /* null: a830IrqArmCpu() */
    A830Fence *fence;    /* signalled on CP_CACHE_FLUSH_TS (bit20); its retire state decides retire_call */
    A830IrqHooks hooks;
    /* RBBM_INT_0_MASK this object writes itself while 332 is attached (after attach, and in restore() after the device side
     * was re-initialized): 0 leaves the mask to A830Cp's hw_init (the design programs A8XX_INT_MASK there). The annibale
     * profile's hw_init masks every GPU interrupt; with pagani-gpu-irq=1 the orchestrator sets A830_INT_MASK_FENCE_ONLY here,
     * so only the ring fence interrupts and the error bits stay with annibale's polled checks and the watchdog. */
    uint32_t own_mask;
};

/* Counters of the interrupt path (relaxed; status block counters are updated as well). */
struct A830IrqStats {
    uint64_t entries;        /* primary entries */
    uint64_t fence_irqs;     /* entries with bit20 */
    uint64_t rb_done,pm4;    /* bit17, bit15: normal events, counted */
    uint64_t spurious;       /* entries with status 0 */
    uint64_t errors;         /* entries with an error bit */
    uint64_t retire_calls,fault_calls;
    uint64_t last_fence_irq; /* a830Now() of the latest bit20 */
    uint32_t error_bits;     /* OR of every error bit seen */
    uint32_t last_status;
    uint64_t cx_events,cx_freq_limits;   /* 112: entries, GPU_CC frequency-limit events */
    uint64_t keepalive_timeouts;         /* IFPC: AHB fence not in ALLOW within 100 us */
};

class A830Irq final {
    A830IrqConfig config_{};
    A830IrqGuard gpu_guard_,cx_guard_;
    A830Call *retire_call_=nullptr,*fault_call_=nullptr,*lost_call_=nullptr;
    A830IrqStats stats_{};
    uint32_t pending_=0;          /* OR of every status the top half cleared (ARCHITECTURE 4.12.3 step 2) */
    uint32_t error_pending_=0;    /* error bits handed to faultWork */
    uint32_t lost_kind_=0;        /* a global loss raised from the top half (AHB fence), handled by lostWork */
    uint32_t self_lock_=0;        /* aperture serialization without a hook */
    bool started_=false,gx_on_=true,ifpc_=false,fatal_masked_=false,loss_active_=false;
    bool polled_=false;           /* fallBack(): 332 stays off for the boot, restore() leaves it off */
    static void primaryTrampoline(void *context);
    static void cxHostTrampoline(void *context);
    static void retireCall(void *param0,void *param1);
    static void faultCall(void *param0,void *param1);
    static void lostCall(void *param0,void *param1);
    static void maskGpu(void *context);
    static void maskCx(void *context);
    uint32_t gpuRead(uint32_t dword);
    void gpuWrite(uint32_t dword,uint32_t value);
    void lockMmio();
    void unlockMmio();
    bool keepAlive(bool on);      /* IFPC: KEEPALIVE and the AHB fence poll; false when the fence stayed closed */
    bool gxAcquire();             /* thread context: GX may be read (on, and kept alive under IFPC); gxRelease() follows */
    void gxRelease();
    uint32_t readPipe(unsigned pipe,uint32_t dword);    /* under lockMmio */
    void writePipe(unsigned pipe,uint32_t dword,uint32_t value);
    void report(A830FaultKind kind,uint16_t reason,uint32_t int_status,uint64_t detail,A830FaultReport *out);
    void lose(A830FaultKind kind,uint16_t reason,uint32_t int_status,uint64_t detail);
public:
    A830Irq()=default;
    A830Irq(const A830Irq &)=delete;
    A830Irq &operator=(const A830Irq &)=delete;
    /* Stores the configuration and allocates retire_call, fault_call and lost_call; touches no hardware. Once. */
    IOReturn start(const A830IrqConfig &config);
    /* S8 (ARCHITECTURE 3.4, 4.12.1): clears the RBBM interrupt status (the mask was programmed in S6), attaches 332 level
     * with primary as handler, records the GIC readback and router (record only: the S8 self-test is the criterion), then
     * writes config.own_mask when it is set (annibale, pagani-gpu-irq=1). An error (no export, attach refused) is reason 19
     * for the caller; the irq_off bit is set. Once. */
    IOReturn attach(uint32_t intid);
    /* The annibale profile's pagani-gpu-irq=1 when the S8 check finds the fence interrupt missing (no count, or the waiter
     * not woken by it): 332 off for the rest of the boot (device-side mask 0, SPI disabled, irq_off bit 0), so waiters poll
     * their 16 ms slice and the DCVS thread retires, as in the first round. restore() leaves it off. Thread context. */
    void fallBack();
    bool polled() const {return __atomic_load_n(&polled_,__ATOMIC_ACQUIRE);}
    /* The top half (ARCHITECTURE 4.12.3); handler context = this. */
    void primary();
    static void primary(void *context) {static_cast<A830Irq *>(context)->primary();}
    /* Optional cx_host_irq 112 (only the GPU_CC frequency-limit events are counted): clears and unmasks
     * CX_MISC_INT_0 bit31, attaches with the storm guard. Failure only sets irq_off bit5. */
    IOReturn attachCxHost(uint32_t intid);
    void cxHost();
    /* Bottom halves (thread calls, ARCHITECTURE 4.12.3): faultWork reads the CP fault state under the MMIO lock and raises a
     * global loss for HANG_DETECT and CP_HW_ERROR; retireWork hands the completed fence to the retire hook. */
    void faultWork();
    void retireWork();
    /* A global fault seen elsewhere (watchdog, GMU fatal, WPTR, self-test; thread context): RBBM_INT_0_MASK = 0, snapshot,
     * a830_fault_last, then the sink (deviceLost) or, without one, the fence marked lost and "GPU LOST" published. A second
     * loss while one is being handled is only recorded. */
    void escalate(A830FaultKind kind,uint16_t reason,uint64_t detail);
    /* The CP state of the snapshot, as escalate() takes it (thread context; takes the MMIO lock). */
    void snapshot(A830FaultReport *out);
    /* Progress probe of the software watchdog (thread context): CP_RB_RPTR and the BR IB1 base and remaining size. */
    bool probe(uint32_t *rptr,uint64_t *ib1_base,uint32_t *ib1_rem);
    /* Kick retire_call (an observer that saw the fence pass while the interrupt is off). Any context. */
    void kickRetire();
    /* Recovery and slumber (ARCHITECTURE 4.13.4, 4.17): quiesce() reads RBBM_INT_0_STATUS and, when nothing pending is
     * unmasked, writes RBBM_INT_0_MASK = 0, suspends 332 at the GIC and marks GX off (the top half then never reads a GX
     * register); kIOReturnBusy when an interrupt is pending (slumber gives up). force skips that check (recovery).
     * restore() after the device side was re-initialized (A830Cp programmed the mask again, or own_mask is written here):
     * GX on, the fatal mask state cleared, 332 (and 112) enabled again at the GIC, guards re-armed and irq_off cleared;
     * after fallBack() 332 stays off. */
    IOReturn quiesce(bool force);
    IOReturn restore();
    /* IFPC (P2, M13): the top half and the snapshot keep GX alive (GMU KEEPALIVE, AHB fence poll 100 x 1 us). */
    void setIfpc(bool on) {__atomic_store_n(&ifpc_,on,__ATOMIC_RELEASE);}
    bool ifpc() const {return __atomic_load_n(&ifpc_,__ATOMIC_ACQUIRE);}
    void setGxOn(bool on) {__atomic_store_n(&gx_on_,on,__ATOMIC_RELEASE);}
    bool attached() const {return gpu_guard_.attached();}
    bool off() const {return gpu_guard_.off();}   /* 332 turned off (storm) or never attached: fence waits poll */
    uint32_t readback() const {return gpu_guard_.readback();}
    uint64_t router() const {return gpu_guard_.router();}
    uint64_t count() const {return gpu_guard_.entries();}
    uint64_t fenceIrqs() const {return __atomic_load_n(&stats_.fence_irqs,__ATOMIC_RELAXED);}
    uint64_t lastFenceIrq() const {return __atomic_load_n(&stats_.last_fence_irq,__ATOMIC_ACQUIRE);}
    uint32_t pending() const {return __atomic_load_n(&pending_,__ATOMIC_RELAXED);}
    bool fatalMasked() const {return __atomic_load_n(&fatal_masked_,__ATOMIC_RELAXED);}
    A830IrqStats stats() const;
    A830IrqGuard &gpuGuard() {return gpu_guard_;}
    A830IrqGuard &cxGuard() {return cx_guard_;}
    const A830Mmio &gpuMmio() const {return config_.gpu;}
    const A830Mmio &gmuMmio() const {return config_.gmu;}
    /* The device's interrupt object (set by start(); null before): for the GMU, ring and recovery reporters. */
    static A830Irq *instance();
};
#endif
