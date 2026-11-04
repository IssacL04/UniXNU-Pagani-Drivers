/* SPDX-License-Identifier: MIT */
/* host-test: A830PowerState.cpp A830Recovery.cpp A830Dcvs.cpp platform/sm8750/Sm8750Tsens.c
 * Minimal recovery of ARCHITECTURE 4.13.4 (M10) and its M13 completion against the register model: the gate closes,
 * RBBM_INT_0_MASK goes to 0, the snapshot is taken (GX registers only with GX up), the in-flight jobs fail, the GBIF
 * halts GX side -> client -> arbiter -> release, A830Gmu::forceOff runs after the halt, then S4/S5 (no CX timer re-seed
 * and no SMMU restore while CX stayed up), S6 with the memptrs fence at the last submitted seqno, S7 resume only, the
 * fence waiters are woken, the storm-guarded lines re-enabled, the gate opens. The 3rd report within 60 s and a failed
 * recovery are final. Reports coalesce; the software watchdog fires after 2 s without progress while work is in
 * flight and never without work. A fault during slumber or wake recovers through a full power-up. The design profile
 * throughout; the annibale profile runs the same restart (operations and register writes), and its next slumber releases
 * the OOB_GPU the restart kept. */
#include "A830PowerFakes.h"

static A830PowerState gPower;
static A830Recovery gRecovery;

static void boot(const A830RecoveryConfig &config=A830RecoveryConfig{},const A830PowerConfig &power=A830PowerConfig{})
{
    resetAll();
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    CHECK(gRecovery.init(config,gIo,gPowerEnv,gPowerOps,gRecoveryOps,&gPower)==kIOReturnSuccess);
    gPower.start(4,3);
    gOps.lastSubmitted=120;gOps.retired=117;gOps.jobs=3;
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_STATUS))=0x00800004u;
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS))=A8XX_RBBM_INT_0_HANG_DETECT;
    gHw.at(A830PowerWinGpu,G(A8XX_CP_RB_RPTR))=0x140;
    gHw.at(A830PowerWinGpu,G(A8XX_CP_RB_WPTR))=0x1c0;
    gPipeRegs[A8XX_CP_IB1_BASE]=0x20004000;gPipeRegs[A8XX_CP_IB1_BASE_HI]=0;gPipeRegs[A8XX_CP_IB1_REM_SIZE]=0x30;
}

static void testInit()
{
    resetAll();
    A830PowerState power;
    CHECK(power.init(A830PowerConfig{},gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    A830Recovery r;
    CHECK(r.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,gRecoveryOps,nullptr)==kIOReturnBadArgument);
    A830PowerOps ops=gPowerOps;ops.gmuForceOff=nullptr;
    CHECK(r.init(A830RecoveryConfig{},gIo,gPowerEnv,ops,gRecoveryOps,&power)==kIOReturnBadArgument);
    A830RecoveryOps rops=gRecoveryOps;rops.gate=nullptr;
    CHECK(r.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,rops,&power)==kIOReturnBadArgument);
    A830RecoveryConfig c;c.watchdogLimitMs=100;
    CHECK(r.init(c,gIo,gPowerEnv,gPowerOps,gRecoveryOps,&power)==kIOReturnBadArgument);
    rops.gate=gRecoveryOps.gate;rops.readPipe=nullptr;rops.snapshot=nullptr;rops.fenceSignal=nullptr;rops.irqRestore=nullptr;
    CHECK(r.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,rops,&power)==kIOReturnSuccess);
}

