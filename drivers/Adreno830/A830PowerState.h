/* SPDX-License-Identifier: MIT */
#ifndef A830_POWER_STATE_H
#define A830_POWER_STATE_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* GPU power state machine of the A830 (ARCHITECTURE 4.17, IMPLEMENTATION_PLAN M13 e/f): P0 (default) keeps CX, the GMU
 * and GX up and only lets the DCVS drop to the floor level; P1 slumber powers the GPU down after 250 ms without work
 * (sequence S1..S10, or S1..S9 for the CX-keeping variant) and wakes it on the next submission or, earlier, on a touch;
 * P2 runs the GMU with IFPC. The screen rule (config.screenSlumber, pagani-screenoff bits 1 and 3) adds the same slumber
 * while the display is off, under P0 too: entered once the display has been off and the GPU quiet for screenIdleMs, left at
 * the next submission or as soon as the display comes back. The same bring-up operations serve the wake path here and the
 * recovery of A830Recovery.
 * The annibale profile (A830Profile.h, read at run time) keeps OOB_GPU set from every bring-up until slumber (or for the
 * whole boot under P0): slumber entry releases that reference, and under P2 GX therefore never collapses between
 * submissions (IFPC is negotiated but idle).
 *
 * Other modules are reached through two injected tables, so this file depends on no other driver header:
 *  - A830PowerIo: 32-bit MMIO in the GPU, GMU, RSCC and CX_MISC windows (byte offsets of A830Regs.h, ARCHITECTURE 0.3);
 *  - A830PowerOps: the design-named operations of the other modules (each member says which one it is bound to).
 * Kernel services (time, sleep, the power mutex, the service thread event) come through A830PowerEnv; the kernel
 * implementation is a830_kernel_power_env(), host tests drive a virtual clock.
 *
 * Concurrency. One mutex (env lock) serializes every power transition, every DCVS vote and the recovery. Submitters
 * hold an activity reference (acquire/release) while they touch the ring or GX registers; slumber starts only when no
 * reference is held and every submitted seqno has retired. Short register readers (DCVS busy counter, timer, watchdog)
 * use accessBegin/accessEnd, which never wake the GPU and never block; a transition waits for them to drain. Both use
 * an increment-then-check protocol against the phase (sequentially consistent), so a racing transition either sees the
 * reference or the holder sees the transition and backs off. */

/* Register windows of A830PowerIo (byte offsets: A830_GPU_REG_BYTE, A830_GMU_REG_BYTE, A830_RSCC_REG_BYTE,
 * A830_CX_MISC_REG_BYTE of A830Regs.h). */
enum A830PowerWindow : uint8_t {A830PowerWinGpu,A830PowerWinGmu,A830PowerWinRscc,A830PowerWinCxMisc,A830PowerWinCount};

struct A830PowerIo {
    void *context;
    uint32_t (*read32)(void *context,uint8_t window,uint32_t offset);
    void (*write32)(void *context,uint8_t window,uint32_t offset,uint32_t value);
    void (*barrier)(void *context);                                  /* dsb sy */
};

/* Kernel services. nowNs is monotonic; sleepUs may block (thread context); delayUs spins; lock/unlock is the power
 * mutex (sleepable); wait blocks the service thread until wake() or the absolute deadline; wake is interrupt-safe;
 * startThread starts the one service thread (A830Dcvs::run). */
struct A830PowerEnv {
    void *context;
    uint64_t (*nowNs)(void *context);
    void (*sleepUs)(void *context,uint32_t us);
    void (*delayUs)(void *context,uint32_t us);
    void (*lock)(void *context);
    void (*unlock)(void *context);
    void (*wait)(void *context,uint64_t deadline_ns);
    void (*wake)(void *context);
    IOReturn (*startThread)(void *context,void (*body)(void *argument),void *argument);
};

/* Arguments of A830PowerOps::gmuStart. */
struct A830GmuStartArgs {
    uint8_t level,bus;      /* GMU level and DDR bus level of the first GX_BW_PERF_VOTE (HFI M9) */
    bool ifpc;              /* HFI M5: FEATURE_CTRL{9,1,0x1680} (P2) instead of {9,0,0} */
    bool rsccWake;          /* B3: the last power-down ran the RSCC sleep sequence (4.7.4) */
    bool cxLost;            /* CX collapsed since the last start: TCM and GMU-side registers are gone */
};

