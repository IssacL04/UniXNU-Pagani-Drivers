/* SPDX-License-Identifier: MIT */
#include "A830Votes.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#include <string.h>

namespace {
bool arcUsable(const Sm8750RpmhArc &arc) {return arc.present && arc.count && arc.count<=Sm8750RpmhArcMaxLevels;}

IOReturn refuse(A830VoteTables *out,uint32_t step,uint32_t value,uint32_t resource,IOReturn result)
{
    out->error_step=step;out->error_value=value;out->error_resource=resource;
    return result;
}

/* annibale's GPU levels (kernel-sequences section 14 row 2 and 15.1 item 1; K/adreno8_cx.c:117-121 adreno8_boot_speeds):
 * the points all six K90 DT bins share, none with a qcom,acd-level, as {kHz, vlvl, DDR index}. GMU level g is entry g - 1. */
struct AnnibaleLevel {uint32_t khz,vlvl;uint8_t bus;};
constexpr AnnibaleLevel AnnibaleLevels[A830VotesAnnibaleLevels]={{160000,0x32,2},{222000,0x34,2},{342000,0x38,3}};

/* The annibale profile's table: each of its three points must be in the board's selected table with the same kHz, vlvl
 * and DDR index (they are on PKX110: ARCHITECTURE 4.1.4); kept with one DDR index and no ACD value (annibale votes
 * {1, level, bus} and never sends ACD, K/adreno8_hfi.c:553,658). */
IOReturn annibaleLevels(A830VoteTables *out)
{
    Adreno830Level picked[A830VotesAnnibaleLevels]={};
    for(unsigned i=0;i<A830VotesAnnibaleLevels;++i) {
        const AnnibaleLevel &want=AnnibaleLevels[i];
        unsigned k=0;
        while(k<out->levels && out->level[k].khz!=want.khz)++k;
        if(k==out->levels || out->level[k].vlvl!=want.vlvl || out->level[k].bus!=want.bus)
            return refuse(out,A830VotesStepLevels,i+1,want.khz,kIOReturnUnsupported);
        picked[i]=out->level[k];
        picked[i].bus_min=picked[i].bus_max=want.bus;
        picked[i].acd=0;
    }
    memset(out->level,0,sizeof out->level);
    for(unsigned i=0;i<A830VotesAnnibaleLevels;++i)out->level[i]=picked[i];
    out->levels=A830VotesAnnibaleLevels;
    out->initial=A830VotesAnnibaleInitial;   /* 160 MHz: the first vote {1, 1, 2} (K/adreno8_hfi.c:553) */
    out->fused=0;
    return kIOReturnSuccess;
}

/* The GX (index = GMU level) and CX vote tables, ARCHITECTURE 4.7.7. */
IOReturn voteTables(const Sm8750RpmhResources &res,const Adreno830Board &board,A830VoteTables *out)
{
    const Sm8750RpmhArc &secondary=A830Votes::gxSecondary(res);
    out->gx_secondary=&secondary==&res.gmxc?Sm8750RpmhResGmxc:Sm8750RpmhResMx;
    const bool linuxForm=out->options&A830VotesLinuxDependency;
    /* GMU level 0 is "off": no rail vote, no dependency, 0 Hz (KGSL setup_gx_arc_votes). */
    out->gx[0]={0,A830_DEP_VOTE_OFF,0};
    for(unsigned g=1;g<=out->levels;++g) {
        const Adreno830Level &level=out->level[g-1];
        uint32_t vote=0;
        if(A830Votes::arcVote(res.gfx,secondary,level.vlvl,&vote)!=kIOReturnSuccess)
            return refuse(out,A830VotesStepGx,g,level.vlvl,kIOReturnUnsupported);
        out->gx[g]={vote,A830Votes::dependencyVote(res.mx,level.vlvl,linuxForm),level.khz};
    }
    out->gx_count=uint8_t(out->levels+1);
    /* The GMU's own clock: off, then the two qcom,gmu-freq-table entries (KGSL setup_cx_arc_votes: cx.lvl over mx.lvl). */
    const uint32_t vlvl[A830VotesCxLevels]={0,board.gmu_vlvl[0],board.gmu_vlvl[1]};
    const uint32_t khz[A830VotesCxLevels]={0,board.gmu_khz[0],board.gmu_khz[1]};
    for(unsigned i=0;i<A830VotesCxLevels;++i) {
        if(i && (!vlvl[i] || !khz[i] || khz[i]<=khz[i-1]))return refuse(out,A830VotesStepCx,i,vlvl[i],kIOReturnBadArgument);
        uint32_t vote=0;
        if(A830Votes::arcVote(res.cx,res.mx,vlvl[i],&vote)!=kIOReturnSuccess)
            return refuse(out,A830VotesStepCx,i,vlvl[i],kIOReturnUnsupported);
        out->cx[i]={vote,khz[i]};
    }
    out->cx_count=A830VotesCxLevels;
    return kIOReturnSuccess;
}

/* H2F_MSG_TABLE / HFI_TABLE_GPU_PERF: {hdr, version 0, type}, then the GX entry {count, stride 3, {vote, dep, kHz}...} and
 * the CX entry {count, stride 2, {vote, kHz}...} (KGSL gen8_hfi_send_gpu_perf_table, HFI >= 2.6.1). */
void perfMessage(A830VoteTables *out)
{
    uint32_t *p=out->perf;
    unsigned n=0;
    p[n++]=0;
    p[n++]=0;
    p[n++]=A830_HFI_TABLE_GPU_PERF;
    p[n++]=out->gx_count;
    p[n++]=sizeof(A830GxVote)/4;
    for(unsigned g=0;g<out->gx_count;++g) {p[n++]=out->gx[g].vote;p[n++]=out->gx[g].dep;p[n++]=out->gx[g].khz;}
    p[n++]=out->cx_count;
    p[n++]=sizeof(A830CxVote)/4;
    for(unsigned i=0;i<out->cx_count;++i) {p[n++]=out->cx[i].vote;p[n++]=out->cx[i].khz;}
    p[0]=A830_HFI_HDR(A830_HFI_H2F_TABLE,A830_HFI_MSG_CMD,n,0);
    out->perf_dwords=uint8_t(n);
}

/* H2F_MSG_BW_VOTE_TBL (KGSL hfi_bwtable_cmd, adreno_rpmh_build_bw_table_cmd): the DDR rows {SH0, MC0, ACV} from the GMU
 * table encoder, the CNOC rows {CN0 0x40000000, CN0 0x60000001} (4.3.2: valid 0 only at CNOC level 0, only here). */
IOReturn bwMessage(const Sm8750RpmhResources &res,const Adreno830Board &board,A830VoteTables *out)
{
    if(!board.ddr_kbps || board.ddr_levels<2 || board.ddr_levels>A830VotesMaxDdr)
        return refuse(out,A830VotesStepDdr,board.ddr_levels,0,kIOReturnBadArgument);
    if(!res.cn0.present)return refuse(out,A830VotesStepCnoc,0,Sm8750RpmhResCn0,kIOReturnBadArgument);
    uint32_t *b=out->bw;
    memset(b,0,sizeof out->bw);
    for(unsigned i=0;i<board.ddr_levels;++i) {
        const uint32_t kbps=board.ddr_kbps[i];
        /* qcom,bus-table-ddr: level 0 is "no bandwidth", the rest strictly ascending. */
        if((i==0)!=(kbps==0) || (i>1 && kbps<=board.ddr_kbps[i-1]))
            return refuse(out,A830VotesStepDdr,i,kbps,kIOReturnBadArgument);
        uint32_t row[3]={},wait=0;
        if(A830Votes::bwRow(res,kbps,out->options,row,&wait)!=kIOReturnSuccess)
            return refuse(out,A830VotesStepDdr,i,kbps,kIOReturnBadArgument);
        for(unsigned j=0;j<3;++j)b[A830BwDdrData+i*A830_HFI_BW_MAX_DDR_CMDS+j]=row[j];
        out->ddr_kbps[i]=kbps;
        out->bw_wait_mask=wait;
    }
    out->ddr_levels=board.ddr_levels;
    b[A830BwLevelNum]=board.ddr_levels;
    b[A830BwCnocCmds]=1;
    b[A830BwDdrCmds]=3;
    b[A830BwCnocWait]=1;
    b[A830BwDdrWait]=out->bw_wait_mask;
    b[A830BwCnocAddrs]=res.cn0.addr;
    if(board.cnoc_levels[0])return refuse(out,A830VotesStepCnoc,0,board.cnoc_levels[0],kIOReturnBadArgument);
    for(unsigned c=0;c<A830_HFI_BW_MAX_CNOC_LEVELS;++c)
        b[A830BwCnocData+c*A830_HFI_BW_MAX_CNOC_CMDS]=board.cnoc_levels[c]?uint32_t(Sm8750RpmhCn0On):uint32_t(Sm8750RpmhCn0Off);
    b[A830BwDdrAddrs]=res.sh0.addr;
    b[A830BwDdrAddrs+1]=res.mc0.addr;
    b[A830BwDdrAddrs+2]=res.acv.addr;
    b[0]=A830_HFI_HDR(A830_HFI_H2F_BW_VOTE_TBL,A830_HFI_MSG_CMD,A830_HFI_BW_TABLE_DWORDS,0);
    return kIOReturnSuccess;
}

/* H2F_MSG_ACD_TBL (KGSL gen8_gmu_acd_probe): version 1, stride 1, bit g for every GMU level g with an acd value. */
IOReturn acdMessage(A830VoteTables *out)
{
    uint32_t *a=out->acd;
    memset(a,0,sizeof out->acd);
    unsigned n=0;
    uint32_t mask=0;
    for(unsigned g=1;g<=out->levels;++g) {
        const uint32_t value=out->level[g-1].acd;
        if(!value)continue;
        if(n>=A830VotesMaxAcd)return refuse(out,A830VotesStepAcd,g,value,kIOReturnBadArgument);
        mask|=1u<<g;
        a[A830AcdData+n++]=value;
    }
    a[0]=A830_HFI_HDR(A830_HFI_H2F_ACD_TBL,A830_HFI_MSG_CMD,A830_HFI_ACD_TABLE_DWORDS,0);
    a[A830AcdVersion]=A830AcdTableVersion;
    a[A830AcdEnableByLevel]=mask;
    a[A830AcdStride]=A830AcdTableStride;
    a[A830AcdNumLevels]=n;
    out->acd_mask=mask;
    out->acd_levels=uint8_t(n);
    return kIOReturnSuccess;
}

void copyLevels(const Sm8750GpuLevelTable &table,A830VoteTables *out)
{
    out->levels=table.count;
    out->initial=uint8_t(table.initial+1);
    out->bin=table.bin;
    out->fused=table.fused;
    for(unsigned i=0;i<table.count && i<Adreno830LevelMax;++i) {
        const Sm8750GpuLevel &from=table.levels[i];
        Adreno830Level &to=out->level[i];
        to.khz=from.khz;to.vlvl=from.vlvl;to.bus_min=from.bus_min;to.bus=from.bus;to.bus_max=from.bus_max;to.reserved=0;
        to.acd=from.acd;
    }
}

uint32_t optionsOf(const Adreno830Board &board)
{
    return ((board.knobs.hfi&1u)?uint32_t(A830VotesLinuxDependency):0u)|(board.knobs.maxlevel?uint32_t(A830VotesFullTable):0u);
}
}

