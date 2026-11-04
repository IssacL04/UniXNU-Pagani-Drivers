/* SPDX-License-Identifier: MIT */
/* host-test: A830PowerState.cpp A830Recovery.cpp A830Dcvs.cpp platform/sm8750/Sm8750Tsens.c
 * The power state machine of ARCHITECTURE 4.17 against a register model and logged operations: P0 never sleeps; P1
 * enters slumber only after 250 ms quiet (no holder, everything retired) with the exact S1..S10 order (OOB, pending
 * interrupt check, GPU mask, lowest idle, RSCC TCS, PREPARE_SLUMBER, CX idle, RSCC sleep, GBIF halt, GMU masks, CX
 * power-off); the CX-keeping variant stops before S10; a wake restores CX, SMMU and the CX timer only after a collapse,
 * starts the GMU at the boost level of its reason, keeps the memptrs fence, and re-enables the lines; aborts and
 * failures (with the fault sink) leave the GPU unusable until recovered; IFPC switches through a GMU restart. The design
 * profile throughout, then the annibale profile's one difference here: the OOB_GPU a bring-up keeps is released by
 * slumber entry and the IFPC cycle, not by an abort. */
#include "A830PowerFakes.h"

static uint32_t gSinkCause,gSinkCalls;
static uint64_t gSinkDetail;
static void sink(void *,uint8_t cause,uint64_t detail) {gSinkCause=cause;gSinkDetail=detail;++gSinkCalls;}

static A830PowerState gPower;

