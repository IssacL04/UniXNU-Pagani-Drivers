/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c
 * The DCVS of ARCHITECTURE 4.16 replayed on busy traces: the GMU XO busy counter of the register model advances by
 * busy x window, the virtual clock by one window per service tick. Checked: the up rules (>= 90 % busy: two levels and
 * demand x 1.25; demand above the current level: demand x 1.1), the sustain rule (a burst stops at 734 MHz, one level more per
 * 12 saturated windows), the down rule (4 windows, 6 after a step up), idle to the floor after 64 ms, the wake boost (525 MHz
 * for 50 ms), the touch boost (525 MHz until 300 ms after the last touch), the late-frame boost (+1 for 100 ms when the window
 * is > 50 % busy), a composited load whose busy follows the level (testCompositing), frames that fit their TE and a level that holds
 * still at every load up to the sustain limit (testFrameHeadroom), the ACD and knob ceilings, fixed mode,
 * votes only on change and failed votes kept, no register access while the GMU sleeps, TE-driven windows, MDP underruns (host
 * floor and bus-max), the hang watchdog cadence, and the once-a-second status rows.
 * The design profile's thermal loop (2026-10-30) runs against a first-order heat model of the GPU hotspot fed into the
 * tsens2 model (each level heats toward its own steady temperature under load, plus heat from elsewhere): sustained load
 * settles at the target within one level, the trend holds the ceiling while it falls, the 5 C band holds it in between,
 * 525 MHz is the floor outside the 95 C emergency (389 MHz at once, back to the floor one level per 500 ms below 90 C),
 * recovery one level per 2 s, pagani-gpu-ttarget, 607 MHz without readings and the top at once from a cool first reading,
 * one-level up steps under thermal pressure, and the hooks PaganiArmCpu reads (adreno830_thermal_state, adreno830_cpu_hint). */
#include "A830PowerFakes.h"

static A830PowerState gPower;
static A830Recovery gRecovery;
static A830Dcvs gDcvs;

static uint32_t gTsens[9];
static unsigned gTsensReads;
static uint32_t tsensRead(void *,uint32_t offset)
{
    ++gTsensReads;
    const uint32_t n=(offset-0xa0)/4;
    return n>=1 && n<=8?gTsens[n]:0;
}
static const Sm8750TsensIo gTsensIo={nullptr,tsensRead};
static void temperature(int deci)
{
    for(unsigned n=1;n<=8;++n)gTsens[n]=(uint32_t(n==3?deci:deci-40)&0xfffu)|1u<<21;
}

static constexpr uint64_t WindowNs=16000000ull;

static void boot(A830DcvsConfig config=A830DcvsConfig{},bool sensors=true,const A830PowerConfig &power=A830PowerConfig{})
{
    resetAll();
    gTsensReads=0;
    gQueuePower=&gPower;
    temperature(450);
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    CHECK(gRecovery.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,gRecoveryOps,&gPower)==kIOReturnSuccess);
    gPower.start(config.initialLevel,config.initialBus);
    CHECK(gDcvs.init(config,kTable,gIo,gPowerEnv,gDcvsOps,&gPower,&gRecovery,sensors?&gTsensIo:nullptr)==kIOReturnSuccess);
    CHECK(gDcvs.start()==kIOReturnSuccess);
}

/* One window of `busy` permille; `submit` adds a submission (and retires it at once). */
static void step(uint32_t busy,bool submit=true,uint64_t ns=WindowNs)
{
    gEnv.now+=ns;
    gHw.busyTicks+=uint64_t(A830_XO_HZ)*ns/1000000000ull*busy/1000;
    if(submit) {gDcvs.noteSubmit();++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;}
    gDcvs.serviceTick(gEnv.now);
}
static void steps(unsigned n,uint32_t busy,bool submit=true) {for(unsigned i=0;i<n;++i)step(busy,submit);}

/* Settle: the first reading (45 C, well below the target) lifts the thermal ceiling from the unknown-zone cap to the top at
 * once, then idle at the floor, then the wake boost of the first submission spent. Leaves the GPU at the floor with 10 %
 * busy windows (under a boost the governor level follows demand: a busier window at 525 MHz would leave it above the floor). */
static void settle()
{
    steps(100,0,false);
    CHECK(gDcvs.level()==1 && gDcvs.ceiling()==9);
    steps(6,100);
    CHECK(gDcvs.level()==1);
}

static void testLevels()
{
    boot();
    CHECK(gDcvs.levelAtLeast(160000)==1 && gDcvs.levelAtLeast(525000)==6 && gDcvs.levelAtLeast(526000)==7 &&
          gDcvs.levelAtLeast(2000000)==11 && gDcvs.levelAtMost(607000)==7 && gDcvs.levelAtMost(388999)==3 &&
          gDcvs.levelAtMost(100)==1 && gDcvs.khz(9)==734000 && gDcvs.khz(0)==0 && gDcvs.khz(12)==0);
    CHECK(gDcvs.floorLevel()==1 && gDcvs.busOf(7)==6 && gDcvs.busOf(0)==0);
    /* Ceilings: 734 MHz while ACD is off, even with an explicit higher maximum; ACD on: the table top or the knob. */
    A830DcvsConfig c;c.maxMhz=900;c.maxGiven=true;
    boot(c);steps(100,0,false);
    CHECK(gDcvs.ceiling()==9);
    c.acd=true;boot(c);steps(100,0,false);
    CHECK(gDcvs.ceiling()==11);
    c.maxGiven=false;c.maxMhz=734;boot(c);steps(100,0,false);
    CHECK(gDcvs.ceiling()==11);
    c.maxGiven=true;c.maxMhz=660;boot(c);steps(100,0,false);
    CHECK(gDcvs.ceiling()==8);
    gDcvs.setAcd(false);
    CHECK(gDcvs.ceiling()==8);
    c=A830DcvsConfig{};c.minMhz=300;c.maxMhz=500;c.maxGiven=true;boot(c);steps(100,0,false);
    CHECK(gDcvs.floorLevel()==3 && gDcvs.ceiling()==5 && gDcvs.level()==3);
    /* Bad tables. */
    A830Dcvs d;
    Adreno830Level bad[2]={{300000,0x40,2,4,6,0,0},{200000,0x40,2,4,6,0,0}};
    CHECK(d.init(A830DcvsConfig{},A830DcvsTable{bad,2,kDdr,12},gIo,gPowerEnv,gDcvsOps,&gPower,nullptr,nullptr)==kIOReturnBadArgument);
    bad[1]={400000,0x40,2,9,6,0,0};
    CHECK(d.init(A830DcvsConfig{},A830DcvsTable{bad,2,kDdr,12},gIo,gPowerEnv,gDcvsOps,&gPower,nullptr,nullptr)==kIOReturnBadArgument);
    A830DcvsOps ops=gDcvsOps;ops.perfVote=nullptr;
    CHECK(d.init(A830DcvsConfig{},kTable,gIo,gPowerEnv,ops,&gPower,nullptr,nullptr)==kIOReturnBadArgument);
    /* AB quantization (KGSL): 1/65536 of top x 4 channels, [15:8] = 1. */
    CHECK(A830Dcvs::abQuantize(0,18597656)==0x100u);
    CHECK(A830Dcvs::abQuantize(18597656u*2,18597656)==(0x8000u<<16|0x100u));
    CHECK(A830Dcvs::abQuantize(0xffffffffu,18597656)==(0xfffeu<<16|0x100u));
    CHECK(A830Dcvs::abQuantize(1,18597656)==(1u<<16|0x100u));
    CHECK(A830Dcvs::abQuantize(5,0)==A830_HFI_BW_NO_AB);
}