static void testMinimalRecovery()
{
    boot();
    const size_t before=gHw.log.size();
    CHECK(gRecovery.recover(A830FaultHang,0x800000)==kIOReturnSuccess);
    /* Operation order. */
    const std::vector<std::string> expected={"gate:1","snapshot","failInflight","gmuForceOff","gmuStart","cpStart",
                                             "secureResume","irq:3f:on","fenceSignal","irqRestore","gate:0"};
    std::vector<std::string> ops;
    for(const auto &c:gOps.calls)if(c!="readPipe")ops.push_back(c);
    CHECK(ops==expected);
    CHECK(gOps.count("cxTimerSeed")==0 && gOps.count("smmuRestore")==0 && gOps.count("cxPowerOn")==0 &&
          gOps.count("cxPowerOff")==0);
    /* GMU started at the DCVS target (here the start level), after the RSCC sleep of forceOff; CX stayed up. */
    CHECK(gOps.lastArgs.level==4 && gOps.lastArgs.bus==3 && gOps.lastArgs.rsccWake && !gOps.lastArgs.cxLost && gOps.fence==120);
    /* Registers: mask 0 before the GBIF halt, the halt order, RBBM_GBIF_HALT not released (hw_init does it). */
    const long mask=gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_INT_0_MASK),0,before);
    const long gx=gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_GBIF_HALT),A8XX_RBBM_GBIF_HALT_GX,before);
    const long client=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),A8XX_GBIF_HALT_CLIENT,before);
    const long arb=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),A8XX_GBIF_HALT_ARB,before);
    const long release=gHw.writeAt(A830PowerWinGpu,G(A8XX_GBIF_HALT),0,before);
    CHECK(mask>=0 && gx>mask && client>gx && arb>client && release>arb);
    CHECK(gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_GBIF_HALT),0,before)<0);
    /* Snapshot. */
    const A830HangSnapshot &s=gRecovery.snapshot();
    CHECK(s.cause==A830FaultHang && s.detail==0x800000 && s.cxOn && s.gxOn && s.rbbmStatus==0x00800004u);
    CHECK(s.int0Status==A8XX_RBBM_INT_0_HANG_DETECT && s.cpRptr==0x140 && s.cpWptr==0x1c0 && s.ib1Base==0x20004000 &&
          s.ib1Rem==0x30 && s.fence==117 && s.lastSubmitted==120 && s.smmuFsr[0]==2);
    CHECK(adreno830_status.reg==A830_GPU_REG_PHYS(A8XX_RBBM_STATUS) && adreno830_status.expect==(uint64_t(120)<<32|117));
    /* Status: LOST, RECOVER S5/S6/S7, RUN; counters. */
    CHECK(gStatus.lost.size()==1 && gStatus.lost[0][0]==A830SRunning && gStatus.lost[0][1]==A830FaultHang &&
          gStatus.lost[0][2]==A830ReasonHang);
    bool s5=false,s6=false,s7=false;
    for(const auto &r:gStatus.recovering) {s5|=r[0]==A830SGmu;s6|=r[0]==A830SCp;s7|=r[0]==A830SZap;}
    CHECK(s5 && s6 && s7 && gStatus.running==1 && adreno830_status.state==A830Done);
    CHECK(adreno830_status.recoveries==1 && adreno830_status.hangs==1 && adreno830_status.faults==1);
    CHECK(gRecovery.stats().recoveries==1 && gRecovery.stats().jobsFailed==3 && !gRecovery.lost());
    CHECK(gPower.phase()==A830PowerActive && gPower.votedLevel()==4 && !gPower.rsccSlept() && gOps.oobDepth==0);
    CHECK(adreno830_recovery_gate()==A830GateOpen);
}

static void testGxOff()
{
    /* GX already collapsed: no GX register is touched (synchronous external abort), GBIF halt skips the GX side. */
    boot();
    gHw.at(A830PowerWinGmu,M(A8XX_GMUCX_GFX_PWR_CLK_STATUS))=A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF;
    const size_t before=gHw.log.size();
    bool gxTouched=false;
    gHw.readHook=[&](uint8_t window,uint32_t offset,uint32_t *) {
        if(window==A830PowerWinGpu && offset<G(0x3C00))gxTouched=true;   /* RBBM/CP (GX); GBIF (0x3C00+) is CX */
        return false;
    };
    gOps.during=[&](const std::string &name) {if(name=="gmuStart")gHw.readHook=nullptr;};
    CHECK(gRecovery.recover(A830FaultGmuWatchdog,0)==kIOReturnSuccess);
    CHECK(!gxTouched && !gRecovery.snapshot().gxOn && gRecovery.snapshot().rbbmStatus==0);
    CHECK(gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_GBIF_HALT),A8XX_RBBM_GBIF_HALT_GX,before)<0);
    CHECK(gHw.writeAt(A830PowerWinGpu,G(A8XX_RBBM_INT_0_MASK),0,before)<0);
    CHECK(gStatus.lost[0][2]==A830ReasonGmuFault && adreno830_status.hangs==0);
}

