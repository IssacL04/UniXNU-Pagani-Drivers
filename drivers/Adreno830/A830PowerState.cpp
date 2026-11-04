/* SPDX-License-Identifier: MIT */
#include "A830PowerState.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include "Sm8750GpuPower.h"

namespace {
constexpr uint64_t NsPerUs=1000,NsPerMs=1000000,NsPerS=1000000000;
A830PowerState *gPower;

constexpr uint32_t gpuReg(uint32_t dword) {return A830_GPU_REG_BYTE(dword);}
constexpr uint32_t gmuReg(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}
constexpr uint32_t rsccReg(uint32_t dword) {return A830_RSCC_REG_BYTE(dword);}
constexpr uint32_t cxReg(uint32_t dword) {return A830_CX_MISC_REG_BYTE(dword);}
static_assert(gmuReg(A8XX_GMUCX_CM3_SYSRESET)==0x47000 && gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS)==0x57C4C,"GMU window offsets (0.3)");
static_assert(rsccReg(A8XX_RSCC_TCS0_DRV0_STATUS)==0xD28 && cxReg(A8XX_CX_MISC_INT_0_MASK)==0xCC,"RSCC/CX_MISC offsets");

/* Stage of the 8.3 numbering a bring-up step belongs to (for "GPU RECOVER <n> S<s>" and a final "GPU LOST S<s>"). */
uint8_t stageOf(uint16_t step)
{
    switch(step) {
    case A830WakeStepPowerOn: return A830SPower;
    case A830WakeStepSmmu: return A830SSmmu;
    case A830WakeStepTimer: return A830SFirmware;
    case A830WakeStepGmu: return A830SGmu;
    case A830WakeStepCp: return A830SCp;
    case A830WakeStepSecure: return A830SZap;
    case A830WakeStepIrq: return A830SFence;
    default: return A830SRunning;
    }
}
}

uint8_t A830PowerStepStage(uint16_t step) {return stageOf(step);}

extern "C" A830PowerState *adreno830_power_state(void) {return __atomic_load_n(&gPower,__ATOMIC_ACQUIRE);}

IOReturn A830PowerState::init(const A830PowerConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops)
{
    if(!io.read32 || !io.write32 || !io.barrier)return kIOReturnBadArgument;
    if(!env.nowNs || !env.sleepUs || !env.delayUs || !env.lock || !env.unlock)return kIOReturnBadArgument;
    if(!ops.lastSubmitted || !ops.retired)return kIOReturnBadArgument;
    if(config.slumber>A830SlumberCxKeep || !config.idleMs)return kIOReturnBadArgument;
    if(config.screenSlumber>A830SlumberCxKeep || (config.screenSlumber!=A830SlumberOff && !config.screenIdleMs))
        return kIOReturnBadArgument;
    /* Every power cycle (slumber, IFPC switch) needs the whole bring-up; a full slumber also needs the CX restore. */
    const bool cycles=config.slumber!=A830SlumberOff || config.ifpc || config.screenSlumber!=A830SlumberOff;
    if(cycles && (!ops.gmuStart || !ops.oobSet || !ops.oobClear || !ops.hfiSend || !ops.rsccSleep || !ops.cpStart ||
                  !ops.secureResume))
        return kIOReturnBadArgument;
    if((config.slumber==A830SlumberFull || config.screenSlumber==A830SlumberFull) &&
       (!ops.cxPowerOn || !ops.cxPowerOff || !ops.smmuRestore || !ops.cxTimerSeed))
        return kIOReturnBadArgument;
    config_=config;io_=io;env_=env;ops_=ops;stats_={};
    phase_=A830PowerDown;refs_=access_=gxAccess_=0;
    cxOn_=true;cxLost_=cxRestored_=false;rsccSlept_=false;ifpcActive_=false;ifpcWanted_=config.ifpc;touchPending_=false;
    screenOff_=screenSlept_=screenEntering_=false;screenOffAt_=0;
    targetLevel_=votedLevel_=config.defaultLevel;targetBus_=votedBus_=config.defaultBus;
    submitLevel_=submitBus_=touchLevel_=touchBus_=0;
    initialized_=true;
    return kIOReturnSuccess;
}

