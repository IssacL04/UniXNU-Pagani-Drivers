/* SPDX-License-Identifier: MIT */
/* Authored host doubles for the A830Irq / A830Fence / A830SelfTest tests: a GIC with the PaganiArmCpu SPI exports (strong
 * definitions of armcpu_spi_*, replacing A830Irq.cpp's weak defaults), a GPU register file with the RBBM interrupt semantics
 * (masked status, write-1-to-clear, stuck bits that re-assert), pipe-banked CP registers behind CP_APERTURE_CNTL_HOST, and
 * small GMU / CX_MISC windows. Include it in exactly one translation unit per test program. */
#ifndef A830_FAKE_GPU_H
#define A830_FAKE_GPU_H
#include "A830Irq.h"
#include "A830Regs.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"%s:%d: CHECK failed: %s\n",__FILE__,__LINE__,#x);std::exit(1);}} while(0)

struct FakeAccess {uint32_t dword;bool write;uint32_t value;};

struct FakeGpu {
    std::mutex m;
    uint32_t raw=0,stuck=0,mask=0,aperture=0;
    std::map<uint32_t,uint32_t> regs;
    std::map<uint32_t,uint32_t> pipe[8];
    std::vector<FakeAccess> log;
    bool logging=false;
    uint64_t accesses=0;
    static bool piped(uint32_t dw)
    {
        return dw==A8XX_CP_PROTECT_STATUS_PIPE || dw==A8XX_CP_HW_FAULT_STATUS_PIPE || dw==A8XX_CP_INTERRUPT_STATUS_PIPE ||
               dw==A8XX_CP_SQE_STAT_ADDR_PIPE || dw==A8XX_CP_SQE_STAT_DATA_PIPE || (dw>=A8XX_CP_IB1_BASE && dw<=A8XX_CP_IB2_REM_SIZE);
    }
    uint32_t read(uint32_t dw)
    {
        std::lock_guard<std::mutex> g(m);
        ++accesses;
        uint32_t v;
        if(dw==A8XX_RBBM_INT_0_STATUS)v=(raw|stuck)&mask;
        else if(dw==A8XX_RBBM_INT_0_MASK)v=mask;
        else if(dw==A8XX_CP_APERTURE_CNTL_HOST)v=aperture;
        else if(piped(dw))v=pipe[(aperture>>12)&7][dw];
        else v=regs[dw];
        if(logging)log.push_back({dw,false,v});
        return v;
    }
    void write(uint32_t dw,uint32_t v)
    {
        std::lock_guard<std::mutex> g(m);
        ++accesses;
        if(logging)log.push_back({dw,true,v});
        if(dw==A8XX_RBBM_INT_CLEAR_CMD)raw&=~v;
        else if(dw==A8XX_RBBM_INT_0_MASK)mask=v;
        else if(dw==A8XX_CP_APERTURE_CNTL_HOST)aperture=v;
        else if(piped(dw))pipe[(aperture>>12)&7][dw]=v;
        else regs[dw]=v;
    }
    void raise(uint32_t bits) {std::lock_guard<std::mutex> g(m);raw|=bits;}
    bool asserted() {std::lock_guard<std::mutex> g(m);return ((raw|stuck)&mask)!=0;}
    void startLog() {std::lock_guard<std::mutex> g(m);log.clear();logging=true;}
    std::vector<FakeAccess> stopLog() {std::lock_guard<std::mutex> g(m);logging=false;return log;}
    void set(uint32_t dw,uint32_t v) {std::lock_guard<std::mutex> g(m);regs[dw]=v;}
    uint32_t get(uint32_t dw) {std::lock_guard<std::mutex> g(m);return regs[dw];}
    void setPipe(unsigned p,uint32_t dw,uint32_t v) {std::lock_guard<std::mutex> g(m);pipe[p][dw]=v;}
    uint32_t maskNow() {std::lock_guard<std::mutex> g(m);return mask;}
    uint32_t apertureNow() {std::lock_guard<std::mutex> g(m);return aperture;}
    uint64_t accessCount() {std::lock_guard<std::mutex> g(m);return accesses;}
    A830Mmio mmio()
    {
        return {this,[](void *c,uint32_t off) {return static_cast<FakeGpu *>(c)->read(off/4);},
                [](void *c,uint32_t off,uint32_t v) {static_cast<FakeGpu *>(c)->write(off/4,v);}};
    }
};

