/* SPDX-License-Identifier: MIT */
/* PREALLOC-driven GMU memory (ARCHITECTURE 4.7.2) over host IOKit doubles: the PKX110 request list of gen80000_gmu.bin, the
 * plan and its refusals, allocation with the write-combined CPU view, the scattered fallback, lookups, clearing and release.
 * No mapping and no GMU access (test_gmumap maps the regions). */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGmuMemory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static unsigned wc_calls,wc_fail;
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);
    return kIOReturnSuccess;
}
/* Metadata blocks #0..#10 of gen80000_gmu.bin (ARCHITECTURE 4.7.1). */
static const NativeAdrenoGmuRequest pkx110[]={
    {0x0,0x1ec,1},{0x4000,0x15824,1},{0x200,0x37a8,1},{0x100000,0x1b878,1},{0x200000,0x1eaa0,1},{0x300000,0x24548,1},
    {0x10004000,0x2de8,1},{0x10007000,0xf84,1},{0x10007fdc,0x18,1},{0x60000000,0x1000,7},{0x60001000,0x1000,7}};
enum : size_t {Requests=sizeof pkx110/sizeof pkx110[0],Tables=1027*4096};
static void expectPkx110(const NativeAdrenoGmuPlan &plan,size_t tables)
{
    const NativeAdrenoGmuRegion want[]={
        {0x4000,0x16000,NativeAdrenoGmuRegionCache,15},{0x100000,0x1c000,NativeAdrenoGmuRegionCache,15},
        {0x200000,0x1f000,NativeAdrenoGmuRegionCache,15},{0x300000,0x25000,NativeAdrenoGmuRegionCache,15},
        {0x60000000,0x4000,NativeAdrenoGmuRegionPersist,11},{0x60004000,0x4000,NativeAdrenoGmuRegionLog,11},
        {0x60008000,0x4000,NativeAdrenoGmuRegionHfi,11}};
    CHECK(plan.count==7);
    for(unsigned i=0;i<7;++i)
        CHECK(plan.region[i].iova==want[i].iova && plan.region[i].bytes==want[i].bytes && plan.region[i].kind==want[i].kind &&
              plan.region[i].permissions==want[i].permissions);
    CHECK(plan.cache_bytes==0x76000);             /* 472 KiB, ARCHITECTURE 4.7.2 */
    CHECK(plan.tcm_requests==5 && plan.persist_requests==2 && plan.covered==0 && plan.table_bytes==tables);
}
int main()
{
    unsigned plans=0,refusals=0,allocations=0;
    /* The plan of the PKX110 image: 4 CACHE regions rounded out to pages, persist, log and HFI at their fixed addresses. */
    NativeAdrenoGmuPlan plan{};
    CHECK(NativeAdrenoGmuPlanRequests(pkx110,Requests,Tables,&plan)==kIOReturnSuccess);
    expectPkx110(plan,Tables);++plans;
    {
        NativeAdrenoGmuRequest reversed[Requests];
        for(size_t i=0;i<Requests;++i)reversed[i]=pkx110[Requests-1-i];
        NativeAdrenoGmuPlan again{};
        CHECK(NativeAdrenoGmuPlanRequests(reversed,Requests,Tables,&again)==kIOReturnSuccess);
        expectPkx110(again,Tables);++plans;
        /* A request inside an earlier region needs nothing (KGSL find_gmu_memdesc); a persist-range PREALLOC is covered. */
        NativeAdrenoGmuRequest more[Requests+2];
        std::memcpy(more,pkx110,sizeof pkx110);
        more[Requests]={0x5000,0x100,1};more[Requests+1]={0x60002000,0x1000,1};
        CHECK(NativeAdrenoGmuPlanRequests(more,Requests+2,Tables,&again)==kIOReturnSuccess);
        CHECK(again.covered==2 && again.count==7 && again.cache_bytes==0x76000);++plans;
        /* Only TCM requests: the fixed regions remain. */
        CHECK(NativeAdrenoGmuPlanRequests(pkx110,1,Tables,&again)==kIOReturnSuccess && again.count==3 && !again.cache_bytes);
        ++plans;
    }
    /* Refusals: each leaves an empty plan. */
    {
        const NativeAdrenoGmuRequest bad[][1]={
            {{0x4000,0,1}},                 /* zero bytes */
            {{0x4000,0x1000,3}},            /* not a PREALLOC type */
            {{0x60004000,0x1000,7}},        /* uncached outside the persistent region: the log lives there */
            {{0x2000000,0x1000,1}},         /* in no GMU range */
            {{0x3000,0x2000,1}},            /* crosses ITCM into CACHE */
            {{0xfff000,0x2000,1}},          /* crosses the CACHE end */
            {{0x10007000,0x2000,1}},        /* crosses the DTCM end */
            {{0x80000000,0x1000,7}}};       /* past the GMU space */
        for(const auto &request:bad) {
            NativeAdrenoGmuPlan refused{};refused.count=99;
            CHECK(NativeAdrenoGmuPlanRequests(request,1,Tables,&refused)==kIOReturnBadArgument && refused.count==0);++refusals;
        }
        NativeAdrenoGmuRequest overlap[]={{0x4000,0x15824,1},{0x19000,0x2000,1}};   /* partial overlap after rounding */
        CHECK(NativeAdrenoGmuPlanRequests(overlap,2,Tables,&plan)==kIOReturnBadArgument);++refusals;
        NativeAdrenoGmuRequest many[14];
        for(unsigned i=0;i<14;++i)many[i]={0x4000+0x10000*i,0x1000,1};
        CHECK(NativeAdrenoGmuPlanRequests(many,13,Tables,&plan)==kIOReturnSuccess && plan.count==16);
        CHECK(NativeAdrenoGmuPlanRequests(many,14,Tables,&plan)==kIOReturnBadArgument);++refusals;
        CHECK(NativeAdrenoGmuPlanRequests(pkx110,Requests,0x1001,&plan)==kIOReturnBadArgument);++refusals;
        CHECK(NativeAdrenoGmuPlanRequests(nullptr,1,Tables,&plan)==kIOReturnBadArgument);++refusals;
        CHECK(NativeAdrenoGmuPlanRequests(pkx110,Requests,Tables,nullptr)==kIOReturnBadArgument);++refusals;
        CHECK(NativeAdrenoGmuPlanRequests(pkx110,Requests,Tables,&plan)==kIOReturnSuccess);
    }
    /* Allocation of the PKX110 plan: one contiguous, write-combined object per region, a contiguous table pool. */
    {
        fixture=MemoryFixture{};wc_calls=0;
        NativeAdrenoGmuMemory memory;
        CHECK(!memory.ready() && !memory.regionCount() && !memory.region(0) && !memory.hfi() && !memory.cpu(0x60008000,4));
        CHECK(memory.allocate(plan,36)==kIOReturnSuccess && memory.ready());++allocations;
        CHECK(memory.regionCount()==7 && memory.scatteredRegions()==0 && wc_calls==7);
        const NativeAdrenoGmuMemoryLayout &layout=memory.layout();
        CHECK(layout.persist_iova==0x60000000 && layout.log_iova==0x60004000 && layout.hfi_iova==0x60008000);
        CHECK(layout.tables_iova==0 && layout.tables_bytes==Tables && layout.end_iova==0x6000c000);
        CHECK(layout.cache_bytes==0x76000 && layout.regions==7);
        for(unsigned i=0;i<7;++i) {
            const NativeAdrenoMemory *object=memory.memory(i);
            CHECK(object && object->bytes()==plan.region[i].bytes && object->spanCount()==1 && object->writeCombined());
            CHECK(object->spans()[0].physical+object->bytes()<=(1ull<<36));
            CHECK(memory.handle(i)==0);
        }
        CHECK(memory.tables().bytes()==Tables && memory.tables().spanCount()==1 && !memory.tables().writeCombined());
        CHECK(memory.find(0x4000,0x15824)==0 && memory.find(0x100000,0x1c000)==1 && memory.find(0x60008000,0x4000)==6);
        CHECK(memory.find(0x19ffc,8)==-1 && memory.find(0x1a000,4)==-1 && memory.find(0x4000,0)==-1 && memory.find(~0ull,2)==-1);
        CHECK(memory.cpu(0x60008000,0x4000)==memory.hfi()->mapping());
        CHECK(memory.cpu(0x60004000,4)==memory.log()->mapping() && memory.cpu(0x60000000,0x4000)==memory.persist()->mapping());
        CHECK(static_cast<uint8_t *>(memory.cpu(0x4100,4))==static_cast<uint8_t *>(memory.memory(0)->mapping())+0x100);
        CHECK(!memory.cpu(0x60007ffc,8) && !memory.cpu(0x10004000,4));
        /* Zeroed once at allocation; clear() zeroes only the kinds asked for. */
        uint8_t *hfi=static_cast<uint8_t *>(memory.hfi()->mapping()),*log=static_cast<uint8_t *>(memory.log()->mapping());
        CHECK(hfi[0]==0 && hfi[0x3fff]==0 && log[0x2000]==0);
        std::memset(hfi,0x5a,0x4000);std::memset(log,0x5a,0x4000);
        CHECK(memory.clear(1u<<NativeAdrenoGmuRegionHfi)==kIOReturnSuccess);
        CHECK(hfi[0]==0 && hfi[0x3fff]==0 && log[0]==0x5a);
        CHECK(memory.clear(0)==kIOReturnBadArgument && memory.clear(1u<<NativeAdrenoGmuRegionKinds)==kIOReturnBadArgument);
        CHECK(memory.allocate(plan)==kIOReturnBusy);++refusals;
        CHECK(memory.release()==kIOReturnSuccess && !memory.ready() && !memory.regionCount() && !memory.hfi());
        CHECK(memory.release()==kIOReturnNotReady);
        CHECK(fixture.live.empty());
        CHECK(memory.clear(1)==kIOReturnNotReady);
    }
    /* A plan allocate() cannot trust is refused before anything is allocated. */
    {
        fixture=MemoryFixture{};
        NativeAdrenoGmuMemory memory;
        NativeAdrenoGmuPlan bad=plan;bad.region[1].iova=0x8000;               /* overlaps region 0 */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        bad=plan;bad.region[0].permissions=11;                                /* code region not executable */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        bad=plan;bad.count=6;                                                 /* no HFI region */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        bad=plan;bad.region[5].iova=0x6000c000;                               /* log not at its fixed address */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        bad=plan;bad.table_bytes=0x1800;                                      /* table pool not a page multiple */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        bad=plan;bad.region[0].bytes=0x15824;                                 /* not page rounded */
        CHECK(memory.allocate(bad)==kIOReturnBadArgument);++refusals;
        CHECK(!memory.ready() && fixture.live.empty());
    }
    /* No table pool: the GMU SMMU context owns its own (NativeAdrenoGmuContext); only the regions are allocated. */
    {
        fixture=MemoryFixture{};
        NativeAdrenoGmuPlan bare{};
        CHECK(NativeAdrenoGmuPlanRequests(pkx110,Requests,0,&bare)==kIOReturnSuccess && bare.table_bytes==0 && bare.count==7);
        ++plans;
        NativeAdrenoGmuMemory memory;
        CHECK(memory.allocate(bare)==kIOReturnSuccess && memory.tables().bytes()==0 && memory.layout().tables_bytes==0);
        CHECK(fixture.live.size()==7);++allocations;
        CHECK(memory.release()==kIOReturnSuccess && fixture.live.empty());
    }
    /* A fragmented VM: contiguous requests above 64 KiB fail, those regions come as scattered pages. */
    {
        fixture=MemoryFixture{};
        fixture.contiguous_limit=0x10000;
        NativeAdrenoGmuPlan small=plan;small.table_bytes=0x10000;
        NativeAdrenoGmuMemory memory;
        CHECK(memory.allocate(small)==kIOReturnSuccess);++allocations;
        CHECK(memory.scatteredRegions()==4);   /* the 4 CACHE regions are 0x16000..0x25000 bytes */
        CHECK(memory.cpu(0x300000,0x25000) && memory.memory(3)->writeCombined());
        CHECK(memory.release()==kIOReturnSuccess && fixture.live.empty());
        /* The table pool must stay contiguous: no fallback for it, and nothing is left behind. */
        CHECK(memory.allocate(plan)==kIOReturnNoMemory && !memory.ready() && fixture.live.empty());++refusals;
    }
    /* Allocation and write-combining failures release everything. */
    {
        fixture=MemoryFixture{};
        fixture.allocation_fail=true;
        NativeAdrenoGmuMemory memory;
        CHECK(memory.allocate(plan)==kIOReturnNoMemory && !memory.ready() && fixture.live.empty());++refusals;
        fixture.allocation_fail=false;
        fixture.allocation_fail_count=4;   /* region 1 fails contiguous and scattered (two descriptors), region 0 succeeded */
        wc_calls=0;
        CHECK(memory.allocate(plan)!=kIOReturnSuccess && !memory.ready() && fixture.live.empty());++refusals;
        fixture=MemoryFixture{};
        wc_fail=3;wc_calls=0;
        CHECK(memory.allocate(plan)==kIOReturnIOError && !memory.ready() && fixture.live.empty());++refusals;
        wc_fail=0;
        CHECK(memory.allocate(plan)==kIOReturnSuccess && memory.release()==kIOReturnSuccess && fixture.live.empty());
        ++allocations;
    }
    std::printf("PASS: %u PREALLOC plans (PKX110: 4 CACHE regions 0x76000 bytes, persist/log/HFI at 0x60000000/4000/8000), "
                "%u refusals, %u allocations with write-combined regions and a scattered fallback over host doubles\n",
                plans,refusals,allocations);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change has no host equivalent; wc_fail makes the n-th write-combining call fail. */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool write_combined)
{
    if(!spans || !count)return kIOReturnBadArgument;
    if(write_combined && ++wc_calls==wc_fail)return kIOReturnIOError;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
