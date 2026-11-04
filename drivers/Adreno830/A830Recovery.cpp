/* SPDX-License-Identifier: MIT */
#include "A830Recovery.h"
#include "A830Regs.h"
#include "Adreno830Status.h"

namespace {
constexpr uint64_t NsPerMs=1000000;
A830Recovery *gRecovery;

constexpr uint32_t gpuReg(uint32_t dword) {return A830_GPU_REG_BYTE(dword);}
constexpr uint32_t gmuReg(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}

/* 8.3 reason of a fault cause. */
uint16_t reasonOf(uint8_t cause)
{
    switch(cause) {
    case A830FaultHang: case A830FaultWatchdog: case A830FaultSelfTest: return A830ReasonHang;
    case A830FaultCpHwError: return A830ReasonCpFault;
    case A830FaultGmu: case A830FaultGmuWatchdog: return A830ReasonGmuFault;
    case A830FaultWptrFence: return A830ReasonTimeout;
    case A830FaultHfi: return A830ReasonHfiStatus;
    default: return A830ReasonUnexpectedState;   /* slumber, wake */
    }
}
bool hangClass(uint8_t cause) {return cause==A830FaultHang || cause==A830FaultWatchdog || cause==A830FaultSelfTest;}

void sinkReport(void *context,uint8_t cause,uint64_t detail) {static_cast<A830Recovery *>(context)->report(cause,detail);}

struct Progress {A830Recovery *self;uint64_t begin;const A830PowerEnv *env;};
void showProgress(void *context,uint16_t step)
{
    auto *progress=static_cast<Progress *>(context);
    const uint64_t now=progress->env->nowNs(progress->env->context);
    adreno830_status_recovering(A830PowerStepStage(step),uint32_t((now-progress->begin)/NsPerMs));
}
}

