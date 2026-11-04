/* SPDX-License-Identifier: MIT */
/* Overlay rows of the A830 status block: exact layouts of ARCHITECTURE 8.4 for every row-0 state and every stage page,
 * the 66-character limit for every row kind (worst cases and random blocks), the reason codes and step formats, the
 * irq_off / sticky / qmp fields, the run-time writers, and the publish rule (seq, the overlay and the registry hook move
 * only when the text changes). The bring-up profile suffix of row 0 (p0/p1 once S0 accepted pagani-gpu-profile, left out
 * whole when the row has no room) is counted on a line of its own before the pinned one, and so are the S5 page's annibale
 * fields (TCM words read back, GENERAL_11, RSC status, the AO status from its sticky word) and the late HFI drain
 * (hfi_late, "L<messages>/<acks>/<errors>" on the S5, S6/S7 and S8 pages), and the ring's queue period at the end of the
 * run page's IRQ row (adreno830_status_queue: exact layouts, the 66-character worst case, the writers), and the design
 * profile's thermal loop on the DCVS row (adreno830_status_thermal: " T<target><trend>[!]", aux[3] bits 40-50).
 * host-test: Adreno830Status.cpp */
#include "XnuMemoryFixture.h"
#include "Adreno830Status.h"
#include "A830Profile.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
/* The render server's record, word by word, where the plugin's checkout sits beside this one (run-host-tests.py's default). */
#if __has_include("../../UniXNU/drivers/AdrenoMetal/FrontendSecond.h")
#include "../../UniXNU/drivers/AdrenoMetal/FrontendSecond.h"
#define SAME_WORD(field) static_assert(offsetof(frontendsecond::Record,field)==offsetof(Adreno830StatusFrontend,field),#field)
static_assert(sizeof(frontendsecond::Record)==sizeof(Adreno830StatusFrontend),"the frontend's record");
SAME_WORD(states);SAME_WORD(state_us);SAME_WORD(state_max_us);SAME_WORD(makings);SAME_WORD(making_us);SAME_WORD(making_max_us);
SAME_WORD(clean_us);SAME_WORD(clean_max_us);
#endif

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

static std::vector<std::vector<std::string>> overlay;   /* every boot_progress_gpu_rows call */
static std::vector<std::pair<std::string,uint64_t>> history;
static unsigned registry;                               /* native_adreno_publish_status calls */
static std::vector<std::string> report;                 /* what native_adreno_report_rows answers */
extern "C" void boot_progress_gpu_rows(const char (*rows)[Adreno830StatusRowBytes],unsigned count)
{
    std::vector<std::string> copy;
    for(unsigned i=0;i<count;++i)copy.emplace_back(rows[i]);
    overlay.push_back(copy);
}
extern "C" void boot_progress_display_status(const char *detail,uint64_t value) {history.emplace_back(detail,value);}
/* BootProgress's ramoops log (the meter line, M14 P0): on or off, and every line written. */
static int logOn;
static std::vector<std::string> logged,loggedSeconds;   /* the meter line, the per-second line */
extern "C" int boot_progress_trace_enabled(void) {return logOn;}
extern "C" void boot_progress_trace(const char *text)
{
    CHECK(logOn && text);
    (std::string(text).rfind("gpu sec ",0)==0?loggedSeconds:logged).emplace_back(text);
}
extern "C" void native_adreno_publish_status(const Adreno830Status *status) {CHECK(status==&adreno830_status);++registry;}
extern "C" unsigned native_adreno_report_rows(char rows[3][Adreno830StatusRowBytes])
{
    for(unsigned i=0;i<report.size() && i<3;++i) {
        std::memset(rows[i],'x',Adreno830StatusRowBytes);   /* an over-long row must be cut by the status block */
        std::memcpy(rows[i],report[i].c_str(),std::min<size_t>(report[i].size()+1,Adreno830StatusRowBytes));
    }
    return unsigned(report.size());
}

static unsigned checked;
static std::vector<std::string> format(const Adreno830Status &status,unsigned mode=2,unsigned *count=nullptr)
{
    char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
    std::memset(rows,0x5a,sizeof rows);
    const unsigned n=adreno830_status_format(&status,rows,mode);
    if(count)*count=n;
    std::vector<std::string> result;
    for(auto &row:rows) {
        const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
        CHECK(end && end-row<=int(Adreno830StatusRowMax));
        result.emplace_back(row);
        ++checked;
    }
    return result;
}
static Adreno830Status blank()
{
    Adreno830Status status={};
    status.magic=Adreno830StatusMagic;status.version=Adreno830StatusVersion;status.bytes=sizeof status;
    return status;
}
static unsigned layouts;
static void expectRows(const Adreno830Status &status,const char *r0,const char *r1=nullptr,const char *r2=nullptr,
                       const char *r3=nullptr)
{
    const auto rows=format(status);
    const char *want[4]={r0,r1,r2,r3};
    for(unsigned i=0;i<4;++i)
        if(want[i] && rows[i]!=want[i]) {std::fprintf(stderr,"row %u '%s' != '%s'\n",i,rows[i].c_str(),want[i]);std::exit(1);}
    ++layouts;
}

/* A queue period of the IRQ row's cases (the job attribution figures, added later, all zero). */
static Adreno830StatusQueue period(uint32_t submitted,uint32_t completed,uint32_t missed,uint32_t avg_us,uint32_t max_us,uint32_t slow,
                                   uint32_t depth,uint32_t polled)
{
    Adreno830StatusQueue q{};
    q.submitted=submitted;q.completed=completed;q.missed=missed;q.avg_us=avg_us;q.max_us=max_us;q.slow=slow;q.depth=depth;q.polled=polled;
    return q;
}