static void boot(const A830PowerConfig &config,bool withSink=true)
{
    resetAll();
    gSinkCause=gSinkCalls=0;gSinkDetail=0;
    CHECK(gPower.init(config,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    if(withSink)gPower.setFaultSink(sink,nullptr);
    else gPower.setFaultSink(nullptr,nullptr);
    gPower.start(2,2);
    CHECK(gPower.phase()==A830PowerActive && adreno830_power_state()==&gPower);
}

static A830PowerConfig full() {A830PowerConfig c;c.slumber=A830SlumberFull;return c;}

/* Run the service tick until `ms` of virtual time passed. */
static void runFor(unsigned ms)
{
    const uint64_t end=gEnv.now+uint64_t(ms)*1000000;
    while(gEnv.now<end) {
        const uint64_t next=gPower.tick(gEnv.now);
        gEnv.now=next>gEnv.now && next<end?next:gEnv.now+1000000;
    }
}

static void testInit()
{
    A830PowerState p;
    A830PowerConfig c;
    A830PowerIo io=gIo;io.read32=nullptr;
    CHECK(p.init(c,io,gPowerEnv,gPowerOps)==kIOReturnBadArgument);
    A830PowerOps ops=gPowerOps;ops.retired=nullptr;
    CHECK(p.init(c,gIo,gPowerEnv,ops)==kIOReturnBadArgument);
    /* P0 needs no transition operation; P1 needs all, full P1 also the CX restore. */
    ops=gPowerOps;ops.gmuStart=nullptr;ops.rsccSleep=nullptr;
    CHECK(p.init(c,gIo,gPowerEnv,ops)==kIOReturnSuccess);
    c.slumber=A830SlumberCxKeep;
    CHECK(p.init(c,gIo,gPowerEnv,ops)==kIOReturnBadArgument);
    ops=gPowerOps;ops.smmuRestore=nullptr;
    CHECK(p.init(c,gIo,gPowerEnv,ops)==kIOReturnSuccess);
    c.slumber=A830SlumberFull;
    CHECK(p.init(c,gIo,gPowerEnv,ops)==kIOReturnBadArgument);
    c.slumber=7;
    CHECK(p.init(c,gIo,gPowerEnv,gPowerOps)==kIOReturnBadArgument);
}

static void testP0()
{
    boot(A830PowerConfig{});
    runFor(5000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty() && gHw.log.empty());
    CHECK(gPower.acquire()==kIOReturnSuccess && gPower.holders()==1);
    gPower.release();
    CHECK(gPower.holders()==0 && gPower.accessBegin(false) && gPower.accessBegin(true));
    gPower.accessEnd(true);gPower.accessEnd(false);
    CHECK(gOps.calls.empty());   /* IFPC off: a GX reader does not need OOB */
}

static void testSlumberSequence()
{
    boot(full());
    gOps.lastSubmitted=gOps.retired=41;
    /* Work in flight or a holder: no slumber however long it lasts. */
    gOps.retired=40;
    runFor(1000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    gOps.retired=41;
    CHECK(gPower.acquire()==kIOReturnSuccess);
    runFor(1000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    gPower.release();
    /* Seqnos that advance without release() are activity too. */
    for(unsigned i=0;i<20;++i) {++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;runFor(100);}
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    /* Not yet 250 ms quiet (the last seqno was seen 100 ms ago). */
    runFor(140);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    runFor(20);
    CHECK(gPower.phase()==A830PowerSlumber);
    /* Exact order of S2..S10. */
    const std::vector<std::string> expected={"oobSet","irq:01:off","oobClear","hfi:33:ffff0002:00000002","rsccSleep",
                                             "irq:3f:off","cxPowerOff"};
    CHECK(gOps.calls==expected);
    const long mask=gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_INT_0_MASK),0);
    const long counter=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUCX_POWER_COUNTER_ENABLE),0);
    const long fence=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUAO_AHB_FENCE_CTRL),0);
    const long fal=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUCX_CX_FALNEXT_INTF),1);
    const long client=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),A8XX_GBIF_HALT_CLIENT);
    const long arb=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),A8XX_GBIF_HALT_ARB);
    const long release=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),0);
    const long gmuMask=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffffu);
    const long aoMask=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffffu);
    const long gmuClr=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffffu);
    const long aoClr=gHw.writeAt(A830PowerWinGmu,M(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffffu);
    const long cxMask=gHw.writeAt(A830PowerWinCxMisc,X(A8XX_CX_MISC_INT_0_MASK),0xffffffffu);
    CHECK(mask>=0 && counter>mask && fence>counter && fal>fence && client>fal && arb>client && release>arb);
    CHECK(gmuMask>release && aoMask>gmuMask && gmuClr>aoMask && aoClr>gmuClr && cxMask>aoClr);
    CHECK(gHw.readAny(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS)) && gHw.readAny(A830PowerWinGmu,M(A8XX_GMUCX_RPMH_POWER_STATE)));
    for(uint32_t tcs=0;tcs<10;++tcs)CHECK(gHw.readAny(A830PowerWinRscc,R(A8XX_RSCC_TCS_DRV0_STATUS(tcs))));
    CHECK(gHw.readAny(A830PowerWinGmu,M(A8XX_GMUAO_GPU_CX_BUSY_STATUS)));
    CHECK(!gPower.cxOn() && gPower.cxLost() && gPower.rsccSlept() && gPower.stats().entries==1);
    CHECK(!gStatus.slumber.empty() && adreno830_status.state==A830Slumber);
    /* Asleep: short readers are refused, nothing reads a register; the SLP row counts seconds. */
    const size_t logged=gHw.log.size();
    CHECK(!gPower.accessBegin(false));
    runFor(3000);
    CHECK(gHw.log.size()==logged && gStatus.slumber.back()[0]>=2);
    /* Wake on submission: CX on, SMMU and timer restored (collapse), GMU at the submit boost, fence kept, lines on. */
    gOps.calls.clear();
    gPower.setTarget(1,2);
    gPower.setWakeTargets(6,4,7,6);
    gOps.lastSubmitted=gOps.retired=41;
    CHECK(gPower.acquire()==kIOReturnSuccess && gPower.phase()==A830PowerActive && gPower.holders()==1);
    const std::vector<std::string> wake={"cxPowerOn","smmuRestore","cxTimerSeed","gmuStart","cpStart","secureResume","irq:3f:on"};
    CHECK(gOps.calls==wake);
    CHECK(gOps.lastArgs.level==6 && gOps.lastArgs.bus==4 && gOps.lastArgs.rsccWake && gOps.lastArgs.cxLost &&
          !gOps.lastArgs.ifpc && gOps.fence==41);
    CHECK(gPower.cxOn() && !gPower.cxLost() && !gPower.rsccSlept() && gPower.stats().wakes==1 && gStatus.running==1);
    CHECK(gPower.votedLevel()==6 && gPower.votedBus()==4 && gPower.stats().sleptNs>=3000000000ull);
    gPower.release();
}