IOReturn A830Recovery::init(const A830RecoveryConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops,
                            const A830RecoveryOps &recoveryOps,A830PowerState *power)
{
    if(!power || !io.read32 || !io.write32 || !io.barrier || !env.nowNs || !env.sleepUs || !env.wake)return kIOReturnBadArgument;
    if(!ops.gmuForceOff || !ops.gmuStart || !ops.cpStart || !ops.secureResume || !ops.lastSubmitted || !ops.retired)
        return kIOReturnBadArgument;
    if(!recoveryOps.failInflight || !recoveryOps.gate)return kIOReturnBadArgument;
    if(!config.windowMs || !config.watchdogPeriodMs || config.watchdogLimitMs<config.watchdogPeriodMs || !config.gbifTimeoutUs)
        return kIOReturnBadArgument;
    config_=config;io_=io;env_=env;ops_=ops;rops_=recoveryOps;power_=power;
    snapshot_={};stats_={};
    pendingCause_=A830FaultNone;pendingDetail_=0;lost_=running_=false;
    for(auto &start:history_)start=0;
    wdArmed_=false;
    power_->setFaultSink(sinkReport,this);
    initialized_=true;
    __atomic_store_n(&gRecovery,this,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

/* ---- reports --------------------------------------------------------------------------------------------------- */

void A830Recovery::report(uint8_t cause,uint64_t detail)
{
    if(!initialized_ || cause==A830FaultNone || cause>=A830FaultCount)return;
    __atomic_fetch_add(&stats_.reports,1u,__ATOMIC_RELAXED);
    if(__atomic_load_n(&lost_,__ATOMIC_ACQUIRE))return;
    uint8_t expected=A830FaultNone;
    if(__atomic_load_n(&running_,__ATOMIC_ACQUIRE) ||
       !__atomic_compare_exchange_n(&pendingCause_,&expected,cause,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&stats_.coalesced,1u,__ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&pendingDetail_,detail,__ATOMIC_RELEASE);
    env_.wake(env_.context);
}

IOReturn A830Recovery::runPending()
{
    if(!pending())return kIOReturnSuccess;
    power_->lock();
    const uint8_t cause=__atomic_exchange_n(&pendingCause_,uint8_t(A830FaultNone),__ATOMIC_ACQ_REL);
    const uint64_t detail=__atomic_load_n(&pendingDetail_,__ATOMIC_ACQUIRE);
    const IOReturn result=cause==A830FaultNone?kIOReturnSuccess:recoverLocked(cause,detail);
    power_->unlock();
    return result;
}

IOReturn A830Recovery::recover(uint8_t cause,uint64_t detail)
{
    if(!initialized_ || cause==A830FaultNone || cause>=A830FaultCount)return kIOReturnBadArgument;
    power_->lock();
    /* A report of the same incident that is still pending is answered by this run. */
    __atomic_store_n(&pendingCause_,uint8_t(A830FaultNone),__ATOMIC_RELEASE);
    const IOReturn result=recoverLocked(cause,detail);
    power_->unlock();
    return result;
}

void A830Recovery::declareLost(uint8_t cause,uint64_t detail,IOReturn result)
{
    if(!initialized_)return;
    power_->lock();
    if(!lost()) {
        __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
        rops_.gate(rops_.context,A830GateRecovering,cause);
        power_->beginRecovery();
        capture(cause,detail,&snapshot_);
        if(snapshot_.gxOn) {wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_MASK),0);io_.barrier(io_.context);}
        if(rops_.snapshot)rops_.snapshot(rops_.context,&snapshot_);
        stats_.jobsFailed+=rops_.failInflight(rops_.context);
        adreno830_status_add32(&adreno830_status.faults,1);
        if(hangClass(cause))adreno830_status_add32(&adreno830_status.hangs,1);
        loseLocked(cause,A830SRunning,cause,result);
    }
    power_->unlock();
}

bool A830Recovery::allowed(uint64_t now)
{
    /* history_ keeps the start times of the recent recoveries; the 3rd report within windowMs is final. */
    const uint64_t window=uint64_t(config_.windowMs)*NsPerMs;
    unsigned recent=0;
    for(const uint64_t start:history_)
        if(start && now-start<window)++recent;
    if(recent>=config_.maxInWindow)return false;
    unsigned oldest=0;
    for(unsigned i=1;i<sizeof history_/sizeof history_[0];++i)
        if(history_[i]<history_[oldest])oldest=i;
    history_[oldest]=now?now:1;
    return true;
}

/* ---- the recovery --------------------------------------------------------------------------------------------- */

bool A830Recovery::pollGpu(uint32_t dword,uint32_t mask,uint32_t timeoutUs)
{
    for(uint32_t waited=0;;waited+=10) {
        if((rd(A830PowerWinGpu,gpuReg(dword))&mask)==mask)return true;
        if(waited>=timeoutUs)return false;
        env_.sleepUs(env_.context,10);
    }
}

uint32_t A830Recovery::haltGbif(bool gx)
{
    /* Linux a8xx_bus_clear_pending_transactions: GX side first (only with GX up), then new client requests, then the
     * AXI arbiter, then the GBIF halt released. RBBM_GBIF_HALT stays: hw_init step 1 writes it back to 0. */
    uint32_t timeouts=0;
    if(gx) {
        wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_GBIF_HALT),A8XX_RBBM_GBIF_HALT_GX);
        io_.barrier(io_.context);
        if(!pollGpu(A8XX_RBBM_GBIF_HALT_ACK,A8XX_RBBM_GBIF_HALT_GX,config_.gbifTimeoutUs))timeouts|=1u;
    }
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_CLIENT);
    io_.barrier(io_.context);
    if(!pollGpu(A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_CLIENT,config_.gbifTimeoutUs))timeouts|=2u;
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_ARB);
    io_.barrier(io_.context);
    if(!pollGpu(A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_ARB,config_.gbifTimeoutUs))timeouts|=4u;
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),0);
    io_.barrier(io_.context);
    stats_.gbifTimeouts+=uint32_t(__builtin_popcount(timeouts));
    return timeouts;
}