static void testRateLimit()
{
    boot();
    CHECK(gRecovery.recover(A830FaultHang,1)==kIOReturnSuccess);
    gEnv.now+=10ull*1000000000;
    CHECK(gRecovery.recover(A830FaultCpHwError,2)==kIOReturnSuccess);
    gEnv.now+=10ull*1000000000;
    gOps.calls.clear();
    /* 3rd within 60 s: final, no bring-up. */
    CHECK(gRecovery.recover(A830FaultWatchdog,3)==A830ReturnNotResponding);
    CHECK(gRecovery.lost() && gPower.phase()==A830PowerLost && gOps.count("gmuStart")==0 && gOps.gates.back()==A830GateLost);
    CHECK(gOps.gateCauses.size()>=2 && gOps.gateCauses[gOps.gateCauses.size()-2]==A830FaultWatchdog &&
          gOps.gateCauses.back()==A830FaultWatchdog);
    CHECK(gStatus.lost.back()[0]==A830SRunning && gStatus.lost.back()[1]==A830FaultWatchdog && gRecovery.stats().lostCause==A830FaultWatchdog);
    CHECK(adreno830_recovery_gate()==A830GateLost);
    /* Lost is final: reports and recover() do nothing. */
    gOps.calls.clear();
    gRecovery.report(A830FaultHang,4);
    CHECK(!gRecovery.pending() && gRecovery.recover(A830FaultHang,4)==A830ReturnNotResponding && gOps.calls.empty());
    CHECK(gPower.acquire()==A830ReturnNotResponding);
    /* Spaced more than 60 s apart, recoveries go on. */
    boot();
    for(unsigned i=0;i<5;++i) {
        CHECK(gRecovery.recover(A830FaultHang,i)==kIOReturnSuccess);
        gEnv.now+=31ull*1000000000;
    }
    CHECK(gRecovery.stats().recoveries==5 && !gRecovery.lost());
    /* Recovery disabled (M7-M9 behaviour): the first loss is final. */
    A830RecoveryConfig off;off.enabled=false;
    boot(off);
    CHECK(gRecovery.recover(A830FaultHang,0)==A830ReturnNotResponding && gRecovery.lost() && gOps.count("gmuForceOff")==0);
    CHECK(gOps.count("failInflight")==1 && adreno830_status.faults==1);
}

static void testCauseMatrix()
{
    /* Fault injection matrix (M13 b): each hang-class cause recovers with its 8.3 reason; hangs counts the hang ones. Every
     * gate carries the cause, and only the causes a client's job can produce (the hang detector, CP_HW_ERROR, the software
     * watchdog) charge the culprit's process (A830FaultBlamesClient): a GMU, HFI, fenced-write, slumber, wake or S8
     * self-test fault is the driver's own. */
    struct Case {uint8_t cause;uint16_t reason;bool hang,blame;};
    const Case cases[]={{A830FaultHang,A830ReasonHang,true,true},{A830FaultCpHwError,A830ReasonCpFault,false,true},
                        {A830FaultWatchdog,A830ReasonHang,true,true},{A830FaultGmu,A830ReasonGmuFault,false,false},
                        {A830FaultGmuWatchdog,A830ReasonGmuFault,false,false},{A830FaultWptrFence,A830ReasonTimeout,false,false},
                        {A830FaultSelfTest,A830ReasonHang,true,false},{A830FaultSlumber,A830ReasonUnexpectedState,false,false},
                        {A830FaultWake,A830ReasonUnexpectedState,false,false},{A830FaultHfi,A830ReasonHfiStatus,false,false}};
    static_assert(sizeof cases/sizeof cases[0]==A830FaultCount-1,"every cause");
    for(const Case &c:cases) {
        boot();
        CHECK(gRecovery.recover(c.cause,7)==kIOReturnSuccess && !gRecovery.lost());
        CHECK(gStatus.lost.size()==1 && gStatus.lost[0][1]==c.cause && gStatus.lost[0][2]==c.reason);
        CHECK(adreno830_status.hangs==(c.hang?1u:0u) && adreno830_status.recoveries==1 && gStatus.running==1);
        CHECK(gOps.gates.front()==A830GateRecovering && gOps.gates.back()==A830GateOpen);
        CHECK(gOps.gateCauses==std::vector<uint8_t>(gOps.gates.size(),c.cause));
        CHECK(A830FaultBlamesClient(c.cause)==c.blame);
    }
    CHECK(!A830FaultBlamesClient(A830FaultNone) && !A830FaultBlamesClient(A830FaultCount));
    boot();
    CHECK(gRecovery.recover(A830FaultNone,0)==kIOReturnBadArgument && gRecovery.recover(A830FaultCount,0)==kIOReturnBadArgument);
}

