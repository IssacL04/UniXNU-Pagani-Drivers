/* SPDX-License-Identifier: MIT */
/* host-test: A830Irq.cpp A830Fence.cpp Adreno830Status.cpp tests/A830HostSched.cpp */
/* A830Irq over a fake GIC and a fake RBBM register file (ARCHITECTURE 4.12, PLAN M6/M10/M13): the 332 wiring, the top half's
 * register discipline (only 0x6A/0x61/0x62; no aperture), errors handed to faultWork, fatal masking, global losses through the
 * sink, the storm guard of 332 and of the other lines with and without armcpu_spi_disable, optional lines that only record,
 * cx_host_irq, quiesce/restore, IFPC keepalive and the AHB fence fault. Thread calls run manually for determinism. */
#include "A830FakeGpu.h"
#include "A830Fence.h"
#include "A830HostSched.h"
#include "A830Irq.h"
#include "Adreno830Status.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t kGpuIntid=A830_IRQ_GPU,kCxIntid=A830_IRQ_CX_HOST;
unsigned checks;
#define EXPECT(x) do {CHECK(x);++checks;} while(0)

struct Sink {
    unsigned lost=0,errors=0,retires=0;
    uint32_t last_completed=0,error_bits=0;
    A830FaultReport report{};
    unsigned locks=0,unlocks=0;
};
Sink sink;

A830IrqHooks hooks()
{
    A830IrqHooks h{};
    h.cookie=&sink;
    h.lockMmio=[](void *c) {++static_cast<Sink *>(c)->locks;};
    h.unlockMmio=[](void *c) {++static_cast<Sink *>(c)->unlocks;};
    h.retire=[](void *c,uint32_t completed) {Sink *s=static_cast<Sink *>(c);++s->retires;s->last_completed=completed;};
    h.deviceLost=[](void *c,const A830FaultReport *r) {Sink *s=static_cast<Sink *>(c);++s->lost;s->report=*r;};
    h.errorNoted=[](void *c,uint32_t bits) {Sink *s=static_cast<Sink *>(c);++s->errors;s->error_bits|=bits;};
    h.smmuSnapshot=[](void *,uint32_t *fsr,uint64_t *far) {*fsr=0x2;*far=0x1234000;};
    return h;
}

/* Objects live for the whole program: their thread calls may still be pending between cases. */
struct Rig {
    FakeGpu gpu;
    FakeWindow gmu,cx;
    uint32_t memptrs[1024]{};
    A830Fence fence;
    A830Irq irq;
};

Rig *fresh(bool with_hooks=true,const A830IrqPlatform *platform=nullptr,bool with_cx=false,bool with_gmu=false,
           uint32_t own_mask=0)
{
    a830HostRunCalls(true);
    gGic.reset();
    adreno830_status_reset();
    sink=Sink{};
    Rig *rig=new Rig;
    rig->gpu.mask=A8XX_INT_MASK;
    rig->cx.clear_offset=A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_CLEAR_CMD);
    rig->cx.status_offset=A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_STATUS);
    gGic.level=[rig](uint32_t intid) {
        if(intid==kGpuIntid)return rig->gpu.asserted();
        if(intid==kCxIntid)return (rig->cx.get(A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_STATUS))&
                                   rig->cx.get(A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_MASK)))!=0;
        return false;
    };
    A830FenceConfig fc{};
    fc.memptrs=rig->memptrs;fc.irq=&rig->irq;
    EXPECT(rig->fence.start(fc)==kIOReturnSuccess);
    rig->fence.stopWatchdog();   /* not under test here: thread calls run manually and would include it */
    A830IrqConfig config{};
    config.gpu=rig->gpu.mmio();
    if(with_gmu)config.gmu=rig->gmu.mmio();
    if(with_cx)config.cx_misc=rig->cx.mmio();
    config.platform=platform;
    config.fence=&rig->fence;
    config.own_mask=own_mask;
    if(own_mask)rig->gpu.mask=0;   /* the annibale profile's hw_init leaves every RBBM interrupt masked */
    if(with_hooks)config.hooks=hooks();
    EXPECT(rig->irq.start(config)==kIOReturnSuccess);
    EXPECT(A830Irq::instance()==&rig->irq && A830Fence::instance()==&rig->fence);
    EXPECT(rig->irq.start(config)==kIOReturnBusy);
    return rig;
}

