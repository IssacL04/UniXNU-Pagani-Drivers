/* SPDX-License-Identifier: MIT */
#ifndef A830_DCVS_H
#define A830_DCVS_H
#include "A830PowerState.h"
#include "A830Recovery.h"
#include "Adreno830Board.h"
#include "Sm8750Tsens.h"

/* GPU frequency governor, thermal cap and the run-time service thread of the A830 (ARCHITECTURE 4.16, 5.3.6,
 * IMPLEMENTATION_PLAN M11; M13 d for the ACD ceiling).
 *
 * Busy: the GMU XO power counter 0 (POWER_COUNTER_XOCLK_0_H/L, selected with countable 0x20 and enabled in S5, 4.7.8),
 * 19.2 MHz ticks while the GPU is busy, read high-low-high. A window is two display TEs (16.7 ms at 120 Hz) when the
 * display reports TEs, 16 ms otherwise.
 * Policy (defaults, all in A830DcvsConfig): busy = ticks / (window x 19.2 MHz); demand = current x busy / 0.75.
 *   up: busy >= 0.90 -> at least two levels up and at least the level of demand x 1.25; otherwise demand above the
 *       current frequency -> the lowest level >= demand x 1.1; under thermal pressure (the filtered temperature within
 *       5 C of the target or above it) both rules step up one level per window at most;
 *   sustain: above 734 MHz only a load that kept the GPU at 90 % busy or more: 12 windows in a row (about 200 ms) at the top
 *       level reached so far allow one level more, however far the rules above ask (a burst stops at 734 MHz);
 *   down: one level after 4 consecutive windows (6 right after a step up) whose demand is <= 0.85 x the next lower level (above 734 MHz
 *       <= the next lower level: nothing keeps a load there that does not need it);
 *   idle: 64 ms without a submission and the ring empty -> the floor (pagani-gpu-dcvs-min, 160 MHz);
 *   wake boost: the first submission after idle -> at least 525 MHz for 50 ms;
 *   touch boost: a new touch (PaganiInput's pagani_input_touch_events) -> at least 525 MHz until 300 ms after the last;
 *   under a boost the governor level follows demand up, so the boost does not end in a drop to the floor and the burst of
 *       saturated windows that follows it;
 *   late frame: the display reports a swap after its target TE while the window is more than 50 % busy -> the governor level
 *       + 1 for 100 ms, under the sustain limit;
 * The defaults come from run 20261031-223924-stage1-full-1 (2026-10-31): 70 % / 0.8 held a load at about 55 % busy, so a desktop of
 * 470 MHz of work ran at 832-900 MHz and the GPU's own DDR vote at level 10 (the host vote stays at level 11, A830Dcvs does not lower
 * it: no DDR saving comes from these numbers); one saturated window from 525 MHz or more asked for the top (demand x 1.25 of 1.43 x
 * current); the sustain rule below is what keeps a burst from 832-900 MHz. 75 % and not more because a 120 Hz frame has 8.3 ms: the
 * second's mean fps did not follow the frequency, but the seconds at 115 fps or more did fall with busy (0-60 %: 31/126, 60-75 %: 9/60,
 * 75-85 %: 2/20, 85 % and up: 0/20), and the frames of a model fed that run's load were late by more than one frame twice as often at
 * 85 % as at 75 % (tests: testFrameHeadroom holds the 75 %). A correlation and a model, not a controlled test on the device.
 *   ceiling: pagani-gpu-dcvs-max (734 MHz while ACD is off; with ACD on and no explicit maximum, the table top) and the
 *       thermal ceiling below.
 * Thermal ceiling of the design profile (4.16, 2026-10-30): a closed loop around a target temperature instead of step caps.
 *   input: the hottest GPU sensor of each window (Sm8750Thermal), filtered (1/4 per window); its trend over 500 ms windows:
 *       rising above +0.2 C, falling below -0.2 C, else steady;
 *   down: one level per trend window while the filtered temperature is above the target (pagani-gpu-ttarget, 95 C) and
 *       not falling, never below 525 MHz (the floor that keeps the desktop fluid);
 *   up: one level per 2 s while it stays at least 5 C below the target (the dwell restarts when it leaves that band);
 *   between the two: the ceiling holds (the hysteresis band);
 *   emergency: Sm8750ThermalCritical (105 C, left below 100 C) -> 389 MHz at once and held; once left, back up to the floor
 *       one level per 500 ms;
 *   first reading (at start or after unreadable sensors): at least 5 C below the target -> the top at once, otherwise the
 *       loop works from the ceiling in effect; no usable reading (zone Unknown) -> 607 MHz for as long as that lasts.
 *   The ceiling, the target and the trend show on the DCVS row (" T80+", "!" in an emergency); adreno830_thermal_state()
 *   tells PaganiArmCpu's governor whether this ceiling limits the GPU (the CPU then gives up its top levels, 4.16).
 * Votes: GX_BW_PERF_VOTE {1, level, bus | 0xFFFF0000} only when the level or bus changes; the bus is the level's DT bus,
 * its bus-max while MDP underruns keep growing (5.3.6). A failed vote is counted and the level kept (retried after 1 s).
 * While the GMU sleeps the target is only recorded (A830PowerState::setTarget) and sent as HFI M9 at the next start.
 * pagani-gpu-dcvs=0 votes pagani-gpu-perf once and keeps it (still under the thermal and ACD ceilings).
 *
 * The annibale profile (A830Profile.h, read by init(); kernel-sequences section 13 and section 14 rows 24 and 38) keeps the
 * governor above and changes three things, as the REDMI K90 port runs it on the same SoC:
 *   ceiling: 342 MHz in the first round (no level above it was ever run under XNU); an explicit pagani-gpu-dcvs-max lifts
 *       it (still under the 734 MHz ACD-off ceiling);
 *   thermal: any GPU sensor at 70 C or more, or no usable reading (Sm8750Thermal zone Unknown), holds the GPU at 160 MHz
 *       until every sensor is below 60 C, and the ceiling comes back at once; the design's thermal loop above does not
 *       apply (nor its one-level up rule under pressure);
 *   votes: bw carries the bus level alone (no 0xFFFF0000), and a vote the GMU does not acknowledge ends the frequency
 *       changes for the boot (the GPU stays at the level it last acknowledged, also for the next GMU start).
 * pagani-gpu-maxlevel=n (A830DcvsConfig::maxLevel, both profiles; in the annibale profile S0 then hands over the whole DT
 * table): the knob ceiling is GMU level n (n beyond the table: its top) instead of pagani-gpu-dcvs-max's default and the
 * annibale 342 MHz; an explicit pagani-gpu-dcvs-max still lowers it, the ACD-off 734 MHz and the thermal ceilings still
 * apply. The governor, its boosts and the bus of each level (DT bus, bus-max under underruns) are the ones above.
 *
 * The service thread (run()) also drives the rest of the run-time work: the pending recovery, slumber and touch
 * pre-wake (A830PowerState::tick), the hang watchdog every 500 ms while work is in flight, the fault poll of unwired
 * interrupts and the status rows once a second, and the host DDR floor raise on MDP underruns (4.3.3). */

