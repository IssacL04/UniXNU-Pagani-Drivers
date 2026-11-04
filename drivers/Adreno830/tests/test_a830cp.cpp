/* SPDX-License-Identifier: MIT */
/* A830Cp over a register-level CP model (ARCHITECTURE 4.8-4.11, 4.13.4, 4.15; PLAN M4, M5, M10, M11): the S6 write sequence
 * and values against the design and the generated tables, the aperture discipline, the ring start, the SQE start and CP_ME_INIT,
 * the idle criterion and its timeout dump, the secure exit with the GMEM protect rewrite after it, the power-up list, the clock
 * gating (P2S timeout not fatal), the recovery halts and the fault reads; then the same steps in the annibale profile
 * (A830Profile.h; annibale kernel-sequences 9.2, 9.4, 9.7, section 14 rows 29, 32, 33, 35): annibale's hw_init list and order
 * with RBBM_INT_0_MASK = 0, the strict read-backs, the cold-ring and SQE_CNTL gates, the sGFSR and GMU hooks, the 200 ms
 * CP_ME_INIT criterion, no clock gating after the secure exit, the 3 x 1 us P2S poll. No GPU access.
 * host-test: A830Cp.cpp NativeAdrenoRing.cpp NativeAdrenoPwrup.cpp */
#include "XnuMemoryFixture.h"
#include "A830Cp.h"
#include "A830HwInit.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include "NativeAdrenoPwrup.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace {
uint32_t ringMem[8192],memptrs[1024];
struct Write {uint32_t reg,value,pipe;bool locked,secure;};
struct Event {char kind;uint32_t reg;bool locked;};          /* 'w' write, 'r' read, 'b' barrier, in program order */
struct Model {
    std::map<uint32_t,uint32_t> regs;                       /* global registers */
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> piped;  /* (pipe, register) */
    std::set<uint32_t> pipeRegs;                            /* registers routed by CP_APERTURE_CNTL_HOST */
    std::map<uint32_t,uint32_t> forced;                     /* registers that read back this value whatever was written */
    std::vector<Write> writes;
    std::vector<std::pair<uint32_t,uint32_t>> reads;        /* register, pipe */
    std::vector<Event> log;
    uint32_t aperture=0,rptr=0,wptr=0,sqeStatAddr[8]={};
    bool sqeRunning=false,consume=true,secure=true,p2s=true,gxAck=true,cxAck=true,meInit=false,sqeEcho=false;
    unsigned locks=0,unlocks=0,depth=0,barriers=0,sleeps=0,delays=0,failWrite=0,failRead=0;
    uint64_t clock=0;
    uint32_t meInitWords[8]={};
} m;
uint32_t pipeOf(uint32_t aperture) {return (aperture>>12)&0xf;}
void execute(uint32_t from,uint32_t to)
{
    /* The CP fetches the ring words between the two pointers; only the packets this test cares about are decoded. */
    for(uint32_t i=from;i!=to;) {
        const uint32_t header=ringMem[i];
        uint32_t count=1;
        if((header>>28)==7u) {
            const uint32_t opcode=(header>>16)&0x7fu,n=header&0x3fffu;
            if(opcode==A8XX_PM4_CP_SET_SECURE_MODE && ringMem[(i+1)&8191]==0)m.secure=false;
            if(opcode==A8XX_PM4_CP_ME_INIT) {m.meInit=true;for(uint32_t k=0;k<7 && k<n;++k)m.meInitWords[k]=ringMem[(i+1+k)&8191];}
            count=1+n;
        } else if((header>>28)==4u)count=1+(header&0x7fu);
        i=(i+count)&8191;
    }
}
int write32(void *cookie,uint32_t reg,uint32_t value)
{
    CHECK(cookie==&m);
    if(m.failWrite && m.writes.size()+1==m.failWrite)return 1;
    m.log.push_back({'w',reg,m.depth==1});
    if(reg==A8XX_CP_APERTURE_CNTL_HOST) {
        CHECK(m.depth==1);   /* the aperture only moves under the device MMIO lock */
        CHECK(!(value&~0xf000u) && pipeOf(value)<=7);
        m.aperture=value;
    }
    const uint32_t pipe=pipeOf(m.aperture);
    m.writes.push_back({reg,value,pipe,m.depth==1,m.secure});
    if(m.pipeRegs.count(reg)) {
        CHECK(pipe!=0);   /* a pipe register is never written with the aperture at PIPE_NONE */
        m.piped[{pipe,reg}]=value;
        if(reg==A8XX_CP_SQE_STAT_ADDR_PIPE)m.sqeStatAddr[pipe]=value;
        return 0;
    }
    if(reg!=A8XX_CP_APERTURE_CNTL_HOST && reg<0x10000 && reg!=A8XX_CP_RB_WPTR)
        CHECK(pipe==0);   /* every global GPU register is written with the aperture back at 0 */
    m.regs[reg]=value;
    if(reg==A8XX_CP_SQE_CNTL)m.sqeRunning=value==1;
    if(reg==A8XX_GBIF_HALT)m.regs[A8XX_GBIF_HALT_ACK]=m.cxAck?value:0;
    if(reg==A8XX_RBBM_GBIF_HALT)m.regs[A8XX_RBBM_GBIF_HALT_ACK]=m.gxAck?value:0;
    if(reg==A8XX_CP_RB_WPTR) {
        CHECK(value<8192);
        const uint32_t old=m.wptr;m.wptr=value;
        if(m.sqeRunning && m.consume) {execute(old,value);m.rptr=value;memptrs[0]=value;}
    }
    return 0;
}
int read32(void *cookie,uint32_t reg,uint32_t *value)
{
    CHECK(cookie==&m && value);
    const uint32_t pipe=pipeOf(m.aperture);
    m.reads.push_back({reg,pipe});
    m.log.push_back({'r',reg,m.depth==1});
    if(m.failRead && m.reads.size()==m.failRead)return 1;
    if(auto f=m.forced.find(reg);f!=m.forced.end()) {*value=f->second;return 0;}
    if(m.pipeRegs.count(reg)) {
        /* Reads of a pipe register with the aperture at 0 happen where both drivers do them (the static power-up lists
         * hold RB_CMP_NC_MODE_CNTL); the model answers with its PIPE_NONE view. Writes are checked strictly. */
        if(reg==A8XX_CP_SQE_STAT_DATA_PIPE) {
            CHECK(pipe!=0);
            const uint32_t index=m.sqeStatAddr[pipe];
            *value=index==0?(pipe==A8XX_PIPE_BR?0xe3u:pipe==A8XX_PIPE_BV?0x4au:0):0x5000u+pipe*0x100u+index;
            return 0;
        }
        auto at=m.piped.find({pipe,reg});
        *value=at==m.piped.end()?(pipe<<20|reg):at->second;
        return 0;
    }
    switch(reg) {
    case A8XX_CP_RB_RPTR:*value=m.rptr;return 0;
    case A8XX_CP_RB_WPTR:*value=m.wptr;return 0;
    /* The design treats the running SQE's control as unreliable; the K90 read it back as written (sqeEcho). */
    case A8XX_CP_SQE_CNTL:*value=m.sqeEcho?m.regs[A8XX_CP_SQE_CNTL]:0;return 0;
    case A8XX_RBBM_CGC_P2S_STATUS:*value=m.p2s?1u:0u;return 0;
    default:break;
    }
    auto at=m.regs.find(reg);
    *value=at==m.regs.end()?0:at->second;
    return 0;
}
int barrier(void *cookie) {CHECK(cookie==&m);++m.barriers;m.log.push_back({'b',0,m.depth==1});return 0;}
void sleepUs(void *cookie,uint32_t us) {CHECK(cookie==&m && m.depth==0);++m.sleeps;m.clock+=us;}
void delayUs(void *cookie,uint32_t us) {CHECK(cookie==&m);++m.delays;m.clock+=us;}
uint64_t now(void *cookie) {CHECK(cookie==&m);return m.clock;}
void lock(void *cookie) {CHECK(cookie==&m && m.depth==0);++m.locks;++m.depth;}
void unlock(void *cookie) {CHECK(cookie==&m && m.depth==1);++m.unlocks;--m.depth;}
int fenceStatus(void *,int,uint32_t *status) {*status=0;return 0;}
A830CpIo cpio{&m,write32,read32,barrier,sleepUs,delayUs,now,lock,unlock};
NativeAdrenoRingIo ringio{&m,write32,read32,barrier,&m,fenceStatus,nullptr};
NativeAdrenoRingCache wc{&m,nullptr,nullptr};
struct Smmu {unsigned routes=0,sticky=0;IOReturn result=kIOReturnSuccess;uint32_t s2cr=0x300;} smmu;
IOReturn lpacRoute(void *cookie,uint32_t *readback) {CHECK(cookie==&smmu);++smmu.routes;*readback=smmu.s2cr;return smmu.result;}
void recordSticky(void *cookie) {CHECK(cookie==&smmu);++smmu.sticky;}
A830CpSmmu smmuCheck{&smmu,lpacRoute,recordSticky};
/* The annibale hooks: sGFSR (gfsr, plus `appear` once the SQE runs) and the GMU side; never called under the MMIO lock. */
struct Health {uint32_t gfsr=0,appear=0;int gmu=0;bool failRead=false;unsigned gfsrReads=0,gmuCalls=0;} health;
int globalFaultHook(void *cookie,uint32_t *sgfsr)
{
    CHECK(cookie==&health && sgfsr && m.depth==0);++health.gfsrReads;
    if(health.failRead)return 1;
    *sgfsr=health.gfsr|(m.sqeRunning?health.appear:0);return 0;
}
int gmuFaultHook(void *cookie) {CHECK(cookie==&health && m.depth==0);++health.gmuCalls;return health.gmu;}
A830CpHealth healthHooks{&health,globalFaultHook,gmuFaultHook};