void A830PowerState::start(uint8_t level,uint8_t bus)
{
    if(!initialized_)return;
    /* S5 always starts the GMU with IFPC off (P0, HFI M5 {9,0,0}); config.ifpc switches at the first idle cycle. */
    ifpcActive_=false;
    targetLevel_=votedLevel_=level;targetBus_=votedBus_=bus;
    __atomic_store_n(&lastActivity_,now(),__ATOMIC_RELEASE);
    idleSince_=0;
    seenSubmitted_=ops_.lastSubmitted(ops_.context);
    __atomic_store_n(&cxOn_,true,__ATOMIC_RELEASE);
    __atomic_store_n(&phase_,uint8_t(A830PowerActive),__ATOMIC_SEQ_CST);
    __atomic_store_n(&gPower,this,__ATOMIC_RELEASE);
}

void A830PowerState::lock() {env_.lock(env_.context);}
void A830PowerState::unlock() {env_.unlock(env_.context);}

void A830PowerState::setFaultSink(void (*sink)(void *,uint8_t,uint64_t),void *context) {sink_=sink;sinkContext_=context;}

/* ---- holders ---------------------------------------------------------------------------------------------------- */

IOReturn A830PowerState::acquire()
{
    /* Fast path: increment, then look at the phase (a transition stores its phase, then looks at the count). */
    __atomic_fetch_add(&refs_,1u,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&phase_,__ATOMIC_SEQ_CST)==A830PowerActive)return kIOReturnSuccess;
    __atomic_fetch_sub(&refs_,1u,__ATOMIC_SEQ_CST);
    lock();
    IOReturn result=kIOReturnSuccess;
    switch(phase()) {
    case A830PowerActive: break;
    case A830PowerSlumber: result=wakeLocked(A830WakeSubmit);break;
    default: result=A830ReturnNotResponding;break;   /* down, recovering, lost (transitions hold the mutex) */
    }
    if(result==kIOReturnSuccess)__atomic_fetch_add(&refs_,1u,__ATOMIC_SEQ_CST);
    unlock();
    return result;
}

void A830PowerState::release()
{
    __atomic_store_n(&lastActivity_,now(),__ATOMIC_RELEASE);
    if(__atomic_load_n(&refs_,__ATOMIC_SEQ_CST))__atomic_fetch_sub(&refs_,1u,__ATOMIC_SEQ_CST);
}

bool A830PowerState::accessBegin(bool gx)
{
    __atomic_fetch_add(&access_,1u,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&phase_,__ATOMIC_SEQ_CST)!=A830PowerActive) {
        __atomic_fetch_sub(&access_,1u,__ATOMIC_SEQ_CST);
        return false;
    }
    /* P2: GX may be collapsed between submissions; OOB_GPU (reference counted in A830Gmu) keeps it up. */
    if(gx && ifpcActive()) {
        if(ops_.oobSet(ops_.context)!=kIOReturnSuccess) {
            __atomic_fetch_sub(&access_,1u,__ATOMIC_SEQ_CST);
            return false;
        }
        __atomic_fetch_add(&gxAccess_,1u,__ATOMIC_SEQ_CST);
    }
    return true;
}

void A830PowerState::accessEnd(bool gx)
{
    if(gx && __atomic_load_n(&gxAccess_,__ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&gxAccess_,1u,__ATOMIC_SEQ_CST);
        ops_.oobClear(ops_.context);
    }
    if(__atomic_load_n(&access_,__ATOMIC_SEQ_CST))__atomic_fetch_sub(&access_,1u,__ATOMIC_SEQ_CST);
}

bool A830PowerState::drain(uint32_t us)
{
    /* The holders finish without the mutex (release/accessEnd are lock-free); give them up to `us`. */
    for(uint32_t waited=0;;waited+=50) {
        if(!__atomic_load_n(&refs_,__ATOMIC_SEQ_CST) && !__atomic_load_n(&access_,__ATOMIC_SEQ_CST))return true;
        if(waited>=us)return false;
        env_.sleepUs(env_.context,50);
    }
}

/* ---- service thread --------------------------------------------------------------------------------------------- */