struct A830DcvsTable {
    const Adreno830Level *levels;       /* ascending; GMU level g is levels[g - 1] (level 0 = off is not in the table) */
    uint8_t count;                      /* 11 (common table) .. Adreno830LevelMax */
    const uint32_t *ddrKbps;            /* DDR bus table (12 levels) */
    uint8_t ddrLevels;
};

struct A830DcvsConfig {
    /* boot knobs (ARCHITECTURE 3.6) */
    bool governor=true;                 /* pagani-gpu-dcvs (1); 0 = fixed at fixedLevel */
    uint32_t minMhz=160,maxMhz=734;     /* pagani-gpu-dcvs-min / -max */
    bool maxGiven=false;                /* pagani-gpu-dcvs-max was on the command line */
    uint8_t maxLevel=0;                 /* pagani-gpu-maxlevel: 0 = the ceilings above; n = GMU level n (beyond: the top) */
    uint8_t fixedLevel=2;               /* pagani-gpu-perf */
    bool acd=false;                     /* ACD running (pagani-gpu-acd=1 and its HFI table acknowledged, 4.17 P3) */
    bool abVote=false;                  /* send an AB vote (needs HFI M7 GET_VALUE GMU_AB_VOTE == 1) */
    /* state handed over by S5/S10 */
    uint8_t initialLevel=2,initialBus=2;/* the M9 vote */
    uint32_t hostFloorKbps=0;           /* host DDR floor after S5 (pagani-gpu-ddr-floor) */
    uint32_t hostBootKbps=0;            /* host DDR boot vote (pagani-gpu-ddr) */
    uint8_t underrunFloorLevel=6;       /* host DDR level held while underruns grow below it (5.3.6) */
    /* policy (4.16) */
    uint32_t windowMs=16;               /* timer window when no TE arrives */
    uint32_t teTimeoutMs=50;            /* TEs older than this: back to the timer */
    uint16_t targetBusyPermille=750;
    uint16_t upBusyPermille=900,upJump=2,upBoostPermille=1250,upMarginPermille=1100;
    uint16_t downMarginPermille=850,downWindows=4,downWindowsAfterUp=6;
    uint32_t idleMs=64;
    uint32_t wakeBoostKhz=525000,wakeBoostMs=50;
    uint32_t touchBoostKhz=525000,touchHoldMs=300;
    uint16_t lateBusyPermille=500;
    uint32_t lateHoldMs=100;
    uint32_t acdOffMaxKhz=734000;
    uint32_t sustainKhz=734000;         /* the highest level a burst reaches; 0 = none (every level at once) */
    uint16_t sustainWindows=12;         /* saturated windows in a row at that level or above for one level more */
    /* thermal loop of the design profile (4.16) */
    int32_t thermalTargetDeci=950;      /* pagani-gpu-ttarget (whole C on the command line, x 10 here) */
    int32_t thermalBandDeci=50;         /* up only this far below the target; thermal pressure from here up */
    uint32_t thermalTrendMs=500;        /* trend window; one down step at most per window */
    int32_t thermalTrendDeci=2;         /* filtered change over a trend window beyond +-this: rising / falling */
    uint32_t thermalUpMs=2000;          /* up one level per this while the band below the target held */
    uint32_t thermalFloorKhz=525000;    /* the loop's lowest ceiling outside an emergency */
    uint32_t thermalEmergencyKhz=389000;/* Sm8750ThermalCritical (>= 105 C until below 100 C): at once */
    uint32_t thermalRecoverMs=500;      /* after an emergency: back up to the floor one level per this */
    uint32_t voteRetryMs=1000;
    uint32_t underrunHoldMs=10000;      /* bus-max and the raised host floor end after this long without growth */
    uint32_t statusMs=1000;             /* status rows, fault poll, memory row, underrun check */
    uint32_t residencyMs=10000;         /* residency window of the DCVS row */
    /* annibale profile (kernel-sequences 13, section 14 row 38) */
    uint32_t annibaleMaxKhz=342000;     /* first-round ceiling while pagani-gpu-dcvs-max is not given */
    int32_t annibaleHotDeci=700;        /* any sensor at or above (or no usable reading): held at annibaleHotKhz */
    int32_t annibaleCoolDeci=600;       /* ... until the hottest sensor is below this */
    uint32_t annibaleHotKhz=160000;
};