void A830Recovery::capture(uint8_t cause,uint64_t detail,A830HangSnapshot *out)
{
    A830HangSnapshot s={};
    s.whenNs=env_.nowNs(env_.context);
    s.cause=cause;s.detail=detail;
    s.fence=ops_.retired(ops_.context);
    s.lastSubmitted=ops_.lastSubmitted(ops_.context);
    s.cxOn=power_->cxOn();
    if(s.cxOn) {
        s.gfxPwrClkStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS));
        s.rpmhPowerState=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_RPMH_POWER_STATE));
        s.cxBusyStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_GPU_CX_BUSY_STATUS));
        s.aoStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS));
        s.gmu2hostInfo=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO));
        s.cm3InitResult=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_CM3_FW_INIT_RESULT));
        s.ahbFenceStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS));
        s.intUnmasked=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW));
        s.gxOn=!(s.gfxPwrClkStatus&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF);
    }
    if(s.gxOn) {
        /* GX registers only with GX powered: an access to a collapsed GX is a synchronous external abort (4.7.8). */
        s.rbbmStatus=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_STATUS));
        s.rbbmStatus1=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_STATUS1));
        s.int0Status=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_STATUS));
        s.cpRptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_RPTR));
        s.cpWptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_WPTR));
        s.cpIntGlobal=rd(A830PowerWinGpu,gpuReg(A8XX_CP_INTERRUPT_STATUS_GLOBAL));
        s.cpRlError[0]=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RL_ERROR_DETAILS_0));
        s.cpRlError[1]=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RL_ERROR_DETAILS_1));
        if(rops_.readPipe) {
            uint32_t lo=0,hi=0;
            if(rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE,&lo)==kIOReturnSuccess &&
               rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE_HI,&hi)==kIOReturnSuccess)
                s.ib1Base=uint64_t(hi)<<32|lo;
            rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,&s.ib1Rem);
            lo=hi=0;
            if(rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB2_BASE,&lo)==kIOReturnSuccess &&
               rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB2_BASE_HI,&hi)==kIOReturnSuccess)
                s.ib2Base=uint64_t(hi)<<32|lo;
            rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB2_REM_SIZE,&s.ib2Rem);
        }
    }
    *out=s;
    /* Status block: what a photo of the overlay needs to place the loss (8.2 reg/value/expect). */
    adreno830_status.reg=A830_GPU_REG_PHYS(A8XX_RBBM_STATUS);
    adreno830_status.value=uint64_t(s.int0Status)<<32|s.rbbmStatus;
    adreno830_status.expect=uint64_t(s.lastSubmitted)<<32|s.fence;
    if(s.aoStatus)adreno830_status_sticky(A830StickyGmuAo,s.aoStatus);
}

void A830Recovery::loseLocked(uint8_t cause,uint8_t stage,uint16_t step,IOReturn result)
{
    __atomic_store_n(&lost_,true,__ATOMIC_RELEASE);
    stats_.lostCause=cause;stats_.lostStep=step;stats_.lostResult=result;
    power_->markLost();
    rops_.gate(rops_.context,A830GateLost,cause);
    adreno830_status_lost(stage,step,reasonOf(cause),result);
    __atomic_store_n(&running_,false,__ATOMIC_RELEASE);
}

