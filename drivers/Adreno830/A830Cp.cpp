/* SPDX-License-Identifier: MIT */
#include "A830Cp.h"
#include "A830HwInit.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoPwrup.h"
#include <IOKit/IOLib.h>
#include <string.h>

/* Order: Linux hw_init (a8xx_gpu.c), a8xx_cp_init, a8xx_set_hwcg, a8xx_patch_pwrup_reglist, a8xx_bus_clear_pending_transactions;
 * values: KGSL gen8_start/gen8_rb_start/gen8_cp_init_cmds/gen8_hwcg_set and the generated A830HwInit.h (ARCHITECTURE 4.8-4.11).
 * The annibale profile follows the sequences the REDMI K90 port proved (annibale kernel-sequences 9.2, 9.4, 9.7). Only
 * numeric facts are taken from those sources; the code is this driver's own. */


namespace {
enum : uint32_t {
    ApertureUnknown=0xffffffffu,
    SqeStatCount=16,
    PwrupPage=4096,
    P2sPolls=A830_T_CGC_P2S_US,       /* 10 polls, 1 us apart */
    SpinPolls=100,                    /* GBIF halt: 100 x 1 us spins before 100 us sleeps */
    PipeFirst=A8XX_PIPE_BR,PipeLast=A8XX_PIPE_DDE_BV,
    /* The annibale profile (kernel-sequences 9.2 row 34, 9.4, 9.7). */
    AnnibaleRbbmErrors=0x33C002C2u,   /* RBBM_INT_0_STATUS bits 1,6,7,9,22,23,24,25,28,29 (A8XX_INT_ERRORS without CP_SW) */
    AnnibaleCpErrors=A830_CP_INTERRUPT_STATUS_MASK_GLOBAL_VALUE,   /* CP_INTERRUPT_STATUS_GLOBAL: BR..DDE_BV faults */
    AnnibaleCxBusy=1u<<23,            /* GMUAO GPU_CX_BUSY_STATUS: clears when the GPU is idle (measured on the A830) */
    AnnibaleInitUs=200000,            /* the CP_ME_INIT wait: 2000 polls of 100 us */
    AnnibaleP2sPolls=3,               /* HWCG: P2S polled 3 times, 1 us before each */
    CpSqeIcacheCntlPipe=0x008B0u,     /* CP_SQE_ICACHE_CNTL_PIPE (Mesa a8xx), annibale's timeout dump */
    SgfsrDword=uint32_t((A830_SMMU_PHYS+A830_SMMU_GR0_SGFSR-A830_GPU_PHYS)/4),   /* sGFSR as a GPU-space dword (failure reg) */
};
static_assert(AnnibaleRbbmErrors==(A8XX_INT_ERRORS&~A8XX_RBBM_INT_0_CP_SW),"annibale's GPU error mask");
static_assert(AnnibaleInitUs/A830_POLL_STEP_US==2000,"annibale waits 2000 rounds for CP_ME_INIT");
static_assert(A830_GPU_PHYS+4ull*SgfsrDword==0x03DA0048ull,"sGFSR");
/* The pipes S6 programs per pipe (Linux hw_init and a8xx_nonctxt_config skip LPAC: no LPAC queue). */
bool hwInitPipe(uint32_t pipe) {return pipe!=A8XX_PIPE_LPAC && pipe<=A8XX_PIPE_DDE_BV;}
static_assert(A830_HWINIT_PWRUP_COUNT<=255 && A830_HWINIT_IFPC_COUNT<=255 && A830_HWINIT_DYN_TRIPLETS<=0xffff,
              "the cpu_gpu_lock length fields");
static_assert(size_t(NativeAdrenoPwrup::HeaderBytes)+(A830_HWINIT_PWRUP_COUNT+A830_HWINIT_IFPC_COUNT)*size_t(NativeAdrenoPwrup::PairBytes)+
              A830_HWINIT_DYN_TRIPLETS*size_t(NativeAdrenoPwrup::TripletBytes)<=size_t(PwrupPage),"the power-up list fits its page");
}

A830HwInitConfig A830HwInitDefaults()
{
    A830HwInitConfig config{};
    config.ubwc_mode=A830_UBWC_VERSION;config.highest_bank_bit=A830_HIGHEST_BANK_BIT;
    config.min_access_length=A830_MIN_ACCESS_LENGTH;config.gmem_base=A830_GMEM_BASE;config.uche_trap_base=A830_UCHE_TRAP_ADDR;
    config.llcc_scid=0;
    return config;
}

IOReturn A830Cp::fail(uint16_t step,uint16_t reason,uint32_t reg,uint64_t value,uint64_t expect,IOReturn result)
{
    failure_={step,reason,reg?A830_GPU_REG_PHYS(reg):0,value,expect};
    return result;
}
IOReturn A830Cp::write(uint16_t step,uint32_t reg,uint32_t value)
{
    if(!io_.write32(io_.cookie,reg,value))return kIOReturnSuccess;
    io_error_=true;
    return fail(step,A830ReasonUnexpectedState,reg,value,0,kIOReturnIOError);
}
IOReturn A830Cp::read(uint16_t step,uint32_t reg,uint32_t *value)
{
    *value=0;
    if(!io_.read32(io_.cookie,reg,value))return kIOReturnSuccess;
    io_error_=true;
    return fail(step,A830ReasonUnexpectedState,reg,0,0,kIOReturnIOError);
}
IOReturn A830Cp::write64(uint16_t step,uint32_t reg,uint64_t value)
{
    const IOReturn status=write(step,reg,uint32_t(value));
    return status?status:write(step,reg+1,uint32_t(value>>32));
}
IOReturn A830Cp::barrier(uint16_t step)
{
    if(!io_.barrier(io_.cookie))return kIOReturnSuccess;
    io_error_=true;
    return fail(step,A830ReasonUnexpectedState,0,0,0,kIOReturnIOError);
}
/* annibale's checked write: the write, a barrier, then the register must read back what was written. */
IOReturn A830Cp::writeChecked(uint16_t step,uint32_t reg,uint32_t value)
{
    IOReturn s=write(step,reg,value);
    if(!s)s=barrier(step);
    uint32_t seen=0;
    if(!s)s=read(step,reg,&seen);
    if(!s && seen!=value)s=fail(step,A830ReasonReadback,reg,seen,value,kIOReturnIOError);
    return s;
}
/* CP_APERTURE_CNTL_HOST = pipe << 12 (slice 0, no slice select), skipped when it already holds that value; a barrier
 * follows so the pipe access after it is routed by it (KGSL gen8_host_aperture_set). The annibale profile reads every
 * aperture write back and stops when it differs (annibale gpu_aperture). */
IOReturn A830Cp::aperture(uint16_t step,uint32_t pipe)
{
    const uint32_t value=A8XX_CP_APERTURE_CNTL_HOST_PIPE(pipe);
    if(value==aperture_)return kIOReturnSuccess;
    aperture_=ApertureUnknown;
    IOReturn status=kIOReturnSuccess;
    if(adreno830_annibale())status=writeChecked(step,A8XX_CP_APERTURE_CNTL_HOST,value);   /* write, barrier, read-back */
    else if(!(status=write(step,A8XX_CP_APERTURE_CNTL_HOST,value)))status=barrier(step);
    if(!status)aperture_=value;
    return status;
}
IOReturn A830Cp::pipeWrite(uint16_t step,uint32_t pipe,uint32_t reg,uint32_t value)
{
    const IOReturn status=aperture(step,pipe);
    return status?status:write(step,reg,value);
}
IOReturn A830Cp::pipeRead(uint16_t step,uint32_t pipe,uint32_t reg,uint32_t *value)
{
    *value=0;
    const IOReturn status=aperture(step,pipe);
    return status?status:read(step,reg,value);
}
/* The device MMIO lock; entering it forgets the cached aperture, so the first pipe access of every locked operation writes
 * CP_APERTURE_CNTL_HOST even after a GX power cycle reset the register behind the cache. */
void A830Cp::lock()
{
    if(io_.lock)io_.lock(io_.cookie);
    aperture_=ApertureUnknown;
}
void A830Cp::unlock()
{
    if(io_.unlock)io_.unlock(io_.cookie);
}
uint64_t A830Cp::now() const {return io_.now_us(io_.cookie);}

