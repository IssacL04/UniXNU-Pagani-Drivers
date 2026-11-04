/* SPDX-License-Identifier: MIT */
/* host-test: A830GmuFirmware.cpp NativeAdrenoGmuMemory.cpp NativeAdrenoMemory.cpp
 * gen80000_gmu.bin parsing and loading (ARCHITECTURE 4.7.1-4.7.2): the block table of the PKX110 image read at test time
 * from the stock backup (A830_GMU_FW overrides the path; a synthetic image of the same layout is used when it is absent),
 * its PREALLOC plan, TCM loading through a recording GMU window and DDR loading into the write-combined regions, and every
 * refusal of the parser and the loader. Both profiles of A830Profile.h: the design loads without reading anything back;
 * the annibale profile reads every TCM word back and compares every DDR block (kernel-sequences 4.5, section 14 row 16),
 * and a word that does not stick or cannot be read fails the load. */
#include "XnuMemoryFixture.h"
#include "A830GmuFirmware.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    return kIOReturnSuccess;
}

/* The gmu window (0x68000 bytes) as an array; every TCM write is recorded. `stuck` is a byte offset whose writes are
 * dropped (a TCM word that does not take the image), fail_read/fail_write the access number that fails. */
static uint32_t window[0x68000/4];
static struct {unsigned writes,reads,fail_write,fail_read,other_window;uint32_t stuck;} gmu;
static int read32(void *cookie,unsigned w,uint32_t offset,uint32_t *value)
{
    CHECK(cookie==&gmu && value);
    if(w!=A830GmuWindowGmu) {++gmu.other_window;return 1;}
    CHECK(!(offset&3) && offset<sizeof window);
    if(++gmu.reads==gmu.fail_read)return 1;
    *value=window[offset/4];return 0;
}
static int write32(void *cookie,unsigned w,uint32_t offset,uint32_t value)
{
    CHECK(cookie==&gmu);
    if(w!=A830GmuWindowGmu) {++gmu.other_window;return 1;}
    CHECK(!(offset&3) && offset<sizeof window);
    if(++gmu.writes==gmu.fail_write)return 1;
    if(gmu.stuck && offset==gmu.stuck)return 0;
    window[offset/4]=value;return 0;
}
static unsigned barriers;
static void barrier(void *cookie) {CHECK(cookie==&gmu);++barriers;}

static void put32(std::vector<uint8_t> &image,uint32_t value)
{for(int i=0;i<4;++i)image.push_back(uint8_t(value>>(8*i)));}
static void block(std::vector<uint8_t> &image,uint32_t addr,uint32_t size,uint32_t type,uint32_t value,const uint8_t *data=nullptr)
{
    put32(image,addr);put32(image,size);put32(image,type);put32(image,value);
    for(uint32_t i=0;i<size;++i)image.push_back(data?data[i]:uint8_t(i*7+addr));
}
/* Same block table as the PKX110 image (ARCHITECTURE 4.7.1), synthetic payloads. */
static std::vector<uint8_t> synthetic()
{
    std::vector<uint8_t> image;
    const uint32_t meta[][3]={{0x0,0x1ec,1},{0x4000,0x15824,1},{0x200,0x37a8,1},{0x100000,0x1b878,1},{0x200000,0x1eaa0,1},
        {0x300000,0x24548,1},{0x10004000,0x2de8,1},{0x10007000,0xf84,1},{0x10007fdc,0x18,1},{0x60000000,0x1000,7},{0x60001000,0x1000,7}};
    for(const auto &m:meta)block(image,m[0],0,m[2],m[1]);
    uint8_t itcm[0x1ec];
    for(uint32_t i=0;i<sizeof itcm;++i)itcm[i]=uint8_t(i*13+1);
    const uint32_t boot[4]={0x10007F80,0x105,0x145,0x159};
    std::memcpy(itcm,boot,sizeof boot);
    block(image,0x0,0x1ec,0xEFBEADDE,0xEFBEADDE,itcm);
    block(image,0x4000,0x15824,0xEFBEADDE,0xEFBEADDE);
    const uint8_t zeros[0x28]={};
    block(image,0x10007f98,0x28,0xEFBEADDE,0xEFBEADDE,zeros);
    const uint32_t versions[6]={0xffffffff,0x50010019,0xffffffff,0x50000112,0,0x20070001};
    block(image,0x10007fdc,0x18,0xEFBEADDE,0xEFBEADDE,reinterpret_cast<const uint8_t *>(versions));
    return image;
}
static bool readFile(const char *path,std::vector<uint8_t> *out)
{
    FILE *file=std::fopen(path,"rb");
    if(!file)return false;
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];size_t n;
    while((n=std::fread(buffer,1,sizeof buffer,file))>0)bytes.insert(bytes.end(),buffer,buffer+n);
    std::fclose(file);
    *out=bytes;
    return true;
}
static A830GmuBlocks blocks,other;   /* about 2 KiB each, static as in the kernel */