void A830PowerState::noteTouch()
{
    /* The screen rule: a touch on the dark display (a pocket, a sleeve) is no activity and no reason to wake the GPU. */
    if(config_.screenSlumber!=A830SlumberOff && screenOff())return;
    __atomic_store_n(&lastActivity_,now(),__ATOMIC_RELEASE);
    touchPending_=true;
}

void A830PowerState::setScreenOff(bool off,uint64_t now)
{
    if(config_.screenSlumber==A830SlumberOff || off==screenOff())return;
    if(off) {screenOffAt_=now;touchPending_=false;}
    __atomic_store_n(&screenOff_,off,__ATOMIC_RELEASE);
}

uint64_t A830PowerState::tick(uint64_t now)
{
    const uint64_t idleNs=uint64_t(config_.idleMs)*NsPerMs;
    switch(phase()) {
    case A830PowerSlumber:
        /* The display came back while the GPU slept through it being off: wake now, before its first frame waits for it. */
        if(screenSlept() && !screenOff()) {
            touchPending_=false;
            wake(A830WakeScreen);
            return now+idleNs;
        }
        if(touchPending_ && config_.touchPrewake) {
            touchPending_=false;
            wake(A830WakeTouch);
            return now+idleNs;
        }
        touchPending_=false;
        if(now>=nextSlumberRow_) {publishSlumber(now);nextSlumberRow_=now+NsPerS;}
        return nextSlumberRow_;
    case A830PowerActive: break;
    default: touchPending_=false;return now+NsPerS;
    }
    touchPending_=false;
    /* A seqno written since the last look is activity even if the writer did not stamp it (release()). */
    const uint32_t submitted=ops_.lastSubmitted(ops_.context);
    if(submitted!=seenSubmitted_) {seenSubmitted_=submitted;__atomic_store_n(&lastActivity_,now,__ATOMIC_RELEASE);}
    const bool quiet=!holders() && ops_.retired(ops_.context)==submitted;
    if(!quiet) {idleSince_=0;return now+idleNs;}
    if(!idleSince_)idleSince_=now;
    const uint64_t last=lastActivityNs();
    const uint64_t since=last>idleSince_?last:idleSince_;
    /* P2 switch: the GMU must restart for FEATURE_CTRL{9,...}; done in the first quiet moment (CX-keeping cycle). */
    if(ifpcWanted_!=ifpcActive() && ops_.gmuStart) {
        lock();
        if(phase()==A830PowerActive && enterLocked(A830SlumberCxKeep,now)==kIOReturnSuccess) {
            ++stats_.ifpcCycles;
            wakeLocked(A830WakeIfpc);
        }
        unlock();
        return now+idleNs;
    }
    /* The screen rule (config.screenSlumber) where P1 is not configured: its mode once the display has been off and the GPU
     * quiet for screenIdleMs, counted from the later of the two. */
    uint8_t mode=config_.slumber;
    uint64_t from=since,wait=idleNs;
    const bool screen=mode==A830SlumberOff && config_.screenSlumber!=A830SlumberOff && screenOff() && !stats_.screenBlocked;
    if(screen) {
        mode=config_.screenSlumber;
        wait=uint64_t(config_.screenIdleMs)*NsPerMs;
        if(screenOffAt_>from)from=screenOffAt_;
    }
    if(mode==A830SlumberOff)return now+NsPerS;
    /* `now` is the service thread's reading from before this tick: a holder that released (or a wake that ended) since then
     * stamped its activity later. The quiet time counts from that stamp; an unsigned now - from would wrap and slumber at once. */
    if(from>now || now-from<wait)return from+wait;
    screenEntering_=screen;
    if(enterSlumber(mode)==kIOReturnSuccess && screen)++stats_.screenEntries;
    screenEntering_=false;
    return now+idleNs;
}

IOReturn A830PowerState::wake(A830WakeReason reason)
{
    lock();
    const IOReturn result=wakeLocked(reason);
    unlock();
    return result;
}

IOReturn A830PowerState::enterSlumber(uint8_t mode)
{
    lock();
    const IOReturn result=enterLocked(mode,now());
    unlock();
    return result;
}

