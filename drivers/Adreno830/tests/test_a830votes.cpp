/* SPDX-License-Identifier: MIT */
/* A830Votes against a synthetic SM8750 command DB (gmxc present / dummy / absent, gbx absent, two BCM aux sets, two VCD
 * layouts) and the generated PKX110 level bins: GX, dependency (KGSL and Linux forms) and CX votes, the GMU bandwidth
 * table (x = y and x = 0), the CNOC rows, the ACD table (0x0FF0), the HFI message layouts and the level selection,
 * each checked word for word against an independent transcription of the KGSL formulas
 * (adreno_rpmh_setup_volt_dependency_tbl, setup_dependency_domain_tbl, tcs_cmd_data, gen8_gmu_acd_probe) and, where
 * both define the same thing, against UniXNU kits/AdrenoFirmware/AdrenoHfiVotes.c. All of that in the design profile; the
 * annibale profile of A830Profile.h (kernel-sequences section 14 rows 2, 24, 25) is checked against annibale's own table:
 * three levels 160/222/342 MHz at vlvl 0x32/0x34/0x38 and DDR index 2/2/3, initial level 1, no ACD, x = 0, bus-only bw.
 * host-test: A830Votes.cpp platform/sm8750/Sm8750Rpmh.c platform/sm8750/Sm8750SocInfo.c ../UniXNU/kits/AdrenoFirmware/AdrenoHfiVotes.c */
#include "A830Votes.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#include "Sm8750GpuLevels.h"
#include "../../UniXNU/kits/AdrenoFirmware/AdrenoHfiVotes.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"%s:%d: CHECK failed: %s\n",__FILE__,__LINE__,#x);std::abort();} ++checks;} while(0)

static unsigned checks;

/* ---- synthetic command DB (Linux cmd-db.c layout) ------------------------------------------------------------------ */

struct Image {uint8_t bytes[Sm8750CmdDbBytes];unsigned slaves;uint32_t cursor;};
static Image image;
static Sm8750CmdDb db;