bool onlyStatusRegisters(const std::vector<FakeAccess> &log)
{
    for(const FakeAccess &a:log)
        if(a.dword!=A8XX_RBBM_INT_0_STATUS && a.dword!=A8XX_RBBM_INT_CLEAR_CMD && a.dword!=A8XX_RBBM_INT_0_MASK)return false;
    return true;
}
bool touched(const std::vector<FakeAccess> &log,uint32_t dword,bool write)
{
    return std::any_of(log.begin(),log.end(),[&](const FakeAccess &a) {return a.dword==dword && a.write==write;});
}

/* Platform tables for "export missing" and "attach refused". */
IOReturn refuseAttach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *) {return kIOReturnNoResources;}
const A830IrqPlatform kNoDisable={armcpu_spi_attach,nullptr,nullptr,armcpu_spi_count};
const A830IrqPlatform kRefuse={refuseAttach,armcpu_spi_disable,armcpu_spi_enable,armcpu_spi_count};
const A830IrqPlatform kNoAttach={nullptr,nullptr,nullptr,nullptr};

void attachClearsAndWires()
{
    Rig *rig=fresh();
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);   /* the CP init fence of S6 */
    rig->gpu.startLog();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    const auto log=rig->gpu.stopLog();
    /* cleared before the line is enabled: the status read and everything in the mask */
    EXPECT(touched(log,A8XX_RBBM_INT_CLEAR_CMD,true) && rig->gpu.raw==0);
    EXPECT(std::any_of(log.begin(),log.end(),[](const FakeAccess &a) {
        return a.dword==A8XX_RBBM_INT_CLEAR_CMD && a.write && (a.value&A8XX_INT_MASK)==A8XX_INT_MASK;}));
    EXPECT(gGic.attaches==1 && gGic.enabled(kGpuIntid) && !gGic.find(kGpuIntid)->edge);
    EXPECT(rig->irq.attached() && !rig->irq.off());
    EXPECT(rig->irq.readback()==A830IrqReadbackLevel && rig->irq.router()==gGic.router);
    EXPECT((adreno830_status.irq_off&A830IrqOffGpu)==0);
    EXPECT(rig->irq.attach(kGpuIntid)==kA830ExclusiveAccess);
    /* a readback that is not 0xA003 is only recorded for 332: the S8 self-test is the criterion */
    Rig *other=fresh();
    gGic.forced_readback=0xA002;   /* Group 0 */
    EXPECT(other->irq.attach(kGpuIntid)==kIOReturnSuccess && other->irq.readback()==0xA002 && !other->irq.off());
}

void attachFailures()
{
    Rig *rig=fresh(true,&kRefuse);
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnNoResources);
    EXPECT(!rig->irq.attached() && rig->irq.off());
    EXPECT(adreno830_status.irq_off&A830IrqOffGpu);
    Rig *none=fresh(true,&kNoAttach);
    EXPECT(none->irq.attach(kGpuIntid)==kIOReturnUnsupported && (adreno830_status.irq_off&A830IrqOffGpu));
    A830Irq unstarted;
    EXPECT(unstarted.attach(kGpuIntid)==kIOReturnNotReady);
    A830IrqConfig bad{};
    EXPECT(unstarted.start(bad)==kIOReturnBadArgument);
}

void fenceTopHalf()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->fence.submitted(1);
    rig->memptrs[1]=1;
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS|A8XX_RBBM_INT_0_CP_RB_DONE_TS);
    const uint64_t irqs=adreno830_status.irqs,fence_irqs=adreno830_status.fence_irqs;
    rig->gpu.startLog();
    EXPECT(gGic.deliver(kGpuIntid)==1);
    const auto log=rig->gpu.stopLog();
    /* read, clear exactly what was read, read again (storm guard); nothing else */
    EXPECT(log.size()==3 && onlyStatusRegisters(log));
    EXPECT(!log[0].write && log[1].write && log[1].dword==A8XX_RBBM_INT_CLEAR_CMD &&
           log[1].value==(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS|A8XX_RBBM_INT_0_CP_RB_DONE_TS));
    EXPECT(!touched(log,A8XX_CP_APERTURE_CNTL_HOST,true) && !touched(log,A8XX_CP_APERTURE_CNTL_HOST,false));
    EXPECT(rig->fence.irqSeq()==1 && rig->irq.fenceIrqs()==1 && rig->irq.lastFenceIrq()!=0);
    EXPECT(adreno830_status.irqs==irqs+1 && adreno830_status.fence_irqs==fence_irqs+1);
    EXPECT(rig->irq.stats().rb_done==1 && sink.locks==0);
    /* retire_call was entered (a completion waits for retirement) and hands the completed fence over */
    EXPECT(a830HostRunCalls(true)>=1 && sink.retires==1 && sink.last_completed==1 && !rig->fence.retirePending());
    /* nothing to retire: the next fence interrupt does not enter retire_call */
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && a830HostRunCalls(true)==0 && rig->fence.irqSeq()==2);
    EXPECT((rig->irq.pending()&A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS)!=0);
}