void A830PowerState::setIfpc(bool enable) {ifpcWanted_=enable;}

void A830PowerState::setTarget(uint8_t level,uint8_t bus)
{
    if(!level)return;   /* GMU level 0 is off (KGSL gen8_gmu_dcvs_set refuses it) */
    __atomic_store_n(&targetLevel_,level,__ATOMIC_RELAXED);
    __atomic_store_n(&targetBus_,bus,__ATOMIC_RELAXED);
}

void A830PowerState::setWakeTargets(uint8_t submitLevel,uint8_t submitBus,uint8_t touchLevel,uint8_t touchBus)
{
    submitLevel_=submitLevel;submitBus_=submitBus;touchLevel_=touchLevel;touchBus_=touchBus;
}

void A830PowerState::noteVoted(uint8_t level,uint8_t bus)
{
    __atomic_store_n(&votedLevel_,level,__ATOMIC_RELAXED);
    __atomic_store_n(&votedBus_,bus,__ATOMIC_RELAXED);
}

/* ---- slumber (4.17 P1: S1..S10) ---------------------------------------------------------------------------------- */

bool A830PowerState::poll(uint8_t window,uint32_t offset,uint32_t mask,uint32_t value,uint32_t timeoutUs,uint32_t stepUs)
{
    for(uint32_t waited=0;;waited+=stepUs) {
        if((rd(window,offset)&mask)==value)return true;
        if(waited>=timeoutUs)return false;
        env_.sleepUs(env_.context,stepUs);
    }
}

void A830PowerState::fail(uint16_t step,IOReturn result,uint8_t cause)
{
    ++stats_.failures;stats_.lastStep=step;stats_.lastError=result;
    /* A slumber the screen rule began, or the wake from one taken while the display was off: the rule ends for the boot. */
    if(screenEntering_ || screenSlept())stats_.screenBlocked=true;
    /* The GPU is in an unknown state: nobody may use it until the recovery has run (or for good without one). */
    __atomic_store_n(&phase_,uint8_t(sink_?A830PowerRecovering:A830PowerLost),__ATOMIC_SEQ_CST);
    if(sink_)sink_(sinkContext_,cause,uint64_t(step)<<32|uint32_t(result));
}

