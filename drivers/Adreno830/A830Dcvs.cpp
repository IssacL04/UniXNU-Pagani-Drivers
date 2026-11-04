/* SPDX-License-Identifier: MIT */
#include "A830Dcvs.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include "Adreno830Status.h"

extern "C" {
/* PaganiInput's touch frame counter. A weak zero definition here keeps the archive linkable without the input driver
 * (the statically linked kernel refuses undefined weak references); PaganiInput's own weak definition coalesces with it
 * into one word. */
__attribute__((weak)) volatile uint32_t pagani_input_touch_events;
/* The display's power as PaganiPanel publishes it from the display's power callback (bit 0: off; the count of changes above
 * it), read by the screen rule of A830PowerState (pagani-screenoff bit 1) only. Weak and zero (on) without the panel driver. */
__attribute__((weak)) volatile uint32_t pagani_screen_state;
}

namespace {
constexpr uint64_t NsPerMs=1000000,NsPerS=1000000000;
A830Dcvs *gDcvs;

constexpr uint32_t gmuReg(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}
/* 19.2 MHz XO ticks to ns: 1e9 / 19.2e6 = 625 / 12. */
constexpr uint64_t ticksToNs(uint64_t ticks) {return ticks*625/12;}
static_assert(ticksToNs(A830_XO_HZ)==NsPerS,"XO tick length");

uint64_t ms(uint32_t value) {return uint64_t(value)*NsPerMs;}
uint8_t max8(uint8_t a,uint8_t b) {return a>b?a:b;}
uint8_t min8(uint8_t a,uint8_t b) {return a<b?a:b;}

void threadBody(void *argument) {static_cast<A830Dcvs *>(argument)->run();}
}

/* ---- set-up ------------------------------------------------------------------------------------------------------ */

IOReturn A830Dcvs::init(const A830DcvsConfig &config,const A830DcvsTable &table,const A830PowerIo &io,const A830PowerEnv &env,
                        const A830DcvsOps &ops,A830PowerState *power,A830Recovery *recovery,const Sm8750TsensIo *tsens)
{
    if(!power || !io.read32 || !env.nowNs || !env.wake || !env.lock || !env.unlock || !ops.perfVote)return kIOReturnBadArgument;
    if(!table.levels || !table.count || table.count>Adreno830LevelMax || !table.ddrKbps || !table.ddrLevels)
        return kIOReturnBadArgument;
    for(unsigned i=0;i<table.count;++i) {
        const Adreno830Level &level=table.levels[i];
        if(!level.khz || (i && level.khz<table.levels[i-1].khz) || level.bus>=table.ddrLevels ||
           level.bus_max>=table.ddrLevels || level.bus>level.bus_max)
            return kIOReturnBadArgument;
    }
    if(!config.windowMs || !config.targetBusyPermille || config.initialLevel<1 || config.initialLevel>table.count ||
       config.initialBus>=table.ddrLevels || config.underrunFloorLevel>=table.ddrLevels)
        return kIOReturnBadArgument;
    config_=config;table_=table;io_=io;env_=env;ops_=ops;power_=power;recovery_=recovery;tsens_=tsens;
    stats_={};
    annibale_=adreno830_annibale();hot_=false;stopped_=false;
    stats_.annibale=annibale_;
    Sm8750ThermalInit(&thermal_);
    level_=config.initialLevel;bus_=config.initialBus;
    floor_=levelAtLeast(config.minMhz*1000u);
    sustainLevel_=config.sustainKhz?levelAtMost(config.sustainKhz):table.count;
    setAcd(config.acd);
    /* Until a sensor reads valid the zone is Unknown: start under its cap; the first reading decides (design: the top at once
     * when it is at least 5 C below the target). The annibale rule starts cool, as the K90 port does; the first window's
     * reading decides (no usable reading: 160 MHz at once). */
    thermalFloor_=levelAtLeast(config.thermalFloorKhz);
    emergencyLevel_=levelAtMost(config.thermalEmergencyKhz);
    unreadableLevel_=levelAtMost(Sm8750ThermalUnreadableCapKhz);
    thermalCeiling_=annibale_?table_.count:unreadableLevel_;
    filtered16_=trendRef16_=0;filterValid_=emergency_=false;trend_=0;trendAt_=coolSince_=recoverAt_=0;
    __atomic_store_n(&published_,0u,__ATOMIC_RELEASE);
    policy_=level_;
    haveSample_=false;down_=saturated_=0;raisedLast_=false;idle_=false;
    wakeUntil_=touchUntil_=lateUntil_=retryAt_=0;lateLevel_=0;lastBusyPermille_=0;
    submits_=seenSubmits_=vsyncs_=windowVsyncs_=late_=seenLate_=underrunTotal_=seenUnderruns_=0;
    lastSubmitNs_=lastVsyncNs_=lastWindow_=0;touchPrimed_=false;
    seenSeqno_=power->submittedSeqno();hookedSubmits_=0;
    nextStatus_=nextWatchdog_=0;residencyStart_=statusStart_=lastAccount_=0;statusBusyNs_=statusNs_=0;
    for(auto &ns:residencyNs_)ns=0;
    underrunStreak_=0;underrunAt_=0;busMax_=floorRaised_=false;
    power_->setTarget(level_,bus_);
    updateWakeTargets();
    initialized_=true;started_=false;
    return kIOReturnSuccess;
}