static void put16(uint8_t *p,uint32_t v) {p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
static uint32_t get32(const uint8_t *p) {return p[0]|p[1]<<8|p[2]<<16|uint32_t(p[3])<<24;}

struct Entry {const char *id;uint32_t addr;std::vector<uint8_t> aux;};

static void slave(uint16_t id,const std::vector<Entry> &entries)
{
    uint8_t *h=image.bytes+8+16*image.slaves++;
    const uint32_t table=image.cursor,data=table+24*uint32_t(entries.size());
    uint32_t aux=0;
    put16(h,id);put16(h+2,table);put16(h+4,data);put16(h+6,uint32_t(entries.size()));put16(h+8,0x0100);
    for(size_t n=0;n<entries.size();++n) {
        uint8_t *p=image.bytes+Sm8750CmdDbDataOffset+table+24*n;
        std::memcpy(p,entries[n].id,std::strlen(entries[n].id));
        put32(p+16,entries[n].addr);put16(p+20,uint32_t(entries[n].aux.size()));put16(p+22,aux);
        if(!entries[n].aux.empty())
            std::memcpy(image.bytes+Sm8750CmdDbDataOffset+data+aux,entries[n].aux.data(),entries[n].aux.size());
        aux+=(uint32_t(entries[n].aux.size())+3u)&~3u;
    }
    image.cursor=(data+aux+7u)&~7u;
}

static std::vector<uint8_t> arcAux(const std::vector<uint16_t> &levels)
{
    std::vector<uint8_t> out(2*16,0);
    for(size_t i=0;i<levels.size();++i)put16(&out[2*i],levels[i]);
    return out;
}

static std::vector<uint8_t> bcmAux(uint32_t unit,uint16_t width,uint8_t vcd)
{
    std::vector<uint8_t> out(8,0);
    put32(&out[0],unit);put16(&out[4],width);out[6]=vcd;
    return out;
}

/* 16 corners at most (RPMh): 1100/1150 MHz (0x1c0) vote the 0x1c4 corner, 1200 MHz exactly. */
static const std::vector<uint16_t> GfxLevels={0x0,0x30,0x32,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0,0x100,0x140,0x1a0,0x1c4};
static const std::vector<uint16_t> MxLevels={0x0,0x10,0x30,0x40,0x80,0xc0,0x100,0x140,0x180,0x1a0};
static const std::vector<uint16_t> CxLevels={0x0,0x10,0x30,0x38,0x40,0x50,0x60,0x80,0x90,0xc0,0xe0,0x100,0x120,0x140,0x180,0x1a0};
static const std::vector<uint16_t> GmxcLevels={0x0,0x10,0x30,0x40,0x80,0xc0,0x100,0x140,0x180};
/* A gfx.lvl that tops out below 900 MHz's 0x100: no vote for GMU level 11. */
static const std::vector<uint16_t> ShortGfx={0x0,0x10,0x30,0x32,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};

struct Spec {
    int gmxc;               /* 0 absent, 1 table, 2 dummy (all zero) */
    bool k90;               /* SH0/MC0 aux as logged on REDMI K90 (unit 4800000, width 64/16), else unit 1000000 width 32/4 */
    uint8_t vcd[3];         /* SH0, MC0, ACV */
    bool shortGfx;
    bool noSh0;
};

static uint32_t imageRead(void *,uint32_t offset) {return get32(image.bytes+offset);}
static void imageWrite(void *,uint32_t,uint32_t) {std::abort();}
static void imageDelay(void *,unsigned) {}

static void load(const Spec &s)
{
    std::memset(&image,0,sizeof image);
    put32(image.bytes,0x00030001);put32(image.bytes+4,Sm8750CmdDbMagic);
    std::vector<Entry> bcm={{"ACV",0x50000,bcmAux(1,1,s.vcd[2])},
                            {"MC0",0x50004,bcmAux(s.k90?4800000:1000000,s.k90?16:4,s.vcd[1])},
                            {"CN0",0x5000c,bcmAux(1000,4,2)}};
    if(!s.noSh0)bcm.push_back({"SH0",0x50008,bcmAux(s.k90?4800000:1000000,s.k90?64:32,s.vcd[0])});
    slave(Sm8750CmdDbSlaveBcm,bcm);
    std::vector<Entry> arc={{"cx.lvl",0x30000,arcAux(CxLevels)},{"mx.lvl",0x30010,arcAux(MxLevels)},
                            {"gfx.lvl",0x30020,arcAux(s.shortGfx?ShortGfx:GfxLevels)}};
    if(s.gmxc==1)arc.push_back({"gmxc.lvl",0x30030,arcAux(GmxcLevels)});
    if(s.gmxc==2)arc.push_back({"gmxc.lvl",0x30030,arcAux({})});
    slave(Sm8750CmdDbSlaveArc,arc);
    const Sm8750RpmhIo io={nullptr,imageRead,imageWrite,imageDelay};
    CHECK(Sm8750CmdDbCopy(&io,&db)==Sm8750RpmhOk);
}

/* ---- the PKX110 board inputs (generated from the running DT) ---------------------------------------------------- */

static Adreno830Board board;

static void makeBoard()
{
    board={};
    board.magic=Adreno830BoardMagic;board.version=Adreno830BoardVersion;
    for(unsigned i=0;i<Sm8750DtGpuBinCount;++i) {
        const Sm8750GpuBin &bin=Sm8750DtGpuBins[i];
        board.levels[i]=reinterpret_cast<const Adreno830Level *>(bin.levels);
        board.level_count[i]=bin.count;
        board.initial_level[i]=bin.initial;
        board.bin_speed[i]=bin.flags&Sm8750GpuBinHasSpeed?bin.speed_bin:0;
        board.bin_sku_count[i]=bin.flags&Sm8750GpuBinHasSku?bin.sku_count:0;
        for(unsigned k=0;k<Adreno830BinSkus;++k)board.bin_sku[i][k]=bin.sku[k];
    }
    board.common_level_count=Sm8750DtGpuCommonCount;
    board.ddr_kbps=Sm8750DtDdrKbps;board.ddr_levels=Sm8750DtDdrLevels;
    board.cnoc_levels[0]=Sm8750DtCnoc[0];board.cnoc_levels[1]=Sm8750DtCnoc[1];
    board.gmu_khz[0]=Sm8750DtGmuKhz[0];board.gmu_khz[1]=Sm8750DtGmuKhz[1];
    board.gmu_vlvl[0]=Sm8750DtGmuVlvl[0];board.gmu_vlvl[1]=Sm8750DtGmuVlvl[1];
    Adreno830KnobDefaults(&board.knobs);
}

/* ---- the KGSL formulas, transcribed independently ---------------------------------------------------------------- */

static unsigned usable(const std::vector<uint16_t> &rail)   /* adreno_rpmh_arc_cmds on a 16-entry aux */
{
    std::vector<uint16_t> v(16,0);
    for(size_t i=0;i<rail.size();++i)v[i]=rail[i];
    unsigned num;
    for(num=1;num<16;++num)if(v[num-1] && !v[num])break;
    return num;
}

static bool kgslArcVote(const std::vector<uint16_t> &pri,const std::vector<uint16_t> &sec,uint16_t vlvl,uint32_t *vote)
{
    const unsigned pn=usable(pri),sn=usable(sec);
    unsigned k;uint16_t cur=0;bool found=false;
    for(k=0;k<pn;k++)if(pri[k]>=vlvl) {cur=pri[k];found=true;break;}
    if(!found)return false;
    unsigned j;
    for(j=0;j<sn;j++)if(sec[j]>=cur || j+1==sn)break;
    if(j==sn)j=0;
    *vote=uint32_t(cur)<<16|(j&0xff)<<8|(k&0xff);
    return true;
}

static uint32_t kgslDepVote(const std::vector<uint16_t> &mx,uint16_t vlvl,bool linuxForm)
{
    const unsigned n=usable(mx);
    unsigned j;
    for(j=0;j<n;j++)if(mx[j]>=vlvl || j+1==n)break;
    const uint32_t top=linuxForm?0x0FFFC000u:0x3FFFFu<<14;
    return top|(j&0x3f)<<8|0xff;
}

static uint32_t bcmCmd(bool commit,bool valid,uint64_t x,uint64_t y)
{
    return uint32_t(commit)<<30|uint32_t(valid)<<29|uint32_t(x&0x3fff)<<14|uint32_t(y&0x3fff);
}

struct RefBcm {uint32_t unit,width,buswidth;uint8_t vcd;bool fixed;};

/* tcs_cmd_data(bcms, count, ab, ib, data, perfmode) */
static void kgslTcs(const RefBcm *b,int count,uint32_t ab,uint32_t ib,uint32_t *data,uint32_t perfmode)
{
    for(int i=0;i<count;i++) {
        const bool commit=i==count-1 || b[i].vcd!=b[i+1].vcd;
        if(b[i].fixed) {data[i]=!ab && !ib?bcmCmd(commit,false,0,0):bcmCmd(commit,true,0,perfmode);continue;}
        uint64_t avg=uint64_t(ab)*b[i].width/b[i].buswidth,peak=uint64_t(ib)*b[i].width/b[i].buswidth;
        uint64_t x=avg*1000/b[i].unit,y=peak*1000/b[i].unit;
        if(ab && !x)x=1;
        if(ib && !y)y=1;
        if(x>0x3fff)x=0x3fff;
        if(y>0x3fff)y=0x3fff;
        data[i]=bcmCmd(commit,x||y,x,y);
    }
}

static std::vector<uint16_t> secondaryOf(const Spec &s) {return s.gmxc==1?GmxcLevels:MxLevels;}

/* ---- checks -------------------------------------------------------------------------------------------------------- */

static void checkTables(const A830VoteTables &t,const Spec &s,uint32_t options)
{
    const bool linuxForm=options&A830VotesLinuxDependency;
    /* GX: level 0 off, then the KGSL formula per GMU level. */
    CHECK(t.gx_count==t.levels+1 && t.gx[0].vote==0 && t.gx[0].dep==0xFFFFFFFFu && t.gx[0].khz==0);
    CHECK(t.gx_secondary==(s.gmxc==1?uint32_t(Sm8750RpmhResGmxc):uint32_t(Sm8750RpmhResMx)));
    for(unsigned g=1;g<=t.levels;++g) {
        uint32_t vote=0;
        CHECK(kgslArcVote(GfxLevels,secondaryOf(s),uint16_t(t.level[g-1].vlvl),&vote));
        CHECK(t.gx[g].vote==vote);
        CHECK(t.gx[g].dep==kgslDepVote(MxLevels,uint16_t(t.level[g-1].vlvl),linuxForm));
        CHECK(t.gx[g].khz==t.level[g-1].khz);
    }
    /* CX: off, 500 MHz at 0x40, 650 MHz at 0x80 on cx.lvl over mx.lvl. */
    const uint16_t cxv[3]={0,0x40,0x80};
    const uint32_t cxk[3]={0,500000,650000};
    CHECK(t.cx_count==3);
    for(unsigned i=0;i<3;++i) {
        uint32_t vote=0;
        CHECK(kgslArcVote(CxLevels,MxLevels,cxv[i],&vote) && t.cx[i].vote==vote && t.cx[i].khz==cxk[i]);
    }
    /* BW: x = y (Linux a8xx / KGSL with AB) or x = 0 (KGSL without AB), ACV BIT(2) from DDR level 7, valid 0 at level 0. */
    const RefBcm bcms[3]={{s.k90?4800000u:1000000u,s.k90?64u:32u,16,s.vcd[0],false},
                          {s.k90?4800000u:1000000u,s.k90?16u:4u,4,s.vcd[1],false},{1,1,1,s.vcd[2],true}};
    uint32_t wait=0;
    for(int i=0;i<3;++i)if(i==2 || bcms[i].vcd!=bcms[i+1].vcd)wait|=1u<<i;
    CHECK(t.ddr_levels==12 && t.bw_wait_mask==wait);
    const uint32_t *b=t.bw;
    CHECK(b[0]==0x0000A003u && b[A830BwLevelNum]==12 && b[A830BwCnocCmds]==1 && b[A830BwDdrCmds]==3);
    CHECK(b[A830BwCnocWait]==1 && b[A830BwDdrWait]==wait);
    CHECK(b[A830BwCnocAddrs]==0x5000c && b[A830BwCnocData]==0x40000000u &&
          b[A830BwCnocData+A830_HFI_BW_MAX_CNOC_CMDS]==0x60000001u);
    CHECK(b[A830BwDdrAddrs]==0x50008 && b[A830BwDdrAddrs+1]==0x50004 && b[A830BwDdrAddrs+2]==0x50000);
    for(unsigned i=0;i<16;++i) {
        uint32_t row[3]={};
        if(i<12) {
            const uint32_t kbps=Sm8750DtDdrKbps[i];
            kgslTcs(bcms,3,options&A830VotesBwNoAb?0:kbps,kbps,row,i>=7?4u:0u);
        }
        for(unsigned j=0;j<8;++j)CHECK(b[A830BwDdrData+i*8+j]==(j<3?row[j]:0u));
    }
    /* ACD: GMU levels with an acd value, ascending. */
    uint32_t mask=0;unsigned n=0;
    for(unsigned g=1;g<=t.levels;++g)
        if(t.level[g-1].acd) {mask|=1u<<g;CHECK(t.acd[A830AcdData+n]==t.level[g-1].acd);++n;}
    CHECK(t.acd_mask==mask && t.acd_levels==n && t.acd[0]==0x00002507u && t.acd[A830AcdVersion]==1 &&
          t.acd[A830AcdEnableByLevel]==mask && t.acd[A830AcdStride]==1 && t.acd[A830AcdNumLevels]==n);
    for(unsigned i=A830AcdData+n;i<A830_HFI_ACD_TABLE_DWORDS;++i)CHECK(t.acd[i]==0);
    /* Perf message: {hdr, 0, GPU_PERF, count, 3, {vote, dep, kHz}..., 3, 2, {vote, kHz}...}. */
    const uint32_t dwords=3+2+3*t.gx_count+2+2*3;
    CHECK(t.perf_dwords==dwords && t.perf[0]==A830_HFI_HDR(15,0,dwords,0));
    CHECK(t.perf[1]==0 && t.perf[2]==1 && t.perf[3]==t.gx_count && t.perf[4]==3);
    for(unsigned g=0;g<t.gx_count;++g)
        CHECK(t.perf[5+3*g]==t.gx[g].vote && t.perf[6+3*g]==t.gx[g].dep && t.perf[7+3*g]==t.gx[g].khz);
    const unsigned cxAt=5+3*t.gx_count;
    CHECK(t.perf[cxAt]==3 && t.perf[cxAt+1]==2);
    for(unsigned i=0;i<3;++i)CHECK(t.perf[cxAt+2+2*i]==t.cx[i].vote && t.perf[cxAt+3+2*i]==t.cx[i].khz);
}

/* The 11-level common table of ARCHITECTURE 4.1.4. */
static void checkCommonTable(const A830VoteTables &t)
{
    static const uint32_t mhz[11]={160,222,342,389,443,525,607,660,734,832,900};
    static const uint32_t vlvl[11]={0x32,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0,0x100};
    static const uint8_t bus[11][3]={{2,2,2},{2,2,2},{2,3,3},{2,4,6},{2,4,6},{2,4,6},{4,6,8},{4,6,8},{6,8,10},{7,10,10},{7,10,10}};
    static const uint32_t acd[11]={0,0,0,0xC8285FFD,0xC02E5FFD,0xC02E5FFD,0xA82E5FFD,0x882C5FFD,0xA82A5FFD,0x882A5FFD,0x882A5FFD};
    CHECK(t.levels==11 && t.initial==2 && t.fused==0);
    for(unsigned i=0;i<11;++i) {
        CHECK(t.level[i].khz==mhz[i]*1000 && t.level[i].vlvl==vlvl[i] && t.level[i].acd==acd[i]);
        CHECK(t.level[i].bus_min==bus[i][0] && t.level[i].bus==bus[i][1] && t.level[i].bus_max==bus[i][2]);
    }
    CHECK(t.acd_mask==0x0FF0 && t.acd_levels==8 && t.perf_dwords==49 && t.gx_count==12);
}

static void testDefaultBuild()
{
    const Spec specs[]={{1,true,{1,0,0},false,false},{0,true,{0,0,0},false,false},{2,false,{1,1,0},false,false},
                        {1,false,{0,0,1},false,false}};
    for(const Spec &s:specs) {
        load(s);
        static A830VoteTables t;
        CHECK(A830Votes::build(db,board,&t)==kIOReturnSuccess);
        CHECK(t.error_step==A830VotesStepNone && t.options==0 && t.bin>=-1);
        checkCommonTable(t);
        checkTables(t,s,0);
        /* The design-named form and the S0 forms agree. */
        static A830VoteTables u;
        const A830VoteInputs inputs={A830VotesNoFuse,0,0,0};
        CHECK(A830Votes::build(db,board,inputs,&u)==kIOReturnSuccess && !std::memcmp(&t,&u,sizeof t));
        Sm8750RpmhResources res;
        CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
        Sm8750GpuLevelTable levels;
        CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,A830VotesNoFuse,0,0,&levels)==Sm8750SocInfoOk);
        CHECK(A830Votes::build(res,board,levels,0,&u)==kIOReturnSuccess && !std::memcmp(&t,&u,sizeof t));
        /* S0 diagnostic words. */
        uint64_t aux[6]={};
        CHECK(A830Votes::statusAux(res,aux)==(A830S0Arc|A830S0Bcm));
        CHECK(aux[1]==(usable(GfxLevels)|uint64_t(usable(MxLevels))<<8|uint64_t(usable(CxLevels))<<16|
                       uint64_t(s.gmxc==1?usable(GmxcLevels):0xff)<<24));
        CHECK(aux[2]==((s.k90?4800000u:1000000u)|uint64_t(s.k90?64:32)<<32));
        CHECK(aux[3]==((s.k90?4800000u:1000000u)|uint64_t(s.k90?16:4)<<32));
        CHECK(aux[4]==(s.vcd[2]|uint64_t(2)<<8));
    }
}

