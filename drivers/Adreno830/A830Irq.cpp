/* SPDX-License-Identifier: MIT */
#include "A830Irq.h"
#include "A830Fence.h"
#include "A830Regs.h"
#include "Adreno830Status.h"

/* The PaganiArmCpu exports, as weak defaults: the static kernel link refuses an undefined weak reference, and a board whose
 * ArmCpu lacks an export must still link (ARCHITECTURE 4.12.1). Unsupported reads as "missing". */
extern "C" __attribute__((weak)) IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *)
{
    return kIOReturnUnsupported;
}
extern "C" __attribute__((weak)) uint64_t armcpu_spi_count(uint32_t) {return 0;}
extern "C" __attribute__((weak)) IOReturn armcpu_spi_disable(uint32_t) {return kIOReturnUnsupported;}
extern "C" __attribute__((weak)) IOReturn armcpu_spi_enable(uint32_t) {return kIOReturnUnsupported;}

extern "C" A830FaultReport a830_fault_last;
A830FaultReport a830_fault_last;
extern "C" A830IrqErrorRecord a830_irq_error_last;
A830IrqErrorRecord a830_irq_error_last;

namespace {
const A830IrqPlatform kArmCpu={armcpu_spi_attach,armcpu_spi_disable,armcpu_spi_enable,armcpu_spi_count};
A830Irq *gInstance;

uint32_t directRead(void *base,uint32_t offset) {return *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base)+offset);}
void directWrite(void *base,uint32_t offset,uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base)+offset)=value;
}

template<typename T> T load(const T &value) {return __atomic_load_n(&value,__ATOMIC_RELAXED);}
template<typename T> void bump(T &counter) {__atomic_fetch_add(&counter,1,__ATOMIC_RELAXED);}

/* RBBM_INT_0 events that are not errors (ARCHITECTURE 4.12.3 step 4): PM4CPINTERRUPT, RB_DONE_TS, CACHE_FLUSH_TS. */
constexpr uint32_t kNormal=A8XX_RBBM_INT_0_PM4CPINTERRUPT|A8XX_RBBM_INT_0_CP_RB_DONE_TS|A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS;
static_assert((A8XX_INT_ERRORS&kNormal)==0 && (A8XX_INT_ERRORS|kNormal)==A8XX_INT_MASK,"error bits are the mask minus the events");
}

const A830IrqPlatform *a830IrqArmCpu(void) {return &kArmCpu;}
A830Mmio a830MmioDirect(volatile void *base) {return {const_cast<void *>(base),directRead,directWrite};}

/* ---- A830IrqGuard ---- */