void errorsOnlyQueued()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.set(A8XX_CP_RL_ERROR_DETAILS_0,0x11);rig->gpu.set(A8XX_CP_RL_ERROR_DETAILS_1,0x22);
    rig->gpu.set(A8XX_RBBM_SW_FUSE_INT_STATUS,0x4);
    rig->gpu.raise(A8XX_RBBM_INT_0_UCHE_OOB_ACCESS|A8XX_RBBM_INT_0_CP_AHB_ERROR|A8XX_RBBM_INT_0_SW_FUSE_VIOLATION);
    rig->gpu.startLog();
    EXPECT(gGic.deliver(kGpuIntid)==1);
    const auto log=rig->gpu.stopLog();
    EXPECT(onlyStatusRegisters(log) && !touched(log,A8XX_RBBM_INT_0_MASK,true));   /* not fatal: the mask stays */
    EXPECT(sink.errors==0);   /* nothing decoded in the top half */
    EXPECT(a830HostRunCalls(true)==1);
    EXPECT(sink.errors==1 && sink.error_bits==(A8XX_RBBM_INT_0_UCHE_OOB_ACCESS|A8XX_RBBM_INT_0_CP_AHB_ERROR|
                                               A8XX_RBBM_INT_0_SW_FUSE_VIOLATION));
    EXPECT(sink.lost==0 && !rig->fence.lost() && rig->gpu.maskNow()==A8XX_INT_MASK);
    EXPECT(a830_irq_error_last.rl_error[0]==0x11 && a830_irq_error_last.rl_error[1]==0x22 && a830_irq_error_last.fuse==0x4);
    EXPECT(rig->gpu.get(A8XX_RBBM_SW_FUSE_INT_MASK)==0);
}

void cpHwErrorIsGlobal()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.set(A8XX_RBBM_STATUS,0x80000004);rig->gpu.set(A8XX_CP_RB_RPTR,0x40);rig->gpu.set(A8XX_CP_RB_WPTR,0x80);
    rig->gpu.set(A8XX_CP_INTERRUPT_STATUS_GLOBAL,0x00010001);
    rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_HW_FAULT_STATUS_PIPE,0x12);
    rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_INTERRUPT_STATUS_PIPE,0x100);   /* opcode error */
    rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_SQE_STAT_DATA_PIPE,0xDEAD);
    rig->gpu.setPipe(A8XX_PIPE_BV,A8XX_CP_PROTECT_STATUS_PIPE,0x00100123);
    rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE,0x20001000);rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,7);
    rig->memptrs[1]=3;rig->fence.submitted(5);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_HW_ERROR|A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    rig->gpu.startLog();
    EXPECT(gGic.deliver(kGpuIntid)==1);
    const auto log=rig->gpu.stopLog();
    /* the top half masks everything but the fence at once (ARCHITECTURE 4.12.3 step 4) and touches no aperture */
    EXPECT(onlyStatusRegisters(log));
    EXPECT(std::any_of(log.begin(),log.end(),[](const FakeAccess &a) {
        return a.dword==A8XX_RBBM_INT_0_MASK && a.write && a.value==A830_INT_MASK_FENCE_ONLY;}));
    EXPECT(rig->irq.fatalMasked() && sink.lost==0);
    rig->gpu.startLog();
    a830HostRunCalls(true);
    const auto bottom=rig->gpu.stopLog();
    EXPECT(sink.lost==1 && sink.report.kind==A830IrqFaultCpHwError && sink.report.reason==A830ReasonCpFault);
    EXPECT(sink.report.int_status&A8XX_RBBM_INT_0_CP_HW_ERROR);
    EXPECT(sink.report.pipe_hw[0]==0x12 && sink.report.pipe_opcode[0]==0xDEAD && sink.report.pipe_protect[1]==0x00100123);
    EXPECT(sink.report.rbbm_status==0x80000004 && sink.report.rptr==0x40 && sink.report.wptr==0x80);
    EXPECT(sink.report.cp_global==0x00010001 && sink.report.ib1_base==0x20001000 && sink.report.ib1_rem==7);
    EXPECT(sink.report.fence==3 && sink.report.submitted==5 && sink.report.smmu_fsr==2 && sink.report.gx_read);
    /* the bottom half took the MMIO lock for the aperture and wrote the aperture back to 0 */
    EXPECT(sink.locks>=1 && sink.locks==sink.unlocks && rig->gpu.apertureNow()==0);
    EXPECT(touched(bottom,A8XX_CP_APERTURE_CNTL_HOST,true) && rig->gpu.maskNow()==0);
    EXPECT(a830_fault_last.kind==A830IrqFaultCpHwError && adreno830_status.faults==1);
    /* with a sink the fence is not marked lost here: the recovery entry decides */
    EXPECT(!rig->fence.lost());
}