IOReturn A830PowerState::enterLocked(uint8_t mode,uint64_t now)
{
    if(phase()!=A830PowerActive)return kIOReturnNotReady;
    if(mode!=A830SlumberFull && mode!=A830SlumberCxKeep)return kIOReturnBadArgument;
    /* S1: no holder, every submitted seqno retired, no short reader left. */
    __atomic_store_n(&phase_,uint8_t(A830PowerEntering),__ATOMIC_SEQ_CST);
    if(holders() || ops_.retired(ops_.context)!=ops_.lastSubmitted(ops_.context) || !drain(config_.drainUs)) {
        ++stats_.aborts;
        __atomic_store_n(&phase_,uint8_t(A830PowerActive),__ATOMIC_SEQ_CST);
        return kIOReturnBusy;
    }
    const uint64_t begin=env_.nowNs(env_.context);
    /* S2: GX up under OOB_GPU; give up if an enabled interrupt is pending or the GPU is still busy. */
    IOReturn result=ops_.oobSet(ops_.context);
    if(result!=kIOReturnSuccess) {fail(A830SlumberStepOob,result,A830FaultSlumber);return result;}
    const uint32_t pending=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_STATUS))&A8XX_INT_MASK;
    const uint32_t busy=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_STATUS))&A8XX_RBBM_STATUS_BUSY_MASK;
    if(pending || busy) {
        ops_.oobClear(ops_.context);
        ++stats_.aborts;
        __atomic_store_n(&phase_,uint8_t(A830PowerActive),__ATOMIC_SEQ_CST);
        return kIOReturnBusy;
    }
    wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_MASK),0);
    io_.barrier(io_.context);
    if(ops_.irqEnable)ops_.irqEnable(ops_.context,A830PowerIrqGpu,false);
    ops_.oobClear(ops_.context);
    /* The annibale profile holds the OOB_GPU of the last bring-up while the GPU is up (secureResume leaves it set,
     * kernel-sequences section 14 row 28); the GMU cannot idle under it, so slumber drops that reference too. The next
     * gmuStart sets it again. */
    if(adreno830_annibale())ops_.oobClear(ops_.context);
    /* S3: the GMU reached the requested idle level (ACTIVE with IFPC off, IFPC with GX clocks off in P2). */
    const bool ifpc=ifpcActive();
    const uint32_t idleLevel=ifpc?A830_GMU_RPMH_POWER_STATE_IFPC:A830_GMU_RPMH_POWER_STATE_ACTIVE;
    bool reached=false;
    for(uint32_t waited=0;;waited+=A830_POLL_STEP_US) {
        const uint32_t state=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_RPMH_POWER_STATE));
        const uint32_t clocks=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS));
        if(state==idleLevel && !(ifpc && !(clocks&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF))) {reached=true;break;}
        if(waited>=A830_T_LOWEST_IDLE_US)break;
        env_.sleepUs(env_.context,A830_POLL_STEP_US);
    }
    if(!reached) {fail(A830SlumberStepLowestIdle,kIOReturnTimeout,A830FaultSlumber);return kIOReturnTimeout;}
    /* S4: no RPMh vote of the GPU RSC outstanding (TCS0..9 idle, 2 ms each). */
    for(uint32_t tcs=0;tcs<A8XX_RSCC_TCS_COUNT;++tcs)
        if(!poll(A830PowerWinRscc,rsccReg(A8XX_RSCC_TCS_DRV0_STATUS(tcs)),A8XX_RSCC_TCS_IDLE,A8XX_RSCC_TCS_IDLE,
                 A830_T_RSCC_TCS_SLUMBER_US,10)) {
            fail(A830SlumberStepRpmh,kIOReturnTimeout,A830FaultSlumber);
            return kIOReturnTimeout;
        }
    /* S5: stop the busy counter, PREPARE_SLUMBER at the default level, fence to ALLOW, keep GEMNOC out of collapse. */
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_ENABLE),0);
    const uint32_t prepare[2]={uint32_t(config_.defaultBus)|A830_HFI_BW_NO_AB,config_.defaultLevel};
    result=ops_.hfiSend(ops_.context,A830_HFI_H2F_PREPARE_SLUMBER,prepare,2);
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_CTRL),A830_AHB_FENCE_CTRL_ALLOW);
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUCX_CX_FALNEXT_INTF),A830_GMU_CX_FAL_VETO);
    io_.barrier(io_.context);
    if(result!=kIOReturnSuccess) {fail(A830SlumberStepPrepare,result,A830FaultSlumber);return result;}
    /* S6: the GMU went idle (CXGXCPUBUSYIGNAHB, KGSL gen8 bit 30). */
    if(!poll(A830PowerWinGmu,gmuReg(A8XX_GMUAO_GPU_CX_BUSY_STATUS),A830_GMU_CX_BUSY_KGSL,0,A830_T_CX_IDLE_US,A830_POLL_STEP_US)) {
        fail(A830SlumberStepCxIdle,kIOReturnTimeout,A830FaultSlumber);
        return kIOReturnTimeout;
    }
    /* S7: RSCC sleep (CM3 reset, GENERAL_9 saved, sleep request, bit 16 of RSC_STATUS0_DRV0). */
    result=ops_.rsccSleep(ops_.context);
    if(result!=kIOReturnSuccess) {fail(A830SlumberStepRscc,result,A830FaultSlumber);return result;}
    rsccSlept_=true;
    /* S8: GBIF halt, client then arbiter, then released (KGSL gen8_halt_gbif). */
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_CLIENT);
    io_.barrier(io_.context);
    const bool client=poll(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT_ACK),A8XX_GBIF_HALT_CLIENT,A8XX_GBIF_HALT_CLIENT,
                           A830_T_GBIF_HALT_US,10);
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_ARB);
    io_.barrier(io_.context);
    const bool arbiter=poll(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT_ACK),A8XX_GBIF_HALT_ARB,A8XX_GBIF_HALT_ARB,
                            A830_T_GBIF_HALT_US,10);
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),0);
    io_.barrier(io_.context);
    if(!client || !arbiter) {fail(A830SlumberStepGbif,kIOReturnTimeout,A830FaultSlumber);return kIOReturnTimeout;}
    /* S9: GMU and CX_MISC interrupts masked and cleared; the GIC lines of the domain that goes down disabled. */
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffffu);
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffffu);
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffffu);
    wr(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffffu);
    wr(A830PowerWinCxMisc,cxReg(A8XX_CX_MISC_INT_0_MASK),0xffffffffu);
    wr(A830PowerWinCxMisc,cxReg(A8XX_CX_MISC_INT_CLEAR_CMD),0xffffffffu);
    io_.barrier(io_.context);
    if(ops_.irqEnable)
        ops_.irqEnable(ops_.context,mode==A830SlumberFull?A830PowerIrqCxDomain:A830PowerIrqGpuDomain,false);
    /* S10 (full slumber): the CX power-down of 4.2.3; the host votes stay. CX may stay up if the GMU still votes it. */
    bool lostCx=false;
    if(mode==A830SlumberFull) {
        uint32_t warnings=0;
        result=ops_.cxPowerOff(ops_.context,&warnings);
        stats_.warnings=warnings;
        if(result!=kIOReturnSuccess) {fail(A830SlumberStepPowerOff,result,A830FaultSlumber);return result;}
        lostCx=!(warnings&Sm8750GpuOffCxStillOn);
        __atomic_store_n(&cxOn_,false,__ATOMIC_RELEASE);
    }
    if(lostCx) {cxLost_=true;cxRestored_=false;}
    slumberMode_=mode;
    sleptAt_=now;nextSlumberRow_=0;
    ++stats_.entries;
    /* Asleep with the display off (screen rule on): the display coming back wakes the GPU (tick). */
    __atomic_store_n(&screenSlept_,config_.screenSlumber!=A830SlumberOff && screenOff(),__ATOMIC_RELEASE);
    stats_.lastEntryUs=uint32_t((env_.nowNs(env_.context)-begin)/NsPerUs);
    __atomic_store_n(&phase_,uint8_t(A830PowerSlumber),__ATOMIC_SEQ_CST);
    publishSlumber(now);
    return kIOReturnSuccess;
}

