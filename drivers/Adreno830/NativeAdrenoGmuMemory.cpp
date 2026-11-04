/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuMemory.h"
#include <IOKit/IOLib.h>
#include <string.h>

namespace {
enum : uint32_t {PageMask=NativeAdrenoGmuPageBytes-1};
/* ArmSmmu4K permission bits (ArmSmmu4KRead 1, Write 2, Execute 4, Privileged 8). The GMU fetches its code from the CACHE
 * regions through SID 5: AArch64 forbids a privileged fetch from an EL0-writable page, so they are privileged
 * (ARCHITECTURE 4.6.3); the data regions are privileged and execute-never. */
enum : unsigned {PermissionsCache=15u,PermissionsData=11u};

uint64_t pageDown(uint64_t value) {return value&~uint64_t(PageMask);}
uint64_t pageUp(uint64_t value) {return (value+PageMask)&~uint64_t(PageMask);}
/* [start, end) lies inside [base, limit). */
bool within(uint64_t start,uint64_t end,uint64_t base,uint64_t limit) {return start>=base && end<=limit && start<end;}

/* The CPU view is write-combined: the zeros and the firmware reach DRAM once the stores complete. */
void completeStores()
{
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

NativeAdrenoGmuRegion fixedRegion(uint32_t iova,uint32_t bytes,uint32_t kind)
{
    return {iova,bytes,kind,PermissionsData};
}

/* A plan allocate() can trust: page-aligned nonzero regions inside the GMU space, in ascending order without overlap, known
 * kinds with the permissions of their kind, exactly one persist, log and HFI region at their fixed addresses. */
bool planValid(const NativeAdrenoGmuPlan &plan)
{
    if(!plan.count || plan.count>NativeAdrenoGmuMaxRegions || (plan.table_bytes&PageMask))return false;
    uint64_t previous_end=0;
    unsigned fixed[NativeAdrenoGmuRegionKinds]={};
    for(uint32_t i=0;i<plan.count;++i) {
        const NativeAdrenoGmuRegion &r=plan.region[i];
        if(!r.bytes || ((r.iova|r.bytes)&PageMask) || uint64_t(r.iova)+r.bytes>GmuApertureBytes)return false;
        if(r.iova<previous_end)return false;
        previous_end=uint64_t(r.iova)+r.bytes;
        if(r.kind>=NativeAdrenoGmuRegionKinds)return false;
        ++fixed[r.kind];
        if(r.kind==NativeAdrenoGmuRegionCache) {
            if(r.permissions!=PermissionsCache || !within(r.iova,previous_end,NativeAdrenoGmuCacheIova,NativeAdrenoGmuCacheEnd))
                return false;
            continue;
        }
        static const uint32_t at[NativeAdrenoGmuRegionKinds][2]={{0,0},
            {NativeAdrenoGmuPersistIova,NativeAdrenoGmuPersistBytes},{NativeAdrenoGmuLogIova,NativeAdrenoGmuLogBytes},
            {NativeAdrenoGmuHfiIova,NativeAdrenoGmuHfiBytes}};
        if(r.permissions!=PermissionsData || r.iova!=at[r.kind][0] || r.bytes!=at[r.kind][1])return false;
    }
    return fixed[NativeAdrenoGmuRegionPersist]==1 && fixed[NativeAdrenoGmuRegionLog]==1 && fixed[NativeAdrenoGmuRegionHfi]==1;
}
}

IOReturn NativeAdrenoGmuPlanRequests(const NativeAdrenoGmuRequest *requests,size_t count,size_t table_bytes,
    NativeAdrenoGmuPlan *plan)
{
    if(!plan)return kIOReturnBadArgument;
    *plan={};
    if((count && !requests) || (table_bytes&PageMask))return kIOReturnBadArgument;
    NativeAdrenoGmuPlan built{};
    NativeAdrenoGmuRegion cache[NativeAdrenoGmuMaxCacheRegions]{};
    uint32_t caches=0;
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoGmuRequest &request=requests[i];
        if(!request.bytes || (request.type!=NativeAdrenoGmuRequestPrealloc && request.type!=NativeAdrenoGmuRequestPersist))
            return kIOReturnBadArgument;
        const uint64_t start=request.iova,end=start+request.bytes;
        /* TCM requests: the TCMs are written through CM3_ITCM_START/CM3_DTCM_START, nothing to allocate (KGSL
         * gen8_gmu_process_prealloc returns early for GMU_ITCM/GMU_DTCM). */
        if(within(start,end,NativeAdrenoGmuItcmIova,NativeAdrenoGmuItcmIova+NativeAdrenoGmuItcmBytes) ||
            within(start,end,NativeAdrenoGmuDtcmIova,NativeAdrenoGmuDtcmIova+NativeAdrenoGmuDtcmBytes)) {
            ++built.tcm_requests;
            continue;
        }
        if(within(start,end,NativeAdrenoGmuCacheIova,NativeAdrenoGmuCacheEnd)) {
            const uint64_t low=pageDown(start),high=pageUp(end);
            if(low<NativeAdrenoGmuCacheIova || high>NativeAdrenoGmuCacheEnd)return kIOReturnBadArgument;
            bool covered=false;
            for(uint32_t c=0;c<caches;++c) {
                const uint64_t base=cache[c].iova,limit=base+cache[c].bytes;
                if(low>=base && high<=limit) {covered=true;break;}
                if(low<limit && high>base)return kIOReturnBadArgument;   /* partial overlap: no single region holds it */
            }
            if(covered) {++built.covered;continue;}
            if(caches==NativeAdrenoGmuMaxCacheRegions)return kIOReturnBadArgument;
            cache[caches++]={uint32_t(low),uint32_t(high-low),NativeAdrenoGmuRegionCache,PermissionsCache};
            continue;
        }
        /* Uncached kernel space: the fixed layout keeps 0x60000000-0x60004000 for persistent requests (two 4 KiB blocks on
         * PKX110); the log and the HFI follow at fixed addresses, so a request anywhere else cannot be backed. */
        if(within(start,end,NativeAdrenoGmuPersistIova,NativeAdrenoGmuPersistIova+NativeAdrenoGmuPersistBytes)) {
            if(request.type==NativeAdrenoGmuRequestPersist)++built.persist_requests;else ++built.covered;
            continue;
        }
        return kIOReturnBadArgument;
    }
    /* Address order: insertion sort of at most 13 entries. */
    for(uint32_t i=1;i<caches;++i)
        for(uint32_t j=i;j && cache[j-1].iova>cache[j].iova;--j) {
            const NativeAdrenoGmuRegion swap=cache[j-1];cache[j-1]=cache[j];cache[j]=swap;
        }
    for(uint32_t i=0;i<caches;++i) {
        built.region[built.count++]=cache[i];
        built.cache_bytes+=cache[i].bytes;
    }
    built.region[built.count++]=fixedRegion(NativeAdrenoGmuPersistIova,NativeAdrenoGmuPersistBytes,NativeAdrenoGmuRegionPersist);
    built.region[built.count++]=fixedRegion(NativeAdrenoGmuLogIova,NativeAdrenoGmuLogBytes,NativeAdrenoGmuRegionLog);
    built.region[built.count++]=fixedRegion(NativeAdrenoGmuHfiIova,NativeAdrenoGmuHfiBytes,NativeAdrenoGmuRegionHfi);
    built.table_bytes=table_bytes;
    *plan=built;
    return kIOReturnSuccess;
}