static void testCxStillOnAndKeep()
{
    /* Full slumber, but another voter kept CX (Sm8750GpuOffCxStillOn): no SMMU/timer restore on wake. */
    boot(full());
    gOps.warnings=Sm8750GpuOffCxStillOn;
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && !gPower.cxOn() && !gPower.cxLost());
    gOps.calls.clear();
    CHECK(gPower.wake(A830WakeExplicit)==kIOReturnSuccess);
    CHECK(gOps.index("cxPowerOn")==0 && gOps.count("smmuRestore")==0 && gOps.count("cxTimerSeed")==0 && !gOps.lastArgs.cxLost);
    /* CX-keeping variant: S1..S9 only, no power-off, no power-on, GPU-domain lines only. */
    A830PowerConfig keep;keep.slumber=A830SlumberCxKeep;
    boot(keep);
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.cxOn() && gOps.count("cxPowerOff")==0 && gOps.count("irq:27:off")==1);
    gOps.calls.clear();
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    CHECK(gOps.count("cxPowerOn")==0 && gOps.count("smmuRestore")==0 && gOps.index("gmuStart")==0 && gOps.lastArgs.rsccWake);
}

static void testAhbTimeout()
{
    /* M13 c: off by default; when on, the five CX_AHB controls get 0x1B12 after each CX power-up, before the GMU. */
    boot(full());
    runFor(300);
    gPower.acquire();gPower.release();
    CHECK(!gHw.wrote(A830PowerWinCxMisc,X(A8XX_CX_MISC_CX_AHB_AON_CNTL)));
    A830PowerConfig c=full();c.ahbTimeout=true;
    boot(c);
    runFor(300);
    const size_t before=gHw.log.size();
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    for(uint32_t control=A8XX_CX_MISC_CX_AHB_AON_CNTL;control<=A8XX_CX_MISC_CX_AHB_HOST_CNTL;++control)
        CHECK(gHw.writeAt(A830PowerWinCxMisc,X(control),A830_CX_AHB_CNTL_VALUE,before)>=0);
    CHECK(X(A8XX_CX_MISC_CX_AHB_AON_CNTL)==0x40 && X(A8XX_CX_MISC_CX_AHB_HOST_CNTL)==0x50);
}

static void testTouchPrewake()
{
    boot(full());
    gPower.setWakeTargets(6,4,7,6);
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber);
    gPower.noteTouch();
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().touchWakes==1 && gOps.lastArgs.level==7 && gOps.lastArgs.bus==6);
    /* A touch while active only restarts the idle clock. */
    gEnv.now+=200000000ull;
    gPower.noteTouch();
    runFor(200);
    CHECK(gPower.phase()==A830PowerActive);
    runFor(100);
    CHECK(gPower.phase()==A830PowerSlumber);
    /* touchPrewake off: a touch does not wake. */
    A830PowerConfig c=full();c.touchPrewake=false;
    boot(c);
    runFor(300);
    gPower.noteTouch();
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerSlumber);
}

static void testAborts()
{
    /* An enabled interrupt pending under OOB: abort, OOB cleared, nothing masked. */
    boot(full());
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS))=A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnBusy && gPower.phase()==A830PowerActive);
    CHECK(gOps.calls==std::vector<std::string>({"oobSet","oobClear"}) && !gHw.wrote(A830PowerWinGpu,G(A8XX_RBBM_INT_0_MASK)));
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS))=0;
    /* Busy GPU (RBBM_STATUS beyond bit 0): abort. Bit 0 alone is idle. */
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_STATUS))=A8XX_RBBM_STATUS_CP_BUSY;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnBusy);
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_STATUS))=A8XX_RBBM_STATUS_CP_AHB_BUSY_CX_MASTER;
    /* A holder or a short reader that does not leave within drainUs. */
    CHECK(gPower.accessBegin(false));
    const uint64_t before=gEnv.now;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnBusy && gEnv.now-before>=100000000ull);
    gPower.accessEnd(false);
    CHECK(gPower.stats().aborts==3);
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess && gPower.phase()==A830PowerSlumber);
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnNotReady);
    CHECK(gSinkCalls==0);
}

