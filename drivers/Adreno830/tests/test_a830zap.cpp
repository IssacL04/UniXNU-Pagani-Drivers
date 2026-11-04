/* SPDX-License-Identifier: MIT */
/* host-test: A830Zap.cpp Adreno830Status.cpp NativeAdrenoSha256.cpp platform/sm8750/Sm8750Scm.c */
/* A830Zap against a model of the secure world behind SMC #0 and a model of the memory it uses. The model checks every
 * register of every SMC, the order of the whole sequence (metadata written and pushed out before PAS_INIT_IMAGE, the
 * region mapped only after PAS_MEM_SETUP and unmapped right after PAS_AUTH_AND_RESET, the metadata page freed only after
 * success), the exact metadata TZ is handed and the exact bytes in the region at authentication. PKX110's
 * gen80000_zap.mbn is read at test time (never copied into the repository): from $A830_ZAP_MBN, else from the device
 * backup next to the repository; synthetic images cover every refusal. */
#include "A830Zap.h"
#include "Adreno830Status.h"
#include "NativeAdrenoSha256.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(cond, ...)                                                                                                  \
    do {                                                                                                                  \
        if(!(cond)) {                                                                                                     \
            std::fprintf(stderr,"FAIL %s:%d: %s: ",__FILE__,__LINE__,#cond);                                             \
            std::fprintf(stderr,__VA_ARGS__);                                                                             \
            std::fputc('\n',stderr);                                                                                      \
            std::abort();                                                                                                 \
        }                                                                                                                 \
    } while(0)

namespace {
constexpr uint64_t MetaPhys=0x987654000ull;      /* a DRAM page of XNU's window, 4 KiB aligned */
constexpr uint64_t Region=0x9B09A000ull,RegionBytes=0x2000;
constexpr uint8_t PinnedSha[32]={0x1a,0xa7,0xaf,0x12,0x0c,0x4e,0xda,0x02,0xa3,0x72,0xc6,0xb1,0x93,0xf4,0xea,0xe9,
                                 0x27,0x2d,0x92,0xe2,0x49,0x43,0x00,0xa1,0xb9,0xbb,0x0d,0x5a,0xd0,0xd9,0x3c,0x43};

enum Event {EvAlloc,EvBarrier,EvInit,EvMemSetup,EvMap,EvAuth,EvUnmap,EvFree,EvRemote,EvOther};

/* ---- the world: SCM model + memory model ---- */
struct Step {
    uint64_t in[8];uint32_t mask;   /* registers that must match */
    uint64_t out[4];                /* x0..x3 returned */
    uint64_t x6;bool setX6;
    bool repeat;
};

struct World {
    std::vector<Step> steps;size_t next=0;unsigned smcs=0;
    uint64_t now=1000000;unsigned locks=0;int held=0;
    std::vector<Event> log;
    /* memory */
    alignas(4096) uint8_t meta[4096];
    alignas(4096) uint8_t region[RegionBytes];
    bool metaLive=false,mapped=false;unsigned allocs=0,frees=0,maps=0,unmaps=0,barriers=0;
    unsigned barrierAtAlloc=0,barrierAtMap=0;
    uint64_t mapPhys=0,mapBytes=0;
    bool failAlloc=false,failMap=false,wrongPhys=false;
    /* what TZ was given */
    std::vector<uint8_t> expectMeta;      /* metadata bytes the INIT_IMAGE check expects */
    std::vector<uint8_t> expectRegion;    /* region bytes the AUTH_AND_RESET check expects */
    bool checkMeta=false,checkRegion=false,metaChecked=false,regionChecked=false;
} *w;

Step &expect(uint64_t x0,uint64_t x1,uint64_t x2,uint64_t x3,uint64_t x4,uint64_t x5)
{
    w->steps.push_back({{x0,x1,x2,x3,x4,x5,0,0},0xff,{0,0,0,0},0,false,false});
    return w->steps.back();
}
Step &answer(Step &s,int64_t x0,uint64_t x1=0,uint64_t x2=0,uint64_t x3=0)
{
    s.out[0]=uint64_t(x0);s.out[1]=x1;s.out[2]=x2;s.out[3]=x3;return s;
}

Event eventOf(uint64_t x0)
{
    switch(uint32_t(x0)) {
    case 0x42000201: return EvInit;
    case 0x42000202: return EvMemSetup;
    case 0x42000205: return EvAuth;
    case 0x4200010a: return EvRemote;
    default: return EvOther;
    }
}

int modelSmc(void *,uint64_t regs[8])
{
    CHECK(w->next<w->steps.size(),"unexpected SMC #%u x0=%#llx x2=%#llx",w->smcs,(unsigned long long)regs[0],
          (unsigned long long)regs[2]);
    Step &s=w->steps[w->next];
    if(!s.repeat)++w->next;
    ++w->smcs;
    for(unsigned i=0;i<8;++i)
        CHECK(!(s.mask&1u<<i) || regs[i]==s.in[i],"SMC #%u: x%u = %#llx, expected %#llx",w->smcs,i,
              (unsigned long long)regs[i],(unsigned long long)s.in[i]);
    const bool fast=(regs[0]&0x80000000u)!=0;
    CHECK(fast || w->held==1,"yielding SMC without the SCM lock");
    const Event e=eventOf(regs[0]);
    if(e!=EvOther && (w->log.empty() || w->log.back()!=e || regs[0]==1))w->log.push_back(e);
    if(e==EvInit && w->checkMeta && !w->metaChecked) {
        /* TZ reads the metadata from the physical page: it must be complete and pushed out by a barrier. */
        CHECK(regs[3]==MetaPhys && w->metaLive,"metadata page");
        CHECK(w->barriers>w->barrierAtAlloc,"metadata not pushed out before PAS_INIT_IMAGE");
        CHECK(std::memcmp(w->meta,w->expectMeta.data(),w->expectMeta.size())==0,"metadata bytes");
        for(size_t i=w->expectMeta.size();i<sizeof w->meta;++i)CHECK(w->meta[i]==0,"metadata padding at %zu",i);
        w->metaChecked=true;
    }
    if(e==EvAuth && w->checkRegion && !w->regionChecked) {
        CHECK(w->mapped && w->barriers>w->barrierAtMap,"region not written and pushed out before AUTH_AND_RESET");
        CHECK(std::memcmp(w->region,w->expectRegion.data(),w->expectRegion.size())==0,"region bytes");
        for(size_t i=w->expectRegion.size();i<RegionBytes;++i)CHECK(w->region[i]==0xa5,"region beyond the span at %zu",i);
        w->regionChecked=true;
    }
    w->now+=5;
    for(unsigned i=0;i<8;++i)regs[i]=0xbad0000000000000ull|uint64_t(i)<<8|w->smcs;
    for(unsigned i=0;i<4;++i)regs[i]=s.out[i];
    if(s.setX6)regs[6]=s.x6;
    return 0;
}
uint64_t modelNow(void *) {return w->now;}
void modelSleep(void *,uint32_t us) {w->now+=us;}
void modelLock(void *) {CHECK(w->held==0,"recursive lock");++w->held;++w->locks;}
void modelUnlock(void *) {CHECK(w->held==1,"unlock");--w->held;}

IOReturn modelAlloc(void *,size_t bytes,void **cpu,uint64_t *phys,void **handle)
{
    CHECK(bytes==4096,"metadata allocation of %zu bytes",bytes);
    CHECK(!w->metaLive,"second metadata page");
    w->log.push_back(EvAlloc);
    if(w->failAlloc)return kIOReturnNoMemory;
    ++w->allocs;w->metaLive=true;w->barrierAtAlloc=w->barriers;
    std::memset(w->meta,0xcc,sizeof w->meta);
    *cpu=w->meta;*phys=w->wrongPhys?MetaPhys+0x10:MetaPhys;*handle=w->meta;
    return kIOReturnSuccess;
}
void modelFree(void *,void *handle)
{
    CHECK(handle==w->meta && w->metaLive,"free of a page not allocated");
    w->log.push_back(EvFree);++w->frees;w->metaLive=false;
}
IOReturn modelMap(void *,uint64_t phys,uint64_t bytes,void **cpu,void **handle)
{
    CHECK(!w->mapped,"region mapped twice");
    w->log.push_back(EvMap);
    if(w->failMap)return kIOReturnNoMemory;
    CHECK(phys==Region && bytes<=RegionBytes && !(bytes&4095),"region mapping %#llx+%#llx",(unsigned long long)phys,
          (unsigned long long)bytes);
    ++w->maps;w->mapped=true;w->mapPhys=phys;w->mapBytes=bytes;w->barrierAtMap=w->barriers;
    *cpu=w->region;*handle=w->region;
    return kIOReturnSuccess;
}
void modelUnmap(void *,void *handle)
{
    CHECK(handle==w->region && w->mapped,"unmap without mapping");
    w->log.push_back(EvUnmap);++w->unmaps;w->mapped=false;
}
void modelBarrier(void *) {++w->barriers;if(w->log.empty() || w->log.back()!=EvBarrier)w->log.push_back(EvBarrier);}

Sm8750ScmOps scmOps={nullptr,modelSmc,modelNow,modelSleep,modelLock,modelUnlock,nullptr,nullptr};
const A830ZapMemoryOps memoryOps={nullptr,modelAlloc,modelFree,modelMap,modelUnmap,modelBarrier};
Sm8750Scm scm;

World *fresh()
{
    static World world;
    world=World();
    w=&world;
    w->steps.reserve(64);
    std::memset(w->region,0xa5,sizeof w->region);
    Sm8750ScmInit(&scm,&scmOps);
    adreno830_status_reset();
    return w;
}

void done()
{
    CHECK(w->next==w->steps.size(),"script: %zu of %zu steps used",w->next,w->steps.size());
    CHECK(w->held==0 && !w->mapped,"lock or mapping left behind");
}

bool sameLog(std::initializer_list<Event> expected)
{
    return w->log.size()==expected.size() && std::equal(expected.begin(),expected.end(),w->log.begin());
}

/* ---- images ---- */
void wr16(std::vector<uint8_t> &v,size_t at,uint16_t x) {v[at]=uint8_t(x);v[at+1]=uint8_t(x>>8);}
void wr32(std::vector<uint8_t> &v,size_t at,uint32_t x) {for(unsigned i=0;i<4;++i)v[at+i]=uint8_t(x>>(8*i));}
struct Ph {uint32_t type,offset,vaddr,paddr,filesz,memsz,flags,align;};
void putPh(std::vector<uint8_t> &v,unsigned index,const Ph &p)
{
    const size_t at=0x34+32*size_t(index);
    const uint32_t f[8]={p.type,p.offset,p.vaddr,p.paddr,p.filesz,p.memsz,p.flags,p.align};
    for(unsigned i=0;i<8;++i)wr32(v,at+4*i,f[i]);
}
/* The shape of gen80000_zap.mbn: [0,0x94) ELF + 3 headers, segment at 0x1000 (0x3B4 bytes), hash at 0x2000 (0xF38). */
std::vector<uint8_t> synthetic(uint32_t paddr=0x1000,uint32_t flags=0x08000007,uint32_t filesz=0x3B4,uint32_t memsz=0x3B4)
{
    std::vector<uint8_t> v(0x2F38);
    for(size_t i=0;i<v.size();++i)v[i]=uint8_t(i*7+3);
    const uint8_t ident[16]={0x7f,'E','L','F',1,1,1,0,0,0,0,0,0,0,0,0};
    std::memcpy(v.data(),ident,16);
    wr16(v,16,2);wr16(v,18,164);wr32(v,20,1);wr32(v,24,paddr);wr32(v,28,0x34);wr32(v,32,0);wr32(v,36,3);
    wr16(v,40,52);wr16(v,42,32);wr16(v,44,3);wr16(v,46,0);wr16(v,48,0);wr16(v,50,0);
    putPh(v,0,{0,0,0,0,0x94,0,0x07000000,0});
    putPh(v,1,{1,0x1000,paddr,paddr,filesz,memsz,flags,0x100000});
    putPh(v,2,{0,0x2000,0,0,0xF38,0xF38,0x02000000,0x1000});
    return v;
}
std::vector<uint8_t> metadataOf(const std::vector<uint8_t> &v,uint32_t header=0x94,uint32_t hashOff=0x2000,uint32_t hash=0xF38)
{
    std::vector<uint8_t> m(v.begin(),v.begin()+header);
    m.insert(m.end(),v.begin()+hashOff,v.begin()+hashOff+hash);
    return m;
}
std::vector<uint8_t> regionOf(const std::vector<uint8_t> &v,uint32_t at,uint32_t off,uint32_t filesz,uint32_t span)
{
    std::vector<uint8_t> r(span,0);
    std::memcpy(r.data()+at,v.data()+off,filesz);
    return r;
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> bytes;
    if(FILE *f=std::fopen(path.c_str(),"rb")) {
        uint8_t chunk[4096];size_t n;
        while((n=std::fread(chunk,1,sizeof chunk,f))>0)bytes.insert(bytes.end(),chunk,chunk+n);
        std::fclose(f);
    }
    return bytes;
}
std::string firmwarePath()
{
    if(const char *env=std::getenv("A830_ZAP_MBN"))return env;
    std::string here=__FILE__;                                   /* <repo>/tests/test_a830zap.cpp */
    const size_t cut=here.rfind("/tests/");
    if(cut==std::string::npos)return "";
    return here.substr(0,cut)+"/../一加13T原厂备份/GPU固件/gen80000_zap.mbn";
}

/* The successful first load of an image laid out like PKX110's: the full register script. */
void scriptSuccess(uint32_t span=0x1000)
{
    answer(expect(0x42000201,0x82,13,MetaPhys,0,0),0,0);
    answer(expect(0x42000202,3,13,Region,span,0),0,0);
    answer(expect(0x42000205,1,13,0,0,0),0,0);
}
}

int main()
{
    A830ZapImage image;
    unsigned checks=0;

    /* ---- 1. PKX110's gen80000_zap.mbn, when the device backup is on this machine ---- */
    const std::string path=firmwarePath();
    const std::vector<uint8_t> real=readFile(path);
    bool haveReal=false;
    if(!real.empty()) {
        uint8_t digest[32];
        NativeAdrenoSha256::hash(real.data(),real.size(),digest);
        CHECK(real.size()==12088 && std::memcmp(digest,PinnedSha,32)==0,"%s is not the pinned gen80000_zap.mbn",path.c_str());
        haveReal=true;
        CHECK(A830Zap::parseMdt(real.data(),real.size(),&image)==kIOReturnSuccess,"parse: error %u",image.error);
        CHECK(image.entry==0x1000 && image.phnum==3 && image.header_bytes==0x94 && image.hash_index==2 &&
              image.hash_offset==0x2000 && image.hash_bytes==0xF38 && image.loads==1 && image.load.index==1 &&
              image.load.offset==0x1000 && image.load.paddr==0x1000 && image.load.filesz==0x3B4 && image.load.memsz==0x3B4 &&
              image.load.flags==0x08000007 && image.min_paddr==0x1000 && image.max_paddr==0x2000 && image.span==0x1000 &&
              image.metadata_bytes==0xFCC && image.relocatable && image.error==A830ZapErrorNone,"PKX110 layout");
        std::vector<uint8_t> meta(4096,0xee);
        size_t built=0;
        CHECK(A830Zap::buildMetadata(image,meta.data(),meta.size(),&built)==kIOReturnSuccess && built==0xFCC,"metadata");
        const std::vector<uint8_t> want=metadataOf(real);
        CHECK(want.size()==0xFCC && std::memcmp(meta.data(),want.data(),0xFCC)==0,"metadata = [0,0x94) + [0x2000,0x2F38)");
        for(size_t i=0xFCC;i<4096;++i)CHECK(meta[i]==0,"metadata padding");
        /* The whole first load of the real image. */
        fresh();
        w->checkMeta=w->checkRegion=true;w->expectMeta=want;w->expectRegion=regionOf(real,0,0x1000,0x3B4,0x1000);
        scriptSuccess();
        A830Zap zap;
        CHECK(zap.init(&scm,&memoryOps)==kIOReturnSuccess,"init");
        CHECK(zap.load(real.data(),real.size(),Region,RegionBytes)==kIOReturnSuccess,"load of the real image");
        CHECK(w->metaChecked && w->regionChecked && zap.loaded() && w->mapBytes==0x1000,"TZ saw the real image");
        CHECK(sameLog({EvAlloc,EvBarrier,EvInit,EvMemSetup,EvMap,EvBarrier,EvAuth,EvUnmap,EvFree}),"order");
        done();
        checks+=5;
    }

    /* ---- 2. parseMdt and buildMetadata on synthetic images ---- */
    std::vector<uint8_t> v=synthetic();
    CHECK(A830Zap::parseMdt(v.data(),v.size(),&image)==kIOReturnSuccess && image.span==0x1000 && image.relocatable &&
          image.metadata_bytes==0xFCC,"synthetic PKX110 shape");
    struct Refusal {const char *what;void (*mutate)(std::vector<uint8_t> &);A830ZapError error;};
    const Refusal refusals[]={
        {"short",[](std::vector<uint8_t> &x) {x.resize(51);},A830ZapErrorShort},
        {"magic",[](std::vector<uint8_t> &x) {x[1]='e';},A830ZapErrorIdent},
        {"ELF64",[](std::vector<uint8_t> &x) {x[4]=2;},A830ZapErrorIdent},
        {"big endian",[](std::vector<uint8_t> &x) {x[5]=2;},A830ZapErrorIdent},
        {"version",[](std::vector<uint8_t> &x) {x[6]=0;},A830ZapErrorIdent},
        {"phentsize",[](std::vector<uint8_t> &x) {wr16(x,42,56);},A830ZapErrorHeaders},
        {"headers beyond the file",[](std::vector<uint8_t> &x) {wr32(x,28,uint32_t(x.size()-40));},A830ZapErrorHeaders},
        {"too many headers",[](std::vector<uint8_t> &x) {wr16(x,44,65);},A830ZapErrorHeaders},
        {"section table beyond the file",[](std::vector<uint8_t> &x) {wr16(x,46,40);wr16(x,48,4);wr32(x,32,0x2F00);},
         A830ZapErrorHeaders},
        {"section entry size",[](std::vector<uint8_t> &x) {wr16(x,46,32);wr16(x,48,1);wr32(x,32,0x100);},A830ZapErrorHeaders},
        {"one header",[](std::vector<uint8_t> &x) {wr16(x,44,1);},A830ZapErrorFirstHeader},
        {"first header PT_LOAD",[](std::vector<uint8_t> &x) {wr32(x,0x34,1);},A830ZapErrorFirstHeader},
        {"no hash",[](std::vector<uint8_t> &x) {wr32(x,0x34+64+24,0x01000000);},A830ZapErrorHash},
        {"hash beyond the file",[](std::vector<uint8_t> &x) {wr32(x,0x34+64+4,0x2100);},A830ZapErrorMetadata},
        {"header part beyond the file",[](std::vector<uint8_t> &x) {wr32(x,0x34+16,0x3000);},A830ZapErrorMetadata},
        {"metadata above a page",[](std::vector<uint8_t> &x) {wr32(x,0x34+16,0x100);},A830ZapErrorMetadata},
        {"no loadable segment",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+20,0);},A830ZapErrorLoad},
        {"loadable segment of hash type",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+24,0x0A000007);},A830ZapErrorLoad},
        {"two loadable segments",[](std::vector<uint8_t> &x) {wr16(x,44,4);putPh(x,3,{1,0x1000,0x3000,0x3000,0x100,0x100,
                                                                                       0x08000007,0});},
         A830ZapErrorLoad},
        {"filesz above memsz",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+20,0x300);},A830ZapErrorSegment},
        {"segment beyond the file",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+4,0x2E00);},A830ZapErrorSegment},
        {"unaligned load address",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+12,0x1010);},A830ZapErrorAlignment},
        {"span above every region",[](std::vector<uint8_t> &x) {wr32(x,0x34+32+20,0x2400);},A830ZapErrorSpan},
    };
    for(const Refusal &r:refusals) {
        std::vector<uint8_t> x=synthetic();
        r.mutate(x);
        CHECK(A830Zap::parseMdt(x.data(),x.size(),&image)==kIOReturnBadArgument && image.error==r.error,"%s: error %u",
              r.what,image.error);
        ++checks;
    }
    CHECK(A830Zap::parseMdt(nullptr,100,&image)==kIOReturnBadArgument && image.error==A830ZapErrorShort,"null image");
    CHECK(A830Zap::parseMdt(v.data(),v.size(),nullptr)==kIOReturnBadArgument,"null output");
    /* Split image: the hash follows the header part directly and the file is exactly the two (Linux's first case). */
    {
        std::vector<uint8_t> split(v.begin(),v.begin()+0x94);
        split.insert(split.end(),v.begin()+0x2000,v.end());
        wr32(split,0x34+32+4,0x94);wr32(split,0x34+32+16,0);   /* segment's bytes not in this file: filesz 0 */
        CHECK(A830Zap::parseMdt(split.data(),split.size(),&image)==kIOReturnSuccess && image.hash_offset==0x94 &&
              image.metadata_bytes==0xFCC,"split metadata");
        std::vector<uint8_t> meta(4096);
        size_t built=0;
        CHECK(A830Zap::buildMetadata(image,meta.data(),meta.size(),&built)==kIOReturnSuccess &&
              std::memcmp(meta.data(),split.data(),0xFCC)==0,"split metadata bytes");
    }
    /* buildMetadata refusals: too small, unparsed. */
    {
        CHECK(A830Zap::parseMdt(v.data(),v.size(),&image)==kIOReturnSuccess,"reparse");
        std::vector<uint8_t> small(0xFCB);
        size_t built=7;
        CHECK(A830Zap::buildMetadata(image,small.data(),small.size(),&built)==kIOReturnBadArgument && built==0,"small buffer");
        A830ZapImage bad=image;bad.error=A830ZapErrorHash;
        CHECK(A830Zap::buildMetadata(bad,small.data(),small.size(),nullptr)==kIOReturnBadArgument,"refused image");
    }
    /* loadSegments: relocation from the lowest address, zero fill, the region's bounds. */
    {
        std::vector<uint8_t> x=synthetic(0x5000,0x08000007,0x100,0x300);   /* bss of 0x200 */
        CHECK(A830Zap::parseMdt(x.data(),x.size(),&image)==kIOReturnSuccess && image.span==0x1000,"bss image");
        alignas(16) static volatile uint8_t buf[0x2000];
        for(auto &b:buf)b=0x5a;
        CHECK(A830Zap::loadSegments(image,buf,Region,0x1000)==kIOReturnSuccess,"load bss image");
        for(unsigned i=0;i<0x100;++i)CHECK(buf[i]==x[0x1000+i],"segment byte %u",i);
        for(unsigned i=0x100;i<0x1000;++i)CHECK(buf[i]==0,"zero fill at %u",i);
        for(unsigned i=0x1000;i<0x2000;++i)CHECK(buf[i]==0x5a,"beyond the span at %u",i);
        CHECK(A830Zap::loadSegments(image,buf,Region,0x800)==kIOReturnBadArgument,"region smaller than the span");
        CHECK(A830Zap::loadSegments(image,nullptr,Region,0x1000)==kIOReturnBadArgument,"no mapping");
        /* Not relocatable: the segment keeps its address, which must lie in the region. */
        std::vector<uint8_t> fixed=synthetic(uint32_t(Region+0x1000),0x00000007);
        CHECK(A830Zap::parseMdt(fixed.data(),fixed.size(),&image)==kIOReturnSuccess && !image.relocatable,"fixed image");
        for(auto &b:buf)b=0x5a;
        CHECK(A830Zap::loadSegments(image,buf,Region,0x2000)==kIOReturnSuccess,"fixed image load");
        for(unsigned i=0;i<0x1000;++i)CHECK(buf[i]==0x5a,"fixed image: nothing below its address (%u)",i);
        for(unsigned i=0;i<0x3B4;++i)CHECK(buf[0x1000+i]==fixed[0x1000+i],"fixed image byte %u",i);
        CHECK(A830Zap::loadSegments(image,buf,Region,0x1800)==kIOReturnBadArgument,"fixed image outside a short region");
        std::vector<uint8_t> elsewhere=synthetic(0x80000000u,0x00000007);
        CHECK(A830Zap::parseMdt(elsewhere.data(),elsewhere.size(),&image)==kIOReturnSuccess &&
              A830Zap::loadSegments(image,buf,Region,0x2000)==kIOReturnBadArgument,"fixed image not in the region");
    }
    checks+=4;

    /* ---- 3. the first load: registers, order, memory and status words ---- */
    {
        fresh();
        w->checkMeta=w->checkRegion=true;w->expectMeta=metadataOf(v);w->expectRegion=regionOf(v,0,0x1000,0x3B4,0x1000);
        scriptSuccess();
        A830Zap zap;
        CHECK(zap.init(&scm,&memoryOps)==kIOReturnSuccess,"init");
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnSuccess,"load");
        CHECK(zap.loaded() && w->allocs==1 && w->frees==1 && w->maps==1 && w->unmaps==1 && !w->metaLive,"resources");
        CHECK(sameLog({EvAlloc,EvBarrier,EvInit,EvMemSetup,EvMap,EvBarrier,EvAuth,EvUnmap,EvFree}),"order");
        const A830ZapReport &r=zap.report();
        CHECK(r.step==A830ZapStepRelease && r.loads==1 && r.smcs==3 && r.waitq==0 && r.x0==0 && r.x1==0 &&
              r.fid==0x42000205u && r.metadata_phys==MetaPhys && r.metadata_bytes==0xFCC && r.span==0x1000 &&
              !r.metadata_held,"report");
        CHECK(adreno830_status.step==A830ZapStepRelease && adreno830_status.aux[A830ZapAuxX0]==0 &&
              adreno830_status.aux[A830ZapAuxWord]==(uint64_t(7)<<32|uint64_t(3)<<48),"status words %#llx",
              (unsigned long long)adreno830_status.aux[A830ZapAuxWord]);
        /* The orchestrator finishes S7 (steps 8-12) and notes the ZAP words again: the S7 step it reached stays. */
        adreno830_status.stage=A830SZap;
        adreno830_status_step(A830ZapStepOobClear);
        zap.noteStatus();
        CHECK(adreno830_status.step==A830ZapStepOobClear && (adreno830_status.aux[A830ZapAuxWord]>>32&0xff)==12,
              "step 12 after a complete secure exit");
        adreno830_status.stage=A830SCp;   /* outside S7 the ZAP's own step is shown */
        zap.noteStatus();
        CHECK(adreno830_status.step==A830ZapStepRelease,"step outside S7");
        A830ZapFailure f;
        zap.failure(&f);
        CHECK(f.reason==0 && f.step==A830ZapStepRelease,"no failure");
        /* Once per boot: a second load issues nothing. */
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnSuccess && w->smcs==3 && w->allocs==1,"second load");
        /* Resume: the default issues nothing; SET_REMOTE_STATE(0, 13) on request. */
        CHECK(zap.resume(A830ZapResumeSecureMode)==kIOReturnSuccess && w->smcs==3,"secure-mode resume");
        answer(expect(0x4200010a,2,0,13,0,0),0,0);
        CHECK(zap.resume(A830ZapResumeRemoteState)==kIOReturnSuccess && zap.report().resumes==1 &&
              zap.report().fid==0x4200010au,"remote-state resume");
        answer(expect(0x4200010a,2,0,13,0,0),0,0x16);
        CHECK(zap.resume(A830ZapResumeRemoteState)==kIOReturnIOError && zap.report().resume_failures==1 && zap.loaded(),
              "failed remote-state resume keeps the ZAP");
        zap.failure(&f);
        CHECK(f.step==A830ZapStepRemoteState && f.reason==A830ReasonSmc && f.reg==0x4200010a && f.value==0x16,"resume failure");
        CHECK(zap.resume(A830ZapResume(7))==kIOReturnBadArgument,"unknown resume mode");
        done();
        checks+=8;
    }

    /* ---- 4. PAS failures keep the metadata page and stop loading for this boot ---- */
    struct PasFailure {const char *what;unsigned failAt;int64_t x0;uint64_t x1;IOReturn ior;A830ZapStep step;};
    const PasFailure pasFailures[]={
        {"INIT_IMAGE x0",0,-1,0,kIOReturnIOError,A830ZapStepInitImage},
        {"INIT_IMAGE x1",0,0,0x5,kIOReturnIOError,A830ZapStepInitImage},
        {"MEM_SETUP x0",1,-3,0,kIOReturnIOError,A830ZapStepMemSetup},
        {"MEM_SETUP x1",1,0,0xfffffff2,kIOReturnIOError,A830ZapStepMemSetup},
        {"AUTH_AND_RESET x0",2,-2,0,kIOReturnIOError,A830ZapStepAuthReset},
        {"AUTH_AND_RESET x1",2,0,1,kIOReturnIOError,A830ZapStepAuthReset},
        {"AUTH_AND_RESET -4",2,-4,0,kIOReturnIOError,A830ZapStepAuthReset},
    };
    for(const PasFailure &p:pasFailures) {
        fresh();
        const uint64_t fids[3]={0x42000201,0x42000202,0x42000205};
        const uint64_t args[3][4]={{0x82,13,MetaPhys,0},{3,13,Region,0x1000},{1,13,0,0}};
        for(unsigned i=0;i<=p.failAt;++i) {
            Step &s=expect(fids[i],args[i][0],args[i][1],args[i][2],args[i][3],0);
            if(i==p.failAt)answer(s,p.x0,p.x1);
            else answer(s,0,0);
        }
        A830Zap zap;
        zap.init(&scm,&memoryOps);
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==p.ior,"%s: status",p.what);
        const A830ZapReport &r=zap.report();
        A830ZapFailure f;
        zap.failure(&f);
        CHECK(!zap.loaded() && r.metadata_held && w->metaLive && w->frees==0 && r.step==p.step && f.step==p.step &&
              f.reason==A830ReasonSmc && f.reg==fids[p.failAt] && f.value==(p.x0?uint64_t(p.x0):p.x1),"%s: kept",p.what);
        CHECK(adreno830_status.step==p.step && adreno830_status.aux[A830ZapAuxX0]==uint64_t(p.x0) &&
              uint32_t(adreno830_status.aux[A830ZapAuxWord])==uint32_t(p.x1) &&
              (adreno830_status.aux[A830ZapAuxWord]>>32&0xff)==p.step,"%s: status words",p.what);
        CHECK(p.failAt<2?w->maps==0:(w->maps==1 && w->unmaps==1),"%s: region mapping",p.what);
        /* No second attempt this boot, and no SMC for it. */
        const unsigned before=w->smcs;
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnNotReady && w->smcs==before && w->allocs==1,
              "%s: no reload",p.what);
        zap.failure(&f);
        CHECK(f.reason==A830ReasonUnexpectedState,"%s: reload refused",p.what);
        CHECK(zap.resume(A830ZapResumeRemoteState)==kIOReturnNotReady && w->smcs==before,"%s: no resume",p.what);
        w->metaLive=false;   /* the page is held for good; the model forgets it */
        done();
        ++checks;
    }

    /* ---- 5. SCM behaviour under the PAS calls: EBUSY, INTERRUPTED, WAITQ (poll) ---- */
    {
        fresh();
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),-12);
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),0,0);
        Step &interrupted=answer(expect(0x42000202,3,13,Region,0x1000,0),1);
        interrupted.x6=0x77;interrupted.setX6=true;
        Step &resumed=answer(expect(1,3,13,Region,0x1000,0),0,0);
        resumed.in[6]=0x77;
        answer(expect(0x42000205,1,13,0,0,0),2,1,0x31);                    /* WAITQ_SLEEP on context 1 */
        answer(expect(0xc2002403,0,0,0,0,0),-1);                           /* nothing pending yet */
        answer(expect(0xc2002403,0,0,0,0,0),0,1,1,0);                      /* context 1, WAKE_ONE */
        answer(expect(0x42002402,1,0x31,0,0,0),0,0);                       /* WAITQ_RESUME(call context) */
        A830Zap zap;
        zap.init(&scm,&memoryOps);
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnSuccess,"load through EBUSY/INTERRUPTED/WAITQ");
        const A830ZapReport &r=zap.report();
        CHECK(r.busy==1 && r.interrupted==1 && r.waitq==1 && r.smcs==8 && zap.loaded(),"counters: busy %u int %u wq %u smcs %u",
              r.busy,r.interrupted,r.waitq,r.smcs);
        CHECK((adreno830_status.aux[A830ZapAuxWord]>>40&0xff)==1 && (adreno830_status.aux[A830ZapAuxWord]>>48)==8,
              "wq and SMC counts in the status word");
        done();
        /* EBUSY forever: Busy after 20 retries, metadata held. */
        fresh();
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),-12).repeat=true;
        A830Zap busy;
        busy.init(&scm,&memoryOps);
        CHECK(busy.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnBusy && busy.report().busy==20 &&
              busy.report().metadata_held,"EBUSY forever");
        w->metaLive=false;
        /* WAITQ never woken: the 1 s budget ends it. */
        fresh();
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),0,0);
        answer(expect(0x42000202,3,13,Region,0x1000,0),0,0);
        answer(expect(0x42000205,1,13,0,0,0),2,0,0x12);
        answer(expect(0xc2002403,0,0,0,0,0),-1).repeat=true;
        A830Zap stuck;
        stuck.init(&scm,&memoryOps);
        CHECK(stuck.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnTimeout && stuck.report().waitq==1 &&
              !w->mapped && stuck.report().metadata_held,"WAITQ timeout unmaps the region and keeps the metadata");
        w->metaLive=false;
        checks+=3;
    }

    /* ---- 6. memory failures and refusals before any SMC ---- */
    {
        fresh();
        w->failAlloc=true;
        A830Zap zap;
        zap.init(&scm,&memoryOps);
        A830ZapFailure f;
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnNoMemory && w->smcs==0,"no metadata page");
        zap.failure(&f);
        CHECK(f.step==A830ZapStepMetadata && f.reason==A830ReasonNoMemory && !zap.report().metadata_held,"alloc failure");
        /* Nothing reached TZ: a later attempt may still load. */
        w->failAlloc=false;w->log.clear();
        scriptSuccess();
        CHECK(zap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnSuccess && zap.loaded(),"load after an alloc failure");
        done();

        fresh();
        w->wrongPhys=true;
        A830Zap misaligned;
        misaligned.init(&scm,&memoryOps);
        CHECK(misaligned.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnNoMemory && w->smcs==0 && w->frees==1,
              "unaligned metadata page is returned, never handed to TZ");
        done();

        fresh();
        w->failMap=true;
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),0,0);
        answer(expect(0x42000202,3,13,Region,0x1000,0),0,0);
        A830Zap nomap;
        nomap.init(&scm,&memoryOps);
        CHECK(nomap.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnNoMemory && nomap.report().metadata_held &&
              !nomap.loaded(),"region mapping failure after MEM_SETUP");
        nomap.failure(&f);
        CHECK(f.step==A830ZapStepLoad && f.reason==A830ReasonNoMemory && f.reg==Region,"map failure record");
        w->metaLive=false;
        done();

        /* Refused images and regions: no allocation, no SMC; reason Firmware. */
        fresh();
        A830Zap refused;
        refused.init(&scm,&memoryOps);
        std::vector<uint8_t> bad=synthetic();
        bad[0]=0;
        CHECK(refused.load(bad.data(),bad.size(),Region,RegionBytes)==kIOReturnBadArgument && w->allocs==0 && w->smcs==0,
              "bad image");
        refused.failure(&f);
        CHECK(f.step==A830ZapStepParse && f.reason==A830ReasonFirmware && f.reg==A830ZapErrorIdent && f.value==bad.size() &&
              refused.report().error==A830ZapErrorIdent,"bad image record");
        CHECK(refused.load(v.data(),v.size(),Region,0x800)==kIOReturnBadArgument && w->allocs==0,"region below the span");
        refused.failure(&f);
        CHECK(f.reason==A830ReasonFirmware && f.reg==A830ZapErrorSpan,"span record");
        CHECK(refused.load(v.data(),v.size(),Region+0x10,RegionBytes)==kIOReturnBadArgument,"unaligned region");
        CHECK(refused.load(nullptr,0,Region,RegionBytes)==kIOReturnBadArgument,"no image");
        /* Not initialised. */
        A830Zap bare;
        CHECK(bare.load(v.data(),v.size(),Region,RegionBytes)==kIOReturnNotReady,"not initialised");
        CHECK(bare.resume(A830ZapResumeSecureMode)==kIOReturnNotReady,"resume without a load");
        CHECK(bare.init(nullptr,&memoryOps)==kIOReturnBadArgument && bare.init(&scm,nullptr)==kIOReturnBadArgument,"init args");
        A830ZapMemoryOps partial=memoryOps;partial.barrier=nullptr;
        CHECK(bare.init(&scm,&partial)==kIOReturnBadArgument,"missing barrier");
        done();
        checks+=6;
    }

    /* ---- 7. a fixed (non-relocatable) image: no MEM_SETUP, placed at its own address ---- */
    {
        fresh();
        std::vector<uint8_t> fixed=synthetic(uint32_t(Region),0x00000007);
        w->checkRegion=true;w->expectRegion=regionOf(fixed,0,0x1000,0x3B4,0x1000);
        answer(expect(0x42000201,0x82,13,MetaPhys,0,0),0,0);
        answer(expect(0x42000205,1,13,0,0,0),0,0);
        A830Zap zap;
        zap.init(&scm,&memoryOps);
        Sm8750SmcResult out[3];
        CHECK(A830Zap::parseMdt(fixed.data(),fixed.size(),&image)==kIOReturnSuccess,"fixed parse");
        CHECK(zap.authenticate(image,Region,RegionBytes,out)==kIOReturnSuccess && out[0].fid==0x42000201u && out[1].fid==0 &&
              out[2].fid==0x42000205u && w->regionChecked && zap.loaded(),"fixed image authenticated without MEM_SETUP");
        CHECK(sameLog({EvAlloc,EvBarrier,EvInit,EvMap,EvBarrier,EvAuth,EvUnmap,EvFree}),"fixed order");
        done();
        ++checks;
    }

    std::printf("PASS: A830Zap MDT parse (%s), metadata 0xFCC, PAS 13 INIT_IMAGE/MEM_SETUP/AUTH_AND_RESET with EBUSY, "
                "INTERRUPTED and WAITQ, failures keep the metadata page, SET_REMOTE_STATE resume; %u groups\n",
                haveReal?"gen80000_zap.mbn and synthetic":"synthetic only, gen80000_zap.mbn not found",checks);
    return 0;
}