void A830Dcvs::setAcd(bool running)
{
    config_.acd=running;
    /* 4.16: 734 MHz while ACD is off, whatever pagani-gpu-dcvs-max says; with ACD on the knob, or the table top. */
    uint32_t maxKhz=config_.maxGiven || !running?config_.maxMhz*1000u:table_.levels[table_.count-1].khz;
    if(config_.maxLevel) {
        /* pagani-gpu-maxlevel=n: GMU level n (beyond the table: its top) replaces the default maximum and the annibale
         * 342 MHz; an explicit pagani-gpu-dcvs-max still lowers it, the ACD-off ceiling below still applies. */
        const uint32_t levelKhz=table_.levels[min8(config_.maxLevel,table_.count)-1].khz;
        maxKhz=config_.maxGiven && config_.maxMhz*1000u<levelKhz?config_.maxMhz*1000u:levelKhz;
    }
    if(!running && maxKhz>config_.acdOffMaxKhz)maxKhz=config_.acdOffMaxKhz;
    /* annibale: 342 MHz in the first round; only an explicit pagani-gpu-dcvs-max or pagani-gpu-maxlevel lifts it (section 14
     * row 38). */
    if(annibale_ && !config_.maxGiven && !config_.maxLevel && maxKhz>config_.annibaleMaxKhz)maxKhz=config_.annibaleMaxKhz;
    knobCeiling_=levelAtMost(maxKhz);
    if(floor_>knobCeiling_)floor_=knobCeiling_;
}

IOReturn A830Dcvs::start()
{
    if(!initialized_)return kIOReturnNotReady;
    if(started_)return kIOReturnBusy;
    const uint64_t now=env_.nowNs(env_.context);
    lastWindow_=lastAccount_=residencyStart_=statusStart_=now;
    nextStatus_=now+ms(config_.statusMs);
    nextWatchdog_=now+(recovery_?ms(recovery_->config().watchdogPeriodMs):0);
    /* pagani-gpu-dcvs=0: one vote of pagani-gpu-perf now (S10, M7); the thread keeps it under the ceilings. */
    if(!config_.governor)vote(now,fixedLevel(),busOf(fixedLevel()),0);
    __atomic_store_n(&gDcvs,this,__ATOMIC_RELEASE);
    started_=true;
    if(!env_.startThread)return kIOReturnSuccess;   /* host tests drive serviceTick() themselves */
    const IOReturn result=env_.startThread(env_.context,threadBody,this);
    if(result!=kIOReturnSuccess)started_=false;
    return result;
}

void A830Dcvs::run()
{
    for(;;) {
        const uint64_t now=env_.nowNs(env_.context);
        env_.wait(env_.context,serviceTick(now));
    }
}

/* ---- level arithmetic ---------------------------------------------------------------------------------------------- */

uint8_t A830Dcvs::levelAtLeast(uint32_t khz) const
{
    for(uint8_t level=1;level<=table_.count;++level)
        if(table_.levels[level-1].khz>=khz)return level;
    return table_.count;
}

uint8_t A830Dcvs::levelAtMost(uint32_t khz) const
{
    uint8_t found=1;
    for(uint8_t level=1;level<=table_.count;++level)
        if(table_.levels[level-1].khz<=khz)found=level;
    return found;
}

uint8_t A830Dcvs::busOf(uint8_t level) const
{
    if(level<1 || level>table_.count)return 0;
    const Adreno830Level &entry=table_.levels[level-1];
    return busMax_?entry.bus_max:entry.bus;
}

uint8_t A830Dcvs::ceiling() const
{
    const uint8_t ceiling=min8(knobCeiling_,thermalCeiling_);
    return ceiling?ceiling:1;
}

uint8_t A830Dcvs::fixedLevel() const
{
    uint8_t level=config_.fixedLevel;
    if(level<1)level=1;
    if(level>table_.count)level=table_.count;
    return min8(level,ceiling());
}

uint32_t A830Dcvs::abQuantize(uint32_t abKbps,uint32_t topKbps)
{
    /* KGSL adreno_gmu_bus_ab_quantize: AB in 1/65536 of the DDR maximum (top level x 4 channels), at least 1 when
     * non-zero, at most 0xFFFE; [15:8] = 1 marks it valid. The caller ORs the IB bus level into [7:0]. */
    if(!topKbps)return A830_HFI_BW_NO_AB;
    const uint64_t maximum=uint64_t(topKbps)*A830_DDR_CHANNELS;
    uint64_t vote=uint64_t(abKbps)*65536u/maximum;
    if(vote>0xfffe)vote=0xfffe;
    if(abKbps && !vote)vote=1;
    return uint32_t(vote)<<16|1u<<8;
}