void hangWhileSmmuStalledIsNotAHang()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.set(0x1E /* RBBM_MISC_STATUS */,1);
    rig->gpu.raise(A8XX_RBBM_INT_0_HANG_DETECT);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->gpu.maskNow()==A830_INT_MASK_FENCE_ONLY);
    a830HostRunCalls(true);
    EXPECT(sink.lost==0 && sink.errors==1 && rig->gpu.maskNow()==A8XX_INT_MASK && !rig->irq.fatalMasked());
}

void hangWithoutSinkLosesTheDevice()
{
    Rig *rig=fresh(false);
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    adreno830_status.stage=A830SRunning;
    rig->fence.submitted(9);
    rig->gpu.raise(A8XX_RBBM_INT_0_HANG_DETECT);
    EXPECT(gGic.deliver(kGpuIntid)==1);
    a830HostRunCalls(true);
    EXPECT(rig->fence.lost() && adreno830_status.state==A830Lost && adreno830_status.reason==A830ReasonHang);
    EXPECT(adreno830_status.hangs==1 && rig->gpu.maskNow()==0);
    EXPECT(rig->fence.waitNs(9,1000000)==kA830NotResponding);
    /* a second loss while the first is handled is only recorded */
    rig->irq.escalate(A830IrqFaultGmu,A830ReasonGmuFault,0x8000);
    EXPECT(a830_fault_last.kind==A830IrqFaultGmu && adreno830_status.hangs==1 && adreno830_status.gmu_faults==0);
}

void stormStuckErrorBit()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.stuck=A8XX_RBBM_INT_0_UCHE_TRAP;   /* survives every clear */
    const unsigned entries=gGic.deliver(kGpuIntid);
    EXPECT(entries==A830_IRQ_STORM_GPU);
    EXPECT(rig->irq.off() && rig->gpu.maskNow()==0 && gGic.disables==1 && !gGic.enabled(kGpuIntid));
    EXPECT(adreno830_status.irq_off&A830IrqOffGpu);
    EXPECT(rig->irq.gpuGuard().trips()==1);
    /* recovery re-initialized the device side: restore() enables 332 again and clears irq_off */
    rig->gpu.stuck=0;rig->gpu.mask=A8XX_INT_MASK;
    EXPECT(rig->irq.restore()==kIOReturnSuccess);
    EXPECT(!rig->irq.off() && gGic.enables==1 && gGic.enabled(kGpuIntid) && !(adreno830_status.irq_off&A830IrqOffGpu));
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.fenceIrqs()==1);
}

void stormSpurious()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    /* 999 entries with status 0, then a real event: the count starts over */
    for(unsigned i=0;i<A830_IRQ_STORM_GPU-1;++i)EXPECT(gGic.pulse(kGpuIntid));
    EXPECT(!rig->irq.off() && rig->irq.gpuGuard().stalls()==A830_IRQ_STORM_GPU-1);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.gpuGuard().stalls()==0);
    for(unsigned i=0;i<A830_IRQ_STORM_GPU-1;++i)EXPECT(gGic.pulse(kGpuIntid));
    EXPECT(!rig->irq.off());
    EXPECT(gGic.pulse(kGpuIntid) && rig->irq.off() && gGic.disables==1 && rig->gpu.maskNow()==0);
    EXPECT(!gGic.pulse(kGpuIntid));   /* disabled at the GIC */
    EXPECT(rig->irq.stats().spurious==2*(A830_IRQ_STORM_GPU-1)+1);
}

void stormWithoutDisableExport()
{
    Rig *rig=fresh(true,&kNoDisable);
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.stuck=A8XX_RBBM_INT_0_UCHE_OOB_ACCESS;
    /* only the device-side mask: RBBM_INT_0_MASK = 0 deasserts the level line, which ends the storm */
    EXPECT(gGic.deliver(kGpuIntid)==A830_IRQ_STORM_GPU);
    EXPECT(rig->irq.off() && gGic.disables==0 && gGic.enabled(kGpuIntid) && rig->gpu.maskNow()==0 && !rig->gpu.asserted());
    EXPECT(adreno830_status.irq_off&A830IrqOffGpu);
    /* restore() without the enable export: the guard is re-armed, nothing to enable */
    rig->gpu.stuck=0;rig->gpu.mask=A8XX_INT_MASK;
    EXPECT(rig->irq.restore()==kIOReturnSuccess && !rig->irq.off() && gGic.enables==0);
}