IOReturn A830IrqGuard::attach(const A830IrqPlatform *platform,uint32_t intid,bool edge,uint32_t off_bit,uint32_t threshold,
                              void (*handler)(void *),void *context,void (*mask)(void *),void *mask_context,bool check_readback)
{
    if(attached_)return kA830ExclusiveAccess;
    platform_=platform;intid_=intid;edge_=edge;off_bit_=off_bit;threshold_=threshold?threshold:A830_IRQ_STORM_OTHER;
    mask_=mask;mask_context_=mask_context;stalls_=0;readback_=0;router_=0;
    if(!platform || !platform->attach || !handler || (!edge && !mask)) {
        __atomic_store_n(&off_,true,__ATOMIC_RELEASE);
        adreno830_status_irq_off(off_bit_);
        return !handler || (!edge && !mask)?kIOReturnBadArgument:kIOReturnUnsupported;
    }
    uint32_t readback=0;
    uint64_t router=0;
    /* The handler may run before this returns (a level source already asserted): note() works before attached_ is set. */
    const IOReturn result=platform->attach(intid,edge,handler,context,&readback,&router);
    readback_=readback;router_=router;
    if(result!=kIOReturnSuccess) {
        __atomic_store_n(&off_,true,__ATOMIC_RELEASE);
        adreno830_status_irq_off(off_bit_);
        return result;
    }
    attached_=true;
    if(check_readback && (readback&A830IrqReadbackMask)!=(edge?A830IrqReadbackEdge:A830IrqReadbackLevel)) {
        /* Not the configuration asked for (e.g. a Group 0 line): the line is not trusted, its owner polls. ArmCpu has no
         * detach; the device-side mask and a disable (when exported) keep it quiet. */
        if(mask_)mask_(mask_context_);
        if(platform->disable)platform_disabled_=platform->disable(intid)==kIOReturnSuccess;
        __atomic_store_n(&off_,true,__ATOMIC_RELEASE);
        adreno830_status_irq_off(off_bit_);
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

bool A830IrqGuard::note(uint32_t entry_status,uint32_t reread_status)
{
    return noteProgress(entry_status!=0 && (reread_status&entry_status)!=entry_status);
}

bool A830IrqGuard::noteProgress(bool progressed)
{
    bump(entries_);
    if(edge_ || off())return false;
    if(progressed) {stalls_=0;return false;}
    if(++stalls_<threshold_)return false;
    trip();
    return true;
}

void A830IrqGuard::trip()
{
    if(__atomic_exchange_n(&off_,true,__ATOMIC_ACQ_REL))return;
    ++trips_;
    if(mask_)mask_(mask_context_);
    platform_disabled_=platform_ && platform_->disable && platform_->disable(intid_)==kIOReturnSuccess;
    adreno830_status_irq_off(off_bit_);
}

IOReturn A830IrqGuard::rearm()
{
    if(!attached_)return kIOReturnNotReady;
    IOReturn result=kIOReturnSuccess;
    if(platform_disabled_) {
        result=platform_ && platform_->enable?platform_->enable(intid_):kIOReturnUnsupported;
        if(result==kIOReturnSuccess)platform_disabled_=false;
    }
    stalls_=0;
    __atomic_store_n(&off_,false,__ATOMIC_RELEASE);
    __atomic_fetch_and(&adreno830_status.irq_off,~off_bit_,__ATOMIC_RELAXED);
    return result;
}

IOReturn A830IrqGuard::suspend()
{
    if(!attached_ || off())return kIOReturnSuccess;
    if(!platform_ || !platform_->disable)return kIOReturnUnsupported;
    const IOReturn result=platform_->disable(intid_);
    if(result==kIOReturnSuccess)platform_disabled_=true;
    return result;
}

IOReturn A830IrqGuard::resume()
{
    if(!attached_ || off() || !platform_disabled_)return kIOReturnSuccess;
    const IOReturn result=platform_ && platform_->enable?platform_->enable(intid_):kIOReturnUnsupported;
    if(result==kIOReturnSuccess)platform_disabled_=false;
    stalls_=0;
    return result;
}

uint64_t A830IrqGuard::platformCount() const
{
    return attached_ && platform_ && platform_->count?platform_->count(intid_):0;
}

/* ---- A830Irq ---- */

A830Irq *A830Irq::instance() {return __atomic_load_n(&gInstance,__ATOMIC_ACQUIRE);}

uint32_t A830Irq::gpuRead(uint32_t dword) {return config_.gpu.read32(config_.gpu.cookie,A830_GPU_REG_BYTE(dword));}
void A830Irq::gpuWrite(uint32_t dword,uint32_t value) {config_.gpu.write32(config_.gpu.cookie,A830_GPU_REG_BYTE(dword),value);}

void A830Irq::lockMmio()
{
    if(config_.hooks.lockMmio) {config_.hooks.lockMmio(config_.hooks.cookie);return;}
    while(__atomic_exchange_n(&self_lock_,1u,__ATOMIC_ACQUIRE)) {}
}

void A830Irq::unlockMmio()
{
    if(config_.hooks.unlockMmio) {config_.hooks.unlockMmio(config_.hooks.cookie);return;}
    __atomic_store_n(&self_lock_,0u,__ATOMIC_RELEASE);
}

/* Pipe-banked registers: CP_APERTURE_CNTL_HOST selects the pipe and goes back to 0 after use (ARCHITECTURE 4.8). The caller
 * holds lockMmio() and writes the aperture back to 0 when it is done. */
uint32_t A830Irq::readPipe(unsigned pipe,uint32_t dword)
{
    gpuWrite(A8XX_CP_APERTURE_CNTL_HOST,A8XX_CP_APERTURE_CNTL_HOST_PIPE(pipe));
    return gpuRead(dword);
}

void A830Irq::writePipe(unsigned pipe,uint32_t dword,uint32_t value)
{
    gpuWrite(A8XX_CP_APERTURE_CNTL_HOST,A8XX_CP_APERTURE_CNTL_HOST_PIPE(pipe));
    gpuWrite(dword,value);
}

/* IFPC (ARCHITECTURE 4.12.3, KGSL gen8_gpu_keepalive / gen8_irq_poll_fence): GMU KEEPALIVE = 1, then AHB_FENCE_CTRL must be
 * 0 (ALLOW) within 100 x 1 us before any GX register is touched; KEEPALIVE = 0 when done. */
bool A830Irq::keepAlive(bool on)
{
    if(!config_.gmu.valid())return true;
    config_.gmu.write32(config_.gmu.cookie,A830_GMU_REG_BYTE(A8XX_GMUCX_PWR_COL_KEEPALIVE),on?1u:0u);
    if(!on)return true;
    for(unsigned poll=0;;++poll) {
        if(config_.gmu.read32(config_.gmu.cookie,A830_GMU_REG_BYTE(A8XX_GMUAO_AHB_FENCE_CTRL))==A830_AHB_FENCE_CTRL_ALLOW)
            return true;
        if(poll==A830_AHB_FENCE_POLLS)return false;
        a830DelayUs(1);
    }
}

/* Thread-context GX access: GX on and, under IFPC, held by KEEPALIVE with the AHB fence open. gxRelease() always follows. */
bool A830Irq::gxAcquire()
{
    if(!__atomic_load_n(&gx_on_,__ATOMIC_ACQUIRE))return false;
    return !ifpc() || keepAlive(true);
}

void A830Irq::gxRelease()
{
    if(ifpc())keepAlive(false);
}

void A830Irq::retireCall(void *param0,void *) {static_cast<A830Irq *>(param0)->retireWork();}
void A830Irq::faultCall(void *param0,void *) {static_cast<A830Irq *>(param0)->faultWork();}
void A830Irq::lostCall(void *param0,void *)
{
    A830Irq *self=static_cast<A830Irq *>(param0);
    const uint32_t kind=__atomic_exchange_n(&self->lost_kind_,0u,__ATOMIC_ACQ_REL);
    if(kind)self->escalate(A830FaultKind(kind),A830ReasonGmuFault,0);
}
void A830Irq::primaryTrampoline(void *context) {static_cast<A830Irq *>(context)->primary();}
void A830Irq::cxHostTrampoline(void *context) {static_cast<A830Irq *>(context)->cxHost();}

/* Storm guard of 332, device side: RBBM_INT_0_MASK = 0, unless GX is off (then nothing may touch a GX register). */
void A830Irq::maskGpu(void *context)
{
    A830Irq *self=static_cast<A830Irq *>(context);
    if(__atomic_load_n(&self->gx_on_,__ATOMIC_ACQUIRE))self->gpuWrite(A8XX_RBBM_INT_0_MASK,0);
}

void A830Irq::maskCx(void *context)
{
    A830Irq *self=static_cast<A830Irq *>(context);
    if(self->config_.cx_misc.valid())
        self->config_.cx_misc.write32(self->config_.cx_misc.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_MASK),0);
}

IOReturn A830Irq::start(const A830IrqConfig &config)
{
    if(started_)return kIOReturnBusy;
    if(!config.gpu.valid())return kIOReturnBadArgument;
    config_=config;
    if(!config_.platform)config_.platform=a830IrqArmCpu();
    retire_call_=a830CallAllocate(retireCall,this);
    fault_call_=a830CallAllocate(faultCall,this);
    lost_call_=a830CallAllocate(lostCall,this);
    if(!retire_call_ || !fault_call_ || !lost_call_)return kIOReturnNoMemory;
    started_=true;
    __atomic_store_n(&gInstance,this,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

IOReturn A830Irq::attach(uint32_t intid)
{
    if(!started_)return kIOReturnNotReady;
    if(gpu_guard_.attached())return kA830ExclusiveAccess;
    /* ARCHITECTURE 3.4 S8: clear what is pending (the CP init fence of S6 left bit20) before the line is enabled. The mask
     * itself was programmed in S6 (A830Cp, 4.8 step 16). */
    const uint32_t status=gpuRead(A8XX_RBBM_INT_0_STATUS);
    gpuWrite(A8XX_RBBM_INT_CLEAR_CMD,status|A8XX_INT_MASK);
    __atomic_store_n(&stats_.last_status,status,__ATOMIC_RELAXED);
    const IOReturn result=gpu_guard_.attach(config_.platform,intid,false,A830IrqOffGpu,A830_IRQ_STORM_GPU,primaryTrampoline,this,
                                            maskGpu,this,false);
    /* annibale with pagani-gpu-irq=1: its hw_init left every RBBM interrupt masked; only the fence one is unmasked, once the
     * line is attached (nothing is pending: cleared above). */
    if(result==kIOReturnSuccess && config_.own_mask && !gpu_guard_.off())gpuWrite(A8XX_RBBM_INT_0_MASK,config_.own_mask);
    return result;
}

void A830Irq::fallBack()
{
    if(!started_)return;
    __atomic_store_n(&polled_,true,__ATOMIC_RELEASE);
    /* The storm guard's own switch-off: RBBM_INT_0_MASK = 0 (GX on), armcpu_spi_disable, irq_off bit 0; off() from now on. */
    gpu_guard_.trip();
}

/* ARCHITECTURE 4.12.3. No lock; with IFPC off only RBBM_INT_0_STATUS, RBBM_INT_CLEAR_CMD and RBBM_INT_0_MASK are touched. */
void A830Irq::primary()
{
    bump(stats_.entries);
    adreno830_status_add64(&adreno830_status.irqs,1);
    if(!__atomic_load_n(&gx_on_,__ATOMIC_ACQUIRE)) {
        /* GX is off (slumber, recovery): nothing may be read. A level line cannot be pending; count it as no progress. */
        bump(stats_.spurious);
        gpu_guard_.noteProgress(false);
        return;
    }
    const bool ifpc=this->ifpc();
    if(ifpc && !keepAlive(true)) {
        /* The AHB fence stayed closed: status and clear are unreachable (KGSL gen8_irq_handler: GMU fault). */
        bump(stats_.keepalive_timeouts);
        __atomic_store_n(&lost_kind_,uint32_t(A830IrqFaultAhbFence),__ATOMIC_RELEASE);
        a830CallEnter(lost_call_);
        keepAlive(false);
        gpu_guard_.noteProgress(false);
        return;
    }
    /* 1. read, clear what was read, then dispatch (Linux a8xx_irq, KGSL gen8_irq_handler). */
    const uint32_t status=gpuRead(A8XX_RBBM_INT_0_STATUS);
    if(status)gpuWrite(A8XX_RBBM_INT_CLEAR_CMD,status);
    /* 2. */
    __atomic_fetch_or(&pending_,status,__ATOMIC_RELAXED);
    __atomic_store_n(&stats_.last_status,status,__ATOMIC_RELAXED);
    bool hard=false;
    /* 4 (first): CP_HW_ERROR and HANG_DETECT keep firing until the CP is reset; only the fence interrupt stays enabled. */
    if(status&A8XX_INT_FATAL) {
        gpuWrite(A8XX_RBBM_INT_0_MASK,A830_INT_MASK_FENCE_ONLY);
        __atomic_store_n(&fatal_masked_,true,__ATOMIC_RELAXED);
        hard=true;
    }
    /* 3. the fence: the waiters directly, retire_call only while a completion waits for retirement. */
    if(status&A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS) {
        bump(stats_.fence_irqs);
        __atomic_store_n(&stats_.last_fence_irq,a830Now(),__ATOMIC_RELEASE);
        adreno830_status_add64(&adreno830_status.fence_irqs,1);
        if(A830Fence *fence=config_.fence) {
            fence->signal();
            if(fence->retirePending()) {bump(stats_.retire_calls);a830CallEnter(retire_call_);}
        }
    }
    if(status&A8XX_RBBM_INT_0_CP_RB_DONE_TS)bump(stats_.rb_done);
    if(status&A8XX_RBBM_INT_0_PM4CPINTERRUPT)bump(stats_.pm4);
    /* 4. every other bit is recorded by faultWork (aperture reads happen there, under the MMIO lock). */
    const uint32_t errors=status&~kNormal;
    if(errors) {
        bump(stats_.errors);
        __atomic_fetch_or(&stats_.error_bits,errors,__ATOMIC_RELAXED);
        __atomic_fetch_or(&error_pending_,errors,__ATOMIC_RELEASE);
        bump(stats_.fault_calls);
        a830CallEnter(fault_call_);
    }
    if(!status)bump(stats_.spurious);
    /* 5. storm guard: an entry with nothing set, or error bits that survive their clear, is no progress. With own_mask
     *    (annibale, pagani-gpu-irq=1: a line the first round never had live) every bit that survives its clear is, as on
     *    the other lines (A830IrqGuard::note): a fence bit the clear does not take ends in polling, not in a livelock. */
    const uint32_t reread=gpuRead(A8XX_RBBM_INT_0_STATUS);
    const bool stuck=config_.own_mask?(status && (reread&status)==status):(errors && (reread&errors)==errors);
    gpu_guard_.noteProgress(status!=0 && !stuck);
    if(ifpc && !hard)keepAlive(false);
}

IOReturn A830Irq::attachCxHost(uint32_t intid)
{
    if(!started_)return kIOReturnNotReady;
    if(!config_.cx_misc.valid()) {adreno830_status_irq_off(A830IrqOffCxHost);return kIOReturnUnsupported;}
    /* KGSL gen8_gmu_irq_enable: clear everything pending, enable only the GPU_CC frequency-limit event. */
    config_.cx_misc.write32(config_.cx_misc.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_CLEAR_CMD),0xFFFFFFFFu);
    config_.cx_misc.write32(config_.cx_misc.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_MASK),A8XX_CX_MISC_INT_GPU_CC_IRQ);
    return cx_guard_.attach(config_.platform,intid,false,A830IrqOffCxHost,A830_IRQ_STORM_OTHER,cxHostTrampoline,this,maskCx,this,true);
}

/* cx_host_irq (KGSL gen8_cx_host_irq_handler): read, clear, count the frequency-limit events. */
void A830Irq::cxHost()
{
    bump(stats_.cx_events);
    const A830Mmio &cx=config_.cx_misc;
    const uint32_t status=cx.read32(cx.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_STATUS));
    if(status)cx.write32(cx.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_CLEAR_CMD),status);
    if(status&A8XX_CX_MISC_INT_GPU_CC_IRQ)bump(stats_.cx_freq_limits);
    cx_guard_.note(status,cx.read32(cx.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_STATUS)));
}