void A830Dcvs::updateWakeTargets()
{
    const uint8_t top=ceiling();
    const uint8_t submit=min8(max8(levelAtLeast(config_.wakeBoostKhz),floor_),top);
    const uint8_t touch=min8(max8(levelAtLeast(config_.touchBoostKhz),floor_),top);
    power_->setWakeTargets(submit,busOf(submit),touch,busOf(touch));
}

/* ---- hooks -------------------------------------------------------------------------------------------------------- */

void A830Dcvs::noteSubmit()
{
    __atomic_fetch_add(&submits_,1u,__ATOMIC_RELAXED);
    __atomic_store_n(&lastSubmitNs_,env_.nowNs(env_.context),__ATOMIC_RELEASE);
    /* First work after idle: let the thread apply the wake boost now instead of at the next window. */
    if(__atomic_load_n(&idle_,__ATOMIC_ACQUIRE))env_.wake(env_.context);
}

void A830Dcvs::noteVsync()
{
    __atomic_store_n(&lastVsyncNs_,env_.nowNs(env_.context),__ATOMIC_RELEASE);
    const uint32_t count=__atomic_add_fetch(&vsyncs_,1u,__ATOMIC_ACQ_REL);
    /* A window every two TEs, while the GPU is up (asleep, the thread keeps its own touch cadence). */
    if(!(count&1u) && power_->phase()==A830PowerActive)env_.wake(env_.context);
    /* Refresh ticks again while the GPU sleeps through a dark display (the display ticks only while it is on): the thread looks
     * at the screen at once and wakes the GPU before the first frame (A830PowerState screen rule). */
    else if(power_->screenSlept() && power_->phase()==A830PowerSlumber)env_.wake(env_.context);
}

void A830Dcvs::noteLateFrame(uint32_t) {__atomic_fetch_add(&late_,1u,__ATOMIC_RELAXED);}

void A830Dcvs::noteUnderruns(uint32_t total) {__atomic_store_n(&underrunTotal_,total,__ATOMIC_RELAXED);}

void A830Dcvs::park()
{
    if(!__atomic_exchange_n(&parked_,1u,__ATOMIC_ACQ_REL))env_.wake(env_.context);
}

/* ---- service thread ------------------------------------------------------------------------------------------------ */

uint64_t A830Dcvs::serviceTick(uint64_t now)
{
    if(!initialized_)return now+ms(1000);
    /* 1. A pending recovery first: nothing else may touch the GPU meanwhile. */
    if(recovery_ && recovery_->pending()) {
        recovery_->runPending();
        haveSample_=false;   /* the GMU busy counter restarted with the GMU */
        now=env_.nowNs(env_.context);
    }
    if(recovery_ && recovery_->lost()) {
        publishThermal(false);
        if(now>=nextStatus_)nextStatus_=now+ms(config_.statusMs);
        return nextStatus_;
    }
    /* A reset is coming (park()): the knob ceiling and the floor go to the lowest level for the rest of the boot. */
    if(__atomic_load_n(&parked_,__ATOMIC_ACQUIRE) && (knobCeiling_!=1 || floor_!=1)) {
        knobCeiling_=1;floor_=1;
        updateWakeTargets();
    }
    /* 2. The display's power (the screen rule, pagani-screenoff bit 1), then touches: boost, and the slumber pre-wake. */
    if(power_->config().screenSlumber!=A830SlumberOff) {
        const bool off=(__atomic_load_n(&pagani_screen_state,__ATOMIC_ACQUIRE)&1u)!=0;
        /* The display back: its first frames get the touch boost (a GPU woken for them starts at that level and keeps it). */
        if(!off && power_->screenOff())touchUntil_=now+ms(config_.touchHoldMs);
        power_->setScreenOff(off,now);
    }
    sampleTouch(now);
    /* 3. Slumber, pre-wake, IFPC switch. */
    uint64_t next=power_->tick(now);
    const bool active=power_->phase()==A830PowerActive;
    if(active) {
        /* A seqno written since the last look is a submission even without the adreno830_dcvs_submit() hook. */
        const uint32_t seqno=power_->submittedSeqno();
        if(seqno!=seenSeqno_) {
            seenSeqno_=seqno;
            if(__atomic_load_n(&submits_,__ATOMIC_RELAXED)==hookedSubmits_) {
                __atomic_fetch_add(&submits_,1u,__ATOMIC_RELAXED);
                __atomic_store_n(&lastSubmitNs_,now,__ATOMIC_RELEASE);
            }
            hookedSubmits_=__atomic_load_n(&submits_,__ATOMIC_RELAXED);
        }
        /* After a wake or a recovery the GMU runs what it was started with. */
        if(power_->votedLevel()!=level_ || power_->votedBus()!=bus_) {
            account(now);
            level_=power_->votedLevel();bus_=power_->votedBus();
        }
        /* 4. Window: two TEs when the display reports them, else the timer. */
        const bool teMode=__atomic_load_n(&vsyncs_,__ATOMIC_ACQUIRE) &&
                          now-__atomic_load_n(&lastVsyncNs_,__ATOMIC_ACQUIRE)<ms(config_.teTimeoutMs);
        const uint32_t vsyncs=__atomic_load_n(&vsyncs_,__ATOMIC_ACQUIRE);
        const bool due=teMode?vsyncs-windowVsyncs_>=2u || now-lastWindow_>=ms(config_.teTimeoutMs)
                             :now-lastWindow_>=ms(config_.windowMs);
        if(due || !haveSample_)window(now);
        /* Boosts can start between windows (first submission after idle, a touch): apply them right away. */
        steer(now);
        const uint64_t windowNext=lastWindow_+ms(teMode?config_.teTimeoutMs:config_.windowMs);
        if(windowNext<next)next=windowNext;
    } else {
        /* Asleep (or being recovered): no register, no vote. The GPU is idle, so the governor sits at the floor and
         * the next GMU start (HFI M9) gets the floor or a boost still running (A830PowerState adds its wake boosts). */
        haveSample_=false;
        account(now);
        policy_=floor_;down_=saturated_=0;raisedLast_=false;
        publishThermal(false);   /* no sensor read while asleep: the CPU governor sees no GPU limit */
        __atomic_store_n(&idle_,true,__ATOMIC_RELEASE);
        const uint8_t level=wanted(now);
        power_->setTarget(level,busOf(level));
        /* Asleep: look at the touch counter every window so the pre-wake hides the wake in the finger's movement (not while the
         * display is off under the screen rule: touches do not wake the GPU then, and the display's ticks wake this thread). */
        if(power_->phase()==A830PowerSlumber && power_->config().touchPrewake && !power_->screenOff()) {
            const uint64_t touchNext=now+ms(config_.windowMs);
            if(touchNext<next)next=touchNext;
        }
    }
    /* 5. Hang watchdog every 500 ms while work is in flight. */
    if(recovery_) {
        const uint64_t period=ms(recovery_->config().watchdogPeriodMs);
        if(now>=nextWatchdog_) {recovery_->watchdogTick(now);nextWatchdog_=now+period;}
        if(power_->inFlight() && nextWatchdog_<next)next=nextWatchdog_;
    }
    /* 6. Once a second: status rows, memory row, unwired fault lines, underruns. */
    if(now>=nextStatus_) {periodic(now);nextStatus_=now+ms(config_.statusMs);}
    if(nextStatus_<next)next=nextStatus_;
    return next>now?next:now+ms(1);
}