static void testFailures()
{
    struct Case {const char *op;uint16_t step;std::function<void()> arrange;};
    const Case cases[]={
        {"oobSet",A830SlumberStepOob,[] {gOps.fail["oobSet"]=kIOReturnTimeout;}},
        {"lowest idle",A830SlumberStepLowestIdle,[] {gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_RPMH_POWER_STATE))=0xf;}},
        {"rpmh",A830SlumberStepRpmh,[] {gHw.at(A830PowerWinRscc,R(A8XX_RSCC_TCS_DRV0_STATUS(7)))=0;}},
        {"prepare",A830SlumberStepPrepare,[] {gOps.fail["hfi:33:ffff0002:00000002"]=kIOReturnTimeout;}},
        {"cx idle",A830SlumberStepCxIdle,[] {gHw.at(A830PowerWinGmu,M(A8XX_GMUAO_GPU_CX_BUSY_STATUS))=A830_GMU_CX_BUSY_KGSL;}},
        {"rscc",A830SlumberStepRscc,[] {gOps.fail["rsccSleep"]=kIOReturnTimeout;}},
        {"gbif",A830SlumberStepGbif,[] {gHw.gbifAck=false;}},
        {"power off",A830SlumberStepPowerOff,[] {gOps.fail["cxPowerOff"]=kIOReturnIOError;}},
    };
    for(const Case &c:cases) {
        boot(full());
        c.arrange();
        const IOReturn r=gPower.enterSlumber(A830SlumberFull);
        CHECK(r!=kIOReturnSuccess);
        CHECK(gPower.phase()==A830PowerRecovering && gSinkCalls==1 && gSinkCause==A830FaultSlumber);
        CHECK(uint16_t(gSinkDetail>>32)==c.step && gPower.stats().lastStep==c.step && gPower.stats().failures==1);
        CHECK(gPower.acquire()==A830ReturnNotResponding && !gPower.accessBegin(false));
    }
    /* The CX-busy bit of Linux (23) is not the KGSL one (30): only bit 30 holds the sequence. */
    boot(full());
    gHw.at(A830PowerWinGmu,M(A8XX_GMUAO_GPU_CX_BUSY_STATUS))=A830_GMU_CX_BUSY_LINUX;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess);
    /* Without a recovery the failure is final. */
    boot(full(),false);
    gOps.fail["rsccSleep"]=kIOReturnTimeout;
    CHECK(gPower.enterSlumber(A830SlumberFull)!=kIOReturnSuccess && gPower.phase()==A830PowerLost);
    /* Wake failures: each bring-up step. */
    const char *wakeOps[]={"cxPowerOn","smmuRestore","gmuStart","cpStart","secureResume"};
    const uint16_t wakeSteps[]={A830WakeStepPowerOn,A830WakeStepSmmu,A830WakeStepGmu,A830WakeStepCp,A830WakeStepSecure};
    for(unsigned i=0;i<5;++i) {
        boot(full());
        runFor(300);
        CHECK(gPower.phase()==A830PowerSlumber);
        gOps.fail[wakeOps[i]]=kIOReturnTimeout;
        gOps.failStep=0x41;
        CHECK(gPower.acquire()==A830ReturnNotResponding);
        CHECK(gPower.phase()==A830PowerRecovering && gSinkCause==A830FaultWake && uint16_t(gSinkDetail>>32)==wakeSteps[i]);
        CHECK(A830PowerStepStage(wakeSteps[i])==(i==0?A830SPower:i==1?A830SSmmu:i==2?A830SGmu:i==3?A830SCp:A830SZap));
        if(i>=2)CHECK(gPower.stats().lastSubStep==0x41);
        CHECK(gPower.holders()==0);
    }
}