/* The generic guard as A830Smmu/A830Gmu use it for their lines. */
struct Line {
    FakeWindow window;
    A830IrqGuard guard;
    uint32_t masked=0;
    static void handler(void *c)
    {
        Line *l=static_cast<Line *>(c);
        const uint32_t s=l->window.get(0);
        if(s)l->window.set(0,s&l->window.get(8));   /* bits in +8 re-assert (a source that cannot be cleared) */
        l->guard.note(s,l->window.get(0));
    }
    static void mask(void *c) {Line *l=static_cast<Line *>(c);++l->masked;l->window.set(4,0xFFFFFFFF);}
};

void genericGuard()
{
    Rig *rig=fresh();
    (void)rig;
    static Line hfi;
    gGic.level=[](uint32_t intid) {return intid==336 && hfi.window.get(0)!=0 && hfi.window.get(4)!=0xFFFFFFFF;};
    EXPECT(hfi.guard.attach(a830IrqArmCpu(),336,false,A830IrqOffHfi,A830_IRQ_STORM_OTHER,Line::handler,&hfi,Line::mask,&hfi,true)==
           kIOReturnSuccess);
    EXPECT(hfi.guard.readback()==A830IrqReadbackLevel);
    /* a source that clears is progress */
    hfi.window.set(0,0x6);
    EXPECT(gGic.deliver(336)==1 && hfi.guard.stalls()==0 && !hfi.guard.off());
    /* a bit that survives its clear: 64 entries, then the device-side mask and armcpu_spi_disable */
    hfi.window.set(0,0x8000);hfi.window.set(8,0x8000);
    EXPECT(gGic.deliver(336)==A830_IRQ_STORM_OTHER);
    EXPECT(hfi.guard.off() && hfi.masked==1 && gGic.disables==1 && (adreno830_status.irq_off&A830IrqOffHfi));
    EXPECT(hfi.guard.rearm()==kIOReturnSuccess && !hfi.guard.off() && gGic.enables==1 && !(adreno830_status.irq_off&A830IrqOffHfi));
    /* an edge line (994) never trips */
    static Line waitq;
    EXPECT(waitq.guard.attach(a830IrqArmCpu(),994,true,A830IrqOffScmWaitq,0,Line::handler,&waitq,nullptr,nullptr,true)==
           kIOReturnSuccess);
    for(unsigned i=0;i<1000;++i)EXPECT(gGic.pulse(994));
    EXPECT(!waitq.guard.off() && waitq.guard.entries()==1000);
}

void optionalLinesOnlyRecord()
{
    Rig *rig=fresh();
    (void)rig;
    /* attach refused: irq_off bit, error returned, the owner polls */
    static Line cb0;
    gGic.attach_result=kIOReturnNoResources;
    EXPECT(cb0.guard.attach(a830IrqArmCpu(),710,false,A830IrqOffSmmuCb0,0,Line::handler,&cb0,Line::mask,&cb0,true)==
           kIOReturnNoResources);
    EXPECT(cb0.guard.off() && !cb0.guard.attached() && (adreno830_status.irq_off&A830IrqOffSmmuCb0));
    /* readback not Group1/enabled/level/0xA0: the line is masked, disabled again and reported off */
    gGic.attach_result=kIOReturnSuccess;gGic.forced_readback=0xA002;
    static Line cb2;
    EXPECT(cb2.guard.attach(a830IrqArmCpu(),712,false,A830IrqOffSmmuCb2,0,Line::handler,&cb2,Line::mask,&cb2,true)==
           kIOReturnIOError);
    EXPECT(cb2.guard.off() && cb2.masked==1 && gGic.disables==1 && (adreno830_status.irq_off&A830IrqOffSmmuCb2));
    /* no export at all */
    static Line gmu;
    EXPECT(gmu.guard.attach(&kNoAttach,337,false,A830IrqOffGmu,0,Line::handler,&gmu,Line::mask,&gmu,true)==kIOReturnUnsupported);
    EXPECT(adreno830_status.irq_off&A830IrqOffGmu);
    /* a level line without a device-side mask is refused */
    static Line nomask;
    EXPECT(nomask.guard.attach(a830IrqArmCpu(),713,false,0,0,Line::handler,&nomask,nullptr,nullptr,true)==kIOReturnBadArgument);
}

