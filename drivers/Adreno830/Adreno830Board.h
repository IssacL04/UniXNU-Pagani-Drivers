/* SPDX-License-Identifier: MIT */
#ifndef ADRENO830_BOARD_H
#define ADRENO830_BOARD_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "A830Profile.h"
#include "NativeAdrenoKopt.h"

/* The only input of the A830 driver: the board fills this structure from its build-time device-tree constants and the
 * boot arguments, then calls adreno830_start(). The driver never reads the ADT or a device repository header; it reads
 * no boot argument itself either (the board passes a getter to Adreno830ParseKnobs below). Every address is physical,
 * every interrupt is a GIC INTID (SPI + 32). */
enum {Adreno830BoardMagic=0x41383342 /* 'A83B' */,Adreno830BoardVersion=1};
/* Adreno830LevelMax: the most GPU levels one table holds, the 11 every bin has plus the speed-bin additions of
 * ARCHITECTURE 4.1.4 (KGSL_MAX_PWRLEVELS); GMU levels run 1..count, 0 is off. */
enum {Adreno830BinCount=6,Adreno830BinSkus=4,Adreno830LevelMax=16};

struct Adreno830Window {uint64_t phys,bytes;};
struct Adreno830Level {             /* one GPU frequency (ascending; GMU level 0 = off is not in the table) */
    uint32_t khz,vlvl;               /* qcom,gpu-freq / qcom,level */
    uint8_t bus_min,bus,bus_max;     /* qcom,bus-min / bus-freq / bus-max */
    uint8_t reserved;
    uint32_t acd;                    /* qcom,acd-level, 0 = none */
};
static_assert(sizeof(Adreno830Level)==16 && offsetof(Adreno830Level,acd)==12,
              "the generated Sm8750GpuLevel tables are handed over as they are");
struct Adreno830Firmware {const uint8_t *bytes;uint64_t length;uint8_t sha256[32];};   /* length: embedded; sha256: pinned */
/* LLCC v6 windows of the runtime DT cache-controller@24800000 (ARCHITECTURE 3.2 B, 4.15), in reg-names order. */
enum Adreno830Llcc : unsigned {Adreno830Llcc0,Adreno830Llcc1,Adreno830Llcc2,Adreno830Llcc3,Adreno830LlccBroadcastOr,
                               Adreno830LlccBroadcastAnd,Adreno830LlccWindows};

/* Boot-argument knobs (ARCHITECTURE 3.6), in this order. Adreno830ParseKnobs sets bit (1 << index) of Adreno830Board::flags
 * for every knob given on the command line, so a stage can tell a default from an explicit value (coherent, perf, ddr). */