/* A plain register window at byte offsets (GMU, CX_MISC); CX_MISC status is write-1-to-clear through INT_CLEAR_CMD. */
struct FakeWindow {
    std::mutex m;
    std::map<uint32_t,uint32_t> regs;
    std::vector<FakeAccess> writes;
    uint32_t clear_offset=~0u,status_offset=~0u;
    uint32_t read(uint32_t off) {std::lock_guard<std::mutex> g(m);return regs[off];}
    void write(uint32_t off,uint32_t v)
    {
        std::lock_guard<std::mutex> g(m);
        writes.push_back({off,true,v});
        if(off==clear_offset)regs[status_offset]&=~v;
        else regs[off]=v;
    }
    void set(uint32_t off,uint32_t v) {std::lock_guard<std::mutex> g(m);regs[off]=v;}
    uint32_t get(uint32_t off) {std::lock_guard<std::mutex> g(m);return regs[off];}
    A830Mmio mmio()
    {
        return {this,[](void *c,uint32_t off) {return static_cast<FakeWindow *>(c)->read(off);},
                [](void *c,uint32_t off,uint32_t v) {static_cast<FakeWindow *>(c)->write(off,v);}};
    }
};

/* The GIC side of PaganiArmCpu: 16 slots, attach enables, disable/enable only for attached INTIDs. */
struct FakeGic {
    struct Line {uint32_t intid=0;bool edge=false;void (*handler)(void *)=nullptr;void *context=nullptr;bool enabled=false;uint64_t count=0;};
    std::mutex m;
    std::vector<Line> lines;
    IOReturn attach_result=kIOReturnSuccess;
    uint32_t forced_readback=0;     /* nonzero: report this readback instead of the configured one */
    uint64_t router=0x0000000081000100ull;
    unsigned attaches=0,disables=0,enables=0;
    std::function<bool(uint32_t)> level;   /* is a level line asserted */
    Line *find(uint32_t intid) {for(Line &l:lines)if(l.intid==intid)return &l;return nullptr;}
    void reset() {std::lock_guard<std::mutex> g(m);lines.clear();attach_result=kIOReturnSuccess;forced_readback=0;attaches=disables=enables=0;level=nullptr;}
    bool enabled(uint32_t intid) {std::lock_guard<std::mutex> g(m);Line *l=find(intid);return l && l->enabled;}
    /* Level delivery: the handler runs while the line is asserted and enabled (bounded); returns the entries taken. */
    unsigned deliver(uint32_t intid,unsigned limit=200000)
    {
        unsigned n=0;
        for(;n<limit;++n) {
            void (*handler)(void *)=nullptr;void *context=nullptr;
            {
                std::lock_guard<std::mutex> g(m);
                Line *l=find(intid);
                if(!l || !l->enabled)break;
                handler=l->handler;context=l->context;
            }
            if(!level || !level(intid))break;
            {std::lock_guard<std::mutex> g(m);find(intid)->count++;}
            handler(context);
        }
        return n;
    }
    /* One entry regardless of the level (a spurious or edge interrupt). */
    bool pulse(uint32_t intid)
    {
        void (*handler)(void *)=nullptr;void *context=nullptr;
        {
            std::lock_guard<std::mutex> g(m);
            Line *l=find(intid);
            if(!l || !l->enabled)return false;
            l->count++;handler=l->handler;context=l->context;
        }
        handler(context);
        return true;
    }
};
inline FakeGic gGic;

extern "C" IOReturn armcpu_spi_attach(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router)
{
    std::lock_guard<std::mutex> g(gGic.m);
    ++gGic.attaches;
    if(gGic.attach_result!=kIOReturnSuccess)return gGic.attach_result;
    if(intid<32 || intid>=1020 || !handler)return kIOReturnBadArgument;
    if(gGic.find(intid))return kA830ExclusiveAccess;
    if(gGic.lines.size()>=16)return kIOReturnNoResources;
    FakeGic::Line l;l.intid=intid;l.edge=edge;l.handler=handler;l.context=context;l.enabled=true;
    gGic.lines.push_back(l);
    if(readback)*readback=gGic.forced_readback?gGic.forced_readback:0xA003u|(edge?4u:0u);
    if(router)*router=gGic.router;
    return kIOReturnSuccess;
}
extern "C" uint64_t armcpu_spi_count(uint32_t intid)
{
    std::lock_guard<std::mutex> g(gGic.m);
    FakeGic::Line *l=gGic.find(intid);
    return l?l->count:0;
}
extern "C" IOReturn armcpu_spi_disable(uint32_t intid)
{
    std::lock_guard<std::mutex> g(gGic.m);
    FakeGic::Line *l=gGic.find(intid);
    if(!l)return kIOReturnBadArgument;
    ++gGic.disables;l->enabled=false;
    return kIOReturnSuccess;
}
extern "C" IOReturn armcpu_spi_enable(uint32_t intid)
{
    std::lock_guard<std::mutex> g(gGic.m);
    FakeGic::Line *l=gGic.find(intid);
    if(!l)return kIOReturnBadArgument;
    ++gGic.enables;l->enabled=true;
    return kIOReturnSuccess;
}
#endif