void A830Dcvs::sampleTouch(uint64_t now)
{
    const uint32_t touches=pagani_input_touch_events;
    if(!touchPrimed_) {touchSeen_=touches;touchPrimed_=true;return;}
    if(touches==touchSeen_)return;
    touchSeen_=touches;
    /* The screen rule: touches on the dark display neither boost nor pre-wake (A830PowerState::noteTouch ignores them too). */
    if(power_->config().screenSlumber!=A830SlumberOff && power_->screenOff())return;
    ++stats_.touches;
    if(now>=touchUntil_)++stats_.touchBoosts;
    touchUntil_=now+ms(config_.touchHoldMs);   /* held until 300 ms after the last touch */
    power_->noteTouch();
}

bool A830Dcvs::readBusy(uint64_t *ticks)
{
    /* High, low, high: a carry between the two halves shows as a changed high word; then the low word is re-read. */
    const uint32_t high=io_.read32(io_.context,A830PowerWinGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0));
    uint32_t low=io_.read32(io_.context,A830PowerWinGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0));
    const uint32_t high2=io_.read32(io_.context,A830PowerWinGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0));
    if(high2!=high)low=io_.read32(io_.context,A830PowerWinGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0));
    *ticks=uint64_t(high2)<<32|low;
    return true;
}

void A830Dcvs::account(uint64_t now)
{
    if(now<=lastAccount_) {lastAccount_=now;return;}
    const uint8_t at=power_->phase()==A830PowerActive?level_:0;
    residencyNs_[at<=Adreno830LevelMax?at:0]+=now-lastAccount_;
    lastAccount_=now;
}