int main()
{
    unsigned limits=0,publishes=0,writers=0;
    /* ARCHITECTURE 8.4, row 0, one example per state. */
    Adreno830Status s=blank();
    s.stage=A830SGmu;s.state=A830Busy;s.step=7;s.boot_ms=12;
    expectRows(s,"GPU S5.07 GMU-HFI RUN 12ms");
    s.state=A830Failed;s.ioreturn=0xe00002d6;s.reg=0x3d7e070;s.value=0;s.reason=A830ReasonTimeout;
    expectRows(s,"GPU S5.07 GMU-HFI FAIL E00002D6 3D7E070=00000000 r5");
    s=blank();s.stage=A830SSmmu;s.state=A830Stopped;s.boot_ms=41;
    expectRows(s,"GPU S3 SMMU STOP ok 41ms");
    s=blank();s.stage=A830SInputs;s.state=A830Stopped;
    expectRows(s,"GPU S0 INPUTS STOP ok 0ms");
    s=blank();s.stage=A830SRunning;s.state=A830Done;s.mhz=525;s.busy_permille=375;s.gmu_level=6;s.bus_level=4;s.submits=123456;
    expectRows(s,"GPU RUN 525M b37 g6/b4 s123456 f0 h0");
    s.irq_off=A830IrqOffHfi;   /* an interrupt the storm guard turned off shows as reason 23 */
    expectRows(s,"GPU RUN 525M b37 g6/b4 s123456 f0 h0 r23");
    s=blank();s.stage=A830SFence;s.state=A830Lost;s.step=2;s.ioreturn=0xe00002d8;s.reason=A830ReasonHang;s.faults=1;
    expectRows(s,"GPU LOST S8.02 E00002D8 r18 f1");
    s=blank();s.stage=A830SCp;s.state=A830Recovering;s.recoveries=1;s.boot_ms=85;
    expectRows(s,"GPU RECOVER 1 S6 85ms");
    s=blank();s.stage=A830SRunning;s.state=A830Slumber;s.aux[5]=12|uint64_t(37)<<24|uint64_t(4)<<48;
    expectRows(s,"GPU SLP 12s w37 4ms");
    s=blank();
    expectRows(s,"GPU S0 INPUTS IDLE");
    s=blank();s.stage=A830SDcvs;s.state=A830Stopped;s.boot_ms=512;
    expectRows(s,"GPU S10 DCVS STOP ok 512ms");
    /* Step numbers are hexadecimal, at least two digits (S3 CB2 steps are 0x2nn); 64-bit values print 16 digits and
     * physical addresses at most 9; reason 22 (GMU host resource) as r22. */
    s=blank();s.stage=A830SSmmu;s.state=A830Failed;s.step=0x203;s.ioreturn=0xe00002bc;s.reg=0x3da8000;
    s.value=0x0000000100000167ULL;s.reason=A830ReasonReadback;
    expectRows(s,"GPU S3.203 SMMU FAIL E00002BC 3DA8000=0000000100000167 r7");
    s=blank();s.stage=A830SGmu;s.state=A830Failed;s.step=0x18;s.ioreturn=0xe00002c7;s.reg=0x0014;s.value=20;
    s.reason=A830ReasonGmuHostResource;
    expectRows(s,"GPU S5.18 GMU-HFI FAIL E00002C7 14=00000014 r22");
    CHECK(A830ReasonGmuHostResource==22 && A830ReasonIrqStorm==23 && A830ReasonStopped==21 && A830ReasonUnsupported==24 &&
          A830ReasonCount==25);

    /* The S0 page: every observation is "-" until its stage recorded it. */
    s=blank();s.stage=A830SInputs;s.state=A830Stopped;s.aux[0]=A830S0FirmwareChecked|A830S0FirmwareOk;
    expectRows(s,nullptr,"SOC - FC - BIN - FW ok","ARC gfx - mx - cx - gmxc -","BCM SH0 - MC0 - ACV - CN0 -");
    s.aux[0]=A830S0FirmwareChecked;
    expectRows(s,nullptr,"SOC - FC - BIN - FW bad");
    s.aux[0]=A830S0SocId|A830S0FeatureCode|A830S0SpeedBin|A830S0FirmwareChecked|A830S0FirmwareOk|A830S0Arc|A830S0Bcm;
    s.soc_id=618;s.feature_code=0;s.speed_bin=3;s.aux[1]=0xff0f0f10;s.aux[2]=1000|uint64_t(4)<<32;s.aux[3]=1000|uint64_t(4)<<32;
    s.aux[4]=0x0200;
    expectRows(s,nullptr,"SOC 618 FC 0 BIN 3 FW ok","ARC gfx 16 mx 15 cx 15 gmxc -","BCM SH0 1000/4 MC0 1000/4 ACV 0 CN0 2");
    /* S1/S2: nothing recorded yet, then the power trace, the SCM call and a TCS transaction. */
    s=blank();s.stage=A830SPower;s.state=A830Busy;
    expectRows(s,nullptr,"PWR st- - PLL -","SCM x0 - x1 - n- FUSE -","TCS - st- c- - -");
    s.aux[0]=7|uint64_t(1)<<8|uint64_t(0x93e8)<<32;s.aux[1]=0x80000000|uint64_t(0x80000007)<<32;
    s.aux[2]=0;s.aux[3]=0|uint64_t(1)<<32;s.aux[4]=0x7;
    s.aux[5]=A830StatusTcs(0,1,0x10100,0x10100,0)|A830S1Power|A830S1Scm|A830S1Tcs;
    expectRows(s,nullptr,"PWR st7 GPUCC+93E8=80000000 PLL 80000007","SCM x0 0 x1 0 n1 FUSE 00000007","TCS 0 st1 c10100 10100 0");
    s.stage=A830SSecureCx;s.aux[2]=uint64_t(int64_t(-4));s.aux[3]=0x12|uint64_t(3)<<32;
    s.aux[5]=A830StatusTcs(0xf,0,0,0,0)|A830S1Scm|A830S1Tcs;
    expectRows(s,nullptr,"PWR st- - PLL -","SCM x0 FFFFFFFC x1 12 n3 FUSE 00000007","TCS - st0 c0 0 0");
    /* S3. */
    s=blank();s.stage=A830SSmmu;s.state=A830Stopped;
    s.aux[0]=0x4c017e18|uint64_t(0x40000015)<<32;s.aux[1]=0x5511|uint64_t(0x002e0406)<<32;s.aux[2]=0|uint64_t(2)<<32;
    s.aux[3]=0x300|uint64_t(0|3<<8|1<<16)<<32;s.aux[4]=0x167|uint64_t(0x303)<<32;s.aux[5]=0x167|uint64_t(0x303)<<32;
    s.sticky[A830StickyCb0Fsr]=0;s.sticky[A830StickyCb2Fsr]=0x400;s.smmu_faults=0;
    expectRows(s,"GPU S3 SMMU STOP ok 0ms","SMMU ID 4C017E18 40000015 00005511 CR0 002E0406",
               "ACR 00000000 GFSR 00000002 SLOT 0/3/1 S2C 00000300","CB0 00000167 00000303 FSR 00000000 CB2 00000167 00000400 F0");
    /* S4. */
    s=blank();s.stage=A830SFirmware;s.state=A830Busy;s.step=1;
    s.aux[0]=0x123456789abcdefULL;s.aux[1]=42;s.aux[2]=15|uint64_t(0x76000)<<32;s.aux[3]=0x1b510|uint64_t(0x62f8)<<32;s.aux[4]=0x1ec;
    expectRows(s,"GPU S4.01 FWMEM RUN 0ms","CXT 0123456789ABCDEF SKEW 42","GMU BLK 15 DDR 76000 TCM 1EC","SQE 1B510 AQE 62F8");
    /* S5: messages not sent print "-". */
    s=blank();s.stage=A830SGmu;s.state=A830Stopped;
    s.aux[0]=0x50010019|uint64_t(0x20070001)<<32;s.aux[1]=0x100|uint64_t(37)<<32;
    s.aux[2]=0|uint64_t(0)<<16|uint64_t(0)<<32|uint64_t(0)<<48;s.aux[3]=0|uint64_t(8)<<16|uint64_t(150)<<32;
    s.aux[4]=0|uint64_t(0)<<32;s.aux[5]=0;
    expectRows(s,nullptr,"GMU FW 50010019 HFI 20070001 INIT 00000100 37ms","HFI M1 0 M2 0 M6 0 M8 0 M9 0",
               "OOB 150us GX 00000000 RPMH 0 AO 00000000");
    s.aux[2]=0|uint64_t(0)<<16|uint64_t(A830HfiNotSent)<<32|uint64_t(A830HfiNotSent)<<48;s.aux[3]=A830HfiNotSent;
    expectRows(s,nullptr,nullptr,"HFI M1 0 M2 0 M6 - M8 - M9 -");
    /* S6/S7. */
    s=blank();s.stage=A830SCp;s.state=A830Stopped;
    s.aux[0]=10|uint64_t(10)<<32;s.aux[1]=0;s.aux[2]=0xe3|uint64_t(0x4a)<<32;s.aux[3]=0;s.aux[4]=0;s.aux[5]=0;
    expectRows(s,nullptr,"CP RPTR 10 WPTR 10 RBBM 00000000 HW 0","SQE PC E3/4A PROT 0 UNMASK 0","ZAP x0 0 x1 0 step 0 wq 0");
    s.stage=A830SZap;s.aux[4]=0;s.aux[5]=0|uint64_t(12)<<32|uint64_t(2)<<40|uint64_t(5)<<48;
    expectRows(s,nullptr,nullptr,nullptr,"ZAP x0 0 x1 0 step 12 wq 2");
    /* S8: V9b/V9c "-" unless the cohtest image ran them. */
    s=blank();s.stage=A830SFence;s.state=A830Stopped;
    s.aux[0]=0xa003|uint64_t(35)<<32;s.aux[1]=3;s.aux[2]=A830CohPass|A830CohFail<<2;
    s.aux[3]=0x0000000100000002ULL;s.aux[4]=0x0000000100000003ULL;
    expectRows(s,nullptr,"IRQ 332 rb A003 n3 wake 35us off 00000000","COH V4 p V9a f V9b - V9c -",
               "TS CP 00000001:00000002 AO 00000001:00000003");
    s.aux[2]=A830CohPass|A830CohPass<<2|A830CohPass<<4|A830CohFail<<6;s.irq_off=A830IrqOffHfi|A830IrqOffScmWaitq;
    expectRows(s,nullptr,"IRQ 332 rb A003 n3 wake 35us off 00000042","COH V4 p V9a p V9b p V9c f");
    /* The run page: 9 DCVS levels fit; more are cut with '+'. */
    s=blank();s.stage=A830SRunning;s.state=A830Done;
    {
        const uint16_t residency[9]={900,20,10,10,10,10,20,10,10};
        uint64_t w[3]={};
        for(unsigned i=0;i<9;++i)w[i/6]|=uint64_t(residency[i])<<((i%6)*10);
        s.aux[0]=160|uint64_t(734)<<16|uint64_t(0)<<32|uint64_t(9)<<40;s.aux[1]=w[0];s.aux[2]=w[1];s.aux[3]=w[2];
    }
    s.aux[4]=512|uint64_t(37)<<16|uint64_t(1)<<32|uint64_t(5)<<34;s.irqs=100;s.waits=50;s.wait_timeouts=0;s.late_wakeups=1;
    s.wptr_retries=0;
    expectRows(s,nullptr,"DCVS 160-734 t900/20/10/10/10/10/20/10/10","MEM pool 512 used 37 imp 5 coh 1",
               "IRQ 100 wait 50 to 0 late 1 wptr 0");
    s.aux[0]=160|uint64_t(1200)<<16|uint64_t(0)<<32|uint64_t(16)<<40;s.aux[1]=~0ull;s.aux[2]=~0ull;s.aux[3]=~0ull;
    CHECK(format(s)[1].back()=='+' && format(s)[1].size()<=Adreno830StatusRowMax);
    /* S9 has no page of its own: the raw words. */
    s=blank();s.stage=A830SMetal;s.state=A830Busy;s.aux[0]=0x1122334455667788ULL;s.aux[5]=1;
    expectRows(s,nullptr,"AUX0 1122334455667788 AUX1 0000000000000000",nullptr,"AUX4 0000000000000000 AUX5 0000000000000001");

    /* Overlay row counts per mode: 0 none, 1 row 0, 2 and 3 all four. */
    unsigned count=9;
    format(s,0,&count);CHECK(count==0);
    format(s,1,&count);CHECK(count==1);
    format(s,2,&count);CHECK(count==4);
    format(s,3,&count);CHECK(count==4);
    CHECK(adreno830_status_format(nullptr,nullptr,2)==0);

    /* Worst cases of every row kind on every page fit uncut in 66 characters. */
    const uint64_t big=~0ULL;
    const uint8_t states[]={A830Idle,A830Busy,A830Done,A830Failed,A830Stopped,A830Lost,A830Recovering,A830Slumber};
    for(uint8_t stage=0;stage<=A830SRunning;++stage)
        for(uint8_t state:states) {
            if(stage>A830SDcvs && stage!=A830SRunning)continue;
            s=blank();s.stage=stage;s.state=state;s.step=0xffff;s.reason=A830ReasonCount-1;s.ioreturn=0xffffffff;
            s.reg=big;s.value=big;s.expect=big;s.boot_ms=UINT32_MAX;s.mhz=UINT32_MAX;s.busy_permille=UINT32_MAX;
            s.gmu_level=UINT32_MAX;s.bus_level=UINT32_MAX;s.submits=999999999999ULL;s.faults=UINT32_MAX;s.hangs=UINT32_MAX;
            s.recoveries=UINT32_MAX;s.soc_id=UINT32_MAX;s.feature_code=UINT32_MAX;s.speed_bin=UINT32_MAX;
            s.irqs=big;s.waits=big;s.wait_timeouts=big;s.late_wakeups=big;s.wptr_retries=UINT32_MAX;s.smmu_faults=UINT32_MAX;
            s.irq_off=UINT32_MAX;
            for(auto &v:s.sticky)v=UINT32_MAX;
            for(auto &word:s.aux)word=big;
            if(stage==A830SInputs) {
                s.aux[0]=A830S0SocId|A830S0FeatureCode|A830S0SpeedBin|A830S0FirmwareChecked|A830S0FirmwareOk|A830S0Arc|A830S0Bcm;
                s.aux[1]=0xfefefefe;
            }
            const auto rows=format(s);
            for(const auto &row:rows) {
                CHECK(!row.empty() && row.size()<=Adreno830StatusRowMax);
                ++limits;
            }
            if(state==A830Failed && stage==A830SGmu)
                CHECK(rows[0]=="GPU S5.FFFF GMU-HFI FAIL FFFFFFFF FFFFFFFFF=FFFFFFFFFFFFFFFF r24");
            if(stage==A830SRunning && state==A830Done)   /* exactly 66: no room for r23 */
                CHECK(rows[0]=="GPU RUN 9999M b100 g255/b255 s999999999999 f4294967295 h4294967295" && rows[0].size()==66);
            if(stage==A830SInputs)
                CHECK(rows[3]=="BCM SH0 4294967295/65535 MC0 4294967295/65535 ACV 255 CN0 255");
            if(stage==A830SSmmu)
                CHECK(rows[3]=="CB0 FFFFFFFF FFFFFFFF FSR FFFFFFFF CB2 FFFFFFFF FFFFFFFF F99999999" && rows[3].size()==66);
        }
    /* Arbitrary contents never overrun a row. */
    std::mt19937_64 random(0x830);
    for(unsigned i=0;i<20000;++i) {
        s=blank();
        unsigned char *bytes=reinterpret_cast<unsigned char *>(&s);
        for(size_t b=0;b<offsetof(Adreno830Status,rows);++b)bytes[b]=uint8_t(random());
        format(s,unsigned(random()%5));
        ++limits;
    }

    /* Publish: seq, the overlay and the registry hook move only when the text changes. */
    adreno830_status_reset();
    adreno830_status_mode(2);
    CHECK(adreno830_status_current_mode()==2);
    const uint32_t seq0=adreno830_status.seq;
    adreno830_status_begin(A830SInputs);                  /* from Idle: no publish */
    CHECK(adreno830_status.seq==seq0 && overlay.empty() && registry==0);
    adreno830_status_end(A830SInputs,1500);
    CHECK(adreno830_status.boot_ms==1);
    adreno830_status_stop(A830SInputs,A830ReasonStopped);
    CHECK(adreno830_status.seq==seq0+1 && overlay.size()==1 && overlay[0].size()==4 && registry==1 &&
          overlay[0][0]=="GPU S0 INPUTS STOP ok 1ms" && std::string(adreno830_status.rows[0])==overlay[0][0]);
    adreno830_status_publish();                           /* same text */
    CHECK(adreno830_status.seq==seq0+1 && overlay.size()==1 && registry==1);
    adreno830_status.aux[0]=A830S0FirmwareChecked|A830S0FirmwareOk;   /* a page row changes */
    adreno830_status_publish();
    CHECK(adreno830_status.seq==seq0+2 && overlay.size()==2 && overlay[1][1]=="SOC - FC - BIN - FW ok" && registry==2);
    adreno830_status_begin(A830SPower);                   /* a stage switch publishes and clears the previous page */
    CHECK(adreno830_status.seq==seq0+3 && overlay.size()==3 && overlay[2][0]=="GPU S1.00 POWER RUN 1ms" &&
          overlay[2][1]=="PWR st- - PLL -" && adreno830_status.aux[0]==0);
    adreno830_status.aux[2]=5;adreno830_status.aux[5]=A830S1Scm;
    adreno830_status_begin(A830SSecureCx);                /* S1 and S2 share a page: their words stay */
    CHECK(adreno830_status.aux[2]==5 && overlay.back()[2]=="SCM x0 5 x1 0 n0 FUSE 00000000");
    adreno830_status_step(4);                             /* steps are stored, not published */
    CHECK(adreno830_status.seq==seq0+4 && adreno830_status.step==4);
    adreno830_status_fail(A830SPower,6,A830ReasonTimeout,IOReturn(0xe00002d6),0x3d90000,0x80000000,0);
    CHECK(adreno830_status.seq==seq0+5 && overlay.size()==5 && overlay[4][0]=="GPU S1.06 POWER FAIL E00002D6 3D90000=80000000 r5");
    CHECK(history.size()==1 && history[0].first=="GPU power" && history[0].second==0xe00002d6);
    publishes+=5;
    /* Mode 1: one row; mode 0: the rows and seq still move, the overlay is never called. */
    adreno830_status_reset();adreno830_status_mode(1);
    adreno830_status_stop(A830SInputs,A830ReasonNone);
    CHECK(overlay.size()==6 && overlay[5].size()==1 && overlay[5][0]=="GPU S0 INPUTS STOP ok 0ms" && adreno830_status.seq==seq0+6);
    adreno830_status_reset();adreno830_status_mode(0);
    adreno830_status_stop(A830SInputs,A830ReasonNone);
    CHECK(overlay.size()==6 && adreno830_status.seq==seq0+7 && std::string(adreno830_status.rows[0])=="GPU S0 INPUTS STOP ok 0ms");
    adreno830_status_mode(7);adreno830_status_publish();  /* clamped to 3: the count changes, so the overlay redraws */
    CHECK(overlay.size()==7 && overlay[6].size()==4 && adreno830_status.seq==seq0+8 && adreno830_status_current_mode()==3);
    publishes+=3;
    /* Mode 3: the plugin's report rows replace the diagnostic page once there are any; long rows are cut at 66. */
    report={"adrenoprob s34 e0 v4/4",std::string(80,'T'),"CLI o2 c31 s0 NGP 0/0/0"};
    adreno830_status_publish();
    CHECK(overlay.size()==8 && overlay[7][1]=="adrenoprob s34 e0 v4/4" && overlay[7][2]==std::string(66,'T') &&
          overlay[7][3]=="CLI o2 c31 s0 NGP 0/0/0");
    adreno830_status_publish();
    CHECK(overlay.size()==8);
    report.clear();
    adreno830_status_publish();                           /* back to the diagnostic page */
    CHECK(overlay.size()==9 && overlay[8][1]=="SOC - FC - BIN - FW -");
    publishes+=3;

    /* Run-time writers. */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();
    adreno830_status_running();
    CHECK(adreno830_status.stage==A830SRunning && adreno830_status.state==A830Done && overlay.back()[0]=="GPU RUN 0M b0 g0/b0 s0 f0 h0");
    const uint16_t residency[3]={500,300,200};
    adreno830_status_dcvs(222,375,2,2,160,734,0,3,residency);
    CHECK(overlay.back()[0]=="GPU RUN 222M b37 g2/b2 s0 f0 h0" && overlay.back()[1]=="DCVS 160-734 t500/300/200");
    const size_t before=overlay.size();
    adreno830_status_dcvs(222,375,2,2,160,734,0,3,residency);   /* unchanged: no publish */
    CHECK(overlay.size()==before);
    adreno830_status_memory(512,48,7,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 48 imp 7 coh 1");
    adreno830_status_irq_off(A830IrqOffSmmuCb0);                /* interrupt-safe: stored, shown at the next publish */
    CHECK(overlay.size()==before+1 && adreno830_status.irq_off==A830IrqOffSmmuCb0);
    adreno830_status_publish();
    CHECK(overlay.back()[0]=="GPU RUN 222M b37 g2/b2 s0 f0 h0 r23");
    adreno830_status_sticky(A830StickySmmuGfsr,2);adreno830_status_sticky(A830StickySmmuGfsr,0);adreno830_status_sticky(9,1);
    CHECK(adreno830_status.sticky[A830StickySmmuGfsr]==2);
    adreno830_status_add32(&adreno830_status.faults,1);adreno830_status_add64(&adreno830_status.submits,5);
    adreno830_status_lost(A830SFence,2,A830ReasonHang,IOReturn(0xe00002d8));
    CHECK(overlay.back()[0]=="GPU LOST S8.02 E00002D8 r18 f1" && history.back().first=="GPU lost");
    adreno830_status.recoveries=1;
    adreno830_status_recovering(A830SCp,85);
    CHECK(overlay.back()[0]=="GPU RECOVER 1 S6 85ms" && overlay.back()[1].rfind("CP RPTR ",0)==0);
    adreno830_status_running();
    CHECK(overlay.back()[0]=="GPU RUN 222M b37 g2/b2 s5 f1 h0 r23");
    adreno830_status_slumber(12,37,4);
    CHECK(overlay.back()[0]=="GPU SLP 12s w37 4ms");
    adreno830_status.qmp=A830QmpTimeout;adreno830_status.coh_limit=1;
    CHECK(offsetof(Adreno830Status,qmp)==184 && A830QmpRefused==3);
    writers+=10;
    CHECK(adreno830_status.magic==Adreno830StatusMagic && adreno830_status.version==1 && adreno830_status.bytes==576);
    CHECK(std::strcmp(adreno830_stage_name(A830SGmu),"GMU-HFI")==0 && std::strcmp(adreno830_stage_name(A830SRunning),"RUN")==0 &&
          std::strcmp(adreno830_stage_name(200),"?")==0 && std::strcmp(adreno830_stage_title(A830SInputs),"GPU board inputs")==0);

    /* ---- Row 0 ends with the bring-up profile once S0 accepted it (pagani-gpu-profile, A830Profile.h) ------------------
     * Counted on their own line, so the pinned line below keeps describing the layouts it always counted. */
    unsigned profileLayouts=0,profileBounds=0,profileTransitions=0;
    auto row0=[&](const Adreno830Status &status) {
        char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
        std::memset(rows,0x5a,sizeof rows);
        CHECK(adreno830_status_format(&status,rows,2)==4);
        for(auto &row:rows) {
            const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
            CHECK(end && end-row<=int(Adreno830StatusRowMax));
        }
        return std::string(rows[0]);
    };
    auto expectProfile=[&](const Adreno830Status &status,const char *want) {
        const std::string got=row0(status);
        if(got!=want) {std::fprintf(stderr,"row 0 '%s' != '%s'\n",got.c_str(),want);std::exit(1);}
        ++profileLayouts;
    };
    s=blank();s.stage=A830SInputs;s.state=A830Stopped;
    expectProfile(s,"GPU S0 INPUTS STOP ok 0ms");          /* not accepted yet: no suffix */
    s.profile=1;
    expectProfile(s,"GPU S0 INPUTS STOP ok 0ms");          /* the value alone is not enough */
    s.profile_set=1;
    expectProfile(s,"GPU S0 INPUTS STOP ok 0ms p1");
    s.profile=0;
    expectProfile(s,"GPU S0 INPUTS STOP ok 0ms p0");
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SGmu;s.state=A830Busy;s.step=7;s.boot_ms=12;
    expectProfile(s,"GPU S5.07 GMU-HFI RUN 12ms p1");
    s.state=A830Failed;s.ioreturn=0xe00002d6;s.reg=0x3d7e070;s.value=0;s.reason=A830ReasonTimeout;
    expectProfile(s,"GPU S5.07 GMU-HFI FAIL E00002D6 3D7E070=00000000 r5 p1");
    /* The hwsched refusal of S0 comes before the profile is accepted: reason 24, no suffix. */
    s=blank();s.stage=A830SInputs;s.state=A830Failed;s.step=0x49;s.ioreturn=0xe00002c7;s.reg=24;s.value=1;
    s.reason=A830ReasonUnsupported;
    expectProfile(s,"GPU S0.49 INPUTS FAIL E00002C7 18=00000001 r24");
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SRunning;s.state=A830Done;s.mhz=342;s.busy_permille=375;s.gmu_level=3;
    s.bus_level=3;s.submits=123456;
    expectProfile(s,"GPU RUN 342M b37 g3/b3 s123456 f0 h0 p1");
    s.irq_off=A830IrqOffHfi;
    expectProfile(s,"GPU RUN 342M b37 g3/b3 s123456 f0 h0 r23 p1");
    s=blank();s.profile_set=1;s.stage=A830SFence;s.state=A830Lost;s.step=2;s.ioreturn=0xe00002d8;s.reason=A830ReasonHang;
    s.faults=1;
    expectProfile(s,"GPU LOST S8.02 E00002D8 r18 f1 p0");
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SCp;s.state=A830Recovering;s.recoveries=1;s.boot_ms=85;
    expectProfile(s,"GPU RECOVER 1 S6 85ms p1");
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SRunning;s.state=A830Slumber;s.aux[5]=12|uint64_t(37)<<24|uint64_t(4)<<48;
    expectProfile(s,"GPU SLP 12s w37 4ms p1");
    s=blank();s.profile_set=1;s.profile=1;
    expectProfile(s,"GPU S0 INPUTS IDLE p1");
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SZap;s.state=A830Done;s.boot_ms=950;
    expectProfile(s,"GPU S7 ZAP DONE 950ms p1");
    /* Worst cases with the widest profile value: every row kind stays within 66 characters, the longest FAIL and RUN rows
     * leave the suffix out rather than cutting a field. */
    for(uint8_t stage=0;stage<=A830SRunning;++stage)
        for(uint8_t state:states) {
            if(stage>A830SDcvs && stage!=A830SRunning)continue;
            for(const uint8_t profile:{uint8_t(0),uint8_t(1),uint8_t(255)}) {
                s=blank();s.stage=stage;s.state=state;s.step=0xffff;s.reason=A830ReasonCount-1;s.ioreturn=0xffffffff;
                s.reg=big;s.value=big;s.boot_ms=UINT32_MAX;s.mhz=UINT32_MAX;s.busy_permille=UINT32_MAX;s.gmu_level=UINT32_MAX;
                s.bus_level=UINT32_MAX;s.submits=999999999999ULL;s.faults=UINT32_MAX;s.hangs=UINT32_MAX;s.recoveries=UINT32_MAX;
                s.irq_off=UINT32_MAX;s.profile=profile;
                for(auto &word:s.aux)word=big;
                const std::string base=row0(s);   /* profile_set 0 */
                s.profile_set=1;
                const std::string row=row0(s);
                const std::string suffix=" p"+std::to_string(profile);
                CHECK(row==(base.size()+suffix.size()<=Adreno830StatusRowMax?base+suffix:base));
                if(stage==A830SGmu && state==A830Failed)CHECK(row==base && base.size()==64);   /* no room: left out whole */
                if(stage==A830SRunning && state==A830Done)CHECK(row==base && base.size()==66);
                if(state==A830Stopped)CHECK(row==base+suffix);
                ++profileBounds;
            }
        }
    /* Publishing: the suffix is text like any other (seq moves once), a reset forgets the profile. */
    adreno830_status_reset();adreno830_status_mode(1);overlay.clear();
    adreno830_status_stop(A830SInputs,A830ReasonStopped);
    const uint32_t seqProfile=adreno830_status.seq;
    CHECK(overlay.back()[0]=="GPU S0 INPUTS STOP ok 0ms");
    adreno830_status.profile=1;adreno830_status.profile_set=1;
    adreno830_status_publish();
    CHECK(adreno830_status.seq==seqProfile+1 && overlay.size()==2 && overlay.back()[0]=="GPU S0 INPUTS STOP ok 0ms p1");
    adreno830_status_publish();
    CHECK(adreno830_status.seq==seqProfile+1 && overlay.size()==2);
    adreno830_status_reset();
    CHECK(!adreno830_status.profile_set && !adreno830_status.profile);
    adreno830_status_stop(A830SInputs,A830ReasonStopped);
    CHECK(overlay.back()[0]=="GPU S0 INPUTS STOP ok 0ms");
    profileTransitions+=3;
    CHECK(offsetof(Adreno830Status,profile)==186 && offsetof(Adreno830Status,profile_set)==187);
    std::printf("PASS: profile suffix of row 0: %u exact layouts (p0/p1 in every state, none before S0 accepted it), %u worst "
                "cases within 66 characters, %u publish transitions\n",profileLayouts,profileBounds,profileTransitions);

    /* ---- The S5 page's annibale fields (TCM words read back, GENERAL_11, RSC status) and the late HFI drain (hfi_late) on the
     * S5, S6/S7 and S8 pages, counted on their own line before the pinned one. ---------------------------------------------- */
    unsigned wiringLayouts=0,wiringBounds=0;
    auto pageRows=[&](const Adreno830Status &status) {
        char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
        std::memset(rows,0x5a,sizeof rows);
        CHECK(adreno830_status_format(&status,rows,2)==4);
        std::vector<std::string> out;
        for(auto &row:rows) {
            const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
            CHECK(end && end-row<=int(Adreno830StatusRowMax));
            out.emplace_back(row);
        }
        return out;
    };
    auto expectPage=[&](const Adreno830Status &status,const char *r1,const char *r2,const char *r3) {
        const auto rows=pageRows(status);
        const char *want[3]={r1,r2,r3};
        for(unsigned i=0;i<3;++i)
            if(want[i] && rows[1+i]!=want[i]) {
                std::fprintf(stderr,"row %u '%s' != '%s'\n",1+i,rows[1+i].c_str(),want[i]);
                std::exit(1);
            }
        ++wiringLayouts;
    };
    CHECK(A830StatusLate(1,2,3,4)==0x04030201u && A830StatusLate(300,256,255,1000)==0xffffffffu &&
          offsetof(Adreno830Status,hfi_late)==20 && offsetof(Adreno830Status,gmu_general11)==188);
    /* S5 in the annibale profile: TCM words (aux[1] bits 48-63), GENERAL_11, RSC_STATUS0_DRV0 (aux[5] low); the AO status is
     * the sticky word, the init ms are aux[1] bits 32-47. */
    s=blank();s.stage=A830SGmu;s.state=A830Stopped;s.profile_set=1;s.profile=A830ProfileAnnibale;
    s.aux[0]=0x50010019|uint64_t(0x20070001)<<32;s.aux[1]=0x100|uint64_t(37)<<32|uint64_t(139)<<48;s.aux[3]=uint64_t(150)<<32;
    s.aux[5]=0x00010000|uint64_t(0x00800000)<<32;s.gmu_general11=0;s.sticky[A830StickyGmuAo]=0x8;
    expectPage(s,"GMU FW 50010019 HFI 20070001 INIT 00000100 37ms TCM 139","HFI M1 0 M2 0 M6 0 M8 0 M9 0 G11 00000000",
               "OOB 150us GX 00000000 RPMH 0 AO 00000008 RSC 00010000");
    /* A late drain ran (an S5 command that failed drains itself): L<messages>/<acks>/<errors>, each shown up to 99; a drain that
     * found nothing shows too. */
    s.hfi_late=A830StatusLate(1,3,1,1);
    expectPage(s,nullptr,"HFI M1 0 M2 0 M6 0 M8 0 M9 0 L3/1/1 G11 00000000",nullptr);
    s.hfi_late=A830StatusLate(255,255,120,0);s.gmu_general11=0x1234;
    expectPage(s,nullptr,"HFI M1 0 M2 0 M6 0 M8 0 M9 0 L99/99/0 G11 00001234",nullptr);
    s.hfi_late=A830StatusLate(1,0,0,0);
    expectPage(s,nullptr,"HFI M1 0 M2 0 M6 0 M8 0 M9 0 L0/0/0 G11 00001234",nullptr);
    /* The design profile, and a block before S0 accepted a profile, show none of the annibale fields whatever the words hold. */
    s.hfi_late=0;s.profile=A830ProfileDesign;
    expectPage(s,"GMU FW 50010019 HFI 20070001 INIT 00000100 37ms","HFI M1 0 M2 0 M6 0 M8 0 M9 0",
               "OOB 150us GX 00000000 RPMH 0 AO 00000008");
    s.profile=A830ProfileAnnibale;s.profile_set=0;
    expectPage(s,"GMU FW 50010019 HFI 20070001 INIT 00000100 37ms","HFI M1 0 M2 0 M6 0 M8 0 M9 0",
               "OOB 150us GX 00000000 RPMH 0 AO 00000008");
    /* S6/S7 and S8: the late drain of their failures (and of a recovery that re-runs them). */
    s=blank();s.stage=A830SCp;s.state=A830Failed;s.hfi_late=A830StatusLate(1,2,1,1);
    expectPage(s,nullptr,nullptr,"ZAP x0 0 x1 0 step 0 wq 0 L2/1/1");
    s.stage=A830SZap;s.hfi_late=0;
    expectPage(s,nullptr,nullptr,"ZAP x0 0 x1 0 step 0 wq 0");
    s=blank();s.stage=A830SFence;s.state=A830Failed;s.hfi_late=A830StatusLate(1,0,0,1);
    expectPage(s,nullptr,"COH V4 - V9a - V9b - V9c - L0/0/1",nullptr);
    s.stage=A830SRunning;s.state=A830Recovering;   /* the run page has no room: no L */
    CHECK(pageRows(s)[3].find(" L")==std::string::npos);
    /* Worst cases: every field at its widest (HFI statuses 0xFFFE: 0xFFFF prints "-"), uncut, within 66 characters. */
    for(const uint8_t stage:{uint8_t(A830SGmu),uint8_t(A830SCp),uint8_t(A830SZap),uint8_t(A830SFence)}) {
        s=blank();s.stage=stage;s.state=A830Failed;s.profile_set=1;s.profile=A830ProfileAnnibale;
        for(auto &word:s.aux)word=big;
        if(stage==A830SGmu) {s.aux[2]=0xfffefffefffefffeULL;s.aux[3]=0xfffffffffffffffeULL;}
        for(auto &v:s.sticky)v=UINT32_MAX;
        s.irq_off=UINT32_MAX;s.gmu_general11=UINT32_MAX;s.hfi_late=UINT32_MAX;
        const auto rows=pageRows(s);
        if(stage==A830SGmu) {
            CHECK(rows[1]=="GMU FW FFFFFFFF HFI FFFFFFFF INIT FFFFFFFF 65535ms TCM 65535" && rows[1].size()==60);
            CHECK(rows[2]=="HFI M1 FFFE M2 FFFE M6 FFFE M8 FFFE M9 FFFE L99/99/99 G11 FFFFFFFF" && rows[2].size()==66);
            CHECK(rows[3]=="OOB 999999us GX FFFFFFFF RPMH FFFFFFFF AO FFFFFFFF RSC FFFFFFFF" && rows[3].size()==63);
        } else if(stage==A830SFence) {
            CHECK(rows[2]=="COH V4 - V9a - V9b - V9c - L99/99/99");
        } else {
            CHECK(rows[3]=="ZAP x0 FFFFFFFF x1 FFFFFFFF step 255 wq 255 L99/99/99" && rows[3].size()==53);
        }
        ++wiringBounds;
    }
    std::printf("PASS: annibale GMU diagnostics and the late HFI drain: %u exact layouts (TCM, GENERAL_11 and RSC on the S5 page "
                "only in annibale, the AO status from its sticky word, L<m>/<a>/<e> on the S5, S6/S7 and S8 pages), %u worst cases "
                "within 66 characters\n",wiringLayouts,wiringBounds);

    /* ---- The performance knobs on the RUN row (pagani-gpu-maxlevel, pagani-gpu-irq: the run page's aux[0] bits 48-56,
     * A830StatusKnobs), counted on their own line before the pinned one. -------------------------------------------------- */
    unsigned knobLayouts=0,knobBounds=0,knobWriters=0;
    auto expectKnob=[&](const Adreno830Status &status,const char *want) {
        const std::string got=row0(status);
        if(got!=want) {std::fprintf(stderr,"row 0 '%s' != '%s'\n",got.c_str(),want);std::exit(1);}
        ++knobLayouts;
    };
    CHECK(A830StatusKnobs(9,1)==(uint64_t(9)|uint64_t(1)<<8)<<48 && A830StatusKnobs(300,7)==(uint64_t(255)|uint64_t(1)<<8)<<48 &&
          A830StatusKnobs(0,0)==0);
    s=blank();s.profile_set=1;s.profile=1;s.stage=A830SRunning;s.state=A830Done;s.mhz=734;s.busy_permille=990;s.gmu_level=9;
    s.bus_level=8;s.submits=3295;s.irq_off=A830IrqOffHfi|A830IrqOffGmu;
    s.aux[0]=160|uint64_t(734)<<16|uint64_t(11)<<40;
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 r23 p1");           /* both 0: the row of today */
    s.aux[0]|=A830StatusKnobs(9,0);
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 r23 p1 m9");
    s.aux[0]|=A830StatusKnobs(0,1);
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 r23 p1 m9 i1");     /* 332 live: irq_off bit 0 clear */
    s.irq_off|=A830IrqOffGpu;
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 r23 p1 m9 i0");     /* given, but polled */
    s.irq_off=0;s.aux[0]=(s.aux[0]&~(uint64_t(0xffff)<<48))|A830StatusKnobs(16,1);
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 p1 m16 i1");
    CHECK(pageRows(s)[1]=="DCVS 160-734 t0/0/0/0/0/0/0/0/0/0/0");         /* the DCVS row reads its own bits only */
    /* Only on the RUN row, and only once S0 accepted them (profile_set). */
    s.state=A830Slumber;s.aux[5]=12|uint64_t(37)<<24|uint64_t(4)<<48;
    expectKnob(s,"GPU SLP 12s w37 4ms p1");
    s.stage=A830SDcvs;s.state=A830Done;s.boot_ms=950;
    expectKnob(s,"GPU S10 DCVS DONE 950ms p1");
    s.stage=A830SRunning;s.profile_set=0;
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0");
    /* Worst cases: a part that does not fit whole is left out; the 66-character RUN row keeps none. */
    for(const uint64_t submits:{0ull,999999ull,99999999999ull,999999999999ull})
        for(const uint32_t faults:{0u,4294967295u})
            for(const uint32_t level:{1u,16u,255u}) {
                s=blank();s.profile_set=1;s.profile=1;s.stage=A830SRunning;s.state=A830Done;s.mhz=UINT32_MAX;
                s.busy_permille=UINT32_MAX;s.gmu_level=UINT32_MAX;s.bus_level=UINT32_MAX;s.submits=submits;s.faults=faults;
                s.hangs=faults;s.irq_off=UINT32_MAX;
                const std::string plain=row0(s);
                s.aux[0]=A830StatusKnobs(level,1);
                const std::string row=row0(s);
                std::string want=plain;
                const std::string m=" m"+std::to_string(level);
                if(want.size()+m.size()<=Adreno830StatusRowMax)want+=m;
                if(want.size()+3<=Adreno830StatusRowMax)want+=" i0";   /* irq_off all ones: polled */
                CHECK(row==want && row.size()<=Adreno830StatusRowMax);
                if(submits==999999999999ull && faults)CHECK(row==plain && row.size()==66);
                ++knobBounds;
            }
    /* The writers: S0's adreno830_status_knobs is only stored; every adreno830_status_dcvs carries it into the run page's
     * aux[0]; the storm guard's irq_off bit turns i1 into i0; a reset forgets the knobs. */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();
    adreno830_status.profile=1;adreno830_status.profile_set=1;
    adreno830_status_knobs(11,1);
    adreno830_status_running();
    CHECK(overlay.back()[0]=="GPU RUN 0M b0 g0/b0 s0 f0 h0 p1");        /* shown from the first DCVS row on */
    const uint16_t eleven[11]={0,0,0,0,0,0,0,0,1000,0,0};
    adreno830_status_dcvs(734,990,9,8,160,734,1,11,eleven);
    CHECK(overlay.back()[0]=="GPU RUN 734M b99 g9/b8 s0 f0 h0 p1 m11 i1" &&
          overlay.back()[1]=="DCVS 160-734 t0/0/0/0/0/0/0/0/1000/0/0" && (adreno830_status.aux[0]>>48)==(11u|1u<<8));
    adreno830_status_irq_off(A830IrqOffGpu);
    adreno830_status_publish();
    CHECK(overlay.back()[0]=="GPU RUN 734M b99 g9/b8 s0 f0 h0 r23 p1 m11 i0");
    adreno830_status_reset();adreno830_status.profile=1;adreno830_status.profile_set=1;
    adreno830_status_running();
    adreno830_status_dcvs(342,990,3,3,160,342,1,3,eleven);
    CHECK(overlay.back()[0]=="GPU RUN 342M b99 g3/b3 s0 f0 h0 p1" && (adreno830_status.aux[0]>>48)==0);
    knobWriters+=4;
    /* The GPU temperature after the knobs: whole degrees (rounded), only once a reading was valid, clamped to 127, left out
     * when it does not fit; the DCVS row carries it like the knobs and a reset forgets it. */
    CHECK(A830StatusTemp(523)==uint64_t(52)<<57 && A830StatusTemp(525)==uint64_t(53)<<57 && A830StatusTemp(0)==0 &&
          A830StatusTemp(-40)==0 && A830StatusTemp(2000)==uint64_t(127)<<57);
    s=blank();s.profile_set=1;s.profile=0;s.stage=A830SRunning;s.state=A830Done;s.mhz=734;s.busy_permille=990;s.gmu_level=9;
    s.bus_level=8;s.submits=3295;s.aux[0]=160|uint64_t(734)<<16|uint64_t(11)<<40|A830StatusTemp(523);
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 p0 52C");
    s.aux[0]|=A830StatusKnobs(9,1);
    expectKnob(s,"GPU RUN 734M b99 g9/b8 s3295 f0 h0 p0 m9 i1 52C");
    s.submits=999999999999ull;s.faults=s.hangs=4294967295u;s.irq_off=UINT32_MAX;s.mhz=s.busy_permille=s.gmu_level=s.bus_level=UINT32_MAX;
    CHECK(row0(s).size()<=Adreno830StatusRowMax && row0(s).find('C')==std::string::npos);
    adreno830_status_reset();adreno830_status.profile=0;adreno830_status.profile_set=1;
    adreno830_status_running();
    adreno830_status_temperature(456);
    adreno830_status_dcvs(607,700,7,6,160,734,1,11,eleven);
    CHECK(overlay.back()[0]=="GPU RUN 607M b70 g7/b6 s0 f0 h0 p0 46C" && overlay.back()[1]=="DCVS 160-734 t0/0/0/0/0/0/0/0/1000/0/0");
    adreno830_status_reset();adreno830_status.profile=0;adreno830_status.profile_set=1;
    adreno830_status_running();
    adreno830_status_dcvs(607,700,7,6,160,734,1,11,eleven);
    CHECK(overlay.back()[0]=="GPU RUN 607M b70 g7/b6 s0 f0 h0 p0");
    knobWriters+=2;
    /* The MEM row's vm: MiB mapped in the GPU VM (rounded up), appended once it is not 0, carried by adreno830_status_memory
     * next to the pool's MiB in use, forgotten on reset; the object count keeps 15 bits. */
    adreno830_status_reset();adreno830_status.profile=0;adreno830_status.profile_set=1;
    adreno830_status_running();
    adreno830_status_vm(uint64_t(812)<<20);
    adreno830_status_memory(512,380,1402,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 380 imp 1402 coh 1 vm 812");
    adreno830_status_vm(1);
    adreno830_status_memory(512,380,16384,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 380 imp 16384 coh 1 vm 1");
    adreno830_status_vm(~uint64_t(0)>>1);
    adreno830_status_memory(65535,65535,99999,3);
    CHECK(overlay.back()[2]=="MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767" && overlay.back()[2].size()<=Adreno830StatusRowMax);
    adreno830_status_reset();adreno830_status.profile=0;adreno830_status.profile_set=1;
    adreno830_status_running();
    adreno830_status_memory(512,0,6,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 0 imp 6 coh 1");
    knobWriters+=4;
    std::printf("PASS: performance knobs on the RUN row: %u exact layouts (m<n>, i1 live, i0 polled, only on RUN once S0 "
                "accepted them), %u worst cases within 66 characters, %u writer cases (stored by S0, carried by the DCVS row, "
                "forgotten on reset)\n",knobLayouts,knobBounds,knobWriters);

    /* ---- The ring's queue period at the end of the run page's IRQ row (A830Fence's queue meter, adreno830_status_queue,
     * ARCHITECTURE 4.13.5), counted on its own line before the pinned one. ------------------------------------------------- */
    unsigned queueLayouts=0,queueBounds=0,queueWriters=0;
    auto queueRows=[&](const Adreno830Status &status,const Adreno830StatusQueue *queue) {
        char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
        std::memset(rows,0x5a,sizeof rows);
        CHECK(adreno830_status_format(&status,rows,2,queue)==4);
        std::vector<std::string> out;
        for(auto &row:rows) {
            const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
            CHECK(end && end-row<=int(Adreno830StatusRowMax));
            out.emplace_back(row);
        }
        return out;
    };
    auto expectQueue=[&](const Adreno830Status &status,const Adreno830StatusQueue &queue,const char *want) {
        const std::string got=queueRows(status,&queue)[3];
        if(got!=want) {std::fprintf(stderr,"row 3 '%s' != '%s'\n",got.c_str(),want);std::exit(1);}
        ++queueLayouts;
    };
    s=blank();s.stage=A830SRunning;s.state=A830Done;s.irqs=1942;s.waits=5963;s.wait_timeouts=3698;s.late_wakeups=2;
    s.wptr_retries=0;
    Adreno830StatusQueue q=period(120,120,0,4200,37200,3,5,0);
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3");
    {
        /* null is exactly the layout without a period (wptr included); rows 0-2 never change */
        const auto with=queueRows(s,&q),without=queueRows(s,nullptr);
        CHECK(without==pageRows(s) && without[3]=="IRQ 1942 wait 5963 to 3698 late 2 wptr 0");
        CHECK(with[0]==without[0] && with[1]==without[1] && with[2]==without[2]);
    }
    q.polled=1;
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q~4/38ms d5 s3");     /* seen without the fence interrupt */
    q={};
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q -");                /* an idle second */
    q=period(3,0,0,0,0,0,3,0);
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q -/-ms d3 s0");      /* submitted, nothing completed yet */
    q=period(0,0,2,0,0,0,0,0);
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q -/-ms d0 s0");      /* only completions without their stamp */
    q=period(60,60,0,499,16000,0,1,0);
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q 0/16ms d1 s0");     /* avg to the nearest ms; max 16: no slow one */
    q=period(60,60,0,500,16001,1,2,0);
    expectQueue(s,q,"IRQ 1942 wait 5963 to 3698 late 2 | q 1/17ms d2 s1");     /* max rounds up */
    /* Growing counters keep the tail on the row: in full while they fit (6/6/5/4 characters), then k, M, ... rounded down. */
    q=period(120,120,0,4200,37200,3,5,0);
    s.irqs=999999;s.waits=1000000;s.wait_timeouts=123456;s.late_wakeups=10000;
    expectQueue(s,q,"IRQ 999999 wait 1000k to 123k late 10k | q 4/38ms d5 s3");
    s.irqs=99999999;s.waits=100000000;s.wait_timeouts=99999;s.late_wakeups=9999;
    expectQueue(s,q,"IRQ 99999k wait 100M to 99999 late 9999 | q 4/38ms d5 s3");
    s.irqs=~0ull;s.waits=~0ull;s.wait_timeouts=~0ull;s.late_wakeups=~0ull;
    expectQueue(s,q,"IRQ 18446P wait 18446P to 18E late 18E | q 4/38ms d5 s3");
    /* The run page only: S10 and the RUN stage in every state; any other page keeps its rows. */
    s=blank();s.stage=A830SDcvs;s.state=A830Busy;
    expectQueue(s,q,"IRQ 0 wait 0 to 0 late 0 | q 4/38ms d5 s3");
    s.stage=A830SRunning;s.state=A830Slumber;s.aux[5]=12|uint64_t(37)<<24|uint64_t(4)<<48;
    expectQueue(s,q,"IRQ 0 wait 0 to 0 late 0 | q 4/38ms d5 s3");
    for(uint8_t stage=0;stage<A830SDcvs;++stage) {
        s=blank();s.stage=stage;s.state=A830Stopped;s.irqs=5;
        CHECK(queueRows(s,&q)==queueRows(s,nullptr));
        ++queueLayouts;
    }
    /* Worst cases: each counter at the widest value of its field, the period clamped (avg 999, max 9999, depth and slow 999):
     * exactly 66 characters. */
    const uint64_t widths[][4]={{999999,999999,99999,9999},{99999999,99999999,9999999,999999},{~0ull,~0ull,~0ull,~0ull},
                                {0,0,0,0},{100000000,100000000,10000000,1000000}};
    const Adreno830StatusQueue tails[]={period(UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,1),
                                        period(1,1,0,998999,9998001,999,999,1),period(0,0,0,0,0,0,0,0),period(5,0,0,0,0,0,999,1)};
    for(const auto &counters:widths)
        for(const auto &tail:tails)
            for(const uint8_t state:{uint8_t(A830Done),uint8_t(A830Slumber)}) {
                s=blank();s.stage=A830SRunning;s.state=state;
                s.irqs=counters[0];s.waits=counters[1];s.wait_timeouts=counters[2];s.late_wakeups=counters[3];
                s.wptr_retries=UINT32_MAX;
                const std::string row=queueRows(s,&tail)[3];
                CHECK(row.size()<=Adreno830StatusRowMax && row.find(" | q")!=std::string::npos && row.find("wptr")==std::string::npos);
                if(counters[0]==99999999 && tail.polled && tail.completed)
                    CHECK(row=="IRQ 99999k wait 99999k to 9999k late 999k | q~999/9999ms d999 s999" && row.size()==66);
                ++queueBounds;
            }
    /* Arbitrary blocks and periods never overrun a row. */
    std::mt19937_64 rnd(0x4313);
    for(unsigned i=0;i<20000;++i) {
        s=blank();
        unsigned char *bytes=reinterpret_cast<unsigned char *>(&s);
        for(size_t b=0;b<offsetof(Adreno830Status,rows);++b)bytes[b]=uint8_t(rnd());
        if(i&1)s.stage=A830SRunning;
        Adreno830StatusQueue tail;
        uint32_t *words=reinterpret_cast<uint32_t *>(&tail);
        for(size_t w=0;w<sizeof tail/4;++w)words[w]=uint32_t(rnd()>>(rnd()%33));
        queueRows(s,&tail);
        ++queueBounds;
    }
    /* The writers: the DCVS thread's period is stored (no publish), shown by the next publish (the DCVS row), replaced by the
     * next period, forgotten on null and on reset; the 576-byte block keeps its layout. */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();
    adreno830_status_running();
    const uint32_t seqQueue=adreno830_status.seq;
    adreno830_status.irqs=1942;adreno830_status.waits=5963;adreno830_status.wait_timeouts=3698;adreno830_status.late_wakeups=2;
    q=period(120,120,0,4200,37200,3,5,0);
    adreno830_status_queue(&q);
    CHECK(adreno830_status.seq==seqQueue && overlay.size()==1);
    const uint16_t three[3]={500,300,200};
    adreno830_status_dcvs(222,375,2,2,160,734,0,3,three);
    CHECK(overlay.size()==2 && overlay.back()[3]=="IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3" &&
          std::string(adreno830_status.rows[3])==overlay.back()[3]);
    q={};
    adreno830_status_queue(&q);
    adreno830_status_dcvs(222,375,2,2,160,734,0,3,three);
    CHECK(overlay.size()==3 && overlay.back()[3]=="IRQ 1942 wait 5963 to 3698 late 2 | q -");
    adreno830_status_queue(nullptr);
    adreno830_status_publish();
    CHECK(overlay.size()==4 && overlay.back()[3]=="IRQ 1942 wait 5963 to 3698 late 2 wptr 0");
    adreno830_status_queue(&q);
    adreno830_status_reset();
    adreno830_status_running();
    CHECK(overlay.back()[3]=="IRQ 0 wait 0 to 0 late 0 wptr 0");
    CHECK(sizeof(Adreno830Status)==576 && offsetof(Adreno830Status,rows)==304 && offsetof(Adreno830Status,submits)==192);
    queueWriters+=5;
    std::printf("PASS: queue period on the IRQ row: %u exact layouts (q, q~ polled, q - idle, -/- nothing completed, avg to the "
                "nearest and max up to the ms, counters in full then k/M/G/T/P/E, the run page only), %u worst cases and random "
                "periods within 66 characters, %u writer cases (stored by the DCVS thread, shown by the next publish, forgotten "
                "on null and reset)\n",queueLayouts,queueBounds,queueWriters);

    /* ---- The GPU clients at the end of the run page's MEM row (native_adreno_client_tick, adreno830_status_clients, ARCHITECTURE
     * 8.4), counted on its own line before the pinned one. ------------------------------------------------------------------ */
    unsigned clientLayouts=0,clientBounds=0,clientWriters=0;
    auto memRow=[&](const Adreno830Status &status,const Adreno830StatusClients *clients) {
        char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
        std::memset(rows,0x5a,sizeof rows);
        CHECK(adreno830_status_format(&status,rows,2,nullptr,clients)==4);
        std::vector<std::string> result;
        for(auto &row:rows) {
            const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
            CHECK(end && end-row<=int(Adreno830StatusRowMax));
            result.emplace_back(row);
        }
        return result;
    };
    auto clientsOf=[](const char *name,uint32_t mib,uint32_t imported,uint32_t orphan,uint32_t fallback) {
        Adreno830StatusClients c{};
        std::strncpy(c.top,name,sizeof c.top-1);
        c.top_mib=mib;c.top_import_mib=imported;c.top_orphan=orphan;c.fallback_mib=fallback;c.clients=2;c.orphans=orphan;
        return c;
    };
    auto expectMem=[&](const Adreno830Status &status,const Adreno830StatusClients &c,const char *text) {
        const auto rows=memRow(status,&c);
        if(rows[2]!=text) {std::fprintf(stderr,"MEM row: got \"%s\" expected \"%s\"\n",rows[2].c_str(),text);CHECK(false);}
        const auto plain=memRow(status,nullptr);
        CHECK(rows[0]==plain[0] && rows[1]==plain[1] && rows[3]==plain[3]);   /* only the MEM row changes */
        ++clientLayouts;
    };
    s=blank();s.stage=A830SRunning;s.state=A830Done;
    s.aux[4]=512|uint64_t(380)<<16|uint64_t(1)<<32|uint64_t(1402)<<34|uint64_t(812)<<49;
    expectMem(s,clientsOf("backboardd",612,96,0,0),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 backboardd:516+96");
    expectMem(s,clientsOf("backboardd",612,0,0,0),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 backboardd:612");
    expectMem(s,clientsOf("backboardd",612,96,0,40),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 sc40 backboardd:516+96");
    expectMem(s,clientsOf("backboardd",612,96,1,40),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 sc40 ~backboard:516+96");
    expectMem(s,clientsOf("WidgetRenderer_Default",123,0,0,0),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 WidgetRenderer_Def:123");
    expectMem(s,clientsOf("PosterBoard",0,0,0,7),"MEM pool 512 used 380 imp 1402 coh 1 vm 812 sc7");   /* nothing held: no name */
    expectMem(s,clientsOf("",5,0,0,0),"MEM pool 512 used 380 imp 1402 coh 1 vm 812");
    /* Without the vm field the name gets the room. */
    s.aux[4]=512|uint64_t(380)<<16|uint64_t(1)<<32|uint64_t(1402)<<34;
    expectMem(s,clientsOf("WidgetRenderer_Default",123,4,0,0),"MEM pool 512 used 380 imp 1402 coh 1 WidgetRenderer_Defa:119+4");
    /* The widest MEM row: the split while it leaves three characters of the name, else the total, else no client field. */
    s.aux[4]=65535|uint64_t(65535)<<16|uint64_t(3)<<32|uint64_t(32767)<<34|uint64_t(32767)<<49;
    expectMem(s,clientsOf("backboardd",99999,99999,1,0),"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 ~backbo:0+99999");
    expectMem(s,clientsOf("backboardd",99999,12345,0,0),"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 backboard:99999");
    expectMem(s,clientsOf("backboardd",99999,99999,1,99999),"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 sc99999");
    expectMem(s,clientsOf("backboardd",4000000000u,4000000000u,1,4000000000u),
              "MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 sc99999");
    expectMem(s,clientsOf("backboardd",999,0,0,9999),"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 sc9999 back:999");
    expectMem(s,clientsOf("bb",9,1,1,0),"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 ~bb:8+1");   /* a short name whole */
    /* Other pages keep their rows; S10 and RUN in every state show it. */
    for(uint8_t stage=0;stage<A830SDcvs;++stage) {
        s=blank();s.stage=stage;s.state=A830Stopped;s.aux[4]=512;
        const Adreno830StatusClients c=clientsOf("backboardd",612,96,0,40);
        CHECK(memRow(s,&c)==memRow(s,nullptr));
        ++clientLayouts;
    }
    s=blank();s.stage=A830SDcvs;s.state=A830Busy;s.aux[4]=512;
    expectMem(s,clientsOf("backboardd",612,96,0,0),"MEM pool 512 used 0 imp 0 coh 0 backboardd:516+96");
    /* Worst cases and random figures and names: always within 66 characters, the name never cut below three characters (a shorter
     * one whole), six when the imports are split out. */
    std::mt19937_64 crnd(0x4314);
    for(unsigned i=0;i<20000;++i) {
        s=blank();s.stage=A830SRunning;s.state=(i&1)?A830Done:A830Slumber;
        s.aux[4]=crnd();
        Adreno830StatusClients c{};
        const unsigned length=unsigned(crnd()%sizeof c.top);
        for(unsigned k=0;k<length;++k)c.top[k]=char('A'+crnd()%26);
        const uint32_t cases[]={0,1,9,99999,100000,UINT32_MAX};
        c.top_mib=(i%3)?uint32_t(crnd()>>(crnd()%64)):cases[crnd()%6];
        c.top_import_mib=(i%5)?uint32_t(crnd()>>(crnd()%64)):cases[crnd()%6];
        c.fallback_mib=(i%7)?uint32_t(crnd()>>(crnd()%64)):cases[crnd()%6];
        c.top_orphan=crnd()&1;
        const std::string row=memRow(s,&c)[2];
        const size_t colon=row.rfind(':');
        if(colon!=std::string::npos) {
            const size_t space=row.rfind(' ',colon);
            CHECK(space!=std::string::npos);
            const size_t shown=colon-space-1-(row[space+1]=='~'?1:0);
            const bool split=row.find('+',colon)!=std::string::npos;
            CHECK(shown<=length && shown>=std::min<size_t>(split?6:3,length));
        }
        ++clientBounds;
    }
    /* The writers: stored by the DCVS thread (no publish), shown by the next memory row, forgotten on null and on reset. */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();
    adreno830_status_running();
    const uint32_t seqClients=adreno830_status.seq;
    Adreno830StatusClients stored=clientsOf("backboardd",612,96,0,40);
    adreno830_status_clients(&stored);
    CHECK(adreno830_status.seq==seqClients && overlay.size()==1);
    adreno830_status_vm(uint64_t(812)<<20);
    adreno830_status_memory(512,380,1402,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 380 imp 1402 coh 1 vm 812 sc40 backboardd:516+96");
    std::memset(stored.top,'x',sizeof stored.top);   /* an unterminated name is cut at the struct's end */
    adreno830_status_clients(&stored);
    adreno830_status_memory(512,380,1402,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 380 imp 1402 coh 1 vm 812 sc40 xxxxxxxxxx:516+96");
    adreno830_status_clients(nullptr);
    adreno830_status_memory(512,380,1402,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 380 imp 1402 coh 1 vm 812");
    adreno830_status_clients(&stored);
    adreno830_status_reset();
    adreno830_status_running();
    adreno830_status_memory(512,0,6,1);
    CHECK(overlay.back()[2]=="MEM pool 512 used 0 imp 6 coh 1");
    CHECK(sizeof(Adreno830Status)==576);
    clientWriters+=5;
    std::printf("PASS: GPU clients on the MEM row: %u exact layouts (sc<MiB> outside the pool, the largest client <name>:<MiB> or "
                "<own>+<imports>, ~ for an orphan, the name cut to the room, the run page only), %u worst cases and random figures "
                "within 66 characters, %u writer cases (stored by the DCVS thread, shown by the next memory row, forgotten on null "
                "and reset)\n",clientLayouts,clientBounds,clientWriters);

    /* ---- The design profile's thermal loop on the run page's DCVS row (A830Dcvs, adreno830_status_thermal, A830StatusThermal in
     * aux[3] bits 40-50; ARCHITECTURE 4.16, 8.4): the target and the trend after the ceiling, '!' in the 95 C emergency. Counted
     * on its own line before the pinned one. ---------------------------------------------------------------------------------- */
    unsigned thermalLayouts=0,thermalBounds=0,thermalWriters=0;
    auto expectDcvs=[&](const Adreno830Status &status,const char *want) {
        const std::string got=pageRows(status)[1];
        if(got!=want) {std::fprintf(stderr,"row 1 '%s' != '%s'\n",got.c_str(),want);std::exit(1);}
        ++thermalLayouts;
    };
    CHECK(A830StatusThermal(800,A830StatusTrendRising,0)==(uint64_t(80)|uint64_t(2)<<7)<<40 &&
          A830StatusThermal(805,A830StatusTrendFalling,A830StatusThermalEmergency)==(uint64_t(81)|uint64_t(3)<<7|uint64_t(1)<<9)<<40 &&
          A830StatusThermal(0,A830StatusTrendSteady,0)==uint64_t(1)<<47 && A830StatusThermal(-50,A830StatusTrendNone,0)==0 &&
          A830StatusThermal(5000,7,3)==(uint64_t(127)|uint64_t(3)<<7|uint64_t(1)<<9)<<40 && A830StatusThermalShift==40);
    {
        auto words=[](const uint16_t *permille,unsigned count,uint64_t *w) {
            w[0]=w[1]=w[2]=0;
            for(unsigned i=0;i<count;++i)w[i/6]|=uint64_t(permille[i])<<((i%6)*10);
        };
        const uint16_t eleven[11]={170,1,8,783,0,0,35,0,0,0,0};
        uint64_t w[3];
        words(eleven,11,w);
        s=blank();s.stage=A830SRunning;s.state=A830Done;
        s.aux[0]=160|uint64_t(607)<<16|uint64_t(1)<<32|uint64_t(11)<<40;s.aux[1]=w[0];s.aux[2]=w[1];s.aux[3]=w[2];
        expectDcvs(s,"DCVS 160-607 t170/1/8/783/0/0/35/0/0/0/0");   /* nothing stored: the row as before (annibale) */
        s.aux[3]=w[2]|A830StatusThermal(800,A830StatusTrendRising,0);
        expectDcvs(s,"DCVS 160-607 T80+ t170/1/8/783/0/0/35/0/0/0/0");
        s.aux[3]=w[2]|A830StatusThermal(800,A830StatusTrendFalling,0);
        expectDcvs(s,"DCVS 160-607 T80- t170/1/8/783/0/0/35/0/0/0/0");
        s.aux[3]=w[2]|A830StatusThermal(800,A830StatusTrendSteady,0);
        expectDcvs(s,"DCVS 160-607 T80= t170/1/8/783/0/0/35/0/0/0/0");
        s.aux[3]=w[2]|A830StatusThermal(750,A830StatusTrendNone,0);   /* no usable reading: the 607 MHz cap, trend '?' */
        expectDcvs(s,"DCVS 160-607 T75? t170/1/8/783/0/0/35/0/0/0/0");
        s.aux[0]=160|uint64_t(389)<<16|uint64_t(1)<<32|uint64_t(11)<<40;
        s.aux[3]=w[2]|A830StatusThermal(800,A830StatusTrendRising,A830StatusThermalEmergency);
        expectDcvs(s,"DCVS 160-389 T80+! t170/1/8/783/0/0/35/0/0/0/0");
        /* 16 levels: the residency of levels 13-16 (aux[3] bits 0-39) and the field (bits 40-50) share aux[3] untouched. */
        const uint16_t sixteen[16]={0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4};
        words(sixteen,16,w);
        s.aux[0]=160|uint64_t(900)<<16|uint64_t(1)<<32|uint64_t(16)<<40;s.aux[1]=w[0];s.aux[2]=w[1];
        s.aux[3]=w[2]|A830StatusThermal(800,A830StatusTrendSteady,0);
        expectDcvs(s,"DCVS 160-900 T80= t0/0/0/0/0/0/0/0/0/0/0/0/1/2/3/4");
        /* The other rows and pages never show it. */
        const auto run=pageRows(s);
        CHECK(run[0].find(" T80")==std::string::npos && run[2].find(" T80")==std::string::npos);
        s.stage=A830SFence;s.state=A830Stopped;
        CHECK(pageRows(s)[1].find(" T80")==std::string::npos);
        thermalLayouts+=1;
    }
    /* Worst cases: the widest field (" T127-!") before the widest residency is cut with '+' and stays within 66. */
    for(const uint64_t field:{A830StatusThermal(5000,A830StatusTrendFalling,A830StatusThermalEmergency),
                              A830StatusThermal(800,A830StatusTrendRising,0),uint64_t(0)}) {
        s=blank();s.stage=A830SRunning;s.state=A830Done;
        s.aux[0]=0xffff|uint64_t(0xffff)<<16|uint64_t(1)<<32|uint64_t(16)<<40;
        s.aux[1]=s.aux[2]=0;
        for(unsigned i=0;i<6;++i) {s.aux[1]|=uint64_t(1000)<<(i*10);s.aux[2]|=uint64_t(1000)<<(i*10);}
        s.aux[3]=field;
        for(unsigned i=0;i<4;++i)s.aux[3]|=uint64_t(1000)<<(i*10);
        const std::string row=pageRows(s)[1];
        CHECK(row.size()<=Adreno830StatusRowMax && row.back()=='+');
        if(field>>A830StatusThermalShift==(127|3<<7|1<<9))CHECK(row.rfind("DCVS 65535-65535 T127-! t1000/1000/",0)==0);
        ++thermalBounds;
    }
    std::mt19937_64 noise(0x5480);
    for(unsigned i=0;i<20000;++i) {
        s=blank();
        unsigned char *bytes=reinterpret_cast<unsigned char *>(&s);
        for(size_t b=0;b<offsetof(Adreno830Status,rows);++b)bytes[b]=uint8_t(noise());
        s.stage=A830SRunning;
        pageRows(s);
        ++thermalBounds;
    }
    /* The writers: the DCVS thread's adreno830_status_thermal is only stored; every adreno830_status_dcvs carries it into aux[3]
     * next to the residency; a newer store replaces it; a reset forgets it (the annibale profile never stores one). */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();
    adreno830_status_running();
    const uint32_t seqThermal=adreno830_status.seq;
    adreno830_status_thermal(800,A830StatusTrendRising,0);
    CHECK(adreno830_status.seq==seqThermal && overlay.size()==1);
    const uint16_t levels11[11]={0,0,0,0,0,0,600,400,0,0,0};
    adreno830_status_dcvs(607,880,7,6,160,900,1,11,levels11);
    CHECK(overlay.back()[1]=="DCVS 160-900 T80+ t0/0/0/0/0/0/600/400/0/0/0" &&
          (adreno830_status.aux[3]>>A830StatusThermalShift)==(80u|2u<<7));
    adreno830_status_thermal(800,A830StatusTrendFalling,A830StatusThermalEmergency);
    adreno830_status_dcvs(389,990,4,4,160,389,1,11,levels11);
    CHECK(overlay.back()[1]=="DCVS 160-389 T80-! t0/0/0/0/0/0/600/400/0/0/0");
    adreno830_status_reset();adreno830_status_mode(2);
    adreno830_status_running();
    adreno830_status_dcvs(607,880,7,6,160,900,1,11,levels11);
    CHECK(overlay.back()[1]=="DCVS 160-900 t0/0/0/0/0/0/600/400/0/0/0" && (adreno830_status.aux[3]>>A830StatusThermalShift)==0);
    CHECK(sizeof(Adreno830Status)==576);
    thermalWriters+=3;
    std::printf("PASS: thermal loop on the DCVS row: %u exact layouts (T<target> with ?/=/+/- after the ceiling, ! in the emergency, "
                "16 levels sharing aux[3], the run page only), %u worst cases and random blocks within 66 characters, %u writer "
                "cases (stored by the DCVS thread, carried by the DCVS row, forgotten on reset)\n",thermalLayouts,thermalBounds,
                thermalWriters);

    /* ---- M14 P0 (preemption phase 0 and the per-process page-table data): the CP_SMMU_APERTURE outcome on the S3 page, the job
     * attribution on the MEM row (" fw<max>ms/<n4>", the client figures yielding), the context entries on the IRQ row (" sw<n>" when
     * it fits), and the ramoops meter line (every figure, at most one line per ten periods, only a changed one, only while the log
     * runs). Counted on its own line before the pinned one. ------------------------------------------------------------------ */
    unsigned p0Layouts=0,p0Bounds=0,p0Writers=0;
    auto runRows=[&](const Adreno830Status &status,const Adreno830StatusQueue *queue,const Adreno830StatusClients *clients) {
        char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
        std::memset(rows,0x5a,sizeof rows);
        CHECK(adreno830_status_format(&status,rows,2,queue,clients)==4);
        std::vector<std::string> out;
        for(auto &row:rows) {
            const char *end=static_cast<const char *>(std::memchr(row,0,sizeof row));
            CHECK(end && end-row<=int(Adreno830StatusRowMax));
            out.emplace_back(row);
        }
        return out;
    };
    auto expectRow=[&](const std::string &got,const char *want) {
        if(got!=want) {std::fprintf(stderr,"row '%s' != '%s'\n",got.c_str(),want);std::exit(1);}
        ++p0Layouts;
    };
    /* The S3 page: " AP<state>" once recorded (aux[3] bits 56-58), 54 characters; nothing before. */
    s=blank();s.stage=A830SSmmu;s.state=A830Stopped;
    s.aux[2]=uint64_t(2)<<32;s.aux[3]=0x300|uint64_t(0|3<<8|1<<16)<<32;
    expectRow(runRows(s,nullptr,nullptr)[2],"ACR 00000000 GFSR 00000002 SLOT 0/3/1 S2C 00000300");
    for(uint32_t state=0;state<3;++state) {
        s.aux[3]=0x300|uint64_t(0|3<<8|1<<16)<<32|A830StatusAperture(state);
        const std::string row=runRows(s,nullptr,nullptr)[2];
        expectRow(row,(std::string("ACR 00000000 GFSR 00000002 SLOT 0/3/1 S2C 00000300 AP")+char('0'+state)).c_str());
        CHECK(row.size()==54);
    }
    CHECK(A830StatusAperture(7)==(uint64_t(7)<<56) && A830StatusAperture(1)==(uint64_t(5)<<56));
    /* The MEM row: " fw<max>ms/<n4>" before the client figures, which take what is left; nothing while fw max is 0. */
    s=blank();s.stage=A830SRunning;s.state=A830Done;
    s.aux[4]=512|uint64_t(380)<<16|uint64_t(1)<<32|uint64_t(1402)<<34|uint64_t(812)<<49;
    Adreno830StatusClients bb{};
    std::strncpy(bb.top,"backboardd",sizeof bb.top-1);
    bb.top_mib=612;bb.top_import_mib=96;bb.clients=2;
    Adreno830StatusQueue fw=period(120,120,0,4200,37200,3,5,0);
    fw.rs_jobs=60;fw.rs_avg_us=800;fw.rs_max_us=3100;fw.fw_jobs=60;fw.fw_avg_us=300;fw.fw_max_us=5200;fw.fw_over4=1;
    fw.ext_jobs=20;fw.ext_busy_us=81000;
    expectRow(runRows(s,&fw,&bb)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 fw6ms/1 backboa:516+96");
    CHECK(runRows(s,&fw,&bb)[2].size()==66);
    expectRow(runRows(s,&fw,nullptr)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 fw6ms/1");
    {
        const auto with=runRows(s,&fw,&bb),plain=runRows(s,nullptr,nullptr);
        CHECK(with[0]==plain[0] && with[1]==plain[1]);   /* rows 0 and 1 never change */
    }
    Adreno830StatusQueue quiet=fw;
    quiet.fw_max_us=0;quiet.fw_over4=0;                   /* render-server jobs with nothing external ahead: no field */
    expectRow(runRows(s,&quiet,&bb)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 backboardd:516+96");
    quiet=fw;quiet.fw_jobs=0;                             /* no fw known */
    expectRow(runRows(s,&quiet,&bb)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 backboardd:516+96");
    quiet=fw;quiet.fw_max_us=4000;quiet.fw_over4=0;       /* 4 ms is not above 4 ms */
    expectRow(runRows(s,&quiet,nullptr)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 fw4ms/0");
    quiet=fw;quiet.fw_max_us=UINT32_MAX;quiet.fw_over4=UINT32_MAX;
    expectRow(runRows(s,&quiet,nullptr)[2],"MEM pool 512 used 380 imp 1402 coh 1 vm 812 fw9999ms/999");
    /* The widest MEM row with the widest field: 63 characters, the client figures (sc and the name) left out whole. */
    s.aux[4]=65535|uint64_t(65535)<<16|uint64_t(3)<<32|uint64_t(32767)<<34|uint64_t(32767)<<49;
    Adreno830StatusClients wide=bb;
    wide.fallback_mib=99999;
    expectRow(runRows(s,&quiet,&wide)[2],"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 fw9999ms/999");
    expectRow(runRows(s,&fw,&wide)[2],"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 fw6ms/1 sc99999");   /* sc whole, no name */
    wide.fallback_mib=7;   /* sc fits, then no room for three characters of the name */
    expectRow(runRows(s,&fw,&wide)[2],"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 fw6ms/1 sc7");
    wide.fallback_mib=0;   /* the name cut to three, the total in place of the split */
    expectRow(runRows(s,&fw,&wide)[2],"MEM pool 65535 used 65535 imp 32767 coh 3 vm 32767 fw6ms/1 bac:612");
    CHECK(runRows(s,&fw,&wide)[2].size()==66);
    /* The IRQ row: " sw<n>" after the queue tail when the period entered new contexts and it fits whole. */
    s=blank();s.stage=A830SRunning;s.state=A830Done;s.irqs=1942;s.waits=5963;s.wait_timeouts=3698;s.late_wakeups=2;
    Adreno830StatusQueue sw=period(120,120,0,4200,37200,3,5,0);
    sw.contexts=12;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3 sw12");
    sw.contexts=0;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3");
    sw.contexts=123456;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3 sw9999");
    sw=period(3,0,0,0,0,0,3,0);sw.contexts=2;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 1942 wait 5963 to 3698 late 2 | q -/-ms d3 s0 sw2");
    s.irqs=99999999;s.waits=99999999;s.wait_timeouts=9999999;s.late_wakeups=999999;   /* the 66-character row: no room */
    sw=period(1,1,0,998999,9998001,999,999,1);sw.contexts=5;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 99999k wait 99999k to 9999k late 999k | q~999/9999ms d999 s999");
    s.irqs=99999;s.waits=99999;s.wait_timeouts=9999;s.late_wakeups=99;sw.contexts=9999;   /* 61 characters: room for "sw5", not "sw9999" */
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 99999 wait 99999 to 9999 late 99 | q~999/9999ms d999 s999");
    sw.contexts=5;
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 99999 wait 99999 to 9999 late 99 | q~999/9999ms d999 s999 sw5");
    s.late_wakeups=999;   /* 62 characters: " sw5" fills the row to its 66th character */
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 99999 wait 99999 to 9999 late 999 | q~999/9999ms d999 s999 sw5");
    sw.contexts=10;       /* one character more: left out */
    expectRow(runRows(s,&sw,nullptr)[3],"IRQ 99999 wait 99999 to 9999 late 999 | q~999/9999ms d999 s999");
    /* Random blocks, periods and clients: every row within 66 characters, the fw field whole when present. */
    std::mt19937_64 prnd(0x1414);
    for(unsigned i=0;i<20000;++i) {
        s=blank();
        unsigned char *bytes=reinterpret_cast<unsigned char *>(&s);
        for(size_t b=0;b<offsetof(Adreno830Status,rows);++b)bytes[b]=uint8_t(prnd());
        s.stage=(i&3)?uint8_t(A830SRunning):uint8_t(prnd()%A830StageCount);
        Adreno830StatusQueue q2{};
        uint32_t *words=reinterpret_cast<uint32_t *>(&q2);
        for(size_t w=0;w<sizeof q2/4;++w)words[w]=uint32_t(prnd()>>(prnd()%33));
        Adreno830StatusClients c{};
        const unsigned length=unsigned(prnd()%sizeof c.top);
        for(unsigned k=0;k<length;++k)c.top[k]=char('a'+prnd()%26);
        c.top_mib=uint32_t(prnd()>>(prnd()%64));c.top_import_mib=uint32_t(prnd()>>(prnd()%64));
        c.fallback_mib=uint32_t(prnd()>>(prnd()%64));c.top_orphan=prnd()&1;
        const auto rows=runRows(s,&q2,&c);
        const size_t at=rows[2].find(" fw");
        if(at!=std::string::npos) {
            const size_t ms=rows[2].find("ms/",at);
            CHECK(ms!=std::string::npos && ms>at+3 && ms+3<rows[2].size());
        }
        ++p0Bounds;
    }
    /* The meter line: one window of a minute (sixty periods), written once while the log runs on a diagnostic page, not again while it
     * stays the same. Averages to the nearest 0.1 ms (849 us: 0.8), maxima up (15401 us: 15.5); poll counts the periods seen without
     * the fence interrupt. */
    adreno830_status_reset();adreno830_status_mode(2);overlay.clear();logged.clear();logOn=1;
    Adreno830StatusClients load{};
    load.clients=9;load.peak_clients=14;load.window_overflows=3;load.faults_polled=2;
    adreno830_status_clients(&load);
    adreno830_status_aperture(1);
    Adreno830StatusQueue busy=period(120,120,0,4200,37200,3,5,0);
    busy.rs_jobs=100;busy.rs_avg_us=849;busy.rs_max_us=9000;busy.fw_jobs=100;busy.fw_avg_us=100;busy.fw_max_us=2000;
    busy.ext_jobs=34;busy.ext_busy_us=81250;busy.contexts=39;
    auto busyWindow=[&] {
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {
            Adreno830StatusQueue q2=busy;
            if(second==4) {q2.rs_max_us=15401;q2.fw_max_us=6200;q2.fw_over4=2;q2.contexts=61;}
            if(second==7 || second==8)q2.polled=1;
            adreno830_status_queue(&q2);
            adreno830_status_meter();
        }
    };
    const uint32_t seqMeter=adreno830_status.seq;
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {
        Adreno830StatusQueue q2=busy;
        if(second==4) {q2.rs_max_us=15401;q2.fw_max_us=6200;q2.fw_over4=2;q2.contexts=61;}
        if(second==7 || second==8)q2.polled=1;
        adreno830_status_queue(&q2);
        adreno830_status_meter();
        CHECK(logged.size()==(second+1<Adreno830StatusMeterPeriods?0u:1u));
    }
    CHECK(Adreno830StatusMeterPeriods==60 &&
          logged.back()=="gpu meter rs 6000 0.8/15.5ms fw 6000 0.1/6.2ms n4 2 ext 2040 4875.0ms ctx 2362 61/s poll 2 conn 9/14 nos 0 "
                         "ovf 3 vmr 0/0 pf 0/0/0/2 ap1");
    CHECK(adreno830_status.seq==seqMeter && overlay.empty());   /* nothing drawn, nothing published */
    busyWindow();                                               /* the same window again: no line */
    CHECK(logged.size()==1);
    Adreno830StatusQueue idle{};
    for(unsigned round=0;round<2;++round)
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {adreno830_status_queue(&idle);adreno830_status_meter();}
    CHECK(logged.size()==2 &&
          logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 conn 9/14 nos 0 ovf 3 vmr 0/0 pf 0/0/0/2 ap1");
    /* A slot refusal changes the line at the next window, idle or not. */
    load.slot_refusals=1;adreno830_status_clients(&load);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {adreno830_status_queue(&idle);adreno830_status_meter();}
    CHECK(logged.size()==3 && logged.back().find(" nos 1 ")!=std::string::npos);
    /* The log off: nothing is written and the window still closes every sixty periods. */
    logOn=0;
    for(unsigned second=0;second<Adreno830StatusMeterPeriods+3;++second) {adreno830_status_queue(&busy);adreno830_status_meter();}
    logOn=1;
    for(unsigned second=0;second<Adreno830StatusMeterPeriods-3;++second) {adreno830_status_queue(&idle);adreno830_status_meter();}
    CHECK(logged.size()==4 &&
          logged.back().rfind("gpu meter rs 300 0.8/9.0ms fw 300 0.1/2.0ms n4 0 ext 102 243.8ms ctx 117 39/s poll 0 ",0)==0);
    /* The default overlay (pagani-gpu-status=1) and the overlay off log nothing new, busy or not, window after window; the report page
     * (3) logs like the diagnostic page (2). */
    for(const unsigned mode:{1u,0u,1u}) {
        adreno830_status_mode(mode);
        busyWindow();
        CHECK(logged.size()==4);
    }
    adreno830_status_mode(3);
    busyWindow();
    CHECK(logged.size()==5 && logged.back().rfind("gpu meter rs 6000 0.8/15.5ms ",0)==0);
    adreno830_status_mode(2);
    /* Before S3 stored the aperture and before the shell's first tick: "ap-", no client figures. A reset forgets the last line. */
    adreno830_status_reset();logged.clear();
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==1 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 ap-");
    adreno830_status_aperture(2);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==2 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 ap2");
    /* The widest line: 186 characters (one ramoops record); random windows never pass Adreno830StatusMeterBytes - 1. */
    adreno830_status_reset();logged.clear();
    Adreno830StatusClients most{};
    most.clients=most.peak_clients=most.slot_refusals=most.window_overflows=most.reserve_objects=most.reserve_spans=UINT32_MAX;
    most.faults_alike=most.faults_apart=most.faults_undecided=most.faults_polled=UINT32_MAX;
    adreno830_status_clients(&most);adreno830_status_aperture(2);
    Adreno830StatusQueue full{};
    full.rs_jobs=full.fw_jobs=full.ext_jobs=1000000;full.rs_avg_us=full.fw_avg_us=UINT32_MAX;full.rs_max_us=full.fw_max_us=UINT32_MAX;
    full.fw_over4=full.ext_busy_us=full.contexts=UINT32_MAX;full.polled=1;
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {adreno830_status_queue(&full);adreno830_status_meter();}
    CHECK(logged.size()==1 && logged.back().size()==186 &&
          logged.back()=="gpu meter rs 99999 9999.9/9999.9ms fw 99999 9999.9/9999.9ms n4 99999 ext 99999 9999.9ms ctx 99999 9999/s "
                         "poll 60 conn 99/99 nos 99999 ovf 99999 vmr 99999/99999 pf 9999/9999/9999/9999 ap2");
    /* 200 random windows, each with its own magnitude (from all zero to every field saturated). */
    for(unsigned window=0;window<200;++window) {
        const uint64_t mask=(uint64_t(1)<<(window%33))-1;
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {
            Adreno830StatusQueue q2{};
            uint32_t *words=reinterpret_cast<uint32_t *>(&q2);
            for(size_t w=0;w<sizeof q2/4;++w)words[w]=uint32_t(prnd()&mask);
            Adreno830StatusClients c{};
            uint32_t *figures=&c.top_mib;
            for(size_t w=0;w<(sizeof c-offsetof(Adreno830StatusClients,top_mib))/4;++w)figures[w]=uint32_t(prnd()&mask);
            adreno830_status_clients(&c);
            adreno830_status_queue(&q2);
            adreno830_status_meter();
            ++p0Bounds;
        }
    }
    CHECK(logged.size()>=150);   /* the random windows were laid out and written (an unchanged one is not) */
    for(const std::string &line:logged)CHECK(line.size()<Adreno830StatusMeterBytes && line.size()<=309 && line.rfind("gpu meter rs ",0)==0);
    /* The per-second line (2026-10-31, ARCHITECTURE 8.8): every second with a submission or a completion, or with Adreno830StatusSecondBurst
     * calls and no GPU work; pagani-diag, a diagnostic page and the log running; the longest job of each side, the queue, the
     * fence wait, the second's calls and the busiest process. Without pagani-diag (a loop image, 32 KiB of events ring and no UFS log) the
     * meter line stays and this one is not written: the three conditions, each alone. */
    adreno830_status_reset();adreno830_status_mode(3);logged.clear();loggedSeconds.clear();
    {
        Adreno830StatusQueue quiet=period(120,120,0,4200,21000,3,5,0);
        Adreno830StatusClients spinning{};
        spinning.second_calls=Adreno830StatusSecondBurst;
        adreno830_status_clients(&spinning);
        CHECK(!adreno830_status_current_diag() && !adreno830_status_diag_lines());
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {adreno830_status_queue(&quiet);adreno830_status_meter();}
        CHECK(loggedSeconds.empty() && logged.size()==1 && logged.back().rfind("gpu meter rs ",0)==0);
        adreno830_status_diag(true);
        CHECK(adreno830_status_current_diag() && adreno830_status_diag_lines());
        logOn=0;CHECK(!adreno830_status_diag_lines());logOn=1;
        for(const unsigned mode:{0u,1u}) {adreno830_status_mode(mode);CHECK(!adreno830_status_diag_lines());}
        adreno830_status_mode(2);CHECK(adreno830_status_diag_lines());
        /* A boot's reset does not undo it (S0 sets it after the reset, as the mode). */
        adreno830_status_reset();CHECK(adreno830_status_current_diag());
        adreno830_status_mode(3);logged.clear();loggedSeconds.clear();
    }
    Adreno830StatusClients sec{};
    sec.clients=8;sec.peak_clients=8;sec.second_calls=12513;sec.second_top_calls=12400;
    std::memcpy(sec.second_top,"WidgetRenderer_Defa",20);
    sec.second_classes[Adreno830ClassFree]=4000;sec.second_classes[Adreno830ClassWait]=8000;sec.second_classes[Adreno830ClassCpu]=400;
    sec.calls.max_us[Adreno830CallAlloc]=11001;sec.calls.max_us[Adreno830CallMap]=4100;sec.calls.max_us[Adreno830CallFree]=1500;
    sec.calls.max_us[Adreno830CallImport]=1;sec.calls.max_us[Adreno830CallSubmit]=400;
    sec.calls.import_maps=7;sec.calls.map_us=38000;sec.calls.unmap_us=20200;sec.calls.fence_us=12050;
    adreno830_status_clients(&sec);
    Adreno830StatusQueue job=period(120,120,0,4200,21000,3,5,0);
    job.rs_jobs=86;job.rs_max_us=15401;job.ext_jobs=34;job.ext_max_us=3200;job.fw_jobs=86;job.fw_max_us=6200;
    adreno830_status_queue(&job);
    adreno830_status_meter();
    CHECK(loggedSeconds.size()==1 && logged.empty() &&
          loggedSeconds.back()=="gpu sec rs 86 15.5ms ext 34 3.2ms q 21.0ms d5 fw 6.2ms wf 12.1ms cl 12513 ca 12/5/2/1/1 vt 38/21 im 7 top "
                                "WidgetRender 12400 fr4000 wf8000 cp400");
    /* Render-server jobs alone, no calls: only those fields. */
    Adreno830StatusClients calmSecond{};
    adreno830_status_clients(&calmSecond);
    Adreno830StatusQueue fewJobs=period(2,2,0,1000,1500,0,1,0);
    fewJobs.rs_jobs=2;fewJobs.rs_max_us=1400;
    adreno830_status_queue(&fewJobs);adreno830_status_meter();
    CHECK(loggedSeconds.size()==2 && loggedSeconds.back()=="gpu sec rs 2 1.4ms ext 0 q 1.5ms d1 cl 0");
    /* Nothing submitted or completed: no line under Adreno830StatusSecondBurst calls, one from there on (a client spinning with an idle GPU). */
    Adreno830StatusQueue none{};
    adreno830_status_queue(&none);adreno830_status_meter();
    CHECK(loggedSeconds.size()==2);
    calmSecond.second_calls=Adreno830StatusSecondBurst-1;adreno830_status_clients(&calmSecond);
    adreno830_status_queue(&none);adreno830_status_meter();
    CHECK(loggedSeconds.size()==2);
    calmSecond.second_calls=Adreno830StatusSecondBurst;adreno830_status_clients(&calmSecond);
    adreno830_status_queue(&none);adreno830_status_meter();
    CHECK(loggedSeconds.size()==3 && loggedSeconds.back()=="gpu sec rs 0 ext 0 cl 1000");
    /* Page 2 logs it as well; the default overlay, the overlay off and the log off do not. */
    adreno830_status_mode(2);adreno830_status_queue(&job);adreno830_status_meter();
    CHECK(loggedSeconds.size()==4);
    for(const unsigned mode:{1u,0u}) {adreno830_status_mode(mode);adreno830_status_queue(&job);adreno830_status_meter();CHECK(loggedSeconds.size()==4);}
    adreno830_status_mode(3);logOn=0;adreno830_status_queue(&job);adreno830_status_meter();logOn=1;
    CHECK(loggedSeconds.size()==4);
    /* Before the shell's first tick: the jobs alone. */
    adreno830_status_clients(nullptr);adreno830_status_queue(&job);adreno830_status_meter();
    CHECK(loggedSeconds.size()==5 && loggedSeconds.back()=="gpu sec rs 86 15.5ms ext 34 3.2ms q 21.0ms d5 fw 6.2ms");
    /* The render server's frontend (2026-11-01): reports add up until a line takes them, the longest stays the longest, whole ms rounded up, a
     * bracket that is all 0 left out; a second without a line keeps them, the line after the one that took them has none. A line with
     * no clients has no place for them yet: they wait. */
    {
        Adreno830StatusFrontend front{};
        front.states=2;front.state_us=1500;front.state_max_us=1001;front.clean_us=96100;front.clean_max_us=1900;
        adreno830_status_frontend(&front);
        adreno830_status_frontend(nullptr);
        adreno830_status_queue(&job);adreno830_status_meter();
        CHECK(loggedSeconds.size()==6 && loggedSeconds.back().find(" pl ")==std::string::npos);
        calmSecond.second_calls=0;adreno830_status_clients(&calmSecond);
        adreno830_status_queue(&none);adreno830_status_meter();
        CHECK(loggedSeconds.size()==6);
        const size_t lines=loggedSeconds.size();
        front={};
        front.states=1;front.state_us=300;front.state_max_us=300;front.makings=3;front.making_us=42000;front.making_max_us=30001;
        front.clean_us=800;front.clean_max_us=1200;
        adreno830_status_frontend(&front);
        adreno830_status_queue(&fewJobs);adreno830_status_meter();
        CHECK(loggedSeconds.size()==lines+1);
        CHECK(loggedSeconds.back()=="gpu sec rs 2 1.4ms ext 0 q 1.5ms d1 cl 0 pl 3 2/2 vk 3 42/31 cln 97/2");
        adreno830_status_queue(&fewJobs);adreno830_status_meter();
        CHECK(loggedSeconds.size()==lines+2 && loggedSeconds.back().find(" pl ")==std::string::npos &&
              loggedSeconds.back().find(" cln ")==std::string::npos);
        /* One bracket alone. */
        front={};front.clean_us=1;adreno830_status_frontend(&front);
        adreno830_status_queue(&fewJobs);adreno830_status_meter();
        CHECK(loggedSeconds.back().find(" pl ")==std::string::npos && loggedSeconds.back().find(" vk ")==std::string::npos &&
              loggedSeconds.back().size()>7 && loggedSeconds.back().rfind(" cln 1/0")==loggedSeconds.back().size()-8);
        /* A reset forgets them. */
        front={};front.states=1;adreno830_status_frontend(&front);
        adreno830_status_reset();adreno830_status_mode(3);
        adreno830_status_clients(&calmSecond);adreno830_status_queue(&fewJobs);adreno830_status_meter();
        CHECK(loggedSeconds.back().find(" pl ")==std::string::npos);
    }
    /* The widest line: every field saturated, the classes of eight digits, the sums past 32 bits (they stay at the top, a wrap would read 1): 315 characters. */
    {
        Adreno830StatusFrontend widest{};
        std::memset(&widest,0xff,sizeof widest);
        adreno830_status_frontend(&widest);
        std::memset(&widest,0,sizeof widest);
        widest.states=widest.state_us=widest.makings=widest.making_us=widest.clean_us=2;
        adreno830_status_frontend(&widest);
    }
    Adreno830StatusClients widestCalls{};
    widestCalls.second_calls=widestCalls.second_top_calls=UINT32_MAX;std::memcpy(widestCalls.second_top,"WidgetRenderer_Defa",20);
    for(uint32_t &n:widestCalls.second_classes)n=UINT32_MAX;
    for(uint32_t &us:widestCalls.calls.max_us)us=UINT32_MAX;
    widestCalls.calls.import_maps=widestCalls.calls.map_us=widestCalls.calls.unmap_us=widestCalls.calls.fence_us=UINT32_MAX;
    Adreno830StatusQueue widestJobs{};
    widestJobs.submitted=widestJobs.completed=widestJobs.rs_jobs=widestJobs.ext_jobs=UINT32_MAX;widestJobs.depth=UINT32_MAX;
    widestJobs.rs_max_us=widestJobs.ext_max_us=widestJobs.max_us=widestJobs.fw_max_us=UINT32_MAX;
    adreno830_status_clients(&widestCalls);adreno830_status_queue(&widestJobs);adreno830_status_meter();
    CHECK(loggedSeconds.back().size()==315 && loggedSeconds.back().size()<Adreno830StatusMeterBytes);
    CHECK(loggedSeconds.back().find(" pl 9999 99999/9999 vk 9999 99999/9999 cln 99999/9999 top ")!=std::string::npos);
    ++p0Bounds;
    logged.clear();loggedSeconds.clear();
    /* The call figures (NativeAdrenoMeter.h, ARCHITECTURE 8.8), between pf and ap: each tick's calls fold into the window once, however many
     * meter calls see them; longest-so-far figures whole ms rounded up, sums added up; a bracket that is all 0 is left out, and
     * with no calls at all the line is the one above. */
    adreno830_status_reset();logged.clear();adreno830_status_aperture(1);
    Adreno830StatusClients calm{};calm.clients=3;calm.peak_clients=3;
    Adreno830StatusClients calls=calm;
    calls.calls.max_us[Adreno830CallAlloc]=11001;calls.calls.max_us[Adreno830CallMap]=999;calls.calls.max_us[Adreno830CallFree]=2000;
    calls.calls.max_us[Adreno830CallImport]=1;calls.calls.max_us[Adreno830CallSubmit]=400;
    calls.calls.wait_us[Adreno830WaitConnection]=2500;calls.calls.wait_us[Adreno830WaitMemory]=7000;
    calls.calls.import_maps=120;calls.calls.shared_maps=3;calls.calls.shared_us=5100;
    calls.calls.map_us=38000;calls.calls.unmap_us=20200;calls.calls.zero_kib=420000;calls.calls.zero_us=61000;
    calls.calls.contig_tries=2;calls.calls.contig_fails=1;calls.calls.outside_bos=4;
    adreno830_status_clients(&calls);
    Adreno830StatusClients later=calm;
    later.calls.max_us[Adreno830CallAlloc]=3000;later.calls.max_us[Adreno830CallMap]=5000;later.calls.wait_us[Adreno830WaitVm]=800;
    later.calls.import_maps=2;later.calls.map_us=1000;later.calls.zero_kib=1;later.calls.zero_us=1;
    adreno830_status_clients(&later);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==1 &&
          logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 conn 3/3 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0 "
                         "ca 12/5/2/1/1 lk 3/0/7/1 im 122/3/6 vt 39/21 zf 411/62 cs 2/1/4 ap1");
    /* The next window: the same clients again and no new call: only the bracket-free line. */
    adreno830_status_clients(&calm);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==2 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 conn 3/3 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0 ap1");
    /* One bracket alone, and the BOs outside the pool on their own: above 128 KiB they make no attempt. */
    Adreno830StatusClients single=calm;single.calls.contig_tries=7;
    adreno830_status_clients(&single);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==3 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 conn 3/3 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0 cs 7/0/0 ap1");
    single=calm;single.calls.outside_bos=5;
    adreno830_status_clients(&single);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==4 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 conn 3/3 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0 cs 0/0/5 ap1");
    /* The widest line with them: 309 characters, past BootProgress's 200 of a record (the rest goes into a '+' one), within the buffer. */
    adreno830_status_reset();logged.clear();adreno830_status_aperture(2);
    Adreno830StatusClients widest=most;
    uint32_t *figures=reinterpret_cast<uint32_t *>(&widest.calls);
    for(size_t w=0;w<sizeof widest.calls/4;++w)figures[w]=UINT32_MAX;
    adreno830_status_clients(&widest);
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second) {adreno830_status_queue(&full);adreno830_status_meter();}
    CHECK(logged.size()==1 && logged.back().size()==309 && logged.back().size()<Adreno830StatusMeterBytes &&
          logged.back()=="gpu meter rs 99999 9999.9/9999.9ms fw 99999 9999.9/9999.9ms n4 99999 ext 99999 9999.9ms ctx 99999 9999/s "
                         "poll 60 conn 99/99 nos 99999 ovf 99999 vmr 99999/99999 pf 9999/9999/9999/9999 ca 9999/9999/9999/9999/9999 "
                         "lk 9999/9999/9999/9999 im 99999/99999/99999 vt 99999/99999 zf 99999/99999 cs 99999/99999/99999 ap2");
    /* A reset forgets a window's calls. */
    adreno830_status_reset();logged.clear();
    adreno830_status_clients(&calls);
    adreno830_status_reset();
    for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)adreno830_status_meter();
    CHECK(logged.size()==1 && logged.back()=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 ap-");
    adreno830_status_reset();logOn=0;logged.clear();
    adreno830_status_diag(false);
    p0Writers+=11+6+6;
    std::printf("PASS: M14 P0 rows: %u exact layouts (AP<state> on the S3 page, fw<max>ms/<n4> before the MEM row's client figures, "
                "which yield whole, sw<n> on the IRQ row when it fits, to the last character), %u random rows and meter lines within "
                "their bounds, %u meter writer cases (one line per sixty periods, only a changed one, only on the diagnostic pages "
                "(status 2, 3) with the log on, the polled periods counted, nothing drawn, ap- before S3, the widest line 180 "
                "characters; with the call figures folded once per tick, whole ms up, brackets left out at 0, 309)\n",p0Layouts,p0Bounds,
                p0Writers);

    std::printf("PASS: %u exact row layouts, %u bounded rows (worst cases on every page and random blocks, %u rows checked), "
                "%u publish transitions and %u run-time writers; seq, the overlay and the registry move only on changed text\n",
                layouts,limits,checked,publishes,writers);
    return 0;
}