static void testFailedRecovery()
{
    struct Case {const char *op;uint8_t stage;};
    const Case cases[]={{"gmuStart",A830SGmu},{"cpStart",A830SCp},{"secureResume",A830SZap}};
    for(const Case &c:cases) {
        boot();
        gOps.fail[c.op]=kIOReturnTimeout;
        gOps.failStep=0x41;
        CHECK(gRecovery.recover(A830FaultHang,0)==kIOReturnTimeout);
        CHECK(gRecovery.lost() && gPower.phase()==A830PowerLost && gOps.gates.back()==A830GateLost);
        CHECK(gStatus.lost.back()[0]==c.stage && gStatus.lost.back()[1]==0x41 && gStatus.lost.back()[3]==uint32_t(kIOReturnTimeout));
        CHECK(gOps.count("fenceSignal")==0 && gOps.count("gate:0")==0 && gRecovery.stats().failures==1);
    }
    /* A failing forceOff is recorded but the restart goes on (KGSL ignores it too); GBIF timeouts as well. */
    boot();
    gOps.fail["gmuForceOff"]=kIOReturnTimeout;
    gHw.gbifAck=false;gHw.rbbmGbifAck=false;
    CHECK(gRecovery.recover(A830FaultHang,0)==kIOReturnSuccess && gRecovery.stats().gbifTimeouts==3);
    CHECK(!gOps.lastArgs.rsccWake);
}

static void testReports()
{
    boot();
    gRecovery.report(A830FaultNone,0);
    gRecovery.report(A830FaultCount,0);
    CHECK(!gRecovery.pending() && gEnv.wakes==0);
    adreno830_recovery_report(A830FaultCpHwError,0x200);
    CHECK(gRecovery.pending() && gEnv.wakes==1 && adreno830_recovery_gate()==A830GateRecovering);
    gRecovery.report(A830FaultHang,0x800000);
    CHECK(gRecovery.stats().coalesced==1 && gEnv.wakes==1);
    CHECK(gRecovery.runPending()==kIOReturnSuccess && !gRecovery.pending());
    CHECK(gRecovery.snapshot().cause==A830FaultCpHwError && gRecovery.snapshot().detail==0x200 && gStatus.lost[0][2]==A830ReasonCpFault);
    CHECK(gRecovery.runPending()==kIOReturnSuccess && gRecovery.stats().recoveries==1);
    /* A report from inside a running recovery is coalesced into it. */
    gOps.during=[&](const std::string &name) {if(name=="cpStart")gRecovery.report(A830FaultWptrFence,0);};
    gEnv.now+=61ull*1000000000;
    CHECK(gRecovery.recover(A830FaultHang,0)==kIOReturnSuccess && !gRecovery.pending() && gRecovery.stats().coalesced==2);
    gOps.during=nullptr;
    /* declareLost: final without an attempt. */
    boot();
    gRecovery.declareLost(A830FaultWptrFence,0,kIOReturnTimeout);
    CHECK(gRecovery.lost() && gOps.count("gmuStart")==0 && gOps.count("failInflight")==1 && gOps.gates.back()==A830GateLost);
    CHECK(gStatus.lost.back()[2]==A830ReasonTimeout && gStatus.lost.back()[3]==uint32_t(kIOReturnTimeout));
    /* Both of its gates name the fenced write, which charges nobody. */
    CHECK((gOps.gates==std::vector<uint8_t>{A830GateRecovering,A830GateLost}) &&
          (gOps.gateCauses==std::vector<uint8_t>{A830FaultWptrFence,A830FaultWptrFence}) && !A830FaultBlamesClient(A830FaultWptrFence));
}