void cxHost()
{
    Rig *none=fresh();
    EXPECT(none->irq.attachCxHost(kCxIntid)==kIOReturnUnsupported && (adreno830_status.irq_off&A830IrqOffCxHost));
    Rig *rig=fresh(true,nullptr,true);
    const uint32_t status=A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_STATUS),mask=A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_INT_0_MASK);
    rig->cx.set(status,0x5);   /* stale */
    EXPECT(rig->irq.attachCxHost(kCxIntid)==kIOReturnSuccess);
    EXPECT(rig->cx.get(status)==0 && rig->cx.get(mask)==A8XX_CX_MISC_INT_GPU_CC_IRQ && !(adreno830_status.irq_off&A830IrqOffCxHost));
    rig->cx.set(status,A8XX_CX_MISC_INT_GPU_CC_IRQ);
    EXPECT(gGic.deliver(kCxIntid)==1 && rig->irq.stats().cx_freq_limits==1 && rig->cx.get(status)==0);
    /* a stuck status trips the guard after 64 entries: CX_MISC mask 0, SPI disabled */
    rig->cx.clear_offset=~0u;   /* clears no longer work */
    rig->cx.set(status,A8XX_CX_MISC_INT_GPU_CC_IRQ);
    EXPECT(gGic.deliver(kCxIntid)==A830_IRQ_STORM_OTHER && rig->irq.cxGuard().off() && rig->cx.get(mask)==0);
    EXPECT(adreno830_status.irq_off&A830IrqOffCxHost);
}

void quiesceAndRestore()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_RB_DONE_TS);
    EXPECT(rig->irq.quiesce(false)==kIOReturnBusy && rig->gpu.maskNow()==A8XX_INT_MASK);   /* slumber gives up */
    EXPECT(gGic.deliver(kGpuIntid)==1);
    EXPECT(rig->irq.quiesce(false)==kIOReturnSuccess && rig->gpu.maskNow()==0 && !gGic.enabled(kGpuIntid));
    EXPECT(!(adreno830_status.irq_off&A830IrqOffGpu));   /* a suspension is not a storm */
    /* GX off: a spurious entry touches no register at all */
    gGic.find(kGpuIntid)->enabled=true;
    const uint64_t before=rig->gpu.accessCount();
    EXPECT(gGic.pulse(kGpuIntid) && rig->gpu.accessCount()==before);
    gGic.find(kGpuIntid)->enabled=false;
    /* the device side was re-initialized (A830Cp programmed the mask): restore enables the line again */
    rig->gpu.mask=A8XX_INT_MASK;rig->gpu.raise(A8XX_RBBM_INT_0_PM4CPINTERRUPT);   /* stale, cleared by restore */
    EXPECT(rig->irq.restore()==kIOReturnSuccess && gGic.enabled(kGpuIntid) && rig->gpu.raw==0);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.fenceIrqs()==1);
    /* recovery forces it even with something pending */
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_RB_DONE_TS);
    EXPECT(rig->irq.quiesce(true)==kIOReturnSuccess);
    EXPECT(rig->irq.restore()==kIOReturnSuccess);
}

void ifpcKeepalive()
{
    Rig *rig=fresh(true,nullptr,false,true);
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->irq.setIfpc(true);
    const uint32_t keepalive=A830_GMU_REG_BYTE(A8XX_GMUCX_PWR_COL_KEEPALIVE),fence=A830_GMU_REG_BYTE(A8XX_GMUAO_AHB_FENCE_CTRL);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.fenceIrqs()==1);
    {
        std::lock_guard<std::mutex> g(rig->gmu.m);
        const auto &w=rig->gmu.writes;
        EXPECT(w.size()==2 && w[0].dword==keepalive && w[0].value==1 && w[1].dword==keepalive && w[1].value==0);
    }
    /* the AHB fence never opens: no GX access, a GMU fault from thread context (KGSL gen8_irq_poll_fence) */
    rig->gmu.set(fence,1);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    const uint64_t delayed=a830HostDelayed(),accesses=rig->gpu.accessCount();
    EXPECT(gGic.pulse(kGpuIntid));
    EXPECT(rig->gpu.accessCount()==accesses && a830HostDelayed()-delayed==100 && rig->irq.stats().keepalive_timeouts==1);
    EXPECT(rig->gmu.get(keepalive)==0);
    a830HostRunCalls(true);
    EXPECT(sink.lost==1 && sink.report.kind==A830IrqFaultAhbFence && sink.report.reason==A830ReasonGmuFault);
    EXPECT(adreno830_status.gmu_faults==1);
}