struct A830DcvsOps {
    void *context;
    IOReturn (*perfVote)(void *context,uint8_t level,uint32_t bw);   /* A830Hfi::perfVote: GX_BW_PERF_VOTE {1, level, bw} */
    IOReturn (*hostDdr)(void *context,uint32_t kbps);                /* Sm8750RpmhHostDdr on the S1-S5 host (may be null) */
    void (*pollFaults)(void *context);                               /* A830Gmu/A830Smmu poll of unwired lines (may be null) */
    void (*memoryRow)(void *context,uint32_t *poolMib,uint32_t *usedMib,uint32_t *imports,uint32_t *coherent); /* may be null */
    /* The ring's queue period into the status block (A830Fence::takeQueue + adreno830_status_queue, ARCHITECTURE 4.13.5),
     * once a second after the memory row, awake or asleep, before the DCVS row publishes; may be null. */
    void (*queueRow)(void *context);
};

struct A830DcvsStats {
    uint32_t windows,votes,voteFailures,wakeBoosts,touchBoosts,lateBoosts,lateFrames,thermalCaps,busMaxEntries,floorRaises;
    uint32_t sustainHolds;              /* windows whose rules asked for more than the sustain limit */
    uint32_t touches,vsyncs,underruns;
    uint8_t level,bus,floor,ceiling,thermalCeiling,zone;
    uint16_t busyPermille;              /* last status period */
    int32_t tempDeci;                   /* hottest GPU sensor, last valid read */
    uint32_t residency[Adreno830LevelMax + 1];   /* permille of the last residency window, by GMU level */
    bool annibale;                      /* the annibale profile was in effect at init() */
    bool thermalHot;                    /* annibale: held at 160 MHz by the 70/60 C rule */
    bool votesStopped;                  /* annibale: an unacknowledged vote ended the frequency changes */
    /* the design profile's thermal loop (4.16) */
    int32_t filteredDeci;               /* filtered hottest sensor (0 before a reading) */
    int8_t trend;                       /* over the last trend window: +1 rising, 0 steady, -1 falling */
    bool emergency;                     /* Sm8750ThermalCritical: the ceiling held at 389 MHz */
    bool limiting;                      /* the thermal ceiling is below the knob/ACD ceiling */
    uint32_t thermalDowns,thermalUps,emergencies,pressureSteps;   /* loop steps, emergencies entered, up steps cut to one */
};