void A830Irq::retireWork()
{
    A830Fence *fence=config_.fence;
    if(!fence)return;
    const uint32_t completed=fence->completed();
    if(config_.hooks.retire)config_.hooks.retire(config_.hooks.cookie,completed);
    fence->noteRetired(completed);
}

void A830Irq::kickRetire()
{
    if(started_) {bump(stats_.retire_calls);a830CallEnter(retire_call_);}
}

void A830Irq::snapshot(A830FaultReport *out)
{
    A830FaultReport r{};
    r.when=a830Now();
    if(A830Fence *fence=config_.fence) {r.fence=fence->completed();r.submitted=fence->lastSubmitted();}
    if(config_.hooks.smmuSnapshot)config_.hooks.smmuSnapshot(config_.hooks.cookie,&r.smmu_fsr,&r.smmu_far);
    if(config_.gmu.valid())
        r.unmasked=config_.gmu.read32(config_.gmu.cookie,A830_GMU_REG_BYTE(A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW));
    if(gxAcquire()) {
        r.gx_read=true;
        r.rbbm_status=gpuRead(A8XX_RBBM_STATUS);
        r.rbbm_status1=gpuRead(A8XX_RBBM_STATUS1);
        r.rptr=gpuRead(A8XX_CP_RB_RPTR);
        r.wptr=gpuRead(A8XX_CP_RB_WPTR);
        r.cp_global=gpuRead(A8XX_CP_INTERRUPT_STATUS_GLOBAL);
        r.rl_error[0]=gpuRead(A8XX_CP_RL_ERROR_DETAILS_0);
        r.rl_error[1]=gpuRead(A8XX_CP_RL_ERROR_DETAILS_1);
        /* Per pipe (Linux a8xx_cp_hw_err_irq, KGSL gen8_get_cp_*fault_status): BR then BV. */
        lockMmio();
        static const unsigned pipes[2]={A8XX_PIPE_BR,A8XX_PIPE_BV};
        for(unsigned i=0;i<2;++i) {
            r.pipe_hw[i]=readPipe(pipes[i],A8XX_CP_HW_FAULT_STATUS_PIPE);
            r.pipe_sw[i]=readPipe(pipes[i],A8XX_CP_INTERRUPT_STATUS_PIPE);
            r.pipe_protect[i]=readPipe(pipes[i],A8XX_CP_PROTECT_STATUS_PIPE);
            if(r.pipe_sw[i]&A8XX_CP_SW_FAULT_OPCODE_ERROR) {
                writePipe(pipes[i],A8XX_CP_SQE_STAT_ADDR_PIPE,A8XX_CP_SQE_STAT_OPCODE);
                r.pipe_opcode[i]=readPipe(pipes[i],A8XX_CP_SQE_STAT_DATA_PIPE);
            }
        }
        r.ib1_base=readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE)|uint64_t(readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE_HI))<<32;
        r.ib1_rem=readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE);
        r.ib2_base=readPipe(A8XX_PIPE_BR,A8XX_CP_IB2_BASE)|uint64_t(readPipe(A8XX_PIPE_BR,A8XX_CP_IB2_BASE_HI))<<32;
        r.ib2_rem=readPipe(A8XX_PIPE_BR,A8XX_CP_IB2_REM_SIZE);
        gpuWrite(A8XX_CP_APERTURE_CNTL_HOST,A8XX_CP_APERTURE_CNTL_HOST_PIPE(A8XX_PIPE_NONE));
        unlockMmio();
    }
    gxRelease();
    *out=r;
}