IOReturn A830Cp::attach(const A830CpIo *io,NativeAdrenoRing *ring,const A830CpSmmu *smmu)
{
    if(attached_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !io->sleep_us || !io->now_us || !ring ||
        (io->lock==nullptr)!=(io->unlock==nullptr) || (smmu && (!smmu->lpacRoute || !smmu->recordSticky)))return kIOReturnBadArgument;
    io_=*io;
    if(!io_.delay_us)io_.delay_us=io_.sleep_us;
    smmu_=smmu?*smmu:A830CpSmmu{};
    ring_=ring;failure_={};stall_={};record_={};
    aperture_=ApertureUnknown;gmem_protect_=0;pwrup_patched_=false;io_error_=false;secure_=true;
    attached_=true;
    return kIOReturnSuccess;
}
void A830Cp::setHealth(const A830CpHealth *health)
{
    health_=health?*health:A830CpHealth{};
}
/* annibale's status check (gpu_status_ok; kernel-sequences 9.2 row 34): no fault on the GMU side (A830CpHealth), no GPU
 * error bit in RBBM_INT_0_STATUS, no pipe fault in CP_INTERRUPT_STATUS_GLOBAL; any of them is fatal at once. */
IOReturn A830Cp::checkStatus(uint16_t step)
{
    if(health_.gmu_fault) {
        const int fault=health_.gmu_fault(health_.cookie);
        if(fault)return fail(step,A830ReasonGmuFault,0,uint32_t(fault),0,kIOReturnIOError);
    }
    uint32_t irq=0,cp=0;
    IOReturn s=read(step,A8XX_RBBM_INT_0_STATUS,&irq);
    if(!s)s=read(step,A8XX_CP_INTERRUPT_STATUS_GLOBAL,&cp);
    if(s)return s;
    stall_.int0_status=irq;stall_.global_status=cp;
    if(irq&AnnibaleRbbmErrors)
        return fail(step,(irq&A8XX_RBBM_INT_0_HANG_DETECT)?A830ReasonHang:A830ReasonCpFault,A8XX_RBBM_INT_0_STATUS,irq,0,
                    kIOReturnIOError);
    if(cp&AnnibaleCpErrors)return fail(step,A830ReasonCpFault,A8XX_CP_INTERRUPT_STATUS_GLOBAL,cp,0,kIOReturnIOError);
    return kIOReturnSuccess;
}
/* annibale's sGFSR sample through A830CpHealth (0 without the hook); a read that does not complete is fatal. */
IOReturn A830Cp::globalFault(uint16_t step,uint32_t *sgfsr)
{
    *sgfsr=0;
    if(!health_.global_fault || !health_.global_fault(health_.cookie,sgfsr))return kIOReturnSuccess;
    *sgfsr=0;
    return fail(step,A830ReasonSmmuFault,SgfsrDword,0,0,kIOReturnIOError);
}

/* ARCHITECTURE 4.8, steps 1-17 (Linux hw_init order, KGSL gen8_start values). The annibale profile writes the subset annibale
 * proved, in its order (kernel-sequences 9.2 rows 1-31, the direct ring; section 14 row 29): the halt releases read back
 * strictly, RBBM_INT_0_MASK = 0 right after them, steps 3-15 without the eight extra writes of the design (UCHE CCHE trap and
 * write-through bases, RBBM and slice perfcounter enables, CP_PERFCTR_CP_SEL[0], bicubic TABLE_0, the GMEM save/restore
 * enable, RBBM_INT_CLEAR_CMD), the CP global mask, the SECVID range last. Step 9a follows config.llcc_scid in both profiles
 * (0 unless pagani-gpu-llcc=1 activated the slices). */