static void expectLayout(const A830GmuBlocks &b,size_t bytes)
{
    CHECK(b.bytes==bytes && b.end==bytes && bytes==0x15B40);
    CHECK(b.count==15 && b.metadata==11 && b.data==4 && b.requests==11 && b.ignored_metadata==0);
    const uint32_t data[4][3]={{0x0,0x1ec,A830GmuTargetItcm},{0x4000,0x15824,A830GmuTargetDdr},
                               {0x10007f98,0x28,A830GmuTargetDtcm},{0x10007fdc,0x18,A830GmuTargetDtcm}};
    for(unsigned i=0;i<4;++i) {
        const A830GmuBlock &blk=b.block[11+i];
        CHECK(blk.addr==data[i][0] && blk.size==data[i][1] && blk.target==data[i][2] && blk.type==0xEFBEADDE);
    }
    CHECK(b.block[11].offset==0xb0+16 && b.block[12].offset==0x2ac+16 && b.block[13].offset==0x15ae0+16 && b.block[14].offset==0x15b18+16);
    CHECK(b.request[1].iova==0x4000 && b.request[1].bytes==0x15824 && b.request[9].type==7 && b.request[10].iova==0x60001000);
    CHECK(b.itcm[0]==A830_GMU_ITCM_BOOT_WORD0 && b.itcm[1]==A830_GMU_ITCM_BOOT_WORD1 && b.itcm[2]==0x145 && b.itcm[3]==0x159);
    CHECK(b.versions==7 && b.core==A830_GMU_CORE_FW_VERSION && b.pwr==A830_GMU_PWR_FW_VERSION && b.hfi==A830_GMU_HFI_VERSION);
}