enum Adreno830Knob : uint32_t {
    Adreno830KnobStop,Adreno830KnobStatus,Adreno830KnobAsync,Adreno830KnobRails,Adreno830KnobDdrBoot,Adreno830KnobDdrFloor,
    Adreno830KnobActlr,Adreno830KnobCoherent,Adreno830KnobDcvs,Adreno830KnobDcvsMin,Adreno830KnobDcvsMax,Adreno830KnobPerf,
    Adreno830KnobLlcc,Adreno830KnobSpeedbin,Adreno830KnobAcd,Adreno830KnobHfi,Adreno830KnobDiag,Adreno830KnobQmp,
    Adreno830KnobCohtest,Adreno830KnobVa,
    /* M13 single-variable switches the design leaves unnamed (IMPLEMENTATION_PLAN M13 c, e, f; ARCHITECTURE 4.4.2, 4.17):
     * all default to the design's P0 (no slumber, no IFPC, no CX_AHB timeout detection). */
    Adreno830KnobSlumber,Adreno830KnobIfpc,Adreno830KnobAhbTimeout,
    /* The bring-up profile (A830Profile.h): 1 annibale (default), 0 design; S0 checks it and sets adreno830_profile_value
     * before anything reads it. HWSCHED is reserved: 0 only, S0 refuses anything else (reason 24) until the GMU hardware
     * scheduler is built (annibale kernel-sequences section 14 row 1: RB is the main line, HWSCHED the proven fallback). */
    Adreno830KnobProfile,Adreno830KnobHwsched,
    /* The bring-up register trace (A830Trace.h, ARCHITECTURE 8.7): 1 (default while the bring-up runs) every register
     * access and SMC of the bring-up and of the power/recovery operations goes to the ramoops log before it is made, polls
     * and sequential blocks counted; 2 the same with a line for every access; 0 off. Nothing is written while BootProgress's
     * log is off (pagani-ramoops=0) or absent. */
    Adreno830KnobTrace,
    /* Performance switches of the annibale profile (both 0 = the first round as measured, 2026-10-30):
     *  - pagani-gpu-maxlevel=n (0..Adreno830LevelMax): the GMU gets the whole DT-derived level table the design profile
     *    builds (A830Votes, option A830VotesFullTable) instead of annibale's 160/222/342 MHz points, everything else of the
     *    profile kept, and the DCVS ceiling is GMU level n (n beyond the table: its top) instead of 342 MHz, still under the
     *    ACD-off 734 MHz ceiling, an explicit pagani-gpu-dcvs-max and the thermal rule (A830Dcvs). In the design profile
     *    (whose table is already whole) n is only that ceiling.
     *  - pagani-gpu-irq=1: S8 attaches GPU interrupt 332 as the design does and unmasks the ring fence interrupt only
     *    (A830IrqConfig::own_mask), so fence waits wake on it; a refused attach or a fence interrupt that never arrives
     *    leaves the polled fences of the first round (irq_off bit 0), never a failure. No effect in the design profile
     *    (332 is its hard dependency there). */
    Adreno830KnobMaxLevel,Adreno830KnobIrq,
    /* pagani-gpu-ttarget=<C> (50..100, default 95): the target temperature of the design profile's thermal loop (A830Dcvs,
     * ARCHITECTURE 4.16); the annibale profile keeps its 70/60 C rule. */
    Adreno830KnobTtarget,
    /* pagani-screenoff=<bits> (default 0), shared with the board's other drivers (bit 0 the CPU governor, bit 2 the panel):
     * bit 1 lets the GPU slumber while the display is off (A830PowerConfig::screenSlumber, CX kept), bit 3 with the full
     * collapse instead. Other bits are not the GPU's: S0 checks no range. */
    Adreno830KnobScreenOff,
    /* pagani-gpu-kopt=<bits> (default NativeAdrenoKoptAll): the kernel-side latency changes of 2026-10-31, a bit each (NativeAdrenoKopt.h);
     * S0 refuses any other bit. The last knob this board word has room for: the pool flag takes bit 31. */
    Adreno830KnobKopt,Adreno830KnobCount
};
static_assert(uint32_t(Adreno830KnobCount)<=31,"Adreno830Board::flags: a bit per knob, then Adreno830BoardFlagPool");
enum : uint32_t {
    Adreno830StopNever=0xffffffff,   /* knobs.stop when pagani-gpu-stop is absent */
    Adreno830BoardFlagPool=1u<<Adreno830KnobCount,   /* pagani-gpu-pool was given (pool_mib) */
};

struct Adreno830Board {
    uint32_t magic,version;
    Adreno830Window gpu,rscc,cx_dbgc,cx_misc,gmu,gmu_ao,gpucc,gcc,smmu,cmd_db,smem,zap,qfprom,rsc_drv2,
                    aoss_msgram,ipcc,tsens_tm;           /* the last three are optional: the M3 QMP step, M11 thermal */
    Adreno830Window llcc[Adreno830LlccWindows];      /* optional (M11): zero windows leave the LLCC slices untouched */
    uint32_t chip_id,soc_id;                          /* 0x44050001, 618 */
    uint32_t irq_gpu,irq_cx_host,irq_hfi,irq_gmu;    /* 332, 112, 336, 337 */
    uint32_t irq_smmu_global,irq_smmu_cb[4];         /* 706, 710..713 */
    uint32_t irq_scm_waitq;                          /* 994 */
    uint16_t sid_gpu,sid_lpac,sid_gmu;               /* 0, 1, 5 */
    uint8_t slot_gpu,slot_lpac,slot_gmu;             /* preferred 0, 3, 1 */
    uint8_t cb_gpu,cb_gmu;                           /* 0, 2 */
    uint32_t smmu_actlr;                             /* 0x303 */
    uint32_t pool_mib;                               /* 512 */
    uint32_t qfprom_speedbin_offset,speedbin_bits;   /* 0x138, 9 */
    const Adreno830Level *levels[Adreno830BinCount];uint8_t level_count[Adreno830BinCount];uint8_t initial_level[Adreno830BinCount];
    uint32_t bin_speed[Adreno830BinCount],bin_sku_count[Adreno830BinCount],bin_sku[Adreno830BinCount][Adreno830BinSkus];
    uint32_t common_level_count;                     /* 11: the <= 900 MHz levels every bin has */
    const uint32_t *ddr_kbps;uint8_t ddr_levels;     /* 12 levels */
    uint32_t cnoc_levels[2];                         /* {0, 100} */
    uint32_t gmu_khz[2],gmu_vlvl[2];                 /* {500000, 650000}, {0x40, 0x80} */
    uint32_t ubwc_mode,highest_bank_bit,min_access_length;
    Adreno830Firmware gmu_fw,sqe_fw,aqe_fw,zap_fw;
    uint32_t flags;                                  /* Adreno830Knob bits and Adreno830BoardFlag*: boot arguments given */
    struct Adreno830Knobs {uint32_t stop,status,async,rails,ddr_boot,ddr_floor,actlr,coherent,
                           dcvs,dcvs_min_mhz,dcvs_max_mhz,perf,llcc,speedbin,acd,hfi,diag,
                           qmp,cohtest,va,
                           slumber,ifpc,ahbto,          /* slumber 0 off, 1 P1 full, 2 P1 keeping CX; ifpc 0/1 (P2); ahbto 0/1 */
                           profile,hwsched,             /* profile A830Profile* (0/1); hwsched 0 (reserved) */
                           trace,                       /* 0 off, 1 bring-up trace (default), 2 every access */
                           maxlevel,irq,                /* maxlevel 0 or a GMU level (1..16); irq 0/1 (annibale profile) */
                           ttarget,                     /* design thermal loop target, whole degrees C (50..100) */
                           screenoff,                   /* pagani-screenoff bits (the GPU's: 1 slumber while dark, 3 full) */
                           kopt;} knobs;                /* pagani-gpu-kopt bits (NativeAdrenoKopt*) */
    uint32_t system_diag;                            /* pagani-diag: not a knob of this driver (no flag bit, no table row, 0 when absent) */
};
static_assert(sizeof(Adreno830Board::Adreno830Knobs)==Adreno830KnobCount*sizeof(uint32_t),"one word per knob, in Adreno830Knob order");

