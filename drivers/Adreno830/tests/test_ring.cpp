/* SPDX-License-Identifier: MIT */
/* The Gen8 ring over a software CP register peer (ARCHITECTURE 4.9, 4.10, 4.13; PLAN M4, M7, M10, M13 f). No GPU access.
 * host-test: NativeAdrenoRing.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoRing.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace {
struct Peer {
    std::vector<uint32_t> regs;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    std::vector<uint32_t> reads;
    unsigned barriers=0,publish=0,acquire=0,fail_write=0,fail_read=0,fail_barrier=0,fail_publish=0;
    std::vector<std::pair<size_t,size_t>> published;   /* word index, words */
    std::vector<const char *> order;
    unsigned delays=0,delay_us=0,fence_reads=0,fence_clears=0,dropped=0;
    unsigned locks=0,unlocks=0,depth=0;
    Peer():regs(0x10000,0){}
};
Peer peer;
uint64_t clock_us=0;
uint32_t ring[8192];
uint32_t memptrs[1024];
int write32(void *cookie,uint32_t reg,uint32_t value)
{
    CHECK(cookie==&peer && reg<peer.regs.size());
    if(peer.writes.size()+1==peer.fail_write)return 1;
    peer.regs[reg]=value;peer.writes.emplace_back(reg,value);peer.order.push_back("write");
    return 0;
}
int read32(void *cookie,uint32_t reg,uint32_t *value)
{
    CHECK(cookie==&peer && value && reg<peer.regs.size());
    peer.reads.push_back(reg);
    if(peer.reads.size()==peer.fail_read)return 1;
    *value=peer.regs[reg];return 0;
}
int barrier(void *cookie)
{
    CHECK(cookie==&peer);++peer.barriers;peer.order.push_back("barrier");
    return peer.barriers==peer.fail_barrier;
}
void delay(void *cookie,uint32_t us) {CHECK(cookie==&peer);++peer.delays;peer.delay_us+=us;clock_us+=us;}
/* The GMU AHB fence: the next `dropped` WPTR writes are reported dropped; KGSL never clears the status between tries. */
int fenceStatus(void *cookie,int clear,uint32_t *status)
{
    CHECK(cookie==&peer && status);
    if(clear) {++peer.fence_clears;return 0;}
    ++peer.fence_reads;
    *status=peer.dropped?(--peer.dropped,A830_AHB_FENCE_STATUS_WRITEDROPPED0):0;
    return 0;
}
int publish(void *cookie,const volatile uint32_t *words,size_t bytes)
{
    CHECK(cookie==&peer && words && bytes && !(bytes&3));
    ++peer.publish;peer.order.push_back("publish");
    peer.published.emplace_back(size_t(words-ring),bytes/4);
    return peer.publish==peer.fail_publish;
}
int acquire(void *cookie,volatile uint32_t *words,size_t bytes)
{
    CHECK(cookie==&peer && words==memptrs && bytes==16);++peer.acquire;return 0;
}
uint64_t now(void *cookie) {CHECK(cookie==&peer);return clock_us;}
int idle(void *cookie) {CHECK(cookie==&peer);clock_us+=1000;return 0;}
void lock(void *cookie) {CHECK(cookie==&peer && !peer.depth);++peer.locks;++peer.depth;}
void unlock(void *cookie) {CHECK(cookie==&peer && peer.depth==1);++peer.unlocks;--peer.depth;}
NativeAdrenoRingIo io{&peer,write32,read32,barrier,nullptr,nullptr,nullptr};
NativeAdrenoRingIo fencedIo{&peer,write32,read32,barrier,&peer,fenceStatus,delay};
NativeAdrenoRingCache wc{&peer,nullptr,nullptr};          /* write-combined: barriers only */
NativeAdrenoRingCache cached{&peer,publish,acquire};
NativeAdrenoRingClock clk{&peer,now,idle};
NativeAdrenoRingLock ringLock{&peer,lock,unlock};
/* A830Fence stand-in: completes when memptrs+4 reaches the fence within the call, else times out; `lost` reports a lost device. */
struct Waiter {unsigned calls=0;bool lost=false;uint32_t complete_on_call=0,rptr_on_complete=0;} waiter;
IOReturn waitHook(void *cookie,uint32_t fence,uint32_t timeout_us,uint32_t *observed)
{
    CHECK(cookie==&waiter && peer.depth==0);   /* never called with the ring lock held */
    ++waiter.calls;
    if(waiter.lost)return kIOReturnIOError;
    if(waiter.complete_on_call && waiter.calls>=waiter.complete_on_call) {
        memptrs[1]=fence;
        if(waiter.rptr_on_complete)memptrs[0]=waiter.rptr_on_complete;   /* the CP consumed what it completed */
    }
    *observed=memptrs[1];
    if((int32_t)(memptrs[1]-fence)>=0)return kIOReturnSuccess;
    clock_us+=timeout_us;
    return kIOReturnTimeout;
}
NativeAdrenoRingWaiter hook{&waiter,waitHook,nullptr};
/* The queue meter's stamp hook (A830Fence::ringStamp stand-in): every stamped fence, in order, with the ring lock depth. */
std::vector<uint32_t> stamped,stampFlags;
void stampHook(void *cookie,uint32_t fence,uint32_t flags)
{
    CHECK(cookie==&waiter);
    stamped.push_back(fence);stampFlags.push_back(flags);peer.order.push_back("stamp");
    if(peer.locks)CHECK(peer.depth==1);   /* under the ring lock when there is one */
}
NativeAdrenoRingWaiter stampingHook{&waiter,waitHook,stampHook};
void reset()
{
    peer=Peer{};clock_us=0;waiter=Waiter{};stamped.clear();stampFlags.clear();
    for(auto &v:ring)v=0xdeadbeef;
    for(auto &v:memptrs)v=0xa5a5a5a5;
}
int attach(NativeAdrenoRing &r,const NativeAdrenoRingIo &with=io,const NativeAdrenoRingCache &cache=wc)
{return r.attach(&with,&cache,ring,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300000);}
uint32_t op(uint32_t header) {return (header>>28)==7u?(header>>16)&0x7fu:0xffu;}
uint32_t reg4(uint32_t header) {return (header>>28)==4u?(header>>8)&0x7ffffu:0xffffffffu;}
size_t wptrWrites() {size_t n=0;for(auto &w:peer.writes)n+=w.first==A8XX_CP_RB_WPTR;return n;}
/* Walks the packets in ring[from, to): the payload of the last CP_THREAD_CONTROL (0xffffffff: none, 0xfffffffe: not packets). */
uint32_t lastThread(uint32_t from,uint32_t to)
{
    uint32_t thread=0xffffffffu;
    for(uint32_t i=from;i<to;) {
        const uint32_t header=ring[i];
        if((header>>28)==7u) {
            if(((header>>16)&0x7fu)==0x17u)thread=ring[i+1];
            i+=1+(header&0x3fffu);
        } else if((header>>28)==4u)i+=1+(header&0x7fu);
        else return 0xfffffffeu;
        if(i>to)return 0xfffffffeu;
    }
    return thread;
}
/* The index of the first packet in ring[from, to) that is TYPE4 to reg (wait: CP_WAIT_REG_MEM polling reg), or to. */
uint32_t findPacket(uint32_t from,uint32_t to,uint32_t reg,bool wait)
{
    for(uint32_t i=from;i<to;) {
        const uint32_t header=ring[i];
        if((header>>28)==7u) {
            if(wait && ((header>>16)&0x7fu)==0x3cu && ring[i+2]==reg)return i;
            i+=1+(header&0x3fffu);
        } else if((header>>28)==4u) {
            if(!wait && reg4(header)==reg)return i;
            i+=1+(header&0x7fu);
        } else return to;
    }
    return to;
}
/* The CP consumed everything up to the published WPTR and wrote its fences. */
void consume(uint32_t fence) {memptrs[0]=peer.regs[A8XX_CP_RB_WPTR];memptrs[1]=fence;memptrs[3]=fence;}
}