void reset(uint32_t profile=A830ProfileDesign)
{
    adreno830_profile_value=profile;
    m=Model{};smmu=Smmu{};health=Health{};
    std::memset(ringMem,0xee,sizeof(ringMem));std::memset(memptrs,0xee,sizeof(memptrs));
    /* Pipe-routed registers: the BR/BV entries of the non-context table, the dynamic power-up list and the CP _PIPE block. */
    for(uint32_t i=0;i<A830_HWINIT_NONCTXT_COUNT;++i)if(A830HwInitNonctxt[i].pipes&~1u)m.pipeRegs.insert(A830HwInitNonctxt[i].offset);
    for(uint32_t i=0;i<A830_HWINIT_DYN_COUNT;++i)m.pipeRegs.insert(A830HwInitDynPwrup[i].offset);
    for(uint32_t reg:{0x8700u,0x8e08u,0x8f00u,0x84du,0x84eu,0x84fu,0x8b0u,0x8b3u,0x8b4u,0x8b7u,0x8b8u,0x8b9u,0x8bbu,0x8f0u,0x8f1u,
                      0x8e50u,0x900u,0x901u,0x902u,0x904u,0x905u,0x906u})
        m.pipeRegs.insert(reg);
    for(uint32_t i=0;i<16;++i)m.pipeRegs.insert(0x8a0u+i);
    m.regs[A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0]=0x00001234u;
}
struct Expect {uint32_t reg,value,pipe;};
std::vector<Expect> hwInitSequence(uint32_t scid)
{
    std::vector<Expect> e;
    auto add=[&e](uint32_t reg,uint32_t value,uint32_t pipe=0) {e.push_back({reg,value,pipe});};
    /* ARCHITECTURE 4.8, written out from the design table (not from A830Cp.cpp). */
    add(0x3c45,0);add(0x0a,0);                                                     /* 1 */
    add(0xf803,0);add(0xf800,0);add(0xf801,0);add(0xf802,0);                       /* 2 */
    add(0x19e,0xffffffffu);                                                        /* 3 */
    add(0xf08,0x04000000);add(0xf09,0);add(0xae10,0x04000000);add(0xae11,0);       /* 4 */
    for(uint32_t reg:{0xe08u,0xe06u,0xf06u,0xf04u}) {add(reg,0xfffff000u);add(reg+1,0x1ffffu);}   /* 5 */
    add(0x460,1);add(0x500,1);add(0x1fc41,4);add(0x8d0,1);                         /* 6 */
    add(0x8700,0x60,2);add(0x8700,0x60,1);add(0x8e08,0x6,1);add(0x8f00,0x20006,1);add(0xae02,0x26);add(0xb604,0x6);   /* 7 */
    const uint32_t bicubic[20]={0,0x3fe05ff4,0x3fa0ebee,0x3f5193ed,0x3f0243f0,0,0x3fd093e8,0x3f4133dc,0x3ea1dfdb,0x3e0283e0,
        0x0000ac2b,0x0000f01d,0x00114412,0x0021980a,0x0051ec05,0x0000380e,0x3ff09001,0x3fc10bfa,0x3f9193f7,0x3f7227f7};
    for(uint32_t i=0;i<20;++i)add(0xb606+i,bicubic[i]);                            /* 8 */
    add(0xe11,0x81);                                                               /* 9 */
    if(scid) {add(0x3c02,0x09249249);add(0x3c01,0x02492500);}                     /* 9a (SCID 9) */
    for(uint32_t pipe:{0u,1u,2u,4u,5u,6u,7u})                                      /* 10 */
        for(uint32_t i=0;i<A830_HWINIT_NONCTXT_COUNT;++i)
            if(A830HwInitNonctxt[i].pipes&(1u<<pipe))add(A830HwInitNonctxt[i].offset,A830HwInitNonctxt[i].value,pipe);
    add(0x2f,0x40cfffff);add(0x58f,0x40000000);                                   /* 11 */
    add(0x23b0e,0xff000000);add(0x1fc40,0x1220);add(0x1fc10,1);                   /* 12 */
    add(0x84e,0xffff000b,1);add(0x84e,0xffff000b,2);                               /* 13 */
    for(uint32_t i=0;i<A830_HWINIT_PROTECT_COUNT;++i)add(0x850+A830HwInitProtectRegs[i].index,A830HwInitProtectRegs[i].value);
    add(0x8af,0x80027c02,1);add(0x8af,0x80027c02,2);
    add(0x8e50,1,1);                                                               /* 14 */
    for(uint32_t pipe:{1u,2u,4u,5u,6u,7u}) {add(0x84d,pipe==1?0x6f:0x0f,pipe);add(0x8b8,0x07e83f7f,pipe);add(0x8b4,0x77f,pipe);}   /* 15 */
    add(0x61,0xffffffffu);add(0x8b6,0x007f007f);add(0x62,0x33d283c2);              /* 16 */
    return e;
}
/* annibale kernel-sequences 9.2 rows 2-31 (the direct ring), written out from the extraction (not from A830Cp.cpp). The
 * non-context table keeps the 13T value of TPL1_DBG_ECO_CNTL (section 14 row 30); 9a is the 13T LLCC step when it is on. */
