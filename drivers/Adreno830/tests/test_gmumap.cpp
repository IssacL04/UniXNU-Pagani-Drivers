/* SPDX-License-Identifier: MIT */
/* The PREALLOC-driven GMU regions (ARCHITECTURE 4.7.2) mapped into a GMU-only VM (lower region over the whole 2 GiB GMU
 * space, no upper region) over a software SMMU peer that takes the kit's 32- and 64-bit accesses: every region at its GMU
 * address with the GMU permissions (CACHE privileged read/write/execute, persist/log/HFI privileged read/write), refusals,
 * roll-back of a failed map, unmap and release. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGmuMemory.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define SMMU_BYTES 0x40000
#define CB 0x22000
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);
    ++fixture.cache_calls;
    return kIOReturnSuccess;
}
static uint32_t mmio[SMMU_BYTES/4];
static struct {unsigned calls,writes64;} mf;
static ArmSmmu4KIo io;
static ArmSmmu4KStreamState streams;
static ArmSmmu4KStream routes[1]={{2,0}};
static ArmSmmu4KStreamResult route_result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KContextImage image;
static int read32(void *c,size_t at,uint32_t *v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);++mf.calls;*v=mmio[at/4];return 0;}
static int read64(void *c,size_t at,uint64_t *v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);++mf.calls;std::memcpy(v,mmio+at/4,8);return 0;}
static int write32(void *c,size_t at,uint32_t v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);++mf.calls;mmio[at/4]=v;return 0;}
/* The kit invalidates TLB entries with single 64-bit stores (TLBIVA*, ARCHITECTURE 4.5.2): a peer must take them. */
static int write64(void *c,size_t at,uint64_t v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);++mf.calls;++mf.writes64;std::memcpy(mmio+at/4,&v,8);return 0;}
static int barrier(void *c){CHECK(c==&mf);++mf.calls;return 0;}
/* Metadata blocks #0..#10 of gen80000_gmu.bin. */
static const NativeAdrenoGmuRequest pkx110[]={
    {0x0,0x1ec,1},{0x4000,0x15824,1},{0x200,0x37a8,1},{0x100000,0x1b878,1},{0x200000,0x1eaa0,1},{0x300000,0x24548,1},
    {0x10004000,0x2de8,1},{0x10007000,0xf84,1},{0x10007fdc,0x18,1},{0x60000000,0x1000,7},{0x60001000,0x1000,7}};