static void testOptions()
{
    const Spec s={1,true,{1,0,0},false,false};
    load(s);
    static A830VoteTables t;
    /* pagani-gpu-hfi bit0: the Linux/author dependency form, through the board knob of the design form. */
    board.knobs.hfi=1;
    CHECK(A830Votes::build(db,board,&t)==kIOReturnSuccess && t.options==A830VotesLinuxDependency);
    checkTables(t,s,A830VotesLinuxDependency);
    for(unsigned g=1;g<=t.levels;++g)CHECK((t.gx[g].dep>>28)==0 && (t.gx[g].dep&0x0fffc000u)==0x0fffc000u);
    board.knobs.hfi=0;
    /* KGSL without AB: x = 0 on SH0/MC0, y unchanged. */
    const A830VoteInputs noAb={A830VotesNoFuse,0,0,A830VotesBwNoAb};
    CHECK(A830Votes::build(db,board,noAb,&t)==kIOReturnSuccess);
    checkTables(t,s,A830VotesBwNoAb);
    for(unsigned i=1;i<12;++i)for(unsigned j=0;j<2;++j)CHECK(((t.bw[A830BwDdrData+i*8+j]>>14)&0x3fff)==0);
}

static void testLevelSelection()
{
    const Spec s={1,true,{1,0,0},false,false};
    load(s);
    static A830VoteTables t;
    struct Case {uint32_t fuse,soc,fused;int bin;uint8_t levels;uint32_t topMhz;};
    const Case cases[]={
        {A830VotesNoFuse,0,1,5,11,900},    /* no fuse: never fused, whatever the knob (the fallback SKU group still matches) */
        {0xBE,0,1,0,11,900},               /* bin 0: nothing above 900 MHz */
        {0xDD,0,1,1,13,1050},              /* bin 1: 967, 1050 (the repeated 1050 once) */
        {0xE8,0,1,2,14,1100},
        {0xF2,0,1,4,14,1150},
        {0x123,1,1,3,14,1100},             /* SKU group {1, 2} */
        {0x123,3,1,5,15,1200},             /* fallback SKU group {3, 0} */
        {0xDD,0,0,1,11,900},               /* matched but pagani-gpu-speedbin=0: recorded only */
    };
    for(const Case &c:cases) {
        const A830VoteInputs inputs={c.fuse,c.soc,c.fused,0};
        CHECK(A830Votes::build(db,board,inputs,&t)==kIOReturnSuccess);
        CHECK(t.bin==c.bin && t.levels==c.levels && t.level[t.levels-1].khz==c.topMhz*1000 && t.initial==2);
        CHECK(t.fused==(c.fused && c.fuse!=A830VotesNoFuse && c.bin>=0));
        for(unsigned i=1;i<t.levels;++i)CHECK(t.level[i].khz>t.level[i-1].khz);
        checkTables(t,s,0);
        CHECK(t.perf_dwords==3+2+3*(t.levels+1)+2+6);
    }
    /* Selection on its own, and the common-count cross-check. */
    const A830VoteInputs common={A830VotesNoFuse,0,0,0};
    CHECK(A830Votes::selectLevels(board,common,&t)==kIOReturnSuccess && t.levels==11);
    board.common_level_count=10;
    CHECK(A830Votes::selectLevels(board,common,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepCommon &&
          t.error_value==11);
    board.common_level_count=11;
    /* Bins: an empty board, and a bin list that ends early. */
    Sm8750GpuBin bins[Adreno830BinCount];
    CHECK(A830Votes::boardBins(board,bins)==6 && (bins[3].flags&Sm8750GpuBinHasSku) && !(bins[3].flags&Sm8750GpuBinHasSpeed));
    CHECK((bins[0].flags&Sm8750GpuBinHasSpeed) && bins[0].speed_bin==0xBE && bins[5].sku_count==2);
    const Adreno830Level *saved=board.levels[2];
    board.levels[2]=nullptr;
    CHECK(A830Votes::boardBins(board,bins)==2);
    board.levels[2]=saved;
    Adreno830Board empty=board;
    for(auto &level:empty.levels)level=nullptr;
    CHECK(A830Votes::selectLevels(empty,common,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepBins);
}

static void testFailures()
{
    static A830VoteTables t;
    /* gfx.lvl without a corner for 0x100: GMU level 11 cannot be voted. */
    Spec s={1,true,{1,0,0},true,false};
    load(s);
    CHECK(A830Votes::build(db,board,&t)==kIOReturnUnsupported && t.error_step==A830VotesStepGx && t.error_value==11 &&
          t.error_resource==0x100);
    /* No SH0 in cmd-db. */
    s={1,true,{1,0,0},false,true};
    load(s);
    CHECK(A830Votes::build(db,board,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepCmdDb &&
          t.error_resource==Sm8750RpmhResSh0);
    /* Bad board tables. */
    s={1,true,{1,0,0},false,false};
    load(s);
    Adreno830Board bad=board;
    static const uint32_t unsorted[12]={0,781250,2136718,5285156,5285156,6671875,8171875,10687500,12449218,14398437,
                                        16500000,18597656};
    bad.ddr_kbps=unsorted;
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepDdr && t.error_value==4);
    bad=board;bad.ddr_levels=17;
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepDdr);
    bad=board;bad.ddr_levels=8;   /* 900 MHz wants bus 10 */
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepDdr);
    bad=board;bad.gmu_khz[1]=400000;   /* GMU clocks not ascending */
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepCx);
    bad=board;bad.gmu_vlvl[1]=0x400;   /* above every cx.lvl corner */
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnUnsupported && t.error_step==A830VotesStepCx && t.error_value==2);
    bad=board;bad.cnoc_levels[0]=100;
    CHECK(A830Votes::build(db,bad,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepCnoc);
    /* A level table straight from the caller must be ascending and in range. */
    Sm8750RpmhResources res;
    CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
    Sm8750GpuLevelTable levels={};
    CHECK(A830Votes::build(res,board,levels,0,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepLevels);
    CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,A830VotesNoFuse,0,0,&levels)==Sm8750SocInfoOk);
    levels.levels[3].khz=levels.levels[2].khz;
    CHECK(A830Votes::build(res,board,levels,0,&t)==kIOReturnBadArgument && t.error_step==A830VotesStepLevels);
    CHECK(A830Votes::selectLevels(board,{A830VotesNoFuse,0,0,0},nullptr)==kIOReturnBadArgument);
}

