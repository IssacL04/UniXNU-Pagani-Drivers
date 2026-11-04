/* SPDX-License-Identifier: MIT */
/* host-test: A830Gmu.cpp A830GmuFirmware.cpp NativeAdrenoGmuMemory.cpp NativeAdrenoMemory.cpp platform/sm8750/Sm8750Rpmh.c
 * A830Gmu over a GMU register model keyed by the physical addresses ARCHITECTURE 4.7.3-4.7.9, 4.4.2, 4.12.3 and 4.17 write
 * down (an oracle independent of A830Regs.h): B1-B8 with the real gen80000_gmu.bin (A830_GMU_FW, else a synthetic image of the
 * same layout), the fatal conditions ARCHITECTURE 10 keeps and the residues it records, OOB reference counting, the GX
 * confirmation that never touches GX, the counters, force-off in KGSL order, RSCC sleep/wake with GENERAL_9, slumber S2-S9,
 * the IFPC helpers, the CX timer seed, the 336/337 top halves, bottom half and storm guard, attach/re-enable, the poll mode,
 * the MMIO adapter and the temporary DRV-2 GX vote, all in the design profile; then the annibale profile of A830Profile.h
 * (kernel-sequences 4.6 and section 14 rows 6, 16-22): masked-and-polled interrupts in its order, the RSC woken when it is
 * asleep at the cold boot with the request restored, GENERAL_11 recorded, every TCM word read back, AHB_FENCE_RANGE_0
 * 0x80C808C0, the 500 ms HFI limit, no CX timer seed, the strictly read-back AHB timeouts; each against the design. */
#include "XnuMemoryFixture.h"
#include "A830Gmu.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

/* --- host doubles: memory, status ----------------------------------------------------------------------------------- */
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state) {CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();return kIOReturnSuccess;}
extern "C" {Adreno830Status adreno830_status;}
static unsigned publishes;
void adreno830_status_publish(void) {++publishes;}

/* --- the register model ------------------------------------------------------------------------------------------- */
/* Physical addresses as ARCHITECTURE writes them. */
enum : uint64_t {
    SYSRESET=0x3D7E000,BOOT_CONFIG=0x3D7E004,INIT=0x3D7E070,CM3_CFG=0x3D7E0B4,HFI_STATUS=0x3D7E600,QTBL_INFO=0x3D7E610,
    QTBL_ADDR=0x3D7E614,HFI_INIT=0x3D7E618,CLR=0x3D7E644,INFO=0x3D7E648,MASK=0x3D7E64C,H2G=0x3D7E650,GENERAL8=0x3D7E720,
    GENERAL9=0x3D7E724,GENERAL10=0x3D7E728,ICACHE=0x3D7D000,DCACHE=0x3D7D004,SYSBUS=0x3D7D03C,MRC_QOS=0x3D7D42C,
    KEEPALIVE=0x3D7DF90,GXSTAT=0x3D7DFA0,RPMH=0x3D7DFA4,FAL=0x3D7DFB0,FALNEXT=0x3D7DFB4,PWRCNT_EN=0x3D7F040,XOCLK0=0x3D7F100,
    XOCLK1=0x3D7F104,BUSY_L=0x3D7F280,BUSY_H=0x3D7F284,AO_CLR=0x3D8EC10,AO_STATUS=0x3D8EC14,AO_MASK=0x3D8EC18,
    RSCC_REQ=0x3D8EC1C,RSCC_ACK=0x3D8EC20,CGC_MODE=0x3D8EC24,CGC_DELAY=0x3D8EC28,CGC_HYST=0x3D8EC2C,CX_BUSY=0x3D8EC30,
    CX_BUSY_MASK=0x3D8EC38,FENCE_CTRL=0x3D8EC40,FENCE_RANGE=0x3D8EC44,FENCE_STATUS=0x3D8EC4C,FENCE_CLR=0x3D8EC50,
    ITCM=0x3D6D000,DTCM=0x3D71000,CORE_VER=0x3D74FE0,PWR_VER=0x3D74FE8,HFI_VER=0x3D74FF0,GX_GDSCR=0x3D68024,GX_CFG=0x3D68028,
    GENERAL11=0x3D7E72C,
    RSC_STATUS0=0x3D50010,SEQ_BUSY=0x3D50404,TCS0=0x3D50D28,TCS_STRIDE=0x2A0,
    GBIF_CX_CONFIG=0x3D0F000,QSB_SIDE0=0x3D0F00C,GBIF_HALT=0x3D0F114,GBIF_ACK=0x3D0F118,GBIF_START=0x3D0F000,
    CX_AO_LO=0x3D9E200,CX_AO_HI=0x3D9E204,CX_GXSTAT=0x3D9E20C,
};
static const uint64_t base[A830GmuWindows]={0x3D37000,0x3D40000,0x3D50000,0x3D00000,0x3D9E000};
static const uint32_t limit[A830GmuWindows]={0x68000,0x10000,0x10000,0x40000,0x2000};
enum Boot {BootOk,BootNever,BootNmi,BootCm3,BootWdog,BootLegacy};
struct Access {char op;uint64_t phys;uint32_t value;uint64_t time;};
struct Model {
    std::map<uint64_t,uint32_t> reg;
    std::vector<Access> log;
    uint64_t now=0;
    Boot boot=BootOk;uint32_t boot_us=3000;
    bool hfi_ok=true;uint32_t hfi_us=400;
    bool oob_ok=true;uint32_t oob_us=200;bool gx_ok=true;uint32_t gx_us=30;
    bool rscc_sleep_ok=true,rscc_wake_ok=true,gbif_ack=true,gx_collapse_ok=true,tcs_busy=false,cx_busy=false,cx_busy23=false;
    bool cm3_reset=true;
    uint32_t noise_info=0,noise_ao=0,sticky_info=0,sticky_ao=0,fence_drops=0,idle_state=0,general9=0x1234;
    uint32_t cfg_ignore=0;            /* B6 register that ignores writes (diag read back) */
    uint64_t release_at=~0ull,hfi_at=~0ull,oob_at=~0ull,gx_at=~0ull,noise_at=~0ull;uint32_t noise_us=1000;
    uint32_t oob_ack=0;
    unsigned reads=0,writes=0,fail_read=0,fail_write=0,gx_accesses=0,sleeps=0,delays=0,barriers=0;
    /* CPU side for the CX timer */
    uint64_t counter=1000;uint32_t counter_step=3,cx_write_cost=5;bool irq_enabled=true;unsigned irq_offs=0,cx_writes_irq_on=0;
    std::vector<uint64_t> counter_reads;
    /* hooks */
    int reset_fail=0,drain_fail=0,vote_fail=0;
    unsigned resets=0,locks=0,unlocks=0,drains=0,faults=0;bool locked=false;
    uint16_t fault_reason=0;uint32_t fault_info=0,fault_ao=0;
    std::vector<int> votes;
};
static Model m;
static uint32_t &R(uint64_t phys) {return m.reg[phys];}
static void initial()
{
    m=Model{};
    R(SYSRESET)=1;R(GXSTAT)=3;R(RPMH)=0;R(SEQ_BUSY)=0;
    R(XOCLK0)=0x13170000;R(XOCLK1)=0x000000ff;
}
static void advance()
{
    if(m.release_at<=m.now) {
        m.release_at=~0ull;
        switch(m.boot) {
        case BootOk: R(INIT)=0x100;break;
        case BootLegacy: R(INIT)=0xBABEFACE;break;
        case BootNmi: R(INIT)=0x200;break;
        case BootCm3: R(INFO)|=1u<<15;break;
        case BootWdog: R(AO_STATUS)|=1u;break;
        case BootNever: break;
        }
        if(m.boot==BootOk || m.boot==BootLegacy) {R(RPMH)=m.idle_state;R(GENERAL9)=m.general9;}
    }
    if(m.noise_at<=m.now) {m.noise_at=~0ull;R(INFO)|=m.noise_info;R(AO_STATUS)|=m.noise_ao;}
    if(m.hfi_at<=m.now) {m.hfi_at=~0ull;R(HFI_STATUS)=1;}
    if(m.oob_at<=m.now) {
        m.oob_at=~0ull;R(INFO)|=m.oob_ack;
        if(m.oob_ack==(1u<<31) && m.gx_ok && (R(GXSTAT)&3))m.gx_at=m.now+m.gx_us;
    }
    if(m.gx_at<=m.now) {m.gx_at=~0ull;R(GXSTAT)=0;}
}
static uint64_t physOf(unsigned window,uint32_t offset)
{
    CHECK(window<A830GmuWindows && window!=A830GmuWindowGmuAo && !(offset&3) && offset<limit[window]);
    return base[window]+offset;
}
static int read32(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    CHECK(cookie==&m && value);
    const uint64_t phys=physOf(window,offset);
    ++m.reads;
    if(window==A830GmuWindowGpu && phys<GBIF_START)++m.gx_accesses;
    if(m.reads==m.fail_read)return 1;
    advance();
    uint32_t v=m.reg.count(phys)?m.reg[phys]:0;
    if(phys==FENCE_STATUS) {v=m.fence_drops?1u:0u;if(m.fence_drops)--m.fence_drops;}
    if(phys>=TCS0 && phys<TCS0+10*TCS_STRIDE && !((phys-TCS0)%TCS_STRIDE))v=m.tcs_busy?0:1;
    if(phys==CX_BUSY)v=(m.cx_busy?(1u<<30):0)|(m.cx_busy23?(1u<<23):0);
    *value=v;
    m.log.push_back({'r',phys,v,m.now});
    return 0;
}
static int write32(void *cookie,unsigned window,uint32_t offset,uint32_t value)
{
    CHECK(cookie==&m);
    const uint64_t phys=physOf(window,offset);
    ++m.writes;
    if(window==A830GmuWindowGpu && phys<GBIF_START)++m.gx_accesses;
    if(m.writes==m.fail_write)return 1;
    advance();
    m.log.push_back({'w',phys,value,m.now});
    switch(phys) {
    case SYSRESET:
        m.cm3_reset=value!=0;
        m.release_at=value?~0ull:m.now+(m.boot==BootOk || m.boot==BootLegacy?m.boot_us:m.boot_us/3);
        m.noise_at=value || !(m.noise_info|m.noise_ao)?~0ull:m.now+m.noise_us;
        break;
    case HFI_INIT: if(value==1 && m.hfi_ok && !m.cm3_reset)m.hfi_at=m.now+m.hfi_us;break;
    case H2G:
        if(value==(1u<<30) && m.oob_ok) {m.oob_ack=1u<<31;m.oob_at=m.now+m.oob_us;}
        if(value==(1u<<28) && m.oob_ok) {m.oob_ack=1u<<30;m.oob_at=m.now+m.oob_us;}
        return 0;   /* write-only */
    case CLR: R(INFO)&=~(value&~m.sticky_info);return 0;
    case AO_CLR: R(AO_STATUS)&=~(value&~m.sticky_ao);return 0;
    case RSCC_REQ:
        if((value&1) && m.rscc_sleep_ok)R(RSC_STATUS0)|=1u<<16;
        if((value&2) && m.rscc_wake_ok) {R(RSCC_ACK)|=2;R(SEQ_BUSY)=0;R(RSC_STATUS0)&=~(1u<<16);}
        if(!value)R(RSCC_ACK)=0;
        break;
    case GBIF_HALT: R(GBIF_ACK)=m.gbif_ack?(value&3):0;break;
    case GX_GDSCR: if((value&1) && m.gx_collapse_ok) {R(GX_CFG)|=1u<<15;R(GXSTAT)=3;}break;
    case CX_AO_LO: case CX_AO_HI:
        m.counter+=m.cx_write_cost;
        if(m.irq_enabled)++m.cx_writes_irq_on;
        break;
    default: break;
    }
    if(phys!=m.cfg_ignore)R(phys)=value;
    return 0;
}
static void barrier(void *cookie) {CHECK(cookie==&m);++m.barriers;}
static uint64_t now_us(void *cookie) {CHECK(cookie==&m);return m.now;}
static void sleep_us(void *cookie,uint32_t us) {CHECK(cookie==&m && us);m.now+=us;++m.sleeps;}
static void delay_us(void *cookie,uint32_t us) {CHECK(cookie==&m && us);m.now+=us;++m.delays;}
static uint64_t counter(void *cookie) {CHECK(cookie==&m);m.counter+=m.counter_step;m.counter_reads.push_back(m.counter);return m.counter;}
static uintptr_t irq_disable(void *cookie) {CHECK(cookie==&m);const uintptr_t was=m.irq_enabled;m.irq_enabled=false;++m.irq_offs;return was;}
static void irq_restore(void *cookie,uintptr_t state) {CHECK(cookie==&m);m.irq_enabled=state!=0;}
static int hook_reset(void *cookie) {CHECK(cookie==&m);++m.resets;m.log.push_back({'H',1,0,m.now});return m.reset_fail;}
static void hook_lock(void *cookie) {CHECK(cookie==&m && !m.locked);m.locked=true;++m.locks;}
static void hook_unlock(void *cookie) {CHECK(cookie==&m && m.locked);m.locked=false;++m.unlocks;}
static int hook_drain(void *cookie) {CHECK(cookie==&m && !m.locked);++m.drains;return m.drain_fail;}
static void hook_fault(void *cookie,uint16_t reason,uint32_t info,uint32_t ao)
{CHECK(cookie==&m && m.locked);++m.faults;m.fault_reason=reason;m.fault_info=info;m.fault_ao=ao;}
static int hook_vote(void *cookie,int on) {CHECK(cookie==&m);m.votes.push_back(on);m.log.push_back({'V',0,uint32_t(on),m.now});return m.vote_fail;}

