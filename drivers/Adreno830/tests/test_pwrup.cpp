/* SPDX-License-Identifier: MIT */
/* The power-up list (Linux struct cpu_gpu_lock) and its Gen8 builder over the generated tables (ARCHITECTURE 4.11 step 10,
 * 4.17 P2; PLAN M4, M13 f). No GPU access.
 * host-test: NativeAdrenoPwrup.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoPwrup.h"
#include "A830HwInit.h"
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static uint32_t word(const uint8_t *image,size_t index) {uint32_t v;std::memcpy(&v,image+4*index,4);return v;}
int main()
{
    /* Exact layout: lock words 0, lengths u8/u8/u16, pairs then triplets; nothing written past the image. */
    const uint32_t ir[]={0x10,0x20},iv[]={0xaa,0xbb};
    const uint32_t pr[]={0x30},pv[]={0xcc};
    const uint32_t dt[]={0x1000,0x40,0xdd,0x2000,0x50,0xee};
    NativeAdrenoPwrupCounts counts{2,1,2};
    CHECK(NativeAdrenoPwrup::requiredBytes(counts)==16+3*8+2*12);
    uint8_t image[128];
    std::memset(image,0x5a,sizeof(image));
    size_t written=0;
    CHECK(NativeAdrenoPwrup::pack(counts,ir,iv,pr,pv,dt,image,sizeof(image),&written)==kIOReturnSuccess);
    CHECK(written==16+24+24);
    const uint32_t expected[]={0,0,0,0x00020102,0x10,0xaa,0x20,0xbb,0x30,0xcc,0x1000,0x40,0xdd,0x2000,0x50,0xee};
    CHECK(!std::memcmp(image,expected,sizeof(expected)));
    CHECK(image[written]==0x5a);
    /* Capacity and count refusals leave everything untouched. */
    size_t before=0;
    std::memset(image,0x5a,sizeof(image));
    CHECK(NativeAdrenoPwrup::pack(counts,ir,iv,pr,pv,dt,image,written-1,&before)==kIOReturnNoResources);
    CHECK(!before && image[0]==0x5a && image[sizeof(image)-1]==0x5a);
    CHECK(NativeAdrenoPwrup::pack(counts,nullptr,iv,pr,pv,dt,image,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrup::pack(counts,ir,iv,pr,pv,nullptr,image,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrup::pack(counts,ir,iv,pr,pv,dt,nullptr,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrup::pack(counts,ir,iv,pr,pv,dt,image,sizeof(image),nullptr)==kIOReturnBadArgument);
    NativeAdrenoPwrupCounts big{0x100,0,0};
    CHECK(NativeAdrenoPwrup::pack(big,ir,iv,pr,pv,dt,image,sizeof(image),&before)==kIOReturnBadArgument);
    NativeAdrenoPwrupCounts zero{0,0,0};
    CHECK(NativeAdrenoPwrup::requiredBytes(zero)==16);
    CHECK(NativeAdrenoPwrup::pack(zero,nullptr,nullptr,nullptr,nullptr,nullptr,image,16,&before)==kIOReturnSuccess);
    CHECK(before==16 && !image[12] && !image[13] && !image[14] && !image[15]);
    /* The Gen8 list of the generated A830 tables: IFPC 77 pairs, pwrup 49 pairs, 60 dynamic triplets over the pipes in each
     * entry's mask (BR..DDE_BV, LPAC included as Linux reads it), into one 4 KiB page that is zero after the image. */
    {
        CHECK(A830_HWINIT_IFPC_COUNT==77 && A830_HWINIT_PWRUP_COUNT==49 && A830_HWINIT_DYN_COUNT==28 && A830_HWINIT_DYN_TRIPLETS==60);
        std::vector<uint32_t> ifpc(A830_HWINIT_IFPC_COUNT),pwrup(A830_HWINIT_PWRUP_COUNT),dyn;
        for(uint32_t i=0;i<A830_HWINIT_IFPC_COUNT;++i)ifpc[i]=A830HwInitIfpc[i]^0x5a5a0000u;
        for(uint32_t i=0;i<A830_HWINIT_PWRUP_COUNT;++i)pwrup[i]=A830HwInitPwrup[i]^0x00a50000u;
        for(uint32_t pipe=1;pipe<=7;++pipe)
            for(uint32_t i=0;i<A830_HWINIT_DYN_COUNT;++i)
                if(A830HwInitDynPwrup[i].pipes&A830_HWINIT_PIPE(pipe)) {
                    dyn.push_back(pipe<<12);dyn.push_back(A830HwInitDynPwrup[i].offset);dyn.push_back(pipe*0x100000u+i);
                }
        CHECK(dyn.size()==3*A830_HWINIT_DYN_TRIPLETS);
        const A830PwrupTables tables={A830HwInitIfpc,ifpc.data(),A830_HWINIT_IFPC_COUNT,A830HwInitPwrup,pwrup.data(),
                                      A830_HWINIT_PWRUP_COUNT,dyn.data(),uint32_t(dyn.size()/3)};
        static uint8_t page[4096];
        std::memset(page,0xee,sizeof(page));
        size_t bytes=0;
        CHECK(NativeAdrenoPwrup::buildGen8(tables,page,sizeof(page),&bytes)==kIOReturnSuccess);
        CHECK(bytes==16+(77+49)*8+60*12 && bytes==1744);
        CHECK(word(page,0)==0 && word(page,1)==0 && word(page,2)==0 && page[12]==77 && page[13]==49 && page[14]==60 && page[15]==0);
        size_t at=4;
        for(uint32_t i=0;i<77;++i,at+=2)CHECK(word(page,at)==A830HwInitIfpc[i] && word(page,at+1)==ifpc[i]);
        for(uint32_t i=0;i<49;++i,at+=2)CHECK(word(page,at)==A830HwInitPwrup[i] && word(page,at+1)==pwrup[i]);
        for(size_t i=0;i<dyn.size();++i,++at)CHECK(word(page,at)==dyn[i]);
        CHECK(word(page,4+2*77)==0x00e01u);   /* the pwrup list starts with UCHE_MODE_CNTL */
        CHECK(word(page,4+2*126)==0x1000 && word(page,4+2*126+1)==0x008b4u);   /* BR CP_HW_FAULT_STATUS_MASK_PIPE first */
        for(size_t i=bytes;i<sizeof(page);++i)CHECK(page[i]==0);
        /* A triplet must name exactly one pipe in [BR, DDE_BV] in its aperture word. */
        std::vector<uint32_t> bad=dyn;
        bad[0]=0;
        A830PwrupTables broken=tables;broken.dynamic=bad.data();
        std::memset(page,0xee,sizeof(page));
        CHECK(NativeAdrenoPwrup::buildGen8(broken,page,sizeof(page),&bytes)==kIOReturnBadArgument && !bytes && page[0]==0xee);
        bad[0]=8u<<12;
        CHECK(NativeAdrenoPwrup::buildGen8(broken,page,sizeof(page),&bytes)==kIOReturnBadArgument);
        bad[0]=(1u<<12)|(1u<<16);   /* a slice select is not part of the list */
        CHECK(NativeAdrenoPwrup::buildGen8(broken,page,sizeof(page),&bytes)==kIOReturnBadArgument);
        CHECK(NativeAdrenoPwrup::buildGen8(tables,page,1743,&bytes)==kIOReturnNoResources && page[0]==0xee);
        CHECK(NativeAdrenoPwrup::buildGen8(tables,nullptr,sizeof(page),&bytes)==kIOReturnBadArgument);
        A830PwrupTables missing=tables;missing.pwrup_values=nullptr;
        CHECK(NativeAdrenoPwrup::buildGen8(missing,page,sizeof(page),nullptr)==kIOReturnBadArgument);
        /* The CP_ME_INIT-time page may be all zero (lists filled after the ZAP): an empty list is a valid image. */
        const A830PwrupTables empty={};
        std::memset(page,0xee,sizeof(page));
        CHECK(NativeAdrenoPwrup::buildGen8(empty,page,sizeof(page),&bytes)==kIOReturnSuccess && bytes==16);
        for(uint8_t b:page)CHECK(b==0);
    }
    std::printf("PASS: 1 exact cpu_gpu_lock image, 1 Gen8 list (77 IFPC + 49 pwrup pairs, 60 pipe triplets) with zero tail, "
                "1 empty list, 6 capacity/argument refusals and 3 aperture refusals\n");
}