static void testUpDown()
{
    boot();
    settle();
    /* demand above current -> lowest level >= demand x 1.1: at 88 % busy g1 -> g2 -> g3 -> g5 -> g7 -> g9, the ACD-off ceiling. */
    const uint8_t expected[]={2,3,5,7,9,9,9};
    for(uint8_t want:expected) {step(880);CHECK(gDcvs.level()==want);}
    /* The vote: GX_BW_PERF_VOTE {level, bus | 0xFFFF0000}, sent only on a change. */
    CHECK(gOps.votes.back().first==9 && gOps.votes.back().second==(0xffff0000u|8));
    const size_t votes=gOps.votes.size();
    steps(5,880);
    CHECK(gOps.votes.size()==votes && gDcvs.stats().votes==votes);
    /* Down: 6 windows right after a step up, then 4 per level. */
    steps(5,100);
    CHECK(gDcvs.level()==9);
    step(100);
    CHECK(gDcvs.level()==8);
    steps(3,100);
    CHECK(gDcvs.level()==8);
    step(100);
    CHECK(gDcvs.level()==7);
    /* A window that does not qualify resets the count. */
    steps(3,100);step(700);steps(3,100);
    CHECK(gDcvs.level()==7);
    step(100);
    CHECK(gDcvs.level()==6);
    /* >= 90 % busy: at least two levels up and demand x 1.25 (g6 at 95 %: 665 MHz x 1.25 = 831 -> the ceiling, g9). */
    step(950);
    CHECK(gDcvs.level()==9);
    boot();settle();
    step(900);
    CHECK(gDcvs.level()==3);   /* g1 at 90 %: +2 = g3; demand 192 x 1.25 = 240 MHz -> g3 */
    /* Idle: 64 ms without submission and nothing in flight -> the floor at once. */
    steps(10,950);
    CHECK(gDcvs.level()>=6);
    steps(3,0,false);
    CHECK(gDcvs.level()>1);
    step(0,false);step(0,false);
    CHECK(gDcvs.level()==1);
    /* Work still in flight is not idle. */
    boot();settle();
    steps(10,950);
    ++gOps.lastSubmitted;   /* submitted, not retired */
    steps(10,0,false);
    CHECK(gDcvs.level()>1);
}

static void testBoosts()
{
    boot();
    settle();
    /* Wake boost: first submission after idle -> 525 MHz (g6) for 50 ms, applied at once. */
    steps(6,0,false);
    CHECK(gDcvs.level()==1);
    gEnv.now+=1000000;
    gDcvs.noteSubmit();
    ++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;
    CHECK(gEnv.wakes>=1);   /* the thread is woken for it */
    gDcvs.serviceTick(gEnv.now);
    CHECK(gDcvs.level()==6 && gDcvs.stats().wakeBoosts>=1);
    steps(2,100);
    CHECK(gDcvs.level()==6);
    steps(2,100);
    CHECK(gDcvs.level()==1);
    /* Without the adreno830_dcvs_submit() hook a new ring seqno counts as the submission (one tick later). */
    steps(6,0,false);
    CHECK(gDcvs.level()==1);
    const uint32_t boosts=gDcvs.stats().wakeBoosts;
    ++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;
    gEnv.now+=1000000;
    gDcvs.serviceTick(gEnv.now);
    CHECK(gDcvs.level()==6 && gDcvs.stats().wakeBoosts==boosts+1);
    steps(4,100,false);
    CHECK(gDcvs.level()==1);
    /* Touch boost: >= 525 MHz (g6) until 300 ms after the last touch. */
    pagani_input_touch_events=pagani_input_touch_events+1;
    step(100);
    CHECK(gDcvs.level()==6 && gDcvs.stats().touchBoosts==1);
    steps(10,100);
    pagani_input_touch_events=pagani_input_touch_events+1;   /* extends the hold */
    steps(18,100);
    CHECK(gDcvs.level()==6 && gDcvs.stats().touchBoosts==1 && gDcvs.stats().touches==2);
    steps(2,100);
    CHECK(gDcvs.level()==1);
    /* Late frame with a busy window (> 50 %): current + 1 for 100 ms; with a quiet window: nothing. */
    steps(4,510);
    const uint8_t base=gDcvs.level();
    adreno830_dcvs_late_frame(4000);
    step(510);
    CHECK(gDcvs.level()==base+1 && gDcvs.stats().lateBoosts==1);
    steps(5,510);
    CHECK(gDcvs.level()==base+1);
    steps(2,510);
    CHECK(gDcvs.level()==base);
    adreno830_dcvs_late_frame(4000);
    step(300);
    CHECK(gDcvs.stats().lateBoosts==1 && gDcvs.stats().lateFrames==2);
    /* Boosts never pass the ceiling: the 105 C emergency (389 MHz, g4) beats the touch boost. */
    temperature(1060);
    pagani_input_touch_events=pagani_input_touch_events+1;
    step(100);
    CHECK(gDcvs.level()==4 && gDcvs.ceiling()==4);
}

/* ---- The design profile's thermal loop (ARCHITECTURE 4.16, 2026-10-30) --------------------------------------------------- */

/* A first-order heat model of the GPU hotspot: each window it moves toward the steady temperature of the level the GPU ran at
 * in that window (scaled by the window's busy), plus heat from elsewhere (the CPU clusters), with time constant tauS; the
 * hottest sensor reads it (the others 4 C less, temperature()). Steady temperatures by GMU level at full load: g7 (607 MHz)
 * 77 C and g8 (660 MHz) 81 C bracket the 80 C target; 900 MHz would end at 98 C. `wobble` adds a deterministic +-0.3 C
 * sensor ripple. */
struct HeatModel {
    double celsius=45,ambient=40,extra=0,tauS=2.0;
    double steady[12]={0,48,52,58,62,66,72,77,81,86,92,98};
    bool wobble=false;
    unsigned windows=0;
};
static HeatModel gHeat;
struct HeatTrace {
    uint8_t minCeiling=255,maxCeiling=0,minLevel=255;
    double minC=1000,maxC=-1000;
    unsigned changes=0;
    uint8_t last=0;
    void note(uint8_t ceiling,uint8_t level,double celsius)
    {
        if(ceiling<minCeiling)minCeiling=ceiling;
        if(ceiling>maxCeiling)maxCeiling=ceiling;
        if(level<minLevel)minLevel=level;
        if(celsius<minC)minC=celsius;
        if(celsius>maxC)maxC=celsius;
        if(last && ceiling!=last)++changes;
        last=ceiling;
    }
};
static const bool gTraceHeat=std::getenv("A830_HEAT_TRACE")!=nullptr;
static void heatStep(uint32_t busy,HeatTrace *trace=nullptr)
{
    const double load=gHeat.ambient+gHeat.extra+(gHeat.steady[gDcvs.level()]-gHeat.ambient)*busy/1000.0;
    gHeat.celsius+=(load-gHeat.celsius)*(double(WindowNs)/1e9)/gHeat.tauS;
    const double ripple=gHeat.wobble?0.3*(int((gHeat.windows*7)%5)-2)/2.0:0.0;
    ++gHeat.windows;
    temperature(int((gHeat.celsius+ripple)*10+0.5));
    step(busy);
    if(trace)trace->note(gDcvs.ceiling(),gDcvs.level(),gHeat.celsius);
    if(gTraceHeat && !(gHeat.windows%32))
        std::fprintf(stderr,"t=%6.2fs T=%5.1f filt=%4d trend=%2d ceil=%2u level=%2u\n",gHeat.windows*0.016,gHeat.celsius,
                     gDcvs.stats().filteredDeci,int(gDcvs.stats().trend),gDcvs.ceiling(),gDcvs.level());
}
/* `n` windows at a temperature moving linearly from `from` to `to` (deci-C). */
static void ramp(int from,int to,unsigned n,uint32_t busy=950)
{
    for(unsigned i=1;i<=n;++i) {temperature(from+(to-from)*int(i)/int(n));step(busy);}
}
static unsigned heatCases;

/* Sustained full load from cold, against the heat model: 900 MHz bursts would end at 98 C; the loop takes the ceiling down
 * one level per 500 ms while the filtered reading is above 80 C and not falling, waits while it falls, and the ceiling then
 * stays within one level with the hotspot 5 C under the target to 1 C above it; never near the emergency, never below the
 * 525 MHz floor. A faster hotspot (1 s) with sensor ripple settles the same way. */