void escalateFromElsewhere()
{
    Rig *rig=fresh();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    rig->irq.escalate(A830IrqFaultWptr,A830ReasonUnexpectedState,200);
    EXPECT(sink.lost==1 && sink.report.kind==A830IrqFaultWptr && sink.report.detail==200 && rig->gpu.maskNow()==0);
    rig->irq.escalate(A830IrqFaultWatchdog,A830ReasonHang,2000);
    EXPECT(sink.lost==1 && a830_fault_last.kind==A830IrqFaultWatchdog);   /* recorded, not dispatched twice */
    EXPECT(adreno830_status.faults==1);
    /* after the recovery restored the device, a new loss is dispatched again */
    rig->gpu.mask=A8XX_INT_MASK;
    EXPECT(rig->irq.restore()==kIOReturnSuccess);
    rig->irq.escalate(A830IrqFaultGmu,A830ReasonGmuFault,0x8000);
    EXPECT(sink.lost==2 && sink.report.kind==A830IrqFaultGmu && adreno830_status.gmu_faults==1);
}

/* The annibale profile's pagani-gpu-irq=1 (A830IrqConfig::own_mask = the fence interrupt only): hw_init left RBBM_INT_0_MASK
 * at 0; attach unmasks the fence bit once the line is attached, error bits stay masked; a quiesce and the next hw_init mask
 * everything again and restore() unmasks the fence bit; fallBack() turns 332 off for the boot (masked, SPI disabled,
 * irq_off bit 0) and restore() leaves it so; a refused attach writes no mask; the storm guard also trips on a fence bit the
 * clear does not take. Without own_mask (design) neither attach nor restore writes the mask. */
