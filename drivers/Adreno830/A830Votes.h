/* SPDX-License-Identifier: MIT */
#ifndef A830_VOTES_H
#define A830_VOTES_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
#include "A830Regs.h"
#include "Adreno830Board.h"
#include "Sm8750Rpmh.h"
#include "Sm8750SocInfo.h"

/* The RPMh vote tables the GMU firmware runs on (ARCHITECTURE 4.7.7, encodings of 4.3.2, levels of 4.1.4), built once at
 * S0 from what cmd-db holds on this phone and the DT-derived board tables, and kept for every later GMU boot (slumber wake,
 * recovery) unchanged:
 *  - the GMU level table: the levels every speed bin shares (pagani-gpu-speedbin=0, the default) or with the matched
 *    bin's faster levels appended (=1); GMU level g = table index + 1, level 0 is "off";
 *  - GX votes (KGSL adreno_rpmh_setup_volt_dependency_tbl): primary gfx.lvl, secondary gmxc.lvl when cmd-db has a
 *    non-dummy one, else mx.lvl; vote = cur_vlvl << 16 | sec_idx << 8 | pri_idx;
 *  - dependency votes (gen8 only, KGSL GEN8_DEP_VOTE_SET): mx index of the target vlvl, no CX dependency (the DT has no
 *    qcom,cx-level), 0xFFFFC000 | mx << 8 | 0xFF; Linux/author form 0x0FFFC000 | ... with pagani-gpu-hfi bit0;
 *    level 0 gets 0xFFFFFFFF;
 *  - CX votes of the GMU itself: {off, 500 MHz at 0x40, 650 MHz at 0x80}, primary cx.lvl, secondary mx.lvl;
 *  - the GMU bandwidth table (HFI BW_VOTE_TBL): one {SH0, MC0, ACV} row per DDR level, encoded by
 *    Sm8750RpmhGmuBwRequest (the GMU-table encoder; the host-vote encoder Sm8750RpmhHostDdrRequest never feeds this
 *    table, 4.3.2), and the CNOC rows {CN0 off, CN0 on};
 *  - the ACD table: bit g of enable_by_level for every GMU level with a qcom,acd-level, values in ascending g.
 * The three tables are also kept as ready HFI messages (header with sequence 0; A830Hfi stamps the sequence).
 * annibale profile (A830Profile.h; kernel-sequences section 14 rows 2 and 25, 15.1 item 1): the GMU level table is the
 * three points annibale runs on, 160/222/342 MHz at vlvl 0x32/0x34/0x38 and DDR index 2/2/3 (K/adreno8_cx.c:117-121),
 * taken from the board's selected table and required to match there exactly; each keeps one DDR index (bus_min = bus =
 * bus_max) and no ACD value, so the HFI perf table has the four entries {off, 160, 222, 342} (24 payload dwords, 5.4) and
 * the ACD table is empty; the initial level is 1 (160 MHz: the first vote is {1, 1, 2}, 6.1); the bandwidth rows always
 * have x = 0 (A830VotesBwNoAb, K/adreno8_hfi.c:442-443). The floor entry (GMU level 0) is the same in both profiles: vote 0,
 * dependency 0xFFFFFFFF, 0 kHz (K/adreno8_cx.c:322-324, adreno8_dep_vote).
 * pagani-gpu-maxlevel != 0 (A830VotesFullTable) keeps the board's whole selected table in the annibale profile as well: the
 * design's levels with their DT bus-min/bus/bus-max and ACD values, the same GX/dependency/CX votes and ACD table (sent only
 * with pagani-gpu-acd=1, as in the design), the 12 DDR rows with annibale's x = 0; the initial level stays the 160 MHz
 * level, so the first vote is still {1, 1, 2}.
 * Pure computation: no hardware access, no allocation, no logging. */