/* ---- wake ---------------------------------------------------------------------------------------------------------- */

IOReturn A830PowerState::bringUp(uint8_t level,uint8_t bus,bool powerOn,uint16_t *step,uint16_t *subStep,
                                 void (*progress)(void *,uint16_t),void *progressContext)
{
    uint16_t local=0,sub=0;
    if(!step)step=&local;
    if(!subStep)subStep=&sub;
    *subStep=0;
    auto at=[&](uint16_t value) {*step=value;if(progress)progress(progressContext,value);};
    IOReturn result=kIOReturnSuccess;
    if(powerOn) {
        at(A830WakeStepPowerOn);
        if(!ops_.cxPowerOn)return kIOReturnUnsupported;
        result=ops_.cxPowerOn(ops_.context);
        if(result!=kIOReturnSuccess)return result;
        __atomic_store_n(&cxOn_,true,__ATOMIC_RELEASE);
        if(config_.ahbTimeout)armAhbTimeout();
    }
    if(cxLost_ && !cxRestored_) {
        /* CX collapsed: the SMMU contexts and the CX timer are gone (4.4.2: re-seed only before the GMU reload, with
         * the GMU in reset). Done once per collapse: a retry of the bring-up does not repeat them. */
        at(A830WakeStepSmmu);
        if(!ops_.smmuRestore || !ops_.cxTimerSeed)return kIOReturnUnsupported;
        result=ops_.smmuRestore(ops_.context);
        if(result!=kIOReturnSuccess)return result;
        at(A830WakeStepTimer);
        ops_.cxTimerSeed(ops_.context);
        cxRestored_=true;
    }
    at(A830WakeStepGmu);
    if(!ops_.gmuStart || !ops_.cpStart || !ops_.secureResume)return kIOReturnUnsupported;
    /* cxLost stays set until a GMU start succeeds: every attempt after a collapse reloads the whole firmware. */
    const A830GmuStartArgs args={level,bus,ifpcWanted_,rsccSlept_,cxLost_};
    result=ops_.gmuStart(ops_.context,&args,subStep);
    if(result!=kIOReturnSuccess)return result;
    rsccSlept_=false;cxLost_=false;cxRestored_=false;
    __atomic_store_n(&ifpcActive_,ifpcWanted_,__ATOMIC_RELEASE);
    at(A830WakeStepCp);
    result=ops_.cpStart(ops_.context,ops_.lastSubmitted(ops_.context),subStep);
    if(result!=kIOReturnSuccess)return result;
    at(A830WakeStepSecure);
    result=ops_.secureResume(ops_.context,subStep);
    if(result!=kIOReturnSuccess)return result;
    at(A830WakeStepIrq);
    if(ops_.irqEnable)ops_.irqEnable(ops_.context,A830PowerIrqCxDomain,true);
    noteVoted(level,bus);
    return kIOReturnSuccess;
}