static void testThermalSettle()
{
    for(const double tau:{2.0,1.0}) {
        A830DcvsConfig c;c.acd=true;c.thermalTargetDeci=800;   /* ACD on: the table top, 900 MHz; the 80 C target the model brackets */
        boot(c);
        gHeat=HeatModel{};gHeat.tauS=tau;gHeat.wobble=tau<2.0;
        steps(100,0,false);
        CHECK(gDcvs.ceiling()==11 && gDcvs.thermalCeiling()==11 && gDcvs.level()==1 && !gDcvs.stats().limiting);
        HeatTrace early,late;
        for(unsigned i=0;i<1875;++i)heatStep(1000,&early);   /* 30 s */
        for(unsigned i=0;i<3750;++i)heatStep(1000,&late);    /* 60 s more */
        if(gTraceHeat)std::fprintf(stderr,"tau %.1f: early ceiling %u-%u %.1f-%.1f C, late ceiling %u-%u %.1f-%.1f C, %u changes\n",
                                   tau,early.minCeiling,early.maxCeiling,early.minC,early.maxC,late.minCeiling,late.maxCeiling,
                                   late.minC,late.maxC,late.changes);
        /* The transient: 85.2 C at its peak with the 2 s hotspot, 88.5 C with the 1 s one, far under the 105 C emergency. */
        CHECK(early.maxCeiling==11 && early.minCeiling>=6 && early.maxC<90.0 && gDcvs.stats().emergencies==0);
        CHECK(late.maxCeiling-late.minCeiling<=1 && late.minCeiling>=7 && late.maxCeiling<=8 && late.minLevel>=7);
        CHECK(late.minC>=75.0 && late.maxC<=81.5);
        CHECK(gDcvs.stats().thermalDowns>=3 && gDcvs.stats().thermalCaps==1 && gDcvs.ceiling()<11);
        /* The load ends: the hotspot cools, at least 5 C under the target the ceiling climbs one level per 2 s back to the top. */
        HeatTrace idle;
        for(unsigned i=0;i<2500;++i)heatStep(100,&idle);     /* 40 s at 10 % busy */
        CHECK(gDcvs.ceiling()==11 && idle.minCeiling>=7 && gDcvs.stats().thermalUps>=3 && !gDcvs.stats().limiting);
        ++heatCases;
    }
    /* Levels that straddle the band: 607 MHz settles at 73 C (in the band below), 660 MHz at 83 C (above the target). No level
     * holds the target, so the loop cycles, and it cycles between those two levels only. */
    A830DcvsConfig c;c.acd=true;c.thermalTargetDeci=800;
    boot(c);
    gHeat=HeatModel{};gHeat.steady[7]=73;gHeat.steady[8]=83;
    steps(100,0,false);
    HeatTrace warmup,cycle;
    for(unsigned i=0;i<1875;++i)heatStep(1000,&warmup);
    for(unsigned i=0;i<7500;++i)heatStep(1000,&cycle);       /* 2 minutes */
    if(gTraceHeat)std::fprintf(stderr,"straddling levels: ceiling %u-%u %.1f-%.1f C, %u changes\n",cycle.minCeiling,
                               cycle.maxCeiling,cycle.minC,cycle.maxC,cycle.changes);
    CHECK(cycle.minCeiling==7 && cycle.maxCeiling==8 && cycle.changes>=4 && cycle.minC>=72.0 && cycle.maxC<=83.5);
    ++heatCases;
}

/* The discrete rules on exact readings: down per trend window above the target while rising or steady, held while falling,
 * held in the 5 C band, never below the floor; up one level per 2 s of the band below; the emergency; unreadable sensors. */
static void testThermalRules()
{
    A830DcvsConfig c;c.acd=true;c.thermalTargetDeci=800;   /* the rules on an 80 C target (the default is 95 C) */
    boot(c);
    steps(100,0,false);
    CHECK(gDcvs.ceiling()==11 && gDcvs.stats().trend==0);
    /* 90 C, steady: one level per trend window (32 windows of 16 ms), 900 -> 832 -> 734 MHz. */
    const uint32_t downs=gDcvs.stats().thermalDowns;
    temperature(900);
    unsigned n=0;
    while(gDcvs.ceiling()==11 && n<40) {step(950);++n;}
    CHECK(gDcvs.ceiling()==10 && n<=33 && gDcvs.stats().trend==1);
    steps(31,950);
    CHECK(gDcvs.ceiling()==10);
    steps(2,950);
    CHECK(gDcvs.ceiling()==9 && gDcvs.stats().thermalDowns==downs+2 && gDcvs.stats().trend==0 && gDcvs.level()==9);
    /* Falling (90 -> 82 C over 4 s, 1 C per trend window): the ceiling holds; steady again at 82 C it goes on down to the
     * 525 MHz floor (g6) and stays there however hot it is outside an emergency. */
    ramp(900,820,250);
    CHECK(gDcvs.ceiling()==9 && gDcvs.stats().trend==-1);
    steps(32*3+8,950);
    CHECK(gDcvs.ceiling()==6);
    temperature(940);
    steps(32*10,950);
    CHECK(gDcvs.ceiling()==6 && gDcvs.level()==6 && !gDcvs.stats().emergency && gDcvs.stats().limiting);
    /* The band (75-80 C): neither up nor down. */
    temperature(780);
    steps(32*10,950);
    CHECK(gDcvs.ceiling()==6);
    /* At least 5 C under the target: up one level per 2 s (125 windows) of it, the dwell starting when the filtered reading
     * entered the band below. */
    const uint32_t ups=gDcvs.stats().thermalUps;
    temperature(740);
    steps(120,950);
    CHECK(gDcvs.ceiling()==6);
    steps(15,950);
    CHECK(gDcvs.ceiling()==7 && gDcvs.stats().thermalUps==ups+1);
    steps(110,950);
    CHECK(gDcvs.ceiling()==7);
    steps(20,950);
    CHECK(gDcvs.ceiling()==8);
    /* Leaving the band below restarts the dwell. */
    temperature(770);
    steps(60,950);
    temperature(740);
    steps(120,950);
    CHECK(gDcvs.ceiling()==8);
    steps(15,950);
    CHECK(gDcvs.ceiling()==9);
    /* Emergency: 105 C takes 389 MHz (g4) in the same window, whatever the trend; it holds through the 5 C hysteresis; below
     * 100 C the ceiling returns to the floor one level per 500 ms, then the loop goes on from there. */
    temperature(1055);
    step(950);
    CHECK(gDcvs.ceiling()==4 && gDcvs.level()==4 && gDcvs.stats().emergencies==1 && gDcvs.stats().zone==Sm8750ThermalCritical);
    temperature(1005);
    steps(32*8,950);
    CHECK(gDcvs.ceiling()==4 && gDcvs.level()==4);
    temperature(995);
    step(950);
    CHECK(gDcvs.ceiling()==4);
    steps(32,950);
    CHECK(gDcvs.ceiling()==5);
    steps(32,950);
    CHECK(gDcvs.ceiling()==6);
    steps(32*4,950);
    CHECK(gDcvs.ceiling()==6 && gDcvs.stats().emergencies==1);
    temperature(600);
    steps(130,950);
    CHECK(gDcvs.ceiling()==7);
    steps(125*4,950);
    CHECK(gDcvs.ceiling()==11 && !gDcvs.stats().limiting);
    steps(24,950);   /* the levels above 734 MHz at one per 12 saturated windows (the sustain rule) */
    CHECK(gDcvs.level()==11);
    ++heatCases;
    /* Readings lost for good (Sm8750ThermalInvalidLimit windows): 607 MHz while they are; back cool: the top at once. */
    for(unsigned s=1;s<=8;++s)gTsens[s]&=~(1u<<21);
    steps(7,950);
    CHECK(gDcvs.ceiling()==11);
    steps(2,950);
    CHECK(gDcvs.ceiling()==7 && gDcvs.stats().zone==Sm8750ThermalUnknown);
    steps(300,950);
    CHECK(gDcvs.ceiling()==7 && gDcvs.level()==7);
    temperature(500);
    step(950);
    CHECK(gDcvs.ceiling()==11);
    /* Readings lost right after an emergency: the zone keeps Critical through the first 7 invalid windows, then the ceiling
     * climbs back to the 607 MHz unreadable cap one level per 500 ms instead of staying at 389 MHz. */
    boot(c);
    steps(100,0,false);
    temperature(1060);
    step(950);
    CHECK(gDcvs.ceiling()==4);
    for(unsigned s=1;s<=8;++s)gTsens[s]&=~(1u<<21);
    steps(8,950);
    CHECK(gDcvs.ceiling()==4 && gDcvs.stats().zone==Sm8750ThermalUnknown && !gDcvs.stats().emergency);
    steps(32*3+2,950);
    CHECK(gDcvs.ceiling()==7);
    steps(100,950);
    CHECK(gDcvs.ceiling()==7 && gDcvs.level()==7);
    /* A warm first reading (78 C) leaves the start-up cap (607 MHz) in place: the loop works from it. */
    boot(c);
    temperature(780);
    steps(32*10,950);
    CHECK(gDcvs.ceiling()==7);
    temperature(830);
    steps(65,950);
    CHECK(gDcvs.ceiling()==6);
    /* No sensors at all, or sensors that never report VALID: 607 MHz for good, whatever the load. */
    boot(A830DcvsConfig{},false);
    steps(100,0,false);
    steps(200,1000);
    CHECK(gDcvs.ceiling()==7 && gDcvs.level()==7 && gTsensReads==0 && !gDcvs.stats().limiting);
    boot();
    for(unsigned s=1;s<=8;++s)gTsens[s]&=~(1u<<21);
    steps(100,0,false);
    steps(200,1000);
    CHECK(gDcvs.ceiling()==7 && gDcvs.stats().zone==Sm8750ThermalUnknown);
    ++heatCases;
    /* pagani-gpu-ttarget: 70 C moves the whole loop down by 10 C. */
    A830DcvsConfig cool=c;cool.thermalTargetDeci=700;
    boot(cool);
    steps(100,0,false);
    CHECK(gDcvs.ceiling()==11);
    temperature(720);
    steps(33,950);
    CHECK(gDcvs.ceiling()==10);
    temperature(660);
    steps(32*8,950);
    CHECK(gDcvs.ceiling()==10);
    temperature(640);
    steps(135,950);
    CHECK(gDcvs.ceiling()==11);
    /* The heat model under full load with an 85 C target: the loop holds one level higher than at 80 C (660 MHz, 81 C). At 70 C
     * the 525 MHz floor (72 C at full load) would win: the floor is kept outside an emergency whatever the target. */
    A830DcvsConfig warm=c;warm.thermalTargetDeci=850;
    boot(warm);
    gHeat=HeatModel{};
    steps(100,0,false);
    HeatTrace settle,held;
    for(unsigned i=0;i<1875;++i)heatStep(1000,&settle);
    for(unsigned i=0;i<3750;++i)heatStep(1000,&held);
    if(gTraceHeat)std::fprintf(stderr,"target 85: held ceiling %u-%u %.1f-%.1f C\n",held.minCeiling,held.maxCeiling,held.minC,
                               held.maxC);
    CHECK(held.maxCeiling-held.minCeiling<=1 && held.minCeiling>=8 && held.minC>=80.0 && held.maxC<=86.5);
    boot(cool);
    gHeat=HeatModel{};
    steps(100,0,false);
    HeatTrace floor70;
    for(unsigned i=0;i<1875;++i)heatStep(1000);
    for(unsigned i=0;i<1875;++i)heatStep(1000,&floor70);
    CHECK(floor70.minCeiling==6 && floor70.maxCeiling==6 && floor70.maxC<=72.5 && gDcvs.stats().emergencies==0);
    ++heatCases;
}