static void testHelpers()
{
    const Spec s={1,true,{1,0,0},false,false};
    load(s);
    static A830VoteTables t;
    /* Knobs to inputs: pagani-gpu-speedbin and pagani-gpu-hfi bit0 (bit1 belongs to A830Hfi). */
    A830VoteInputs in=A830Votes::inputsFor(board,0xDD,7);
    CHECK(in.speed_bin==0xDD && in.soc_code==7 && in.fused==0 && in.options==0);
    board.knobs.speedbin=1;board.knobs.hfi=3;
    in=A830Votes::inputsFor(board,A830VotesNoFuse,0);
    CHECK(in.fused==1 && in.options==A830VotesLinuxDependency && in.speed_bin==A830VotesNoFuse);
    board.knobs.speedbin=0;board.knobs.hfi=0;
    CHECK(A830Votes::build(db,board,&t)==kIOReturnSuccess);
    CHECK(A830Votes::ddrKbps(t,11)==18597656 && A830Votes::ddrKbps(t,2)==2136718 && A830Votes::ddrKbps(t,12)==0);
    CHECK(A830Votes::levelAtOrAbove(t,160)==1 && A830Votes::levelAtOrAbove(t,734)==9 && A830Votes::levelAtOrAbove(t,735)==10);
    CHECK(A830Votes::levelAtOrAbove(t,2000)==0 && A830Votes::levelAtOrBelow(t,607)==7 && A830Votes::levelAtOrBelow(t,100)==0);
    CHECK(A830Votes::levelAtOrBelow(t,5000)==11);
    /* 4.16: no AB -> 0xFFFF0000 | bus; AB -> quantized << 16 | 1 << 8 | bus (KGSL adreno_gmu_bus_ab_quantize). */
    const uint32_t top=18597656;
    CHECK(A830Votes::bwWord(6,false,1000000,top)==0xFFFF0006u && A830Votes::bwWord(0xFF,false,0,top)==0xFFFF00FFu);
    CHECK(A830Votes::abQuantize(0,top)==0 && A830Votes::abQuantize(1,top)==1 && A830Votes::abQuantize(top*4u,top)==0xFFFE);
    CHECK(A830Votes::abQuantize(top,top)==0x4000);   /* a quarter of the four channels */
    CHECK(A830Votes::abQuantize(5000000,top)==uint32_t(5000000ull*65536/(uint64_t(top)*4)));
    CHECK(A830Votes::bwWord(4,true,top,top)==(0x4000u<<16|1u<<8|4u) && A830Votes::bwWord(4,true,0,top)==0x00000104u);
    CHECK(A830Votes::abQuantize(5,0)==0xFFFE);
    /* The pieces on bad rails. */
    Sm8750RpmhArc none={};
    uint32_t vote=0;
    Sm8750RpmhResources res;
    CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
    CHECK(A830Votes::arcVote(none,res.mx,0x40,&vote)==kIOReturnBadArgument);
    CHECK(A830Votes::arcVote(res.gfx,res.mx,0x40,nullptr)==kIOReturnBadArgument);
    CHECK(A830Votes::arcVote(res.gfx,res.mx,0x1c5,&vote)==kIOReturnBadArgument);
    CHECK(A830Votes::arcVote(res.gfx,res.mx,0x1a0,&vote)==kIOReturnSuccess && vote==(0x1a0u<<16|9u<<8|14u));
    CHECK(A830Votes::arcVote(res.gfx,res.mx,0x1c0,&vote)==kIOReturnSuccess && vote==(0x1c4u<<16|9u<<8|15u));
    CHECK(A830Votes::dependencyVote(res.mx,0x1000,false)==(0xFFFFC000u|9u<<8|0xffu));   /* above every mx: the last one */
    CHECK(&A830Votes::gxSecondary(res)==&res.gmxc);
    uint32_t row[3],wait;
    CHECK(A830Votes::bwRow(res,0,0,row,&wait)==kIOReturnSuccess && row[0]==0x40000000u && row[1]==0 && row[2]==0x40000000u &&
          wait==5);   /* SH0 (VCD 1) commits before MC0/ACV (VCD 0); valid 0 at 0 kBps */
    CHECK(A830Votes::bwRow(res,0,0,nullptr,&wait)==kIOReturnBadArgument);
}