static A830GmuIo gio{&m,read32,write32,barrier};
static A830GmuOs gos{&m,now_us,sleep_us,delay_us,counter,irq_disable,irq_restore};
static A830GmuHooks hooks{&m,hook_reset,hook_lock,hook_unlock,hook_drain,hook_fault,hook_vote,&m};
static A830GmuBlocks blocks;
static NativeAdrenoGmuMemory memory;
static std::vector<uint8_t> image;

/* --- interrupt plumbing doubles --------------------------------------------------------------------------------------- */
static struct {
    IOReturn status=kIOReturnSuccess;uint32_t readback=0xA003;bool null_work=false;unsigned kicks=0;
    std::vector<uint32_t> attached,disabled,enabled;void (*handler[2])(void *)={};void *context=nullptr;
} irq;
static int work_token;
static IOReturn opAttach(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router)
{
    CHECK(!edge && handler && context && readback && router);
    irq.handler[irq.attached.size()&1]=handler;irq.context=context;irq.attached.push_back(intid);
    *readback=irq.readback;*router=0x80000000;
    return irq.status;
}
static IOReturn opDisable(uint32_t intid) {irq.disabled.push_back(intid);return kIOReturnSuccess;}
static IOReturn opEnable(uint32_t intid) {irq.enabled.push_back(intid);return kIOReturnSuccess;}
static void *opAllocate(A830Gmu *gmu) {CHECK(gmu);return irq.null_work?nullptr:&work_token;}
static void opKick(void *work) {CHECK(work==&work_token);++irq.kicks;}
static const A830GmuIrqOps ops{opAttach,opDisable,opEnable,opAllocate,opKick};

/* --- helpers ------------------------------------------------------------------------------------------------------- */
static long indexOf(char op,uint64_t phys,long from=0,int64_t value=-1)
{
    for(size_t i=from;i<m.log.size();++i)
        if(m.log[i].op==op && m.log[i].phys==phys && (value<0 || m.log[i].value==uint32_t(value)))return long(i);
    return -1;
}
static unsigned countOf(char op,uint64_t phys,int64_t value=-1)
{
    unsigned n=0;
    for(const Access &a:m.log)if(a.op==op && a.phys==phys && (value<0 || a.value==uint32_t(value)))++n;
    return n;
}
static void bindGmu(A830Gmu &gmu,uint32_t diag=0,const A830GmuHooks *h=&hooks)
{
    A830GmuConfig config;
    A830GmuDefaultConfig(&config);
    config.diag=diag;
    CHECK(gmu.bind(gio,gos,config,&blocks,&memory,h)==kIOReturnSuccess && gmu.bound());
}
static void put32(std::vector<uint8_t> &out,uint32_t value) {for(int i=0;i<4;++i)out.push_back(uint8_t(value>>(8*i)));}
static void block(std::vector<uint8_t> &out,uint32_t addr,uint32_t size,uint32_t type,uint32_t value,const uint8_t *data=nullptr)
{
    put32(out,addr);put32(out,size);put32(out,type);put32(out,value);
    for(uint32_t i=0;i<size;++i)out.push_back(data?data[i]:uint8_t(i*7+addr));
}
static std::vector<uint8_t> synthetic(uint32_t core)
{
    std::vector<uint8_t> out;
    const uint32_t meta[][3]={{0x0,0x1ec,1},{0x4000,0x15824,1},{0x200,0x37a8,1},{0x100000,0x1b878,1},{0x200000,0x1eaa0,1},
        {0x300000,0x24548,1},{0x10004000,0x2de8,1},{0x10007000,0xf84,1},{0x10007fdc,0x18,1},{0x60000000,0x1000,7},{0x60001000,0x1000,7}};
    for(const auto &meta_block:meta)block(out,meta_block[0],0,meta_block[2],meta_block[1]);
    uint8_t itcm[0x1ec];
    for(uint32_t i=0;i<sizeof itcm;++i)itcm[i]=uint8_t(i*13+1);
    const uint32_t boot[4]={0x10007F80,0x105,0x145,0x159};
    std::memcpy(itcm,boot,sizeof boot);
    block(out,0x0,0x1ec,0xEFBEADDE,0xEFBEADDE,itcm);
    block(out,0x4000,0x15824,0xEFBEADDE,0xEFBEADDE);
    const uint8_t zeros[0x28]={};
    block(out,0x10007f98,0x28,0xEFBEADDE,0xEFBEADDE,zeros);
    const uint32_t versions[6]={0xffffffff,core,0xffffffff,0x50000112,0,0x20070001};
    block(out,0x10007fdc,0x18,0xEFBEADDE,0xEFBEADDE,reinterpret_cast<const uint8_t *>(versions));
    return out;
}
static bool readFile(const char *path,std::vector<uint8_t> *out)
{
    FILE *file=std::fopen(path,"rb");
    if(!file)return false;
    uint8_t buffer[65536];size_t n;
    out->clear();
    while((n=std::fread(buffer,1,sizeof buffer,file))>0)out->insert(out->end(),buffer,buffer+n);
    std::fclose(file);
    return true;
}
/* Cold boot of a fresh model to the HFI-ready state. */
static void boot(A830Gmu &gmu)
{
    uint32_t result=0;
    CHECK(gmu.coldBoot(&result)==kIOReturnSuccess && result==0x100 && gmu.booted());
}

/* TCM reads in the log: the B4 read-back, the 4-word ITCM check and B5's 3 DTCM version words. */
static unsigned tcmReads(size_t from=0)
{
    unsigned n=0;
    for(size_t i=from;i<m.log.size();++i)
        if(m.log[i].op=='r' && ((m.log[i].phys>=ITCM && m.log[i].phys<ITCM+0x4000) || (m.log[i].phys>=DTCM && m.log[i].phys<DTCM+0x4000)))++n;
    return n;
}