int NativeAdrenoGmuMemory::indexOf(uint32_t kind) const
{
    if(!ready_)return -1;
    for(uint32_t i=0;i<plan_.count;++i)if(plan_.region[i].kind==kind)return int(i);
    return -1;
}

IOReturn NativeAdrenoGmuMemory::allocate(const NativeAdrenoGmuPlan &plan,unsigned address_bits)
{
    if(ready_ || tables_.bytes())return kIOReturnBusy;
    for(const NativeAdrenoMemory &object:region_)if(object.bytes())return kIOReturnBusy;
    if(!planValid(plan) || plan.table_bytes>size_t(SIZE_MAX))return kIOReturnBadArgument;
    scattered_=0;
    for(uint32_t i=0;i<plan.count;++i) {
        const size_t bytes=plan.region[i].bytes;
        IOReturn status=region_[i].allocate(bytes,address_bits,true,bytes);
        /* A fragmented VM may have no contiguous run of this size: scattered pages work as well, each one more VM span. */
        if((status==kIOReturnNoMemory || status==kIOReturnUnsupported) && !region_[i].bytes() && !region_[i].quarantined()) {
            status=region_[i].allocate(bytes,address_bits,false,bytes);
            if(status==kIOReturnSuccess)++scattered_;
        }
        if(status==kIOReturnSuccess)status=region_[i].makeWriteCombined();
        if(status!=kIOReturnSuccess) {
            (void)releaseAll();
            return status;
        }
    }
    if(plan.table_bytes) {
        const IOReturn status=tables_.allocate(plan.table_bytes,address_bits,true,plan.table_bytes);
        if(status!=kIOReturnSuccess) {
            (void)releaseAll();
            return status;
        }
    }
    plan_=plan;
    layout_={};
    for(uint32_t i=0;i<plan.count;++i) {
        const NativeAdrenoGmuRegion &r=plan.region[i];
        if(r.kind==NativeAdrenoGmuRegionPersist)layout_.persist_iova=r.iova;
        if(r.kind==NativeAdrenoGmuRegionLog)layout_.log_iova=r.iova;
        if(r.kind==NativeAdrenoGmuRegionHfi)layout_.hfi_iova=r.iova;
        if(uint64_t(r.iova)+r.bytes>layout_.end_iova)layout_.end_iova=uint64_t(r.iova)+r.bytes;
    }
    layout_.tables_iova=0;
    layout_.tables_bytes=plan.table_bytes;
    layout_.cache_bytes=plan.cache_bytes;
    layout_.regions=plan.count;
    ready_=true;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGmuMemory::releaseAll()
{
    IOReturn status=kIOReturnSuccess;
    for(NativeAdrenoMemory &object:region_)
        if(object.bytes() && object.release()!=kIOReturnSuccess)status=kIOReturnIOError;
    if(tables_.bytes() && tables_.release()!=kIOReturnSuccess)status=kIOReturnIOError;
    return status;
}

IOReturn NativeAdrenoGmuMemory::release()
{
    if(!ready_)return kIOReturnNotReady;
    for(uint64_t handle:handles_)if(handle)return kIOReturnBusy;
    const IOReturn status=releaseAll();
    if(status)return status;
    ready_=false;layout_={};plan_={};scattered_=0;
    return kIOReturnSuccess;
}

int NativeAdrenoGmuMemory::find(uint64_t iova,uint64_t bytes) const
{
    if(!ready_ || !bytes || iova+bytes<iova)return -1;
    for(uint32_t i=0;i<plan_.count;++i) {
        const NativeAdrenoGmuRegion &r=plan_.region[i];
        if(iova>=r.iova && iova+bytes<=uint64_t(r.iova)+r.bytes)return int(i);
    }
    return -1;
}

void *NativeAdrenoGmuMemory::cpu(uint64_t iova,uint64_t bytes) const
{
    const int index=find(iova,bytes);
    if(index<0 || !region_[index].mapping())return nullptr;
    return static_cast<uint8_t *>(region_[index].mapping())+(iova-plan_.region[index].iova);
}

IOReturn NativeAdrenoGmuMemory::clear(uint32_t kinds)
{
    if(!ready_)return kIOReturnNotReady;
    if(!kinds || (kinds>>NativeAdrenoGmuRegionKinds))return kIOReturnBadArgument;
    for(uint32_t i=0;i<plan_.count;++i)
        if(kinds&(1u<<plan_.region[i].kind) && region_[i].mapping())memset(region_[i].mapping(),0,region_[i].bytes());
    completeStores();
    return kIOReturnSuccess;
}
