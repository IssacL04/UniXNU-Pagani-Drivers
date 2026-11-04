/* SPDX-License-Identifier: MIT */
/* Host model of the SM8750 GPU SMMU (MMU-500 at 0x3DA0000) as EL1 sees it on this phone and on the same SoC (ARCHITECTURE
 * 4.5.1): the global space is emulated by the hypervisor (sCR0 writes dropped, S2CR 0x03000018 and SMR 0 at boot, an sGFSR
 * write-back that may be ignored), the context banks hold random values at boot (CFIE and M set, fault bits in FSR), SCTLR does
 * not keep ASIDPNE, TCR2 reads TBI0/TBI1 as 1, ACTLR does not take a write while sACR.CACHE_LOCK is set, FSR is write-one-to-
 * clear, TLBSTATUS stays busy for a while after TLBSYNC. Writes the hardware punished are counted as aborts (an S2CR translating
 * to CB1, an S2CR with SHCFG 0) and access widths are checked: TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL only as one 64-bit store,
 * TLBIASID/TLBIALL/TLBSYNC/SCTLR/FSR only as 32-bit stores. Also the PaganiArmCpu SPI interface and a clock for the platform. */
#ifndef A830_SMMU_MODEL_H
#define A830_SMMU_MODEL_H
#include "A830Smmu.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define MODEL_CHECK(x) do {if(!(x)){std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(1);}}while(0)

struct A830SmmuModel {
    enum : uint32_t {Bytes=0x40000,GlobalPages=32,Banks=21,Smrs=24};
    alignas(4096) uint32_t r[Bytes/4];
    /* behaviour */
    bool sgfsr_write_ignored=true,sacr_write_ignored=false,tlb_stuck=false;
    unsigned tlb_busy=2;
    /* observation */
    unsigned aborts=0,violations=0,tlbiasid=0,tlbiall=0,tlbsync=0,writes=0,reads=0;
    std::vector<uint64_t> tlbival;
    std::vector<std::pair<uint32_t,uint32_t>> s2cr_writes;   /* (slot, value) in order */
    uint32_t busy_left[Banks]{};
    uint32_t last_tlbiasid=0;
    ArmSmmu4KIo io{};