/* The UniXNU kit (AdrenoFirmware/AdrenoHfiVotes.c) on the inputs where both define the same encoding: GX targets that are
 * exact corners of gfx.lvl (the kit searches the secondary rail for the target, KGSL for the corner), and the modern HFI
 * bandwidth layout with SH0/MC0 x = y and the ACV perfmode bit from 10687500 kBps. */
static void testAgainstKit()
{
    const Spec specs[]={{1,true,{1,0,0},false,false},{0,false,{0,0,0},false,false},{2,true,{0,1,1},false,false}};
    for(const Spec &s:specs) {
        load(s);
        static A830VoteTables t;
        CHECK(A830Votes::build(db,board,{0xF2,0,1,0},&t)==kIOReturnSuccess && t.levels==14);
        std::vector<uint8_t> pri=arcAux(GfxLevels),sec=arcAux(secondaryOf(s));
        uint32_t levels[16]={0},votes[16]={0};
        for(unsigned g=1;g<=t.levels;++g)levels[g]=t.level[g-1].vlvl;
        /* 1150 MHz is 0x1c0, not a corner of this gfx.lvl: compare up to 1050 MHz (0x1a0). */
        const unsigned exact=13;
        CHECK(t.level[exact-1].vlvl==0x1a0);
        CHECK(AdrenoHfiArcVotes(levels,exact+1,pri.data(),pri.size(),sec.data(),sec.size(),votes,16)==AdrenoHfiStartupSuccess);
        for(unsigned g=0;g<=exact;++g)CHECK(votes[g]==t.gx[g].vote);
        /* CX votes on cx.lvl over mx.lvl (every target is a corner). */
        const uint32_t cxv[3]={0,0x40,0x80};
        std::vector<uint8_t> cx=arcAux(CxLevels),mx=arcAux(MxLevels);
        CHECK(AdrenoHfiArcVotes(cxv,3,cx.data(),cx.size(),mx.data(),mx.size(),votes,16)==AdrenoHfiStartupSuccess);
        for(unsigned i=0;i<3;++i)CHECK(votes[i]==t.cx[i].vote);
        /* Bandwidth. */
        AdrenoHfiBcm bcms[3]={};
        bcms[0].address=0x50008;bcms[0].buswidth=16;
        bcms[1].address=0x50004;bcms[1].buswidth=4;
        bcms[2].address=0x50000;bcms[2].fixed=1;bcms[2].perfmode=4;bcms[2].perfmode_kbps=10687500;
        CHECK(AdrenoHfiBcmAux(&bcms[0],bcmAux(s.k90?4800000:1000000,s.k90?64:32,s.vcd[0]).data(),8)==AdrenoHfiStartupSuccess);
        CHECK(AdrenoHfiBcmAux(&bcms[1],bcmAux(s.k90?4800000:1000000,s.k90?16:4,s.vcd[1]).data(),8)==AdrenoHfiStartupSuccess);
        CHECK(AdrenoHfiBcmAux(&bcms[2],bcmAux(1,1,s.vcd[2]).data(),8)==AdrenoHfiStartupSuccess);
        static AdrenoHfiStartupConfig config;
        std::memset(&config,0,sizeof config);
        CHECK(AdrenoHfiBuildBandwidth(Sm8750DtDdrKbps,12,bcms,3,0x5000c,&config)==AdrenoHfiStartupSuccess);
        CHECK(config.bandwidth_count==t.bw[A830BwLevelNum] && config.cnoc_count==t.bw[A830BwCnocCmds] &&
              config.ddr_count==t.bw[A830BwDdrCmds] && config.cnoc_wait_mask==t.bw[A830BwCnocWait] &&
              config.ddr_wait_mask==t.bw[A830BwDdrWait]);
        for(unsigned i=0;i<6;++i) {
            CHECK(config.cnoc_addresses[i]==t.bw[A830BwCnocAddrs+i]);
            CHECK(config.cnoc_data[0][i]==t.bw[A830BwCnocData+i] && config.cnoc_data[1][i]==t.bw[A830BwCnocData+6+i]);
        }
        for(unsigned j=0;j<8;++j)CHECK(config.ddr_addresses[j]==t.bw[A830BwDdrAddrs+j]);
        for(unsigned i=0;i<16;++i)for(unsigned j=0;j<8;++j)CHECK(config.ddr_data[i][j]==t.bw[A830BwDdrData+i*8+j]);
    }
}

