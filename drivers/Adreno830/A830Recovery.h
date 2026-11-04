/* SPDX-License-Identifier: MIT */
#ifndef A830_RECOVERY_H
#define A830_RECOVERY_H
#include "A830PowerState.h"

/* Hang detection and recovery of the A830 (ARCHITECTURE 4.13.4, IMPLEMENTATION_PLAN M10 and M13 b).
 *
 * Global device loss (hang-class faults only; SMMU page faults never come here, they cost the one connection its FAR
 * belongs to): RBBM_INT_0_STATUS HANG_DETECT or CP_HW_ERROR, the software watchdog (2 s without progress while work is
 * in flight), a GMU fatal (CM3 fault bit 15, WDOG_BITE), a WPTR fenced write dropped 200 times, a failed slumber or
 * wake. Every such report goes to the minimal recovery first:
 *   gate (no submission, no new connection) -> RBBM_INT_0_MASK = 0 -> snapshot into the status block -> the in-flight
 *   jobs fail and their connections get faults++ -> GBIF halt (Linux a8xx_bus_clear_pending_transactions: GX side,
 *   then client, then arbiter, then release) -> A830Gmu::forceOff -> [CX powered again only if it was off] -> S4/S5
 *   (GMU firmware reload; the CX timer is not re-seeded while CX stayed up; cold boot with OOB and the GX power check)
 *   -> S6 (hw_init + CP_ME_INIT, the ring empty and the memptrs fence at the last submitted seqno) -> S7 resume
 *   (CP_SET_SECURE_MODE 0 only: the ZAP stays resident; GMEM_PROTECT, HWCG, OOB clear; the annibale profile keeps
 *   OOB_GPU set, as its first bring-up did) -> fence waiters woken ->
 *   interrupt lines and the storm-guarded ones re-enabled (armcpu_spi_enable) -> gate open, "GPU RUN".
 * The SMMU contexts are not touched while CX stayed up. A failed recovery, or the 3rd report within 60 s, makes the
 * loss permanent: the gate stays closed (newUserClient refuses, every connection is faulted), "GPU LOST" stays.
 * In-flight jobs are failed, not replayed (design 4.13.4).
 *
 * report() may be called from any context, interrupt context included (two atomics and env.wake); the recovery itself
 * runs on the service thread (A830Dcvs owns it) or synchronously through recover() (S8 self-test timeout, tests). */

/* Global registers captured before the reset (GX only when it is powered), plus what A830RecoveryOps::snapshot adds. */
struct A830HangSnapshot {
    uint64_t whenNs;
    uint8_t cause;
    bool cxOn,gxOn;
    uint8_t reserved;
    uint32_t fence,lastSubmitted;
    /* GMU (CX) */
    uint32_t gfxPwrClkStatus,rpmhPowerState,cxBusyStatus,aoStatus,gmu2hostInfo,cm3InitResult,ahbFenceStatus,intUnmasked;
    /* GX */
    uint32_t rbbmStatus,rbbmStatus1,int0Status,cpRptr,cpWptr,cpIntGlobal,cpRlError[2];
    uint64_t ib1Base,ib2Base;
    uint32_t ib1Rem,ib2Rem;
    /* A830RecoveryOps::snapshot: BR and BV pipes, SMMU CB0 and CB2 */
    uint32_t pipeHwFault[2],pipeIntStatus[2],pipeStatus[2],pipeProtect[2];
    uint32_t smmuFsr[2];
    uint64_t smmuFar[2];
    uint64_t detail;
};

/* The gate of the ABI shell (NativeAdrenoPublish/UserClient). */
enum A830Gate : uint8_t {A830GateOpen,A830GateRecovering,A830GateLost};

/* Whether a fault is one a client's job can cause, so the hang is charged to the process whose job the CP stood in
 * (native_adreno_recovery_begin, NativeAdrenoClientHangLimit): RBBM HANG_DETECT, CP_HW_ERROR (a protected register or an
 * illegal packet in an IB) and the software watchdog. A GMU fault or watchdog bite, an HFI exchange, the fenced WPTR write,
 * a slumber or wake and the S8 self-test are the driver's own (unlike hangClass, which counts the self-test as a hang). */
static inline bool A830FaultBlamesClient(uint8_t cause)
{
    return cause==A830FaultHang || cause==A830FaultCpHwError || cause==A830FaultWatchdog;
}

struct A830RecoveryOps {
    void *context;
    /* NativeAdrenoSubmit + NativeAdrenoUserClient: every job not retired fails; each owning connection gets faults++.
     * Returns the jobs failed. */
    uint32_t (*failInflight)(void *context);
    /* NativeAdrenoPublish: Open accepts submissions and new connections; Recovering refuses both (and counts a
     * suspend); Lost refuses both for good and faults every connection. cause is the A830FaultCause of the recovery
     * or loss the gate belongs to (the shell charges a hang only for A830FaultBlamesClient causes). */
    void (*gate)(void *context,uint8_t gate,uint8_t cause);
    void (*fenceSignal)(void *context);                               /* A830Fence::signal: wake every waiter */
    void (*irqRestore)(void *context);                                /* A830Irq: armcpu_spi_enable the storm-guarded lines */
    /* A830Cp: read a _PIPE register through CP_APERTURE_CNTL_HOST under the MMIO lock (aperture written back to 0). */
    IOReturn (*readPipe)(void *context,uint8_t pipe,uint32_t reg,uint32_t *value);
    /* A830Cp/A830Smmu: per-pipe CP fault registers, CB0/CB2 FSR/FAR into the snapshot (may be null). */
    void (*snapshot)(void *context,A830HangSnapshot *snapshot);
};