IOReturn A830Cp::initLocked(const A830HwInitConfig &config)
{
    IOReturn s=kIOReturnSuccess;
    uint32_t value=0;
    const bool annibale=adreno830_annibale();
#define A830_TRY(expr) do {if((s=(expr)))return s;} while(0)
    /* 1. Aperture to 0 (forced), then the GBIF halts cleared in case GX was not collapsed; each cleared halt read back once
     *    (design: recorded, Linux's posting read; annibale: after a barrier, and it must be 0). */
    A830_TRY(aperture(A830CpStepClearHalt,A8XX_PIPE_NONE));
    const uint32_t halts[2]={A8XX_GBIF_HALT,A8XX_RBBM_GBIF_HALT};
    uint32_t *const halted[2]={&record_.gbif_halt,&record_.rbbm_gbif_halt};
    for(unsigned i=0;i<2;++i) {
        A830_TRY(write(A830CpStepClearHalt,halts[i],0));
        if(annibale)A830_TRY(barrier(A830CpStepClearHalt));
        A830_TRY(read(A830CpStepClearHalt,halts[i],halted[i]));
        if(annibale && *halted[i])return fail(A830CpStepClearHalt,A830ReasonReadback,halts[i],*halted[i],0,kIOReturnIOError);
    }
    /* annibale: every GPU interrupt masked before anything else, its waits poll (9.2 row 3; section 14 rows 29, 36). */
    if(annibale)A830_TRY(write(A830CpStepIrqMasks,A8XX_RBBM_INT_0_MASK,0));
    /* 2. No secure rendering: the trusted range off (Linux; KGSL would program its secure window). annibale writes it last. */
    auto secvid=[&]()->IOReturn {
        IOReturn r=write(A830CpStepSecvid,A8XX_RBBM_SECVID_TSB_CNTL,0);
        if(!r)r=write64(A830CpStepSecvid,A8XX_RBBM_SECVID_TSB_TRUSTED_BASE,0);
        return r?r:write(A830CpStepSecvid,A8XX_RBBM_SECVID_TSB_TRUSTED_SIZE,0);
    };
    if(!annibale)A830_TRY(secvid());
    /* 3. Every block counts into the GPU busy counter. */
    A830_TRY(write(A830CpStepBusyMask,A8XX_RBBM_PERFCTR_GPU_BUSY_MASKED,A830_RBBM_PERFCTR_GPU_BUSY_MASKED_VALUE));
    /* 4. The GMEM window as UCHE and SP see it (64 MiB). */
    A830_TRY(write64(A830CpStepGmemRange,A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN,config.gmem_base));
    A830_TRY(write64(A830CpStepGmemRange,A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN,config.gmem_base));
    /* 5. The UCHE trap region one page below the top of the 49-bit space; write-through base = trap base (no L2 bypass).
     *    The CCHE copies are the design's only. */
    A830_TRY(write64(A830CpStepTrapBase,A8XX_UCHE_TRAP_BASE,config.uche_trap_base));
    A830_TRY(write64(A830CpStepTrapBase,A8XX_UCHE_WRITE_THRU_BASE,config.uche_trap_base));
    if(!annibale) {
        A830_TRY(write64(A830CpStepTrapBase,A8XX_UCHE_CCHE_TRAP_BASE,config.uche_trap_base));
        A830_TRY(write64(A830CpStepTrapBase,A8XX_UCHE_CCHE_WRITE_THRU_BASE,config.uche_trap_base));
    }
    /* 6. Performance counters on, the IFPC countable on GMU XOCLK_1, CP counter 0 counts cycles (annibale: XOCLK_1 only). */
    if(!annibale) {
        A830_TRY(write(A830CpStepCounters,A8XX_RBBM_PERFCTR_CNTL,A830_RBBM_PERFCTR_CNTL_VALUE));
        A830_TRY(write(A830CpStepCounters,A8XX_RBBM_SLICE_PERFCTR_CNTL,A830_RBBM_PERFCTR_CNTL_VALUE));
    }
    A830_TRY(write(A830CpStepCounters,A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_1,A830_GMU_PWRCNT_XOCLK1_IFPC));
    if(!annibale)A830_TRY(write(A830CpStepCounters,A8XX_CP_PERFCTR_CP_SEL(0),A830_CP_PERFCTR_CP_SEL0_VALUE));
    /* 7. UBWC (ARCHITECTURE 4.14.3; KGSL gen8_start encoding): BV and BR GRAS, BR RB, then the global SP/TPL1 copies. */
    {
        uint32_t mode=0,amsbc=0,rgb565=0,fp16=0,rgba8888=0;
        switch(config.ubwc_mode) {
        case 5:amsbc=1;rgb565=1;mode=4;break;
        case 4:amsbc=1;rgb565=1;fp16=1;rgba8888=1;mode=2;break;
        case 3:amsbc=1;mode=1;break;
        default:break;
        }
        const uint32_t hbb=config.highest_bank_bit-13,hbbLo=hbb&3,hbbHi=(hbb>>2)&1,mal=config.min_access_length==64?1:0;
        const uint32_t gras=(hbb&0xf)<<5,ccu=hbbHi<<3|hbbLo<<1,
            cmp=mode<<15|rgba8888<<4|fp16<<3|rgb565<<2|amsbc<<1|mal,
            sp=hbbHi<<10|A830_UBWC_UAVFLAGPRD_INV<<4|mal<<3|hbbLo<<1,tpl1=hbbHi<<4|mal<<3|hbbLo<<1;
        A830_TRY(pipeWrite(A830CpStepUbwc,A8XX_PIPE_BV,A8XX_GRAS_NC_MODE_CNTL,gras));
        A830_TRY(pipeWrite(A830CpStepUbwc,A8XX_PIPE_BR,A8XX_GRAS_NC_MODE_CNTL,gras));
        A830_TRY(pipeWrite(A830CpStepUbwc,A8XX_PIPE_BR,A8XX_RB_CCU_NC_MODE_CNTL,ccu));
        A830_TRY(pipeWrite(A830CpStepUbwc,A8XX_PIPE_BR,A8XX_RB_CMP_NC_MODE_CNTL,cmp));
        A830_TRY(aperture(A830CpStepUbwc,A8XX_PIPE_NONE));
        A830_TRY(write(A830CpStepUbwc,A8XX_SP_NC_MODE_CNTL,sp));
        A830_TRY(write(A830CpStepUbwc,A8XX_TPL1_NC_MODE_CNTL,tpl1));
    }
    /* 8. Bicubic filter weights 0..19 (annibale: 1..19, TABLE_0 is never written). */
    for(uint32_t i=annibale?1:0;i<A830_HWINIT_BICUBIC_COUNT;++i)
        A830_TRY(write(A830CpStepBicubic,A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(i),A830HwInitBicubic[i]));
    /* 9. UCHE client prefetch. */
    A830_TRY(write(A830CpStepClientPf,A8XX_UCHE_CLIENT_PF,A830_UCHE_CLIENT_PF_VALUE));
    /* 9a. (M11) the GPU blocks' LLCC sub-cache id, only when the GPU slice is active (KGSL _llc_configure_gpu_scid);
     *     rewritten at every GX power-up because these registers do not survive it. */
    if(config.llcc_scid) {
        A830_TRY(write(A830CpStepScache,A8XX_GBIF_SCACHE_CNTL1,A8XX_GBIF_SCACHE_CNTL1_SCID(config.llcc_scid)));
        A830_TRY(write(A830CpStepScache,A8XX_GBIF_SCACHE_CNTL0,A8XX_GBIF_SCACHE_CNTL0_SCID(config.llcc_scid)));
    }
    /* 10. The non-context table pipe by pipe: PIPE_NONE entries with the aperture at 0, then BR, BV, AQE0/1, DDE_BR/BV. */
    for(uint32_t pipe=A8XX_PIPE_NONE;pipe<=A8XX_PIPE_DDE_BV;++pipe) {
        if(!hwInitPipe(pipe))continue;
        for(uint32_t i=0;i<A830_HWINIT_NONCTXT_COUNT;++i) {
            const A830HwInitPipeReg &entry=A830HwInitNonctxt[i];
            if(!(entry.pipes&A830_HWINIT_PIPE(pipe)))continue;
            if(entry.offset==A8XX_RB_GC_GMEM_PROTECT)gmem_protect_=entry.value;
            A830_TRY(pipeWrite(A830CpStepNonctxt,pipe,entry.offset,entry.value));
        }
    }
    A830_TRY(aperture(A830CpStepNonctxt,A8XX_PIPE_NONE));
    /* 11. Hardware hang detection: enable bit 30 with 0xCFFFFF cycles; the slice copy enabled. */
    A830_TRY(write(A830CpStepHangDetect,A8XX_RBBM_INTERFACE_HANG_INT_CNTL,A830_RBBM_INTERFACE_HANG_INT_CNTL_VALUE));
    A830_TRY(write(A830CpStepHangDetect,A8XX_RBBM_SLICE_INTERFACE_HANG_INT_CNTL,A830_RBBM_SLICE_INTERFACE_HANG_INT_CNTL_VALUE));
    /* 12. GMU counter 0 counts GPU busy (DCVS input), the power counters on (also written in the OOB window, 4.7.8). */
    A830_TRY(write(A830CpStepGmuCounters,A8XX_GMUAO_GPU_CX_BUSY_MASK,A830_GMU_CX_BUSY_MASK_VALUE));
    A830_TRY(read(A830CpStepGmuCounters,A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0,&value));
    A830_TRY(write(A830CpStepGmuCounters,A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0,
                   (value&~A830_GMU_PWRCNT_SELECT_MASK)|A830_GMU_PWRCNT_XOCLK0_GPU_BUSY));
    A830_TRY(write(A830CpStepGmuCounters,A8XX_GMUCX_POWER_COUNTER_ENABLE,A830_GMU_POWER_COUNTER_ENABLE_VALUE));
    /* 13. CP register protection: BR and BV control, the global table (0..46 and 63), the last span on CP_PROTECT_PIPE[15]. */
    A830_TRY(pipeWrite(A830CpStepProtect,A8XX_PIPE_BR,A8XX_CP_PROTECT_CNTL_PIPE,A830_CP_PROTECT_CNTL_VALUE));
    A830_TRY(pipeWrite(A830CpStepProtect,A8XX_PIPE_BV,A8XX_CP_PROTECT_CNTL_PIPE,A830_CP_PROTECT_CNTL_VALUE));
    A830_TRY(aperture(A830CpStepProtect,A8XX_PIPE_NONE));
    for(uint32_t i=0;i<A830_HWINIT_PROTECT_COUNT;++i)
        A830_TRY(write(A830CpStepProtect,A8XX_CP_PROTECT_GLOBAL(A830HwInitProtectRegs[i].index),A830HwInitProtectRegs[i].value));
    A830_TRY(pipeWrite(A830CpStepProtect,A8XX_PIPE_BR,A8XX_CP_PROTECT_PIPE(15),A830_HWINIT_PROTECT_LAST_VALUE));
    A830_TRY(pipeWrite(A830CpStepProtect,A8XX_PIPE_BV,A8XX_CP_PROTECT_PIPE(15),A830_HWINIT_PROTECT_LAST_VALUE));
    A830_TRY(aperture(A830CpStepProtect,A8XX_PIPE_NONE));
    /* 14. GMEM save/restore for preemption (Linux writes it unconditionally; harmless without preemption). Not annibale's. */
    if(!annibale) {
        A830_TRY(pipeWrite(A830CpStepGmemSaveRestore,A8XX_PIPE_BR,A8XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE,
                           A830_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_VALUE));
        A830_TRY(aperture(A830CpStepGmemSaveRestore,A8XX_PIPE_NONE));
    }
    /* 15. Per pipe: privileged CP accesses (BR also the crash dumper), the software and hardware fault interrupt masks. */
    for(uint32_t pipe=PipeFirst;pipe<=PipeLast;++pipe) {
        if(!hwInitPipe(pipe))continue;
        A830_TRY(pipeWrite(A830CpStepPipeMasks,pipe,A8XX_CP_APRIV_CNTL_PIPE,pipe==A8XX_PIPE_BR?A830_CP_APRIV_BR_VALUE:A830_CP_APRIV_VALUE));
        A830_TRY(pipeWrite(A830CpStepPipeMasks,pipe,A8XX_CP_INTERRUPT_STATUS_MASK_PIPE,A830_CP_INTERRUPT_STATUS_MASK_PIPE_VALUE));
        A830_TRY(pipeWrite(A830CpStepPipeMasks,pipe,A8XX_CP_HW_FAULT_STATUS_MASK_PIPE,A830_CP_HW_FAULT_STATUS_MASK_PIPE_VALUE));
    }
    A830_TRY(aperture(A830CpStepPipeMasks,A8XX_PIPE_NONE));
    /* 16. Interrupts: stale status cleared, the CP global and RBBM masks set. GIC 332 is wired only in S8, so a level here
     *     stays pending inside the GPU. annibale: the CP global mask only (RBBM_INT_0_MASK stays 0, nothing is cleared),
     *     then the SECVID range of step 2. */
    if(!annibale)A830_TRY(write(A830CpStepIrqMasks,A8XX_RBBM_INT_CLEAR_CMD,0xffffffffu));
    A830_TRY(write(A830CpStepIrqMasks,A8XX_CP_INTERRUPT_STATUS_MASK_GLOBAL,A830_CP_INTERRUPT_STATUS_MASK_GLOBAL_VALUE));
    if(!annibale)A830_TRY(write(A830CpStepIrqMasks,A8XX_RBBM_INT_0_MASK,A8XX_INT_MASK));
    else A830_TRY(secvid());
    /* 17. CP_SMMU_APERTURE is the SCM call of S3 (and of the recovery), not a register here. */
    return barrier(A830CpStepIrqMasks);
#undef A830_TRY
}
IOReturn A830Cp::hwInit(const A830HwInitConfig &config)
{
    if(!attached_)return kIOReturnNotReady;
    if(config.highest_bank_bit<13 || config.highest_bank_bit>20 ||
        (config.min_access_length!=32 && config.min_access_length!=64) || !config.gmem_base || (config.gmem_base&0xfff) ||
        (config.uche_trap_base&0xfff) || config.llcc_scid>0x3f)
        return fail(A830CpStepClearHalt,A830ReasonArgument,0,config.highest_bank_bit,A830_HIGHEST_BANK_BIT,kIOReturnBadArgument);
    io_error_=false;
    lock();
    IOReturn status=initLocked(config);
    if(status) {
        const A830CpFailure first=failure_;
        (void)aperture(A830CpStepClearHalt,A8XX_PIPE_NONE);   /* best effort: never leave a pipe selected */
        failure_=first;                                       /* the report keeps the failure that stopped hw_init */
    }
    unlock();
    /* annibale: hw_init ends with its status check (9.2 row 34); global registers and the GMU hook, so outside the lock. */
    if(!status && adreno830_annibale())status=checkStatus(A830CpStepHealth);
    if(!status) {++record_.hw_inits;secure_=true;}
    return status;
}
void A830Cp::writePipe(uint8_t pipe,uint32_t reg,uint32_t value)
{
    if(!attached_ || pipe>A8XX_PIPE_DDE_BV)return;
    lock();
    if(!pipeWrite(A830CpStepNonctxt,pipe,reg,value))(void)aperture(A830CpStepNonctxt,A8XX_PIPE_NONE);
    unlock();
}
IOReturn A830Cp::readPipe(uint8_t pipe,uint32_t reg,uint32_t *value)
{
    if(!value)return kIOReturnBadArgument;
    *value=0;
    if(!attached_)return kIOReturnNotReady;
    if(pipe>A8XX_PIPE_DDE_BV)return kIOReturnBadArgument;
    lock();
    IOReturn status=pipeRead(A830CpStepNonctxt,pipe,reg,value);
    const IOReturn back=aperture(A830CpStepNonctxt,A8XX_PIPE_NONE);
    unlock();
    return status?status:back;
}
IOReturn A830Cp::stageMicrocode(const uint8_t *fw,uint64_t fw_bytes,void *dst,size_t dst_bytes,size_t *copied)
{
    if(copied)*copied=0;
    if(!fw || !dst || fw_bytes<=A830_FW_HEADER_BYTES || fw_bytes-A830_FW_HEADER_BYTES>dst_bytes || (fw_bytes&3))
        return kIOReturnBadArgument;
    const size_t image=size_t(fw_bytes-A830_FW_HEADER_BYTES);
    memcpy(dst,fw+A830_FW_HEADER_BYTES,image);
    memset(static_cast<uint8_t *>(dst)+image,0,dst_bytes-image);
    if(copied)*copied=image;
    return kIOReturnSuccess;
}
IOReturn A830Cp::loadMicrocode(uint64_t sqe_iova,uint64_t aqe_iova)
{
    if(!attached_)return kIOReturnNotReady;
    if(!sqe_iova || (sqe_iova&4095) || sqe_iova>=(1ull<<48) || (aqe_iova&4095) || aqe_iova>=(1ull<<48))
        return fail(A830CpStepMicrocode,A830ReasonArgument,A8XX_CP_SQE_INSTR_BASE,sqe_iova,A830_GPU_VA_SQE,kIOReturnBadArgument);
    if(adreno830_annibale()) {
        /* annibale (9.2 rows 35-36, gpu_ucode_base): each base written, a barrier, both halves read back equal. */
        auto base=[this](uint32_t reg,uint64_t iova)->IOReturn {
            IOReturn r=write64(A830CpStepMicrocode,reg,iova);
            if(!r)r=barrier(A830CpStepMicrocode);
            for(uint32_t half=0;!r && half<2;++half) {
                const uint32_t want=uint32_t(iova>>(32*half));
                uint32_t seen=0;
                r=read(A830CpStepMicrocode,reg+half,&seen);
                if(!r && seen!=want)r=fail(A830CpStepMicrocode,A830ReasonReadback,reg+half,seen,want,kIOReturnIOError);
            }
            return r;
        };
        const IOReturn sqe=base(A8XX_CP_SQE_INSTR_BASE,sqe_iova);
        return sqe || !aqe_iova?sqe:base(A8XX_CP_AQE_INSTR_BASE_0,aqe_iova);
    }
    IOReturn status=write64(A830CpStepMicrocode,A8XX_CP_SQE_INSTR_BASE,sqe_iova);
    if(!status && aqe_iova)status=write64(A830CpStepMicrocode,A8XX_CP_AQE_INSTR_BASE_0,aqe_iova);
    return status?status:barrier(A830CpStepMicrocode);
}
/* KGSL gen8_rb_start order: the BR and BV read pointer shadows, the control word, the base. CP_RB_RPTR and CP_RB_WPTR are then
 * only recorded: KGSL never checks the hardware pointers here (ARCHITECTURE 4.9, 10 #1).
 * annibale (kernel-sequences 9.7 steps 3-4; section 14 row 32): the pointers are read first and must both be 0 (a cold CP),
 * then the base, the control word and the BR and BV shadow addresses, every half written, fenced and read back equal. */