class A830Dcvs {
public:
    IOReturn init(const A830DcvsConfig &config,const A830DcvsTable &table,const A830PowerIo &io,const A830PowerEnv &env,
                  const A830DcvsOps &ops,A830PowerState *power,A830Recovery *recovery,const Sm8750TsensIo *tsens);
    /* S10: fixed mode votes pagani-gpu-perf once; then the service thread starts (both modes). */
    IOReturn start();
    /* One service iteration at `now`; returns the next deadline (ns). run() loops on it; tests call it directly. */
    uint64_t serviceTick(uint64_t now);
    void run();

    /* Hooks (lock-free, interrupt-safe). */
    void noteSubmit();                  /* every submission (wake boost, idle clock) */
    void noteVsync();                   /* a display TE */
    void noteLateFrame(uint32_t lateUs);/* a swap after its target TE */
    void noteUnderruns(uint32_t total); /* MDP INTF1 underrun count (cumulative) */
    void park();                        /* a reset is coming: hold the lowest level and its bus from the next tick on */
    void setAcd(bool running);          /* ACD came up or went away (ceiling) */

    /* Pure helpers (tested directly). */
    uint8_t levelAtLeast(uint32_t khz) const;   /* lowest level >= khz, the top when none */
    uint8_t levelAtMost(uint32_t khz) const;    /* highest level <= khz, 1 when none */
    uint32_t khz(uint8_t level) const {return level>=1 && level<=table_.count?table_.levels[level-1].khz:0;}
    uint8_t busOf(uint8_t level) const;
    static uint32_t abQuantize(uint32_t abKbps,uint32_t topKbps);   /* KGSL adreno_gmu_bus_ab_quantize, 4 channels */

