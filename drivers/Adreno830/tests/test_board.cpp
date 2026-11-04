/* SPDX-License-Identifier: MIT */
/* Real parser over the exact SM8750 / OnePlus 13T GPU, GMU and GPU-SMMU cells (IMPLEMENTATION_PLAN 2.1 test_board): the UniXNU
 * sm8750.dtsi nodes (#address-cells = #size-cells = 2) and the phone's runtime device tree (LDT, 1/1 cells under its soc node,
 * live.dtb of this PKX110). No device access.
 * host-test: NativeAdrenoBoard.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoBoard.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void put(uint8_t *at,uint32_t value)
{at[0]=uint8_t(value>>24);at[1]=uint8_t(value>>16);at[2]=uint8_t(value>>8);at[3]=uint8_t(value);}
/* Big-endian property bytes of n cells. */
static size_t cellsOf(uint8_t *out,const uint32_t *cells,size_t n)
{
    for(size_t i=0;i<n;++i)put(out+4*i,cells[i]);
    return 4*n;
}
static NativeAdrenoWindow windows[NativeAdrenoMaxWindows];
int main()
{
    unsigned sets=0,refusals=0;
    /* sm8750.dtsi gpu@3d00000: kgsl_3d0_reg_memory, rscc, cx_dbgc, cx_misc; #address-cells = #size-cells = 2. */
    {
        static const uint32_t cells[]={0,0x03d00000,0,0x40000, 0,0x03d50000,0,0x10000, 0,0x03d61000,0,0x3000, 0,0x03d9e000,0,0x2000};
        uint8_t raw[sizeof cells];const size_t bytes=cellsOf(raw,cells,sizeof cells/4);
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,NativeAdrenoMaxWindows,&count)==kIOReturnSuccess && count==4);
        CHECK(windows[0].base==0x03d00000 && windows[0].bytes==0x40000);
        CHECK(windows[1].base==0x03d50000 && windows[1].bytes==0x10000);
        CHECK(windows[2].base==0x03d61000 && windows[2].bytes==0x3000);
        CHECK(windows[3].base==0x03d9e000 && windows[3].bytes==0x2000);
        ++sets;
    }
    /* The runtime tree's gpu node: the same four, then qdss_gfx, qdss_etr and qdss_tmc (seven windows, 1/1 cells). */
    {
        static const uint32_t cells[]={0x3d00000,0x40000,0x3d50000,0x10000,0x3d61000,0x3000,0x3d9e000,0x2000,
            0x10900000,0x80000,0x10048000,0x8000,0x10b05000,0x1000};
        uint8_t raw[sizeof cells];const size_t bytes=cellsOf(raw,cells,sizeof cells/4);
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,1,1,windows,NativeAdrenoMaxWindows,&count)==kIOReturnSuccess && count==7);
        CHECK(windows[3].base==0x03d9e000 && windows[3].bytes==0x2000 && windows[4].base==0x10900000 && windows[6].bytes==0x1000);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,1,1,windows,4,&count)==kIOReturnNoResources && !count);   /* capacity is exact */
        ++sets;
    }
    /* gmu@3d37000: gmu (0x68000: the GMU window spans 0x3D37000..0x3D9F000, CM3_SYSRESET at +0x47000) and gmu_ao_blk_dec0. */
    {
        static const uint32_t dtsi[]={0,0x03d37000,0,0x68000, 0,0x03d40000,0,0x10000};
        uint8_t raw[sizeof dtsi];size_t bytes=cellsOf(raw,dtsi,sizeof dtsi/4);
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,NativeAdrenoMaxWindows,&count)==kIOReturnSuccess && count==2);
        CHECK(windows[0].base==0x03d37000 && windows[0].bytes==0x68000 && windows[1].base==0x03d40000 && windows[1].bytes==0x10000);
        CHECK(windows[0].base+0x47000==0x03d7e000);   /* CM3_SYSRESET, dword 0x1F800 (ARCHITECTURE 0.3) */
        static const uint32_t live[]={0x3d37000,0x68000,0x3d40000,0x10000};
        uint8_t live_raw[sizeof live];bytes=cellsOf(live_raw,live,sizeof live/4);
        CHECK(NativeAdrenoBoard::parseReg(live_raw,bytes,1,1,windows,2,&count)==kIOReturnSuccess && count==2 &&
            windows[0].bytes==0x68000);
        /* A 2/1 node (64-bit address, 32-bit size) parses too. */
        static const uint32_t mixed[]={0,0x03da0000,0x40000};
        uint8_t mixed_raw[sizeof mixed];bytes=cellsOf(mixed_raw,mixed,sizeof mixed/4);
        CHECK(NativeAdrenoBoard::parseReg(mixed_raw,bytes,2,1,windows,1,&count)==kIOReturnSuccess && count==1 &&
            windows[0].base==0x03da0000 && windows[0].bytes==0x40000);   /* gpu_smmu */
        ++sets;
    }
    /* The GPU SMMU specifiers: #iommu-cells = 2 (SID, mask). gpu: gfx3d_user SID 0, gfx3d_lpac SID 1 (the dtsi); the runtime tree's
     * kgsl-iommu children add gfx3d_secure SID 2; gmu: SID 5. Phandle 0x275 is the runtime tree's gpu_smmu. */
    {
        static const uint32_t gpu[]={0x275,0,0, 0x275,1,0};
        uint8_t raw[sizeof gpu];size_t bytes=cellsOf(raw,gpu,sizeof gpu/4);
        uint16_t sids[4]{};
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseIommus(raw,bytes,2,sids,4,&count)==kIOReturnSuccess && count==2 && sids[0]==0 && sids[1]==1);
        static const uint32_t children[]={0x275,0,0, 0x275,1,0, 0x275,2,0};
        uint8_t child_raw[sizeof children];bytes=cellsOf(child_raw,children,sizeof children/4);
        CHECK(NativeAdrenoBoard::parseIommus(child_raw,bytes,2,sids,4,&count)==kIOReturnSuccess && count==3 && sids[2]==2);
        static const uint32_t gmu[]={0x275,5,0};
        uint8_t gmu_raw[sizeof gmu];bytes=cellsOf(gmu_raw,gmu,sizeof gmu/4);
        CHECK(NativeAdrenoBoard::parseIommus(gmu_raw,bytes,2,sids,4,&count)==kIOReturnSuccess && count==1 && sids[0]==5);
        uint16_t untouched[2]={0xbeef,0xbeef};
        CHECK(NativeAdrenoBoard::parseIommus(child_raw,sizeof children,2,untouched,2,&count)==kIOReturnNoResources);
        CHECK(untouched[0]==0xbeef && untouched[1]==0xbeef && !count);
        CHECK(NativeAdrenoBoard::parseIommus(raw,sizeof gpu-1,2,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,sizeof gpu,0,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,sizeof gpu,5,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(nullptr,sizeof gpu,2,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,sizeof gpu,2,nullptr,4,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseIommus(raw,sizeof gpu,2,sids,4,nullptr)==kIOReturnBadArgument);
        /* #iommu-cells = 1 (the apps SMMU) reads the same SIDs from a shorter entry. */
        static const uint32_t apps[]={0x7e,0x4a4};
        uint8_t apps_raw[sizeof apps];bytes=cellsOf(apps_raw,apps,sizeof apps/4);
        CHECK(NativeAdrenoBoard::parseIommus(apps_raw,bytes,1,sids,4,&count)==kIOReturnSuccess && count==1 && sids[0]==0x4a4);
        uint8_t unnamed[12];put(unnamed,0);put(unnamed+4,0);put(unnamed+8,0);
        CHECK(NativeAdrenoBoard::parseIommus(unnamed,12,2,sids,4,&count)==kIOReturnBadArgument);
        uint8_t wide[12];put(wide,0x275);put(wide+4,0x10000);put(wide+8,0);
        CHECK(NativeAdrenoBoard::parseIommus(wide,12,2,sids,4,&count)==kIOReturnBadArgument);
        refusals+=9;
        ++sets;
    }
    /* Refusals precede any output: nothing is written on failure. */
    {
        static const uint32_t cells[]={0,0x03d00000,0,0x40000, 0,0x03d50000,0,0x10000};
        uint8_t raw[sizeof cells];const size_t bytes=cellsOf(raw,cells,sizeof cells/4);
        size_t count=99;
        std::memset(windows,0x5a,sizeof(windows));
        CHECK(NativeAdrenoBoard::parseReg(nullptr,bytes,2,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,0,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,3,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,0,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,1,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes+1,2,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,nullptr,2,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,2,nullptr)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a));
        /* A zero size and an address wrap are refused. */
        static const uint32_t zero[]={0,0x03d00000,0,0};
        uint8_t one[16];cellsOf(one,zero,4);
        CHECK(NativeAdrenoBoard::parseReg(one,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        uint8_t wrap[16];put(wrap,0xffffffff);put(wrap+4,0xfffff000);put(wrap+8,0);put(wrap+12,0x2000);
        CHECK(NativeAdrenoBoard::parseReg(wrap,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a) && !count);
        refusals+=10;
    }
    std::printf("PASS: %u real SM8750/PKX110 cell sets (GPU 4+3 windows, GMU 2 windows, #iommu-cells=2 SIDs 0/1/2/5) and %u "
        "malformed/limit refusals over the device-tree parser\n",sets,refusals);
}