std::vector<Expect> annibaleSequence(uint32_t scid)
{
    std::vector<Expect> e;
    auto add=[&e](uint32_t reg,uint32_t value,uint32_t pipe=0) {e.push_back({reg,value,pipe});};
    add(0x3c45,0);add(0x0a,0);                                                     /* row 2: the halts released */
    add(0x62,0);                                                                   /* row 3: RBBM_INT_0_MASK = 0 */
    add(0x19e,0xffffffffu);                                                        /* row 4 */
    add(0xf08,0x04000000);add(0xf09,0);add(0xae10,0x04000000);add(0xae11,0);       /* rows 5-6 */
    add(0xe08,0xfffff000u);add(0xe09,0x1ffff);add(0xe06,0xfffff000u);add(0xe07,0x1ffff);   /* rows 7-8 */
    add(0x1fc41,4);                                                                /* row 9 */
    add(0x8700,0x60,2);add(0x8700,0x60,1);add(0x8e08,6,1);add(0x8f00,0x20006,1);  /* rows 10-13 */
    add(0xae02,0x26);add(0xb604,6);                                               /* rows 14-15 */
    const uint32_t bicubic[19]={0x3fe05ff4,0x3fa0ebee,0x3f5193ed,0x3f0243f0,0,0x3fd093e8,0x3f4133dc,0x3ea1dfdb,0x3e0283e0,
        0x0000ac2b,0x0000f01d,0x00114412,0x0021980a,0x0051ec05,0x0000380e,0x3ff09001,0x3fc10bfa,0x3f9193f7,0x3f7227f7};
    for(uint32_t i=0;i<19;++i)add(0xb607+i,bicubic[i]);                           /* row 16: TABLE_1..19 */
    add(0xe11,0x81);                                                               /* row 17 */
    if(scid) {add(0x3c02,0x09249249);add(0x3c01,0x02492500);}
    for(uint32_t pipe:{0u,1u,2u,4u,5u})                                            /* row 18: pipes 0..5 without LPAC */
        for(uint32_t i=0;i<A830_HWINIT_NONCTXT_COUNT;++i)
            if(A830HwInitNonctxt[i].pipes&(1u<<pipe))add(A830HwInitNonctxt[i].offset,A830HwInitNonctxt[i].value,pipe);
    add(0x2f,0x40cfffff);add(0x58f,0x40000000);                                   /* rows 19-20 */
    add(0x23b0e,0xff000000);add(0x1fc40,0x1220);add(0x1fc10,1);                   /* rows 21-23 */
    add(0x84e,0xffff000b,1);add(0x84e,0xffff000b,2);                               /* row 24 */
    for(uint32_t i=0;i<A830_HWINIT_PROTECT_COUNT;++i)add(0x850+A830HwInitProtectRegs[i].index,A830HwInitProtectRegs[i].value);
    add(0x8af,0x80027c02,1);add(0x8af,0x80027c02,2);                               /* rows 25-26 */
    for(uint32_t pipe:{1u,2u,4u,5u,6u,7u}) {add(0x84d,pipe==1?0x6f:0x0f,pipe);add(0x8b8,0x07e83f7f,pipe);add(0x8b4,0x77f,pipe);}   /* 27 */
    add(0x8b6,0x007f007f);                                                         /* row 28 */
    add(0xf803,0);add(0xf800,0);add(0xf801,0);add(0xf802,0);                       /* rows 29-31: SECVID last */
    return e;
}
std::vector<Write> withoutAperture()
{
    std::vector<Write> w;
    for(auto &x:m.writes)if(x.reg!=A8XX_CP_APERTURE_CNTL_HOST)w.push_back(x);
    return w;
}
bool sameSequence(const std::vector<Write> &got,const std::vector<Expect> &expect)
{
    if(got.size()!=expect.size()) {std::fprintf(stderr,"%zu writes, want %zu\n",got.size(),expect.size());return false;}
    for(size_t i=0;i<got.size();++i)
        if(got[i].reg!=expect[i].reg || got[i].value!=expect[i].value || got[i].pipe!=expect[i].pipe || !got[i].locked) {
            std::fprintf(stderr,"write %zu: got %#x=%#x pipe %u, want %#x=%#x pipe %u\n",i,got[i].reg,got[i].value,got[i].pipe,
                         expect[i].reg,expect[i].value,expect[i].pipe);
            return false;
        }
    return true;
}
size_t writesTo(uint32_t reg) {size_t n=0;for(auto &w:m.writes)n+=w.reg==reg;return n;}
/* Every write of reg in the log is followed by a barrier and a read of the same register (annibale's checked write). */
bool checkedWrites(uint32_t reg)
{
    size_t seen=0;
    for(size_t i=0;i<m.log.size();++i) {
        if(m.log[i].kind!='w' || m.log[i].reg!=reg)continue;
        if(i+2>=m.log.size() || m.log[i+1].kind!='b' || m.log[i+2].kind!='r' || m.log[i+2].reg!=reg)return false;
        ++seen;
    }
    return seen>0;
}
int attachRing(NativeAdrenoRing &r) {return r.attach(&ringio,&wc,ringMem,sizeof(ringMem),0x200000,memptrs,sizeof(memptrs),0x300000);}
/* hwInit, microcode, ring and CP start on a fresh model (the SQE control echoes, as on the K90). */
void bringUp(NativeAdrenoRing &r,A830Cp &cp)
{
    CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
    m.sqeEcho=true;
    uint32_t fence=0;
    CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess);
    CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess && cp.startCp(0x301000,&fence)==kIOReturnSuccess &&
          fence==10);
}
}