A830VoteInputs A830Votes::inputsFor(const Adreno830Board &board,uint32_t speedBin,uint32_t socCode)
{
    return {speedBin,socCode,board.knobs.speedbin?1u:0u,optionsOf(board)};
}

/* ---- the pieces ---------------------------------------------------------------------------------------------------- */

const Sm8750RpmhArc &A830Votes::gxSecondary(const Sm8750RpmhResources &res)
{
    /* KGSL build_dcvs_table: a gmxc.lvl in cmd-db that is not a dummy carries the GX secondary vote, else mx.lvl. */
    return arcUsable(res.gmxc)?res.gmxc:res.mx;
}

IOReturn A830Votes::arcVote(const Sm8750RpmhArc &primary,const Sm8750RpmhArc &secondary,uint32_t vlvl,uint32_t *vote)
{
    if(!vote || !arcUsable(primary) || !arcUsable(secondary))return kIOReturnBadArgument;
    unsigned k=0;
    while(k<primary.count && primary.vlvl[k]<vlvl)++k;
    if(k==primary.count)return kIOReturnBadArgument;   /* no primary corner reaches the level */
    const uint32_t cur=primary.vlvl[k];
    unsigned j=0;
    while(j+1<secondary.count && secondary.vlvl[j]<cur)++j;   /* first secondary >= the corner, else the last */
    *vote=A830_ARC_VOTE(k,j,cur);
    return kIOReturnSuccess;
}