    uint8_t level() const {return level_;}
    uint8_t bus() const {return bus_;}
    uint8_t floorLevel() const {return floor_;}
    uint8_t ceiling() const;            /* min(knob/ACD ceiling, thermal ceiling) */
    uint8_t thermalCeiling() const {return thermalCeiling_;}
    const A830DcvsStats &stats() const {return stats_;}
    const Sm8750Thermal &thermal() const {return thermal_;}
    /* adreno830_thermal_state(): lock-free, any thread. */
    int32_t thermalState(uint32_t *flags) const;

private:
    void window(uint64_t now);
    uint8_t decide(uint64_t now,uint32_t busyPermille,bool active);
    void steer(uint64_t now);
    uint8_t wanted(uint64_t now) const;   /* governor level with the running boosts, under the ceilings */
    void vote(uint64_t now,uint8_t level,uint8_t bus,uint32_t busyPermille);
    uint8_t fixedLevel() const;
    void updateWakeTargets();
    void sampleThermal(uint64_t now);
    void thermalLoop(uint64_t now);     /* the design profile's closed loop (4.16) */
    bool thermalPressure() const;       /* design: filtered temperature at or above target - band */
    void publishThermal(bool active);   /* the word adreno830_thermal_state() reads */
    void sampleTouch(uint64_t now);
    void periodic(uint64_t now);
    bool readBusy(uint64_t *ticks);
    void account(uint64_t now);

    A830DcvsConfig config_{};
    A830DcvsTable table_{};
    A830PowerIo io_{};
    A830PowerEnv env_{};
    A830DcvsOps ops_{};
    A830PowerState *power_=nullptr;
    A830Recovery *recovery_=nullptr;
    const Sm8750TsensIo *tsens_=nullptr;
    Sm8750Thermal thermal_{};
    A830DcvsStats stats_{};
    bool initialized_=false,started_=false;
    bool annibale_=false,hot_=false,stopped_=false;   /* the profile at init(); annibale thermal hold; votes ended */
    /* levels */
    uint8_t level_=0,bus_=0,floor_=1,knobCeiling_=1,thermalCeiling_=1;
    /* thermal loop (design profile) */
    uint8_t thermalFloor_=1,emergencyLevel_=1,unreadableLevel_=1;
    int32_t filtered16_=0,trendRef16_=0;      /* filtered hottest sensor, deci-C x 16 */
    bool filterValid_=false,emergency_=false;
    int8_t trend_=0;
    uint64_t trendAt_=0,coolSince_=0,recoverAt_=0;
    uint32_t published_=0;                    /* adreno830_thermal_state: deci-C (int16) | A830Thermal* flags << 16 */
    /* busy sampling */
    uint64_t lastTicks_=0,lastSample_=0;
    bool haveSample_=false;
    /* policy state */
    uint8_t policy_=0;                  /* governor level before boosts */
    uint8_t sustainLevel_=1;            /* the highest level a burst reaches (A830DcvsConfig::sustainKhz) */
    uint16_t down_=0,saturated_=0;      /* qualifying windows in a row: down rule, sustain rule */
    bool raisedLast_=false,idle_=false;
    uint64_t wakeUntil_=0,touchUntil_=0,lateUntil_=0,retryAt_=0;
    uint8_t lateLevel_=0;
    uint32_t lastBusyPermille_=0;
    /* hooks */
    uint32_t submits_=0,seenSubmits_=0,windowSubmits_=0,vsyncs_=0,windowVsyncs_=0,late_=0,seenLate_=0,underrunTotal_=0,
             seenUnderruns_=0;
    uint64_t lastSubmitNs_=0,lastVsyncNs_=0,lastWindow_=0;
    uint32_t seenSeqno_=0,hookedSubmits_=0;   /* ring seqno seen, hook count at that time (submissions without the hook) */
    uint32_t touchSeen_=0,parked_=0;
    bool touchPrimed_=false;
    /* periodic */
    uint64_t nextStatus_=0,nextWatchdog_=0,residencyStart_=0,statusStart_=0,statusBusyNs_=0,statusNs_=0,lastAccount_=0;
    uint64_t residencyNs_[Adreno830LevelMax + 1]={};
    /* underruns */
    uint32_t underrunStreak_=0;
    uint64_t underrunAt_=0;
    bool busMax_=false,floorRaised_=false;
};