void A830PowerState::armAhbTimeout()
{
    static constexpr uint32_t controls[]={A8XX_CX_MISC_CX_AHB_AON_CNTL,A8XX_CX_MISC_CX_AHB_GMU_CNTL,A8XX_CX_MISC_CX_AHB_CP_CNTL,
                                          A8XX_CX_MISC_CX_AHB_VBIF_SMMU_CNTL,A8XX_CX_MISC_CX_AHB_HOST_CNTL};
    for(const uint32_t control:controls)wr(A830PowerWinCxMisc,cxReg(control),A830_CX_AHB_CNTL_VALUE);
    io_.barrier(io_.context);
}

IOReturn A830PowerState::wakeLocked(A830WakeReason reason)
{
    const A830PowerPhase current=phase();
    if(current==A830PowerActive)return kIOReturnSuccess;
    if(current!=A830PowerSlumber)return A830ReturnNotResponding;
    __atomic_store_n(&phase_,uint8_t(A830PowerWaking),__ATOMIC_SEQ_CST);
    const uint64_t begin=env_.nowNs(env_.context);
    uint8_t level=targetLevel(),bus=targetBus();
    if(reason==A830WakeSubmit && submitLevel_>level) {level=submitLevel_;bus=submitBus_;}
    /* A touch, or the display coming back (its first frames follow), starts the GMU at the touch boost level. */
    if((reason==A830WakeTouch || reason==A830WakeScreen) && touchLevel_>level) {level=touchLevel_;bus=touchBus_;}
    uint16_t step=0,sub=0;
    const IOReturn result=bringUp(level,bus,slumberMode_==A830SlumberFull,&step,&sub,nullptr,nullptr);
    if(result!=kIOReturnSuccess) {
        stats_.lastSubStep=sub;
        fail(step,result,A830FaultWake);
        return A830ReturnNotResponding;
    }
    const uint64_t end=env_.nowNs(env_.context);
    ++stats_.wakes;
    if(reason==A830WakeTouch)++stats_.touchWakes;
    if(reason==A830WakeScreen)++stats_.screenWakes;
    __atomic_store_n(&screenSlept_,false,__ATOMIC_RELEASE);
    stats_.lastWakeUs=uint32_t((end-begin)/NsPerUs);
    if(stats_.lastWakeUs>stats_.maxWakeUs)stats_.maxWakeUs=stats_.lastWakeUs;
    stats_.sleptNs+=begin>sleptAt_?begin-sleptAt_:0;
    __atomic_store_n(&lastActivity_,end,__ATOMIC_RELEASE);
    idleSince_=0;
    __atomic_store_n(&phase_,uint8_t(A830PowerActive),__ATOMIC_SEQ_CST);
    adreno830_status_running();
    return kIOReturnSuccess;
}

void A830PowerState::publishSlumber(uint64_t now)
{
    const uint64_t asleep=now>sleptAt_?now-sleptAt_:0;
    adreno830_status_slumber(uint32_t(asleep/NsPerS),stats_.wakes,(stats_.lastWakeUs+999)/1000);
}

/* ---- recovery ------------------------------------------------------------------------------------------------------ */