void A830Dcvs::window(uint64_t now)
{
    uint64_t ticks=0;
    if(!power_->accessBegin(false)) {haveSample_=false;return;}
    readBusy(&ticks);
    power_->accessEnd(false);
    const uint32_t vsyncs=__atomic_load_n(&vsyncs_,__ATOMIC_ACQUIRE);
    if(!haveSample_ || now<=lastSample_) {
        lastTicks_=ticks;lastSample_=lastWindow_=now;windowVsyncs_=vsyncs;haveSample_=true;
        return;
    }
    const uint64_t span=now-lastSample_;
    uint64_t busyNs=ticksToNs(ticks-lastTicks_);
    if(busyNs>span)busyNs=span;
    const uint32_t busy=uint32_t(busyNs*1000/span);
    lastTicks_=ticks;lastSample_=lastWindow_=now;windowVsyncs_=vsyncs;
    statusBusyNs_+=busyNs;statusNs_+=span;
    lastBusyPermille_=busy;
    ++stats_.windows;
    account(now);
    sampleThermal(now);
    /* Late frame: the display missed a TE while this window was more than half busy. */
    const uint32_t late=__atomic_load_n(&late_,__ATOMIC_RELAXED);
    if(late!=seenLate_) {
        stats_.lateFrames+=late-seenLate_;
        seenLate_=late;
        if(busy>config_.lateBusyPermille) {
            /* From the governor's level, not the boosted one: late frames in a row must not walk up the table (most are the CPU's). */
            lateLevel_=min8(uint8_t(policy_+1),max8(policy_,sustainLevel_));
            lateUntil_=now+ms(config_.lateHoldMs);
            ++stats_.lateBoosts;
        }
    }
    const bool submitted=__atomic_load_n(&submits_,__ATOMIC_RELAXED)!=windowSubmits_;
    windowSubmits_=__atomic_load_n(&submits_,__ATOMIC_RELAXED);
    policy_=decide(now,busy,submitted || power_->inFlight());
}

uint8_t A830Dcvs::decide(uint64_t now,uint32_t busyPermille,bool active)
{
    if(!config_.governor)return fixedLevel();
    /* Demand: the frequency that would have kept this window at the target busy (75 %). */
    const uint64_t current=khz(level_);
    const uint64_t demand=current*busyPermille/config_.targetBusyPermille;
    uint8_t policy=policy_;
    bool raised=false;
    /* Under thermal pressure (design: the filtered temperature within 5 C of the target or above it) the up rules step one
     * level per window at most: fewer bursts to levels the thermal loop then has to take back. */
    const uint8_t oneUp=min8(uint8_t(level_+1),table_.count);
    const bool pressure=thermalPressure();
    if(busyPermille>=config_.upBusyPermille) {
        const uint8_t jump=min8(uint8_t(level_+config_.upJump),table_.count);
        const uint8_t needed=levelAtLeast(uint32_t(demand*config_.upBoostPermille/1000));
        uint8_t up=max8(jump,needed);
        if(pressure && up>oneUp) {up=oneUp;++stats_.pressureSteps;}
        policy=max8(up,policy);
        raised=true;
    } else if(demand>current) {
        uint8_t needed=levelAtLeast(uint32_t(demand*config_.upMarginPermille/1000));
        if(pressure && needed>oneUp) {needed=oneUp;++stats_.pressureSteps;}
        if(needed>policy) {policy=needed;raised=true;}
    } else if(policy<level_ && demand>khz(policy)) {
        /* Under a boost: the governor level follows demand, so that the boost ends at the level the load needs, not at the floor. */
        policy=levelAtLeast(uint32_t(demand));down_=0;
    } else if(policy>floor_ && demand*1000<=uint64_t(khz(uint8_t(policy-1)))*(policy>sustainLevel_?1000u:config_.downMarginPermille)) {
        /* Past the sustain level no margin: a level the load no longer needs goes as soon as the next one holds it at the target, or
         * 470 MHz of work, which 734 MHz holds at 64 %, would stay at 832 MHz after every burst (demand 627 > 0.85 x 734). */
        if(++down_>=(raisedLast_?config_.downWindowsAfterUp:config_.downWindows)) {
            --policy;down_=0;raisedLast_=false;
        }
    } else {
        down_=0;
    }
    if(raised) {raisedLast_=true;down_=0;}
    /* Sustain: a burst stops at sustainLevel_; beyond it, or beyond the level reached so far, one level per sustainWindows windows
     * saturated in a row. What is already reached stays for the down rule to take back. */
    if(busyPermille>=config_.upBusyPermille && level_>=sustainLevel_)++saturated_;
    else saturated_=0;
    uint8_t limit=max8(sustainLevel_,policy_);
    if(saturated_>=config_.sustainWindows) {limit=min8(uint8_t(limit+1),table_.count);saturated_=0;}
    if(policy>limit) {policy=limit;++stats_.sustainHolds;}
    /* Idle: 64 ms without a submission and nothing in flight -> the floor. */
    const uint64_t lastSubmit=__atomic_load_n(&lastSubmitNs_,__ATOMIC_ACQUIRE);
    if(!active && now-lastSubmit>=ms(config_.idleMs)) {
        policy=floor_;down_=saturated_=0;raisedLast_=false;
        __atomic_store_n(&idle_,true,__ATOMIC_RELEASE);
    }
    if(policy<floor_)policy=floor_;
    if(policy>ceiling())policy=ceiling();
    return policy;
}

void A830Dcvs::steer(uint64_t now)
{
    /* Wake boost: the first submission after idle -> >= 525 MHz for 50 ms. */
    const uint32_t submits=__atomic_load_n(&submits_,__ATOMIC_RELAXED);
    if(submits!=seenSubmits_) {
        seenSubmits_=submits;
        if(__atomic_load_n(&idle_,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&idle_,false,__ATOMIC_RELEASE);
            wakeUntil_=now+ms(config_.wakeBoostMs);
            ++stats_.wakeBoosts;
        }
    }
    const uint8_t level=wanted(now);
    vote(now,level,busOf(level),lastBusyPermille_);
}