int main()
{
    unsigned checks=0;
    /* Every group up to the annibale part checks the design profile (ARCHITECTURE 4.7 as written). */
    adreno830_profile_value=A830ProfileDesign;
    const char *path=std::getenv("A830_GMU_FW");
    if(!path)path="/Users/xiaochoumao/unixnu-13t/\xe4\xb8\x80\xe5\x8a\xa0" "13T\xe5\x8e\x9f\xe5\x8e\x82\xe5\xa4\x87\xe4\xbb\xbd/GPU"
                  "\xe5\x9b\xba\xe4\xbb\xb6/gen80000_gmu.bin";
    const bool have_real=readFile(path,&image);
    if(!have_real)image=synthetic(0x50010019);
    CHECK(A830GmuFirmware::parse(image.data(),image.size(),&blocks)==kIOReturnSuccess);
    NativeAdrenoGmuPlan plan{};
    CHECK(A830GmuFirmware::plan(blocks,0,&plan)==kIOReturnSuccess && memory.allocate(plan,36)==kIOReturnSuccess);

    /* bind */
    {
        initial();
        A830Gmu gmu;
        A830GmuConfig config;A830GmuDefaultConfig(&config);
        CHECK(config.chip_id==0x08000100 && config.ahb_fence_range==0x80C808A0 && config.idle_level==0 && config.hfi_irq==336 &&
              config.gmu_irq==337 && config.log_bytes==0x4000);
        uint32_t result=7;
        CHECK(gmu.coldBoot(&result)==kIOReturnNotReady && result==0);
        CHECK(gmu.oobSet(nullptr)==kIOReturnNotReady && gmu.forceOff()==kIOReturnNotReady && gmu.pollFatal()==kIOReturnNotReady);
        A830GmuIo bad_io{&m,nullptr,write32,barrier};
        CHECK(gmu.bind(bad_io,gos,config,&blocks,&memory,&hooks)==kIOReturnBadArgument);
        A830GmuOs bad_os=gos;bad_os.sleep_us=nullptr;
        CHECK(gmu.bind(gio,bad_os,config,&blocks,&memory,&hooks)==kIOReturnBadArgument);
        A830GmuConfig bad=config;bad.log_bytes=0x1800;
        CHECK(gmu.bind(gio,gos,bad,&blocks,&memory,&hooks)==kIOReturnBadArgument);
        CHECK(gmu.bind(gio,gos,config,&blocks,&memory,&hooks)==kIOReturnSuccess);
        CHECK(gmu.bind(gio,gos,config,&blocks,&memory,&hooks)==kIOReturnBusy);
        checks+=9;
    }

    /* B1-B8: the happy path, in order, with residue from before (recorded, cleared, never a failure). */
    {
        initial();
        R(INFO)=0x00800004;R(AO_STATUS)=0x8;R(GENERAL9)=0x77;
        A830Gmu gmu;bindGmu(gmu);
        boot(gmu);
        const A830GmuReport &r=gmu.report();
        CHECK(r.info_before==0x00800004 && r.ao_before==0x8 && R(INFO)==0 && R(AO_STATUS)==0);
        /* B1 then B2 */
        const long b1=indexOf('w',SYSRESET,0,1);
        CHECK(b1>=0 && indexOf('w',CLR,b1,0xffffffff)>b1);
        const long b2=indexOf('w',CLR,b1,0xffffffff);
        CHECK(indexOf('w',AO_CLR,b2,0xffffffff)>b2 && R(MASK)==0xFF7F7FF9 && R(AO_MASK)==0xFFFFFFD6);
        CHECK(indexOf('w',MASK,0)>b2 && indexOf('w',AO_MASK,0)>indexOf('w',AO_CLR,0));
        /* B4: TCMs hold the image's words, the main code is in DDR, the ITCM check matched. */
        CHECK(R(ITCM)==0x10007F80 && R(ITCM+4)==0x105 && R(ITCM+8)==0x145 && R(ITCM+12)==0x159);
        CHECK(R(CORE_VER)==0x50010019 && R(PWR_VER)==0x50000112 && R(HFI_VER)==0x20070001);
        CHECK(std::memcmp(memory.cpu(0x4000,0x15824),&image[blocks.block[12].offset],0x15824)==0);
        CHECK(r.itcm[0]==0x10007F80 && r.core==0x50010019 && r.pwr==0x50000112 && r.hfi==0x20070001);
        CHECK(!(r.warnings&(A830GmuWarnItcm|A830GmuWarnVersion)));
        /* The queues reset while CM3 is in reset, before B6. */
        const long hook=indexOf('H',1);
        CHECK(m.resets==1 && hook>b2);
        /* B6, exactly this sequence of writes (physical addresses from ARCHITECTURE 4.7.3), with no read among them. */
        const struct {uint64_t phys;uint32_t value;} b6[]={
            {RPMH,0xF},{FALNEXT,1},{FAL,1},{INIT,0},{BOOT_CONFIG,2},{QTBL_ADDR,0x60008000},{QTBL_INFO,1},{FENCE_RANGE,0x80C808A0},
            {CM3_CFG,0x4052},{0x3D0F00C,0x00071E20},{0x3D0F010,0x00071E20},{0x3D0F014,0x00071E20},{0x3D0F018,0x00071E20},
            {GBIF_CX_CONFIG,0x20023000},{MRC_QOS,0x33},{GBIF_HALT,1u<<3},{GENERAL9,0},{GENERAL10,0x08000100},{GENERAL8,0x60004003},
            {SYSBUS,1},{ICACHE,1},{DCACHE,1}};
        long at=hook+1;
        for(const auto &entry:b6) {
            CHECK(m.log[at].op=='w' && m.log[at].phys==entry.phys && m.log[at].value==entry.value);
            ++at;
        }
        CHECK(m.log[at].op=='w' && m.log[at].phys==SYSRESET && m.log[at].value==0);   /* B7 follows B6 directly */
        CHECK(!(r.warnings&A830GmuWarnB6Readback) && r.b6_mismatches==0);
        /* B7 waited for 0x100 (3 ms in this model), B8 for the HFI. */
        CHECK(r.init_result==0x100 && r.init_us>=3000 && r.init_us<3200 && R(HFI_INIT)==1 && r.hfi_status==1);
        CHECK(r.boots==1 && r.boot_attempts==1 && m.gx_accesses==0 && !gmu.rsccAsleep());
        CHECK(countOf('w',RSCC_REQ)==0);   /* B3 only after an RSCC sleep */
        /* Design: no RSC status, no GENERAL_11, no TCM read-back beyond the 4 ITCM words, 100 ms for the HFI. */
        CHECK(indexOf('r',RSC_STATUS0)<0 && indexOf('r',GENERAL11)<0 && tcmReads()==4+3 && r.tcm_verified==0 && r.ddr_verified==0);
        CHECK(r.general11==0 && r.rsc_status==0 && A830GmuHfiInitUs()==100000);
        checks+=19;
    }

    /* B7 failures: only the conditions ARCHITECTURE 10 keeps are fatal. */
    {
        struct {Boot boot;uint16_t reason;uint64_t reg;IOReturn result;uint64_t max_us;} cases[]={
            {BootNever,A830ReasonTimeout,INIT,kIOReturnTimeout,101000},
            {BootNmi,A830ReasonGmuNmi,INIT,kIOReturnIOError,1200},
            {BootCm3,A830ReasonGmuFault,INFO,kIOReturnIOError,1200},
            {BootWdog,A830ReasonGmuFault,AO_STATUS,kIOReturnIOError,1200}};
        for(const auto &c:cases) {
            initial();m.boot=c.boot;
            A830Gmu gmu;bindGmu(gmu);
            uint32_t result=0;
            const uint64_t started=m.now;
            CHECK(gmu.coldBoot(&result)==c.result && !gmu.booted());
            const A830GmuFailure &f=gmu.failure();
            CHECK(f.step==7 && f.reason==c.reason && f.reg==c.reg);
            const uint64_t spent=m.now-started;
            CHECK(c.boot==BootNever?spent>=100000 && spent<=c.max_us:spent<=c.max_us);
            if(c.boot==BootNmi)CHECK(f.value==0x200 && result==0x200);
            if(c.boot==BootCm3)CHECK(gmu.fatalSeen());
            ++checks;
        }
        /* Recorded only: GMU2HOST bit23, AO FENCE_ERR and HOST_AHB_BUS_ERR seen by the B7 poll 1 ms before the result. */
        initial();adreno830_status=Adreno830Status{};
        m.noise_info=1u<<23;m.noise_ao=(1u<<3)|(1u<<5);
        A830Gmu gmu;bindGmu(gmu);
        boot(gmu);
        const uint32_t w=gmu.report().warnings;
        CHECK((w&A830GmuWarnCm3FaultLinux) && (w&A830GmuWarnFenceErr) && (w&A830GmuWarnAhbBusErr) && !gmu.fatalSeen());
        CHECK(adreno830_status.sticky[3]==((1u<<3)|(1u<<5)) && gmu.report().info_last==(1u<<23));
        checks+=2;
    }
    /* The legacy criterion (core <= 0x20010004): 0xBABEFACE; and the criterion itself. */
    {
        CHECK(A830GmuInitDone(0x50010019,0x100) && A830GmuInitDone(0x50010019,0x300) && !A830GmuInitDone(0x50010019,0x101));
        CHECK(!A830GmuInitDone(0x20010004,0x100) && A830GmuInitDone(0x20010004,0xBABEFACE) && A830GmuInitDone(0,0xBABEFACE));
        static A830GmuBlocks legacy_blocks;
        const std::vector<uint8_t> legacy=synthetic(0x20010004);
        CHECK(A830GmuFirmware::parse(legacy.data(),legacy.size(),&legacy_blocks)==kIOReturnSuccess);
        initial();m.boot=BootLegacy;
        A830Gmu gmu;
        A830GmuConfig config;A830GmuDefaultConfig(&config);
        CHECK(gmu.bind(gio,gos,config,&legacy_blocks,&memory,&hooks)==kIOReturnSuccess);
        uint32_t result=0;
        CHECK(gmu.coldBoot(&result)==kIOReturnSuccess && result==0xBABEFACE);
        /* A DTCM read-back that differs from the image (here: the version word reads 0) is recorded, and the image's own core
         * version still picks the criterion: the 0x100 result of the PKX110 image is taken. */
        initial();m.cfg_ignore=CORE_VER;
        A830Gmu reread;bindGmu(reread);
        boot(reread);
        CHECK(reread.report().core==0 && (reread.report().warnings&A830GmuWarnVersion));
        checks+=5;
    }
    /* B8 timeout; hook and access failures; missing firmware. */
    {
        initial();m.hfi_ok=false;
        A830Gmu gmu;bindGmu(gmu);
        CHECK(gmu.coldBoot(nullptr)==kIOReturnTimeout && gmu.failure().step==8 && gmu.failure().reason==A830ReasonTimeout &&
              gmu.failure().reg==HFI_STATUS);
        CHECK(gmu.report().hfi_us>=100000 && gmu.report().hfi_us<100200);   /* A830_T_HFI_INIT_US */
        initial();m.hfi_us=150000;   /* ready after 150 ms: too late for the design */
        A830Gmu slow;bindGmu(slow);
        CHECK(slow.coldBoot(nullptr)==kIOReturnTimeout && slow.failure().step==8);
        initial();m.reset_fail=1;
        A830Gmu hook_fails;bindGmu(hook_fails);
        CHECK(hook_fails.coldBoot(nullptr)==kIOReturnNotReady && hook_fails.failure().step==6 &&
              hook_fails.failure().reason==A830ReasonArgument && countOf('w',SYSRESET,0)==0);
        initial();
        A830Gmu io_fails;bindGmu(io_fails);
        m.fail_write=1+4+(0x1ec+0x28+0x18)/4+6;   /* B1, 4 x B2, the TCM words, then the 6th B6 write */
        CHECK(io_fails.coldBoot(nullptr)==kIOReturnIOError && io_fails.failure().step==6 &&
              io_fails.failure().reason==A830ReasonReadback && io_fails.failure().reg==QTBL_ADDR);
        initial();
        A830Gmu no_fw;
        A830GmuConfig config;A830GmuDefaultConfig(&config);
        CHECK(no_fw.bind(gio,gos,config,nullptr,&memory,&hooks)==kIOReturnSuccess);
        CHECK(no_fw.coldBoot(nullptr)==kIOReturnNotReady && no_fw.failure().step==4 && no_fw.failure().reason==A830ReasonFirmware);
        checks+=4;
    }
    /* Diag mode reads B6 back and only records a mismatch (CM3_CFG keeping its NMI bit here). */
    {
        initial();m.cfg_ignore=CM3_CFG;R(CM3_CFG)=0x4252;
        A830Gmu gmu;bindGmu(gmu,1);
        boot(gmu);
        CHECK(gmu.report().b6_mismatches==1 && gmu.report().b6_first==0x1F82D && (gmu.report().warnings&A830GmuWarnB6Readback));
        CHECK(indexOf('r',CM3_CFG)>0);
        checks+=2;
    }

    /* OOB (4.7.8): reference counted, one set and one clear reach the GMU; timeout; perfcounter; unsupported requests. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        uint32_t us=0;
        CHECK(gmu.oobSet(&us)==kIOReturnSuccess && us>=200 && us<400 && gmu.oobReferences()==1);
        CHECK(countOf('w',H2G,1u<<30)==1 && countOf('w',CLR,1u<<31)==1 && !(R(INFO)&(1u<<31)));
        CHECK(gmu.oobSet(&us)==kIOReturnSuccess && gmu.oobReferences()==2 && countOf('w',H2G,1u<<30)==1);
        gmu.oobClear();
        CHECK(gmu.oobReferences()==1 && countOf('w',H2G,1u<<31)==0);
        gmu.oobClear();
        CHECK(gmu.oobReferences()==0 && countOf('w',H2G,1u<<31)==1);
        gmu.oobClear();
        CHECK(countOf('w',H2G,1u<<31)==1);   /* unbalanced clear: nothing */
        CHECK(gmu.oobSet(nullptr,A830_OOB_REQ_PERFCNTR)==kIOReturnSuccess && countOf('w',H2G,1u<<28)==1 &&
              countOf('w',CLR,1u<<30)==1);
        gmu.oobClear(A830_OOB_REQ_PERFCNTR);
        CHECK(countOf('w',H2G,1u<<29)==1);
        CHECK(gmu.oobSet(nullptr,2)==kIOReturnUnsupported && gmu.failure().step==0x20);
        /* A stale acknowledgement is recorded and cleared before the request; the poll waits for the real one. */
        R(INFO)|=1u<<31;
        const uint64_t before=m.now;
        CHECK(gmu.oobSet(&us)==kIOReturnSuccess && m.now-before>=200 && (gmu.report().warnings&A830GmuWarnOobResidue));
        gmu.oobClear();
        m.oob_ok=false;
        const uint64_t start=m.now;
        CHECK(gmu.oobSet(&us)==kIOReturnTimeout && gmu.oobReferences()==0 && m.now-start>=100000);
        CHECK(gmu.failure().step==0x20 && gmu.failure().reason==A830ReasonTimeout && gmu.failure().reg==INFO &&
              gmu.failure().expect==(1u<<31));
        checks+=10;
    }
    /* GX confirmation: within 1 ms; otherwise step 0x21, reason 6, and no GX register touched. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.oobSet(nullptr)==kIOReturnSuccess);
        R(CX_GXSTAT)=0x18;
        CHECK(gmu.waitGxOn()==kIOReturnSuccess && gmu.report().gx_pwrclk==0 && gmu.report().rpmh_state==0);
        CHECK(gmu.report().cx_gx_status==0x18 && m.delays>0 && !(gmu.report().warnings&A830GmuWarnRpmhState));
        initial();m.gx_ok=false;
        A830Gmu off;bindGmu(off);boot(off);
        CHECK(off.oobSet(nullptr)==kIOReturnSuccess);
        const uint64_t start=m.now;
        CHECK(off.waitGxOn()==kIOReturnTimeout && off.failure().step==0x21 && off.failure().reason==A830ReasonUnexpectedState);
        CHECK(off.failure().reg==GXSTAT && off.failure().value==3 && m.now-start>=1000 && m.now-start<1100);
        CHECK(m.gx_accesses==0);
        checks+=5;
    }
    /* The GMU-side registers after the GX confirmation, and the DCVS busy counter. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.writeCounters()==kIOReturnSuccess);
        CHECK(R(CX_BUSY_MASK)==0xFF000000 && R(XOCLK0)==0x13170020 && R(XOCLK1)==0x04 && R(PWRCNT_EN)==1);
        CHECK(R(CGC_MODE)==0x00020000 && R(CGC_DELAY)==0x00010111 && R(CGC_HYST)==0x00005555);
        R(BUSY_H)=5;R(BUSY_L)=0x1234;
        uint64_t ticks=0;
        CHECK(gmu.readBusy(&ticks)==kIOReturnSuccess && ticks==0x500001234ull);
        CHECK(gmu.readBusy(nullptr)==kIOReturnBadArgument);
        m.fail_write=m.writes+3;
        CHECK(gmu.writeCounters()==kIOReturnIOError && gmu.failure().step==0x22);
        checks+=6;
    }

    /* Force-off (4.7.9) in KGSL order after a boot with GX on. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.oobSet(nullptr)==kIOReturnSuccess && gmu.waitGxOn()==kIOReturnSuccess);
        const size_t from=m.log.size();
        CHECK(gmu.forceOff()==kIOReturnSuccess && !gmu.booted() && gmu.oobReferences()==0 && gmu.rsccAsleep());
        const long mask=indexOf('w',MASK,from,0xffffffff),ao_mask=indexOf('w',AO_MASK,from,0xffffffff);
        const long falnext=indexOf('w',FALNEXT,from,1),keepalive=indexOf('w',KEEPALIVE,from,0);
        const long tcs=indexOf('r',TCS0,from),fence_clr=indexOf('w',FENCE_CLR,from,7),fence=indexOf('w',FENCE_CTRL,from,0);
        const long reset=indexOf('w',SYSRESET,from,1),halt=indexOf('w',GBIF_HALT,from,2);
        const long vote_on=indexOf('V',0,from,1),gdsc=indexOf('w',GX_GDSCR,from),vote_off=indexOf('V',0,from,0);
        const long general9=indexOf('r',GENERAL9,from),sleep=indexOf('w',RSCC_REQ,from,1),release=indexOf('w',GBIF_HALT,from,0);
        CHECK(mask>=0 && ao_mask>=0 && falnext>mask && keepalive>falnext && tcs>keepalive && fence_clr>tcs && fence>fence_clr);
        CHECK(reset>fence && halt>reset && vote_on>halt && gdsc>vote_on && vote_off>gdsc && general9>vote_off);
        CHECK(sleep>general9 && release>sleep);
        CHECK(indexOf('r',GX_CFG,from)>gdsc && (R(GX_GDSCR)&1) && R(GXSTAT)==3);
        CHECK(countOf('r',TCS0)>=1 && countOf('r',TCS0+9*TCS_STRIDE)>=1 && countOf('r',TCS0+10*TCS_STRIDE)==0);
        CHECK(gmu.report().log_wptr==0x1234 && gmu.report().force_offs==1 && gmu.report().rscc_sleeps==1);
        CHECK(!(gmu.report().warnings&(A830GmuWarnGbifHalt|A830GmuWarnGxStuck|A830GmuWarnGxVote|A830GmuWarnRsccSleep|
                                       A830GmuWarnGxStillOn|A830GmuWarnTcsBusy)));
        CHECK(m.votes.size()==2 && m.gx_accesses==0);
        /* The next boot wakes the RSCC (B3) before loading, and writes the saved log pointer back (B6). */
        const size_t again=m.log.size();
        boot(gmu);
        const long wake=indexOf('w',RSCC_REQ,again,2),ack=indexOf('r',RSCC_ACK,again),busy=indexOf('r',SEQ_BUSY,again);
        const long done=indexOf('w',RSCC_REQ,again,0),itcm=indexOf('w',ITCM,again);
        CHECK(wake>0 && ack>wake && busy>ack && done>busy && itcm>done && !gmu.rsccAsleep());
        CHECK(indexOf('w',GENERAL9,again,0x1234)>itcm && gmu.report().rscc_wakes==1 && gmu.report().boots==2);
        checks+=12;
    }
    /* Force-off records what does not complete, and always completes. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.oobSet(nullptr)==kIOReturnSuccess && gmu.waitGxOn()==kIOReturnSuccess);
        m.tcs_busy=true;m.gbif_ack=false;m.gx_collapse_ok=false;m.rscc_sleep_ok=false;m.vote_fail=-5;
        CHECK(gmu.forceOff()==kIOReturnSuccess && !gmu.booted() && !gmu.rsccAsleep());
        const uint32_t w=gmu.report().warnings;
        CHECK((w&A830GmuWarnTcsBusy) && (w&A830GmuWarnGbifHalt) && (w&A830GmuWarnGxStuck) && (w&A830GmuWarnGxVote) &&
              (w&A830GmuWarnRsccSleep) && (w&A830GmuWarnGxStillOn));
        CHECK(R(GBIF_HALT)==0 && m.votes.size()==1);   /* a failed vote is not taken back */
        /* No voter at all: the collapse is still attempted, the missing vote recorded. */
        initial();
        A830GmuHooks no_vote=hooks;no_vote.gx_vote=nullptr;
        A830Gmu bare;bindGmu(bare,0,&no_vote);boot(bare);
        CHECK(bare.oobSet(nullptr)==kIOReturnSuccess && bare.waitGxOn()==kIOReturnSuccess);
        CHECK(bare.forceOff()==kIOReturnSuccess && (bare.report().warnings&A830GmuWarnGxVote) && (R(GX_GDSCR)&1));
        /* GX already off: no vote, no collapse. */
        initial();
        A830Gmu cold;bindGmu(cold);boot(cold);
        CHECK(cold.forceOff()==kIOReturnSuccess && m.votes.empty() && countOf('w',GX_GDSCR)==0);
        /* Before any successful boot there is no RSCC sleep, and the next boot does not wake it. */
        initial();m.boot=BootNever;
        A830Gmu never;bindGmu(never);
        CHECK(never.coldBoot(nullptr)==kIOReturnTimeout);
        CHECK(never.forceOff()==kIOReturnSuccess && countOf('w',RSCC_REQ)==0 && !never.rsccAsleep());
        m.boot=BootOk;
        const size_t retry=m.log.size();
        boot(never);
        CHECK(indexOf('w',RSCC_REQ,retry)<0 && never.report().boot_attempts==2 && never.report().boots==1);
        checks+=9;
    }
    /* RSCC failures. */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);
        CHECK(gmu.rsccSleep()==kIOReturnSuccess && countOf('w',RSCC_REQ)==0);   /* never booted: nothing */
        boot(gmu);
        m.rscc_sleep_ok=false;
        CHECK(gmu.rsccSleep()==kIOReturnTimeout && gmu.failure().step==0x30 && gmu.failure().reg==RSC_STATUS0);
        m.rscc_sleep_ok=true;
        CHECK(gmu.rsccSleep()==kIOReturnSuccess && gmu.rsccAsleep() && gmu.rsccSleep()==kIOReturnSuccess);
        m.rscc_wake_ok=false;
        CHECK(gmu.coldBoot(nullptr)==kIOReturnTimeout && gmu.failure().step==3 && gmu.failure().reg==RSCC_ACK);
        m.rscc_wake_ok=true;
        CHECK(gmu.rsccWake()==kIOReturnSuccess && !gmu.rsccAsleep() && gmu.rsccWake()==kIOReturnSuccess);
        checks+=5;
    }

    /* Slumber S2-S9 (4.17 P1), then the wake through coldBoot. */
    {
        struct Ops {bool pending=false;int prepare=0;unsigned calls=0,off=0;} ops_state;
        A830GmuSlumberOps sops{&ops_state,
            [](void *c) {return static_cast<Ops *>(c)->pending;},
            [](void *c) {++static_cast<Ops *>(c)->off;},
            [](void *c) {Ops *o=static_cast<Ops *>(c);++o->calls;m.log.push_back({'S',0,0,m.now});return o->prepare;}};
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.writeCounters()==kIOReturnSuccess);
        const size_t from=m.log.size();
        CHECK(gmu.slumber(sops)==kIOReturnSuccess && !gmu.booted() && gmu.rsccAsleep() && gmu.oobReferences()==0);
        const long set=indexOf('w',H2G,from,1u<<30),clear=indexOf('w',H2G,from,1u<<31),rpmh=indexOf('r',RPMH,from);
        const long tcs=indexOf('r',TCS0,rpmh),pwr=indexOf('w',PWRCNT_EN,from,0),hfi=indexOf('S',0,from);
        const long fence=indexOf('w',FENCE_CTRL,from,0),falnext=indexOf('w',FALNEXT,from,1),busy=indexOf('r',CX_BUSY,from);
        const long sleep=indexOf('w',RSCC_REQ,from,1),client=indexOf('w',GBIF_HALT,from,1),arb=indexOf('w',GBIF_HALT,from,2);
        const long release=indexOf('w',GBIF_HALT,from,0),mask=indexOf('w',MASK,from,0xffffffff);
        CHECK(set>=0 && clear>set && rpmh>clear && tcs>rpmh && pwr>tcs && hfi>pwr && fence>hfi && falnext>fence && busy>falnext);
        CHECK(sleep>busy && client>sleep && arb>client && release>arb && mask>release && ops_state.off==1 && ops_state.calls==1);
        CHECK(R(AO_MASK)==0xffffffff && gmu.report().slumbers==1);
        /* Wake: RSCC wake, GENERAL_9 back, boot. */
        const size_t wake=m.log.size();
        boot(gmu);
        CHECK(indexOf('w',RSCC_REQ,wake,2)>0 && indexOf('w',GENERAL9,wake,0x1234)>0);
        /* A GPU interrupt still pending: nothing changed, busy. */
        ops_state.pending=true;
        const size_t busy_from=m.log.size();
        CHECK(gmu.slumber(sops)==kIOReturnBusy && gmu.booted() && gmu.oobReferences()==0 && indexOf('w',RSCC_REQ,busy_from)<0);
        CHECK(countOf('w',H2G,1u<<31)==2 && gmu.report().slumber_aborts==1);
        ops_state.pending=false;
        /* PREPARE_SLUMBER refused: the fence and FALNEXT writes still happen, step 0x55. */
        ops_state.prepare=15;
        const size_t prep=m.log.size();
        CHECK(gmu.slumber(sops)==kIOReturnIOError && gmu.failure().step==0x55 && gmu.failure().reason==A830ReasonHfiStatus);
        CHECK(indexOf('w',FENCE_CTRL,prep,0)>indexOf('S',0,prep) && indexOf('w',FALNEXT,prep,1)>0);
        ops_state.prepare=0;
        /* GMU still busy: step 0x56 after 100 ms. */
        m.cx_busy=true;
        CHECK(gmu.slumber(sops)==kIOReturnTimeout && gmu.failure().step==0x56 && gmu.failure().reg==CX_BUSY);
        m.cx_busy=false;
        /* IFPC idle level: RPMH_POWER_STATE must reach 3 with GX off. */
        gmu.setIdleLevel(3);
        CHECK(gmu.slumber(sops)==kIOReturnTimeout && gmu.failure().step==0x53 && gmu.failure().expect==3);
        R(RPMH)=3;R(GXSTAT)=3;
        CHECK(gmu.slumber(sops)==kIOReturnSuccess);
        A830Gmu idle;bindGmu(idle);
        CHECK(idle.slumber(sops)==kIOReturnNotReady);
        /* The busy bit is a knob (ARCHITECTURE 9.2 #4): KGSL bit30 by default, the Linux bit23 as the alternative. */
        initial();m.cx_busy23=true;
        A830Gmu kgsl;bindGmu(kgsl);boot(kgsl);
        CHECK(kgsl.slumber(sops)==kIOReturnSuccess);
        initial();m.cx_busy23=true;
        A830Gmu linux_bit;
        A830GmuConfig config;A830GmuDefaultConfig(&config);
        CHECK(config.cx_busy_mask==(1u<<30));
        config.cx_busy_mask=1u<<23;
        CHECK(linux_bit.bind(gio,gos,config,&blocks,&memory,&hooks)==kIOReturnSuccess);boot(linux_bit);
        CHECK(linux_bit.slumber(sops)==kIOReturnTimeout && linux_bit.failure().step==0x56);
        A830Gmu refused;config.cx_busy_mask=1;
        CHECK(refused.bind(gio,gos,config,&blocks,&memory,&hooks)==kIOReturnBadArgument);
        initial();m.cx_busy23=true;
        A830Gmu zero;config.cx_busy_mask=0;   /* 0: the KGSL bit */
        CHECK(zero.bind(gio,gos,config,&blocks,&memory,&hooks)==kIOReturnSuccess);boot(zero);
        CHECK(zero.slumber(sops)==kIOReturnSuccess);
        checks+=16;
    }

    /* IFPC helpers: keepalive, the fence allow poll, the fenced write (KGSL gen8_fenced_write). */
    {
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.keepalive(true)==kIOReturnSuccess && R(KEEPALIVE)==1 && gmu.keepalive(false)==kIOReturnSuccess && R(KEEPALIVE)==0);
        R(FENCE_CTRL)=0;
        CHECK(gmu.waitFenceAllow()==kIOReturnSuccess);
        R(FENCE_CTRL)=1;
        const uint64_t start=m.now;
        CHECK(gmu.waitFenceAllow()==kIOReturnTimeout && m.now-start==100);
        m.fence_drops=3;
        uint32_t retries=9;
        CHECK(gmu.fencedWrite(A830GmuWindowGpu,4*0x807,0x40,&retries)==kIOReturnSuccess && retries==3);
        CHECK(countOf('w',0x3D00000+4*0x807,0x40)==4 && m.gx_accesses==4);
        m.fence_drops=1000;
        CHECK(gmu.fencedWrite(A830GmuWindowGpu,4*0x807,0x41,&retries)==kIOReturnTimeout && retries==200);
        CHECK(countOf('w',0x3D00000+4*0x807,0x41)==200 && gmu.report().fenced_retries==203);
        checks+=7;
    }

    /* CX timer (4.4.2): interrupts off, skew measured over 16 samples, seeded once per boot. */
    {
        initial();
        A830CxMisc cx{&gio,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(cx)==kIOReturnSuccess && cx.seeded && cx.seeds==1);
        /* counter() +3 per read, +5 per register write: (t3 - t2) - (t2 - t1) = (5+5+3) - 3 = 10 per sample. */
        CHECK(cx.skew==10 && m.cx_writes_irq_on==0 && m.irq_offs==2 && m.irq_enabled);
        CHECK(cx.seed==m.counter_reads.back()+10);
        CHECK(R(CX_AO_LO)==uint32_t(cx.seed) && R(CX_AO_HI)==uint32_t(cx.seed>>32));
        CHECK(countOf('w',CX_AO_LO)==17 && countOf('w',CX_AO_HI)==17);
        CHECK(indexOf('w',CX_AO_LO)<indexOf('w',CX_AO_HI));
        CHECK(a830CxTimerSeed(cx)==kIOReturnSuccess && countOf('w',CX_AO_LO)==17 && cx.seeds==1);   /* once per boot */
        a830CxTimerLost(cx);
        CHECK(!cx.seeded && a830CxTimerSeed(cx)==kIOReturnSuccess && countOf('w',CX_AO_LO)==34 && cx.seeds==2);
        A830CxMisc bad{&gio,nullptr,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(bad)==kIOReturnBadArgument && !bad.seeded);
        A830GmuOs no_counter=gos;no_counter.counter=nullptr;
        A830CxMisc bad2{&gio,&no_counter,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(bad2)==kIOReturnBadArgument);
        initial();m.fail_write=5;
        A830CxMisc failing{&gio,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(failing)==kIOReturnIOError && !failing.seeded && m.irq_enabled);
        /* M13 c: CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL = 0x1B12. */
        initial();
        A830CxMisc ahb{&gio,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnSuccess);
        for(uint64_t phys=0x3D9E040;phys<=0x3D9E050;phys+=4)CHECK(countOf('w',phys,0x1B12)==1);
        CHECK(m.writes==5 && m.barriers==1 && a830CxAhbTimeout(ahb,0x2000)==kIOReturnBadArgument);
        m.fail_write=m.writes+2;
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnIOError);
        checks+=15;
    }

    /* 336 top half, bottom half and the storm guard (4.12.1, 4.12.3). */
    {
        initial();irq=decltype(irq){};adreno830_status=Adreno830Status{};publishes=0;
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.attachInterrupts(ops)==kIOReturnSuccess && gmu.interruptsWired()==3 && gmu.interruptsOff()==0);
        CHECK(irq.attached.size()==2 && irq.attached[0]==336 && irq.attached[1]==337 && adreno830_status.irq_off==0);
        CHECK(gmu.attachInterrupts(ops)==kIOReturnBusy);
        /* bit1 SIDEMSGQ, bit2 DBGQ and a pending OOB ack: CLR is 0x00808006 whatever is set; the ack bit stays. */
        R(INFO)=(1u<<1)|(1u<<2)|(1u<<31);
        const size_t from=m.log.size();
        A830Gmu::hfiIrq(&gmu);
        CHECK(indexOf('w',CLR,from,0x00808006)>0 && countOf('w',CLR,0x00808006)==1 && R(INFO)==(1u<<31));
        CHECK(irq.kicks==1 && m.drains==0 && m.locks==0);   /* nothing but the interrupt registers in the top half */
        gmu.gmuWork();
        CHECK(m.drains==1 && m.locks==1 && m.unlocks==1 && m.faults==0 && publishes==0);
        /* bit23 alone: recorded. */
        R(INFO)=1u<<23;
        A830Gmu::hfiIrq(&gmu);gmu.gmuWork();
        CHECK(m.faults==0 && (gmu.report().warnings&A830GmuWarnCm3FaultLinux));
        /* bit15: a global GMU fault with reason 14, under the HFI lock, counted and published. */
        R(INFO)=1u<<15;
        A830Gmu::hfiIrq(&gmu);
        CHECK(gmu.fatalSeen());
        gmu.gmuWork();
        CHECK(m.faults==1 && m.fault_reason==A830ReasonGmuFault && m.fault_info==(1u<<15) && adreno830_status.gmu_faults==1);
        CHECK(publishes==1 && gmu.report().faults==1);
        /* The debug drain found F2H_MSG_ERR: reason 16. */
        m.drain_fail=1;R(INFO)=1u<<2;
        A830Gmu::hfiIrq(&gmu);gmu.gmuWork();
        CHECK(m.faults==2 && m.fault_reason==A830ReasonF2hError);
        m.drain_fail=0;
        /* No progress 64 times in a row (a bit the clear cannot clear): device-side mask, SPI disabled, irq_off bit1. */
        m.sticky_info=1u<<2;R(INFO)=1u<<2;
        for(unsigned i=0;i<63;++i)A830Gmu::hfiIrq(&gmu);
        CHECK(irq.disabled.empty() && R(MASK)==0xFF7F7FF9);
        A830Gmu::hfiIrq(&gmu);
        CHECK(irq.disabled.size()==1 && irq.disabled[0]==336 && R(MASK)==0xffffffff && (adreno830_status.irq_off&2));
        CHECK(gmu.interruptsOff()==1 && (gmu.report().warnings&A830GmuWarnStorm) && gmu.report().storms==1);
        const unsigned published=publishes;
        gmu.gmuWork();
        CHECK(publishes==published+1);
        /* Interrupts entering with nothing set count as no progress as well; progress resets the count. */
        initial();irq=decltype(irq){};
        A830Gmu other;bindGmu(other);boot(other);
        CHECK(other.attachInterrupts(ops)==kIOReturnSuccess);
        for(unsigned i=0;i<63;++i)A830Gmu::hfiIrq(&other);
        R(INFO)=1u<<2;A830Gmu::hfiIrq(&other);
        for(unsigned i=0;i<63;++i)A830Gmu::hfiIrq(&other);
        CHECK(irq.disabled.empty());
        A830Gmu::hfiIrq(&other);
        CHECK(irq.disabled.size()==1);
        /* Re-enable after recovery. */
        boot(other);
        CHECK(other.reenableInterrupts()==kIOReturnSuccess && irq.enabled.size()==1 && irq.enabled[0]==336);
        CHECK(other.interruptsOff()==0 && !(adreno830_status.irq_off&2) && R(MASK)==0xFF7F7FF9);
        checks+=17;
    }
    /* 337: watchdog (masked at once, fatal), FENCE_ERR (status recorded), AHB error (recorded), storm. */
    {
        initial();irq=decltype(irq){};adreno830_status=Adreno830Status{};
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.attachInterrupts(ops)==kIOReturnSuccess);
        R(AO_STATUS)=1u<<3;R(FENCE_STATUS)=0;m.fence_drops=1;
        A830Gmu::aoIrq(&gmu);
        CHECK(R(AO_STATUS)==0 && gmu.report().fence_status==1 && adreno830_status.sticky[3]==(1u<<3) && irq.kicks==1);
        gmu.gmuWork();
        CHECK(m.faults==0 && (gmu.report().warnings&A830GmuWarnFenceErr));
        R(AO_STATUS)=1u<<5;A830Gmu::aoIrq(&gmu);gmu.gmuWork();
        CHECK(m.faults==0 && (gmu.report().warnings&A830GmuWarnAhbBusErr));
        R(AO_STATUS)=1u;
        A830Gmu::aoIrq(&gmu);
        CHECK(R(AO_MASK)==(0xFFFFFFD6u|1u) && gmu.fatalSeen());
        gmu.gmuWork();
        CHECK(m.faults==1 && m.fault_reason==A830ReasonGmuFault && m.fault_ao==1u);
        m.sticky_ao=1u<<4;R(AO_STATUS)=1u<<4;
        for(unsigned i=0;i<64;++i)A830Gmu::aoIrq(&gmu);
        CHECK(irq.disabled.size()==1 && irq.disabled[0]==337 && R(AO_MASK)==0xffffffff && (adreno830_status.irq_off&4));
        checks+=7;
    }
    /* Attach failures are recorded and leave polling; the poll finds the fatal bits. */
    {
        initial();irq=decltype(irq){};irq.readback=0x0003;adreno830_status=Adreno830Status{};
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.attachInterrupts(ops)==kIOReturnSuccess && gmu.interruptsOff()==3 && (adreno830_status.irq_off&6)==6);
        CHECK((gmu.report().warnings&A830GmuWarnIrqReadback) && gmu.report().irq_readback[0]==3);
        R(INFO)=1u<<15;
        CHECK(gmu.pollFatal()==kIOReturnSuccess && m.faults==1 && m.fault_reason==A830ReasonGmuFault && R(INFO)==0);
        R(AO_STATUS)=1u;
        CHECK(gmu.pollFatal()==kIOReturnSuccess && m.faults==2 && R(AO_STATUS)==0);
        initial();irq=decltype(irq){};irq.status=kIOReturnUnsupported;
        A830Gmu refused;bindGmu(refused);boot(refused);
        CHECK(refused.attachInterrupts(ops)==kIOReturnSuccess && refused.interruptsOff()==3 && refused.interruptsWired()==0);
        initial();irq=decltype(irq){};irq.null_work=true;
        A830Gmu no_work;bindGmu(no_work);boot(no_work);
        CHECK(no_work.attachInterrupts(ops)==kIOReturnSuccess && no_work.interruptsOff()==3);
        /* Never attached: the poll covers both; attached and on: the poll reads nothing. */
        initial();
        A830Gmu polled;bindGmu(polled);boot(polled);
        R(INFO)=1u<<2;
        CHECK(polled.pollFatal()==kIOReturnSuccess && m.drains==1 && m.faults==0);
        initial();irq=decltype(irq){};
        A830Gmu wired;bindGmu(wired);boot(wired);
        CHECK(wired.attachInterrupts(ops)==kIOReturnSuccess);
        const unsigned reads=m.reads;
        CHECK(wired.pollFatal()==kIOReturnSuccess && m.reads==reads);
        checks+=9;
    }

    /* The MMIO adapter and the dword mapping of A830HfiIo-style callers. */
    {
        static uint32_t gmu_window[0x68000/4],gpu_window[0x40000/4],cx_window[0x2000/4];
        A830GmuMmio mmio{};
        mmio.base[A830GmuWindowGmu]=reinterpret_cast<volatile uint8_t *>(gmu_window);mmio.bytes[A830GmuWindowGmu]=sizeof gmu_window;
        mmio.base[A830GmuWindowGpu]=reinterpret_cast<volatile uint8_t *>(gpu_window);mmio.bytes[A830GmuWindowGpu]=sizeof gpu_window;
        mmio.base[A830GmuWindowCxMisc]=reinterpret_cast<volatile uint8_t *>(cx_window);mmio.bytes[A830GmuWindowCxMisc]=sizeof cx_window;
        A830GmuIo io{};
        CHECK(A830GmuBindMmio(&mmio,&io)==kIOReturnSuccess && io.cookie==&mmio);
        uint32_t value=0;
        CHECK(!io.write32(io.cookie,A830GmuWindowGmu,0x47100,0) && !io.write32(io.cookie,A830GmuWindowGmu,0x67ffc,0xabc));
        CHECK(!io.read32(io.cookie,A830GmuWindowGmu,0x67ffc,&value) && value==0xabc && gmu_window[0x67ffc/4]==0xabc);
        CHECK(io.write32(io.cookie,A830GmuWindowGmu,0x68000,1) && io.read32(io.cookie,A830GmuWindowGmu,2,&value));
        CHECK(io.read32(io.cookie,A830GmuWindowRscc,0,&value) && io.write32(io.cookie,A830GmuWindowGmuAo,0,1));
        CHECK(io.read32(io.cookie,A830GmuWindows,0,&value));
        io.barrier(io.cookie);
        A830GmuMmio missing{};
        CHECK(A830GmuBindMmio(&missing,&io)==kIOReturnBadArgument && !io.read32);
        mmio.bytes[A830GmuWindowGpu]=0x3fffe;
        CHECK(A830GmuBindMmio(&mmio,&io)==kIOReturnBadArgument);
        mmio.bytes[A830GmuWindowGpu]=sizeof gpu_window;
        CHECK(A830GmuBindMmio(&mmio,&io)==kIOReturnSuccess);
        unsigned window=9;uint32_t offset=0;
        CHECK(A830GmuDwordWindow(0x1F840,&window,&offset) && window==A830GmuWindowGmu && offset==0x47100);
        CHECK(A830GmuDwordWindow(0x27880,&window,&offset) && window==A830GmuWindowCxMisc && offset==0x200);
        CHECK(A830GmuDwordWindow(0x3C45,&window,&offset) && window==A830GmuWindowGpu && offset==0xF114);
        CHECK(A830GmuDwordWindow(0x23B13,&window,&offset) && window==A830GmuWindowGmu && offset==0x57C4C);
        CHECK(!A830GmuDwordWindow(0x28000,&window,&offset) && !A830GmuDwordWindow(0x1F840,nullptr,&offset));
        CHECK(!A830GmuWriteDword(io,0x1F994,0x40000000) && gmu_window[(4*0x1F994-0x37000)/4]==0x40000000);
        CHECK(!A830GmuReadDword(io,0x1F994,&value) && value==0x40000000 && A830GmuReadDword(io,0x30000,&value) && value==0);
        CHECK(!A830GmuWriteDword(io,0x27880,0x55) && cx_window[0x200/4]==0x55);
        checks+=16;
    }

    /* The temporary GX vote through a DRV-2 model (force-off step 6). */
    {
        static std::map<uint32_t,uint32_t> drv2;
        static std::vector<std::pair<uint32_t,uint32_t>> sent;
        drv2.clear();sent.clear();
        drv2[0x00]=3u<<16;drv2[0x0c]=(3u<<12)|(16u<<27);
        Sm8750RpmhIo io{nullptr,
            [](void *,uint32_t offset)->uint32_t {
                const uint32_t tcs=offset>=0xd00?(offset-0xd00)%0x2a0:0;
                if(offset>=0xd00+0x2a0*0 && offset<0xd00+0x2a0*3 && offset>=0xd00+0x28 && tcs==0x28)return 1;   /* STATUS idle */
                if(offset>=0xd00 && tcs>=0x40 && (tcs-0x40)%24==0 && tcs<0x40+24*16)return 0x00010100;      /* CMD_STATUS */
                return drv2.count(offset)?drv2[offset]:0;},
            [](void *,uint32_t offset,uint32_t value) {
                drv2[offset]=value;
                const uint32_t tcs=offset>=0xd00?(offset-0xd00)%0x2a0:0;
                if(offset>=0xd00 && tcs>=0x34 && (tcs-0x34)%24==8 && tcs<0x34+24*16)sent.push_back({drv2[offset-4],value});},
            [](void *,unsigned) {}};
        static Sm8750RpmhResources res;
        res=Sm8750RpmhResources{};
        res.gfx.addr=0x30000;res.gfx.present=1;res.gfx.count=4;res.gfx.vlvl[0]=0;res.gfx.vlvl[1]=0x30;res.gfx.vlvl[2]=0x40;
        res.gfx.vlvl[3]=0x80;
        res.gmxc.addr=0x30010;res.gmxc.present=1;res.gmxc.count=3;res.gmxc.vlvl[0]=0;res.gmxc.vlvl[1]=0x40;res.gmxc.vlvl[2]=0x80;
        A830GmuGxVoter voter{&io,&res,{},0,0,0,0};
        CHECK(A830GmuGxVote(&voter,1)==0 && voter.held==1 && voter.gfx_hlvl==1 && voter.gmxc_hlvl==1);
        CHECK(sent.size()==2 && sent[0].first==0x30010 && sent[0].second==1 && sent[1].first==0x30000 && sent[1].second==1);
        sent.clear();
        CHECK(A830GmuGxVote(&voter,0)==0 && voter.held==0);
        CHECK(sent.size()==2 && sent[0].first==0x30000 && sent[0].second==0 && sent[1].first==0x30010 && sent[1].second==0);
        res.gmxc.present=0;sent.clear();
        CHECK(A830GmuGxVote(&voter,1)==0 && sent.size()==1 && sent[0].first==0x30000);
        res.gfx.present=0;
        CHECK(A830GmuGxVote(&voter,1)==Sm8750RpmhArgument && A830GmuGxVote(nullptr,1)==Sm8750RpmhArgument);
        checks+=6;
    }
    /* ===== annibale profile ===================================================================================== */
    adreno830_profile_value=A830ProfileAnnibale;
    {
        /* Defaults: only the AHB fence range differs (0x80C808C0, kernel-sequences 4.6 step 15). */
        A830GmuConfig config;A830GmuDefaultConfig(&config);
        CHECK(config.ahb_fence_range==0x80C808C0 && config.chip_id==0x08000100 && config.idle_level==0 && config.log_bytes==0x4000);
        CHECK(A830GmuHfiInitUs()==500000);
        /* The cold boot with residue, an RSC asleep at the boot and another request bit pending in RSCC_CONTROL_REQ. */
        initial();adreno830_status=Adreno830Status{};
        R(INFO)=0x00800004;R(AO_STATUS)=0x8;R(RSC_STATUS0)=1u<<16;R(RSCC_REQ)=0x4;
        A830Gmu gmu;bindGmu(gmu);
        boot(gmu);
        const A830GmuReport &r=gmu.report();
        CHECK(r.info_before==0x00800004 && r.ao_before==0x8 && R(INFO)==0 && R(AO_STATUS)==0);
        /* B2: AO mask, GMU2HOST mask, AO clear, GMU2HOST clear, all ones, one after the other. */
        const long b1=indexOf('w',SYSRESET,0,1),am=indexOf('w',AO_MASK,b1,0xffffffff);
        CHECK(b1>=0 && am>b1 && m.log[am+1].op=='w' && m.log[am+1].phys==MASK && m.log[am+1].value==0xffffffff);
        CHECK(m.log[am+2].op=='w' && m.log[am+2].phys==AO_CLR && m.log[am+2].value==0xffffffff);
        CHECK(m.log[am+3].op=='w' && m.log[am+3].phys==CLR && m.log[am+3].value==0xffffffff);
        CHECK(countOf('w',MASK,0xFF7F7FF9)==0 && countOf('w',AO_MASK,0xFFFFFFD6)==0 && R(MASK)==0xffffffff && R(AO_MASK)==0xffffffff);
        /* B3: RSC asleep at the boot: status read, REQ read, REQ|BIT1, ACK, SEQ_BUSY, REQ back to 0x4 (BIT1 was ours),
         * all before the firmware. */
        const long status=indexOf('r',RSC_STATUS0,am),req=indexOf('r',RSCC_REQ,status),wake=indexOf('w',RSCC_REQ,req,6);
        const long ack=indexOf('r',RSCC_ACK,wake),busy=indexOf('r',SEQ_BUSY,ack),restore=indexOf('w',RSCC_REQ,busy,4);
        const long itcm=indexOf('w',ITCM,am);
        CHECK(status>am+3 && req>status && wake>req && ack>wake && busy>ack && restore>busy && itcm>restore);
        CHECK(r.rsc_status==(1u<<16) && r.rscc_wakes==1 && !gmu.rsccAsleep() && R(RSCC_REQ)==4 && countOf('w',RSCC_REQ)==2);
        /* B4: every TCM word read back (plus the 4-word ITCM check), the DDR code compared. */
        const unsigned tcm_words=(0x1ec+0x28+0x18)/4;
        CHECK(r.tcm_verified==tcm_words && r.ddr_verified==0x15824 && tcmReads()==tcm_words+4+3 && !(r.warnings&A830GmuWarnItcm));
        /* GENERAL_11 read after the queue reset and before B6, 0: no warning. */
        const long hook=indexOf('H',1),general11=indexOf('r',GENERAL11),b6=indexOf('w',RPMH,hook,0xF);
        CHECK(hook>itcm && general11>hook && b6>general11 && r.general11==0 && !(r.warnings&A830GmuWarnGeneral11));
        /* B6 with the RB fence range, B7, then B8: GMU2HOST cleared and masked before HFI_CTRL_INIT. */
        CHECK(R(FENCE_RANGE)==0x80C808C0 && countOf('w',FENCE_RANGE,0x80C808A0)==0);
        const long release=indexOf('w',SYSRESET,b6,0),clear=indexOf('w',CLR,release,0xffffffff);
        const long mask=indexOf('w',MASK,clear,0xffffffff),hfi=indexOf('w',HFI_INIT,mask,1);
        CHECK(release>b6 && clear>release && mask==clear+1 && hfi==mask+1 && r.boots==1 && r.init_result==0x100);
        /* Interrupts: nothing attached, both lines polled by choice (no irq_off status bit); the poll finds bit15. */
        irq=decltype(irq){};
        CHECK(gmu.attachInterrupts(ops)==kIOReturnSuccess && irq.attached.empty() && gmu.interruptsOff()==3 &&
              gmu.interruptsWired()==0 && r.polled==3 && adreno830_status.irq_off==0);
        CHECK(gmu.attachInterrupts(ops)==kIOReturnBusy && gmu.reenableInterrupts()==kIOReturnSuccess && irq.enabled.empty());
        R(INFO)=(1u<<15)|(1u<<2);
        CHECK(gmu.pollFatal()==kIOReturnSuccess && m.faults==1 && m.fault_reason==A830ReasonGmuFault && R(INFO)==0 &&
              m.drains==1 && irq.kicks==0);
        R(AO_STATUS)=1u;
        CHECK(gmu.pollFatal()==kIOReturnSuccess && m.faults==2 && R(AO_STATUS)==0);
        checks+=19;
    }
    {
        /* The RSC awake at the boot: no RSCC request touched. A wake that is not acknowledged fails B3. */
        initial();R(RSC_STATUS0)=0;
        A830Gmu awake;bindGmu(awake);boot(awake);
        CHECK(indexOf('r',RSC_STATUS0)>0 && countOf('w',RSCC_REQ)==0 && countOf('r',RSCC_REQ)==0 && awake.report().rscc_wakes==0);
        initial();R(RSC_STATUS0)=1u<<16;m.rscc_wake_ok=false;
        A830Gmu stuck;bindGmu(stuck);
        CHECK(stuck.coldBoot(nullptr)==kIOReturnTimeout && stuck.failure().step==3 && stuck.failure().reg==RSCC_ACK &&
              countOf('w',SYSRESET,0)==0);
        /* After this driver's own RSCC sleep (force-off) the annibale wake writes the same 2-then-0 pair as the design. */
        initial();
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.oobSet(nullptr)==kIOReturnSuccess && gmu.waitGxOn()==kIOReturnSuccess && gmu.forceOff()==kIOReturnSuccess &&
              gmu.rsccAsleep());
        const size_t again=m.log.size();
        boot(gmu);
        const long req=indexOf('r',RSCC_REQ,again),wake=indexOf('w',RSCC_REQ,again,2),done=indexOf('w',RSCC_REQ,wake,0);
        CHECK(req>0 && wake>req && done>wake && countOf('w',RSCC_REQ)==4 && indexOf('w',GENERAL9,again,0x1234)>done);
        checks+=4;
    }
    {
        /* GENERAL_11 not 0 (a VRB address left from HWSCHED): recorded with a warning, the boot goes on. */
        initial();R(GENERAL11)=0x6000A000;
        A830Gmu gmu;bindGmu(gmu);boot(gmu);
        CHECK(gmu.report().general11==0x6000A000 && (gmu.report().warnings&A830GmuWarnGeneral11) && R(GENERAL11)==0x6000A000);
        /* The design never reads it. */
        adreno830_profile_value=A830ProfileDesign;
        initial();R(GENERAL11)=0x6000A000;
        A830Gmu design;bindGmu(design);boot(design);
        CHECK(indexOf('r',GENERAL11)<0 && design.report().general11==0 && !(design.report().warnings&A830GmuWarnGeneral11));
        adreno830_profile_value=A830ProfileAnnibale;
        checks+=2;
    }
    {
        /* A TCM word that does not take the image (the first non-zero image word from ITCM 0x100): B4 fails with reason 7,
         * the TCM's physical address, the word read and the image's word; CM3 stays in reset. */
        uint32_t at=0x100;
        while(*reinterpret_cast<const uint32_t *>(&image[blocks.block[11].offset+at])==0)at+=4;
        CHECK(at<0x1ec);
        const uint32_t expect=*reinterpret_cast<const uint32_t *>(&image[blocks.block[11].offset+at]);
        initial();m.cfg_ignore=ITCM+at;
        A830Gmu gmu;bindGmu(gmu);
        uint32_t result=7;
        CHECK(gmu.coldBoot(&result)==kIOReturnIOError && result==0 && !gmu.booted());
        const A830GmuFailure &f=gmu.failure();
        CHECK(f.step==4 && f.reason==A830ReasonReadback && f.reg==ITCM+at && f.value==0 && f.expect==expect);
        CHECK(countOf('w',SYSRESET,0)==0 && indexOf('w',RPMH)<0 && gmu.report().tcm_verified==at/4);
        /* The design profile writes the same word into the same model and boots (only the first 4 ITCM words are looked at). */
        adreno830_profile_value=A830ProfileDesign;
        initial();m.cfg_ignore=ITCM+at;
        A830Gmu design;bindGmu(design);boot(design);
        CHECK(!(design.report().warnings&A830GmuWarnItcm) && R(ITCM+at)==0);
        adreno830_profile_value=A830ProfileAnnibale;
        checks+=4;
    }
    {
        /* B8: 500 ms. Ready after 450 ms boots; never ready fails after 500 ms; B7 keeps its 100 ms. */
        initial();m.hfi_us=450000;
        A830Gmu slow;bindGmu(slow);boot(slow);
        CHECK(slow.report().hfi_us>=450000 && slow.report().hfi_us<450200);
        initial();m.hfi_ok=false;
        A830Gmu never;bindGmu(never);
        CHECK(never.coldBoot(nullptr)==kIOReturnTimeout && never.failure().step==8 && never.failure().reg==HFI_STATUS);
        CHECK(never.report().hfi_us>=500000 && never.report().hfi_us<500200);
        initial();m.boot=BootNever;
        A830Gmu stuck;bindGmu(stuck);
        const uint64_t started=m.now;
        CHECK(stuck.coldBoot(nullptr)==kIOReturnTimeout && stuck.failure().step==7 && m.now-started>=100000 &&
              m.now-started<=101000);
        checks+=5;
    }
    {
        /* No CX timer seed: nothing written, not seeded, counted; the arguments are still checked. */
        initial();
        A830CxMisc cx{&gio,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(cx)==kIOReturnSuccess && !cx.seeded && cx.seeds==0 && cx.skips==1 && m.writes==0 && m.irq_offs==0);
        CHECK(a830CxTimerSeed(cx)==kIOReturnSuccess && cx.skips==2 && countOf('w',CX_AO_LO)==0);
        A830CxMisc bad{&gio,nullptr,false,0,0,0,0,0,0};
        CHECK(a830CxTimerSeed(bad)==kIOReturnBadArgument && bad.skips==0);
        /* AHB timeouts read, written and read back one by one (K/adreno8_cx.c:647-662). */
        initial();
        A830CxMisc ahb{&gio,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnSuccess && ahb.ahb_reg==0);
        for(uint64_t phys=0x3D9E040;phys<=0x3D9E050;phys+=4)CHECK(countOf('w',phys,0x1B12)==1 && R(phys)==0x1B12 && countOf('r',phys)==2);
        CHECK(m.writes==5 && m.reads==10 && m.barriers==5);
        for(unsigned i=0;i<5;++i)CHECK(m.log[i].op=='r');   /* the five values first */
        CHECK(m.log[5].op=='w' && m.log[5].phys==0x3D9E040 && m.log[6].op=='r' && m.log[6].phys==0x3D9E040);
        /* One already at 0x1B12 is left alone. */
        initial();R(0x3D9E048)=0x1B12;
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnSuccess && countOf('w',0x3D9E048)==0 && m.writes==4);
        /* One that does not keep the value, one that reads all ones, a failed read: kIOReturnIOError naming it. */
        initial();m.cfg_ignore=0x3D9E04C;
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnIOError && ahb.ahb_reg==0x3D9E04C && ahb.ahb_read==0 && m.writes==4);
        initial();R(0x3D9E050)=0xffffffff;
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnIOError && ahb.ahb_reg==0x3D9E050 && ahb.ahb_read==0xffffffff && m.writes==0);
        initial();m.fail_read=7;   /* the read back of the second register */
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnIOError && ahb.ahb_reg==0x3D9E044 && ahb.ahb_read==0xffffffff);
        A830GmuIo write_only{&m,nullptr,write32,barrier};
        A830CxMisc no_read{&write_only,&gos,false,0,0,0,0,0,0};
        CHECK(a830CxAhbTimeout(no_read)==kIOReturnBadArgument && a830CxAhbTimeout(ahb,0x2000)==kIOReturnBadArgument);
        /* The design writes the five without reading. */
        adreno830_profile_value=A830ProfileDesign;
        initial();
        CHECK(a830CxAhbTimeout(ahb)==kIOReturnSuccess && m.reads==0 && m.writes==5 && m.barriers==1);
        adreno830_profile_value=A830ProfileAnnibale;
        checks+=13;
    }
    CHECK(memory.release()==kIOReturnSuccess && fixture.live.empty());
    std::printf("PASS: A830 GMU over a register model with %s gen80000_gmu.bin: B1-B8 and its fatal/recorded conditions, OOB and "
                "GX confirmation, force-off in KGSL order, RSCC/GENERAL_9, slumber S2-S9, IFPC helpers, CX timer seed, 336/337 "
                "top/bottom halves and storm guard, DRV-2 GX vote (design); masked-and-polled GMU interrupts, RSC wake at boot, "
                "GENERAL_11, TCM read-back, fence 0x80C808C0, 500 ms HFI, no CX seed, read-back AHB timeouts (annibale) (%u "
                "groups of checks)\n",have_real?"the PKX110":"a synthetic",checks);
}