int main()
{
    unsigned parses=0,refusals=0,loads=0,verified=0;
    /* The design profile first: the loader writes and never reads back. */
    adreno830_profile_value=A830ProfileDesign;
    const char *path=std::getenv("A830_GMU_FW");
    if(!path)path="/Users/xiaochoumao/unixnu-13t/\xe4\xb8\x80\xe5\x8a\xa0" "13T\xe5\x8e\x9f\xe5\x8e\x82\xe5\xa4\x87\xe4\xbb\xbd/GPU"
                  "\xe5\x9b\xba\xe4\xbb\xb6/gen80000_gmu.bin";
    std::vector<uint8_t> real;
    const bool have_real=readFile(path,&real);
    const std::vector<uint8_t> fake=synthetic();
    const std::vector<uint8_t> &image=have_real?real:fake;
    if(have_real)CHECK(real.size()==A830_GMU_FW_FILE_BYTES);
    CHECK(fake.size()==A830_GMU_FW_FILE_BYTES);

    /* Parsing: the 15-block table, the PREALLOC list, the boot stub and the version words. */
    CHECK(A830GmuFirmware::parse(image.data(),image.size(),&blocks)==kIOReturnSuccess);expectLayout(blocks,image.size());++parses;
    CHECK(A830GmuFirmware::parse(fake.data(),fake.size(),&other)==kIOReturnSuccess);expectLayout(other,fake.size());++parses;
    for(unsigned i=0;i<11;++i)
        CHECK(blocks.request[i].iova==other.request[i].iova && blocks.request[i].bytes==other.request[i].bytes &&
              blocks.request[i].type==other.request[i].type);
    CHECK(A830GmuFirmware::tcmOffset(0)==0x36000 && A830GmuFirmware::tcmOffset(0x3ffc)==0x39ffc);
    CHECK(A830GmuFirmware::tcmOffset(0x10004000)==0x3a000 && A830GmuFirmware::tcmOffset(0x10007ffc)==0x3dffc);
    CHECK(A830GmuFirmware::tcmOffset(0x4000)==~0u && A830GmuFirmware::tcmOffset(0x10008000)==~0u);
    CHECK(A830_GMU_PHYS+A830GmuFirmware::tcmOffset(0)==0x3D6D000 && A830_GMU_PHYS+A830GmuFirmware::tcmOffset(0x10004000)==0x3D71000);

    /* Parser refusals: each clears the output. */
    {
        auto refused=[&](const std::vector<uint8_t> &bad) {
            other.count=99;
            const bool ok=A830GmuFirmware::parse(bad.data(),bad.size(),&other)==kIOReturnBadArgument && other.count==0 && !other.image;
            ++refusals;
            return ok;
        };
        std::vector<uint8_t> bad(image.begin(),image.end()-8);              /* last data block past the end */
        CHECK(refused(bad));
        bad.assign(image.begin(),image.begin()+0x15b18+8);                  /* the last header cut after 8 bytes */
        CHECK(refused(bad));
        bad=image;block(bad,0x60002000,0,1,0x1000);                           /* metadata after data */
        CHECK(refused(bad));
        bad.clear();block(bad,0x4000,0,1,0);block(bad,0x4000,4,0,0);         /* zero-byte PREALLOC */
        CHECK(refused(bad));
        bad.clear();block(bad,0xfffff000,0,7,0x2000);block(bad,0x4000,4,0,0);  /* request wraps the 32-bit GMU space */
        CHECK(refused(bad));
        bad.clear();block(bad,0x50000000,8,0,0);                             /* data outside every GMU range */
        CHECK(refused(bad));
        bad.clear();block(bad,0x3ffc,8,0,0);                                 /* crosses the ITCM end */
        CHECK(refused(bad));
        bad.clear();block(bad,0x10007ff8,0x10,0,0);                          /* crosses the DTCM end */
        CHECK(refused(bad));
        bad.clear();block(bad,0x2,4,0,0);                                    /* TCM address not 4-byte aligned */
        CHECK(refused(bad));
        bad.clear();block(bad,0x10004000,6,0,0);                             /* TCM size not a dword multiple */
        CHECK(refused(bad));
        bad.clear();block(bad,0x4000,0,1,0x1000);                            /* metadata only, nothing to load */
        CHECK(refused(bad));
        bad.clear();for(unsigned i=0;i<64;++i)block(bad,0x4000,0,2,0);block(bad,0x4000,4,0,0);   /* 65 blocks */
        CHECK(refused(bad));
        bad.clear();for(unsigned i=0;i<33;++i)block(bad,0x4000,0,1,0x10);block(bad,0x4000,4,0,0);   /* 33 requests */
        CHECK(refused(bad));
        bad.assign(image.begin(),image.begin()+15);                          /* shorter than a header */
        CHECK(refused(bad));
        CHECK(A830GmuFirmware::parse(nullptr,100,&other)==kIOReturnBadArgument);++refusals;
        CHECK(A830GmuFirmware::parse(image.data(),image.size(),nullptr)==kIOReturnBadArgument);++refusals;
        /* Other metadata types are counted and ignored; an odd-sized DDR block is fine (memcpy). */
        std::vector<uint8_t> ok;block(ok,0x4000,0,2,0x50010019);block(ok,0x4000,0,1,0x10);block(ok,0x4000,3,0,0);
        CHECK(A830GmuFirmware::parse(ok.data(),ok.size(),&other)==kIOReturnSuccess && other.ignored_metadata==1 &&
              other.requests==1 && other.data==1 && other.versions==0);++parses;
    }

    /* The plan of the image (ARCHITECTURE 4.7.2): 4 CACHE regions of 0x76000 bytes, persist, log, HFI. */
    NativeAdrenoGmuPlan plan{};
    CHECK(A830GmuFirmware::plan(blocks,0,&plan)==kIOReturnSuccess);
    CHECK(plan.count==7 && plan.cache_bytes==0x76000 && plan.tcm_requests==5 && plan.persist_requests==2);
    CHECK(plan.region[0].iova==0x4000 && plan.region[0].bytes==0x16000 && plan.region[3].iova==0x300000 &&
          plan.region[3].bytes==0x25000 && plan.region[6].iova==0x60008000);
    {
        A830GmuBlocks empty{};
        NativeAdrenoGmuPlan none{};
        CHECK(A830GmuFirmware::plan(empty,0,&none)==kIOReturnBadArgument);++refusals;
        CHECK(A830GmuFirmware::plan(blocks,0,nullptr)==kIOReturnBadArgument);++refusals;
    }

    /* Loading: TCM blocks dword by dword at CM3_ITCM_START/CM3_DTCM_START, the main code copied into the 0x4000 region. */
    A830GmuIo io{&gmu,read32,write32,barrier};
    NativeAdrenoGmuMemory memory;
    CHECK(A830GmuFirmware::load(blocks,io,memory)==kIOReturnNotReady);++refusals;   /* memory not allocated */
    CHECK(memory.allocate(plan,36)==kIOReturnSuccess);
    std::memset(window,0xcc,sizeof window);gmu={};barriers=0;
    A830GmuLoadCheck check{};
    check.tcm_words=99;
    CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnSuccess);++loads;
    CHECK(gmu.writes==(0x1ec+0x28+0x18)/4 && barriers==1 && gmu.other_window==0);
    CHECK(gmu.reads==0 && check.tcm_words==0 && check.ddr_bytes==0 && !check.mismatch);   /* design: nothing read back */
    for(uint32_t i=0;i<0x1ec/4;++i)CHECK(window[(0x36000+4*i)/4]==*reinterpret_cast<const uint32_t *>(&image[0xb0+16+4*i]));
    CHECK(window[0x36000/4]==0x10007F80 && window[0x36004/4]==0x105 && window[(0x36000+0x1ec)/4]==0xcccccccc);
    CHECK(window[(0x3a000+0x3f98)/4]==0 && window[(0x3a000+0x3fe0)/4]==0x50010019 && window[(0x3a000+0x3ff0)/4]==0x20070001);
    CHECK(window[(0x3a000+0x3ff4)/4]==0xcccccccc && window[(0x3a000+0x3f94)/4]==0xcccccccc);
    CHECK(std::memcmp(memory.cpu(0x4000,0x15824),&image[blocks.block[12].offset],0x15824)==0);
    CHECK(static_cast<uint8_t *>(memory.cpu(0x19824,1))[0]==0);   /* the page-rounding tail stays zero */
    /* The ITCM path check reads back the first 4 words (record only). */
    uint32_t words[4]={};
    CHECK(A830GmuFirmware::checkItcm(blocks,io,words)==kIOReturnSuccess && words[0]==0x10007F80 && words[3]==0x159);
    window[0x3600c/4]=0;
    CHECK(A830GmuFirmware::checkItcm(blocks,io,words)==kIOReturnNotReady && words[3]==0);
    CHECK(A830GmuFirmware::checkItcm(blocks,io,nullptr)==kIOReturnBadArgument);
    /* Partial loads: DDR only (S4) touches no register; TCM only copies nothing. */
    std::memset(memory.cpu(0x4000,0x15824),0,0x15824);gmu={};barriers=0;
    CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadDdr)==kIOReturnSuccess && gmu.writes==0 && barriers==0);++loads;
    CHECK(std::memcmp(memory.cpu(0x4000,0x15824),&image[blocks.block[12].offset],0x15824)==0);
    std::memset(memory.cpu(0x4000,0x15824),0,0x15824);
    CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadTcm)==kIOReturnSuccess && gmu.writes==(0x1ec+0x28+0x18)/4);++loads;
    CHECK(static_cast<uint8_t *>(memory.cpu(0x4000,4))[0]==0);
    /* Loader refusals. */
    CHECK(A830GmuFirmware::load(blocks,io,memory,0)==kIOReturnBadArgument);++refusals;
    CHECK(A830GmuFirmware::load(blocks,io,memory,4)==kIOReturnBadArgument);++refusals;
    gmu={};gmu.fail_write=10;
    CHECK(A830GmuFirmware::load(blocks,io,memory)==kIOReturnIOError);++refusals;   /* a TCM write failed */
    A830GmuIo no_write{&gmu,read32,nullptr,barrier};
    CHECK(A830GmuFirmware::load(blocks,no_write,memory)==kIOReturnBadArgument);++refusals;
    CHECK(A830GmuFirmware::load(blocks,no_write,memory,A830GmuLoadDdr)==kIOReturnSuccess);
    /* A TCM word that does not take the image goes unnoticed in the design profile (checkItcm sees the first 4 only). */
    std::memset(window,0xcc,sizeof window);gmu={};gmu.stuck=0x36000+0x100;
    CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnSuccess && gmu.reads==0 && !check.mismatch);
    CHECK(window[(0x36000+0x100)/4]==0xcccccccc && A830GmuFirmware::checkItcm(blocks,io,words)==kIOReturnSuccess);

    /* The annibale profile (K/adreno8_gmu.c:517-546): each TCM block written, completed and read back word by word, each DDR
     * block compared after the copy; the check reports what was verified. */
    adreno830_profile_value=A830ProfileAnnibale;
    {
        const uint32_t tcm_words=(0x1ec+0x28+0x18)/4;
        std::memset(window,0xcc,sizeof window);gmu={};barriers=0;
        std::memset(memory.cpu(0x4000,0x15824),0,0x15824);
        CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnSuccess);++loads;
        CHECK(check.tcm_words==tcm_words && check.ddr_bytes==0x15824 && !check.mismatch && !check.read_failed);
        CHECK(gmu.writes==tcm_words && gmu.reads==tcm_words && barriers==3+1);   /* one per TCM block and the last */
        CHECK(window[0x36000/4]==0x10007F80 && window[(0x3a000+0x3ff0)/4]==0x20070001);
        CHECK(std::memcmp(memory.cpu(0x4000,0x15824),&image[blocks.block[12].offset],0x15824)==0);
        verified+=2;
        /* DDR only (S4): compared, no register touched; without read32 too (only the TCM read-back needs it). */
        gmu={};
        CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadDdr,&check)==kIOReturnSuccess);++loads;
        CHECK(check.ddr_bytes==0x15824 && check.tcm_words==0 && gmu.reads==0 && gmu.writes==0);
        A830GmuIo no_read{&gmu,nullptr,write32,barrier};
        CHECK(A830GmuFirmware::load(blocks,no_read,memory,A830GmuLoadDdr,&check)==kIOReturnSuccess && check.ddr_bytes==0x15824);
        CHECK(A830GmuFirmware::load(blocks,no_read,memory,A830GmuLoadAll,&check)==kIOReturnBadArgument);++refusals;
        CHECK(A830GmuFirmware::load(blocks,no_read,memory,A830GmuLoadTcm,&check)==kIOReturnBadArgument);++refusals;
        /* A TCM word that does not take the image: the load stops at it with the word read and the image's word; the
         * blocks after it (the DDR code, the DTCM) are not loaded. The first image word at or above ITCM 0x100 that is not 0
         * (a dropped write of 0 could not be seen). */
        uint32_t at=0x100;
        while(*reinterpret_cast<const uint32_t *>(&image[blocks.block[11].offset+at])==0)at+=4;
        CHECK(at<0x1ec);
        const uint32_t expect=*reinterpret_cast<const uint32_t *>(&image[blocks.block[11].offset+at]);
        std::memset(window,0xcc,sizeof window);gmu={};gmu.stuck=0x36000+at;
        std::memset(memory.cpu(0x4000,0x15824),0,0x15824);
        CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnIOError);++refusals;
        CHECK(check.mismatch && !check.read_failed && check.addr==at && check.read==0xcccccccc && check.expect==expect);
        CHECK(check.tcm_words==at/4 && check.ddr_bytes==0 && gmu.reads==at/4+1);
        CHECK(window[(0x3a000+0x3ff0)/4]==0xcccccccc && static_cast<uint8_t *>(memory.cpu(0x4000,4))[0]==0);
        /* A read-back access that fails (the 10th): reported as such, read 0xFFFFFFFF. */
        gmu={};gmu.fail_read=10;
        CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnIOError);++refusals;
        CHECK(check.read_failed && !check.mismatch && check.addr==4*9 && check.read==0xffffffffu && check.tcm_words==9);
        /* A failed TCM write is still a plain write failure (nothing verified). */
        gmu={};gmu.fail_write=10;
        CHECK(A830GmuFirmware::load(blocks,io,memory,A830GmuLoadAll,&check)==kIOReturnIOError);++refusals;
        CHECK(!check.mismatch && !check.read_failed && check.tcm_words==0 && gmu.reads==0);
        /* The same loads without a check structure. */
        gmu={};
        CHECK(A830GmuFirmware::load(blocks,io,memory)==kIOReturnSuccess && gmu.reads==tcm_words);
        verified+=6;
    }
    adreno830_profile_value=A830ProfileDesign;
    CHECK(memory.release()==kIOReturnSuccess);
    {
        /* A layout without the 0x4000 region: the main code falls in no area, and nothing at all is written. */
        NativeAdrenoGmuRequest requests[2]={{0x100000,0x1000,1},{0x60000000,0x1000,7}};
        NativeAdrenoGmuPlan partial{};
        CHECK(NativeAdrenoGmuPlanRequests(requests,2,0,&partial)==kIOReturnSuccess && partial.count==4);
        CHECK(memory.allocate(partial)==kIOReturnSuccess);
        gmu={};
        CHECK(A830GmuFirmware::load(blocks,io,memory)==kIOReturnBadArgument && gmu.writes==0);++refusals;
        CHECK(memory.release()==kIOReturnSuccess);
    }
    CHECK(fixture.live.empty());
    std::printf("PASS: %u parses of %s gen80000_gmu.bin (15 blocks, 11 PREALLOC, 4 data, ends 0x15B40), %u TCM/DDR loads, "
                "%u parser/plan/loader refusals; design loads read nothing back, annibale loads read every TCM word back and "
                "compare every DDR block (%u groups of checks)\n",parses,have_real?"the PKX110":"a synthetic",loads,refusals,
                verified);
}