uint8_t A830Dcvs::wanted(uint64_t now) const
{
    if(!config_.governor)return fixedLevel();
    uint8_t level=policy_;
    if(now<wakeUntil_)level=max8(level,levelAtLeast(config_.wakeBoostKhz));
    if(now<touchUntil_)level=max8(level,levelAtLeast(config_.touchBoostKhz));
    if(now<lateUntil_)level=max8(level,lateLevel_);
    level=max8(level,floor_);
    /* The ceilings win over every boost. */
    return level>ceiling()?ceiling():level;
}

void A830Dcvs::vote(uint64_t now,uint8_t level,uint8_t bus,uint32_t busyPermille)
{
    if(!level || level>table_.count)return;
    if(stopped_)return;              /* annibale: an unacknowledged vote ended the changes (the target stays level_) */
    power_->setTarget(level,bus);    /* sent as HFI M9 if the GMU starts before this vote goes out */
    if(level==level_ && bus==bus_)return;
    if(now<retryAt_)return;
    /* bw: the bus level under KGSL's no-AB mark 0xFFFF0000 (design), or alone (annibale, section 14 row 24); the AB vote
     * replaces the mark when the GMU takes one. */
    uint32_t bw=annibale_?uint32_t(bus):bus|A830_HFI_BW_NO_AB;
    if(config_.abVote) {
        const uint32_t ddr=bus<table_.ddrLevels?table_.ddrKbps[bus]:0;
        bw=abQuantize(uint32_t(uint64_t(ddr)*A830_DDR_CHANNELS*busyPermille/1000),table_.ddrKbps[table_.ddrLevels-1])|bus;
    }
    power_->lock();
    IOReturn result=A830ReturnNotResponding;
    /* Under the mutex no power transition runs; only an Active GMU takes a vote (asleep: target recorded above). */
    const bool active=power_->phase()==A830PowerActive;
    if(active)result=ops_.perfVote(ops_.context,level,bw);
    if(active && result==kIOReturnSuccess)power_->noteVoted(level,bus);
    power_->unlock();
    if(!active)return;
    if(result!=kIOReturnSuccess) {
        ++stats_.voteFailures;
        adreno830_status_add32(&adreno830_status.vote_failures,1);
        if(annibale_) {
            /* annibale (kernel-sequences 13): a vote the GMU did not acknowledge ends the frequency changes for this boot;
             * the GPU keeps the level it acknowledged last, and so does the next GMU start (HFI M9 of the target). */
            stopped_=true;stats_.votesStopped=true;
            power_->setTarget(level_,bus_);
            return;
        }
        retryAt_=now+ms(config_.voteRetryMs);   /* keep the current level; try again later */
        return;
    }
    account(now);
    level_=level;bus_=bus;
    ++stats_.votes;
    adreno830_status_add32(&adreno830_status.votes,1);
}

void A830Dcvs::sampleThermal(uint64_t now)
{
    Sm8750TsensSample sample;
    const int read=Sm8750TsensReadGpu(tsens_,&sample);
    Sm8750ThermalUpdate(&thermal_,read,&sample);   /* the zone (Unknown, Critical) and the last usable reading */
    if(thermal_.ever_valid)stats_.tempDeci=thermal_.last_deci;
    stats_.zone=uint8_t(thermal_.zone);
    const bool wasLimiting=thermalCeiling_<knobCeiling_;
    if(annibale_) {
        /* annibale (kernel-sequences 13, K90's processor rule applied to the GPU): hot at 70 C or without a usable reading
         * (the zone keeps the last valid one through short VALID gaps), cool again only below 60 C; hot holds 160 MHz, down
         * at once and back at once, as its speed rule does. */
        if(thermal_.zone==Sm8750ThermalUnknown || thermal_.last_deci>=config_.annibaleHotDeci)hot_=true;
        else if(thermal_.last_deci<config_.annibaleCoolDeci)hot_=false;
        stats_.thermalHot=hot_;
        thermalCeiling_=hot_?levelAtMost(config_.annibaleHotKhz):table_.count;
    } else {
        thermalLoop(now);
    }
    if(!wasLimiting && thermalCeiling_<knobCeiling_)++stats_.thermalCaps;
    stats_.filteredDeci=filterValid_?filtered16_/16:0;stats_.trend=trend_;stats_.emergency=emergency_;
    stats_.limiting=thermal_.zone!=Sm8750ThermalUnknown && thermalCeiling_<knobCeiling_;
    updateWakeTargets();
    publishThermal(true);
}