static void testIfpc()
{
    /* config.ifpc: S5 always boots P0; the first quiet moment restarts the GMU (CX-keeping cycle) with IFPC on. */
    A830PowerConfig c;c.ifpc=true;
    boot(c);
    CHECK(!gPower.ifpcActive() && gPower.ifpcRequested());
    gPower.tick(gEnv.now);
    CHECK(gPower.ifpcActive() && gPower.stats().ifpcCycles==1 && gOps.lastArgs.ifpc && gOps.count("cxPowerOff")==0);
    CHECK(gOps.index("rsccSleep")<gOps.index("gmuStart") && gPower.phase()==A830PowerActive);
    /* GX readers now go through OOB_GPU. */
    gOps.calls.clear();
    CHECK(gPower.accessBegin(true) && gOps.oobDepth==1);
    gPower.accessEnd(true);
    CHECK(gOps.oobDepth==0 && gOps.calls==std::vector<std::string>({"oobSet","oobClear"}));
    CHECK(gPower.accessBegin(false) && gOps.calls.size()==2);
    gPower.accessEnd(false);
    /* P1 + P2: S3 waits for the IFPC level and GX clocks off. */
    A830PowerConfig both=full();both.ifpc=true;
    boot(both);
    gPower.tick(gEnv.now);
    CHECK(gPower.ifpcActive());
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_RPMH_POWER_STATE))=A830_GMU_RPMH_POWER_STATE_IFPC;
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_GFX_PWR_CLK_STATUS))=0;   /* GX still on: not idle yet */
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnTimeout && gSinkCause==A830FaultSlumber);
    boot(both);
    gPower.tick(gEnv.now);
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_RPMH_POWER_STATE))=A830_GMU_RPMH_POWER_STATE_IFPC;
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_GFX_PWR_CLK_STATUS))=A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess);
    /* Switching it off again also restarts the GMU. */
    A830PowerConfig on;on.ifpc=true;
    boot(on);
    gPower.tick(gEnv.now);
    gPower.setIfpc(false);
    gOps.calls.clear();
    /* The power cycle waits for the idle level now in force: IFPC with GX collapsed. */
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_RPMH_POWER_STATE))=A830_GMU_RPMH_POWER_STATE_IFPC;
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_GFX_PWR_CLK_STATUS))=A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF;
    gPower.tick(gEnv.now);
    CHECK(!gPower.ifpcActive() && !gOps.lastArgs.ifpc && gPower.stats().ifpcCycles==2);
}

/* The annibale profile (A830Profile.h): every bring-up leaves its OOB_GPU set (the orchestrator's S7 and secureResume do
 * not clear it, kernel-sequences section 14 row 28), so slumber entry drops that reference together with its own and the
 * GMU can idle; an abort keeps it; the IFPC switch cycle does the same. The design profile clears once (above). Here the
 * held reference is the fake's oobDepth 1 at the start. */
static void testAnnibale()
{
    adreno830_profile_value=A830ProfileAnnibale;
    boot(full());
    gOps.oobDepth=1;
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && gOps.oobDepth==0);
    const std::vector<std::string> expected={"oobSet","irq:01:off","oobClear","oobClear","hfi:33:ffff0002:00000002","rsccSleep",
                                             "irq:3f:off","cxPowerOff"};
    CHECK(gOps.calls==expected);
    /* The wake is the design's: the bring-up operations set it again (gmuStart), resume keeps it (not modelled here). */
    gOps.calls.clear();
    CHECK(gPower.acquire()==kIOReturnSuccess && gOps.count("oobClear")==0);
    gPower.release();
    /* An enabled interrupt pending: only the entry's own reference goes, the bring-up's stays. */
    boot(full());
    gOps.oobDepth=1;
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS))=A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnBusy && gOps.oobDepth==1);
    CHECK(gOps.calls==std::vector<std::string>({"oobSet","oobClear"}));
    /* The IFPC switch (a CX-keeping cycle) releases it the same way; GX readers then stack on the held reference. */
    A830PowerConfig c;c.ifpc=true;
    boot(c);
    gOps.oobDepth=1;
    gPower.tick(gEnv.now);
    CHECK(gPower.ifpcActive() && gOps.count("oobClear")==2 && gOps.oobDepth==0);
    /* The same full slumber in the design profile: one clear, nothing held before. */
    adreno830_profile_value=A830ProfileDesign;
    boot(full());
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && gOps.count("oobClear")==1 && gOps.oobDepth==0);
}