/* The annibale profile: annibale's three points out of the board's table (K/adreno8_cx.c:117-121), the floor entry, the
 * 24-dword perf payload (kernel-sequences 5.4), x = 0 rows, and the bus-only bw word. */
static void testAnnibale()
{
    adreno830_profile_value=A830ProfileAnnibale;
    const uint32_t khz[3]={160000,222000,342000},vlvl[3]={0x32,0x34,0x38};
    const uint8_t bus[3]={2,2,3};
    const Spec specs[]={{1,true,{1,0,0},false,false},{0,true,{0,0,0},false,false},{2,false,{1,1,0},false,false}};
    for(const Spec &s:specs) {
        load(s);
        static A830VoteTables t;
        CHECK(A830Votes::build(db,board,&t)==kIOReturnSuccess && t.error_step==A830VotesStepNone);
        CHECK(t.levels==3 && t.initial==1 && t.fused==0 && t.gx_count==4 && t.cx_count==3 && t.options==A830VotesBwNoAb);
        for(unsigned i=0;i<3;++i) {
            CHECK(t.level[i].khz==khz[i] && t.level[i].vlvl==vlvl[i] && t.level[i].acd==0);
            CHECK(t.level[i].bus_min==bus[i] && t.level[i].bus==bus[i] && t.level[i].bus_max==bus[i]);
        }
        for(unsigned i=3;i<Adreno830LevelMax;++i)CHECK(t.level[i].khz==0 && t.level[i].vlvl==0);
        /* The KGSL formulas on these levels, x = 0 rows, the empty ACD table. */
        checkTables(t,s,A830VotesBwNoAb);
        CHECK(t.acd_levels==0 && t.acd_mask==0 && t.acd[A830AcdNumLevels]==0);
        /* Floor entry {0, 0xFFFFFFFF, 0}; perf message: 25 dwords with the header, 24 of payload. */
        CHECK(t.gx[0].vote==0 && t.gx[0].dep==0xFFFFFFFFu && t.gx[0].khz==0);
        CHECK(t.perf_dwords==25 && t.perf[0]==0x0000190Fu && t.perf[1]==0 && t.perf[2]==1 && t.perf[3]==4 && t.perf[4]==3);
        CHECK(t.perf[5]==0 && t.perf[6]==0xFFFFFFFFu && t.perf[7]==0 && t.perf[10]==160000 && t.perf[13]==222000 &&
              t.perf[16]==342000 && t.perf[17]==3 && t.perf[18]==2 && t.perf[20]==0 && t.perf[22]==500000 && t.perf[24]==650000);
        /* The bounds the DCVS reads from it. */
        CHECK(A830Votes::levelAtOrAbove(t,160)==1 && A830Votes::levelAtOrBelow(t,734)==3 && A830Votes::levelAtOrAbove(t,343)==0);
        CHECK(A830Votes::ddrKbps(t,2)==2136718 && A830Votes::ddrKbps(t,3)==5285156);
        /* The same from every build form, also when the fuse would append faster levels (only the matched bin, which is
         * recorded, differs). */
        static A830VoteTables u;
        CHECK(A830Votes::build(db,board,{0xF2,0,1,0},&u)==kIOReturnSuccess && u.bin==4 && u.levels==3 && u.fused==0);
        u.bin=t.bin;
        CHECK(!std::memcmp(&t,&u,sizeof t));
        Sm8750RpmhResources res;
        CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
        Sm8750GpuLevelTable levels;
        CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,0xF2,0,1,&levels)==Sm8750SocInfoOk && levels.count==14);
        CHECK(A830Votes::build(res,board,levels,0,&u)==kIOReturnSuccess && u.bin==4);
        u.bin=t.bin;
        CHECK(!std::memcmp(&t,&u,sizeof t));
        /* pagani-gpu-hfi bit0 (the Linux dependency form) is still the board's choice. */
        CHECK(A830Votes::build(res,board,levels,A830VotesLinuxDependency,&u)==kIOReturnSuccess &&
              u.options==(A830VotesLinuxDependency|A830VotesBwNoAb));
        checkTables(u,s,A830VotesLinuxDependency|A830VotesBwNoAb);
        /* The selection itself stays the board's (the narrowing belongs to build). */
        CHECK(A830Votes::selectLevels(board,{A830VotesNoFuse,0,0,0},&u)==kIOReturnSuccess && u.levels==11 && u.initial==2);
    }
    /* A board table without one of the three points, or with another vlvl or DDR index there: refused, which level. */
    {
        load(specs[0]);
        Sm8750RpmhResources res;
        CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
        static A830VoteTables t;
        Sm8750GpuLevelTable levels,changed;
        CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,A830VotesNoFuse,0,0,&levels)==Sm8750SocInfoOk);
        changed=levels;changed.levels[1].khz=223000;
        CHECK(A830Votes::build(res,board,changed,0,&t)==kIOReturnUnsupported && t.error_step==A830VotesStepLevels &&
              t.error_value==2 && t.error_resource==222000);
        changed=levels;changed.levels[2].vlvl=0x39;
        CHECK(A830Votes::build(res,board,changed,0,&t)==kIOReturnUnsupported && t.error_value==3);
        changed=levels;changed.levels[0].bus=3;
        CHECK(A830Votes::build(res,board,changed,0,&t)==kIOReturnUnsupported && t.error_value==1 && t.error_resource==160000);
        /* Without 160 MHz altogether (a table that starts at 222 MHz). */
        changed={};
        for(unsigned i=1;i<levels.count;++i)changed.levels[i-1]=levels.levels[i];
        changed.count=uint8_t(levels.count-1);changed.initial=0;changed.bin=-1;
        CHECK(A830Votes::build(res,board,changed,0,&t)==kIOReturnUnsupported && t.error_value==1);
        /* The design builds that table. */
        adreno830_profile_value=A830ProfileDesign;
        CHECK(A830Votes::build(res,board,changed,0,&t)==kIOReturnSuccess && t.levels==10);
        adreno830_profile_value=A830ProfileAnnibale;
    }
    /* bw word: the bus alone without AB (annibale's {1, level, bus}); with AB the same formula as the design. */
    const uint32_t top=18597656;
    CHECK(A830Votes::bwWord(2,false,0,top)==2u && A830Votes::bwWord(3,false,1000000,top)==3u && A830Votes::bwWord(0xFF,false,0,top)==0xFFu);
    CHECK(A830Votes::bwWord(4,true,top,top)==(0x4000u<<16|1u<<8|4u));
    adreno830_profile_value=A830ProfileDesign;
    CHECK(A830Votes::bwWord(2,false,0,top)==0xFFFF0002u);
}