/* Under thermal pressure (the filtered reading within 5 C of the target or above it) the governor's up rules step one level
 * per window; below that band the 90 % rule still jumps two levels and demand x 1.25. */
static void testThermalPressure()
{
    A830DcvsConfig p;p.thermalTargetDeci=800;   /* the band under an 80 C target (the default is 95 C) */
    boot(p);
    settle();
    step(990);
    CHECK(gDcvs.level()==3 && gDcvs.stats().pressureSteps==0);   /* cool: +2 and demand x 1.25 (g1 at 99 %: 283 MHz) */
    boot(p);
    settle();
    temperature(760);   /* 76 C: inside the 5 C under the target */
    steps(40,100);
    CHECK(gDcvs.level()==1 && gDcvs.ceiling()==9 && gDcvs.thermalCeiling()==11);
    const uint32_t cut=gDcvs.stats().pressureSteps;
    const uint8_t climb[]={2,3,4,5,6,7};
    for(const uint8_t want:climb) {step(990);CHECK(gDcvs.level()==want);}
    CHECK(gDcvs.stats().pressureSteps==cut+6);
    /* The demand rule as well: 88 % at g7 asks for 734 MHz (demand x 1.1); it gets one level, then the next. */
    step(880);
    CHECK(gDcvs.level()==8);
    step(880);
    CHECK(gDcvs.level()==9);
    /* Back under the band (74 C filtered): the jumps return. */
    boot(p);
    settle();
    temperature(740);
    steps(40,100);
    step(990);
    CHECK(gDcvs.level()==3);
    ++heatCases;
}

/* A load given as work, the frequency it would saturate (busy x MHz of the 2026-10-31 run: median 182, p90 466, p99 630 in the seconds
 * with frames): the busy of a window follows the level the GPU runs at, as on the device, unlike the fixed busy traces above. The
 * device's configuration: ACD on, 900 MHz at the top. */
static uint32_t busyOf(uint32_t workMhz)
{
    const uint64_t busy=uint64_t(workMhz)*1000000u/gDcvs.khz(gDcvs.level());
    return busy<1000?uint32_t(busy):1000u;
}
static void work(unsigned n,uint32_t workMhz) {for(unsigned i=0;i<n;++i)step(busyOf(workMhz));}
static unsigned compositingCases;