/* The screen rule (A830PowerConfig::screenSlumber, pagani-screenoff bits 1 and 3): under P0 the GPU sleeps only once the display
 * has been off and the GPU quiet for screenIdleMs, counted from the later of the two; touches on the dark display are no activity
 * and do not wake it; a submission wakes it as always and it sleeps again after the next quiet second; the display coming back
 * wakes it at once at the touch level; a failed entry or wake ends the rule for the boot (the recovery takes the GPU back); the
 * full variant collapses CX; with P1 configured, P1 decides and the display coming back still pre-wakes what slept while dark;
 * without the rule the screen state changes nothing. */
static A830PowerConfig screenRule(uint8_t mode=A830SlumberCxKeep) {A830PowerConfig c;c.screenSlumber=mode;return c;}

static void testScreenRule()
{
    /* Validation: the mode's range, the delay, the operations the mode needs. */
    {
        A830PowerState p;
        A830PowerConfig c=screenRule(7);
        CHECK(p.init(c,gIo,gPowerEnv,gPowerOps)==kIOReturnBadArgument);
        c=screenRule();c.screenIdleMs=0;
        CHECK(p.init(c,gIo,gPowerEnv,gPowerOps)==kIOReturnBadArgument);
        A830PowerOps ops=gPowerOps;ops.gmuStart=nullptr;
        CHECK(p.init(screenRule(),gIo,gPowerEnv,ops)==kIOReturnBadArgument);
        ops=gPowerOps;ops.smmuRestore=nullptr;
        CHECK(p.init(screenRule(),gIo,gPowerEnv,ops)==kIOReturnSuccess);
        CHECK(p.init(screenRule(A830SlumberFull),gIo,gPowerEnv,ops)==kIOReturnBadArgument);
    }
    /* P0 with the rule: the display on, it never sleeps. */
    boot(screenRule());
    gPower.setWakeTargets(6,4,7,6);
    gOps.lastSubmitted=gOps.retired=41;
    runFor(5000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty() && !gPower.screenOff());
    /* Quiet for 5 s already: the display going off still waits its second, counted from the off. */
    gPower.setScreenOff(true,gEnv.now);
    CHECK(gPower.screenOff());
    runFor(990);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    runFor(20);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.screenSlept() && gPower.stats().screenEntries==1 && gPower.cxOn());
    const std::vector<std::string> entry={"oobSet","irq:01:off","oobClear","hfi:33:ffff0002:00000002","rsccSleep","irq:27:off"};
    CHECK(gOps.calls==entry && gOps.count("cxPowerOff")==0);
    /* A touch on the dark display: no activity, no pre-wake. */
    gOps.calls.clear();
    gPower.noteTouch();
    runFor(500);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().touchWakes==0 && gOps.calls.empty());
    /* A submission while dark: woken at the submit boost, the slumber again one quiet second after its release. */
    CHECK(gPower.acquire()==kIOReturnSuccess && gPower.phase()==A830PowerActive && !gPower.screenSlept());
    CHECK(gOps.lastArgs.level==6 && gOps.lastArgs.bus==4 && gOps.lastArgs.rsccWake && !gOps.lastArgs.cxLost);
    ++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;
    gPower.release();
    runFor(950);
    CHECK(gPower.phase()==A830PowerActive);
    runFor(100);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.screenSlept() && gPower.stats().screenEntries==2);
    /* The display back: woken in the next tick at the touch boost, counted as a screen wake. */
    gOps.calls.clear();
    gPower.setScreenOff(false,gEnv.now);
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && !gPower.screenSlept() && gPower.stats().screenWakes==1);
    CHECK(gOps.lastArgs.level==7 && gOps.lastArgs.bus==6 && gOps.index("gmuStart")<gOps.index("cpStart") &&
          gOps.count("cxPowerOn")==0 && gOps.count("secureResume")==1);
    /* On again: P0, no sleep however quiet; touches count as activity again. */
    runFor(3000);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().screenEntries==2);
    /* Work in flight when the display goes off: no slumber until it retired and a quiet second passed. */
    gOps.lastSubmitted=50;gOps.retired=49;
    gPower.setScreenOff(true,gEnv.now);
    runFor(3000);
    CHECK(gPower.phase()==A830PowerActive);
    gOps.retired=50;
    runFor(990);
    CHECK(gPower.phase()==A830PowerActive);
    runFor(30);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().screenEntries==3);
    /* Off shorter than the delay: no slumber. */
    gPower.setScreenOff(false,gEnv.now);
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive);
    gPower.setScreenOff(true,gEnv.now);
    runFor(600);
    gPower.setScreenOff(false,gEnv.now);
    runFor(2000);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().screenEntries==3 && gPower.stats().screenWakes==2);

    /* The full variant: CX collapses, the wake restores CX, the SMMU and the CX timer. */
    boot(screenRule(A830SlumberFull));
    gPower.setScreenOff(true,gEnv.now);
    runFor(1100);
    CHECK(gPower.phase()==A830PowerSlumber && !gPower.cxOn() && gOps.count("cxPowerOff")==1);
    gOps.calls.clear();
    gPower.setScreenOff(false,gEnv.now);
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && gOps.index("cxPowerOn")==0 && gOps.count("smmuRestore")==1 &&
          gOps.count("cxTimerSeed")==1);

    /* A failed entry: the recovery takes the GPU back and the rule is off for the boot. */
    boot(screenRule());
    gOps.fail["rsccSleep"]=kIOReturnTimeout;
    gPower.setScreenOff(true,gEnv.now);
    runFor(1100);
    CHECK(gPower.phase()==A830PowerRecovering && gSinkCause==A830FaultSlumber && gPower.stats().screenBlocked);
    gPower.lock();
    CHECK(gPower.beginRecovery()==kIOReturnSuccess);
    gPower.endRecovery(true,2,2);
    gPower.unlock();
    gOps.calls.clear();
    runFor(5000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty() && gPower.stats().screenEntries==0);
    /* A failed wake: the same. */
    boot(screenRule());
    gPower.setScreenOff(true,gEnv.now);
    runFor(1100);
    CHECK(gPower.phase()==A830PowerSlumber);
    gOps.fail["gmuStart"]=kIOReturnTimeout;
    gPower.setScreenOff(false,gEnv.now);
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerRecovering && gSinkCause==A830FaultWake && gPower.stats().screenBlocked);
    gPower.lock();gPower.beginRecovery();gPower.endRecovery(true,2,2);gPower.unlock();
    CHECK(!gPower.screenSlept());
    gPower.setScreenOff(true,gEnv.now);
    runFor(3000);
    CHECK(gPower.phase()==A830PowerActive);

    /* P1 configured: P1's 250 ms decide; what slept while dark is pre-woken when the display comes back, what slept while lit
     * waits for its submission or touch as before. */
    A830PowerConfig both=screenRule();both.slumber=A830SlumberFull;
    boot(both);
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && !gPower.screenSlept());
    gPower.setScreenOff(true,gEnv.now);
    gPower.setScreenOff(false,gEnv.now);
    runFor(500);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().screenWakes==0);
    gPower.setScreenOff(true,gEnv.now);
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    runFor(300);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.screenSlept() && gPower.stats().screenEntries==0);
    gPower.setScreenOff(false,gEnv.now);
    gPower.tick(gEnv.now);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().screenWakes==1);

    /* The service thread reads its clock before the tick; a holder that releases (or a wake that ends) in between stamps
     * the activity after that reading. The quiet time then counts from that stamp: no slumber at once (an unsigned
     * now - stamp would wrap and enter it), the deadline is the stamp plus the delay. The screen rule's second and P1's 250 ms. */
    for(const bool p1:{false,true}) {
        boot(p1?full():screenRule());
        if(!p1)gPower.setScreenOff(true,gEnv.now);
        runFor(p1?200:900);                    /* dark (screen rule) and quiet, not yet long enough */
        CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
        const uint64_t read=gEnv.now+(p1?100:200)*1000000ull;   /* the tick's clock: past the delay from the off / the quiet */
        gEnv.now=read+2000000;                 /* a holder released 2 ms after that reading */
        CHECK(gPower.acquire()==kIOReturnSuccess);
        gPower.release();
        const uint64_t deadline=gPower.tick(read);
        CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty() && gPower.stats().entries==0);
        CHECK(deadline==gEnv.now+(p1?250:1000)*1000000ull);
        runFor(p1?240:990);
        CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
        runFor(20);
        CHECK(gPower.phase()==A830PowerSlumber && gPower.stats().entries==1 && gPower.stats().screenEntries==(p1?0u:1u));
    }

    /* Without the rule: the screen state is ignored, touches count as before, P0 never sleeps. */
    boot(A830PowerConfig{});
    gPower.setScreenOff(true,gEnv.now);
    CHECK(!gPower.screenOff());
    runFor(5000);
    CHECK(gPower.phase()==A830PowerActive && gOps.calls.empty());
    const uint64_t before=gPower.lastActivityNs();
    gEnv.now+=1000000;
    gPower.noteTouch();
    CHECK(gPower.lastActivityNs()>before);
}