extern "C" {
/* The display's hooks (PD NativeLegacyDisplay / DpuProbe call them; the display carries weak empty defaults so it links
 * without this driver). All are interrupt-safe and do nothing before S10. */
void adreno830_dcvs_vsync(void);                   /* each panel TE */
void adreno830_dcvs_late_frame(uint32_t late_us);  /* a swap that missed its target TE by late_us */
void adreno830_dcvs_underruns(uint32_t total);     /* cumulative MDP INTF1 underruns (INTR_STATUS bit 26) */
/* The submit path (NativeAdrenoUserClient/Submit): one call per submission, lock-free. */
void adreno830_dcvs_submit(void);
/* BootProgress RamoopsLog, some seconds before a timed reset (pagani-reset-after): the GPU drops to its lowest level and bus
 * so the SoC and the DRAM stacked on it cool down before the reset (a hot DRAM loses more of the log across it). Interrupt
 * safe; BootProgress carries a weak empty default. */
void adreno830_dcvs_park(void);
/* PaganiInput's touch frame counter; this driver carries a weak zero definition so it links without the input driver. */
extern volatile uint32_t pagani_input_touch_events;
/* PaganiPanel's display power word (bit 0: the system turned the display off; the count of changes above it), read every
 * service tick for the screen rule of A830PowerState (pagani-screenoff bit 1, A830PowerConfig::screenSlumber) and nothing else;
 * a weak zero definition here as for the touch counter. While the rule is on and the display off, touches neither boost the GPU
 * nor pre-wake it, and asleep the thread no longer polls them every window (the display's refresh ticks wake it once the
 * display is back). */
extern volatile uint32_t pagani_screen_state;
/* For PaganiArmCpu's governor (pagani-cpufreq=2; ArmCpu.cpp carries weak defaults returning 0), lock-free, any thread:
 *  - adreno830_thermal_state: the hottest GPU sensor of the last DCVS window in deci-degrees C (0 before a usable reading)
 *    and, in *flags (may be null), A830ThermalValid (a usable reading), A830ThermalLimiting (the thermal ceiling holds the
 *    GPU below its knob/ACD ceiling: the design loop, or the annibale 160 MHz hold), A830ThermalEmergency (105 C until below
 *    100 C). While the GPU sleeps, is being recovered or is lost, the flags are 0 and the last temperature stays.
 *  - adreno830_cpu_hint: a free-running count of accepted GPU submissions (adreno830_status.submits); a change means GPU
 *    work, and the CPU governor then keeps a mid level for 100 ms. */
enum : uint32_t {A830ThermalValid=1u<<0,A830ThermalLimiting=1u<<1,A830ThermalEmergency=1u<<2};
int32_t adreno830_thermal_state(uint32_t *flags);
uint32_t adreno830_cpu_hint(void);
/* The display's own hook names (PD NativeMobileDisplay: NativeLegacyDisplay.cpp calls pagani_display_vsync on every
 * refresh tick and pagani_display_late for a swap past its target, DpuProbe.cpp pagani_display_underrun when the INTF1
 * underrun count grows; the display carries weak empty defaults). These strong definitions forward them to the
 * adreno830_dcvs_* hooks above (te_mach and period_ticks are not needed: the window counts TEs). */
void pagani_display_vsync(uint64_t te_mach, uint64_t period_ticks);
void pagani_display_late(uint64_t late_ns);
void pagani_display_underrun(uint32_t total);
}

#endif