uint32_t A830Votes::dependencyVote(const Sm8750RpmhArc &mx,uint32_t vlvl,bool linuxForm)
{
    unsigned j=0;
    if(arcUsable(mx))
        while(j+1<mx.count && mx.vlvl[j]<vlvl)++j;   /* KGSL setup_dependency_domain_tbl: first mx >= vlvl, else the last */
    return linuxForm?A830_DEP_VOTE_LINUX(A830_DEP_CX_NONE,j):A830_DEP_VOTE(A830_DEP_CX_NONE,j);
}

IOReturn A830Votes::bwRow(const Sm8750RpmhResources &res,uint32_t kbps,uint32_t options,uint32_t data[3],uint32_t *wait)
{
    if(!data || !wait)return kIOReturnBadArgument;
    Sm8750RpmhRequest request;
    if(Sm8750RpmhGmuBwRequest(&res,kbps,&request)!=Sm8750RpmhOk || request.count!=3)return kIOReturnBadArgument;
    for(unsigned j=0;j<3;++j) {
        uint32_t word=request.cmd[j].data;
        /* KGSL without the GMU AB vote leaves x = 0 on the non-fixed BCMs (tcs_cmd_data with ab 0). */
        if((options&A830VotesBwNoAb) && j<2)word&=~(uint32_t(A830_BCM_VOTE_MAX)<<14);
        data[j]=word;
    }
    *wait=request.wait_mask;
    return kIOReturnSuccess;
}