bool A830Irq::probe(uint32_t *rptr,uint64_t *ib1_base,uint32_t *ib1_rem)
{
    if(!started_)return false;
    if(!gxAcquire()) {gxRelease();return false;}
    *rptr=gpuRead(A8XX_CP_RB_RPTR);
    lockMmio();
    *ib1_base=readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE)|uint64_t(readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE_HI))<<32;
    *ib1_rem=readPipe(A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE);
    gpuWrite(A8XX_CP_APERTURE_CNTL_HOST,A8XX_CP_APERTURE_CNTL_HOST_PIPE(A8XX_PIPE_NONE));
    unlockMmio();
    gxRelease();
    return true;
}

void A830Irq::report(A830FaultKind kind,uint16_t reason,uint32_t int_status,uint64_t detail,A830FaultReport *out)
{
    snapshot(out);
    out->kind=kind;out->reason=reason;out->int_status=int_status;out->detail=detail;
}

/* ARCHITECTURE 4.12.3 bottom half: the error bits the top half handed over. HANG_DETECT and CP_HW_ERROR are global losses
 * (4.13.4); the others are recorded (Linux and KGSL only log them). */
void A830Irq::faultWork()
{
    const uint32_t bits=__atomic_exchange_n(&error_pending_,0u,__ATOMIC_ACQ_REL);
    if(!bits)return;
    A830IrqErrorRecord record=a830_irq_error_last;
    record.bits=bits;record.when=a830Now();++record.batches;
    bool stalled=false;
    if(gxAcquire()) {
        /* Linux a8xx_fault_detect_irq: a hang while the SMMU stalls on a fault is the fault's, not a hang (the SMMU handler
         * resumes the transaction): the full mask comes back. Terminate mode with HUPCF should never get here. */
        if((bits&A8XX_INT_FATAL)==A8XX_RBBM_INT_0_HANG_DETECT &&
           (gpuRead(A8XX_RBBM_MISC_STATUS)&A8XX_RBBM_MISC_STATUS_SMMU_STALLED_ON_FAULT)) {
            stalled=true;
            gpuWrite(A8XX_RBBM_INT_0_MASK,A8XX_INT_MASK);
            __atomic_store_n(&fatal_masked_,false,__ATOMIC_RELAXED);
        }
        if(bits&A8XX_RBBM_INT_0_CP_AHB_ERROR) {
            record.rl_error[0]=gpuRead(A8XX_CP_RL_ERROR_DETAILS_0);
            record.rl_error[1]=gpuRead(A8XX_CP_RL_ERROR_DETAILS_1);
        }
        if(bits&A8XX_RBBM_INT_0_SW_FUSE_VIOLATION) {
            /* Linux a8xx_sw_fuse_violation_irq: read, then mask the fuse interrupt (it would fire again). */
            record.fuse=gpuRead(A8XX_RBBM_SW_FUSE_INT_STATUS);
            gpuWrite(A8XX_RBBM_SW_FUSE_INT_MASK,0);
        }
        if(bits&A8XX_RBBM_INT_0_TSB_WRITE_ERROR)
            record.tsb=gpuRead(A8XX_RBBM_SECVID_TSB_STATUS)|uint64_t(gpuRead(A8XX_RBBM_SECVID_TSB_STATUS_HI))<<32;
    }
    gxRelease();
    a830_irq_error_last=record;
    if(!(bits&A8XX_INT_FATAL) || stalled) {
        if(config_.hooks.errorNoted)config_.hooks.errorNoted(config_.hooks.cookie,bits);
        return;
    }
    const bool hang=bits&A8XX_RBBM_INT_0_HANG_DETECT;
    lose(hang?A830IrqFaultHang:A830IrqFaultCpHwError,hang?A830ReasonHang:A830ReasonCpFault,bits,0);
}