enum : uint32_t {
    A830VotesMaxGx=Adreno830LevelMax+1,     /* GPU levels + off (HFI table format allows 32) */
    A830VotesMaxCx=4,                       /* MAX_CX_LEVELS */
    A830VotesCxLevels=3,                    /* off, 500 MHz, 650 MHz */
    A830VotesMaxDdr=A830_HFI_BW_MAX_LEVELS, /* 16 */
    A830VotesMaxAcd=16,                     /* MAX_ACD_NUM_LEVELS (stride 1 uses data[0..15] of data[32]) */
    A830VotesPerfDwordsMax=3u+2u+3u*A830VotesMaxGx+2u+2u*A830VotesMaxCx,   /* 64: hdr, version, type, two entries */
    A830VotesNoFuse=0xffffffffu,            /* A830VoteInputs::speed_bin when the fuse was not read */
    A830VotesAnnibaleLevels=3,              /* annibale profile: GPU levels 160/222/342 MHz (GMU levels 1..3) */
    A830VotesAnnibaleInitial=1,             /* annibale profile: the first vote's GMU level (160 MHz) */
};

/* Build options. */
enum A830VotesOption : uint32_t {
    A830VotesLinuxDependency=1u<<0,   /* pagani-gpu-hfi bit0: dependency votes 0x0FFFC000|mx<<8|cx (Linux/author) */
    A830VotesBwNoAb=1u<<1,            /* GMU bandwidth rows with x = 0 (KGSL without AB) instead of x = y (Linux a8xx) */
    A830VotesFullTable=1u<<2,         /* pagani-gpu-maxlevel != 0: no annibale narrowing, the board's whole selected table */
};

/* Where a build failed (A830VoteTables::error_step); error_value says which entry. */
enum A830VotesStep : uint32_t {
    A830VotesStepNone,
    A830VotesStepCmdDb,       /* error_value = Sm8750Rpmh status, error_resource = Sm8750RpmhRes* */
    A830VotesStepBins,        /* the board's bins: error_value = bin index (0xff: none usable) */
    A830VotesStepLevels,      /* level selection: error_value = Sm8750SocInfo status, or the table size; annibale profile:
                                 error_value = the annibale level (1..3) the board's table lacks with the same kHz, vlvl
                                 and bus, error_resource = its kHz */
    A830VotesStepCommon,      /* the common prefix disagrees with board.common_level_count: error_value = computed count */
    A830VotesStepGx,          /* no gfx.lvl level >= the vlvl of GMU level error_value (error_resource = vlvl) */
    A830VotesStepCx,          /* no cx.lvl level >= the vlvl of GMU clock error_value */
    A830VotesStepDdr,         /* DDR table: error_value = level (count, unsorted entry, or an encoder failure) */
    A830VotesStepCnoc,        /* CNOC table */
    A830VotesStepAcd,         /* more ACD values than the table holds */
};

struct A830GxVote {uint32_t vote,dep,khz;};   /* HFI opp_gx_desc: ARC vote, dependency vote (the firmware's acdLvl), kHz */
struct A830CxVote {uint32_t vote,khz;};       /* HFI opp_desc */

/* Runtime inputs of the level selection, read at S0. */
struct A830VoteInputs {
    uint32_t speed_bin;       /* qfprom gpu_speed_bin (bits [8:0]); A830VotesNoFuse: not read, no bin matches */
    uint32_t soc_code;        /* Sm8750SocCode() of the SMEM socinfo (KGSL adreno_read_soc_code) */
    uint32_t fused;           /* pagani-gpu-speedbin: append the matched bin's faster levels */
    uint32_t options;         /* A830VotesOption */
};