/* Line bits of A830PowerOps::irqEnable: the A830IrqOff* bits of Adreno830Status.h. */
enum : uint32_t {
    A830PowerIrqGpu=1u<<0,A830PowerIrqHfi=1u<<1,A830PowerIrqGmu=1u<<2,A830PowerIrqSmmuCb0=1u<<3,A830PowerIrqSmmuCb2=1u<<4,
    A830PowerIrqCxHost=1u<<5,
    A830PowerIrqGpuDomain=A830PowerIrqGpu|A830PowerIrqHfi|A830PowerIrqGmu|A830PowerIrqCxHost,
    A830PowerIrqCxDomain=A830PowerIrqGpuDomain|A830PowerIrqSmmuCb0|A830PowerIrqSmmuCb2,
};

/* Operations of the other modules (bound by Adreno830Start.cpp). All are thread context; none takes the power mutex.
 * A member may be null only where noted. */
struct A830PowerOps {
    void *context;
    /* 4.2.1: Sm8750GpuPowerOn (the caller holds the host cx/mx votes; they are never revoked); in the annibale profile
     * followed by the checked CX_MISC AHB timeouts of S1 (a830CxAhbTimeout). */
    IOReturn (*cxPowerOn)(void *context);
    /* 4.2.3: Sm8750GpuPowerOff; *warnings = Sm8750GpuPowerTrace.warnings (Sm8750GpuOffCxStillOn: another voter kept CX). */
    IOReturn (*cxPowerOff)(void *context,uint32_t *warnings);
    /* A830Smmu: reprogram CB2 (GMU), CB0 (GPU), the LPAC route and the aperture after CX collapsed (4.5.4-4.5.6). */
    IOReturn (*smmuRestore)(void *context);
    /* 4.4.2 a830CxTimerSeed: only before reloading the GMU after CX collapsed (never while the GMU runs). */
    void (*cxTimerSeed)(void *context);
    /* S4 reload + S5: A830GmuFirmware::load, A830Gmu::coldBoot (B1-B8, RSCC wake, GENERAL_9 written back),
     * A830Hfi::init + A830Hfi::start with the cached tables (M1, M2, M5, M6, M8, M9), A830Gmu::oobSet, waitGxOn, the
     * GMU-side registers of 4.7.8. On failure *step is the 8.3 step of S5 (or S4) that failed. */
    IOReturn (*gmuStart)(void *context,const A830GmuStartArgs *args,uint16_t *step);
    IOReturn (*gmuForceOff)(void *context);                           /* A830Gmu::forceOff (4.7.9) */
    IOReturn (*rsccSleep)(void *context);                             /* A830Gmu::rsccSleep (4.7.4) */
    IOReturn (*oobSet)(void *context);                                /* A830Gmu::oobSet (OOB_GPU, reference counted) */
    void (*oobClear)(void *context);                                  /* A830Gmu::oobClear (a reference with none held: no-op) */
    /* A830Hfi::send: one H2F command (payload without the header) and its acknowledgement. */
    IOReturn (*hfiSend)(void *context,uint8_t id,const uint32_t *payload,uint8_t dwords);
    /* S6: A830Cp::hwInit + startCp. The ring restarts empty (software RPTR/WPTR 0, ring page cleared) and the memptrs
     * fence word is `fence` once it returns (written after any memptrs clearing), so no waiter sees it go back. */
    IOReturn (*cpStart)(void *context,uint32_t fence,uint16_t *step);
    /* S7 without PAS (the ZAP stays resident): CP_SET_SECURE_MODE 0, RB_GC_GMEM_PROTECT, pwrup list, HWCG,
     * OOB_GPU clear (A830Cp::exitSecure, resume form). The annibale profile leaves the OOB_GPU of gmuStart set (S7 does
     * the same, kernel-sequences section 14 row 28); slumber entry then drops that reference with its own (S2). */
    IOReturn (*secureResume)(void *context,uint16_t *step);
    /* GIC lines (A830PowerIrq* bits): armcpu_spi_disable/enable of the wired lines (A830Irq). enable=true turns back
     * on only what an enable=false of this table turned off, never a line the storm guard switched off. May be null. */
    void (*irqEnable)(void *context,uint32_t lines,bool enable);
    uint32_t (*lastSubmitted)(void *context);                         /* NativeAdrenoRing: last seqno written */
    uint32_t (*retired)(void *context);                               /* memptrs fence (BR completed seqno) */
};