uint32_t A830Votes::abQuantize(uint32_t abKbps,uint32_t topKbps)
{
    if(!abKbps)return 0;
    const uint64_t maxKbps=uint64_t(topKbps)*A830_DDR_CHANNELS;
    if(!maxKbps || abKbps>=maxKbps)return 0xFFFE;
    const uint64_t vote=uint64_t(abKbps)*65536/maxKbps;
    return vote?uint32_t(vote):1;
}

uint32_t A830Votes::bwWord(uint8_t bus,bool abSupported,uint32_t abKbps,uint32_t topKbps)
{
    /* No AB: KGSL's 0xFFFF0000 | bus, or annibale's bus alone (K/adreno8_hfi.c:553,658). */
    if(!abSupported)return (adreno830_annibale()?0u:uint32_t(A830_HFI_BW_NO_AB))|bus;
    return abQuantize(abKbps,topKbps)<<16|1u<<8|bus;
}

uint32_t A830Votes::ddrKbps(const A830VoteTables &tables,unsigned level)
{
    return level<tables.ddr_levels && level<A830VotesMaxDdr?tables.ddr_kbps[level]:0;
}

uint8_t A830Votes::levelAtOrAbove(const A830VoteTables &tables,uint32_t mhz)
{
    for(unsigned g=1;g<=tables.levels;++g)
        if(tables.level[g-1].khz>=uint64_t(mhz)*1000)return uint8_t(g);
    return 0;
}

uint8_t A830Votes::levelAtOrBelow(const A830VoteTables &tables,uint32_t mhz)
{
    uint8_t found=0;
    for(unsigned g=1;g<=tables.levels;++g)
        if(tables.level[g-1].khz<=uint64_t(mhz)*1000)found=uint8_t(g);
    return found;
}

uint64_t A830Votes::statusAux(const Sm8750RpmhResources &res,uint64_t aux[6])
{
    if(!aux)return 0;
    auto count=[](const Sm8750RpmhArc &arc)->uint64_t {return arc.present?arc.count:0xff;};
    aux[1]=count(res.gfx)|count(res.mx)<<8|count(res.cx)<<16|count(res.gmxc)<<24;
    aux[2]=res.sh0.unit|uint64_t(res.sh0.width)<<32;
    aux[3]=res.mc0.unit|uint64_t(res.mc0.width)<<32;
    aux[4]=res.acv.vcd|uint64_t(res.cn0.vcd)<<8;
    uint64_t valid=0;
    if(res.gfx.present && res.mx.present && res.cx.present)valid|=A830S0Arc;
    if(res.sh0.present && res.mc0.present && res.acv.present && res.cn0.present)valid|=A830S0Bcm;
    return valid;
}