struct A830VoteTables {
    /* GMU level table (4.1.4), ascending: GMU level g = index + 1. */
    Adreno830Level level[Adreno830LevelMax];
    uint8_t levels;           /* GPU levels; GMU levels 1..levels (annibale profile: 3, or the board's with A830VotesFullTable) */
    uint8_t initial;          /* initial GMU level (2 = 222 MHz on PKX110; annibale profile: 1 = 160 MHz, also full-table) */
    int8_t bin;               /* matched speed-bin group, -1 none */
    uint8_t fused;            /* 1: the matched bin's faster levels were appended (annibale profile: always 0) */
    uint32_t options;         /* A830VotesOption used (annibale profile: with A830VotesBwNoAb) */
    /* Vote tables, index = GMU level (0 = off). */
    A830GxVote gx[A830VotesMaxGx];
    A830CxVote cx[A830VotesMaxCx];
    uint8_t gx_count;         /* levels + 1 */
    uint8_t cx_count;         /* 3 */
    uint8_t ddr_levels;       /* bandwidth levels (12) */
    uint8_t acd_levels;       /* levels with an ACD value (8 for the common table); 0: the ACD table is never sent */
    uint32_t acd_mask;        /* enable_by_level (0x0FF0 for the common table) */
    uint32_t ddr_kbps[A830VotesMaxDdr];
    uint32_t gx_secondary;    /* Sm8750RpmhResGmxc or Sm8750RpmhResMx: the GX vote's secondary rail */
    uint32_t bw_wait_mask;    /* the DDR commit bits (BW table ddr_wait_bitmask) */
    /* Ready HFI messages; word 0 is the header with sequence 0. */
    uint32_t perf[A830VotesPerfDwordsMax];        /* H2F_MSG_TABLE, HFI_TABLE_GPU_PERF: 49 dwords for 11 levels */
    uint8_t perf_dwords;
    uint32_t bw[A830_HFI_BW_TABLE_DWORDS];        /* H2F_MSG_BW_VOTE_TBL, 160 dwords */
    uint32_t acd[A830_HFI_ACD_TABLE_DWORDS];      /* H2F_MSG_ACD_TBL, 37 dwords */
    /* Failure detail (A830VotesStep). */
    uint32_t error_step,error_value,error_resource;
};

/* Dword layout of the HFI messages (the firmware's structures, ARCHITECTURE 4.7.6): the hfi_bwtable_cmd field indexes and
 * the ACD table's version/stride values (KGSL gen8_gmu_acd_probe), from A830Regs.h section 21. */
enum : uint32_t {
    A830BwLevelNum=A830_HFI_BW_LEVEL_NUM,A830BwCnocCmds=A830_HFI_BW_CNOC_CMDS,A830BwDdrCmds=A830_HFI_BW_DDR_CMDS,
    A830BwCnocWait=A830_HFI_BW_CNOC_WAIT,A830BwDdrWait=A830_HFI_BW_DDR_WAIT,   /* dword indexes, header = 0 */
    A830BwCnocAddrs=A830_HFI_BW_CNOC_ADDRS,                    /* [6] */
    A830BwCnocData=A830_HFI_BW_CNOC_DATA,                      /* [2][6] */
    A830BwDdrAddrs=A830_HFI_BW_DDR_ADDRS,                      /* [8] */
    A830BwDdrData=A830_HFI_BW_DDR_DATA,                        /* [16][8] */
    A830AcdVersion=A830_HFI_ACD_VERSION_IDX,A830AcdEnableByLevel=A830_HFI_ACD_ENABLE_IDX,A830AcdStride=A830_HFI_ACD_STRIDE_IDX,
    A830AcdNumLevels=A830_HFI_ACD_LEVELS_IDX,A830AcdData=A830_HFI_ACD_DATA_IDX,
    A830AcdTableVersion=A830_HFI_ACD_TABLE_VERSION,A830AcdTableStride=A830_HFI_ACD_TABLE_STRIDE,
};
static_assert(A830BwDdrData+A830_HFI_BW_MAX_LEVELS*A830_HFI_BW_MAX_DDR_CMDS==A830_HFI_BW_TABLE_DWORDS,"160-dword BW table");
static_assert(A830AcdData+2*A830VotesMaxAcd==A830_HFI_ACD_TABLE_DWORDS,"37-dword ACD table");
static_assert(A830VotesPerfDwordsMax<=255,"one HFI message");

class A830Votes {
public:
    /* Design entry point (IMPLEMENTATION_PLAN M3): parse cmd-db, select the levels without fuse data (the common table;
     * a pagani-gpu-speedbin=1 needs the fuse, see the next form), options from the board knobs (pagani-gpu-hfi bit0). */
    static IOReturn build(const Sm8750CmdDb &db,const Adreno830Board &board,A830VoteTables *out);
    /* S0: with the fuse and the SoC code it read. */
    static IOReturn build(const Sm8750CmdDb &db,const Adreno830Board &board,const A830VoteInputs &inputs,
                          A830VoteTables *out);
    /* S0, from what the orchestrator already parsed and selected (one cmd-db parse, one level table for the whole boot).
     * Every build goes through here; the annibale profile narrows `levels` to annibale's three points here, unless
     * options has A830VotesFullTable (pagani-gpu-maxlevel). */
    static IOReturn build(const Sm8750RpmhResources &res,const Adreno830Board &board,const Sm8750GpuLevelTable &levels,
                          uint32_t options,A830VoteTables *out);