void A830Dcvs::thermalLoop(uint64_t now)
{
    if(thermal_.zone==Sm8750ThermalUnknown) {
        /* No usable reading (never, or Sm8750ThermalInvalidLimit windows in a row): 607 MHz for as long as that lasts (down at
         * once, back up from an emergency one level per 500 ms); the loop starts over from the next reading. */
        if(thermalCeiling_>unreadableLevel_)thermalCeiling_=unreadableLevel_;
        else if(thermalCeiling_<unreadableLevel_ && now>=recoverAt_) {
            ++thermalCeiling_;recoverAt_=now+ms(config_.thermalRecoverMs);
        }
        filterValid_=emergency_=false;trend_=0;coolSince_=0;
        return;
    }
    const int32_t deci=thermal_.last_deci,target=config_.thermalTargetDeci,cool=target-config_.thermalBandDeci;
    if(!filterValid_) {
        filtered16_=trendRef16_=deci*16;trendAt_=now;trend_=0;filterValid_=true;coolSince_=0;
        /* First reading: well below the target nothing has to be held back (the top at once); otherwise the loop works from
         * the ceiling in effect. */
        if(deci<=cool && thermal_.zone!=Sm8750ThermalCritical)thermalCeiling_=table_.count;
    } else {
        filtered16_+=(deci*16-filtered16_)/4;
    }
    const int32_t filtered16=filtered16_;
    bool decide=false;
    if(now-trendAt_>=ms(config_.thermalTrendMs)) {
        /* The trend of the filtered reading over the window that ends now; one down decision per window. */
        const int32_t delta=filtered16-trendRef16_,dead=config_.thermalTrendDeci*16;
        trend_=int8_t(delta>dead?1:delta<-dead?-1:0);
        trendRef16_=filtered16;trendAt_=now;
        decide=true;
    }
    /* Emergency: the critical zone (105 C on the hottest reading, left below 100 C) drops at once and holds. */
    if(thermal_.zone==Sm8750ThermalCritical) {
        if(!emergency_) {emergency_=true;++stats_.emergencies;}
        if(thermalCeiling_>emergencyLevel_)thermalCeiling_=emergencyLevel_;
        coolSince_=0;recoverAt_=now+ms(config_.thermalRecoverMs);
        return;
    }
    emergency_=false;
    if(thermalCeiling_<thermalFloor_) {
        /* Back from an emergency: up to the floor one level per 500 ms; below it the desktop is not fluid. */
        if(now>=recoverAt_) {++thermalCeiling_;recoverAt_=now+ms(config_.thermalRecoverMs);++stats_.thermalUps;}
        coolSince_=0;
        return;
    }
    if(filtered16<=cool*16) {
        /* At least 5 C below the target: up one level per 2 s of it (beyond the knob/ACD ceiling the steps change nothing). */
        if(!coolSince_)coolSince_=now;
        else if(now-coolSince_>=ms(config_.thermalUpMs) && thermalCeiling_<table_.count) {
            ++thermalCeiling_;coolSince_=now;++stats_.thermalUps;
        }
        return;
    }
    coolSince_=0;
    if(decide && filtered16>target*16 && trend_>=0) {
        /* Above the target and not falling: one level down from the ceiling in effect, never below the floor. */
        const uint8_t ceiling=min8(thermalCeiling_,knobCeiling_);
        if(ceiling>thermalFloor_) {thermalCeiling_=uint8_t(ceiling-1);++stats_.thermalDowns;}
    }
}

bool A830Dcvs::thermalPressure() const
{
    return !annibale_ && filterValid_ && filtered16_>=(config_.thermalTargetDeci-config_.thermalBandDeci)*16;
}

void A830Dcvs::publishThermal(bool active)
{
    uint32_t flags=0;
    if(active && thermal_.zone!=Sm8750ThermalUnknown) {
        flags|=A830ThermalValid;
        if(thermalCeiling_<knobCeiling_)flags|=A830ThermalLimiting;
        if(thermal_.zone==Sm8750ThermalCritical)flags|=A830ThermalEmergency;
    }
    const int32_t deci=thermal_.ever_valid?thermal_.last_deci:0;
    __atomic_store_n(&published_,uint32_t(uint16_t(int16_t(deci)))|flags<<16,__ATOMIC_RELEASE);
}

int32_t A830Dcvs::thermalState(uint32_t *flags) const
{
    const uint32_t word=__atomic_load_n(&published_,__ATOMIC_ACQUIRE);
    if(flags)*flags=word>>16;
    return int16_t(uint16_t(word));
}