/* ---- level selection (M1) -------------------------------------------------------------------------------------------- */

unsigned A830Votes::boardBins(const Adreno830Board &board,Sm8750GpuBin bins[Adreno830BinCount])
{
    unsigned n=0;
    for(unsigned i=0;i<Adreno830BinCount;++i) {
        if(!board.levels[i] || !board.level_count[i])break;
        Sm8750GpuBin &bin=bins[n++];
        bin={};
        const uint32_t skus=board.bin_sku_count[i];
        /* A DT group with qcom,sku-codes matches by SKU (any 0 or the SoC code); one without by qcom,speed-bin; a group with
         * both needs both (KGSL adreno_of_get_pwrlevels). */
        bin.flags=(skus?uint32_t(Sm8750GpuBinHasSku):0u)|((!skus || board.bin_speed[i])?uint32_t(Sm8750GpuBinHasSpeed):0u);
        bin.speed_bin=board.bin_speed[i];
        bin.sku_count=uint8_t(skus>0xff?0xff:skus);   /* more than 4 is refused by the selection */
        for(unsigned k=0;k<Adreno830BinSkus && k<skus;++k)bin.sku[k]=board.bin_sku[i][k];
        bin.initial=board.initial_level[i];
        bin.count=board.level_count[i];
        /* Adreno830Level and Sm8750GpuLevel are the same 16-byte layout (both headers assert it): the generated tables are
         * handed over as they are. */
        bin.levels=reinterpret_cast<const Sm8750GpuLevel *>(board.levels[i]);
    }
    return n;
}

IOReturn A830Votes::selectLevels(const Adreno830Board &board,const A830VoteInputs &inputs,A830VoteTables *out)
{
    if(!out)return kIOReturnBadArgument;
    Sm8750GpuBin bins[Adreno830BinCount];
    const unsigned count=boardBins(board,bins);
    if(!count)return refuse(out,A830VotesStepBins,0xff,0,kIOReturnBadArgument);
    Sm8750GpuLevelTable table;
    Sm8750SocInfoStatus status=Sm8750GpuLevelsCommon(bins,count,&table);
    if(status!=Sm8750SocInfoOk)return refuse(out,A830VotesStepLevels,status,count,kIOReturnBadArgument);
    /* The common prefix must be what the board was generated with (11 on PKX110): a disagreement means the generator and
     * the driver read the bins differently. */
    if(board.common_level_count && table.count!=board.common_level_count)
        return refuse(out,A830VotesStepCommon,table.count,board.common_level_count,kIOReturnBadArgument);
    /* Faster levels only with a fuse to match them against (4.1.3): without it the fallback SKU group would match. */
    const bool fused=inputs.fused && inputs.speed_bin!=A830VotesNoFuse;
    status=Sm8750GpuLevelsSelect(bins,count,inputs.speed_bin,inputs.soc_code,fused,&table);
    if(status!=Sm8750SocInfoOk)return refuse(out,A830VotesStepLevels,status,count,kIOReturnBadArgument);
    if(!table.count || table.count>Adreno830LevelMax || table.initial>=table.count)
        return refuse(out,A830VotesStepLevels,table.count,table.initial,kIOReturnBadArgument);
    copyLevels(table,out);
    return kIOReturnSuccess;
}

/* ---- build ------------------------------------------------------------------------------------------------------------ */