int main()
{
    unsigned cases=0;
    /* attach binds and clears memory and writes no register (A830Cp::startRing programs the ring). */
    {
        reset();NativeAdrenoRing r;
        CHECK(attach(r)==kIOReturnSuccess && r.attached() && !r.poisoned());
        CHECK(peer.writes.empty() && peer.reads.empty() && peer.barriers==0);
        for(uint32_t v:ring)CHECK(v==0);
        for(uint32_t v:memptrs)CHECK(v==0);
        CHECK(r.writeIndex()==0 && r.lastSubmitted()==0 && r.ringIova()==0x200000 && r.memptrsIova()==0x300000 &&
              r.ringBytes()==sizeof(ring) && r.memptrs()==memptrs);
        const NativeAdrenoRingRegs &g=r.regs();
        CHECK(g.base==0x800 && g.cntl==0x802 && g.rptr_addr==0x804 && g.bv_rptr_addr==0x808);   /* 0x808: BV RPTR address */
        CHECK(g.wptr==0x807 && g.rptr==0x806 && g.bv_rptr==0x80a && g.scratch==0x82c && g.sram_init==0x449 &&
              g.slice_sram_init==0x5e8 && g.sram_init_status==0x19f);
        CHECK(attach(r)==kIOReturnBusy);
        ++cases;
    }
    /* Kernel submission: words, then CP_SCRATCH_GLOBAL[2] and the CACHE_CLEAN|IRQ timestamp to memptrs+4; one barrier, then
     * the WPTR write; no cache maintenance on write-combined memory. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        const uint32_t words[3]={0x11111111,0x22222222,0x33333333};
        uint32_t fence=0;
        CHECK(r.submit(words,3,&fence)==kIOReturnSuccess && fence==1 && r.outstanding()==1 && r.writeIndex()==10);
        CHECK(ring[0]==words[0] && ring[1]==words[1] && ring[2]==words[2]);
        CHECK(reg4(ring[3])==0x82c && (ring[3]&0xffu)==1 && ring[4]==1);
        CHECK(op(ring[5])==0x46 && (ring[5]&0x3fffu)==4 && ring[6]==0x88000031u && ring[7]==0x300004 && ring[8]==0 && ring[9]==1);
        CHECK(peer.writes.size()==1 && peer.writes[0].first==0x807 && peer.writes[0].second==10);
        CHECK(peer.order.size()==2 && !std::strcmp(peer.order[0],"barrier") && !std::strcmp(peer.order[1],"write"));
        CHECK(peer.reads.empty());   /* the space check reads the RPTR shadow, not a register */
        for(auto &w:peer.writes)CHECK(w.first!=0x808);
        ++cases;
    }
    /* Raw kernel packets (CP_ME_INIT, CP_SET_SECURE_MODE): no fence, the write index is the completion point. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        uint32_t init[10];for(uint32_t i=0;i<10;++i)init[i]=0x70000000u+i;
        uint32_t wptr=0;
        CHECK(r.submitRaw(init,10,&wptr)==kIOReturnSuccess && wptr==10 && r.lastSubmitted()==0 && r.outstanding()==0);
        CHECK(peer.regs[0x807]==10 && ring[9]==0x70000009u && ring[10]==0);
        const uint32_t secure[2]={0x70660001u,0};
        CHECK(r.submitRaw(secure,2,&wptr)==kIOReturnSuccess && wptr==12 && peer.regs[0x807]==12);
        CHECK(r.submitRaw(nullptr,2,&wptr)==kIOReturnBadArgument && r.submitRaw(secure,2,nullptr)==kIOReturnBadArgument);
        ++cases;
    }
    /* Framed submissions: the first one enters a fresh context (Gen8 a6xx_set_pagetable entry with both SRAM inits and the
     * 0x19F wait), later ones of the same client do not; a client change does. The thread before the user words is BOTH in
     * every prologue (Linux a7xx_submit; a BR-only thread stalls Turnip's first Gen8 submission), and the framing does not
     * depend on the bring-up profile (A830Profile.h): both profiles emit the same ring words. */
    static uint32_t byProfile[A830ProfileCount][8192];
    for(uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        adreno830_profile_value=profile;
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        const uint32_t words[2]={0xaaaa0001,0xaaaa0002};
        uint32_t fence=0;
        r.useContext(7);
        CHECK(r.submitFramed(words,2,&fence)==kIOReturnSuccess && fence==1);
        const uint32_t first=11+28+2+31;
        CHECK(peer.regs[0x807]==first);
        /* structure of the context entry: BOTH before the user words, both SRAM init writes, then the 0x19F bit 0 wait */
        CHECK(lastThread(0,39)==A8XX_CP_THREAD_BOTH);
        const uint32_t sram=findPacket(0,39,0x449,false),slice=findPacket(0,39,0x5e8,false),wait=findPacket(0,39,0x19f,true);
        CHECK(sram<slice && slice<wait && wait<39 && ring[sram+1]==1 && ring[slice+1]==1 && ring[wait+4]==1 && ring[wait+5]==1);
        CHECK(op(ring[0])==0x17 && ring[1]==0x80000001u);
        CHECK(op(ring[2])==0x14 && ring[3]==0 && ring[4]==0x300004 && ring[5]==0 && ring[6]==0);   /* wait for fence 0 */
        CHECK(op(ring[7])==0x17 && ring[8]==0x80000003u && op(ring[9])==0x1f && ring[10]==0xf);
        CHECK(op(ring[11])==0x17 && ring[12]==0x80000003u && op(ring[13])==0x46 && ring[14]==0x26);
        CHECK(op(ring[15])==0x17 && ring[16]==0x80000001u);
        CHECK(reg4(ring[17])==0x449 && ring[18]==1 && reg4(ring[19])==0x5e8 && ring[20]==1);
        CHECK(op(ring[21])==0x46 && ring[22]==0x31);
        CHECK(op(ring[23])==0x3c && ring[24]==3 && ring[25]==0x19f && ring[26]==0 && ring[27]==1 && ring[28]==1 && ring[29]==0);
        CHECK(op(ring[30])==0x46 && ring[31]==0x32 && op(ring[32])==0x46 && ring[33]==0x33 && op(ring[34])==0x26 &&
              (ring[34]&0x3fffu)==0);
        CHECK(op(ring[35])==0x17 && ring[36]==3 && op(ring[37])==0x65 && ring[38]==0x101);
        CHECK(ring[39]==words[0] && ring[40]==words[1]);
        const uint32_t f=41;   /* the framed fence */
        CHECK(reg4(ring[f])==0x82c && ring[f+1]==1);
        CHECK(op(ring[f+2])==0x17 && ring[f+3]==1 && op(ring[f+4])==0x46 && ring[f+5]==0x18 && op(ring[f+6])==0x46 && ring[f+7]==0x19);
        CHECK(op(ring[f+8])==0x17 && ring[f+9]==2);
        CHECK(op(ring[f+10])==0x46 && ring[f+11]==0x08000031u && ring[f+12]==0x30000c && ring[f+13]==0 && ring[f+14]==1);
        CHECK(op(ring[f+15])==0x17 && ring[f+16]==1);
        CHECK(op(ring[f+17])==0x14 && ring[f+18]==0 && ring[f+19]==0x30000c && ring[f+20]==0 && ring[f+21]==1);
        CHECK(op(ring[f+22])==0x46 && ring[f+23]==0x88000031u && ring[f+24]==0x300004 && ring[f+25]==0 && ring[f+26]==1);
        CHECK(op(ring[f+27])==0x17 && ring[f+28]==3 && op(ring[f+29])==0x65 && ring[f+30]==0x100);
        consume(1);
        r.useContext(7);
        CHECK(r.submitFramed(words,2,&fence)==kIOReturnSuccess && fence==2 && peer.regs[0x807]==first+11+2+31);
        CHECK(op(ring[first])==0x17 && op(ring[first+2])==0x46 && ring[first+3]==0x32);   /* no context entry */
        CHECK(lastThread(first,first+11)==A8XX_CP_THREAD_BOTH && ring[first+11]==words[0]);
        consume(2);
        r.useContext(8);
        CHECK(r.submitFramed(words,2,&fence)==kIOReturnSuccess && peer.regs[0x807]==first+(11+2+31)+first);
        const uint32_t third=first+11+2+31;
        CHECK(op(ring[third+2])==0x14 && ring[third+6]==2);   /* the new context waits for the last fence */
        CHECK(lastThread(third,third+39)==A8XX_CP_THREAD_BOTH && ring[third+39]==words[0]);
        r.requestContextReset();
        consume(3);
        CHECK(r.submitFramed(words,2,&fence)==kIOReturnSuccess && op(ring[third+first+2])==0x14);
        std::memcpy(byProfile[profile],ring,sizeof(ring));
    }
    CHECK(!std::memcmp(byProfile[A830ProfileDesign],byProfile[A830ProfileAnnibale],sizeof(ring)));
    adreno830_profile_value=A830ProfileAnnibale;
    ++cases;
    /* The fenced WPTR write (KGSL gen8_fenced_write): a dropped write is given again after 10 us, the status never cleared;
     * more than 100 tries is recorded; the 200th failure is an error that poisons the ring. Every WPTR write is fenced. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r,fencedIo)==kIOReturnSuccess);
        const uint32_t one[1]={0xabc};uint32_t fence=0;
        peer.dropped=3;
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess);
        CHECK(wptrWrites()==4 && peer.fence_reads==4 && peer.fence_clears==0 && peer.delays==3 && peer.delay_us==30);
        CHECK(r.wptrStats().writes==1 && r.wptrStats().dropped==1 && r.wptrStats().retries==3 && r.wptrStats().slow==0);
        peer.dropped=150;
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && r.wptrStats().slow==1 && r.wptrStats().retries==153);
        uint32_t wptr=0;peer.dropped=0;
        CHECK(r.submitRaw(one,1,&wptr)==kIOReturnSuccess && r.kick()==kIOReturnSuccess && r.wptrStats().writes==4);
        peer.dropped=1000;
        const size_t before=wptrWrites();
        CHECK(r.submit(one,1,&fence)==kIOReturnTimeout && r.poisoned() && fence==0);
        CHECK(wptrWrites()-before==200 && r.wptrStats().failures==1);
        CHECK(r.submit(one,1,&fence)==kIOReturnNotReady && r.kick()==kIOReturnNotReady);
        ++cases;
    }
    /* Ring space: a stream that can never fit is NoResources; one that does not fit now is Busy, then fits once the CP's
     * RPTR shadow moves (waitForSpace sleeps through the waiter on the next completion, or times out on the clock). */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        static uint32_t big[8192];
        for(auto &w:big)w=0x5a5a5a5a;
        uint32_t fence=0;
        CHECK(r.submit(big,8192-8-7+1,&fence)==kIOReturnNoResources);
        CHECK(r.submitFramed(big,8192-8-70+1,&fence)==kIOReturnNoResources);
        CHECK(r.waitForSpace(8192,false,&clk,1000)==kIOReturnNoResources);
        CHECK(r.submit(big,6000,&fence)==kIOReturnSuccess && fence==1);
        CHECK(r.submit(big,3000,&fence)==kIOReturnBusy && fence==0 && r.lastSubmitted()==1);
        CHECK(r.waitForSpace(3000,false,&clk,5000)==kIOReturnTimeout);
        r.setWaiter(&hook);waiter.complete_on_call=1;waiter.rptr_on_complete=6007;
        CHECK(r.waitForSpace(3000,false,&clk,5000)==kIOReturnSuccess && waiter.calls==1 && r.completedFence()==1);
        CHECK(r.submit(big,3000,&fence)==kIOReturnSuccess && fence==2 && r.writeIndex()==(6007+3007)%8192);
        /* Wrap-around: the second submission continues at index 0. */
        CHECK(ring[8191]==0x5a5a5a5a && ring[0]==0x5a5a5a5a);
        ++cases;
    }
    /* Cache-maintaining clients clean exactly the written words, split at the wrap; nothing else. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r,io,cached)==kIOReturnSuccess);
        static uint32_t words[8000];
        uint32_t fence=0;
        CHECK(r.submit(words,8000,&fence)==kIOReturnSuccess);
        CHECK(peer.published.size()==1 && peer.published[0].first==0 && peer.published[0].second==8007);
        CHECK(peer.acquire==1);   /* the RPTR shadow read */
        memptrs[0]=8007;memptrs[1]=1;
        CHECK(r.submit(words,200,&fence)==kIOReturnSuccess);
        CHECK(peer.published.size()==3 && peer.published[1].first==8007 && peer.published[1].second==185 &&
              peer.published[2].first==0 && peer.published[2].second==22);
        CHECK(!std::strcmp(peer.order[peer.order.size()-3],"publish") && !std::strcmp(peer.order.back(),"write"));
        ++cases;
    }
    /* Waits: polling without a waiter (retires), observe without retiring, the A830Fence hook (Timeout, Complete, Lost). */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        const uint32_t one[1]={1};uint32_t fence=0,observed=0;
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && r.submit(one,1,&fence)==kIOReturnSuccess && fence==2);
        CHECK(r.wait(1,&clk,5000,&observed)==NativeAdrenoRingFenceTimeout && observed==0 && clock_us>=5000);
        CHECK(r.wait(3,&clk,5000,&observed)==NativeAdrenoRingFenceArgument && r.wait(0,&clk,5000,&observed)==NativeAdrenoRingFenceArgument);
        memptrs[1]=1;
        CHECK(r.observeFence(1,nullptr,0,&observed)==NativeAdrenoRingFenceComplete && observed==1 && r.completedFence()==0);
        CHECK(r.wait(1,&clk,5000,&observed)==NativeAdrenoRingFenceComplete && r.completedFence()==1 && r.outstanding()==1);
        r.setWaiter(&hook);r.setLock(&ringLock);
        CHECK(r.wait(2,&clk,1000,&observed)==NativeAdrenoRingFenceTimeout && waiter.calls==1 && peer.locks==0);
        waiter.complete_on_call=2;
        CHECK(r.wait(2,&clk,1000,&observed)==NativeAdrenoRingFenceComplete && observed==2 && r.completedFence()==2 &&
              r.outstanding()==0);
        waiter.lost=true;
        CHECK(r.wait(2,&clk,1000,&observed)==NativeAdrenoRingFenceLost && !r.poisoned());
        CHECK(r.observeFence(2,&clk,1000,&observed)==NativeAdrenoRingFenceLost);
        r.setWaiter(nullptr);
        memptrs[1]=9;   /* a fence the ring never issued: the memory or the CP is lost */
        CHECK(r.wait(2,&clk,1000,&observed)==NativeAdrenoRingFencePoisoned && r.poisoned());
        CHECK(r.detach()==kIOReturnBusy);
        ++cases;
    }
    /* The ring lock brackets every submission, kick and recover exactly once; waits never take it. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);r.setLock(&ringLock);
        const uint32_t one[1]={1};uint32_t fence=0;
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && peer.locks==1 && peer.unlocks==1);
        CHECK(r.submitFramed(one,1,&fence)==kIOReturnSuccess && r.submitRaw(one,1,&fence)==kIOReturnSuccess);
        CHECK(r.kick()==kIOReturnSuccess && peer.locks==4 && peer.unlocks==4 && peer.depth==0);
        uint32_t observed=0;memptrs[1]=2;
        CHECK(r.wait(2,&clk,0,&observed)==NativeAdrenoRingFenceComplete && peer.locks==4);
        uint32_t last=0;
        CHECK(r.recover(&last)==kIOReturnSuccess && last==2 && peer.locks==5 && peer.unlocks==5);
        CHECK(r.detach()==kIOReturnSuccess && peer.locks==6 && peer.unlocks==6);
        ++cases;
    }
    /* Recovery (ARCHITECTURE 4.13.4): ring and pointers back to 0, memptrs fences = the last issued (waiters wake), the
     * poison lifted, numbering continues, the next framed submission enters a fresh context. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r,fencedIo)==kIOReturnSuccess);
        const uint32_t one[1]={0x1234};uint32_t fence=0;
        for(unsigned i=0;i<3;++i)CHECK(r.submit(one,1,&fence)==kIOReturnSuccess);
        memptrs[1]=1;memptrs[0]=8;
        peer.dropped=1000;
        CHECK(r.submit(one,1,&fence)==kIOReturnTimeout && r.poisoned());   /* the WPTR write failed: recovery */
        peer.dropped=0;
        uint32_t last=0;
        CHECK(r.recover(&last)==kIOReturnSuccess && last==3 && !r.poisoned() && r.writeIndex()==0);
        CHECK(memptrs[0]==0 && memptrs[2]==0 && memptrs[1]==3 && memptrs[3]==3 && r.completedFence()==3 && r.outstanding()==0);
        for(uint32_t v:ring)CHECK(v==0);
        uint32_t observed=0;
        CHECK(r.wait(3,&clk,0,&observed)==NativeAdrenoRingFenceComplete);
        CHECK(r.submitFramed(one,1,&fence)==kIOReturnSuccess && fence==4 && op(ring[2])==0x14 && ring[6]==3);
        ++cases;
    }
    /* Stall state: the 22 Gen8 global registers in order, no aperture write; ibBefore finds the indirect buffer. */
    {
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);
        const uint32_t expect[NativeAdrenoRingStallWords]={0x12,0x13,0x15,0x16,0x6a,0x8b5,0x822,0x840,0x806,0x807,0x1a,0x1c,
            0x1e,0x841,0x62,0x815,0x80a,0x82c,0x8e7,0x8e8,0x18,0x71};
        for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)peer.regs[expect[i]]=0x1000+i;
        uint32_t out[NativeAdrenoRingStallWords]={};
        CHECK(r.stallState(out)==kIOReturnSuccess && peer.writes.empty());
        for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)CHECK(peer.reads[i]==expect[i] && out[i]==0x1000+i);
        CHECK(r.stallState(nullptr)==kIOReturnBadArgument);
        const uint32_t ib[4]={0x703f0003u,0x12345000u,0x9u,0x40u};   /* CP_INDIRECT_BUFFER (ibBefore ignores parity) */
        uint32_t fence=0;
        CHECK(r.submit(ib,4,&fence)==kIOReturnSuccess);
        uint64_t iova=0;uint32_t dwords=0;
        CHECK(r.ibBefore(4,&iova,&dwords)==kIOReturnSuccess && iova==0x912345000ull && dwords==0x40);
        CHECK(r.ibBefore(5,&iova,&dwords)==kIOReturnUnsupported);
        peer.fail_read=peer.reads.size()+1;
        CHECK(r.stallState(out)==kIOReturnIOError && r.poisoned());
        ++cases;
    }
    /* Refusals touch neither registers nor memory; access failures after the commit point poison and keep everything. */
    {
        reset();NativeAdrenoRing r;
        CHECK(r.attach(nullptr,&wc,ring,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,nullptr,ring,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,nullptr,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,ring,4096,0x200000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,ring,16384+4096,0x200000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,ring,sizeof(ring),0x204000,memptrs,sizeof(memptrs),0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,ring,sizeof(ring),0x200000,memptrs,32,0x300000)==kIOReturnBadArgument);
        CHECK(r.attach(&io,&wc,ring,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300004)==kIOReturnBadArgument);
        NativeAdrenoRingRegs bad=NativeAdrenoRingGen8Regs;bad.wptr=0;
        CHECK(r.attach(&io,&wc,ring,sizeof(ring),0x200000,memptrs,sizeof(memptrs),0x300000,&bad)==kIOReturnBadArgument);
        CHECK(peer.writes.empty() && peer.reads.empty() && ring[0]==0xdeadbeef && memptrs[0]==0xa5a5a5a5);
        uint32_t fence=0;
        CHECK(r.submit(nullptr,1,nullptr)==kIOReturnBadArgument && r.submit(ring,1,&fence)==kIOReturnNotReady);
        CHECK(r.detach()==kIOReturnNotReady && r.recover(nullptr)==kIOReturnNotReady);
        for(unsigned kind=0;kind<3;++kind) {
            reset();NativeAdrenoRing p;CHECK(attach(p,io,cached)==kIOReturnSuccess);
            if(kind==0)peer.fail_publish=1;
            if(kind==1)peer.fail_barrier=1;
            if(kind==2)peer.fail_write=1;
            const uint32_t one=0x1234;
            CHECK(p.submit(&one,1,&fence)==kIOReturnIOError && !fence && p.poisoned() && p.outstanding()==0);
            CHECK(p.detach()==kIOReturnBusy && memptrs[1]==0);
            ++cases;
        }
        ++cases;
    }
    /* The queue meter's stamp (ARCHITECTURE 4.13.5), counted on its own line: every kernel and framed submission is stamped
     * with its fence once, after its words are published and before the barrier and the WPTR write; raw packets, refused
     * submissions (Busy, NoResources) and a ring without the hook are not; a failed WPTR write leaves the stamp for the
     * same seqno to be stamped again by the next submission after the recovery. */
    unsigned stampCases=0;
    {
        reset();NativeAdrenoRing r;CHECK(attach(r,io,cached)==kIOReturnSuccess);r.setWaiter(&stampingHook);r.setLock(&ringLock);
        const uint32_t one[1]={0x1234};uint32_t fence=0,wptr=0;
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && fence==1 && stamped==std::vector<uint32_t>{1});
        const size_t n=peer.order.size();
        CHECK(n>=4 && !std::strcmp(peer.order[n-4],"publish") && !std::strcmp(peer.order[n-3],"stamp") &&
              !std::strcmp(peer.order[n-2],"barrier") && !std::strcmp(peer.order[n-1],"write"));
        CHECK(r.submitFramed(one,1,&fence)==kIOReturnSuccess && fence==2 && stamped==(std::vector<uint32_t>{1,2}));
        /* the kernel's own work; a framed submission external by default, the first one entering its context */
        CHECK(stampFlags==(std::vector<uint32_t>{NativeAdrenoRingSourceKernel,
                                                 NativeAdrenoRingSourceExternal|NativeAdrenoRingStampContext}));
        CHECK(r.submitRaw(one,1,&wptr)==kIOReturnSuccess && stamped.size()==2);   /* no fence, no stamp */
        static uint32_t big[8192];
        CHECK(r.submit(big,8192,&fence)==kIOReturnNoResources && stamped.size()==2);
        CHECK(r.submit(big,8100,&fence)==kIOReturnBusy && stamped.size()==2 && r.lastSubmitted()==2);
        r.setWaiter(&hook);   /* a waiter without the stamp: nothing stamped, the submission as before */
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && fence==3 && stamped.size()==2);
        ++stampCases;
    }
    {
        reset();NativeAdrenoRing r;CHECK(attach(r,fencedIo)==kIOReturnSuccess);r.setWaiter(&stampingHook);
        const uint32_t one[1]={0x1234};uint32_t fence=0;
        peer.dropped=1000;
        CHECK(r.submit(one,1,&fence)==kIOReturnTimeout && r.poisoned() && stamped==std::vector<uint32_t>{1});
        peer.dropped=0;
        uint32_t last=0;
        CHECK(r.recover(&last)==kIOReturnSuccess && last==0);
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && fence==1 && stamped==(std::vector<uint32_t>{1,1}));
        ++stampCases;
    }
    /* Whose work each stamp names (the queue meter's job attribution, preemption phase 0): the kernel's for submit(), the source a
     * framed submission was given, and NativeAdrenoRingStampContext on the context entry (the first framed submission after attach
     * or a recovery, and every client change). No ring word depends on the source. */
    {
        constexpr uint32_t R=NativeAdrenoRingSourceRender,E=NativeAdrenoRingSourceExternal,K=NativeAdrenoRingSourceKernel,
            C=NativeAdrenoRingStampContext;
        const uint32_t one[1]={0x1234};uint32_t fence=0,last=0;
        std::vector<uint32_t> words[2];
        for(unsigned i=0;i<2;++i) {
            reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);r.setWaiter(&stampingHook);
            r.useContext(7);
            CHECK(r.submitFramed(one,1,&fence,i?E:R)==kIOReturnSuccess && fence==1 && stampFlags==std::vector<uint32_t>{(i?E:R)|C});
            words[i].assign(ring,ring+r.writeIndex());
        }
        CHECK(words[0]==words[1] && !words[0].empty());
        reset();NativeAdrenoRing r;CHECK(attach(r)==kIOReturnSuccess);r.setWaiter(&stampingHook);
        r.useContext(7);
        CHECK(r.submitFramed(one,1,&fence,R)==kIOReturnSuccess && fence==1);
        CHECK(r.submitFramed(one,1,&fence,R)==kIOReturnSuccess && fence==2);   /* the same client: no entry */
        r.useContext(8);
        CHECK(r.submitFramed(one,1,&fence,E)==kIOReturnSuccess && fence==3);   /* a client switch */
        CHECK(r.submit(one,1,&fence)==kIOReturnSuccess && fence==4);           /* the kernel's: never an entry */
        CHECK(r.submitFramed(one,1,&fence,E)==kIOReturnSuccess && fence==5);
        CHECK(r.submitFramed(one,1,&fence,7)==kIOReturnSuccess && fence==6);   /* an unknown source stays in its two bits */
        CHECK(stampFlags==(std::vector<uint32_t>{R|C,R,E|C,K,E,3}));
        CHECK(r.recover(&last)==kIOReturnSuccess && last==6);
        CHECK(r.submitFramed(one,1,&fence,E)==kIOReturnSuccess && fence==7 && stampFlags.back()==(E|C));   /* after a recovery */
        ++stampCases;
    }
    std::printf("PASS: ring stamps for the queue meter: %u cases (kernel and framed submissions stamped once with their fence "
                "between publication and the barrier, under the ring lock; raw packets, refusals and hook-less waiters never; "
                "a dropped WPTR write re-stamped after the recovery; whose work it is (kernel, render server, external) and the "
                "context entry on the stamp, the ring words the same whatever the source)\n",stampCases);
    std::printf("PASS: Gen8 ring: register-free attach, kernel/raw/framed packets with both SRAM inits, fenced WPTR 10 us x 200 "
                "with poison on failure, Busy vs NoResources, exact publication, A830Fence waiter, ring lock, recovery "
                "(%u cases)\n",cases);
}
