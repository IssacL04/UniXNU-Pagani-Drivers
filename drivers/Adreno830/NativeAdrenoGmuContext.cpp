/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuContext.h"
#include <IOKit/IOLib.h>

unsigned NativeAdrenoSmmuOutputBits(uint32_t idr2)
{
    /* Stage-1 output size is ID2.OAS, as arm_smmu_init_domain_context uses smmu->ipa_size; tables and payloads must then lie
     * below it. 40 bits is this driver's own allocation ceiling (SM8750 reports 36). */
    static const uint8_t bits[6]={32,36,40,42,44,48};
    const unsigned code=(idr2>>4)&15;
    if(code>5)return 0;
    return bits[code]>40?40u:bits[code];
}
IOReturn NativeAdrenoGmuContext::bringUp(const NativeAdrenoGmuContextConfig &config,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    const NativeAdrenoSmmuProgrammer *programmer=config.programmer;
    if(!programmer || config.bank>63 || !config.asid || config.asid>0xffff || config.slot>127 || config.sid>0x7fff ||
        config.output_bits>48)return kIOReturnBadArgument;
    used_=true;
    IOReturn status=kIOReturnSuccess;
    auto step=[&](Step s) {report->step=s;if(progress)progress(s);};
    step(Allocate);
    unsigned bits=config.output_bits?config.output_bits:NativeAdrenoSmmuOutputBits(programmer->id2);
    if(bits>40)bits=40;
    output_bits_=bits;report->output_bits=bits;report->table_pages=TablePages;
    if(!bits)status=kIOReturnUnsupported;
    /* Lower region 0..2 GiB: the PREALLOC ranges (0x4000.., 0x100000.., 0x200000.., 0x300000..), the persistent, log and HFI
     * areas at 0x60000000 (ARCHITECTURE 4.7.2) all lie in it. */
    if(!status)status=tables_.allocate(TablePages*0x1000,bits,true,TablePages*0x1000);
    if(!status) {
        step(Prepare);
        status=smmu_.prepare(&tables_,0,0,GmuContextApertureBytes,GmuContextApertureBytes/4096,config.bank,config.asid,4000,bits);
    }
    if(!status) {
        step(Program);
        const ArmSmmu4KStream route{config.slot,config.sid};
        status=smmu_.program(programmer,&route,1);
        report->program=*smmu_.programReport();
    }
    if(!status) {
        step(Attach);
        status=smmu_.attach();
        if(!status)report->streams=uint32_t(smmu_.streamCount());
    }
    if(!status) {
        step(VmAttach);
        NativeAdrenoVmRegion lower={&tables_,smmu_.arenaHandle(),smmu_.stateHandle()};
        /* The GMU memory is a few objects; scattered pages are one span each (0x76000 bytes of CACHE is 118 pages). */
        status=vm_.attach(smmu_.binding(),&lower,nullptr,4096,262144);
    }
    if(!status) {
        step(Map);
        if(config.map)status=config.map(config.map_cookie,&vm_);
        report->spans=uint32_t(vm_.spanCount());
    }
    if(!status) {
        step(Ready);
        ready_=true;
    }
    report->status=status;
    return status;
}