unsigned ownMaskCases;
void ownMaskAndFallBack()
{
    Rig *rig=fresh(true,nullptr,false,false,A830_INT_MASK_FENCE_ONLY);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);   /* the CP init fence of S6, latched while masked */
    rig->gpu.startLog();
    EXPECT(rig->irq.attach(kGpuIntid)==kIOReturnSuccess);
    auto log=rig->gpu.stopLog();
    EXPECT(rig->gpu.maskNow()==A830_INT_MASK_FENCE_ONLY && rig->gpu.raw==0 && gGic.enabled(kGpuIntid) && rig->irq.attached() &&
           !rig->irq.off() && !rig->irq.polled());
    EXPECT(!log.empty() && log.back().write && log.back().dword==A8XX_RBBM_INT_0_MASK && log.back().value==A830_INT_MASK_FENCE_ONLY &&
           touched(log,A8XX_RBBM_INT_CLEAR_CMD,true));
    /* the fence interrupt wakes and retires; error bits stay masked (no entry, nothing lost) */
    rig->fence.submitted(1);
    rig->memptrs[1]=1;
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.fenceIrqs()==1 && rig->fence.irqSeq()==1);
    EXPECT(a830HostRunCalls(true)>=1 && sink.retires==1 && sink.last_completed==1);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_HW_ERROR|A8XX_RBBM_INT_0_HANG_DETECT);
    EXPECT(gGic.deliver(kGpuIntid)==0 && rig->irq.stats().errors==0 && sink.lost==0);
    rig->gpu.raw=0;
    /* slumber or recovery: quiesce masks everything, annibale's hw_init leaves 0, restore unmasks the fence bit again */
    EXPECT(rig->irq.quiesce(true)==kIOReturnSuccess && rig->gpu.maskNow()==0 && !gGic.enabled(kGpuIntid));
    rig->gpu.mask=0;
    EXPECT(rig->irq.restore()==kIOReturnSuccess && rig->gpu.maskNow()==A830_INT_MASK_FENCE_ONLY && gGic.enabled(kGpuIntid));
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    EXPECT(gGic.deliver(kGpuIntid)==1 && rig->irq.fenceIrqs()==2);
    /* fallBack: off for the boot, and a restore keeps it off */
    rig->irq.fallBack();
    EXPECT(rig->irq.off() && rig->irq.polled() && rig->gpu.maskNow()==0 && !gGic.enabled(kGpuIntid) &&
           (adreno830_status.irq_off&A830IrqOffGpu));
    rig->gpu.mask=0;
    EXPECT(rig->irq.restore()==kIOReturnSuccess && rig->irq.off() && rig->gpu.maskNow()==0 && !gGic.enabled(kGpuIntid) &&
           (adreno830_status.irq_off&A830IrqOffGpu));
    /* with 332 off, the waiter that sees its fence pass kicks the retirement itself */
    rig->fence.submitted(2);
    rig->memptrs[1]=2;
    EXPECT(rig->fence.waitNs(2,0)==kIOReturnSuccess && a830HostRunCalls(true)>=1 && sink.last_completed==2);
    ++ownMaskCases;
    /* a refused attach: no mask written, off, and restore cannot bring an unattached line up */
    Rig *refused=fresh(true,&kRefuse,false,false,A830_INT_MASK_FENCE_ONLY);
    refused->gpu.startLog();
    EXPECT(refused->irq.attach(kGpuIntid)==kIOReturnNoResources && refused->irq.off() && !refused->irq.attached());
    log=refused->gpu.stopLog();
    EXPECT(!touched(log,A8XX_RBBM_INT_0_MASK,true) && refused->gpu.maskNow()==0 && (adreno830_status.irq_off&A830IrqOffGpu));
    refused->irq.restore();
    EXPECT(refused->gpu.maskNow()==0 && refused->irq.off());
    ++ownMaskCases;
    /* the storm guard: a fence bit that survives its clear trips it after A830_IRQ_STORM_GPU entries */
    Rig *storm=fresh(true,nullptr,false,false,A830_INT_MASK_FENCE_ONLY);
    EXPECT(storm->irq.attach(kGpuIntid)==kIOReturnSuccess);
    storm->gpu.stuck=A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS;
    EXPECT(gGic.deliver(kGpuIntid)==A830_IRQ_STORM_GPU && storm->irq.off() && storm->gpu.maskNow()==0 &&
           !gGic.enabled(kGpuIntid) && (adreno830_status.irq_off&A830IrqOffGpu) && !storm->irq.polled());
    /* a storm is the design's case: the recovery's restore arms it again (with the fence-only mask) */
    storm->gpu.stuck=0;storm->gpu.mask=0;
    EXPECT(storm->irq.restore()==kIOReturnSuccess && !storm->irq.off() && storm->gpu.maskNow()==A830_INT_MASK_FENCE_ONLY &&
           gGic.enabled(kGpuIntid) && !(adreno830_status.irq_off&A830IrqOffGpu));
    ++ownMaskCases;
    /* the design: the mask is A830Cp's, never written by attach or restore */
    Rig *design=fresh();
    design->gpu.startLog();
    EXPECT(design->irq.attach(kGpuIntid)==kIOReturnSuccess);
    EXPECT(design->irq.quiesce(true)==kIOReturnSuccess);
    design->gpu.mask=A8XX_INT_MASK;   /* hw_init programmed it again */
    EXPECT(design->irq.restore()==kIOReturnSuccess);
    log=design->gpu.stopLog();
    EXPECT(std::count_if(log.begin(),log.end(),[](const FakeAccess &a) {return a.dword==A8XX_RBBM_INT_0_MASK && a.write;})==1 &&
           design->gpu.maskNow()==A8XX_INT_MASK);   /* the one write is quiesce's 0 */
    ++ownMaskCases;
}

void mmioDirect()
{
    uint32_t window[4]={1,2,3,4};
    const A830Mmio m=a830MmioDirect(window);
    EXPECT(m.valid() && m.read32(m.cookie,8)==3);
    m.write32(m.cookie,12,0x55);
    EXPECT(window[3]==0x55);
}
}

int main()
{
    a830HostManualCalls(true);
    mmioDirect();
    attachClearsAndWires();
    attachFailures();
    fenceTopHalf();
    errorsOnlyQueued();
    cpHwErrorIsGlobal();
    hangWhileSmmuStalledIsNotAHang();
    hangWithoutSinkLosesTheDevice();
    stormStuckErrorBit();
    stormSpurious();
    stormWithoutDisableExport();
    genericGuard();
    optionalLinesOnlyRecord();
    cxHost();
    quiesceAndRestore();
    ifpcKeepalive();
    escalateFromElsewhere();
    ownMaskAndFallBack();
    a830HostRunCalls(true);
    a830HostShutdown();
    std::printf("PASS: A830Irq %u checks: 332 wiring and top half (0x6A/0x61/0x62 only), errors to faultWork, fatal masking "
                "and global loss, storm guards with and without armcpu_spi_disable, optional lines, cx_host, quiesce/restore, "
                "IFPC; pagani-gpu-irq (annibale): %u own-mask cases (fence-only mask after attach and restore, fallBack for the "
                "boot, refused attach, stuck fence bit storm, design untouched)\n",checks,ownMaskCases);
    return 0;
}