/* pagani-gpu-maxlevel (A830VotesFullTable) in the annibale profile: the design's whole table (levels with their DT bus
 * mapping and ACD values, the KGSL GX/dependency/CX votes, the ACD table, the 49-dword perf message), the DDR rows with
 * annibale's x = 0, and initial level 1 (the 160 MHz first vote); word for word the design build with x = 0. */
static unsigned fullTableCases;
static void testAnnibaleFullTable()
{
    const Spec specs[]={{1,true,{1,0,0},false,false},{0,true,{0,0,0},false,false},{2,false,{1,1,0},false,false}};
    for(const Spec &s:specs) {
        load(s);
        Sm8750RpmhResources res;
        CHECK(Sm8750RpmhResourcesParse(&db,&res)==Sm8750RpmhOk);
        Sm8750GpuLevelTable levels;
        CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,A830VotesNoFuse,0,0,&levels)==Sm8750SocInfoOk && levels.count==11);
        static A830VoteTables design,full;
        adreno830_profile_value=A830ProfileDesign;
        CHECK(A830Votes::build(res,board,levels,A830VotesBwNoAb,&design)==kIOReturnSuccess);
        adreno830_profile_value=A830ProfileAnnibale;
        CHECK(A830Votes::build(res,board,levels,A830VotesFullTable,&full)==kIOReturnSuccess && full.error_step==A830VotesStepNone);
        CHECK(full.options==(A830VotesFullTable|A830VotesBwNoAb) && full.levels==11 && full.initial==1 && full.fused==0 &&
              full.gx_count==12 && full.cx_count==3 && full.perf_dwords==49 && full.acd_mask==0x0FF0 && full.acd_levels==8);
        checkTables(full,s,A830VotesBwNoAb);
        CHECK(!std::memcmp(full.level,design.level,sizeof full.level) && !std::memcmp(full.gx,design.gx,sizeof full.gx) &&
              !std::memcmp(full.cx,design.cx,sizeof full.cx) && full.perf_dwords==design.perf_dwords &&
              !std::memcmp(full.perf,design.perf,sizeof full.perf) && !std::memcmp(full.bw,design.bw,sizeof full.bw) &&
              !std::memcmp(full.acd,design.acd,sizeof full.acd) && design.initial==2);
        for(unsigned i=1;i<12;++i)for(unsigned j=0;j<2;++j)CHECK(((full.bw[A830BwDdrData+i*8+j]>>14)&0x3fff)==0);
        /* The bounds the DCVS and S0 read from it: 734 MHz is level 9, 900 MHz the top, the bus of each level the DT's. */
        CHECK(A830Votes::levelAtOrAbove(full,160)==1 && A830Votes::levelAtOrBelow(full,734)==9 &&
              A830Votes::levelAtOrBelow(full,900)==11 && A830Votes::levelAtOrAbove(full,901)==0);
        CHECK(full.level[8].bus_min==6 && full.level[8].bus==8 && full.level[8].bus_max==10 && full.level[2].bus==3);
        /* Through the board's knob: the design-named form and inputsFor take it from pagani-gpu-maxlevel. */
        Adreno830Board knobbed=board;knobbed.knobs.maxlevel=9;
        static A830VoteTables viaBoard;
        CHECK(A830Votes::build(db,knobbed,&viaBoard)==kIOReturnSuccess && viaBoard.levels==11 && viaBoard.initial==1 &&
              viaBoard.options==(A830VotesFullTable|A830VotesBwNoAb) && !std::memcmp(viaBoard.perf,full.perf,sizeof full.perf));
        CHECK(A830Votes::inputsFor(knobbed,A830VotesNoFuse,0).options==A830VotesFullTable &&
              A830Votes::inputsFor(board,A830VotesNoFuse,0).options==0);
        /* With the fuse (pagani-gpu-speedbin=1) the matched bin's faster levels come along: bin 4, 14 levels to 1150 MHz. */
        Sm8750GpuLevelTable fused;
        CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,0xF2,0,1,&fused)==Sm8750SocInfoOk && fused.count==14);
        CHECK(A830Votes::build(res,board,fused,A830VotesFullTable,&full)==kIOReturnSuccess && full.levels==14 &&
              full.initial==1 && full.gx_count==15 && full.level[13].khz==1150000 && full.bin==4);
        checkTables(full,s,A830VotesBwNoAb);
        /* Without the option the narrowing stays: annibale's three points. */
        CHECK(A830Votes::build(res,board,levels,0,&full)==kIOReturnSuccess && full.levels==3 && full.initial==1);
        ++fullTableCases;
    }
    adreno830_profile_value=A830ProfileDesign;
}

int main()
{
    /* Everything but testAnnibale and testAnnibaleFullTable checks the design profile. */
    adreno830_profile_value=A830ProfileDesign;
    makeBoard();
    testDefaultBuild();
    testOptions();
    testLevelSelection();
    testFailures();
    testHelpers();
    testAgainstKit();
    testAnnibale();
    testAnnibaleFullTable();
    std::printf("PASS: A830Votes GX/dependency/CX votes, BW table (x=y and x=0), ACD 0x0FF0, level selection and HFI tables "
                "match the KGSL formulas and the AdrenoFirmware kit (design); annibale's 160/222/342 MHz table, initial level "
                "1, x = 0 rows, 24-dword perf payload and bus-only bw word (annibale); pagani-gpu-maxlevel: the design's whole "
                "table in annibale (%u command DBs, initial 1, x = 0, fused bins) (%u checks)\n",fullTableCases,checks);
    return 0;
}