IOReturn A830Cp::startRing(uint64_t ring_iova,uint64_t ring_bytes,uint64_t memptrs_iova)
{
    if(!attached_ || !ring_->attached())return kIOReturnNotReady;
    if(ring_iova!=ring_->ringIova() || ring_bytes!=ring_->ringBytes() || memptrs_iova!=ring_->memptrsIova())
        return fail(A830CpStepRing,A830ReasonArgument,A8XX_CP_RB_BASE,ring_iova,ring_->ringIova(),kIOReturnBadArgument);
    if(ring_->writeIndex() || ring_->poisoned())   /* only a fresh ring (attach, or recover after a reset) */
        return fail(A830CpStepRing,A830ReasonUnexpectedState,A8XX_CP_RB_WPTR,ring_->writeIndex(),0,kIOReturnNotReady);
    uint32_t log2=0;
    for(uint64_t q=ring_bytes/8;q>1;q>>=1)++log2;
    uint32_t block=0;
    for(uint32_t q=A830_RING_BLOCK_BYTES/8;q>1;q>>=1)++block;
    const NativeAdrenoRingRegs &regs=ring_->regs();
    const uint32_t cntl=A8XX_CP_RB_CNTL_BUFSZ(log2)|A8XX_CP_RB_CNTL_BLKSZ(block);
    const uint64_t rptr=memptrs_iova+A830_MEMPTRS_RPTR,bv=memptrs_iova+A830_MEMPTRS_BV_RPTR;
    IOReturn s=kIOReturnSuccess;
    if(adreno830_annibale()) {
        s=read(A830CpStepRing,regs.rptr,&record_.rptr_at_start);
        if(!s)s=read(A830CpStepRing,regs.wptr,&record_.wptr_at_start);
        if(s)return s;
        if(record_.rptr_at_start || record_.wptr_at_start)
            return fail(A830CpStepRing,A830ReasonUnexpectedState,record_.rptr_at_start?regs.rptr:regs.wptr,
                        record_.rptr_at_start|uint64_t(record_.wptr_at_start)<<32,0,kIOReturnNotReady);
        const uint32_t sequence[7][2]={{regs.base,uint32_t(ring_iova)},{regs.base+1,uint32_t(ring_iova>>32)},{regs.cntl,cntl},
            {regs.rptr_addr,uint32_t(rptr)},{regs.rptr_addr+1,uint32_t(rptr>>32)},
            {regs.bv_rptr_addr,uint32_t(bv)},{regs.bv_rptr_addr+1,uint32_t(bv>>32)}};
        const uint32_t count=regs.bv_rptr_addr?7:5;
        for(uint32_t i=0;!s && i<count;++i)s=writeChecked(A830CpStepRing,sequence[i][0],sequence[i][1]);
        return s;
    }
    s=write64(A830CpStepRing,regs.rptr_addr,rptr);
    if(!s && regs.bv_rptr_addr)s=write64(A830CpStepRing,regs.bv_rptr_addr,bv);
    if(!s)s=write(A830CpStepRing,regs.cntl,cntl);
    if(!s)s=write64(A830CpStepRing,regs.base,ring_iova);
    if(!s)s=barrier(A830CpStepRing);
    if(!s)s=read(A830CpStepRing,regs.rptr,&record_.rptr_at_start);
    if(!s)s=read(A830CpStepRing,regs.wptr,&record_.wptr_at_start);
    return s;
}
IOReturn A830Cp::idleState(uint32_t target,bool *idle)
{
    uint32_t rptr=0,status=0,irq=0;
    *idle=false;
    IOReturn s=read(A830CpStepMeInit,A8XX_CP_RB_RPTR,&rptr);
    if(!s)s=read(A830CpStepMeInit,A8XX_RBBM_STATUS,&status);
    if(!s)s=read(A830CpStepMeInit,A8XX_RBBM_INT_0_STATUS,&irq);
    if(s)return s;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t shadow=ring_->memptrs()[A830_MEMPTRS_RPTR/4];
    stall_.rptr=rptr;stall_.shadow_rptr=shadow;stall_.rbbm_status=status;stall_.int0_status=irq;
    *idle=rptr==target && shadow==target && !(status&A8XX_RBBM_STATUS_BUSY_MASK) && !(irq&A8XX_RBBM_INT_0_HANG_DETECT);
    return kIOReturnSuccess;
}
IOReturn A830Cp::waitIdle(uint32_t timeout_us)
{
    return idle(timeout_us,A830CpStepMeInit);
}
/* annibale's wait (kernel-sequences 9.7 step 9; section 14 row 33): every poll runs the status check, then reads both read
 * pointers and their memptrs shadows, which must lie inside the ring; the GPU is idle when the BR pointer and its shadow
 * equal the write index, GPU_CX_BUSY_STATUS bit 23 is clear and RBBM_STATUS has no bit but bit 0, confirmed by one more
 * status check. A 100 us sleep follows every poll that was not idle; polls of them at most. */