static void testCompositing()
{
    A830DcvsConfig c;c.acd=true;
    boot(c);steps(100,0,false);
    CHECK(gDcvs.ceiling()==11 && gDcvs.level()==1);
    /* Steady desktops: the level that holds the load at 75 % or less, no higher than the up rules' 1.1 x demand asks: 182 MHz of work at
     * 342 MHz, 300 at 443, 470 at 734 (before 2026-10-31: about 1.8 x the work, 470 at 832-900 MHz with the GPU's DDR vote at level 10);
     * the DT bus goes with it. */
    work(200,182);
    CHECK(gDcvs.level()==3);
    work(200,300);
    CHECK(gDcvs.level()==5 && gDcvs.bus()==4);
    work(200,470);
    CHECK(gDcvs.level()==9 && gDcvs.bus()==8);
    /* And down again. */
    work(200,182);
    CHECK(gDcvs.level()==3);
    /* A burst does not strand a load above the sustain level: 470 and 520 MHz of work go back to the 734 MHz that holds them under the
     * target, which the 0.85 margin alone would not do (demand 627 > 624), and a load 734 MHz cannot hold at the target keeps 832. */
    for(const uint32_t load:{470u,520u}) {
        work(60,1000);
        CHECK(gDcvs.level()==11);
        work(40,load);
        CHECK(gDcvs.level()==9);
    }
    work(60,1000);
    work(40,600);
    CHECK(gDcvs.level()==10);
    work(200,182);
    CHECK(gDcvs.level()==3);
    /* A burst in a desktop: three saturated windows stop at 734 MHz (a jump, not one level per window: the frame is late now), and
     * the level is back in 700 ms. */
    work(100,200);
    uint8_t top=0;
    for(unsigned i=0;i<3;++i) {step(1000);if(gDcvs.level()>top)top=gDcvs.level();}
    CHECK(top==9 && gDcvs.stats().sustainHolds>=1);
    work(42,200);
    CHECK(gDcvs.level()==3);
    /* A load that needs it: 734 MHz at once, 832 after 12 saturated windows there, 900 after 12 more. */
    const unsigned windows[]={3,12,12};
    const uint8_t levels[]={9,10,11};
    for(unsigned i=0;i<3;++i) {work(windows[i],1000);CHECK(gDcvs.level()==levels[i]);}
    work(100,1000);
    CHECK(gDcvs.level()==11);
    /* A busy window less restarts the count. */
    work(200,182);
    CHECK(gDcvs.level()==3);
    work(3,1000);
    CHECK(gDcvs.level()==9);
    work(10,1000);step(800);work(11,1000);
    CHECK(gDcvs.level()==9);
    work(1,1000);
    CHECK(gDcvs.level()==10);
    /* Late frames in a row: one level above the governor's, not one more each (they are mostly the CPU's), and never past the sustain
     * limit. */
    work(200,300);
    CHECK(gDcvs.level()==6);
    for(unsigned i=0;i<30;++i) {adreno830_dcvs_late_frame(4000);work(1,300);CHECK(gDcvs.level()<=7);}
    work(10,300);
    CHECK(gDcvs.level()==6);
    work(200,620);
    CHECK(gDcvs.level()==9);
    for(unsigned i=0;i<20;++i) {adreno830_dcvs_late_frame(4000);work(1,620);CHECK(gDcvs.level()==9);}
    /* A wake boost that ends: the load's level, not the floor and the saturated windows after it. */
    steps(10,0,false);
    CHECK(gDcvs.level()==1);
    work(1,300);
    uint8_t low=255;
    for(unsigned i=0;i<20;++i) {work(1,300);if(gDcvs.level()<low)low=gDcvs.level();}
    CHECK(low>=5 && gDcvs.level()==5);
    /* An explicit pagani-gpu-dcvs-max stays the hard limit. */
    A830DcvsConfig capped=c;capped.maxMhz=660;capped.maxGiven=true;
    boot(capped);steps(100,0,false);
    work(100,1000);
    CHECK(gDcvs.ceiling()==8 && gDcvs.level()==8);
    /* The defaults before 2026-10-31, what this rule replaced: one saturated window at 525 MHz asked for the top. */
    A830DcvsConfig before=c;before.targetBusyPermille=700;before.downMarginPermille=800;before.sustainKhz=0;
    boot(before);steps(100,0,false);
    work(1,300);
    CHECK(gDcvs.level()==6);
    step(1000);
    CHECK(gDcvs.level()==11);
    compositingCases+=11;
}

/* What a window's busy does not show: at 120 Hz a window holds two frames of 8.3 ms, and a frame longer than that is late however the window
 * averages out. Frames alternate 30 % over and 30 % under the load's mean; the GPU runs what fits into each TE at the level of the window
 * and carries the rest, and the window's busy is what it really ran, so the governor sees the carry too (the model of the review of
 * 2026-11-01, without its spikes). Every load up to what 734 MHz holds at the target settles with no late frame and no vote: the margin
 * of a 75 % target is what those 30 % take. The same sweep at 85 % / 0.9, the first choice of 2026-10-31, has late frames: this test tells
 * the two apart, which busy alone does not. */
static double gCarry;
static unsigned frames(unsigned windows,uint32_t workMhz)
{
    unsigned late=0;
    for(unsigned i=0;i<windows;++i) {
        double ran=0;
        for(unsigned frame=0;frame<2;++frame) {
            const double capacity=gDcvs.khz(gDcvs.level())*1e3/120;
            const double load=gCarry+workMhz*1e6/120*(frame?0.7:1.3);
            const double done=load<capacity?load:capacity;
            gCarry=load-done;
            ran+=done/capacity;
            if(gCarry>0)++late;
        }
        step(uint32_t(ran*500),true,16666666ull);
    }
    return late;
}
static unsigned headroomCases;
static unsigned lateLoads(const A830DcvsConfig &config,bool down)
{
    boot(config);steps(100,0,false);
    gCarry=0;
    unsigned tight=0;
    const uint32_t first=down?540:60,last=down?60:540;
    for(uint32_t load=first;;load=down?load-6:load+6) {
        (void)frames(300,load);
        const size_t votes=gOps.votes.size();
        if(frames(120,load))++tight;
        CHECK(gOps.votes.size()==votes);   /* settled: the rules do not hand the level back and forth */
        ++headroomCases;
        if(load==last)break;
    }
    return tight;
}
static void testFrameHeadroom()
{
    A830DcvsConfig c;c.acd=true;
    CHECK(lateLoads(c,false)==0 && lateLoads(c,true)==0);
    A830DcvsConfig thin=c;thin.targetBusyPermille=850;thin.downMarginPermille=900;
    CHECK(lateLoads(thin,false)>0);
    /* Past what 734 MHz holds at the target the sustain rule keeps the level (75-90 % busy) and spends the margin: heat against late frames. */
    boot(c);steps(100,0,false);
    gCarry=0;
    (void)frames(300,620);
    CHECK(gDcvs.level()==9 && frames(120,620)>0);
    ++headroomCases;
}

/* What PaganiArmCpu's governor reads: adreno830_thermal_state (deci-C and A830Thermal* flags: a usable reading, the thermal
 * ceiling holding the GPU below its knob/ACD ceiling, the emergency; 0 while asleep, lost or before S10) and adreno830_cpu_hint
 * (the accepted submissions). The DCVS row's thermal field is stored once a second in the design profile only. */
static void testThermalHooks()
{
    resetAll();
    {
        uint32_t flags=7;
        A830Dcvs idle;
        CHECK(idle.thermalState(&flags)==0 && flags==0);
    }
    A830DcvsConfig c;c.acd=true;c.thermalTargetDeci=800;
    boot(c);
    uint32_t flags=0;
    steps(100,0,false);
    CHECK(adreno830_thermal_state(&flags)==450 && flags==A830ThermalValid && adreno830_thermal_state(nullptr)==450);
    temperature(900);
    steps(33,950);
    CHECK(adreno830_thermal_state(&flags)==900 && flags==(A830ThermalValid|A830ThermalLimiting));
    temperature(1062);
    step(950);
    CHECK(adreno830_thermal_state(&flags)==1062 && flags==(A830ThermalValid|A830ThermalLimiting|A830ThermalEmergency));
    /* Status rows once a second: target 80 C, the trend code, the emergency flag. */
    steps(70,950);
    CHECK(!gStatus.thermal.empty() && gStatus.thermal.back()[0]==800 && gStatus.thermal.back()[2]==int32_t(A830StatusThermalEmergency));
    temperature(450);
    steps(70,950);
    CHECK(gStatus.thermal.back()[2]==0 && gStatus.thermal.back()[1]==int32_t(A830StatusTrendFalling));
    steps(200,950);
    CHECK(gStatus.thermal.back()[1]==int32_t(A830StatusTrendSteady));
    /* Asleep (slumber): no reading, no limit for the CPU; the last temperature stays. */
    A830PowerConfig keep;keep.slumber=A830SlumberCxKeep;
    boot(c,true,keep);
    temperature(900);
    steps(10,500);
    steps(33,500);
    CHECK(adreno830_thermal_state(&flags)==900 && (flags&A830ThermalLimiting));
    steps(40,0,false);
    CHECK(gPower.phase()==A830PowerSlumber && adreno830_thermal_state(&flags)==900 && flags==0);
    /* No usable reading: no flag at all (the 607 MHz cap is not heat). */
    boot(c,false);
    steps(50,950);
    CHECK(gDcvs.ceiling()==7 && adreno830_thermal_state(&flags)==0 && flags==0);
    /* The annibale profile: its 160 MHz hold is a limit as well; it stores no DCVS-row field. */
    adreno830_profile_value=A830ProfileAnnibale;
    A830DcvsConfig a;a.initialLevel=1;a.initialBus=2;
    boot(a);
    steps(70,950);
    CHECK(adreno830_thermal_state(&flags)==450 && flags==A830ThermalValid && gStatus.thermal.empty());
    temperature(720);
    step(950);
    CHECK(adreno830_thermal_state(&flags)==720 && flags==(A830ThermalValid|A830ThermalLimiting));
    adreno830_profile_value=A830ProfileDesign;
    /* adreno830_cpu_hint: the accepted submissions as the status block counts them. */
    adreno830_status.submits=0x100000005ull;
    CHECK(adreno830_cpu_hint()==5u);
    adreno830_status.submits=0x100000006ull;
    CHECK(adreno830_cpu_hint()==6u);
    ++heatCases;
}