struct A830RecoveryConfig {
    bool enabled=true;              /* M10: a global loss is recovered first; false = M7-M9 behaviour (loss is final) */
    uint32_t windowMs=60000;        /* the 3rd report within this window is final */
    uint8_t maxInWindow=2;          /* recoveries allowed within the window */
    uint32_t watchdogPeriodMs=500;  /* progress check period while work is in flight */
    uint32_t watchdogLimitMs=2000;  /* no progress for this long = hang */
    uint32_t gbifTimeoutUs=100000;  /* each GBIF halt acknowledgement */
};

struct A830RecoveryStats {
    uint32_t reports,coalesced,recoveries,failures,watchdogHangs,jobsFailed;
    uint32_t lastMs,maxMs;
    uint8_t lastCause,lostCause;
    uint16_t lostStep;
    IOReturn lostResult;
    uint32_t gbifTimeouts;
};

class A830Recovery {
public:
    IOReturn init(const A830RecoveryConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops,
                  const A830RecoveryOps &recoveryOps,A830PowerState *power);
    /* Any context: remember the first cause and wake the service thread. Reports during a recovery are coalesced. */
    void report(uint8_t cause,uint64_t detail);
    bool pending() const {return __atomic_load_n(&pendingCause_,__ATOMIC_ACQUIRE)!=A830FaultNone;}
    /* Service thread: run the pending recovery, if any. */
    IOReturn runPending();
    /* Synchronous recovery of `cause` (takes the power mutex). */
    IOReturn recover(uint8_t cause,uint64_t detail);
    /* Final loss without a recovery attempt (also the M7-M9 path). Takes the power mutex. */
    void declareLost(uint8_t cause,uint64_t detail,IOReturn result);
    /* Service thread, every watchdogPeriodMs while work is in flight (A830Dcvs calls it); returns true on a new hang. */
    bool watchdogTick(uint64_t now);
    bool lost() const {return __atomic_load_n(&lost_,__ATOMIC_ACQUIRE);}
    bool busy() const {return __atomic_load_n(&running_,__ATOMIC_ACQUIRE);}   /* a recovery is running */
    const A830HangSnapshot &snapshot() const {return snapshot_;}
    const A830RecoveryStats &stats() const {return stats_;}
    const A830RecoveryConfig &config() const {return config_;}

    /* GBIF halt (4.13.4; KGSL gen8_halt_gbif / Linux a8xx_bus_clear_pending_transactions): returns the acknowledgements
     * that timed out (bit 0 GX, 1 client, 2 arbiter); halts are released in any case. */
    uint32_t haltGbif(bool gx);
    /* Capture the global registers (GX ones only when GFX_PWR_CLK_STATUS says GX is on). */
    void capture(uint8_t cause,uint64_t detail,A830HangSnapshot *out);

private:
    IOReturn recoverLocked(uint8_t cause,uint64_t detail);
    void loseLocked(uint8_t cause,uint8_t stage,uint16_t step,IOReturn result);
    bool allowed(uint64_t now);
    uint32_t rd(uint8_t window,uint32_t offset) const {return io_.read32(io_.context,window,offset);}
    void wr(uint8_t window,uint32_t offset,uint32_t value) const {io_.write32(io_.context,window,offset,value);}
    bool pollGpu(uint32_t dword,uint32_t mask,uint32_t timeoutUs);

    A830RecoveryConfig config_{};
    A830PowerIo io_{};
    A830PowerEnv env_{};
    A830PowerOps ops_{};
    A830RecoveryOps rops_{};
    A830PowerState *power_=nullptr;
    A830HangSnapshot snapshot_{};
    A830RecoveryStats stats_{};
    uint8_t pendingCause_=A830FaultNone;
    uint64_t pendingDetail_=0;
    bool lost_=false,running_=false,initialized_=false;
    uint64_t history_[4]={};            /* start times of the recent recoveries */
    /* watchdog */
    uint64_t wdLastProgress_=0;
    uint32_t wdFence_=0,wdRptr_=0,wdIb1Rem_=0;
    uint64_t wdIb1_=0;
    bool wdArmed_=false;
};

extern "C" {
/* Fault entry for the other modules (A830Irq fault_call, A830Gmu gmuWork/aoIrq, NativeAdrenoRing fenced write, the S8
 * self-test): forwards to the recovery of the running device; a no-op before S10. Interrupt-safe. */
void adreno830_recovery_report(uint8_t cause,uint64_t detail);
/* 0 open, 1 recovering, 2 lost (A830Gate): what the ABI shell checks before a submission or a new connection. */
uint8_t adreno830_recovery_gate(void);
}

#endif