IOReturn A830Recovery::recoverLocked(uint8_t cause,uint64_t detail)
{
    if(lost())return A830ReturnNotResponding;
    __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
    const uint64_t begin=env_.nowNs(env_.context);
    stats_.lastCause=cause;
    if(cause==A830FaultWatchdog)++stats_.watchdogHangs;
    if(hangClass(cause))adreno830_status_add32(&adreno830_status.hangs,1);
    /* No submission, no new connection; holders drained (a stuck one does not stop the recovery). */
    rops_.gate(rops_.context,A830GateRecovering,cause);
    power_->beginRecovery();
    /* RBBM_INT_0_MASK = 0, then the snapshot (4.13.4). */
    capture(cause,detail,&snapshot_);
    if(snapshot_.gxOn) {wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_MASK),0);io_.barrier(io_.context);}
    if(rops_.snapshot)rops_.snapshot(rops_.context,&snapshot_);
    /* The jobs in flight fail (not replayed); their connections get faults++. */
    stats_.jobsFailed+=rops_.failInflight(rops_.context);
    adreno830_status_add32(&adreno830_status.faults,1);
    adreno830_status_lost(A830SRunning,cause,reasonOf(cause),A830ReturnNotResponding);
    if(!config_.enabled || !allowed(begin)) {
        loseLocked(cause,A830SRunning,cause,A830ReturnNotResponding);
        return A830ReturnNotResponding;
    }
    Progress progress={this,begin,&env_};
    if(power_->cxOn()) {
        /* CX stayed up: stop the GPU's bus traffic, then the GMU (KGSL order: fence, CM3 reset, GBIF, GX, RSCC sleep). */
        adreno830_status_recovering(A830SGmu,0);
        haltGbif(snapshot_.gxOn);
        if(ops_.gmuForceOff(ops_.context)==kIOReturnSuccess)power_->setRsccSlept(true);
    }
    /* S4/S5/S6/S7 again (plus S1/S3/S4 restore when CX was off); the GMU starts at the DCVS target. */
    const uint8_t level=power_->targetLevel(),bus=power_->targetBus();
    uint16_t step=0,subStep=0;
    const IOReturn result=power_->bringUp(level,bus,!power_->cxOn(),&step,&subStep,showProgress,&progress);
    if(result!=kIOReturnSuccess) {
        /* "GPU LOST S<stage of the failing step>.<its own step>": e.g. S6.41 for a CP_ME_INIT that never finished. */
        ++stats_.failures;
        loseLocked(cause,A830PowerStepStage(step),subStep?subStep:step,result);
        return result;
    }
    /* Waiters see the fence at the last submitted seqno (cpStart wrote it); interrupts the storm guard turned off. */
    if(rops_.fenceSignal)rops_.fenceSignal(rops_.context);
    if(rops_.irqRestore)rops_.irqRestore(rops_.context);
    power_->endRecovery(true,level,bus);
    ++stats_.recoveries;
    adreno830_status_add32(&adreno830_status.recoveries,1);
    const uint64_t end=env_.nowNs(env_.context);
    stats_.lastMs=uint32_t((end-begin)/NsPerMs);
    if(stats_.lastMs>stats_.maxMs)stats_.maxMs=stats_.lastMs;
    wdArmed_=false;
    rops_.gate(rops_.context,A830GateOpen,cause);
    adreno830_status_running();
    __atomic_store_n(&running_,false,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

/* ---- software watchdog (4.13.4) ---------------------------------------------------------------------------------- */

bool A830Recovery::watchdogTick(uint64_t now)
{
    if(!initialized_ || lost() || __atomic_load_n(&running_,__ATOMIC_ACQUIRE) || pending())return false;
    const uint32_t fence=ops_.retired(ops_.context);
    if(power_->phase()!=A830PowerActive || fence==ops_.lastSubmitted(ops_.context)) {wdArmed_=false;return false;}
    if(!power_->accessBegin(true))return false;
    const uint32_t rptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_RPTR));
    uint32_t lo=0,hi=0,rem=0;
    if(rops_.readPipe) {
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE,&lo);
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE_HI,&hi);
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,&rem);
    }
    power_->accessEnd(true);
    const uint64_t ib1=uint64_t(hi)<<32|lo;
    /* Any change of the fence, the ring read pointer or the BR IB1 position is progress. */
    if(!wdArmed_ || fence!=wdFence_ || rptr!=wdRptr_ || ib1!=wdIb1_ || rem!=wdIb1Rem_) {
        wdArmed_=true;wdFence_=fence;wdRptr_=rptr;wdIb1_=ib1;wdIb1Rem_=rem;wdLastProgress_=now;
        return false;
    }
    if(now-wdLastProgress_<uint64_t(config_.watchdogLimitMs)*NsPerMs)return false;
    wdArmed_=false;
    report(A830FaultWatchdog,uint64_t(rptr)<<32|fence);
    return true;
}

/* ---- C entry points ------------------------------------------------------------------------------------------------ */

extern "C" void adreno830_recovery_report(uint8_t cause,uint64_t detail)
{
    if(A830Recovery *recovery=__atomic_load_n(&gRecovery,__ATOMIC_ACQUIRE))recovery->report(cause,detail);
}

extern "C" uint8_t adreno830_recovery_gate(void)
{
    A830Recovery *recovery=__atomic_load_n(&gRecovery,__ATOMIC_ACQUIRE);
    if(!recovery)return A830GateOpen;
    if(recovery->lost())return A830GateLost;
    if(recovery->pending() || recovery->busy())return A830GateRecovering;
    return A830GateOpen;
}
