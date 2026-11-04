/* SPDX-License-Identifier: MIT */
/* The generated A830HwInit.h against the values ARCHITECTURE 4.7.3, 4.7.8, 4.8 and 4.11 write out, and (when the pinned sources
 * are present) the generator's own --check against KGSL and Linux. Also compiles the header as C (it is C11/C++17 clean).
 */
#include "XnuMemoryFixture.h"
#include "A830HwInit.h"
#include "A830Regs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main()
{
    /* Non-context table (4.8 step 10, KGSL gen8_0_0, 40 entries): CP_SMMU_STREAM_ID_LPAC 0x101 first (LPAC fetches with SID1),
     * RB_GC_GMEM_PROTECT 0x0C000000 on BR, the KGSL-only UCHE_CACHE_WAYS present, Linux's RB_CCU_DBG_ECO_CNTL absent
     * (ARCHITECTURE 1.3 #8), only PIPE_NONE/BR/BV masks, no duplicate (offset, pipe). */
    CHECK(!std::strcmp(A830_HWINIT_VARIANT,"kgsl") && A830_HWINIT_NONCTXT_COUNT==40);
    CHECK(A830HwInitNonctxt[0].offset==0x814 && A830HwInitNonctxt[0].value==0x101 && A830HwInitNonctxt[0].pipes==1);
    std::set<uint32_t> seen;
    unsigned gmem=0,ways=0;
    for(uint32_t i=0;i<A830_HWINIT_NONCTXT_COUNT;++i) {
        const A830HwInitPipeReg &e=A830HwInitNonctxt[i];
        CHECK(seen.insert(e.offset).second && e.pipes && !(e.pipes&~0x7u) && (e.pipes==1 || !(e.pipes&1)));
        CHECK(e.offset!=0x8e06 && e.offset!=0x808 && e.offset!=0x815 && e.offset!=0xf400);
        if(e.offset==0x8e09) {++gmem;CHECK(e.value==0x0c000000 && e.pipes==A830_HWINIT_PIPE(1));}
        if(e.offset==0xe04) {++ways;CHECK(e.value==0x00080000);}
        if(e.offset==0xb602)CHECK(e.value==0x00000724);
        if(e.offset==0xb600)CHECK(e.value==0x10100000);
        if(e.offset==0x0ae08)CHECK(e.value==((1u<<26)|(1u<<30)));
        if(e.offset==0x0ae09)CHECK(e.value==((1u<<22)|(1u<<23)));
        if(e.offset==0x00e16)CHECK(e.value==(1u<<8));
        if(e.offset==0x00df0)CHECK(e.value==(1u<<11));
    }
    CHECK(gmem==1 && ways==1 && A830_HWINIT_GMEM_PROTECT_VALUE==A830_RB_GC_GMEM_PROTECT_VALUE);
    /* Protect table (4.8 step 13): entries 0..46 and 63, value = start | (end-start) << 18 | noaccess << 31; 47..62 unwritten. */
    CHECK(A830_HWINIT_PROTECT_COUNT==48 && A830_HWINIT_PROTECT_LAST_INDEX==63 && A830_HWINIT_PROTECT_LAST_VALUE==0x80027c02);
    CHECK(A830_HWINIT_PROTECT_LAST_VALUE==A830_CP_PROTECT_LAST_VALUE);
    for(uint32_t i=0;i<47;++i)CHECK(A830HwInitProtectRegs[i].index==i);
    CHECK(A830HwInitProtectRegs[47].index==63 && A830HwInitProtectRegs[47].value==0x80027c02);
    CHECK(A830HwInitProtectRegs[0].value==A8XX_CP_PROTECT_REG(0x0,0x3a3,0));
    CHECK(A830HwInitProtectRegs[2].value==A8XX_CP_PROTECT_REG(0x440,0x1f,1));
    CHECK(A830HwInitProtectRegs[9].value==A8XX_CP_PROTECT_REG(0x800,0x29,1));      /* the ring registers, no CP access */
    CHECK(A830HwInitProtectRegs[18].value==A8XX_CP_PROTECT_REG(0xe03,0x1fff,1));
    CHECK(A830HwInitProtectRegs[46].value==A8XX_CP_PROTECT_REG(0x27882,0x1,1));
    for(uint32_t i=1;i<A830_HWINIT_PROTECT_COUNT;++i)
        CHECK((A830HwInitProtectRegs[i].value&0x3ffff)>=(A830HwInitProtectRegs[i-1].value&0x3ffff));
    /* GBIF CX (4.7.3 B6) and AO HWCG (4.7.8): the values A830Regs.h carries. */
    CHECK(A830_HWINIT_GBIF_CX_COUNT==6);
    for(uint32_t i=0;i<4;++i)CHECK(A830HwInitGbifCx[i].offset==A8XX_GBIF_QSB_SIDE(i) && A830HwInitGbifCx[i].value==A830_GBIF_QSB_SIDE_VALUE);
    CHECK(A830HwInitGbifCx[4].offset==A8XX_GBIF_CX_CONFIG && A830HwInitGbifCx[4].value==A830_GBIF_CX_CONFIG_VALUE);
    CHECK(A830HwInitGbifCx[5].offset==A8XX_GMUCX_MRC_GBIF_QOS_CTRL && A830HwInitGbifCx[5].value==A830_GMU_MRC_GBIF_QOS_VALUE);
    CHECK(A830_HWINIT_AO_HWCG_COUNT==3);
    CHECK(A830HwInitAoHwcg[0].offset==A8XX_GMUAO_CGC_MODE_CNTL && A830HwInitAoHwcg[0].value==A830_GMU_CGC_MODE_VALUE);
    CHECK(A830HwInitAoHwcg[1].offset==A8XX_GMUAO_CGC_DELAY_CNTL && A830HwInitAoHwcg[1].value==A830_GMU_CGC_DELAY_VALUE);
    CHECK(A830HwInitAoHwcg[2].offset==A8XX_GMUAO_CGC_HYST_CNTL && A830HwInitAoHwcg[2].value==A830_GMU_CGC_HYST_VALUE);
    /* Bicubic weights (4.8 step 8), as the design lists them. */
    const uint32_t bicubic[20]={0,0x3FE05FF4,0x3FA0EBEE,0x3F5193ED,0x3F0243F0,0,0x3FD093E8,0x3F4133DC,0x3EA1DFDB,0x3E0283E0,
        0x0000AC2B,0x0000F01D,0x00114412,0x0021980A,0x0051EC05,0x0000380E,0x3FF09001,0x3FC10BFA,0x3F9193F7,0x3F7227F7};
    CHECK(A830_HWINIT_BICUBIC_COUNT==A830_TPL1_BICUBIC_WEIGHTS_WRITTEN && !std::memcmp(A830HwInitBicubic,bicubic,sizeof bicubic));
    /* Power-up lists (Linux a830): no duplicates; the IFPC list covers the whole protect table and the interrupt mask; the
     * dynamic list's masks never select PIPE_NONE; 60 triplets. */
    CHECK(A830_HWINIT_PWRUP_COUNT==49 && A830_HWINIT_IFPC_COUNT==77 && A830_HWINIT_DYN_COUNT==28 && A830_HWINIT_DYN_TRIPLETS==60);
    std::set<uint32_t> pwrup(A830HwInitPwrup,A830HwInitPwrup+A830_HWINIT_PWRUP_COUNT),ifpc(A830HwInitIfpc,A830HwInitIfpc+A830_HWINIT_IFPC_COUNT);
    CHECK(pwrup.size()==A830_HWINIT_PWRUP_COUNT && ifpc.size()==A830_HWINIT_IFPC_COUNT);
    for(uint32_t i=0;i<64;++i)CHECK(ifpc.count(0x850+i));
    CHECK(ifpc.count(0x8b6) && pwrup.count(0xe11) && pwrup.count(0xb606) && pwrup.count(0xb619) && pwrup.count(0xf08));
    unsigned triplets=0;
    for(uint32_t i=0;i<A830_HWINIT_DYN_COUNT;++i) {
        CHECK(!(A830HwInitDynPwrup[i].pipes&1u) && A830HwInitDynPwrup[i].pipes && !(A830HwInitDynPwrup[i].pipes&~0xffu));
        for(uint32_t p=1;p<8;++p)triplets+=A830HwInitDynPwrup[i].pipes>>p&1;
    }
    CHECK(triplets==A830_HWINIT_DYN_TRIPLETS);
    CHECK(std::strlen(A830_HWINIT_KGSL_GPULIST_SHA256)==64 && std::strlen(A830_HWINIT_LINUX_CATALOG_SHA256)==64);
    /* The header is C11 too (A830Regs.h's rule for shared hardware headers). */
    {
        std::string root=__FILE__;
        root.resize(root.rfind("/tests/"));
        const std::string c="printf '#include \"A830HwInit.h\"\\nint main(void){return (int)A830HwInitNonctxt[0].value-0x101;}\\n' | "
            "xcrun clang -std=c11 -Wall -Wextra -Werror -x c -I '"+root+"' - -o /dev/null";
        CHECK(std::system(c.c_str())==0);
        /* The generator re-derives the tables from the pinned KGSL/Linux sources and compares them byte for byte; the sources
         * live outside the repository (ARCHITECTURE 0.2 K/, L/), so their absence is reported, not failed. */
        const std::string check="python3 -I '"+root+"/scripts/derive-a830-hwinit.py' --check >/dev/null 2>&1";
        const std::string sources="test -f '/private/tmp/claude-501/-Users-xiaochoumao-Library-Application-Support-Claude-scratch-"
            "workspaces-8d57cd08-f137-4208-a8b6-3362ffa35fb5-03638cf4-7c10-4079-a7eb-13992523ab3d-scratch-2026-10-29-ff4716/"
            "5559c563-618d-4813-9848-be3768908d8c/scratchpad/design/smmu-cp/dl_kgsl13t/adreno-gpulist.h'";
        if(std::system(sources.c_str())==0) {
            CHECK(std::system(check.c_str())==0);
            std::printf("NOTE: derive-a830-hwinit.py --check passed against the pinned sources\n");
        } else std::printf("NOTE: pinned KGSL/Linux sources absent; generator check skipped\n");
    }
    std::printf("PASS: A830HwInit.h (kgsl): 40 non-context, 48 protect, 6 GBIF, 3 AO HWCG, 20 bicubic, 49/77/28 power-up entries "
                "(60 triplets) match ARCHITECTURE 4.7-4.11 and A830Regs.h; the header compiles as C11\n");
}
