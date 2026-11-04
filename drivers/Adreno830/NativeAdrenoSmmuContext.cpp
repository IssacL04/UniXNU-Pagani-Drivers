/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSmmuContext.h"
#include <IOKit/IOLib.h>

static bool validProgrammer(const NativeAdrenoSmmuProgrammer *p)
{
    return p && p->io && p->claim && p->program && p->route && p->verify && p->fault && p->disable;
}
IOReturn NativeAdrenoSmmuContext::prepare(NativeAdrenoMemory *tables,unsigned upper,uint64_t iova,
    size_t bytes,size_t leaf_budget,unsigned bank,unsigned asid,unsigned poll_budget,unsigned output_bits,unsigned input_bits)
{
    if(prepared_ || poisoned_)return kIOReturnBusy;
    /* ASID 0 is refused: ASID = bank + 1 keeps every bank's ASID unique while SCTLR.ASIDPNE does not stick (ARCHITECTURE 1.3 #3). */
    if(!tables || upper>1 || !bytes || (iova&4095) || !leaf_budget || leaf_budget>MaxLeaves ||
        bank>63 || !asid || asid>0xffff || !poll_budget || poll_budget>1048576 || input_bits<32 || input_bits>48)
        return kIOReturnBadArgument;
    const IOReturn prepared=NativeAdrenoPrepareSmmuMemory(tables,input_bits,output_bits,iova,bytes,upper,leaf_budget,
        &arena_,&state_,&visibility_);
    if(prepared!=kIOReturnSuccess)return prepared==kIOReturnNoResources?prepared:kIOReturnBadArgument;
    /* The adreno SMMU node is dma-coherent, so table walks are inner write-back (TCR IRGN/ORGN WBWA, SH IS). Terminate on a fault
     * (no stall) with HUPCF: a faulting transaction reads zeros or is dropped and the GPU does not hang (ARCHITECTURE 4.12.3). */
    config_=(ArmSmmu4KContextConfig){.lower=upper?nullptr:&arena_.config,.upper=upper?&arena_.config:nullptr,
        .lower_tables=upper?nullptr:&state_.tables,.upper_tables=upper?&state_.tables:nullptr,.id0=0,.id2=0,
        .asid=asid,.coherent_walk=1,.stall_on_fault=0,.continue_after_fault=1};
    tables_=tables;bank_=bank;poll_budget_=poll_budget;prepared_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::setUpperGuard(uint64_t physical)
{
    if(!prepared_ || programmed_ || poisoned_)return kIOReturnNotReady;
    /* Only a lower context has a free TTBR1; the guard must lie below the context's output size and be page aligned. */
    if(config_.upper || (physical&4095) || (physical && physical>=(UINT64_C(1)<<arena_.config.output_bits)))
        return kIOReturnBadArgument;
    upper_guard_=physical;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::program(const NativeAdrenoSmmuProgrammer *programmer,const ArmSmmu4KStream *streams,size_t count)
{
    if(!prepared_ || programmed_ || poisoned_ || !validProgrammer(programmer) || !streams || !count || count>MaxStreams)
        return kIOReturnBadArgument;
    for(size_t i=0;i<count;++i)
        for(size_t j=0;j<i;++j)
            if(streams[i].slot==streams[j].slot || streams[i].sid==streams[j].sid)return kIOReturnBadArgument;
    /* The kit encodes the image from the SMMU's actual IDs (S1TS, 4K granule, UBS/OAS); the programmer writes it. */
    ArmSmmu4KContextConfig actual=config_;
    actual.id0=programmer->id0;actual.id2=programmer->id2;
    ArmSmmu4KContextImage image;
    const ArmSmmu4KStatus encoded=ArmSmmu4KEncodeContext(&actual,&image);
    if(encoded!=ArmSmmu4KSuccess)return encoded==ArmSmmu4KUnsupported?kIOReturnUnsupported:kIOReturnBadArgument;
    programmer_=*programmer;config_.id0=programmer->id0;config_.id2=programmer->id2;
    for(size_t i=0;i<count;++i)streams_[i]=streams[i];
    stream_count_=count;
    report_={};
    /* From here a failure leaves every resource where it is: the board's release path faults what was claimed. */
    for(size_t i=0;i<count;++i) {
        const IOReturn claimed=programmer_.claim(programmer_.cookie,bank_,&streams_[i]);
        if(claimed!=kIOReturnSuccess) {poisoned_=true;return claimed;}
    }
    const uint64_t ttbr1=upper_guard_?(uint64_t(config_.asid)<<48|upper_guard_):0;
    const IOReturn status=programmer_.program(programmer_.cookie,bank_,&image,ttbr1,&report_);
    if(status!=kIOReturnSuccess) {poisoned_=true;return status;}
    if(!report_.context_offset) {poisoned_=true;return kIOReturnIOError;}
    programmed_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::attach()
{
    if(!programmed_ || attached_ || poisoned_ || disabled_)return kIOReturnBadArgument;
    for(size_t i=routed_;i<stream_count_;++i) {
        const IOReturn status=programmer_.route(programmer_.cookie,bank_,&streams_[i]);
        if(status!=kIOReturnSuccess) {poisoned_=true;return status;}
        routed_=i+1;
    }
    attached_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::addStream(const ArmSmmu4KStream &stream)
{
    if(!attached_ || poisoned_ || disabled_)return kIOReturnBadArgument;
    if(stream_count_>=MaxStreams)return kIOReturnNoResources;
    for(size_t i=0;i<stream_count_;++i)
        if(streams_[i].slot==stream.slot || streams_[i].sid==stream.sid)return kIOReturnBadArgument;
    streams_[stream_count_++]=stream;
    IOReturn status=programmer_.claim(programmer_.cookie,bank_,&stream);
    if(status==kIOReturnSuccess)status=programmer_.route(programmer_.cookie,bank_,&stream);
    if(status!=kIOReturnSuccess) {poisoned_=true;return status;}
    routed_=stream_count_;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::observe() const
{
    if(!attached_ || poisoned_ || disabled_)return kIOReturnBadArgument;
    return programmer_.verify(programmer_.cookie,bank_,streams_,stream_count_);
}
IOReturn NativeAdrenoSmmuContext::disable()
{
    if(!attached_ || poisoned_ || disabled_)return kIOReturnBadArgument;
    /* ARCHITECTURE 4.5.7: streams FAULT first (SMRs kept), then SCTLR = 0 with TLBIALL and TLBSYNC. */
    IOReturn status=programmer_.fault(programmer_.cookie,bank_,streams_,stream_count_);
    if(status==kIOReturnSuccess)status=programmer_.disable(programmer_.cookie,bank_);
    if(status!=kIOReturnSuccess) {poisoned_=true;return status;}
    attached_=false;disabled_=true;
    return kIOReturnSuccess;
}
NativeAdrenoSmmuBinding NativeAdrenoSmmuContext::binding()
{
    if(!attached_ || poisoned_ || disabled_)return {};
    return {&programmer_,&config_,bank_,report_.context_offset,streams_,stream_count_};
}