IOReturn A830Cp::idleAnnibale(uint32_t polls,uint16_t step)
{
    const uint32_t target=ring_->writeIndex(),words=uint32_t(ring_->ringBytes()/4);
    const uint64_t start=now();
    bool reached=false;
    for(uint32_t poll=0;poll<polls;++poll) {
        uint32_t rptr=0,bv=0;
        IOReturn s=checkStatus(step);
        if(!s)s=read(step,A8XX_CP_RB_RPTR,&rptr);
        if(!s)s=read(step,A8XX_CP_RB_RPTR_BV,&bv);
        if(s)return s;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const volatile uint32_t *memptrs=ring_->memptrs();
        const uint32_t shadow=memptrs[A830_MEMPTRS_RPTR/4],bvShadow=memptrs[A830_MEMPTRS_BV_RPTR/4];
        stall_.rptr=rptr;stall_.bv_rptr=bv;stall_.shadow_rptr=shadow;stall_.shadow_bv_rptr=bvShadow;
        if(rptr>=words || shadow>=words)
            return fail(step,A830ReasonUnexpectedState,A8XX_CP_RB_RPTR,rptr|uint64_t(shadow)<<32,words,kIOReturnIOError);
        if(bv>=words || bvShadow>=words)
            return fail(step,A830ReasonUnexpectedState,A8XX_CP_RB_RPTR_BV,bv|uint64_t(bvShadow)<<32,words,kIOReturnIOError);
        reached=rptr==target && shadow==target;
        if(reached) {
            s=read(step,A8XX_GMUAO_GPU_CX_BUSY_STATUS,&stall_.cx_busy);
            if(!s && !(stall_.cx_busy&AnnibaleCxBusy))s=read(step,A8XX_RBBM_STATUS,&stall_.rbbm_status);
            if(s)return s;
            if(!(stall_.cx_busy&AnnibaleCxBusy) && !(stall_.rbbm_status&A8XX_RBBM_STATUS_BUSY_MASK)) {
                if((s=checkStatus(step)))return s;
                const uint64_t spent=now()-start;
                record_.idle_us=uint32_t(spent>0xffffffffull?0xffffffffull:spent);
                record_.cx_busy_idle=stall_.cx_busy;
                return kIOReturnSuccess;
            }
        }
        io_.sleep_us(io_.cookie,A830_POLL_STEP_US);
    }
    /* The criterion that failed: the read pointers, then GPU_CX_BUSY_STATUS, then RBBM_STATUS. */
    if(!reached)
        return fail(step,A830ReasonTimeout,A8XX_CP_RB_RPTR,stall_.rptr|uint64_t(stall_.shadow_rptr)<<32,target,kIOReturnTimeout);
    if(stall_.cx_busy&AnnibaleCxBusy)
        return fail(step,A830ReasonTimeout,A8XX_GMUAO_GPU_CX_BUSY_STATUS,stall_.cx_busy,0,kIOReturnTimeout);
    return fail(step,A830ReasonTimeout,A8XX_RBBM_STATUS,stall_.rbbm_status,0,kIOReturnTimeout);
}
IOReturn A830Cp::idle(uint32_t timeout_us,uint16_t step)
{
    if(!attached_ || !ring_->attached())return kIOReturnNotReady;
    if(adreno830_annibale()) {   /* polls of 100 us, at least one */
        const uint64_t polls=(uint64_t(timeout_us)+A830_POLL_STEP_US-1)/A830_POLL_STEP_US;
        return idleAnnibale(polls?uint32_t(polls):1u,step);
    }
    const uint32_t target=ring_->writeIndex();
    const uint64_t start=now();
    for(;;) {
        bool idle=false;
        const IOReturn s=idleState(target,&idle);
        if(s)return s;
        const uint64_t spent=now()-start;
        if(idle) {
            record_.idle_us=uint32_t(spent>0xffffffffull?0xffffffffull:spent);
            (void)io_.read32(io_.cookie,A8XX_GMUAO_GPU_CX_BUSY_STATUS,&record_.cx_busy_idle);   /* record only (4.9) */
            return kIOReturnSuccess;
        }
        if(spent>=timeout_us) {
            /* The criterion that failed: the read pointers, then the busy bits, then the hang bit. */
            if(stall_.rptr!=target || stall_.shadow_rptr!=target)
                return fail(step,A830ReasonTimeout,A8XX_CP_RB_RPTR,stall_.rptr|uint64_t(stall_.shadow_rptr)<<32,target,
                            kIOReturnTimeout);
            if(stall_.int0_status&A8XX_RBBM_INT_0_HANG_DETECT)
                return fail(step,A830ReasonHang,A8XX_RBBM_INT_0_STATUS,stall_.int0_status,0,kIOReturnTimeout);
            return fail(step,A830ReasonTimeout,A8XX_RBBM_STATUS,stall_.rbbm_status,0,kIOReturnTimeout);
        }
        const uint64_t left=timeout_us-spent;
        io_.sleep_us(io_.cookie,uint32_t(left<A830_POLL_STEP_US?left:A830_POLL_STEP_US));
    }
}
IOReturn A830Cp::startCp(uint64_t pwrup_iova,uint32_t *fence)
{
    if(!fence)return kIOReturnBadArgument;
    *fence=0;
    if(!attached_ || !ring_->attached() || ring_->poisoned())return kIOReturnNotReady;
    if(!pwrup_iova || (pwrup_iova&3) || pwrup_iova>=(1ull<<48))
        return fail(A830CpStepSqeStart,A830ReasonArgument,0,pwrup_iova,A830_GPU_VA_PWRUP,kIOReturnBadArgument);
    if(ring_->writeIndex())   /* CP_ME_INIT must be the first packet the SQE fetches */
        return fail(A830CpStepSqeStart,A830ReasonUnexpectedState,A8XX_CP_RB_WPTR,ring_->writeIndex(),0,kIOReturnNotReady);
    /* The only functional gate before the SQE starts in the design: the LPAC stream is routed (a missing route parks BR/BV at
     * SQE PC 0xE3/0x4A). Sticky SMMU fault status is recorded, never a precondition. */
    if(smmu_.lpacRoute) {
        uint32_t readback=0;
        const IOReturn route=smmu_.lpacRoute(smmu_.cookie,&readback);
        record_.lpac_route=readback;
        smmu_.recordSticky(smmu_.cookie);
        if(route)return fail(A830CpStepLpacRoute,A830ReasonSmmuRoute,0,readback,A830_SMMU_S2CR_TRANS_NSH(A830_SMMU_CB_GPU),route);
    }
    /* annibale (9.7 steps 5-6; section 14 row 32): sGFSR before the SQE start and again after its barrier; a bit already set
     * is only recorded (Gunyah may keep a stale one), a bit that appeared with the start is fatal. CP_SQE_CNTL must then read
     * back 1, before anything reaches the ring. The design records the read-back only. */
    const bool annibale=adreno830_annibale();
    IOReturn s=annibale?globalFault(A830CpStepSqeStart,&record_.sgfsr_before):kIOReturnSuccess;
    if(!s)s=write(A830CpStepSqeStart,A8XX_CP_SQE_CNTL,A830_CP_SQE_CNTL_START);
    if(!s)s=barrier(A830CpStepSqeStart);
    if(!s && annibale && !(s=globalFault(A830CpStepSqeStart,&record_.sgfsr_after)) &&
        (record_.sgfsr_after&~record_.sgfsr_before))
        s=fail(A830CpStepSqeStart,A830ReasonSmmuFault,SgfsrDword,record_.sgfsr_after,record_.sgfsr_before,kIOReturnIOError);
    if(!s)s=read(A830CpStepSqeStart,A8XX_CP_SQE_CNTL,&record_.sqe_cntl_readback);
    if(!s && annibale && record_.sqe_cntl_readback!=A830_CP_SQE_CNTL_START)
        s=fail(A830CpStepSqeStart,A830ReasonReadback,A8XX_CP_SQE_CNTL,record_.sqe_cntl_readback,A830_CP_SQE_CNTL_START,
               kIOReturnIOError);
    if(s)return s;
    uint32_t words[A830_CP_INIT_RING_DWORDS]={0};
    if(AdrenoPM4Type7(A8XX_PM4_CP_THREAD_CONTROL,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(A8XX_PM4_CP_ME_INIT,A830_CP_ME_INIT_DWORDS,&words[2])!=AdrenoPM4Success)
        return fail(A830CpStepMeInit,A830ReasonArgument,0,0,0,kIOReturnIOError);
    words[1]=A830_CP_THREAD_CONTROL_INIT;
    words[3]=meInitMask();
    words[4]=A830_CP_ME_INIT_HW_CONTEXTS;
    words[5]=A830_CP_ME_INIT_ERROR_DETECT;
    words[6]=A830_CP_ME_INIT_OPERATION_MODE;
    words[7]=uint32_t(pwrup_iova);
    words[8]=uint32_t(pwrup_iova>>32);
    words[9]=A830_CP_ME_INIT_DYN_TRIPLETS;
    uint32_t wptr=0;
    s=ring_->submitRaw(words,A830_CP_INIT_RING_DWORDS,&wptr);
    if(s)return fail(A830CpStepMeInit,s==kIOReturnTimeout?A830ReasonTimeout:A830ReasonUnexpectedState,A8XX_CP_RB_WPTR,
                     ring_->writeIndex(),A830_CP_INIT_RING_DWORDS,s);
    *fence=wptr;
    s=idle(annibale?AnnibaleInitUs:A830_T_CP_IDLE_US,A830CpStepMeInit);   /* annibale 200 ms (2000 polls), design 2 s */
    if(!s)++record_.cp_starts;
    return s;
}
void A830Cp::dumpLocked()
{
    A830CpStall &d=stall_;
    d={};
    bool ok=true;
    const bool annibale=adreno830_annibale();
    auto get=[&](uint32_t reg,uint32_t *value) {if(io_.read32(io_.cookie,reg,value)) {*value=0;ok=false;}};
    get(A8XX_RBBM_STATUS,&d.rbbm_status);get(A8XX_RBBM_STATUS1,&d.rbbm_status1);get(A8XX_RBBM_INT_0_STATUS,&d.int0_status);
    get(A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW,&d.int0_unmasked);
    get(A8XX_CP_RB_RPTR,&d.rptr);get(A8XX_CP_RB_WPTR,&d.wptr);get(A8XX_CP_RB_RPTR_BV,&d.bv_rptr);
    get(A8XX_CP_RL_ERROR_DETAILS_0,&d.rl_error[0]);get(A8XX_CP_RL_ERROR_DETAILS_1,&d.rl_error[1]);
    get(A8XX_CP_CP2GMU_STATUS,&d.cp2gmu);get(A8XX_CP_INTERRUPT_STATUS_GLOBAL,&d.global_status);
    get(A8XX_GMUAO_GPU_CX_BUSY_STATUS,&d.cx_busy);
    if(annibale) {   /* the rest of annibale's timeout dump (kernel-sequences 9.7 step 10) */
        uint32_t lo=0,hi=0;
        get(A8XX_RBBM_GFX_STATUS,&d.gfx_status);get(A8XX_RBBM_GFX_BR_STATUS,&d.gfx_br_status);
        get(A8XX_RBBM_GFX_BV_STATUS,&d.gfx_bv_status);get(A8XX_RBBM_MISC_STATUS,&d.misc_status);
        get(A8XX_CP_SQE_INSTR_BASE,&lo);get(A8XX_CP_SQE_INSTR_BASE_HI,&hi);d.sqe_base=uint64_t(hi)<<32|lo;
        get(A8XX_CP_RB_BASE,&lo);get(A8XX_CP_RB_BASE_HI,&hi);d.rb_base=uint64_t(hi)<<32|lo;
        get(A8XX_CP_RB_CNTL,&d.rb_cntl);
    }
    if(ring_->attached()) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        d.shadow_rptr=ring_->memptrs()[A830_MEMPTRS_RPTR/4];d.shadow_bv_rptr=ring_->memptrs()[A830_MEMPTRS_BV_RPTR/4];
    }
    const uint32_t pipes[2]={A8XX_PIPE_BR,A8XX_PIPE_BV};
    for(unsigned p=0;p<2;++p) {
        A830CpStallPipe &pipe=d.pipe[p];
        if(aperture(A830CpStepMeInit,pipes[p])) {ok=false;break;}
        get(A8XX_CP_HW_FAULT_STATUS_PIPE,&pipe.hw_fault);get(A8XX_CP_INTERRUPT_STATUS_PIPE,&pipe.interrupt);
        get(A8XX_CP_PIPE_STATUS_PIPE,&pipe.pipe_status);get(A8XX_CP_SQE_STATUS_PIPE,&pipe.sqe_status);
        get(A8XX_CP_PROTECT_STATUS_PIPE,&pipe.protect_status);
        if(annibale)get(CpSqeIcacheCntlPipe,&pipe.icache_cntl);
        for(uint32_t i=0;i<SqeStatCount;++i) {
            if(io_.write32(io_.cookie,A8XX_CP_SQE_STAT_ADDR_PIPE,i)) {ok=false;break;}
            get(A8XX_CP_SQE_STAT_DATA_PIPE,&pipe.sqe_stat[i]);
        }
    }
    if(aperture(A830CpStepMeInit,A8XX_PIPE_NONE))ok=false;
    d.valid=ok;
}
void A830Cp::dumpStall(Adreno830Status *status)
{
    if(!attached_)return;
    const A830CpFailure kept=failure_;   /* a diagnostic dump never replaces the failure it documents */
    lock();
    dumpLocked();
    unlock();
    failure_=kept;
    /* annibale: sGFSR at the timeout (its gpu_global_fault_sample), outside the MMIO lock. */
    if(adreno830_annibale() && health_.global_fault && health_.global_fault(health_.cookie,&stall_.sgfsr)) {
        stall_.sgfsr=0;stall_.valid=false;
    }
    if(!status)return;
    const A830CpStall &d=stall_;
    status->aux[0]=d.rptr|uint64_t(d.wptr)<<32;
    status->aux[1]=d.rbbm_status|uint64_t(d.pipe[0].hw_fault)<<32;
    status->aux[2]=d.pipe[0].sqe_stat[A8XX_CP_SQE_STAT_PC]|uint64_t(d.pipe[1].sqe_stat[A8XX_CP_SQE_STAT_PC])<<32;
    status->aux[3]=d.pipe[0].protect_status|uint64_t(d.int0_unmasked)<<32;
}
IOReturn A830Cp::exitSecure(uint32_t timeout_us)
{
    if(!attached_ || !ring_->attached())return kIOReturnNotReady;
    uint32_t words[2]={0,0};
    if(AdrenoPM4Type7(A8XX_PM4_CP_SET_SECURE_MODE,1,&words[0])!=AdrenoPM4Success)
        return fail(A830CpStepSecureMode,A830ReasonArgument,0,0,0,kIOReturnIOError);
    uint32_t wptr=0;
    IOReturn s=ring_->submitRaw(words,2,&wptr);
    if(s)return fail(A830CpStepSecureMode,s==kIOReturnTimeout?A830ReasonTimeout:A830ReasonUnexpectedState,A8XX_CP_RB_WPTR,
                     ring_->writeIndex(),wptr,s);
    s=idle(timeout_us,A830CpStepSecureMode);
    if(s)return s;
    secure_=false;++record_.secure_exits;
    return kIOReturnSuccess;
}
/* Linux: GMEM_PROTECT must be programmed after the GPU left secure mode (the non-context table wrote it once before). */
IOReturn A830Cp::programGmemProtect()
{
    if(!attached_)return kIOReturnNotReady;
    const uint32_t value=gmem_protect_?gmem_protect_:A830_HWINIT_GMEM_PROTECT_VALUE;
    lock();
    IOReturn s=pipeWrite(A830CpStepGmemProtect,A8XX_PIPE_BR,A8XX_RB_GC_GMEM_PROTECT,value);
    const IOReturn back=aperture(A830CpStepGmemProtect,A8XX_PIPE_NONE);
    unlock();
    return s?s:back;
}
/* a8xx_patch_pwrup_reglist: the IFPC list, the pwrup list (global registers, aperture 0), then per pipe BR..DDE_BV the
 * dynamic entries with that pipe's bit as {pipe << 12, offset, value read through that aperture}. */