IOReturn A830Votes::build(const Sm8750RpmhResources &res,const Adreno830Board &board,const Sm8750GpuLevelTable &levels,
                          uint32_t options,A830VoteTables *out)
{
    if(!out)return kIOReturnBadArgument;
    memset(out,0,sizeof *out);
    const bool annibale=adreno830_annibale();
    /* annibale's bandwidth rows have x (AB) = 0 on SH0/MC0 (kernel-sequences 5.4, section 14 row 25). */
    out->options=options|(annibale?uint32_t(A830VotesBwNoAb):0u);
    out->bin=-1;
    if(!arcUsable(res.gfx) || !arcUsable(res.mx) || !arcUsable(res.cx))
        return refuse(out,A830VotesStepCmdDb,Sm8750RpmhArgument,
                      !arcUsable(res.gfx)?Sm8750RpmhResGfx:!arcUsable(res.mx)?Sm8750RpmhResMx:Sm8750RpmhResCx,
                      kIOReturnBadArgument);
    if(!res.sh0.present || !res.mc0.present || !res.acv.present || !res.cn0.present)
        return refuse(out,A830VotesStepCmdDb,Sm8750RpmhArgument,
                      !res.sh0.present?Sm8750RpmhResSh0:!res.mc0.present?Sm8750RpmhResMc0:
                      !res.acv.present?Sm8750RpmhResAcv:Sm8750RpmhResCn0,kIOReturnBadArgument);
    if(!levels.count || levels.count>Adreno830LevelMax || levels.initial>=levels.count)
        return refuse(out,A830VotesStepLevels,levels.count,levels.initial,kIOReturnBadArgument);
    for(unsigned i=0;i<levels.count;++i)
        if(!levels.levels[i].khz || !levels.levels[i].vlvl || (i && levels.levels[i].khz<=levels.levels[i-1].khz))
            return refuse(out,A830VotesStepLevels,i,levels.levels[i].khz,kIOReturnBadArgument);
    copyLevels(levels,out);
    if(annibale && !(options&A830VotesFullTable)) {
        const IOReturn narrowed=annibaleLevels(out);
        if(narrowed!=kIOReturnSuccess)return narrowed;
    } else if(annibale) {
        /* pagani-gpu-maxlevel: the design's whole table (levels, bus mapping, ACD values); the first vote stays annibale's
         * 160 MHz level, {1, 1, 2} on PKX110 (S5 picks it the same way). */
        const uint8_t first=A830Votes::levelAtOrAbove(*out,AnnibaleLevels[0].khz/1000u);
        out->initial=first?first:1;
    }
    IOReturn result=voteTables(res,board,out);
    if(result==kIOReturnSuccess)result=bwMessage(res,board,out);
    if(result==kIOReturnSuccess)result=acdMessage(out);
    if(result!=kIOReturnSuccess)return result;
    for(unsigned g=1;g<=out->levels;++g)
        if(out->level[g-1].bus>=out->ddr_levels || out->level[g-1].bus_max>=out->ddr_levels)
            return refuse(out,A830VotesStepDdr,g,out->level[g-1].bus_max,kIOReturnBadArgument);
    perfMessage(out);
    return kIOReturnSuccess;
}

IOReturn A830Votes::build(const Sm8750CmdDb &db,const Adreno830Board &board,const A830VoteInputs &inputs,A830VoteTables *out)
{
    if(!out)return kIOReturnBadArgument;
    /* The 128 KiB snapshot and the tables are the caller's (static); only the parsed resources live on the stack. */
    Sm8750RpmhResources res;
    const int parsed=Sm8750RpmhResourcesParse(&db,&res);
    if(parsed!=Sm8750RpmhOk) {
        memset(out,0,sizeof *out);
        out->bin=-1;
        return refuse(out,A830VotesStepCmdDb,uint32_t(parsed),res.failed,kIOReturnBadArgument);
    }
    A830VoteTables *selected=out;
    memset(selected,0,sizeof *selected);
    IOReturn result=selectLevels(board,inputs,selected);
    if(result!=kIOReturnSuccess)return result;
    Sm8750GpuLevelTable table={};
    table.count=selected->levels;table.initial=uint8_t(selected->initial-1);table.bin=selected->bin;table.fused=selected->fused;
    for(unsigned i=0;i<selected->levels;++i) {
        const Adreno830Level &from=selected->level[i];
        Sm8750GpuLevel &to=table.levels[i];
        to.khz=from.khz;to.vlvl=from.vlvl;to.bus_min=from.bus_min;to.bus=from.bus;to.bus_max=from.bus_max;to.reserved=0;
        to.acd=from.acd;
    }
    return build(res,board,table,inputs.options,out);
}

IOReturn A830Votes::build(const Sm8750CmdDb &db,const Adreno830Board &board,A830VoteTables *out)
{
    const A830VoteInputs inputs={A830VotesNoFuse,0,board.knobs.speedbin,optionsOf(board)};
    return build(db,board,inputs,out);
}