static void testFixed()
{
    A830DcvsConfig c;c.governor=false;c.fixedLevel=9;
    boot(c);
    /* S10: one vote of pagani-gpu-perf, under the start-up thermal cap (unknown zone: 607 MHz). */
    CHECK(gOps.votes.size()==1 && gOps.votes[0].first==7 && gDcvs.level()==7);
    steps(100,1000);
    CHECK(gDcvs.level()==9 && gOps.votes.size()==2);   /* g9 once the first reading (45 C) lifts the thermal ceiling */
    steps(100,0,false);
    CHECK(gDcvs.level()==9);   /* no idle drop in fixed mode */
    temperature(1070);
    step(500);
    CHECK(gDcvs.level()==4);
    /* pagani-gpu-perf above the ACD ceiling is clamped. */
    c.fixedLevel=11;
    boot(c);steps(200,100);
    CHECK(gDcvs.level()==9);
}

static void testVotesAndSleep()
{
    boot();
    settle();
    gOps.fail["perfVote"]=kIOReturnTimeout;
    const unsigned attempts=gOps.count("perfVote");
    step(880);
    CHECK(gDcvs.level()==1 && gDcvs.stats().voteFailures==1 && adreno830_status.vote_failures==1);
    /* Retried after 1 s, not before. */
    steps(30,880);
    CHECK(gDcvs.level()==1 && gOps.count("perfVote")==attempts+1);
    steps(40,880);
    CHECK(gDcvs.level()>1 && adreno830_status.votes==gDcvs.stats().votes);
    /* GMU asleep (CX-keeping slumber): no register read, the target only recorded for HFI M9. */
    A830PowerConfig keep;keep.slumber=A830SlumberCxKeep;
    boot(A830DcvsConfig{},true,keep);
    steps(10,500);
    steps(30,0,false);   /* 250 ms quiet: slumber */
    CHECK(gPower.phase()==A830PowerSlumber && gPower.targetLevel()==1);
    CHECK(gDcvs.serviceTick(gEnv.now)<=gEnv.now+WindowNs);   /* asleep, the touch counter is still read every window */
    gOps.calls.clear();
    pagani_input_touch_events=pagani_input_touch_events+1;   /* pre-wake at the touch level (g6, bus 4) */
    gEnv.now+=1000000;
    gDcvs.serviceTick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && gOps.lastArgs.level==6 && gOps.lastArgs.bus==4 && gDcvs.level()==6);
    /* Asleep again; target changes are recorded without a vote and without MMIO. */
    steps(40,0,false);
    CHECK(gPower.phase()==A830PowerSlumber);
    const size_t quiet=gHw.log.size(),quietVotes=gOps.votes.size();
    steps(100,0,false);
    CHECK(gHw.log.size()==quiet && gOps.votes.size()==quietVotes && gPower.targetLevel()==1);
    /* The ring's queue period is still taken once a second while asleep (the row then reads " | q -"). */
    CHECK(gOps.queueRows>=2 && !gOps.queueAwake.back() && gOps.queueRows==gOps.memoryRows);
}

static void testTe()
{
    boot();
    settle();
    const uint32_t windows=gDcvs.stats().windows;
    /* 120 Hz TEs: a window every two TEs (16.7 ms), even when the thread wakes earlier. */
    for(unsigned i=0;i<20;++i) {
        gEnv.now+=8333333;
        gHw.busyTicks+=uint64_t(A830_XO_HZ)*8333333/1000000000*500/1000;
        adreno830_dcvs_vsync();
        gDcvs.noteSubmit();++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;
        gDcvs.serviceTick(gEnv.now);
    }
    CHECK(gDcvs.stats().windows-windows==10 && gEnv.wakes>=10);
    /* TEs stop: back to the 16 ms timer after 50 ms. */
    const uint32_t before=gDcvs.stats().windows;
    steps(10,500);
    CHECK(gDcvs.stats().windows-before>=7);
}

static void testUnderruns()
{
    A830DcvsConfig c;c.hostFloorKbps=kDdr[3];c.hostBootKbps=kDdr[11];
    boot(c);
    settle();
    steps(10,880);
    CHECK(gDcvs.level()==9 && gDcvs.bus()==8);
    adreno830_dcvs_underruns(1);
    steps(70,880);
    /* First growth: host DDR floor up to level 6; bus unchanged. */
    CHECK(gOps.hostDdr.size()==1 && gOps.hostDdr[0]==kDdr[6] && gDcvs.bus()==8 && adreno830_status.underruns==1);
    adreno830_dcvs_underruns(3);
    steps(70,880);
    /* Second consecutive growth: bus-max (g9: 10). */
    CHECK(gDcvs.bus()==10 && gDcvs.stats().busMaxEntries==1 && gOps.votes.back().second==(0xffff0000u|10));
    /* 10 s without growth: bus back, floor back to pagani-gpu-ddr-floor. */
    steps(700,880);
    CHECK(gDcvs.bus()==8 && gOps.hostDdr.back()==kDdr[3]);
    /* Default floor (= boot level 11 >= level 6): no host vote at all. */
    A830DcvsConfig standard;standard.hostFloorKbps=standard.hostBootKbps=kDdr[11];
    boot(standard);
    settle();
    adreno830_dcvs_underruns(5);
    steps(70,500);
    CHECK(gOps.hostDdr.empty());
}

static void testServiceAndStatus()
{
    boot();
    gOps.lastSubmitted=10;gOps.retired=10;
    settle();
    /* Once a second: DCVS row with residency (levels 1..11), memory row, fault poll, and the ring's queue period stored
     * after the memory row and before the DCVS row publishes it. */
    CHECK(gStatus.dcvs.size()>=1 && gStatus.memory.size()>=1 && gOps.polls>=1);
    CHECK(gOps.queueRows==gStatus.memory.size() && gOps.queueRows==gStatus.dcvs.size());
    for(size_t i=0;i<gOps.queueOrder.size();++i)CHECK(gOps.queueOrder[i].first==i+1 && gOps.queueOrder[i].second==i);
    const auto &row=gStatus.dcvs.back();
    CHECK(row[4]==160 && row[5]==734 && row[6]==1 && row[7]==11 && gStatus.residency.size()==11);
    steps(700,0,false);
    unsigned sum=0;
    for(uint16_t permille:gStatus.residency)sum+=permille;
    /* The first 10 s window also holds the start (g2) and the wake boost (g6) of settle(). */
    CHECK(sum>=990 && sum<=1000 && gStatus.residency[0]>=980 && gStatus.residency[5]>0);
    /* Watchdog cadence: work in flight without progress -> a recovery about 2 s later, run by the service thread. */
    gOps.lastSubmitted=12;gOps.retired=11;
    steps(160,800,false);
    CHECK(gRecovery.stats().recoveries==1 && gRecovery.stats().watchdogHangs==1);
    /* A reported fault is recovered by the next service tick. */
    adreno830_recovery_report(A830FaultCpHwError,0);
    step(100);
    CHECK(gRecovery.stats().recoveries==2 && !gRecovery.pending());
    /* The deadline never lies in the past. */
    CHECK(gDcvs.serviceTick(gEnv.now)>gEnv.now);
    /* The thread starts once through the environment. */
    resetAll();
    A830PowerState p;A830Dcvs d;
    CHECK(p.init(A830PowerConfig{},gIo,gThreadEnv,gPowerOps)==kIOReturnSuccess);
    p.start(2,2);
    CHECK(d.init(A830DcvsConfig{},kTable,gIo,gThreadEnv,gDcvsOps,&p,nullptr,nullptr)==kIOReturnSuccess);
    CHECK(d.start()==kIOReturnSuccess && gEnv.threads==1 && d.start()==kIOReturnBusy);
}