enum A830PowerPhase : uint8_t {
    A830PowerDown,          /* before start() */
    A830PowerActive,        /* GMU running, GPU usable (P2: GX may collapse between submissions) */
    A830PowerEntering,      /* S1..S10 running (mutex held) */
    A830PowerSlumber,       /* GMU stopped; CX off (P1) or kept (CX-keeping) */
    A830PowerWaking,        /* wake sequence running (mutex held) */
    A830PowerRecovering,    /* A830Recovery owns the GPU */
    A830PowerLost,          /* permanent device loss */
};
enum A830SlumberMode : uint8_t {A830SlumberOff,A830SlumberFull,A830SlumberCxKeep};
enum A830WakeReason : uint8_t {A830WakeSubmit,A830WakeTouch,A830WakeIfpc,A830WakeExplicit,A830WakeScreen};

/* Fault causes reported to A830Recovery (also the step of a "GPU LOST S15.<cause>" row). */
enum A830FaultCause : uint8_t {
    A830FaultNone,
    A830FaultHang,          /* RBBM_INT_0_STATUS HANG_DETECT (bit 23) */
    A830FaultCpHwError,     /* RBBM_INT_0_STATUS CP_HW_ERROR (bit 9) */
    A830FaultWatchdog,      /* software watchdog: 2 s without progress with work in flight */
    A830FaultGmu,           /* GMU2HOST CM3 fault (bit 15), NMI, F2H error */
    A830FaultGmuWatchdog,   /* GMU AO WDOG_BITE (bit 0) */
    A830FaultWptrFence,     /* WPTR fenced write dropped 200 times (4.13.2) */
    A830FaultSelfTest,      /* S8 V9b/V9c timeout (pagani-gpu-cohtest=1) */
    A830FaultSlumber,       /* slumber entry failed after the GPU interrupts were masked */
    A830FaultWake,          /* wake from slumber failed */
    A830FaultHfi,           /* an HFI exchange of a power transition failed */
    A830FaultCount
};

/* Step numbers stored in adreno830_status.step / the stats (8.3 style): slumber S1..S10, wake 0x11.. */
enum : uint16_t {
    A830SlumberStepIdle=1,A830SlumberStepOob=2,A830SlumberStepLowestIdle=3,A830SlumberStepRpmh=4,A830SlumberStepPrepare=5,
    A830SlumberStepCxIdle=6,A830SlumberStepRscc=7,A830SlumberStepGbif=8,A830SlumberStepGmuIrq=9,A830SlumberStepPowerOff=10,
    A830WakeStepPowerOn=0x11,A830WakeStepSmmu=0x12,A830WakeStepTimer=0x13,A830WakeStepGmu=0x14,A830WakeStepCp=0x15,
    A830WakeStepSecure=0x16,A830WakeStepIrq=0x17,
};

struct A830PowerConfig {
    uint8_t slumber=A830SlumberOff;   /* P1 (M13 e); Off is P0, the design default */
    bool ifpc=false;                  /* P2 (M13 f): idle level IFPC, HFI FEATURE_CTRL{9,1,0x1680} */
    bool touchPrewake=true;           /* a touch while asleep wakes the GPU before the first submission */
    uint8_t defaultLevel=2,defaultBus=2;   /* PREPARE_SLUMBER {bw = default bus | 0xFFFF0000, freq = default level} */
    uint32_t idleMs=250;              /* no submission and everything retired for this long: slumber */
    uint32_t drainUs=100000;          /* a transition waits this long for activity/access holders to leave */
    bool ahbTimeout=false;            /* M13 c: CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL = 0x1B12 after each CX power-up */
    /* Slumber while the display is off (pagani-screenoff bit 1: CxKeep, with bit 3: Full; the board's knob): the system turned
     * the display off (setScreenOff, from pagani_screen_state) and the GPU was quiet for screenIdleMs since then and since its
     * last work. Meanwhile touches neither count as activity nor pre-wake; a submission wakes it as always, and the display
     * coming back wakes it at once (A830WakeScreen). A failed entry or wake of such a slumber ends the rule for the boot
     * (screenBlocked; the recovery takes the GPU back as for any power failure). Off: the screen state is not looked at. */
    uint8_t screenSlumber=A830SlumberOff;
    uint32_t screenIdleMs=1000;
};

struct A830PowerStats {
    uint32_t entries,aborts,failures,wakes,touchWakes,ifpcCycles;
    uint32_t lastWakeUs,maxWakeUs,lastEntryUs;
    uint64_t sleptNs;                 /* total time in Slumber */
    uint16_t lastStep;                /* step of the last failure (A830SlumberStep* / A830WakeStep*) */
    uint16_t lastSubStep;             /* the failing operation's own 8.3 step (gmuStart, cpStart, secureResume) */
    IOReturn lastError;
    uint32_t warnings;                /* Sm8750GpuPowerOff warnings of the last full slumber */
    uint32_t screenEntries,screenWakes;   /* slumbers entered by the screen rule; wakes because the display came back */
    bool screenBlocked;               /* a screen-rule slumber or its wake failed: the rule is off for the boot */
};