    static constexpr uint32_t cb(unsigned bank) {return (GlobalPages+bank)*0x1000u;}
    uint32_t &at(uint32_t offset) {MODEL_CHECK(offset%4==0 && offset<Bytes);return r[offset/4];}
    uint64_t at64(uint32_t offset) {return uint64_t(at(offset+4))<<32|at(offset);}
    void set64(uint32_t offset,uint64_t value) {at(offset)=uint32_t(value);at(offset+4)=uint32_t(value>>32);}
    /* Boot state: seed drives the random context banks (every boot differs, ARCHITECTURE 4.5.1). */
    void boot(uint32_t seed,uint32_t sgfsr=0,bool cache_lock=false)
    {
        std::memset(r,0,sizeof r);
        aborts=violations=tlbiasid=tlbiall=tlbsync=writes=reads=0;tlbival.clear();s2cr_writes.clear();
        for(auto &b:busy_left)b=0;
        at(0x00)=0x002E0406;at(0x20)=0x4C017E18;at(0x24)=0x40000015;at(0x28)=0x5511;
        at(0x10)=0x00000002|(cache_lock?(1u<<26):0);at(0x48)=sgfsr;
        for(uint32_t n=0;n<Smrs;++n) {at(0x800+4*n)=0;at(0xC00+4*n)=0x03000018;}
        uint32_t x=seed?seed:1;
        auto next=[&] {x^=x<<13;x^=x>>17;x^=x<<5;return x;};
        for(uint32_t bank=0;bank<Banks;++bank) {
            at(0x1000+4*bank)=next();at(0x1800+4*bank)=next();at(0x1400+4*bank)=next()&0xFFFF;
            for(uint32_t off=0;off<0x80;off+=4)at(cb(bank)+off)=next();
            at(cb(bank)+0x00)|=0x41;             /* M and CFIE set at random */
            at(cb(bank)+0x58)|=0x4000000A;       /* SS, TF and PF pending at random */
            at(cb(bank)+0x10)|=0x60;
        }
        io={this,Bytes,read32,read64,write32,write64,barrier};
    }
    static bool tlbi64(uint32_t inbank) {return (inbank>=0x600 && inbank<0x610) || (inbank>=0x620 && inbank<0x630);}
    int bankOf(uint32_t offset) const {return offset>=cb(0) && offset<cb(Banks)?int((offset-cb(0))/0x1000):-1;}
    static int read32(void *cookie,size_t offset,uint32_t *value)
    {
        A830SmmuModel *m=static_cast<A830SmmuModel *>(cookie);
        MODEL_CHECK(offset%4==0 && offset<Bytes && value);
        ++m->reads;
        const int bank=m->bankOf(uint32_t(offset));
        if(bank>=0 && (offset&0xFFF)==0x7F4) {
            if(m->tlb_stuck)*value=1;
            else if(m->busy_left[bank]) {--m->busy_left[bank];*value=1;}
            else *value=0;
            return 0;
        }
        *value=m->r[offset/4];
        return 0;
    }
    static int read64(void *cookie,size_t offset,uint64_t *value)
    {
        A830SmmuModel *m=static_cast<A830SmmuModel *>(cookie);
        MODEL_CHECK(offset%8==0 && offset<Bytes && value);
        ++m->reads;
        *value=m->at64(uint32_t(offset));
        return 0;
    }
    static int write32(void *cookie,size_t offset,uint32_t value)
    {
        A830SmmuModel *m=static_cast<A830SmmuModel *>(cookie);
        MODEL_CHECK(offset%4==0 && offset<Bytes);
        ++m->writes;
        const uint32_t o=uint32_t(offset);
        if(o>=0x20 && o<0x2C)return 0;                                   /* IDRs are read-only */
        if(o==0x00)return 0;                                              /* sCR0: the hypervisor drops it */
        if(o==0x10) {if(!m->sacr_write_ignored)m->r[o/4]=value;return 0;}
        if(o==0x48) {if(!m->sgfsr_write_ignored)m->r[o/4]&=~value;return 0;}
        if(o>=0xC00 && o<0xC00+4*Smrs) {
            if((((value>>16)&3)==0 && (value&0xFF)==1) || ((value>>8)&3)==0)++m->aborts;   /* synchronous external abort */
            m->s2cr_writes.push_back({(o-0xC00)/4,value});
            m->r[o/4]=value;return 0;
        }
        if(o>=0x1400 && o<0x1400+4*Banks)return 0;                        /* CBFRSYNRA: read-only */
        const int bank=m->bankOf(o);
        if(bank>=0) {
            const uint32_t in=o&0xFFF;
            if(tlbi64(in)) {++m->violations;return 0;}                     /* a split TLBIVA* store invalidates with ASID 0 */
            switch(in) {
            case 0x000:m->r[o/4]=value&~0x1000u;return 0;                 /* ASIDPNE is not kept */
            case 0x004:if(!(m->r[0x10/4]&(1u<<26)))m->r[o/4]=value;return 0;
            case 0x010:m->r[o/4]=value|0x60u;return 0;                     /* TBI0/TBI1 read as 1 */
            case 0x058:m->r[o/4]&=~value;return 0;                         /* write one to clear */
            case 0x610:++m->tlbiasid;m->last_tlbiasid=value;return 0;
            case 0x618:++m->tlbiall;return 0;
            case 0x7F0:++m->tlbsync;m->busy_left[bank]=m->tlb_busy;return 0;
            case 0x7F4:return 0;
            default:m->r[o/4]=value;return 0;
            }
        }
        m->r[o/4]=value;
        return 0;
    }
    static int write64(void *cookie,size_t offset,uint64_t value)
    {
        A830SmmuModel *m=static_cast<A830SmmuModel *>(cookie);
        MODEL_CHECK(offset%8==0 && offset<Bytes);
        ++m->writes;
        const uint32_t o=uint32_t(offset);
        const int bank=m->bankOf(o);
        if(bank>=0 && tlbi64(o&0xFFF)) {if((o&0xFFF)==0x620)m->tlbival.push_back(value);return 0;}
        if(bank>=0 && ((o&0xFFF)==0x20 || (o&0xFFF)==0x28)) {m->set64(o,value);return 0;}
        ++m->violations;                                                   /* any other 64-bit store */
        return 0;
    }
    static int barrier(void *) {return 0;}
    /* A context fault as the hardware records it. */
    void fault(unsigned bank,uint32_t fsr,uint64_t far,uint16_t sid)
    {
        at(cb(bank)+0x58)|=fsr;set64(cb(bank)+0x60,far);at(cb(bank)+0x68)=0x11;at(0x1400+4*bank)=sid;
    }
};