/* The annibale profile (A830Profile.h; kernel-sequences 13, section 14 rows 24 and 38): the governor of the cases above,
 * under a 342 MHz ceiling that only an explicit pagani-gpu-dcvs-max lifts; the 70/60 C rule (160 MHz while any sensor is
 * at 70 C or no usable reading exists, back at once below 60 C) instead of the 85/95 C caps; bw without 0xFFFF0000; and a
 * vote the GMU does not acknowledge ends the changes for the boot. */
static void testAnnibale()
{
    adreno830_profile_value=A830ProfileAnnibale;
    A830DcvsConfig c;c.initialLevel=1;c.initialBus=2;   /* the {1, 1, 2} first vote of S5 */
    boot(c);
    CHECK(gDcvs.stats().annibale && gDcvs.ceiling()==3 && gDcvs.level()==1);   /* cool until read: only the 342 MHz knob ceiling */
    steps(20,950);
    CHECK(gDcvs.level()==3 && gDcvs.ceiling()==3 && !gDcvs.stats().thermalHot);
    CHECK(gOps.votes.back().first==3 && gOps.votes.back().second==3u);   /* g3, bw = its bus level 3 alone */
    for(const auto &vote:gOps.votes)CHECK(!(vote.second&0xffff0000u));
    pagani_input_touch_events=pagani_input_touch_events+1;   /* the 607 MHz touch boost stays under 342 MHz */
    step(100);
    CHECK(gDcvs.level()==3 && gDcvs.stats().touchBoosts==1);
    steps(70,500);
    CHECK(gStatus.dcvs.size()>=1 && gStatus.dcvs.back()[4]==160 && gStatus.dcvs.back()[5]==342);
    /* pagani-gpu-dcvs-max lifts it (still 734 MHz at most with ACD off); ACD on without it: still 342 MHz. */
    A830DcvsConfig lifted=c;lifted.maxGiven=true;lifted.maxMhz=607;
    boot(lifted);steps(20,950);
    CHECK(gDcvs.ceiling()==7 && gDcvs.level()==7);
    lifted.maxMhz=900;
    boot(lifted);steps(30,950);
    CHECK(gDcvs.ceiling()==9 && gDcvs.level()==9);
    A830DcvsConfig acd=c;acd.acd=true;
    boot(acd);steps(20,950);
    CHECK(gDcvs.ceiling()==3);
    /* Thermal: 70.0 C -> 160 MHz at once; 65 C still held; below 60 C the ceiling is back at once (no 500 ms steps). */
    boot(c);steps(20,950);
    temperature(700);
    step(950);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1 && gDcvs.stats().thermalHot && gDcvs.stats().thermalCaps==1);
    temperature(650);
    steps(40,950);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1);
    temperature(599);
    step(950);
    CHECK(gDcvs.ceiling()==3 && !gDcvs.stats().thermalHot);
    step(950);
    CHECK(gDcvs.level()==3);
    temperature(699);   /* 69.9 C: not hot */
    steps(5,950);
    CHECK(gDcvs.ceiling()==3);
    temperature(900);   /* the design's 85 C cap would be 607 MHz; annibale holds 160 MHz */
    step(950);
    CHECK(gDcvs.ceiling()==1 && gDcvs.stats().zone==Sm8750ThermalHot);
    /* No usable reading: no sensors at all (from the first window); sensors that lose VALID for good (once the zone goes
     * Unknown, after Sm8750ThermalInvalidLimit windows; a short gap keeps the last reading). */
    boot(c,false);
    steps(5,950);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1 && gTsensReads==0 && gDcvs.stats().thermalHot);
    boot(c);steps(20,950);
    for(unsigned n=1;n<=8;++n)gTsens[n]&=~(1u<<21);
    steps(4,950);
    CHECK(gDcvs.ceiling()==3 && gDcvs.level()==3);
    steps(10,950);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1 && gDcvs.stats().zone==Sm8750ThermalUnknown);
    /* An unacknowledged vote ends the changes: no retry, the target stays the level the GMU has. */
    boot(c);
    gOps.fail["perfVote"]=kIOReturnTimeout;
    steps(3,950);
    CHECK(gDcvs.level()==1 && gDcvs.stats().voteFailures==1 && gDcvs.stats().votesStopped && gPower.targetLevel()==1 &&
          gPower.targetBus()==2 && adreno830_status.vote_failures==1);
    const unsigned attempts=gOps.count("perfVote");
    steps(200,950);
    temperature(800);steps(10,950);temperature(450);steps(10,950);
    CHECK(gOps.count("perfVote")==attempts && gDcvs.level()==1 && gPower.targetLevel()==1);
    /* Fixed mode (pagani-gpu-dcvs=0): pagani-gpu-perf under the 342 MHz ceiling, voted at once. */
    A830DcvsConfig fixed=c;fixed.governor=false;fixed.fixedLevel=7;
    boot(fixed);
    CHECK(gOps.votes.size()==1 && gOps.votes[0].first==3 && gOps.votes[0].second==3u && gDcvs.level()==3);
    adreno830_profile_value=A830ProfileDesign;
}

/* pagani-gpu-maxlevel (A830DcvsConfig::maxLevel; in the annibale profile S0 then hands over the whole DT table, as kTable
 * here): the knob ceiling is GMU level n, beyond the table its top; an explicit pagani-gpu-dcvs-max lowers it; the ACD-off
 * 734 MHz ceiling and the thermal rules stay. The governor is the one above: at 99 % busy 160 -> 342 -> 607 -> 734 MHz in
 * three windows, every vote with the level's DT bus (alone, annibale), the 70/60 C hold of the annibale profile intact. */
static unsigned maxLevelCases;
static void testMaxLevel()
{
    adreno830_profile_value=A830ProfileAnnibale;
    A830DcvsConfig c;c.initialLevel=1;c.initialBus=2;c.maxLevel=9;
    boot(c);
    CHECK(gDcvs.ceiling()==9 && gDcvs.level()==1);
    step(990);   /* the first window only takes the busy sample */
    CHECK(gDcvs.level()==1);
    const uint8_t ramp[][2]={{3,3},{7,6},{9,8}};   /* GMU level, its DT bus */
    for(const auto &want:ramp) {
        step(990);
        CHECK(gDcvs.level()==want[0] && gDcvs.bus()==want[1]);
        CHECK(gOps.votes.back().first==want[0] && gOps.votes.back().second==uint32_t(want[1]));   /* bus alone, no 0xFFFF0000 */
    }
    steps(70,990);
    CHECK(gDcvs.level()==9 && gStatus.dcvs.size()>=1 && gStatus.dcvs.back()[4]==160 && gStatus.dcvs.back()[5]==734);
    /* The annibale thermal hold stays: 70 C -> 160 MHz at once, below 60 C the ceiling level 9 is back at once. */
    temperature(700);
    step(990);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1 && gDcvs.stats().thermalHot);
    temperature(599);
    step(990);step(990);
    CHECK(gDcvs.ceiling()==9 && gDcvs.level()>=3 && !gDcvs.stats().thermalHot);
    /* Ceilings: a lower level; beyond the table its top, under the ACD-off 734 MHz until ACD runs; dcvs-max lowers it. */
    c.maxLevel=5;boot(c);steps(30,990);
    CHECK(gDcvs.ceiling()==5 && gDcvs.level()==5);
    c.maxLevel=16;boot(c);steps(30,990);
    CHECK(gDcvs.ceiling()==9 && gDcvs.level()==9);
    c.acd=true;boot(c);steps(30,990);
    CHECK(gDcvs.ceiling()==11 && gDcvs.level()==11);
    gDcvs.setAcd(false);
    CHECK(gDcvs.ceiling()==9);
    c.acd=false;c.maxLevel=11;c.maxGiven=true;c.maxMhz=607;boot(c);steps(30,990);
    CHECK(gDcvs.ceiling()==7 && gDcvs.level()==7);
    /* Fixed mode (pagani-gpu-dcvs=0) under the level ceiling. */
    A830DcvsConfig fixed;fixed.initialLevel=1;fixed.initialBus=2;fixed.maxLevel=9;fixed.governor=false;fixed.fixedLevel=11;
    boot(fixed);
    CHECK(gOps.votes.size()==1 && gOps.votes[0].first==9 && gOps.votes[0].second==8u && gDcvs.level()==9);
    /* Without the knob the annibale ceiling is 342 MHz again. */
    c=A830DcvsConfig{};c.initialLevel=1;c.initialBus=2;boot(c);steps(30,990);
    CHECK(gDcvs.ceiling()==3 && gDcvs.level()==3);
    /* The design profile: n is its ceiling too (the unknown-zone thermal cap of 607 MHz first, then level n). */
    adreno830_profile_value=A830ProfileDesign;
    A830DcvsConfig d;d.maxLevel=7;boot(d);steps(100,0,false);
    CHECK(gDcvs.ceiling()==7);
    d.maxLevel=16;boot(d);steps(100,0,false);
    CHECK(gDcvs.ceiling()==9);
    maxLevelCases+=9;
}