IOReturn A830PowerState::beginRecovery()
{
    __atomic_store_n(&phase_,uint8_t(A830PowerRecovering),__ATOMIC_SEQ_CST);
    /* A holder that does not leave within drainUs is stuck behind the hang itself; the recovery goes on. */
    return drain(config_.drainUs)?kIOReturnSuccess:kIOReturnTimeout;
}

void A830PowerState::endRecovery(bool ok,uint8_t level,uint8_t bus)
{
    __atomic_store_n(&screenSlept_,false,__ATOMIC_RELEASE);   /* recovered or lost, the GPU no longer sleeps */
    if(!ok) {markLost();return;}
    noteVoted(level,bus);
    __atomic_store_n(&lastActivity_,now(),__ATOMIC_RELEASE);
    idleSince_=0;
    __atomic_store_n(&cxOn_,true,__ATOMIC_RELEASE);
    __atomic_store_n(&phase_,uint8_t(A830PowerActive),__ATOMIC_SEQ_CST);
}

void A830PowerState::markLost() {__atomic_store_n(&phase_,uint8_t(A830PowerLost),__ATOMIC_SEQ_CST);}

/* ---- kernel services ---------------------------------------------------------------------------------------------- */

#ifndef ADRENO830_HOST_TEST
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>

namespace {
struct KernelPowerEnv {
    IOLock *lock;
    uint32_t woken;            /* a wake arrived since the last wait */
    uint32_t channel;          /* the wait event */
    void (*body)(void *);
    void *argument;
};
KernelPowerEnv gKernelEnv;
A830PowerEnv gKernelPowerEnv;

uint64_t kernelNow(void *)
{
    uint64_t nanoseconds=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&nanoseconds);
    return nanoseconds;
}
void kernelSleep(void *,uint32_t us)
{
    if(us>=1000)IOSleep(us/1000);
    if(us%1000)IODelay(us%1000);
}
void kernelDelay(void *,uint32_t us) {IODelay(us);}
void kernelLock(void *context) {IOLockLock(static_cast<KernelPowerEnv *>(context)->lock);}
void kernelUnlock(void *context) {IOLockUnlock(static_cast<KernelPowerEnv *>(context)->lock);}
void kernelWait(void *context,uint64_t deadlineNs)
{
    auto *env=static_cast<KernelPowerEnv *>(context);
    if(__atomic_exchange_n(&env->woken,0u,__ATOMIC_ACQ_REL))return;
    uint64_t deadline=0;
    nanoseconds_to_absolutetime(deadlineNs,&deadline);
    assert_wait_deadline(reinterpret_cast<event_t>(&env->channel),THREAD_UNINT,deadline);
    /* A wake between the check above and assert_wait set the flag: do not block on it. */
    if(__atomic_exchange_n(&env->woken,0u,__ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(),THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&env->woken,0u,__ATOMIC_RELEASE);
}
void kernelWake(void *context)
{
    auto *env=static_cast<KernelPowerEnv *>(context);
    __atomic_store_n(&env->woken,1u,__ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(&env->channel));
}
void kernelThreadMain(void *parameter,wait_result_t)
{
    auto *env=static_cast<KernelPowerEnv *>(parameter);
    env->body(env->argument);
}
IOReturn kernelStartThread(void *context,void (*body)(void *),void *argument)
{
    auto *env=static_cast<KernelPowerEnv *>(context);
    if(env->body)return kIOReturnBusy;   /* one service thread per boot */
    env->body=body;env->argument=argument;
    thread_t thread=nullptr;
    if(kernel_thread_start(kernelThreadMain,env,&thread)!=KERN_SUCCESS) {env->body=nullptr;return kIOReturnNoResources;}
    thread_deallocate(thread);
    return kIOReturnSuccess;
}
}

extern "C" const A830PowerEnv *a830_kernel_power_env(void)
{
    if(!gKernelEnv.lock) {
        IOLock *lock=IOLockAlloc();
        if(!lock)return nullptr;
        gKernelEnv.lock=lock;
        gKernelPowerEnv={&gKernelEnv,kernelNow,kernelSleep,kernelDelay,kernelLock,kernelUnlock,kernelWait,kernelWake,
                         kernelStartThread};
    }
    return &gKernelPowerEnv;
}
#endif