/* Host builds have no kIOReturnNotResponding in their IOKit double. */
#ifdef ADRENO830_HOST_TEST
inline constexpr IOReturn A830ReturnNotResponding=0x2ed;
#else
inline constexpr IOReturn A830ReturnNotResponding=kIOReturnNotResponding;
#endif

class A830PowerState {
public:
    /* Copies the tables; validates that the members it calls are present (BadArgument otherwise). */
    IOReturn init(const A830PowerConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops);
    /* S10: the GPU runs at GMU level `level` / bus `bus` (the M9 vote of S5); phase Active. */
    void start(uint8_t level,uint8_t bus);

    /* Submission path (thread context): keep the GPU usable. acquire wakes it from slumber (blocking, a few ms) and
     * fails with A830ReturnNotResponding while it is lost or being recovered; release is lock-free. */
    IOReturn acquire();
    void release();
    /* Short readers: true only while Active (P2 + gx: under OOB_GPU, so GX stays up); pair with accessEnd(same gx). */
    bool accessBegin(bool gx);
    void accessEnd(bool gx);

    /* Service thread: idle -> slumber, touch pre-wake, IFPC switch, the screen rule. Returns the next deadline it needs (ns). */
    uint64_t tick(uint64_t now);
    void noteTouch();                       /* a touch was seen (service thread, from the DCVS touch sampler) */
    /* The display's power as the DCVS service thread read it (pagani_screen_state bit 0) at `now`; ignored without
     * config.screenSlumber. screenSlept: the GPU sleeps and the display was off when it went to sleep (the display coming back
     * wakes it). */
    void setScreenOff(bool off,uint64_t now);
    bool screenOff() const {return __atomic_load_n(&screenOff_,__ATOMIC_ACQUIRE);}
    bool screenSlept() const {return __atomic_load_n(&screenSlept_,__ATOMIC_ACQUIRE);}
    IOReturn wake(A830WakeReason reason);   /* takes the mutex; no-op when Active */
    IOReturn enterSlumber(uint8_t mode);    /* takes the mutex; S1..S10 (Full) or S1..S9 (CxKeep) */
    void setIfpc(bool enable);              /* applied by the next idle power cycle (the GMU must restart) */

    /* DCVS: the level of the next GMU start (HFI M9), and the start level after a submission/touch wake (boosts). */
    void setTarget(uint8_t level,uint8_t bus);
    void setWakeTargets(uint8_t submitLevel,uint8_t submitBus,uint8_t touchLevel,uint8_t touchBus);
    void noteVoted(uint8_t level,uint8_t bus);   /* a GX_BW_PERF_VOTE was acknowledged */

    /* Recovery (A830Recovery, mutex held by the caller): Recovering drains the holders (config.drainUs); endRecovery
     * returns to Active (ok) or Lost; markLost is final. */
    IOReturn beginRecovery();
    void endRecovery(bool ok,uint8_t level,uint8_t bus);
    void markLost();
    /* Where slumber/wake failures go (A830Recovery::report). */
    void setFaultSink(void (*sink)(void *context,uint8_t cause,uint64_t detail),void *context);

    void lock();
    void unlock();
    A830PowerPhase phase() const {return A830PowerPhase(__atomic_load_n(&phase_,__ATOMIC_SEQ_CST));}
    bool cxOn() const {return __atomic_load_n(&cxOn_,__ATOMIC_ACQUIRE);}
    bool cxLost() const {return cxLost_;}
    void setCxState(bool on,bool lost) {__atomic_store_n(&cxOn_,on,__ATOMIC_RELEASE);cxLost_=lost;cxRestored_=false;}
    bool ifpcActive() const {return __atomic_load_n(&ifpcActive_,__ATOMIC_ACQUIRE);}
    bool ifpcRequested() const {return ifpcWanted_;}
    bool rsccSlept() const {return rsccSlept_;}
    void setRsccSlept(bool slept) {rsccSlept_=slept;}
    uint8_t targetLevel() const {return __atomic_load_n(&targetLevel_,__ATOMIC_RELAXED);}
    uint8_t targetBus() const {return __atomic_load_n(&targetBus_,__ATOMIC_RELAXED);}
    uint8_t votedLevel() const {return __atomic_load_n(&votedLevel_,__ATOMIC_RELAXED);}
    uint8_t votedBus() const {return __atomic_load_n(&votedBus_,__ATOMIC_RELAXED);}
    uint32_t submittedSeqno() const {return ops_.lastSubmitted(ops_.context);}
    /* Work submitted and not yet retired (memptrs fence behind the last seqno written). */
    bool inFlight() const {return ops_.retired(ops_.context)!=ops_.lastSubmitted(ops_.context);}
    uint32_t holders() const {return __atomic_load_n(&refs_,__ATOMIC_SEQ_CST);}
    uint64_t lastActivityNs() const {return __atomic_load_n(&lastActivity_,__ATOMIC_ACQUIRE);}
    const A830PowerStats &stats() const {return stats_;}
    const A830PowerConfig &config() const {return config_;}
    uint64_t now() const {return env_.nowNs(env_.context);}