int main()
{
    /* A GMU-only context: lower region covering the whole 0..2 GiB GMU space, no upper region; the context owns the table
     * pool (NativeAdrenoGmuContext), so the GMU memory plan has none. */
    NativeAdrenoMemory tables;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    std::memset(mmio,0,sizeof(mmio));mf={};streams=(ArmSmmu4KStreamState){};
    io=(ArmSmmu4KIo){&mf,SMMU_BYTES,read32,read64,write32,write64,barrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
    CHECK(tables.allocate(1027*4096,40,true,1027*4096)==kIOReturnSuccess);
    CHECK(NativeAdrenoPrepareSmmuMemory(&tables,32,40,0,GmuApertureBytes,0,size_t(1)<<19,
        &arena,&state,&visibility)==kIOReturnSuccess);
    CHECK(state.tables.table_pages==1027);
    context=(ArmSmmu4KContextConfig){.lower=&arena.config,.upper=nullptr,
        .lower_tables=&state.tables,.upper_tables=nullptr,.id0=mmio[0x20/4],
        .id2=mmio[0x28/4],.asid=3,.coherent_walk=1,.stall_on_fault=0,
        .continue_after_fault=1};
    ArmSmmu4KContextConfig actual=context;
    CHECK(ArmSmmu4KEncodeContext(&actual,&image)==ArmSmmu4KSuccess);
    mmio[0x1008/4]=image.cbar;mmio[0x1808/4]=image.cba2r;
    mmio[CB/4]=image.sctlr;mmio[(CB+0x10)/4]=image.tcr2;mmio[(CB+0x30)/4]=image.tcr;
    std::memcpy(mmio+(CB+0x20)/4,&image.ttbr0,8);
    std::memcpy(mmio+(CB+0x28)/4,&image.ttbr1,8);
    mmio[(CB+0x38)/4]=image.mair0;
    CHECK(ArmSmmu4KAttachStreams(&io,&streams,routes,1,2,&context,&route_result)==ArmSmmu4KSuccess);
    NativeAdrenoVmRegion lower={&tables,&arena,&state};
    CHECK(vm.attach(&io,&context,&streams,&lower,nullptr,64,2000)==kIOReturnSuccess);
    CHECK(vm.spanCount()==0 && vm.objectCount()==0);

    NativeAdrenoGmuPlan plan{};
    CHECK(NativeAdrenoGmuPlanRequests(pkx110,sizeof pkx110/sizeof pkx110[0],0,&plan)==kIOReturnSuccess && plan.count==7);
    NativeAdrenoGmuMemory memory;
    unsigned refusals=0;
    CHECK(memory.mapInto(&vm)==kIOReturnBadArgument);++refusals;   /* not allocated yet */
    CHECK(memory.allocate(plan,36)==kIOReturnSuccess);
    CHECK(memory.mapInto(nullptr)==kIOReturnBadArgument);++refusals;
    CHECK(memory.mapInto(&vm)==kIOReturnSuccess);
    CHECK(vm.objectCount()==7 && vm.spanCount()==7);
    /* The MEM row's vm: the bytes of every live mapping, back to 0 once all are unmapped (a leak shows as growth). */
    uint64_t planned=0;
    for(unsigned i=0;i<7;++i)planned+=plan.region[i].bytes;
    CHECK(vm.mappedBytes()==planned);
    for(unsigned i=0;i<7;++i) {
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        CHECK(memory.handle(i) && vm.describe(memory.handle(i),&iova,&bytes,&permissions)==kIOReturnSuccess);
        CHECK(iova==plan.region[i].iova && bytes==plan.region[i].bytes);
        /* ArmSmmu4KRead 1, Write 2, Execute 4, Privileged 8: the GMU's code pages must be privileged (a privileged fetch from
         * an EL0-writable page faults, ARCHITECTURE 4.6.3), the data pages execute-never. */
        CHECK(permissions==(i<4?15u:11u));
    }
    CHECK(memory.mapInto(&vm)==kIOReturnBusy);++refusals;
    CHECK(memory.release()==kIOReturnBusy);++refusals;   /* still mapped */
    uint64_t iova=0;
    CHECK(vm.findAddress(1,0x2000,0x2000,&iova)!=kIOReturnSuccess);   /* no upper region */
    CHECK(memory.unmapFrom(&vm)==kIOReturnSuccess);
    CHECK(vm.objectCount()==0 && vm.spanCount()==0 && vm.mappedBytes()==0);
    for(unsigned i=0;i<7;++i)CHECK(memory.handle(i)==0);
    CHECK(memory.unmapFrom(&vm)==kIOReturnSuccess && memory.unmapFrom(nullptr)==kIOReturnBadArgument);

    /* Something else already at 0x200000: the third region fails and the two mapped before it are taken down again. */
    NativeAdrenoMemory squatter;
    CHECK(squatter.allocate(0x1000,36,true,0x1000)==kIOReturnSuccess);
    uint64_t squatter_handle=0;
    CHECK(vm.map(&squatter,0,0x200000,3,0,2,&squatter_handle)==kIOReturnSuccess && vm.objectCount()==1);
    CHECK(memory.mapInto(&vm)!=kIOReturnSuccess);++refusals;
    CHECK(vm.objectCount()==1);
    for(unsigned i=0;i<7;++i)CHECK(memory.handle(i)==0);
    CHECK(vm.unmap(squatter_handle)==kIOReturnSuccess && squatter.release()==kIOReturnSuccess);
    CHECK(memory.mapInto(&vm)==kIOReturnSuccess && vm.objectCount()==7);
    CHECK(memory.unmapFrom(&vm)==kIOReturnSuccess);

    CHECK(memory.release()==kIOReturnSuccess);
    CHECK(vm.detach()==kIOReturnSuccess);
    CHECK(tables.retireAfterQuiescence()==kIOReturnSuccess);
    CHECK(tables.release()==kIOReturnSuccess);
    CHECK(fixture.live.empty() && fixture.metadata.empty());
    std::printf("PASS: 7 PREALLOC GMU regions mapped at their GMU addresses (CACHE privileged RWX, persist/log/HFI privileged RW), "
                "1 rolled-back failed map, %u refusals, unmap and release over a software SMMU peer\n",refusals);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