/* The PaganiArmCpu SPI interface, a clock and the SCM call of the platform. */
struct A830SmmuHostPlatform {
    struct Line {uint32_t intid;void (*handler)(void *);void *context;bool enabled;};
    uint64_t clock=0,delayed=0;
    unsigned delays=0,notifies=0,disables=0,enables=0,apertures=0;
    uint32_t readback=0xA003;
    IOReturn attach_status=kIOReturnSuccess;
    int aperture_status=0;int64_t aperture_x0=0;uint32_t aperture_cb=~0u;
    std::vector<Line> lines;
    static A830SmmuHostPlatform *&current() {static A830SmmuHostPlatform *p;return p;}
    static uint64_t now(void *cookie) {return static_cast<A830SmmuHostPlatform *>(cookie)->clock;}
    static void delay(void *cookie,uint32_t us)
    {auto *p=static_cast<A830SmmuHostPlatform *>(cookie);p->clock+=us;p->delayed+=us;++p->delays;}
    static IOReturn attach(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router)
    {
        A830SmmuHostPlatform *p=current();
        MODEL_CHECK(p && !edge && handler && readback && router);
        if(p->attach_status!=kIOReturnSuccess)return p->attach_status;
        for(const Line &l:p->lines)MODEL_CHECK(l.intid!=intid);   /* one attach per SPI: there is no detach */
        p->lines.push_back({intid,handler,context,true});
        *readback=p->readback;*router=0x81000000;
        return kIOReturnSuccess;
    }
    static IOReturn disable(uint32_t intid)
    {
        A830SmmuHostPlatform *p=current();
        for(Line &l:p->lines)if(l.intid==intid) {l.enabled=false;++p->disables;return kIOReturnSuccess;}
        return kIOReturnBadArgument;
    }
    static IOReturn enable(uint32_t intid)
    {
        A830SmmuHostPlatform *p=current();
        for(Line &l:p->lines)if(l.intid==intid) {l.enabled=true;++p->enables;return kIOReturnSuccess;}
        return kIOReturnBadArgument;
    }
    static void notify(void *cookie) {++static_cast<A830SmmuHostPlatform *>(cookie)->notifies;}
    /* The ring's fences a context fault's top half reads (M14 P0): bound when fenced is set, counted per read. */
    bool fenced=false;
    uint32_t completed=0,issued=0;
    unsigned fence_reads=0;
    static void fences(void *cookie,uint32_t *completed,uint32_t *issued)
    {
        auto *p=static_cast<A830SmmuHostPlatform *>(cookie);
        ++p->fence_reads;*completed=p->completed;*issued=p->issued;
    }
    static int aperture(void *cookie,uint32_t cb,int64_t *x0)
    {
        auto *p=static_cast<A830SmmuHostPlatform *>(cookie);
        ++p->apertures;p->aperture_cb=cb;*x0=p->aperture_x0;
        return p->aperture_status;
    }
    /* The GIC delivering a level SPI: the handler runs while the line is enabled (as long as times allows). */
    unsigned fire(uint32_t intid,unsigned times=1)
    {
        unsigned ran=0;
        for(Line &l:lines)if(l.intid==intid)
            for(unsigned i=0;i<times && l.enabled;++i) {l.handler(l.context);++ran;}
        return ran;
    }
    A830SmmuPlatform platform(bool irqs=true)
    {
        current()=this;
        return {this,now,delay,irqs?attach:nullptr,irqs?disable:nullptr,irqs?enable:nullptr,notify,aperture,fenced?fences:nullptr};
    }
};
/* The 13T board values (ARCHITECTURE 3.5): CB0 GPU (slots 0 and 3, SIDs 0 and 1), CB2 GMU (slot 1, SID 5), ACTLR 0x303. */
inline A830SmmuConfig A830SmmuBoardConfig(A830SmmuModel &model,A830SmmuHostPlatform &host,Adreno830Status *status,bool irqs=true)
{
    A830SmmuConfig config{};
    config.io=&model.io;config.phys=0x3DA0000;config.platform=host.platform(irqs);config.actlr=0x303;
    config.cb_gpu=0;config.cb_gmu=2;config.slot_gpu=0;config.slot_lpac=3;config.slot_gmu=1;
    config.sid_gpu=0;config.sid_lpac=1;config.sid_gmu=5;
    config.irq_cb[0]=710;config.irq_cb[1]=711;config.irq_cb[2]=712;config.irq_cb[3]=713;config.irq_global=706;
    config.status=status;
    return config;
}
#endif
