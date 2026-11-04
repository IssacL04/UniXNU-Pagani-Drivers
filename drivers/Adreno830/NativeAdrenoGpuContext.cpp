/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGpuContext.h"
#include <IOKit/IOLib.h>

static const uint64_t kIovas[GpuBufferCount]={GpuSqeIova,GpuAqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
static const size_t kBytes[GpuBufferCount]={GpuSqeBytes,GpuAqeBytes,GpuRingBytes,GpuPageBytes,GpuPageBytes,GpuPageBytes};
/* ARCHITECTURE 4.6.2/4.6.3: the microcode is read and executed by the CP and readable without privilege (adreno_fw_create_bo maps
 * it MSM_BO_GPU_READONLY without NOEXEC; PTE 0x...EC3); the ring is privileged read-only (0x0060...E83); memptrs, pwrup and
 * scratch privileged read/write (0x0060...E03): CP fetches of the ring, the RPTR shadow and in-ring packets run with APRIV,
 * user IBs without it, so a user IB can reach neither. */
static const unsigned kPermissions[GpuBufferCount]={
    ArmSmmu4KRead|ArmSmmu4KExecute,ArmSmmu4KRead|ArmSmmu4KExecute,ArmSmmu4KRead|ArmSmmu4KPrivileged,
    ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,
    ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged};
static NativeAdrenoGpuContext *board_gpu_context;
/* The input size a stage-1 context can take, from IDR2.UBS (bits 11:8, {32,36,40,42,44,48}): the bound the kit's encoder checks the
 * context's input bits against (ArmSmmu4KEncodeContext). IDR2.IAS (bits 3:0) is the stage-2 input size and bounds nothing here. 0 for
 * an encoding the kit cannot use. SM8750 reports UBS 48 bits, IAS 36 (A830_SMMU_IDR2_SM8750 0x5511). */
static unsigned smmuInputBits(uint32_t idr2)
{
    static const uint8_t bits[6]={32,36,40,42,44,48};
    const unsigned code=(idr2>>8)&15;
    return code>5?0u:bits[code];
}

IOReturn NativeAdrenoGpuContext::bringUp(const NativeAdrenoGpuContextConfig &config,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    const NativeAdrenoSmmuProgrammer *programmer=config.programmer;
    if(!programmer || config.bank>63 || !config.asid || config.asid>0xffff || config.slot>127 || config.lpac_slot>127 ||
        config.slot==config.lpac_slot || config.sid==config.lpac_sid || config.sid>0x7fff || config.lpac_sid>0x7fff ||
        (config.aperture_gib!=2 && config.aperture_gib!=4 && config.aperture_gib!=16) || config.output_bits>48)
        return kIOReturnBadArgument;
    used_=true;
    IOReturn status=kIOReturnSuccess;
    auto step=[&](Step s) {report->step=s;if(progress)progress(s);};
    step(Allocate);
    unsigned bits=config.output_bits?config.output_bits:NativeAdrenoSmmuOutputBits(programmer->id2);
    if(bits>40)bits=40;
    output_bits_=bits;report->output_bits=bits;
    if(!bits)status=kIOReturnUnsupported;
    /* pagani-gpu-va: 16 GiB (8209 table pages, a 34-bit input) by default for 32 client windows, 4 GiB (2053), 2 GiB (1027) as the old
     * fallback (ARCHITECTURE 4.6.1, 6.2). 16 GiB needs IDR2.UBS of 34 bits or more and a 32 MiB contiguous table pool; without either
     * the context takes the 4 GiB aperture, and the publish shell the 4 GiB windows (apertureBytes()). */
    const bool wide=config.aperture_gib==16 && smmuInputBits(programmer->id2)>=34;
    aperture_=config.aperture_gib==2?uint64_t(GpuApertureBytes2G):wide?uint64_t(GpuApertureBytes16G):uint64_t(GpuApertureBytes);
    size_t pages=config.aperture_gib==2?TablePages2G:wide?TablePages16G:TablePages;
    /* The BO pool first, while the VM still has long contiguous runs (normally the orchestrator reserved it before CB2; then this
     * only reports it). Not fatal: without it each BO is scattered and changes its own pages' attribute. */
    if(!status && config.pool_mib)
        status=NativeAdrenoMemory::reservePoolBestEffort(bits,config.pool_mib,16,config.write_combined_pool,&pool_mib_);
    if(!status) {
        status=tables_.allocate(pages*0x1000,bits,true,pages*0x1000);
        if(status && wide && !tables_.quarantined()) {
            aperture_=GpuApertureBytes;pages=TablePages;
            status=tables_.allocate(pages*0x1000,bits,true,pages*0x1000);
        }
    }
    report->table_pages=uint32_t(pages);
    if(!status)status=NativeAdrenoPrepareGuardTable(&guard_,bits);
    /* The kernel buffers: contiguous, zeroed, then write-combined for every CPU mapping (barriers only from here on). */
    for(unsigned i=0;!status && i<GpuBufferCount;++i) {
        status=buffers_[i].allocate(kBytes[i],bits,true,kBytes[i]);
        if(!status)status=buffers_[i].makeWriteCombined();
    }
    if(!status && !(alias_scratch_=IONewData(uint32_t,AliasScratchEntries)))status=kIOReturnNoMemory;
    if(!status) {
        step(Prepare);
        status=smmu_.prepare(&tables_,0,0,aperture_,size_t(aperture_/4096),config.bank,config.asid,4000,bits,inputBits());
        if(!status)smmu_.setAliasScratch(alias_scratch_,AliasScratchEntries);
        if(!status)status=smmu_.setUpperGuard(guard_.spans()[0].physical);
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
    }
    if(!status) {
        /* ARCHITECTURE 4.5.5: SID1 -> slot 3 -> CB0 once CB0 is enabled and before the SQE starts; without it BR/BV stop at
         * THREAD_SYNC (PC 0xE3/0x4A) and the RPTR never moves. */
        step(Lpac);
        status=smmu_.addStream({config.lpac_slot,config.lpac_sid});
    }
    report->streams=uint32_t(smmu_.attached()?smmu_.streamCount():0);
    if(!status) {
        step(VmAttach);
        NativeAdrenoVmRegion lower={&tables_,smmu_.arenaHandle(),smmu_.stateHandle()};
        /* 262144 spans: 1 GiB of BOs even when every page is its own span. The Metal runtime pools its memory in blocks of up to 64 MiB,
         * which XNU rarely finds physically contiguous; one scattered block is 16384 spans (int-070 ran out at 16384). 131072 until the
         * windows of the 16 GiB table (2026-11-01): a heap of 448 MiB takes blocks of 16 MiB where 124 MiB took 4, and run 20261031-223924
         * was already refusing mappings at 12 clients (vmr 37, records or ranges: the line did not tell them apart then), with 98304 spans
         * left past the render server's reserve. */
        status=vm_.attach(smmu_.binding(),&lower,nullptr,262144,262144);
        /* The null guard and the GMEM window are never mapped (ARCHITECTURE 4.6.2). */
        if(!status)status=vm_.reserveWindow(0,GpuNullGuardBytes);
        if(!status)status=vm_.reserveWindow(GpuGmemIova,GpuGmemBytes);
    }
    if(!status) {
        step(Map);
        for(unsigned i=0;!status && i<GpuBufferCount;++i)
            status=vm_.map(&buffers_[i],0,kIovas[i],kPermissions[i],0,2,&handles_[i]);
        report->spans=uint32_t(vm_.spanCount());
    }
    if(!status) {
        step(Ready);
        ready_=true;
        board_gpu_context=this;
    }
    report->status=status;
    return status;
}
IOReturn NativeAdrenoGpuContext::buffers(NativeAdrenoGpuBuffer out[GpuBufferCount]) const
{
    if(!out)return kIOReturnBadArgument;
    for(unsigned i=0;i<GpuBufferCount;++i)out[i]={};
    if(!ready_)return kIOReturnNotReady;
    for(unsigned i=0;i<GpuBufferCount;++i) {
        const NativeAdrenoMemory &m=buffers_[i];
        if(m.spanCount()!=1 || m.spans()[0].bytes!=m.bytes())return kIOReturnUnsupported;
        out[i]={m.spans()[0].physical,m.bytes(),kIovas[i],m.mapping()};
    }
    return kIOReturnSuccess;
}
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[GpuBufferCount])
{
    if(!out)return kIOReturnBadArgument;
    if(!board_gpu_context) {
        for(unsigned i=0;i<GpuBufferCount;++i)out[i]={};
        return kIOReturnNotReady;
    }
    return board_gpu_context->buffers(out);
}
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm()
{
    return board_gpu_context?board_gpu_context->vm():nullptr;
}