/* park() before a timed reset: from the next tick the lowest level and its bus, whatever the load, the touch or the wake
 * boost asks for, and for the rest of the boot; the C hook reaches the running governor. */
/* The screen rule through the service thread (pagani-screenoff bit 1): the thread reads PaganiPanel's pagani_screen_state each
 * tick; with the display off the GPU sleeps after its quiet second and the thread then wakes only for its status period (no
 * touch polling every window); touches on the dark display neither boost nor wake; a refresh tick of the display (it ticks only
 * while on) wakes the thread, which wakes the GPU at the touch level once the word says on; without the rule the word changes
 * nothing. */
static void testScreen()
{
    A830PowerConfig rule;rule.screenSlumber=A830SlumberCxKeep;
    boot(A830DcvsConfig{},true,rule);
    settle();
    pagani_screen_state=0;
    steps(200,0,false);
    CHECK(gPower.phase()==A830PowerActive);   /* P0 with the display on: never asleep */
    pagani_screen_state=1;                    /* off (one change) */
    steps(55,0,false);
    CHECK(gPower.phase()==A830PowerActive && gPower.screenOff());
    steps(10,0,false);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.screenSlept() && gPower.stats().screenEntries==1);
    /* Asleep and dark: the next deadline is the status period, not the next window. */
    const uint64_t next=gDcvs.serviceTick(gEnv.now);
    CHECK(next>gEnv.now+WindowNs);
    /* Dark touches: no boost, no wake, not counted. */
    const uint32_t touches=gDcvs.stats().touches,boosts=gDcvs.stats().touchBoosts;
    pagani_input_touch_events=pagani_input_touch_events+3;
    steps(20,0,false);
    CHECK(gPower.phase()==A830PowerSlumber && gDcvs.stats().touches==touches && gDcvs.stats().touchBoosts==boosts &&
          gPower.stats().touchWakes==0);
    /* While dark nothing ticks; once the display is back its first refresh tick wakes the thread. */
    const unsigned wakes=gEnv.wakes;
    pagani_screen_state=2;                    /* on again */
    adreno830_dcvs_vsync();
    CHECK(gEnv.wakes>wakes);
    gOps.calls.clear();
    gEnv.now+=1000000;
    gDcvs.serviceTick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().screenWakes==1 && gOps.lastArgs.level==6 && gOps.lastArgs.bus==4);
    CHECK(gOps.count("gmuStart")==1 && gDcvs.level()==6);
    /* Touches with the display on boost as before. */
    pagani_input_touch_events=pagani_input_touch_events+1;
    step(100);
    CHECK(gDcvs.stats().touches==touches+1);
    /* Off again, then a submission while dark wakes it; it sleeps again after the next quiet second. */
    pagani_screen_state=3;
    steps(70,0,false);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().screenEntries==2);
    CHECK(gPower.acquire()==kIOReturnSuccess && gPower.phase()==A830PowerActive);
    gDcvs.noteSubmit();++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;
    gPower.release();
    steps(70,0,false);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().screenEntries==3);
    pagani_screen_state=4;
    steps(2,0,false);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().screenWakes==2);

    /* Without the rule: the word changes nothing (P0 stays up, dark touches boost as ever). */
    boot();
    settle();
    pagani_screen_state=5;
    const uint32_t lit=gDcvs.stats().touches;
    pagani_input_touch_events=pagani_input_touch_events+1;
    steps(200,0,false);
    CHECK(gPower.phase()==A830PowerActive && !gPower.screenOff() && gDcvs.stats().touches==lit+1);
    pagani_screen_state=0;
}

static void testPark()
{
    boot();
    settle();
    steps(4,950);
    CHECK(gDcvs.level()==9);
    gDcvs.park();
    step(950);
    CHECK(gDcvs.level()==1 && gDcvs.ceiling()==1 && gOps.votes.back().first==1 &&
          gOps.votes.back().second==(0xffff0000u|kTable.levels[0].bus));
    pagani_input_touch_events=pagani_input_touch_events+1;
    steps(20,1000);
    CHECK(gDcvs.level()==1);
    adreno830_dcvs_park();   /* a second park (the hook) changes nothing */
    steps(2,1000);
    CHECK(gDcvs.level()==1 && gDcvs.ceiling()==1);
}

int main()
{
    /* The design profile for the cases written for it; testAnnibale switches to the annibale one (the boot default). */
    adreno830_profile_value=A830ProfileDesign;
    testLevels();
    testUpDown();
    testBoosts();
    testThermalSettle();
    testThermalRules();
    testThermalPressure();
    testCompositing();
    testFrameHeadroom();
    testThermalHooks();
    testFixed();
    testVotesAndSleep();
    testTe();
    testUnderruns();
    testServiceAndStatus();
    testAnnibale();
    testMaxLevel();
    adreno830_profile_value=A830ProfileDesign;
    testScreen();
    testPark();
    std::printf("PASS: A830 DCVS on busy and temperature traces: up 90 %%/x1.25 and x1.1, down 4/6 windows, idle 64 ms, "
                "wake/touch/late boosts, ACD ceiling; %u composited-load cases (steady desktops at 75 %% or less, a burst stops at 734 "
                "MHz, one level more per 12 saturated windows, late frames do not climb, a boost ends at the load's level); %u "
                "frame-headroom cases (+-30 %% frames in their 8.3 ms and no vote at every load up to 540 MHz of work, late frames at 85 %% "
                "busy or past 734 MHz's target); design "
                "thermal loop: %u heat cases (full load settles within one level 5 C "
                "under the 80 C target to 1 C above it for 1 s and 2 s hotspots, levels straddling the band cycle between those "
                "two only, down per 500 ms above it unless falling, the "
                "5 C band, the 525 MHz floor, 389 MHz at 105 C and back to the floor per 500 ms below 100 C, up per 2 s, "
                "pagani-gpu-ttarget, 607 MHz without readings, the top from a cool first reading, one-level up steps under "
                "pressure, adreno830_thermal_state and adreno830_cpu_hint), fixed "
                "mode, votes on change, sleep without MMIO, TE windows, underrun floor and bus-max, watchdog, status; "
                "annibale profile: 342 MHz unless pagani-gpu-dcvs-max, 160 MHz at 70 C or unreadable until below 60 C, bw "
                "without 0xFFFF0000, an unacknowledged vote ends the changes; pagani-gpu-maxlevel: %u ceiling cases (ramp "
                "160/342/607/734 MHz with the DT bus, level n, the top beyond the table under ACD off, dcvs-max, thermal "
                "hold, fixed mode, design); the screen rule through the service thread (PaganiPanel's word, dark touches "
                "ignored, no window polling while dark, the display's refresh tick wakes it at the touch level) (%u checks)\n",
                compositingCases,headroomCases,heatCases,maxLevelCases,gChecks);
    return 0;
}