/* ARCHITECTURE 4.13.4: RBBM_INT_0_MASK = 0, snapshot, then the sink. */
void A830Irq::escalate(A830FaultKind kind,uint16_t reason,uint64_t detail)
{
    if(started_)lose(kind,reason,0,detail);
}

void A830Irq::lose(A830FaultKind kind,uint16_t reason,uint32_t int_status,uint64_t detail)
{
    A830FaultReport r{};
    if(__atomic_exchange_n(&loss_active_,true,__ATOMIC_ACQ_REL)) {
        /* A loss is being handled already: keep the newest record only. */
        report(kind,reason,int_status,detail,&r);
        a830_fault_last=r;
        return;
    }
    if(gxAcquire())gpuWrite(A8XX_RBBM_INT_0_MASK,0);
    gxRelease();
    __atomic_store_n(&fatal_masked_,true,__ATOMIC_RELAXED);
    report(kind,reason,int_status,detail,&r);
    a830_fault_last=r;
    switch(kind) {
    case A830IrqFaultHang: case A830IrqFaultWatchdog: case A830IrqFaultSelfTest: adreno830_status_add32(&adreno830_status.hangs,1);break;
    case A830IrqFaultGmu: case A830IrqFaultAhbFence: adreno830_status_add32(&adreno830_status.gmu_faults,1);break;
    default: adreno830_status_add32(&adreno830_status.faults,1);break;
    }
    adreno830_status.reg=A830_GPU_REG_PHYS(A8XX_RBBM_STATUS);
    adreno830_status.value=r.rbbm_status|uint64_t(r.int_status)<<32;
    adreno830_status.expect=r.rptr|uint64_t(r.wptr)<<32;
    if(config_.hooks.deviceLost) {config_.hooks.deviceLost(config_.hooks.cookie,&r);return;}
    if(A830Fence *fence=config_.fence)fence->markLost();
    adreno830_status_lost(adreno830_status.stage,uint16_t(kind),reason,kA830NotResponding);
}