int main()
{
    unsigned cases=0;
    /* ---- Design profile (A830ProfileDesign): ARCHITECTURE 4.8-4.11 ------------------------------------------------- */
    /* S6 hardware init: the exact write sequence and values (design steps 1-16 with the generated tables), every pipe register
     * under its aperture, the aperture only moved under the lock and back at 0, the halt read-backs recorded. */
    for(uint32_t scid:{0u,9u}) {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        A830HwInitConfig config=A830HwInitDefaults();config.llcc_scid=scid;
        CHECK(config.ubwc_mode==5 && config.highest_bank_bit==16 && config.min_access_length==32 && config.gmem_base==0x4000000 &&
              config.uche_trap_base==0x1fffffffff000ull);
        CHECK(cp.hwInit(config)==kIOReturnSuccess);
        CHECK(sameSequence(withoutAperture(),hwInitSequence(scid)));   /* all of hw_init holds the device MMIO lock */
        CHECK(m.aperture==0 && m.writes.front().reg==A8XX_CP_APERTURE_CNTL_HOST && m.writes.front().value==0);
        CHECK(m.writes.back().reg==A8XX_RBBM_INT_0_MASK && m.locks==1 && m.unlocks==1);
        CHECK(m.reads[0].first==0x3c45 && m.reads[1].first==0x0a);
        for(auto &e:m.log)CHECK(!(e.kind=='r' && e.reg==A8XX_CP_APERTURE_CNTL_HOST));   /* the design never reads it back */
        CHECK(cp.gmemProtect()==0x0c000000 && cp.record().hw_inits==1 && cp.secure());
        for(auto &w:m.writes)CHECK(w.reg!=0x808 && w.reg!=0x815 && w.reg!=0xf400);   /* never SQE start, never SECVID_TRUST */
        /* the design has no status check: a GPU error bit or a stale halt does not stop it */
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_CP_HW_ERROR;m.forced[A8XX_GBIF_HALT]=0x8;
        CHECK(cp.hwInit(config)==kIOReturnSuccess && cp.record().gbif_halt==0x8 && cp.record().hw_inits==2);
        ++cases;
    }
    /* SQE/AQE bases, the ring registers in KGSL order (0x808 only ever holds the BV RPTR shadow address), the CP start. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess);
        m.writes.clear();
        CHECK(cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess);
        const Expect bases[]={{0x816,0x100000,0},{0x817,0,0},{0x818,0x180000,0},{0x819,0,0}};
        CHECK(m.writes.size()==4);
        for(size_t i=0;i<4;++i)CHECK(m.writes[i].reg==bases[i].reg && m.writes[i].value==bases[i].value);
        m.writes.clear();m.rptr=0x55;m.wptr=0x77;   /* stale hardware pointers: recorded, never a criterion */
        struct {uint64_t physical,bytes,iova;volatile void *cpu;} ringBuf{0,0x8000,0x200000,ringMem},memBuf{0,0x1000,0x300000,memptrs};
        CHECK(cp.startRing(ringBuf,memBuf)==kIOReturnSuccess);
        const Expect ringRegs[]={{0x804,0x300000,0},{0x805,0,0},{0x808,0x300008,0},{0x809,0,0},{0x802,0x20c,0},{0x800,0x200000,0},
                                 {0x801,0,0}};
        CHECK(m.writes.size()==7);
        for(size_t i=0;i<7;++i)CHECK(m.writes[i].reg==ringRegs[i].reg && m.writes[i].value==ringRegs[i].value);
        CHECK(cp.record().rptr_at_start==0x55 && cp.record().wptr_at_start==0x77);
        m.rptr=0;m.wptr=0;
        m.writes.clear();
        uint32_t fence=0;
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnSuccess && fence==10);
        CHECK(smmu.routes==1 && smmu.sticky==1 && cp.record().lpac_route==0x300);
        CHECK(m.writes[0].reg==0x815 && m.writes[0].value==1 && cp.record().sqe_cntl_readback==0);
        CHECK(m.writes[1].reg==0x807 && m.writes[1].value==10 && m.writes.size()==2);
        CHECK(((ringMem[0]>>16)&0x7fu)==0x17 && (ringMem[0]&0x3fffu)==1 && ringMem[1]==0x08000000);
        CHECK(((ringMem[2]>>16)&0x7fu)==0x48 && (ringMem[2]&0x3fffu)==7);
        const uint32_t me[7]={0x14b,3,0x20000000,2,0x301000,0,0x80000000u};
        CHECK(m.meInit && !std::memcmp(m.meInitWords,me,sizeof(me)) && A830Cp::meInitMask()==0x14b);
        CHECK(cp.record().cp_starts==1 && r.writeIndex()==10 && memptrs[0]==10);
        for(auto &w:m.writes)CHECK(w.reg!=0x808);
        /* S7 steps 8-11: CP_SET_SECURE_MODE 0 through the ring, the GMEM protect rewritten after the GPU left secure mode, the
         * power-up list, then clock gating with the P2S transfer. */
        static uint8_t page[4096];
        std::memset(page,0xee,sizeof(page));
        m.writes.clear();
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess && !cp.secure() && !m.secure && cp.pwrupPatched());
        CHECK(((ringMem[10]>>16)&0x7fu)==0x66 && (ringMem[10]&0x3fffu)==1 && ringMem[11]==0 && r.writeIndex()==12);
        size_t at=0;
        CHECK(m.writes[at].reg==0x807 && m.writes[at].value==12 && m.writes[at].secure);
        ++at;
        while(m.writes[at].reg==A8XX_CP_APERTURE_CNTL_HOST)++at;
        CHECK(m.writes[at].reg==0x8e09 && m.writes[at].value==0x0c000000 && m.writes[at].pipe==1 && !m.writes[at].secure);
        /* the power-up list: the generated lists with the model's current values; triplets read under their pipe */
        CHECK(page[12]==77 && page[13]==49 && page[14]==60 && page[15]==0);
        auto word=[](size_t i) {uint32_t v;std::memcpy(&v,page+4*i,4);return v;};
        CHECK(word(4)==A830HwInitIfpc[0] && word(5)==m.regs[A830HwInitIfpc[0]]);
        CHECK(word(4+2*11)==0x8b6 && word(4+2*11+1)==0x007f007f);           /* CP_INTERRUPT_STATUS_MASK_GLOBAL as programmed */
        CHECK(word(4+2*77)==0xe01 && word(4+2*77+1)==0x00080000);           /* UCHE_MODE_CNTL from the non-context table */
        const size_t dyn=4+2*126;
        CHECK(word(dyn)==0x1000 && word(dyn+1)==0x8b4 && word(dyn+2)==0x77f);   /* BR HW fault mask as programmed */
        bool sawLpac=false,sawGmem=false;
        for(size_t t=0;t<60;++t) {
            const uint32_t pipe=word(dyn+3*t)>>12,reg=word(dyn+3*t+1),value=word(dyn+3*t+2);
            auto it=m.piped.find({pipe,reg});
            CHECK(value==(it==m.piped.end()?(pipe<<20|reg):it->second));
            sawLpac|=pipe==3;
            if(reg==0x8e09) {sawGmem=true;CHECK(value==0x0c000000);}
        }
        CHECK(sawLpac && sawGmem);
        for(size_t i=1744;i<sizeof(page);++i)CHECK(page[i]==0);
        /* HWCG: CGC_0_PC, the AO table, the global override on, load, P2S, override off */
        std::vector<Write> tail;
        for(auto &w:m.writes)if(w.reg==0x10b || w.reg==0x9a || w.reg==0x9b || w.reg==0x9c || (w.reg>=0x23b09 && w.reg<=0x23b0b))tail.push_back(w);
        const Expect hwcg[]={{0x10b,0x702,0},{0x23b09,0x20000,0},{0x23b0a,0x10111,0},{0x23b0b,0x5555,0},{0x9a,1,0},{0x9b,1,0},{0x9c,1,0},
                             {0x9a,0,0}};
        CHECK(tail.size()==8);
        for(size_t i=0;i<8;++i)CHECK(tail[i].reg==hwcg[i].reg && tail[i].value==hwcg[i].value);
        CHECK(cp.record().hwcg_timeouts==0 && cp.record().p2s_status==1 && m.aperture==0 && m.locks==m.unlocks);
        /* A second secure exit does not patch the list again (Linux: once). */
        CHECK(cp.exitSecure(A830_T_CP_IDLE_US)==kIOReturnSuccess && cp.record().secure_exits==2);
        ++cases;
    }
    /* The only start gate is the LPAC route: refused before the SQE starts; sticky SMMU status never blocks. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        smmu.result=kIOReturnNotReady;smmu.s2cr=0x20300;
        uint32_t fence=0;
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnNotReady && !fence);
        CHECK(cp.failure().step==A830CpStepLpacRoute && cp.failure().reason==A830ReasonSmmuRoute && cp.failure().value==0x20300);
        for(auto &w:m.writes)CHECK(w.reg!=0x815 && w.reg!=0x807);
        smmu.result=kIOReturnSuccess;smmu.s2cr=0x300;
        cp.setHealth(&healthHooks);   /* the design never calls the annibale hooks */
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnSuccess && fence==10 && smmu.sticky==2);
        CHECK(health.gfsrReads==0 && health.gmuCalls==0 && cp.record().sgfsr_before==0 && cp.record().sgfsr_after==0);
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnNotReady);   /* CP_ME_INIT only on a fresh ring */
        ++cases;
    }
    /* The idle criterion times out after 2 s of 100 us sleeps (no lock held): step 0x41, reason 5; the dump fills the S6 page
     * (SQE PC BR 0xE3 / BV 0x4A: the missing-LPAC-route signature) with the aperture back at 0. A hang bit is reason 18. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        m.consume=false;
        uint32_t fence=0;
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnTimeout && fence==10);
        CHECK(cp.failure().step==A830CpStepMeInit && cp.failure().reason==A830ReasonTimeout &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x806) && cp.failure().expect==10);
        CHECK(m.clock>=2000000 && m.clock<2000200 && m.sleeps>=20000 && m.sleeps<=20001);
        Adreno830Status status{};
        m.regs[A8XX_RBBM_STATUS]=0x80000005u;m.regs[A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW]=0x00800000u;
        m.piped[{1u,0x8b3u}]=0x10;m.piped[{1u,0x84fu}]=0x2;
        cp.dumpStall(&status);
        const A830CpStall &d=cp.stall();
        CHECK(d.valid && d.rptr==0 && d.wptr==10 && d.shadow_rptr==0 && d.pipe[0].sqe_stat[0]==0xe3 && d.pipe[1].sqe_stat[0]==0x4a);
        CHECK(d.pipe[0].sqe_stat[15]==0x5000u+0x100u+15 && d.pipe[1].sqe_stat[3]==0x5000u+0x200u+3);
        CHECK(d.gfx_status==0 && d.rb_cntl==0 && d.pipe[0].icache_cntl==0);   /* annibale's dump fields stay empty */
        CHECK(status.aux[0]==(10ull<<32) && status.aux[1]==(0x80000005ull|0x10ull<<32) && status.aux[2]==(0xe3ull|0x4aull<<32) &&
              status.aux[3]==(0x2ull|0x00800000ull<<32) && status.aux[4]==0 && status.aux[5]==0);
        CHECK(m.aperture==0 && m.locks==m.unlocks);
        m.consume=true;m.rptr=10;memptrs[0]=10;m.regs[A8XX_RBBM_STATUS]=1;   /* bit 0 alone is idle */
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_HANG_DETECT;
        CHECK(cp.waitIdle(1000)==kIOReturnTimeout && cp.failure().reason==A830ReasonHang);
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS;
        CHECK(cp.waitIdle(1000)==kIOReturnSuccess);
        /* GPU_CX_BUSY_STATUS and CP_INTERRUPT_STATUS_GLOBAL are only recorded in the design */
        m.regs[A8XX_GMUAO_GPU_CX_BUSY_STATUS]=1u<<23;m.regs[A8XX_CP_INTERRUPT_STATUS_GLOBAL]=0x10000;
        CHECK(cp.waitIdle(1000)==kIOReturnSuccess && cp.record().cx_busy_idle==(1u<<23));
        m.regs[A8XX_RBBM_STATUS]=0x4;
        CHECK(cp.waitIdle(500)==kIOReturnTimeout && cp.failure().reg==A830_GPU_REG_PHYS(0x12));
        ++cases;
    }
    /* Clock gating: a P2S timeout is recorded, never fatal, and leaves the override on (Linux, KGSL); off disables it. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        m.p2s=false;
        CHECK(cp.enableHwcg()==kIOReturnSuccess);
        CHECK(cp.record().hwcg_timeouts==1 && m.regs[0x9a]==1 && m.delays==10);
        size_t offWrites=0;for(auto &w:m.writes)offWrites+=w.reg==0x9a && w.value==0;
        CHECK(offWrites==0);
        m.regs[A8XX_GBIF_CX_CONFIG]=0x20023001;
        CHECK(cp.disableHwcg()==kIOReturnSuccess && m.regs[0x23b09]==0 && m.regs[0x9b]==0 && m.regs[A8XX_GBIF_CX_CONFIG]==0x20023000);
        ++cases;
    }
    /* Recovery halts: SQE halt, then GX GBIF, client and arbiter halts each acknowledged, the halt cleared; a missing GX
     * acknowledge is a timeout but the CX side still halts and the halt is still cleared. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,nullptr)==kIOReturnSuccess);
        CHECK(cp.haltSqe()==kIOReturnSuccess && m.regs[0x815]==3);
        m.writes.clear();
        CHECK(cp.haltGbif(A830_T_GBIF_HALT_US)==kIOReturnSuccess);
        const Expect halt[]={{0x0a,1,0},{0x3c45,1,0},{0x3c45,2,0},{0x3c45,0,0}};
        CHECK(m.writes.size()==4);
        for(size_t i=0;i<4;++i)CHECK(m.writes[i].reg==halt[i].reg && m.writes[i].value==halt[i].value);
        CHECK(cp.record().gbif_ack[0]==1 && cp.record().gbif_ack[1]==1 && cp.record().gbif_ack[2]==2);
        m.gxAck=false;m.writes.clear();m.clock=0;
        CHECK(cp.haltGbif(1000)==kIOReturnTimeout && cp.failure().step==A830CpStepHaltGbifGx && m.writes.size()==4 &&
              m.writes.back().reg==0x3c45 && m.writes.back().value==0 && m.clock>=1000);
        ++cases;
    }
    /* Watchdog progress and CP fault detail: pipe registers through their apertures under the lock, aperture back at 0. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,nullptr)==kIOReturnSuccess);
        m.rptr=0x40;m.regs[0x80a]=0x30;memptrs[1]=7;
        m.piped[{1u,0x900u}]=0x20001000u;m.piped[{1u,0x901u}]=0x1;m.piped[{1u,0x902u}]=0x55;
        A830CpProgress progress{};
        CHECK(cp.readProgress(&progress)==kIOReturnSuccess && progress.fence==7 && progress.rptr==0x40 && progress.bv_rptr==0x30 &&
              progress.ib1_base==0x120001000ull && progress.ib1_rem==0x55 && m.aperture==0);
        m.regs[0x8b5]=A8XX_CP_GLOBAL_INT_HWFAULT(1)|A8XX_CP_GLOBAL_INT_SWFAULT(2);
        m.piped[{1u,0x8b3u}]=0x400;m.piped[{2u,0x8b7u}]=(1u<<8)|(1u<<10);m.piped[{2u,0x84fu}]=0x8e09;
        A830CpFault fault{};
        CHECK(cp.readFault(&fault)==kIOReturnSuccess && fault.pipes==((1u<<1)|(1u<<2)) && fault.hw[1]==0x400);
        CHECK(fault.sw[2]==((1u<<8)|(1u<<10)) && fault.opcode[2]==0x5000u+0x200u+1 && fault.protect[2]==0x8e09 && m.aperture==0);
        uint32_t value=0;
        CHECK(cp.readPipe(2,0x84f,&value)==kIOReturnSuccess && value==0x8e09 && m.aperture==0);
        cp.writePipe(1,0x8e09,0x0c000000);
        CHECK((m.piped[{1u,0x8e09u}]==0x0c000000) && m.aperture==0 && m.locks==m.unlocks);
        ++cases;
    }
    /* Microcode staging (S4): the file without its first dword, the tail zeroed; the real SQE/AQE when present. */
    {
        std::vector<uint8_t> fw(4+64);
        for(size_t i=0;i<fw.size();++i)fw[i]=uint8_t(i);
        std::vector<uint8_t> dst(128,0xee);size_t copied=0;
        CHECK(A830Cp::stageMicrocode(fw.data(),fw.size(),dst.data(),dst.size(),&copied)==kIOReturnSuccess && copied==64);
        CHECK(dst[0]==4 && dst[63]==67 && dst[64]==0 && dst[127]==0);
        CHECK(A830Cp::stageMicrocode(fw.data(),fw.size(),dst.data(),63,&copied)==kIOReturnBadArgument && !copied);
        CHECK(A830Cp::stageMicrocode(fw.data(),4,dst.data(),dst.size(),&copied)==kIOReturnBadArgument);
        CHECK(A830Cp::stageMicrocode(nullptr,fw.size(),dst.data(),dst.size(),&copied)==kIOReturnBadArgument);
        const char *dir="/Users/xiaochoumao/unixnu-13t/一加13T原厂备份/GPU固件/";   /* read at test time, never copied */
        const struct {const char *name;size_t image,buffer;} images[]={{"gen80000_sqe.fw",0x1b510,0x1c000},{"gen80000_aqe.fw",0x62f8,0x7000}};
        for(auto &image:images) {
            std::string path=std::string(dir)+image.name;
            FILE *file=std::fopen(path.c_str(),"rb");
            if(!file)continue;
            std::vector<uint8_t> bytes(image.image+4);
            CHECK(std::fread(bytes.data(),1,bytes.size(),file)==bytes.size() && std::fgetc(file)==EOF);
            std::fclose(file);
            std::vector<uint8_t> buffer(image.buffer,0xee);
            CHECK(A830Cp::stageMicrocode(bytes.data(),bytes.size(),buffer.data(),buffer.size(),&copied)==kIOReturnSuccess &&
                  copied==image.image && !std::memcmp(buffer.data(),bytes.data()+4,copied) && buffer.back()==0);
        }
        ++cases;
    }
    /* Refusals before any access; access failures report the step and the register; the recovery re-runs every stage. */
    {
        reset();NativeAdrenoRing r;A830Cp cp;
        A830CpIo half=cpio;half.unlock=nullptr;
        CHECK(cp.attach(nullptr,&r)==kIOReturnBadArgument && cp.attach(&cpio,nullptr)==kIOReturnBadArgument &&
              cp.attach(&half,&r)==kIOReturnBadArgument);
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnNotReady && cp.loadMicrocode(0x100000,0)==kIOReturnNotReady);
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        CHECK(cp.attach(&cpio,&r)==kIOReturnBusy);
        A830HwInitConfig bad=A830HwInitDefaults();bad.highest_bank_bit=12;
        CHECK(cp.hwInit(bad)==kIOReturnBadArgument);
        bad=A830HwInitDefaults();bad.min_access_length=48;
        CHECK(cp.hwInit(bad)==kIOReturnBadArgument && m.writes.empty());
        CHECK(cp.loadMicrocode(0x100800,0)==kIOReturnBadArgument && cp.loadMicrocode(0,0)==kIOReturnBadArgument);
        CHECK(cp.startRing(0x204000,0x8000,0x300000)==kIOReturnBadArgument && m.writes.empty());
        uint32_t fence=0;
        CHECK(cp.startCp(0x301002,&fence)==kIOReturnBadArgument && cp.startCp(0x301000,nullptr)==kIOReturnBadArgument);
        CHECK(cp.patchPwrup(nullptr)==kIOReturnBadArgument && cp.readProgress(nullptr)==kIOReturnBadArgument);
        m.failWrite=15;   /* step 5: the eighth trap-base write */
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().reason==A830ReasonUnexpectedState &&
              cp.failure().step==A830CpStepTrapBase && m.locks==m.unlocks);
        m.failWrite=0;
        /* recovery: SQE halt, bus halt, then the whole S6/S7 sequence again on the recovered ring */
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess);
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess && cp.startCp(0x301000,&fence)==kIOReturnSuccess);
        static uint8_t page[4096];
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess);
        const uint32_t one[1]={0x70100000u};uint32_t issued=0;
        CHECK(r.submit(one,1,&issued)==kIOReturnSuccess && issued==1);
        CHECK(cp.haltSqe()==kIOReturnSuccess && cp.haltGbif(A830_T_GBIF_HALT_US)==kIOReturnSuccess);
        uint32_t last=0;
        CHECK(r.recover(&last)==kIOReturnSuccess && last==1 && memptrs[1]==1);
        m.secure=true;m.rptr=m.wptr=0;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.secure() && cp.record().hw_inits==2);
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess && cp.startCp(0x301000,&fence)==kIOReturnSuccess && fence==10);
        const size_t before=m.writes.size();
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess && !m.secure && cp.record().secure_exits==2);
        size_t gmem=0;for(size_t i=before;i<m.writes.size();++i)gmem+=m.writes[i].reg==0x8e09 && !m.writes[i].secure;
        CHECK(gmem==1 && m.locks==m.unlocks && m.depth==0);
        ++cases;
    }

    /* ---- annibale profile (the default): kernel-sequences 9.2, 9.4, 9.7; section 14 rows 29, 32, 33, 35 ------------- */
    CHECK(A830ProfileAnnibale==1 && A830ProfileDesign==0);
    /* hw_init: exactly annibale's list in annibale's order (RBBM_INT_0_MASK = 0 right after the halts, SECVID last), none of
     * the design's eight extra writes, every aperture write and both halt releases read back after a barrier, the status
     * check (RBBM_INT_0_STATUS, CP_INTERRUPT_STATUS_GLOBAL) after the final barrier and outside the lock. */
    for(uint32_t scid:{0u,9u}) {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        CHECK(adreno830_annibale());
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        cp.setHealth(&healthHooks);
        A830HwInitConfig config=A830HwInitDefaults();config.llcc_scid=scid;
        CHECK(cp.hwInit(config)==kIOReturnSuccess);
        CHECK(sameSequence(withoutAperture(),annibaleSequence(scid)));
        for(uint32_t reg:{0x460u,0x500u,0x8d0u,0xf04u,0xf05u,0xf06u,0xf07u,0xb606u,0x8e50u,0x61u})CHECK(!writesTo(reg));
        CHECK(writesTo(0x62)==1 && m.regs[0x62]==0);                    /* the interrupt mask stays 0 */
        for(auto &w:m.writes)if(w.reg==0xb600)CHECK(w.value==0x10100000u);   /* row 30: the 13T TPL1_DBG_ECO_CNTL */
        CHECK(checkedWrites(A8XX_CP_APERTURE_CNTL_HOST) && checkedWrites(0x3c45) && checkedWrites(0x0a));
        const size_t n=m.log.size();
        CHECK(n>3 && m.log[n-3].kind=='b' && m.log[n-2].kind=='r' && m.log[n-2].reg==0x6a && !m.log[n-2].locked &&
              m.log[n-1].kind=='r' && m.log[n-1].reg==0x8b5 && !m.log[n-1].locked);
        CHECK(health.gmuCalls==1 && health.gfsrReads==0);
        CHECK(m.aperture==0 && m.locks==1 && m.unlocks==1 && cp.record().hw_inits==1 && cp.gmemProtect()==0x0c000000);
        ++cases;
    }
    /* hw_init gates: a halt that does not read back 0, an aperture that does not read back, a GPU error bit or a pipe fault
     * at the status check, a GMU-side fault. CP_SW (bit 8) alone is not annibale's error. Nothing counts as a hw_init. */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        m.forced[A8XX_GBIF_HALT]=0x8;   /* the GMU left GBIF_HALT bit 3 set (K90 before OOB) and it did not clear */
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().step==A830CpStepClearHalt &&
              cp.failure().reason==A830ReasonReadback && cp.failure().reg==A830_GPU_REG_PHYS(0x3c45) && cp.failure().value==0x8);
        CHECK(!writesTo(0x0a) && m.aperture==0 && m.locks==m.unlocks);
        m.forced.clear();m.forced[A8XX_RBBM_GBIF_HALT]=1;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().reg==A830_GPU_REG_PHYS(0x0a) && !writesTo(0x62));
        m.forced.clear();m.forced[A8XX_CP_APERTURE_CNTL_HOST]=0x1000;m.writes.clear();
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().step==A830CpStepClearHalt &&
              cp.failure().reason==A830ReasonReadback && cp.failure().reg==A830_GPU_REG_PHYS(0x81c) &&
              cp.failure().value==0x1000 && cp.failure().expect==0 && !writesTo(0x3c45));
        m.forced.clear();
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_CP_SW;   /* not in 0x33C002C2 */
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess);
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_CP_HW_ERROR;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().step==A830CpStepHealth &&
              cp.failure().reason==A830ReasonCpFault && cp.failure().reg==A830_GPU_REG_PHYS(0x6a) && cp.failure().value==0x200);
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_HANG_DETECT;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().reason==A830ReasonHang);
        m.regs[A8XX_RBBM_INT_0_STATUS]=0;m.regs[A8XX_CP_INTERRUPT_STATUS_GLOBAL]=A8XX_CP_GLOBAL_INT_SWFAULT(1);
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().reason==A830ReasonCpFault &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x8b5) && cp.failure().value==0x10000);
        m.regs[A8XX_CP_INTERRUPT_STATUS_GLOBAL]=0;
        cp.setHealth(&healthHooks);health.gmu=7;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnIOError && cp.failure().step==A830CpStepHealth &&
              cp.failure().reason==A830ReasonGmuFault && cp.failure().value==7);
        health.gmu=0;
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.record().hw_inits==2 && m.locks==m.unlocks);
        ++cases;
    }
    /* Microcode bases and the ring: each written, fenced and read back equal; the ring only on a cold CP (RPTR = WPTR = 0)
     * and in annibale's order (base, control, BR shadow address, BV shadow address). */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess);
        m.writes.clear();m.log.clear();
        CHECK(cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess);
        const Expect bases[]={{0x816,0x100000,0},{0x817,0,0},{0x818,0x180000,0},{0x819,0,0}};
        CHECK(m.writes.size()==4);
        for(size_t i=0;i<4;++i)CHECK(m.writes[i].reg==bases[i].reg && m.writes[i].value==bases[i].value);
        const Event order[]={{'w',0x816,false},{'w',0x817,false},{'b',0,false},{'r',0x816,false},{'r',0x817,false},
                             {'w',0x818,false},{'w',0x819,false},{'b',0,false},{'r',0x818,false},{'r',0x819,false}};
        CHECK(m.log.size()==10);
        for(size_t i=0;i<10;++i)CHECK(m.log[i].kind==order[i].kind && m.log[i].reg==order[i].reg);
        m.forced[0x819]=0xdead;
        CHECK(cp.loadMicrocode(0x100000,0x180000)==kIOReturnIOError && cp.failure().step==A830CpStepMicrocode &&
              cp.failure().reason==A830ReasonReadback && cp.failure().reg==A830_GPU_REG_PHYS(0x819) &&
              cp.failure().value==0xdead && cp.failure().expect==0);
        m.forced.clear();m.forced[0x816]=0;m.writes.clear();
        CHECK(cp.loadMicrocode(0x100000,0x180000)==kIOReturnIOError && cp.failure().reg==A830_GPU_REG_PHYS(0x816) &&
              !writesTo(0x818));   /* the AQE base is not written after a failed SQE base */
        m.forced.clear();
        /* the ring: stale pointers refuse it before any write */
        m.writes.clear();m.rptr=0x55;
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnNotReady && cp.failure().step==A830CpStepRing &&
              cp.failure().reason==A830ReasonUnexpectedState && cp.failure().reg==A830_GPU_REG_PHYS(0x806) &&
              cp.failure().value==0x55 && m.writes.empty() && cp.record().rptr_at_start==0x55);
        m.rptr=0;m.wptr=3;
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnNotReady && cp.failure().reg==A830_GPU_REG_PHYS(0x807) &&
              cp.failure().value==(3ull<<32) && m.writes.empty());
        m.wptr=0;m.forced[0x802]=0x20d;
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnIOError && cp.failure().reason==A830ReasonReadback &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x802) && cp.failure().value==0x20d && cp.failure().expect==0x20c &&
              m.writes.size()==3);
        m.forced.clear();m.writes.clear();m.log.clear();
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess && cp.record().rptr_at_start==0 &&
              cp.record().wptr_at_start==0);
        const Expect ringRegs[]={{0x800,0x200000,0},{0x801,0,0},{0x802,0x20c,0},{0x804,0x300000,0},{0x805,0,0},
                                 {0x808,0x300008,0},{0x809,0,0}};
        CHECK(m.writes.size()==7);
        for(size_t i=0;i<7;++i)CHECK(m.writes[i].reg==ringRegs[i].reg && m.writes[i].value==ringRegs[i].value);
        CHECK(m.log[0].kind=='r' && m.log[0].reg==0x806 && m.log[1].kind=='r' && m.log[1].reg==0x807);   /* pointers first */
        for(auto &reg:ringRegs)CHECK(checkedWrites(reg.reg));
        ++cases;
    }
    /* The SQE start: CP_SQE_CNTL must read back 1 before anything reaches the ring; sGFSR is sampled around the start
     * (a value already set is recorded, a bit that appears is fatal, a failed read is fatal); without hooks no sample. */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        uint32_t fence=0;
        m.writes.clear();   /* sqeEcho off: the SQE control reads 0 */
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnIOError && !fence && cp.failure().step==A830CpStepSqeStart &&
              cp.failure().reason==A830ReasonReadback && cp.failure().reg==A830_GPU_REG_PHYS(0x815) &&
              cp.failure().value==0 && cp.failure().expect==1);
        CHECK(writesTo(0x815)==1 && !writesTo(0x807) && r.writeIndex()==0 && !m.meInit && health.gfsrReads==0);
        cp.setHealth(&healthHooks);
        m.sqeEcho=true;health.failRead=true;m.writes.clear();
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnIOError && cp.failure().reason==A830ReasonSmmuFault &&
              cp.failure().reg==0x03DA0048ull && !writesTo(0x815));   /* no sample, no start */
        health.failRead=false;health.gfsr=0x2;health.appear=0x4;m.writes.clear();m.sqeRunning=false;   /* a stopped SQE */
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnIOError && cp.failure().step==A830CpStepSqeStart &&
              cp.failure().reason==A830ReasonSmmuFault && cp.failure().reg==0x03DA0048ull && cp.failure().value==0x6 &&
              cp.failure().expect==0x2 && !writesTo(0x807));
        CHECK(cp.record().sgfsr_before==0x2 && cp.record().sgfsr_after==0x6);
        health.appear=0;m.writes.clear();m.sleeps=0;m.sqeRunning=false;
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnSuccess && fence==10 && cp.record().sgfsr_before==0x2 &&
              cp.record().sgfsr_after==0x2 && cp.record().sqe_cntl_readback==1 && health.gfsrReads==5);
        CHECK(m.meInit && r.writeIndex()==10 && cp.record().cp_starts==1 && m.sleeps==0 && health.gmuCalls==2);
        ++cases;
    }
    /* The CP_ME_INIT wait: 2000 polls of 100 us (200 ms) and the criterion that failed; CX busy and RBBM_STATUS & ~1 block,
     * read pointers outside the ring, a GPU error, a pipe fault or a GMU fault stop at once (no sleep); the dump carries
     * annibale's registers and never replaces the failure it documents. */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        CHECK(attachRing(r)==kIOReturnSuccess && cp.attach(&cpio,&r,&smmuCheck)==kIOReturnSuccess);
        cp.setHealth(&healthHooks);
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess &&
              cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess);
        m.sqeEcho=true;m.consume=false;m.clock=0;m.sleeps=0;
        uint32_t fence=0;
        CHECK(cp.startCp(0x301000,&fence)==kIOReturnTimeout && fence==10);
        CHECK(cp.failure().step==A830CpStepMeInit && cp.failure().reason==A830ReasonTimeout &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x806) && cp.failure().value==0 && cp.failure().expect==10);
        CHECK(m.sleeps==2000 && m.clock==200000 && health.gmuCalls==1+2000);
        /* the dump: annibale's registers too, sGFSR through the hook, the failure kept even when an aperture does not read back */
        m.regs[A8XX_RBBM_GFX_STATUS]=0x15;m.regs[A8XX_RBBM_GFX_BR_STATUS]=0x1a;m.regs[A8XX_RBBM_GFX_BV_STATUS]=0x1c;
        m.regs[A8XX_RBBM_MISC_STATUS]=A8XX_RBBM_MISC_STATUS_SMMU_STALLED_ON_FAULT;m.piped[{2u,0x8b0u}]=0xb0;health.gfsr=0x2;
        Adreno830Status status{};
        cp.dumpStall(&status);
        const A830CpStall &d=cp.stall();
        CHECK(d.valid && d.wptr==10 && d.gfx_status==0x15 && d.gfx_br_status==0x1a && d.gfx_bv_status==0x1c && d.misc_status==1);
        CHECK(d.rb_cntl==0x20c && d.rb_base==0x200000 && d.sqe_base==0x100000 && d.sgfsr==0x2);
        CHECK(d.pipe[0].icache_cntl==(1u<<20|0x8b0u) && d.pipe[1].icache_cntl==0xb0 && d.pipe[0].sqe_stat[0]==0xe3);
        CHECK(status.aux[0]==(10ull<<32) && status.aux[2]==(0xe3ull|0x4aull<<32) && m.aperture==0 && m.locks==m.unlocks);
        m.forced[A8XX_CP_APERTURE_CNTL_HOST]=0x7000;
        cp.dumpStall(&status);
        CHECK(!cp.stall().valid && cp.failure().step==A830CpStepMeInit && cp.failure().reason==A830ReasonTimeout);
        m.forced.clear();
        /* the criteria one by one (the ring's write index is 10) */
        m.rptr=10;memptrs[0]=10;memptrs[2]=0;m.sleeps=0;
        m.regs[A8XX_GMUAO_GPU_CX_BUSY_STATUS]=1u<<23;
        CHECK(cp.waitIdle(1000)==kIOReturnTimeout && cp.failure().reason==A830ReasonTimeout &&
              cp.failure().reg==0x03D8EC30ull && cp.failure().value==(1u<<23) && m.sleeps==10);
        m.regs[A8XX_GMUAO_GPU_CX_BUSY_STATUS]=1u<<30;m.regs[A8XX_RBBM_STATUS]=0x4;   /* bit 30 is not annibale's busy bit */
        CHECK(cp.waitIdle(1000)==kIOReturnTimeout && cp.failure().reg==A830_GPU_REG_PHYS(0x12) && cp.failure().value==0x4);
        m.regs[A8XX_RBBM_STATUS]=1;
        CHECK(cp.waitIdle(1000)==kIOReturnSuccess && cp.record().cx_busy_idle==(1u<<30));
        m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_CP_SW;   /* not annibale's error */
        CHECK(cp.waitIdle(0)==kIOReturnSuccess);
        m.sleeps=0;m.regs[A8XX_CP_INTERRUPT_STATUS_GLOBAL]=A8XX_CP_GLOBAL_INT_SWFAULT(1);
        CHECK(cp.waitIdle(A830_T_CP_IDLE_US)==kIOReturnIOError && cp.failure().reason==A830ReasonCpFault &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x8b5) && m.sleeps==0);
        adreno830_profile_value=A830ProfileDesign;   /* the same state is idle for the design (CP global not a criterion) */
        CHECK(cp.waitIdle(1000)==kIOReturnSuccess);
        adreno830_profile_value=A830ProfileAnnibale;
        m.regs[A8XX_CP_INTERRUPT_STATUS_GLOBAL]=0;m.regs[A8XX_RBBM_INT_0_STATUS]=A8XX_RBBM_INT_0_HANG_DETECT;
        CHECK(cp.waitIdle(A830_T_CP_IDLE_US)==kIOReturnIOError && cp.failure().reason==A830ReasonHang && m.sleeps==0);
        m.regs[A8XX_RBBM_INT_0_STATUS]=0;m.rptr=8192;
        CHECK(cp.waitIdle(A830_T_CP_IDLE_US)==kIOReturnIOError && cp.failure().reason==A830ReasonUnexpectedState &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x806) && cp.failure().expect==8192 && m.sleeps==0);
        m.rptr=10;memptrs[2]=9000;
        CHECK(cp.waitIdle(A830_T_CP_IDLE_US)==kIOReturnIOError && cp.failure().reg==A830_GPU_REG_PHYS(0x80a) &&
              cp.failure().value==(9000ull<<32) && m.sleeps==0);
        memptrs[2]=0;health.gmu=3;
        CHECK(cp.waitIdle(A830_T_CP_IDLE_US)==kIOReturnIOError && cp.failure().reason==A830ReasonGmuFault &&
              cp.failure().value==3 && m.sleeps==0);
        health.gmu=0;m.rptr=4;   /* the BR pointer behind the write index: not idle, never fatal */
        CHECK(cp.waitIdle(250)==kIOReturnTimeout && cp.failure().reg==A830_GPU_REG_PHYS(0x806) &&
              cp.failure().value==(4ull|10ull<<32) && m.sleeps==3);
        ++cases;
    }
    /* After the ZAP: the secure exit with annibale's criterion, the GMEM protect rewrite after it, the power-up list, and no
     * clock gating (section 14 row 35); an explicit enableHwcg polls P2S 3 x 1 us after a barrier and fails without TXDONE. */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        bringUp(r,cp);
        static uint8_t page[4096];
        std::memset(page,0xee,sizeof(page));
        m.writes.clear();
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess && !cp.secure() && !m.secure && cp.pwrupPatched());
        CHECK(r.writeIndex()==12 && m.writes[0].reg==0x807 && m.writes[0].value==12 && m.writes[0].secure);
        size_t gmem=0;for(auto &w:m.writes)gmem+=w.reg==0x8e09 && w.pipe==1 && !w.secure && w.value==0x0c000000;
        CHECK(gmem==1 && page[12]==77 && page[13]==49 && page[14]==60);
        for(uint32_t reg:{0x10bu,0x9au,0x9bu,0x9cu,0x23b09u,0x23b0au,0x23b0bu})CHECK(!writesTo(reg));
        CHECK(cp.record().hwcg_timeouts==0 && cp.record().p2s_status==0 && m.locks==m.unlocks);
        m.writes.clear();m.log.clear();m.delays=0;
        CHECK(cp.enableHwcg()==kIOReturnSuccess && m.delays==1 && m.regs[0x9a]==0 && writesTo(0x10b)==1);
        size_t trigger=0;
        for(size_t i=0;i<m.log.size();++i)if(m.log[i].kind=='w' && m.log[i].reg==0x9c)trigger=i;
        CHECK(m.log[trigger+1].kind=='b' && m.log[trigger+2].kind=='r' && m.log[trigger+2].reg==0x9f);
        m.p2s=false;m.delays=0;m.writes.clear();
        CHECK(cp.enableHwcg()==kIOReturnTimeout && cp.failure().step==A830CpStepHwcg && cp.failure().reason==A830ReasonTimeout &&
              cp.failure().reg==A830_GPU_REG_PHYS(0x9f) && cp.failure().expect==1 && m.delays==3 && cp.record().hwcg_timeouts==1);
        size_t offWrites=0;for(auto &w:m.writes)offWrites+=w.reg==0x9a && w.value==0;
        CHECK(offWrites==0 && m.regs[0x9a]==1);   /* the override stays on */
        ++cases;
    }
    /* The recovery re-runs the whole sequence in the annibale profile: hw_init, bases, a cold ring, the SQE start, the wait. */
    {
        reset(A830ProfileAnnibale);NativeAdrenoRing r;A830Cp cp;
        bringUp(r,cp);
        static uint8_t page[4096];
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess);
        const uint32_t one[1]={0x70100000u};uint32_t issued=0,fence=0,last=0;
        CHECK(r.submit(one,1,&issued)==kIOReturnSuccess && issued==1);
        CHECK(cp.haltSqe()==kIOReturnSuccess && cp.haltGbif(A830_T_GBIF_HALT_US)==kIOReturnSuccess);
        CHECK(r.recover(&last)==kIOReturnSuccess && last==1);
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnNotReady);   /* the CP was not reset: pointers not cold */
        m.secure=true;m.rptr=m.wptr=0;   /* GX went through a power cycle */
        CHECK(cp.hwInit(A830HwInitDefaults())==kIOReturnSuccess && cp.loadMicrocode(0x100000,0x180000)==kIOReturnSuccess);
        CHECK(cp.startRing(0x200000,0x8000,0x300000)==kIOReturnSuccess && cp.startCp(0x301000,&fence)==kIOReturnSuccess &&
              fence==10 && cp.record().hw_inits==2 && cp.record().cp_starts==2);
        CHECK(cp.finishSecure(page,A830_T_CP_IDLE_US)==kIOReturnSuccess && !m.secure && cp.record().secure_exits==2);
        CHECK(m.locks==m.unlocks && m.depth==0 && m.aperture==0);
        ++cases;
    }
    adreno830_profile_value=A830ProfileAnnibale;
    std::printf("PASS: A830Cp over a register-level CP model, design and annibale profiles: S6 sequences (2+2 configurations) "
                "against ARCHITECTURE 4.8, annibale 9.2 and A830HwInit.h, strict read-backs and the status check, cold ring, "
                "SQE_CNTL and sGFSR gates, ME_INIT criteria (2 s / 2000 x 100 us), dumps, secure exit with GMEM protect, "
                "power-up list, HWCG (10 x 1 us recorded / none, 3 x 1 us fatal), recovery halts and re-runs, fault reads "
                "(%u cases)\n",cases);
}