/* Boot-argument names and fields, in Adreno830Knob order. "pagani-gpu" itself is the board's master switch, not a knob. */
using Adreno830KnobField=uint32_t Adreno830Board::Adreno830Knobs::*;
inline constexpr struct {const char *name;Adreno830KnobField field;} Adreno830KnobTable[Adreno830KnobCount]={
    {"pagani-gpu-stop",&Adreno830Board::Adreno830Knobs::stop},
    {"pagani-gpu-status",&Adreno830Board::Adreno830Knobs::status},
    {"pagani-gpu-async",&Adreno830Board::Adreno830Knobs::async},
    {"pagani-gpu-rails",&Adreno830Board::Adreno830Knobs::rails},
    {"pagani-gpu-ddr",&Adreno830Board::Adreno830Knobs::ddr_boot},
    {"pagani-gpu-ddr-floor",&Adreno830Board::Adreno830Knobs::ddr_floor},
    {"pagani-gpu-actlr",&Adreno830Board::Adreno830Knobs::actlr},
    {"pagani-gpu-coherent",&Adreno830Board::Adreno830Knobs::coherent},
    {"pagani-gpu-dcvs",&Adreno830Board::Adreno830Knobs::dcvs},
    {"pagani-gpu-dcvs-min",&Adreno830Board::Adreno830Knobs::dcvs_min_mhz},
    {"pagani-gpu-dcvs-max",&Adreno830Board::Adreno830Knobs::dcvs_max_mhz},
    {"pagani-gpu-perf",&Adreno830Board::Adreno830Knobs::perf},
    {"pagani-gpu-llcc",&Adreno830Board::Adreno830Knobs::llcc},
    {"pagani-gpu-speedbin",&Adreno830Board::Adreno830Knobs::speedbin},
    {"pagani-gpu-acd",&Adreno830Board::Adreno830Knobs::acd},
    {"pagani-gpu-hfi",&Adreno830Board::Adreno830Knobs::hfi},
    {"pagani-gpu-diag",&Adreno830Board::Adreno830Knobs::diag},
    {"pagani-gpu-qmp",&Adreno830Board::Adreno830Knobs::qmp},
    {"pagani-gpu-cohtest",&Adreno830Board::Adreno830Knobs::cohtest},
    {"pagani-gpu-va",&Adreno830Board::Adreno830Knobs::va},
    {"pagani-gpu-slumber",&Adreno830Board::Adreno830Knobs::slumber},
    {"pagani-gpu-ifpc",&Adreno830Board::Adreno830Knobs::ifpc},
    {"pagani-gpu-ahbto",&Adreno830Board::Adreno830Knobs::ahbto},
    {"pagani-gpu-profile",&Adreno830Board::Adreno830Knobs::profile},
    {"pagani-gpu-hwsched",&Adreno830Board::Adreno830Knobs::hwsched},
    {"pagani-gpu-trace",&Adreno830Board::Adreno830Knobs::trace},
    {"pagani-gpu-maxlevel",&Adreno830Board::Adreno830Knobs::maxlevel},
    {"pagani-gpu-irq",&Adreno830Board::Adreno830Knobs::irq},
    {"pagani-gpu-ttarget",&Adreno830Board::Adreno830Knobs::ttarget},
    {"pagani-screenoff",&Adreno830Board::Adreno830Knobs::screenoff},
    {"pagani-gpu-kopt",&Adreno830Board::Adreno830Knobs::kopt}};