    /* M13 c (4.4.2): AHB accesses that hang get an AHB error before the NOC times out. CX_MISC loses it with CX, so
     * bringUp writes it again after a CX power-up when config.ahbTimeout is set (S1 writes it at power-on through
     * a830CxAhbTimeout; the annibale profile does both always, checked, inside cxPowerOn). */
    void armAhbTimeout();

    /* The GPU bring-up shared with the recovery (mutex held): optional CX power-on, SMMU/timer restore after a
     * collapse, GMU start (S4/S5), CP start (S6, memptrs fence = last submitted), secure resume (S7), interrupt lines.
     * Leaves the phase alone. *step is the A830WakeStep* reached (the failing one on error), *subStep the operation's
     * own step; progress (may be null) is called as each step begins. */
    IOReturn bringUp(uint8_t level,uint8_t bus,bool powerOn,uint16_t *step,uint16_t *subStep,
                     void (*progress)(void *context,uint16_t step),void *progressContext);

private:
    IOReturn wakeLocked(A830WakeReason reason);
    IOReturn enterLocked(uint8_t mode,uint64_t now);
    bool drain(uint32_t us);
    void fail(uint16_t step,IOReturn result,uint8_t cause);
    bool poll(uint8_t window,uint32_t offset,uint32_t mask,uint32_t value,uint32_t timeoutUs,uint32_t stepUs);
    uint32_t rd(uint8_t window,uint32_t offset) const {return io_.read32(io_.context,window,offset);}
    void wr(uint8_t window,uint32_t offset,uint32_t value) const {io_.write32(io_.context,window,offset,value);}
    void publishSlumber(uint64_t now);

    A830PowerConfig config_{};
    A830PowerIo io_{};
    A830PowerEnv env_{};
    A830PowerOps ops_{};
    A830PowerStats stats_{};
    void (*sink_)(void *,uint8_t,uint64_t)=nullptr;
    void *sinkContext_=nullptr;
    uint8_t phase_=A830PowerDown;
    uint32_t refs_=0,access_=0,gxAccess_=0;
    uint64_t lastActivity_=0,sleptAt_=0,idleSince_=0,nextSlumberRow_=0;
    uint32_t seenSubmitted_=0;
    uint8_t targetLevel_=2,targetBus_=2,submitLevel_=0,submitBus_=0,touchLevel_=0,touchBus_=0;
    uint8_t votedLevel_=0,votedBus_=0,slumberMode_=A830SlumberOff;
    bool cxOn_=true,cxLost_=false,cxRestored_=false,rsccSlept_=false,ifpcActive_=false,ifpcWanted_=false,touchPending_=false;
    bool initialized_=false;
    /* the screen rule (config.screenSlumber) */
    bool screenOff_=false,screenSlept_=false,screenEntering_=false;
    uint64_t screenOffAt_=0;
};

/* The 8.3 stage (A830Stage) an A830WakeStep* belongs to: S1 power, S3 SMMU, S4 timer, S5 GMU, S6 CP, S7 secure, S8 irq. */
uint8_t A830PowerStepStage(uint16_t step);

extern "C" {
/* Kernel services (IOLock, IOSleep/IODelay, assert_wait_deadline/thread_block, thread_wakeup) for one service thread.
 * Returns a static environment; nullptr when the mutex cannot be allocated. Not built in host tests. */
const A830PowerEnv *a830_kernel_power_env(void);
/* The power state the rest of the driver consults (NativeAdrenoTimer: accessBegin(false) before reading the GMU AO
 * counter, which is unreadable once CX collapsed; A830Irq: ifpcActive() for the KEEPALIVE of P2). Set by
 * A830PowerState::start(); nullptr before. */
A830PowerState *adreno830_power_state(void);
}

#endif