void A830Dcvs::periodic(uint64_t now)
{
    const bool active=power_->phase()==A830PowerActive;
    /* Residency of the last window (10 s) in permille, by GMU level. */
    account(now);
    if(now-residencyStart_>=ms(config_.residencyMs)) {
        uint64_t total=0;
        for(const uint64_t ns:residencyNs_)total+=ns;
        for(unsigned i=0;i<=Adreno830LevelMax;++i) {
            stats_.residency[i]=total?uint32_t(residencyNs_[i]*1000/total):0;
            residencyNs_[i]=0;
        }
        residencyStart_=now;
    }
    stats_.busyPermille=uint16_t(statusNs_?statusBusyNs_*1000/statusNs_:0);
    statusBusyNs_=statusNs_=0;
    stats_.level=level_;stats_.bus=bus_;stats_.floor=floor_;stats_.ceiling=ceiling();stats_.thermalCeiling=thermalCeiling_;
    /* Underruns (5.3.6): growth -> host DDR floor up to level 6 (when it is lower) and, when it keeps growing, bus-max. */
    const uint32_t total=__atomic_load_n(&underrunTotal_,__ATOMIC_RELAXED);
    if(total!=seenUnderruns_) {
        stats_.underruns=total;
        __atomic_store_n(&adreno830_status.underruns,total,__ATOMIC_RELAXED);
        seenUnderruns_=total;
        underrunAt_=now;
        ++underrunStreak_;
        const uint32_t raised=table_.ddrKbps[config_.underrunFloorLevel];
        if(ops_.hostDdr && !floorRaised_ && config_.hostFloorKbps<raised &&
           ops_.hostDdr(ops_.context,raised)==kIOReturnSuccess) {
            floorRaised_=true;
            ++stats_.floorRaises;
        }
        if(underrunStreak_>=2 && !busMax_) {busMax_=true;++stats_.busMaxEntries;}
    } else {
        underrunStreak_=0;
        if(underrunAt_ && now-underrunAt_>=ms(config_.underrunHoldMs)) {
            underrunAt_=0;
            busMax_=false;
            if(floorRaised_ && ops_.hostDdr && ops_.hostDdr(ops_.context,config_.hostFloorKbps)==kIOReturnSuccess)
                floorRaised_=false;
        }
    }
    if(ops_.memoryRow) {
        uint32_t pool=0,used=0,imports=0,coherent=0;
        ops_.memoryRow(ops_.context,&pool,&used,&imports,&coherent);
        adreno830_status_memory(pool,used,imports,coherent);
    }
    /* The ring's queue over this period (4.13.5): stored only, the DCVS row below (asleep: the next publish) shows it. */
    if(ops_.queueRow)ops_.queueRow(ops_.context);
    if(!active)return;   /* asleep: the SLP row is A830PowerState's; no CX register may be read */
    if(ops_.pollFaults && power_->accessBegin(false)) {
        ops_.pollFaults(ops_.context);
        power_->accessEnd(false);
    }
    uint16_t residency[Adreno830LevelMax];
    for(unsigned i=0;i<table_.count;++i)residency[i]=uint16_t(stats_.residency[i+1]);
    if(thermal_.ever_valid)adreno830_status_temperature(stats_.tempDeci);   /* " <t>C" at the end of the RUN row */
    if(!annibale_) {   /* the design loop on the DCVS row: " T<target><trend>", "!" in an emergency */
        const uint32_t trend=!filterValid_?A830StatusTrendNone:trend_>0?A830StatusTrendRising:trend_<0?A830StatusTrendFalling
                                                                                                      :A830StatusTrendSteady;
        adreno830_status_thermal(config_.thermalTargetDeci,trend,emergency_?A830StatusThermalEmergency:0u);
    }
    adreno830_status_dcvs(khz(level_)/1000,stats_.busyPermille,level_,bus_,khz(floor_)/1000,khz(ceiling())/1000,1,table_.count,
                          residency);
}

/* ---- C hooks ------------------------------------------------------------------------------------------------------ */

extern "C" void adreno830_dcvs_vsync(void)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))dcvs->noteVsync();
}

extern "C" void adreno830_dcvs_late_frame(uint32_t late_us)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))dcvs->noteLateFrame(late_us);
}

extern "C" void adreno830_dcvs_underruns(uint32_t total)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))dcvs->noteUnderruns(total);
}

extern "C" void adreno830_dcvs_submit(void)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))dcvs->noteSubmit();
}

extern "C" void adreno830_dcvs_park(void)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))dcvs->park();
}

/* PaganiArmCpu's governor (pagani-cpufreq=2) reads these from its own thread every 20 ms; ArmCpu.cpp has weak defaults. */
extern "C" int32_t adreno830_thermal_state(uint32_t *flags)
{
    if(A830Dcvs *dcvs=__atomic_load_n(&gDcvs,__ATOMIC_ACQUIRE))return dcvs->thermalState(flags);
    if(flags)*flags=0;
    return 0;
}

extern "C" uint32_t adreno830_cpu_hint(void)
{
    return uint32_t(__atomic_load_n(&adreno830_status.submits,__ATOMIC_RELAXED));
}

extern "C" void pagani_display_vsync(uint64_t,uint64_t)
{
    adreno830_dcvs_vsync();
}

extern "C" void pagani_display_late(uint64_t late_ns)
{
    const uint64_t late_us=late_ns/1000;
    adreno830_dcvs_late_frame(late_us>0xFFFFFFFFull?0xFFFFFFFFu:uint32_t(late_us));
}

extern "C" void pagani_display_underrun(uint32_t total)
{
    adreno830_dcvs_underruns(total);
}