inline constexpr const char *Adreno830PoolKnobName="pagani-gpu-pool";
/* The system's diagnostic switch (BootProgress's stall probe, PaganiDiag): here it turns on the run-time lines of ARCHITECTURE 8.8 and 6.2
 * (adreno830_status_diag). The test images carry it; a default boot, and the loop images that have only the 32 KiB events ring, do not. */
inline constexpr const char *Adreno830SystemDiagName="pagani-diag";

/* Defaults of ARCHITECTURE 3.6: the host rail and DDR votes the author's build was measured with (rails 1, DDR level 11,
 * floor = the boot level), the fixed GMU level 2 (222 MHz) of pagani-gpu-dcvs=0, the 16 GiB CB0 aperture (2026-10-31: 32 client windows;
 * the context falls back to 4 GiB by itself without a 34-bit input or its 32 MiB of tables, pagani-gpu-va=4 asks for it). coherent defaults
 * to the S8 self-test, which a stage tells apart from an explicit 0 by the flag bit. The profile defaults to annibale; the
 * DCVS maximum stays the design's 734 MHz here, and the annibale profile caps the governor at 342 MHz unless
 * pagani-gpu-dcvs-max is given (A830Dcvs; the flag bit tells the two apart). The register trace is on while the bring-up
 * runs on hardware (it writes nothing unless BootProgress's ramoops log runs). pagani-gpu-maxlevel and pagani-gpu-irq
 * default to 0: the first round's three levels and polled fences. The design thermal loop aims at 95 C. */
static inline void Adreno830KnobDefaults(Adreno830Board::Adreno830Knobs *knobs)
{
    *knobs={};
    knobs->stop=Adreno830StopNever;knobs->status=1;knobs->rails=1;knobs->ddr_boot=11;knobs->ddr_floor=11;knobs->actlr=0x303;
    knobs->dcvs=1;knobs->dcvs_min_mhz=160;knobs->dcvs_max_mhz=734;knobs->perf=2;knobs->va=16;
    knobs->profile=A830ProfileAnnibale;knobs->hwsched=0;knobs->trace=1;knobs->ttarget=95;knobs->kopt=NativeAdrenoKoptAll;
}

/* Fill board->knobs, board->flags and board->pool_mib from a boot-argument getter (PE_parse_boot_argn on the device, a table in
 * the host tests). get returns true and the value when the argument is present. The board's own pool_mib and smmu_actlr are
 * the defaults of pagani-gpu-pool and pagani-gpu-actlr. Values are taken as given; adreno830_start validates them. */
static inline void Adreno830ParseKnobs(Adreno830Board *board,bool (*get)(void *cookie,const char *name,uint32_t *value),void *cookie)
{
    if(!board || !get)return;
    Adreno830KnobDefaults(&board->knobs);
    if(board->smmu_actlr)board->knobs.actlr=board->smmu_actlr;
    board->flags&=~((1u<<Adreno830KnobCount)-1u|Adreno830BoardFlagPool);
    for(unsigned i=0;i<Adreno830KnobCount;++i) {
        uint32_t value=0;
        if(get(cookie,Adreno830KnobTable[i].name,&value)) {board->knobs.*Adreno830KnobTable[i].field=value;board->flags|=1u<<i;}
    }
    uint32_t pool=0;
    if(get(cookie,Adreno830PoolKnobName,&pool)) {board->pool_mib=pool;board->flags|=Adreno830BoardFlagPool;}
    board->system_diag=0;
    (void)get(cookie,Adreno830SystemDiagName,&board->system_diag);
    /* pagani-gpu-ddr-floor defaults to the boot level, explicit or not: the host DDR vote is not lowered unless asked. */
    if(!(board->flags&(1u<<Adreno830KnobDdrFloor)))board->knobs.ddr_floor=board->knobs.ddr_boot;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous bring-up of the stages S0..S10 (ARCHITECTURE 3.4); returns once the GPU runs, a stage failed, or the stage
 * selected by pagani-gpu-stop succeeded. Called once per boot; a second call is refused with kIOReturnBusy. */
IOReturn adreno830_start(const Adreno830Board *board);
#ifdef __cplusplus
}
#endif
#endif