    /* M1 level-table selection (4.1.3/4.1.4): the board's six bins through Sm8750GpuLevelsSelect (KGSL bin match), checked
     * against board.common_level_count. Fills level/levels/initial/bin/fused of out only (the board's table in both
     * profiles: the annibale narrowing belongs to build). */
    static IOReturn selectLevels(const Adreno830Board &board,const A830VoteInputs &inputs,A830VoteTables *out);
    /* The inputs the board's knobs and S0's reads give: pagani-gpu-speedbin, pagani-gpu-hfi bit0, pagani-gpu-maxlevel
     * (A830VotesFullTable); speedBin is A830VotesNoFuse when the qfprom read failed. */
    static A830VoteInputs inputsFor(const Adreno830Board &board,uint32_t speedBin,uint32_t socCode);
    /* The board's bins in Sm8750SocInfo form (a bin with sku codes matches by SKU, a bin without by speed bin). Returns the
     * number of usable bins copied (bins with no level table are left out and end the list). */
    static unsigned boardBins(const Adreno830Board &board,Sm8750GpuBin bins[Adreno830BinCount]);

    /* KGSL adreno_rpmh_setup_volt_dependency_tbl for one vlvl: kIOReturnBadArgument when no primary level reaches it. */
    static IOReturn arcVote(const Sm8750RpmhArc &primary,const Sm8750RpmhArc &secondary,uint32_t vlvl,uint32_t *vote);
    /* KGSL GEN8_DEP_VOTE_SET (or the Linux form): mx index of the first mx level >= vlvl, the last one when none is. */
    static uint32_t dependencyVote(const Sm8750RpmhArc &mx,uint32_t vlvl,bool linuxForm);
    /* The GX secondary rail: gmxc.lvl when present and not a dummy, else mx.lvl. */
    static const Sm8750RpmhArc &gxSecondary(const Sm8750RpmhResources &res);
    /* One GMU bandwidth row {SH0, MC0, ACV} through Sm8750RpmhGmuBwRequest; *wait = the commit bits. */
    static IOReturn bwRow(const Sm8750RpmhResources &res,uint32_t kbps,uint32_t options,uint32_t data[3],uint32_t *wait);

    /* GX_BW_PERF_VOTE / PREPARE_SLUMBER bw word (4.16): bus | 0xFFFF0000 without AB; with AB (the GMU answered
     * GET_VALUE(GMU_AB_VOTE) == 1) bus | 1 << 8 | quantized << 16, quantized = ab_kBps * 65536 / (top_kBps * 4) clamped to
     * [1, 0xFFFE] (KGSL adreno_gmu_bus_ab_quantize, num_ddr_channels 4). bus 0xFF means "unchanged". annibale profile:
     * without AB the bus alone (its votes are {1, level, bus}, K/adreno8_hfi.c:553,658; section 14 row 24). */
    static uint32_t bwWord(uint8_t bus,bool abSupported,uint32_t abKbps,uint32_t topKbps);
    static uint32_t abQuantize(uint32_t abKbps,uint32_t topKbps);

    /* kBps of DDR level `level` (0 above the table): the host DDR votes of S2/S5 and the floor (4.3.3). */
    static uint32_t ddrKbps(const A830VoteTables &tables,unsigned level);
    /* GMU level of a frequency: the slowest level at or above mhz (DCVS bounds, pagani-gpu-dcvs-min/max), 0 if none. */
    static uint8_t levelAtOrAbove(const A830VoteTables &tables,uint32_t mhz);
    /* GMU level of a frequency: the fastest level at or below mhz, 0 if none. */
    static uint8_t levelAtOrBelow(const A830VoteTables &tables,uint32_t mhz);

    /* S0 diagnostic page words of Adreno830Status.h (aux[1]: ARC level counts gfx|mx<<8|cx<<16|gmxc<<24, 0xff = absent;
     * aux[2]/aux[3]: SH0/MC0 unit | width << 32; aux[4]: ACV vcd | CN0 vcd << 8). Returns the aux[0] valid bits to OR in
     * (A830S0Arc | A830S0Bcm once both parsed). */
    static uint64_t statusAux(const Sm8750RpmhResources &res,uint64_t aux[6]);
};

#endif