static void testRecoveryInterface()
{
    boot(full());
    gPower.lock();
    CHECK(gPower.beginRecovery()==kIOReturnSuccess && gPower.phase()==A830PowerRecovering);
    CHECK(!gPower.accessBegin(true) && gPower.holders()==0);
    gPower.unlock();
    CHECK(gPower.acquire()==A830ReturnNotResponding && gPower.holders()==0);
    gPower.lock();
    gPower.endRecovery(true,5,4);
    gPower.unlock();
    CHECK(gPower.phase()==A830PowerActive && gPower.votedLevel()==5 && gPower.votedBus()==4);
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    gPower.markLost();
    CHECK(gPower.acquire()==A830ReturnNotResponding && gPower.phase()==A830PowerLost);
    /* Target and level 0. */
    boot(full());
    gPower.setTarget(0,9);
    CHECK(gPower.targetLevel()==2 && gPower.targetBus()==2);
    gPower.setTarget(9,8);
    CHECK(gPower.targetLevel()==9 && gPower.targetBus()==8);
}

int main()
{
    /* The design profile for the cases written for it; testAnnibale switches to the annibale one (the boot default). */
    adreno830_profile_value=A830ProfileDesign;
    testInit();
    testP0();
    testSlumberSequence();
    testCxStillOnAndKeep();
    testAhbTimeout();
    testTouchPrewake();
    testAborts();
    testFailures();
    testIfpc();
    testRecoveryInterface();
    testScreenRule();
    testAnnibale();
    std::printf("PASS: A830 power states: P0 never sleeps, P1 slumber S1-S10 in order after 250 ms quiet, CX-keeping S1-S9, "
                "wake with CX/SMMU/timer restore only after a collapse, touch pre-wake, aborts, 13 failure steps to the "
                "fault sink, IFPC switch by GMU restart; the screen rule (pagani-screenoff): slumber 1 s after the display "
                "goes off and the GPU is quiet, dark touches ignored, submissions wake and it sleeps again, the display back "
                "wakes it at the touch level, a failed entry or wake ends the rule, the full variant, P1 alongside, off by "
                "default; a release stamped after the tick's clock reading still waits the quiet time (rule and P1); annibale "
                "profile: slumber and the IFPC cycle release the held OOB_GPU, an abort keeps it (%u checks)\n",gChecks);
    return 0;
}