IOReturn A830Cp::patchPwrup(void *page)
{
    if(!attached_)return kIOReturnNotReady;
    if(!page)return kIOReturnBadArgument;
    static uint32_t ifpc[A830_HWINIT_IFPC_COUNT],pwrup[A830_HWINIT_PWRUP_COUNT],dynamic[A830_HWINIT_DYN_TRIPLETS*3];
    uint32_t triplets=0;
    lock();
    IOReturn s=aperture(A830CpStepPwrup,A8XX_PIPE_NONE);
    for(uint32_t i=0;!s && i<A830_HWINIT_IFPC_COUNT;++i)s=read(A830CpStepPwrup,A830HwInitIfpc[i],&ifpc[i]);
    for(uint32_t i=0;!s && i<A830_HWINIT_PWRUP_COUNT;++i)s=read(A830CpStepPwrup,A830HwInitPwrup[i],&pwrup[i]);
    for(uint32_t pipe=PipeFirst;!s && pipe<=PipeLast;++pipe)
        for(uint32_t i=0;!s && i<A830_HWINIT_DYN_COUNT;++i) {
            if(!(A830HwInitDynPwrup[i].pipes&A830_HWINIT_PIPE(pipe)))continue;
            if(triplets>=A830_HWINIT_DYN_TRIPLETS) {s=kIOReturnIOError;break;}
            dynamic[triplets*3]=A8XX_CP_APERTURE_CNTL_HOST_PIPE(pipe);
            dynamic[triplets*3+1]=A830HwInitDynPwrup[i].offset;
            s=pipeRead(A830CpStepPwrup,pipe,A830HwInitDynPwrup[i].offset,&dynamic[triplets*3+2]);
            ++triplets;
        }
    const IOReturn back=aperture(A830CpStepPwrup,A8XX_PIPE_NONE);
    unlock();
    if(s || back)return s?s:back;
    const A830PwrupTables tables={A830HwInitIfpc,ifpc,A830_HWINIT_IFPC_COUNT,A830HwInitPwrup,pwrup,A830_HWINIT_PWRUP_COUNT,
                                  dynamic,triplets};
    size_t written=0;
    s=NativeAdrenoPwrup::buildGen8(tables,page,PwrupPage,&written);
    if(s)return fail(A830CpStepPwrup,A830ReasonArgument,0,written,PwrupPage,s);
    s=barrier(A830CpStepPwrup);
    if(!s)pwrup_patched_=true;
    return s;
}
/* a8xx_set_hwcg(on) with the KGSL AO values; the P2S transfer is polled 10 x 1 us. A timeout is recorded only (the author's
 * build treated it as fatal, which it is not); like Linux and KGSL it then leaves RBBM_CLOCK_CNTL_GLOBAL at 1 (clocks forced
 * on, gating not loaded) instead of turning the override off over an incomplete transfer. The annibale profile posts the
 * trigger with a barrier and polls 3 x 1 us; without TXDONE the override also stays on, and the call fails (annibale 9.4). */