static void testWatchdog()
{
    boot();
    /* Nothing in flight: never fires. */
    gOps.retired=gOps.lastSubmitted;
    for(unsigned i=0;i<20;++i) {CHECK(!gRecovery.watchdogTick(gEnv.now));gEnv.now+=500000000ull;}
    /* In flight with progress every check: never fires. */
    gOps.retired=110;
    for(unsigned i=0;i<20;++i) {
        gHw.at(A830PowerWinGpu,G(A8XX_CP_RB_RPTR))=0x100+i*4;
        CHECK(!gRecovery.watchdogTick(gEnv.now));
        gEnv.now+=500000000ull;
    }
    /* Progress only in IB1_REM: still progress. */
    for(unsigned i=0;i<10;++i) {
        gPipeRegs[A8XX_CP_IB1_REM_SIZE]=0x400-i;
        CHECK(!gRecovery.watchdogTick(gEnv.now));
        gEnv.now+=500000000ull;
    }
    /* Stuck: 2 s without any change -> one report, pending. */
    unsigned fired=0;
    for(unsigned i=0;i<6;++i) {fired+=gRecovery.watchdogTick(gEnv.now);gEnv.now+=500000000ull;}
    CHECK(fired==1 && gRecovery.pending());
    CHECK(gRecovery.runPending()==kIOReturnSuccess && gRecovery.stats().watchdogHangs==1 && adreno830_status.hangs==1);
    /* Not active (slumber or recovering): no register is read. */
    A830PowerConfig full;full.slumber=A830SlumberFull;
    boot(A830RecoveryConfig{},full);
    gPower.markLost();
    const size_t logged=gHw.log.size();
    CHECK(!gRecovery.watchdogTick(gEnv.now) && gHw.log.size()==logged);
}

/* An idle GPU: no pending interrupt, RBBM_STATUS idle, everything retired. */
static void quiet()
{
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_INT_0_STATUS))=0;
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_STATUS))=0;
    gOps.retired=gOps.lastSubmitted;
}

static void testSlumberInteraction()
{
    /* A wake failure goes to the recovery through the fault sink; CX is off, so the recovery powers it up, restores
     * the SMMU and re-seeds the CX timer before the GMU, and starts at the DCVS target. */
    A830PowerConfig full;full.slumber=A830SlumberFull;
    boot(A830RecoveryConfig{},full);
    quiet();
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess && !gPower.cxOn() && gPower.cxLost());
    gPower.setTarget(1,2);
    gOps.fail["gmuStart"]=kIOReturnTimeout;
    CHECK(gPower.acquire()==A830ReturnNotResponding && gRecovery.pending() && gPower.phase()==A830PowerRecovering);
    /* The wake had powered CX up before the GMU failed: the recovery sees CX on and does the minimal path. */
    gOps.calls.clear();
    CHECK(gRecovery.runPending()==kIOReturnSuccess && gPower.phase()==A830PowerActive);
    CHECK(gOps.count("gmuForceOff")==1 && gOps.lastArgs.level==1 && gRecovery.snapshot().cause==A830FaultWake);
    CHECK(gOps.count("smmuRestore")==0 && gOps.count("cxTimerSeed")==0 && gOps.count("cxPowerOn")==0);   /* done by the wake */
    CHECK(gOps.lastArgs.cxLost);   /* but the GMU start after the collapse still reloads the whole firmware */
    CHECK(gStatus.lost.back()[2]==A830ReasonUnexpectedState);
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    /* CX power-up itself failed during the wake: the recovery powers up, restores SMMU and timer (CX was lost). */
    boot(A830RecoveryConfig{},full);
    quiet();
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess);
    gOps.fail["cxPowerOn"]=kIOReturnTimeout;
    CHECK(gPower.acquire()==A830ReturnNotResponding && gRecovery.pending());
    gOps.calls.clear();
    CHECK(gRecovery.runPending()==kIOReturnSuccess);
    const size_t on=gOps.index("cxPowerOn"),smmu=gOps.index("smmuRestore"),timer=gOps.index("cxTimerSeed"),gmu=gOps.index("gmuStart");
    CHECK(on<smmu && smmu<timer && timer<gmu && gOps.count("gmuForceOff")==0 && gOps.lastArgs.cxLost);
    CHECK(gPower.cxOn() && !gPower.cxLost());
    /* A slumber entry failure (S6) also recovers. */
    boot(A830RecoveryConfig{},full);
    quiet();
    gHw.at(A830PowerWinGmu,M(A8XX_GMUAO_GPU_CX_BUSY_STATUS))=A830_GMU_CX_BUSY_KGSL;
    CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnTimeout && gRecovery.pending());
    gHw.at(A830PowerWinGmu,M(A8XX_GMUAO_GPU_CX_BUSY_STATUS))=0;
    CHECK(gRecovery.runPending()==kIOReturnSuccess && gPower.phase()==A830PowerActive && gRecovery.snapshot().cause==A830FaultSlumber);
}