IOReturn A830Irq::quiesce(bool force)
{
    if(!started_)return kIOReturnNotReady;
    if(__atomic_load_n(&gx_on_,__ATOMIC_ACQUIRE)) {
        const bool gx=gxAcquire();
        /* ARCHITECTURE 4.17 P1 S2: an unmasked interrupt still pending (or a closed AHB fence) gives the slumber up. */
        if(!force && (!gx || gpuRead(A8XX_RBBM_INT_0_STATUS))) {gxRelease();return kIOReturnBusy;}
        if(gx)gpuWrite(A8XX_RBBM_INT_0_MASK,0);
        gxRelease();
    }
    __atomic_store_n(&gx_on_,false,__ATOMIC_RELEASE);
    gpu_guard_.suspend();
    if(cx_guard_.attached())cx_guard_.suspend();
    return kIOReturnSuccess;
}

IOReturn A830Irq::restore()
{
    if(!started_)return kIOReturnNotReady;
    __atomic_store_n(&error_pending_,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&gx_on_,true,__ATOMIC_RELEASE);
    const bool polled=this->polled();
    /* Whatever the reset left pending is stale. The mask is A830Cp's (hw_init programmed it again), or own_mask's: annibale's
     * hw_init masked everything again, so the fence interrupt goes back on while 332 is attached and not given up. */
    if(gxAcquire()) {
        const uint32_t status=gpuRead(A8XX_RBBM_INT_0_STATUS);
        if(status)gpuWrite(A8XX_RBBM_INT_CLEAR_CMD,status);
        if(config_.own_mask && gpu_guard_.attached() && !polled)gpuWrite(A8XX_RBBM_INT_0_MASK,config_.own_mask);
    }
    gxRelease();
    __atomic_store_n(&fatal_masked_,false,__ATOMIC_RELEASE);
    __atomic_store_n(&loss_active_,false,__ATOMIC_RELEASE);
    IOReturn result=polled?kIOReturnSuccess:gpu_guard_.off()?gpu_guard_.rearm():gpu_guard_.resume();
    if(cx_guard_.attached()) {
        if(cx_guard_.off() && config_.cx_misc.valid()) {
            config_.cx_misc.write32(config_.cx_misc.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_CLEAR_CMD),0xFFFFFFFFu);
            config_.cx_misc.write32(config_.cx_misc.cookie,A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_MASK),
                                    A8XX_CX_MISC_INT_GPU_CC_IRQ);
            cx_guard_.rearm();
        } else cx_guard_.resume();
    }
    return result;
}

A830IrqStats A830Irq::stats() const
{
    A830IrqStats s{};
    s.entries=load(stats_.entries);s.fence_irqs=load(stats_.fence_irqs);s.rb_done=load(stats_.rb_done);s.pm4=load(stats_.pm4);
    s.spurious=load(stats_.spurious);s.errors=load(stats_.errors);s.retire_calls=load(stats_.retire_calls);
    s.fault_calls=load(stats_.fault_calls);s.last_fence_irq=load(stats_.last_fence_irq);s.error_bits=load(stats_.error_bits);
    s.last_status=load(stats_.last_status);s.cx_events=load(stats_.cx_events);s.cx_freq_limits=load(stats_.cx_freq_limits);
    s.keepalive_timeouts=load(stats_.keepalive_timeouts);
    return s;
}