IOReturn A830Cp::enableHwcg()
{
    if(!attached_)return kIOReturnNotReady;
    const bool annibale=adreno830_annibale();
    IOReturn s=write(A830CpStepHwcg,A8XX_RBBM_CGC_0_PC,A830_RBBM_CGC_0_PC_VALUE);
    for(uint32_t i=0;!s && i<A830_HWINIT_AO_HWCG_COUNT;++i)
        s=write(A830CpStepHwcg,A830HwInitAoHwcg[i].offset,A830HwInitAoHwcg[i].value);
    if(!s)s=write(A830CpStepHwcg,A8XX_RBBM_CLOCK_CNTL_GLOBAL,1);
    if(!s)s=write(A830CpStepHwcg,A8XX_RBBM_CGC_GLOBAL_LOAD_CMD,1);
    if(!s)s=write(A830CpStepHwcg,A8XX_RBBM_CGC_P2S_TRIG_CMD,1);
    if(!s && annibale)s=barrier(A830CpStepHwcg);
    if(s)return s;
    uint32_t status=0;
    const uint32_t polls=annibale?AnnibaleP2sPolls:P2sPolls;
    for(uint32_t poll=0;poll<polls;++poll) {
        io_.delay_us(io_.cookie,1);
        if((s=read(A830CpStepHwcg,A8XX_RBBM_CGC_P2S_STATUS,&status)))return s;
        if(status&A8XX_RBBM_CGC_P2S_STATUS_TXDONE)break;
    }
    record_.p2s_status=status;
    if(!(status&A8XX_RBBM_CGC_P2S_STATUS_TXDONE)) {
        ++record_.hwcg_timeouts;
        return annibale?fail(A830CpStepHwcg,A830ReasonTimeout,A8XX_RBBM_CGC_P2S_STATUS,status,A8XX_RBBM_CGC_P2S_STATUS_TXDONE,
                             kIOReturnTimeout):kIOReturnSuccess;
    }
    return write(A830CpStepHwcg,A8XX_RBBM_CLOCK_CNTL_GLOBAL,0);
}
IOReturn A830Cp::disableHwcg()
{
    if(!attached_)return kIOReturnNotReady;
    IOReturn s=kIOReturnSuccess;
    for(uint32_t i=0;!s && i<A830_HWINIT_AO_HWCG_COUNT;++i)s=write(A830CpStepHwcg,A830HwInitAoHwcg[i].offset,0);
    if(!s)s=write(A830CpStepHwcg,A8XX_RBBM_CLOCK_CNTL_GLOBAL,1);
    if(!s)s=write(A830CpStepHwcg,A8XX_RBBM_CGC_GLOBAL_LOAD_CMD,0);
    uint32_t value=0;
    if(!s)s=read(A830CpStepHwcg,A8XX_GBIF_CX_CONFIG,&value);   /* the GMU enabled GBIF clock gating at boot */
    if(!s)s=write(A830CpStepHwcg,A8XX_GBIF_CX_CONFIG,value&~A8XX_GBIF_CX_CONFIG_CLOCK_GATING);
    return s;
}
IOReturn A830Cp::finishSecure(void *pwrup_page,uint32_t timeout_us)
{
    if(!attached_)return kIOReturnNotReady;
    IOReturn s=exitSecure(timeout_us);
    if(!s)s=programGmemProtect();
    if(!s && pwrup_page && !pwrup_patched_)s=patchPwrup(pwrup_page);
    if(s)return s;
    /* Step 11. annibale's first round keeps the inherited clock gating (section 14 row 35: fewer variables until the desktop
     * runs); the design turns it on. */
    return adreno830_annibale()?kIOReturnSuccess:enableHwcg();
}
IOReturn A830Cp::haltSqe()
{
    if(!attached_)return kIOReturnNotReady;
    const IOReturn s=write(A830CpStepHaltSqe,A8XX_CP_SQE_CNTL,A830_CP_SQE_CNTL_HALT);
    return s?s:barrier(A830CpStepHaltSqe);
}
IOReturn A830Cp::haltGbif(uint32_t timeout_us)
{
    if(!attached_)return kIOReturnNotReady;
    /* Spin 1 us steps first (the acknowledges normally come within a few), then 100 us sleeps up to timeout_us. */
    auto until=[&](uint16_t step,uint32_t reg,uint32_t bits,uint32_t *seen)->IOReturn {
        const uint64_t start=now();
        for(uint32_t poll=0;;++poll) {
            const IOReturn r=read(step,reg,seen);
            if(r)return r;
            if((*seen&bits)==bits)return kIOReturnSuccess;
            const uint64_t spent=now()-start;
            if(spent>=timeout_us)return fail(step,A830ReasonTimeout,reg,*seen,bits,kIOReturnTimeout);
            if(poll<SpinPolls)io_.delay_us(io_.cookie,1);
            else io_.sleep_us(io_.cookie,uint32_t(timeout_us-spent<A830_POLL_STEP_US?timeout_us-spent:A830_POLL_STEP_US));
        }
    };
    IOReturn first=kIOReturnSuccess;
    auto keep=[&first](IOReturn r) {if(r && !first)first=r;return r;};
    /* GX side first (its acknowledge needs GX on; a timeout is noted and the CX side still halts). */
    if(!keep(write(A830CpStepHaltGbifGx,A8XX_RBBM_GBIF_HALT,A8XX_RBBM_GBIF_HALT_GX)))
        (void)keep(until(A830CpStepHaltGbifGx,A8XX_RBBM_GBIF_HALT_ACK,A8XX_RBBM_GBIF_HALT_GX,&record_.gbif_ack[0]));
    if(!keep(write(A830CpStepHaltGbif,A8XX_GBIF_HALT,A8XX_GBIF_HALT_CLIENT)))
        (void)keep(until(A830CpStepHaltGbif,A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_CLIENT,&record_.gbif_ack[1]));
    if(!keep(write(A830CpStepHaltGbif,A8XX_GBIF_HALT,A8XX_GBIF_HALT_ARB)))
        (void)keep(until(A830CpStepHaltGbif,A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_ARB,&record_.gbif_ack[2]));
    (void)keep(write(A830CpStepHaltGbif,A8XX_GBIF_HALT,0));   /* the halt must be cleared explicitly */
    return first;
}
IOReturn A830Cp::readProgress(A830CpProgress *progress)
{
    if(!progress)return kIOReturnBadArgument;
    *progress={};
    if(!attached_ || !ring_->attached())return kIOReturnNotReady;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    progress->fence=ring_->memptrs()[A830_MEMPTRS_FENCE/4];
    IOReturn s=read(A830CpStepNonctxt,A8XX_CP_RB_RPTR,&progress->rptr);
    if(!s)s=read(A830CpStepNonctxt,A8XX_CP_RB_RPTR_BV,&progress->bv_rptr);
    if(s)return s;
    lock();
    uint32_t lo=0,hi=0;
    s=pipeRead(A830CpStepNonctxt,A8XX_PIPE_BR,A8XX_CP_IB1_BASE,&lo);
    if(!s)s=read(A830CpStepNonctxt,A8XX_CP_IB1_BASE_HI,&hi);
    if(!s)s=read(A830CpStepNonctxt,A8XX_CP_IB1_REM_SIZE,&progress->ib1_rem);
    const IOReturn back=aperture(A830CpStepNonctxt,A8XX_PIPE_NONE);
    unlock();
    progress->ib1_base=uint64_t(hi)<<32|lo;
    return s?s:back;
}
IOReturn A830Cp::readFault(A830CpFault *fault)
{
    if(!fault)return kIOReturnBadArgument;
    *fault={};
    if(!attached_)return kIOReturnNotReady;
    lock();
    IOReturn s=aperture(A830CpStepNonctxt,A8XX_PIPE_NONE);
    if(!s)s=read(A830CpStepNonctxt,A8XX_CP_INTERRUPT_STATUS_GLOBAL,&fault->global);
    for(uint32_t pipe=PipeFirst;!s && pipe<=PipeLast;++pipe) {
        if(!(fault->global&(A8XX_CP_GLOBAL_INT_HWFAULT(pipe)|A8XX_CP_GLOBAL_INT_SWFAULT(pipe))))continue;
        fault->pipes|=uint8_t(1u<<pipe);
        s=pipeRead(A830CpStepNonctxt,pipe,A8XX_CP_HW_FAULT_STATUS_PIPE,&fault->hw[pipe]);
        if(!s)s=read(A830CpStepNonctxt,A8XX_CP_INTERRUPT_STATUS_PIPE,&fault->sw[pipe]);
        if(!s && (fault->sw[pipe]&A8XX_CP_SW_FAULT_OPCODE_ERROR)) {   /* CP_SQE_STAT[1] holds the opcode */
            s=write(A830CpStepNonctxt,A8XX_CP_SQE_STAT_ADDR_PIPE,A8XX_CP_SQE_STAT_OPCODE);
            if(!s)s=read(A830CpStepNonctxt,A8XX_CP_SQE_STAT_DATA_PIPE,&fault->opcode[pipe]);
        }
        if(!s && (fault->sw[pipe]&A8XX_CP_SW_FAULT_REG_PROTECTION))
            s=read(A830CpStepNonctxt,A8XX_CP_PROTECT_STATUS_PIPE,&fault->protect[pipe]);
    }
    const IOReturn back=aperture(A830CpStepNonctxt,A8XX_PIPE_NONE);
    unlock();
    return s?s:back;
}