/* The bring-up profile: A830Recovery runs the same steps in both (what the annibale profile changes lives in the
 * operations it calls: secureResume keeps OOB_GPU set, cxPowerOn writes the AHB timeouts again). After an annibale
 * restart that kept OOB_GPU, the next slumber releases it with its own reference; the design profile has nothing held. */
static void testProfiles()
{
    std::vector<std::string> seen[A830ProfileCount];
    std::vector<Access> writes[A830ProfileCount];
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        adreno830_profile_value=profile;
        boot();
        CHECK(gRecovery.recover(A830FaultHang,0x800000)==kIOReturnSuccess && gPower.phase()==A830PowerActive);
        for(const auto &c:gOps.calls)if(c!="readPipe")seen[profile].push_back(c);
        for(const auto &a:gHw.log)if(a.write)writes[profile].push_back(a);
        CHECK(gOps.count("oobClear")==0 && gOps.count("cxPowerOn")==0);
    }
    CHECK(seen[A830ProfileDesign]==seen[A830ProfileAnnibale] && seen[A830ProfileDesign].size()==11);
    CHECK(writes[A830ProfileDesign].size()==writes[A830ProfileAnnibale].size());
    for(size_t i=0;i<writes[A830ProfileDesign].size();++i) {
        const Access &d=writes[A830ProfileDesign][i],&a=writes[A830ProfileAnnibale][i];
        CHECK(d.window==a.window && d.offset==a.offset && d.value==a.value);
    }
    /* P1 after the restart: annibale drops the kept reference (two clears), design one. */
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        adreno830_profile_value=profile;
        A830PowerConfig full;full.slumber=A830SlumberFull;
        boot(A830RecoveryConfig{},full);
        CHECK(gRecovery.recover(A830FaultHang,0)==kIOReturnSuccess);
        gOps.oobDepth=profile==A830ProfileAnnibale?1:0;   /* what gmuStart set and secureResume kept (annibale) */
        quiet();
        gOps.calls.clear();
        CHECK(gPower.enterSlumber(A830SlumberFull)==kIOReturnSuccess && gOps.oobDepth==0 &&
              gOps.count("oobClear")==(profile==A830ProfileAnnibale?2u:1u));
    }
    adreno830_profile_value=A830ProfileDesign;
}

int main()
{
    /* The design profile for the cases written for it; testProfiles compares the annibale one (the boot default). */
    adreno830_profile_value=A830ProfileDesign;
    testInit();
    testMinimalRecovery();
    testGxOff();
    testRateLimit();
    testCauseMatrix();
    testFailedRecovery();
    testReports();
    testWatchdog();
    testSlumberInteraction();
    testProfiles();
    std::printf("PASS: A830 recovery: gate, mask, snapshot, jobs failed, GBIF halt GX/client/arbiter, forceOff, S4-S7 "
                "without timer re-seed or SMMU while CX stayed up, fence waiters, irq restore; 3rd in 60 s and failed "
                "restarts final; reports coalesce; 2 s watchdog; slumber/wake faults recover; the same restart in the "
                "annibale and design profiles, the annibale slumber after it releasing the kept OOB_GPU (%u checks)\n",gChecks);
    return 0;
}
