/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoRing.h"
#include "A830Regs.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* Gen8 register numbers are A830Regs.h's (Mesa A8XX names). Packet opcodes and events are adreno_pm4.xml's: CP_EVENT_WRITE7
 * (0x46; event 0x31 CACHE_CLEAN, WRITE_ENABLED bit 27, IRQ bit 31), CP_THREAD_CONTROL (0x17; BR 1, BV 2, BOTH 3, SYNC_THREADS
 * bit 31), CP_SET_MARKER (0x65; 0x101 IFPC disable, 0x100 enable), CP_WAIT_TIMESTAMP (0x14), CP_RESET_CONTEXT_STATE (0x1f),
 * CP_WAIT_REG_MEM (0x3c, WRITE_EQ 3), CP_WAIT_FOR_IDLE (0x26), CCU_INVALIDATE_DEPTH 0x18 / COLOR 0x19, LRZ flush 0x26,
 * CACHE_FLUSH7 0x32 and CACHE_INVALIDATE7 0x33. The framing follows Linux a7xx_submit() and the A8XX branch of
 * a6xx_set_pagetable() (ARCHITECTURE 4.10). */
const NativeAdrenoRingRegs NativeAdrenoRingGen8Regs={
    A8XX_CP_RB_BASE,A8XX_CP_RB_CNTL,A8XX_CP_RB_RPTR_ADDR,A8XX_CP_RB_RPTR_ADDR_BV,
    A8XX_CP_RB_RPTR,A8XX_CP_RB_WPTR,A8XX_CP_RB_RPTR_BV,A8XX_CP_SCRATCH_GLOBAL(2),
    A8XX_RBBM_PERFCTR_SRAM_INIT_CMD,A8XX_RBBM_SLICE_PERFCTR_SRAM_INIT_CMD,A8XX_RBBM_PERFCTR_SRAM_INIT_STATUS,
    {A8XX_RBBM_STATUS,A8XX_RBBM_STATUS1,A8XX_RBBM_GFX_STATUS,A8XX_RBBM_GFX_STATUS1,A8XX_RBBM_INT_0_STATUS,
     A8XX_CP_INTERRUPT_STATUS_GLOBAL,A8XX_CP_CP2GMU_STATUS,A8XX_CP_RL_ERROR_DETAILS_0,A8XX_CP_RB_RPTR,A8XX_CP_RB_WPTR,
     A8XX_RBBM_GFX_BR_STATUS,A8XX_RBBM_GFX_BV_STATUS,A8XX_RBBM_MISC_STATUS,
     A8XX_CP_RL_ERROR_DETAILS_1,A8XX_RBBM_INT_0_MASK,A8XX_CP_SQE_CNTL,A8XX_CP_RB_RPTR_BV,A8XX_CP_SCRATCH_GLOBAL(2),
     A8XX_CP_ALWAYS_ON_COUNTER,A8XX_CP_ALWAYS_ON_COUNTER_HI,A8XX_RBBM_LPAC_STATUS,A8XX_RBBM_SW_FUSE_INT_STATUS}};

namespace {
enum : uint32_t {
    RingFenceOpcode = A8XX_PM4_CP_EVENT_WRITE7,
    RingFenceEvent = A8XX_EVENT_CACHE_CLEAN,
    RingFenceEnable = A8XX_CP_EVENT_WRITE7_WRITE_ENABLED,
    RingFenceIrq = A8XX_CP_EVENT_WRITE7_IRQ,
    RingBlockBytes = A830_RING_BLOCK_BYTES,
    RingThreadControl = A8XX_PM4_CP_THREAD_CONTROL,
    RingThreadBr = A8XX_CP_THREAD_BR,
    RingThreadBv = A8XX_CP_THREAD_BV,
    RingThreadBoth = A8XX_CP_THREAD_BOTH,
    RingSyncThreads = A8XX_CP_THREAD_SYNC,
    RingSetMarker = A8XX_PM4_CP_SET_MARKER,
    RingWaitTimestamp = A8XX_PM4_CP_WAIT_TIMESTAMP,
    RingCcuInvalidateDepth = A8XX_EVENT_CCU_INVALIDATE_DEPTH,
    RingCcuInvalidateColor = A8XX_EVENT_CCU_INVALIDATE_COLOR,
    /* Every prologue's BR-only part ends with the GPU cache flush and invalidate the SQE's CP_SMMU_TABLE_UPDATE handler would
     * do (this ring never sends that packet): CACHE_FLUSH7, CACHE_INVALIDATE7 and a wait for idle. */
    RingCacheFlush7 = A8XX_EVENT_CACHE_FLUSH7,
    RingCacheInvalidate7 = A8XX_EVENT_CACHE_INVALIDATE7,
    RingWaitForIdle = A8XX_PM4_CP_WAIT_FOR_IDLE,
    RingResetContextState = A8XX_PM4_CP_RESET_CONTEXT_STATE,
    RingResetAll = 0xf,              /* CLEAR_ON_CHIP_TS | CLEAR_RESOURCE_TABLE | CLEAR_BV_BR_COUNTER | RESET_GLOBAL_LOCAL_TS */
    RingLrzFlushInvalidate = A8XX_EVENT_LRZ_CACHE_FLUSH,
    RingCacheInvalidate = 0x31,      /* the event a6xx_set_pagetable() emits after the SRAM init */
    RingWaitRegMem = A8XX_PM4_CP_WAIT_REG_MEM,
    RingWaitWriteEq = 3,
    RingFenceKernel = 0,
    RingFenceFramed = 1,
    RingFenceNone = 2
};
/* Packs one TYPE7 packet into words; returns the next free index or 0 when the header cannot be built. */
size_t packet(uint32_t *words,size_t at,uint32_t opcode,const uint32_t *payload,uint32_t count)
{
    if(AdrenoPM4Type7(opcode,count,&words[at])!=AdrenoPM4Success)return 0;
    for(uint32_t i=0;i<count;++i)words[at+1+i]=payload[i];
    return at+1+count;
}
size_t type4(uint32_t *words,size_t at,uint32_t reg,uint32_t value)
{
    if(!at || AdrenoPM4Type4(reg,1,&words[at])!=AdrenoPM4Success)return 0;
    words[at+1]=value;return at+2;
}
}

IOReturn NativeAdrenoRing::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
void NativeAdrenoRing::enter() const {if(lock_.lock)lock_.lock(lock_.cookie);}
void NativeAdrenoRing::leave() const {if(lock_.unlock)lock_.unlock(lock_.cookie);}
void NativeAdrenoRing::setWaiter(const NativeAdrenoRingWaiter *waiter)
{
    waiter_=waiter && waiter->wait?*waiter:NativeAdrenoRingWaiter{};
}
void NativeAdrenoRing::setLock(const NativeAdrenoRingLock *lock)
{
    lock_=lock && lock->lock && lock->unlock?*lock:NativeAdrenoRingLock{};
}
/* KGSL gen8_fenced_write (ARCHITECTURE 4.13.2): CP_RB_WPTR is behind the GMU AHB fence; a write that arrives while the GX
 * domain is not ready is dropped and flagged in WRITEDROPPED0. The write is then given again after 10 us, at most 200 times;
 * more than 100 tries is recorded, the 200th failure is an error and poisons the ring (the device goes to recovery). The
 * status is not cleared between tries, as in KGSL and Linux. */
IOReturn NativeAdrenoRing::writeWptr(uint32_t value)
{
    ++wptr_.writes;
    if(io_.write32(io_.cookie,regs_.wptr,value))return poison();
    if(!io_.fence_status)return kIOReturnSuccess;
    for(uint32_t tries=0;;++tries) {
        uint32_t status=0;
        if(io_.barrier(io_.cookie) || io_.fence_status(io_.fence_cookie,0,&status))return poison();
        if(!(status&A830_AHB_FENCE_STATUS_WRITEDROPPED0)) {
            if(tries)++wptr_.dropped;
            if(tries>NativeAdrenoRingWptrSlowAfter)++wptr_.slow;
            return kIOReturnSuccess;
        }
        if(tries+1>=NativeAdrenoRingWptrRetries) {
            ++wptr_.dropped;++wptr_.failures;
            poisoned_=true;
            return kIOReturnTimeout;
        }
        if(io_.delay_us)io_.delay_us(io_.cookie,NativeAdrenoRingWptrDelayUs);
        ++wptr_.retries;
        if(io_.write32(io_.cookie,regs_.wptr,value))return poison();
    }
}
/* The BR read pointer as the CP last wrote it to memptrs+0 (CP_RB_RPTR_ADDR, RBRPWB privileged): the ring space check never
 * reads a GX register, so it costs no MMIO and works while GX is collapsed (IFPC). */
uint32_t NativeAdrenoRing::readShadow()
{
    if(cache_.acquire && cache_.acquire(cache_.cookie,fence_,16)) {poison();return rptr_shadow_;}
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    rptr_shadow_=fence_[0];
    return rptr_shadow_;
}
IOReturn NativeAdrenoRing::ibBefore(uint32_t rptr,uint64_t *iova,uint32_t *dwords) const
{
    if(!ring_ || !iova || !dwords)return kIOReturnBadArgument;
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    const uint32_t header=ring_[(rptr-4)&mask];
    if((header>>28)!=7u || ((header>>16)&0x7fu)!=A8XX_PM4_CP_INDIRECT_BUFFER || (header&0x3fffu)!=3)return kIOReturnUnsupported;
    *iova=(uint64_t)ring_[(rptr-2)&mask]<<32|ring_[(rptr-3)&mask];
    *dwords=ring_[(rptr-1)&mask]&0xfffffu;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::stallState(uint32_t *out)
{
    if(!out)return kIOReturnBadArgument;
    if(!attached_)return kIOReturnNotReady;
    for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)
        if(io_.read32(io_.cookie,regs_.stall[i],&out[i]))return poison();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::kick()
{
    enter();
    IOReturn status=kIOReturnNotReady;
    if(attached_ && !poisoned_) {
        status=io_.barrier(io_.cookie)?poison():writeWptr(next_);
    }
    leave();
    return status;
}
/* The prologue of a framed submission, as a7xx_submit(): BR with synced threads, then (on a context change) the A8XX context
 * entry of a6xx_set_pagetable(): wait for the last fence, reset the BR/BV synchronization state between thread syncs, LRZ
 * flush, both performance counter SRAM inits (RBBM and slice), the cache event and the wait until RBBM_PERFCTR_SRAM_INIT_STATUS
 * bit 0; then the cache flush/invalidate with a wait for idle, both threads and IFPC off around the user words. The thread the
 * user words run on must be BOTH, as Linux selects it (on the Gen8 A840 a BR-only thread stopped Turnip's first submission:
 * tu_init_hw needs BV to see its buffer). Independent of the bring-up profile (A830Profile.h). */
IOReturn NativeAdrenoRing::emitPrologue(uint32_t at,bool reset,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords]={0};
    const uint32_t br=RingSyncThreads|RingThreadBr,syncBoth=RingSyncThreads|RingThreadBoth,both=RingThreadBoth,
        marker=A830_CP_SET_MARKER_IFPC_DISABLE;
    const uint32_t count=NativeAdrenoRingPrologueWords+(reset?NativeAdrenoRingResetWords:0);
    size_t n=packet(words,0,RingThreadControl,&br,1);
    if(reset) {
        const uint64_t at4=fence_iova_+A830_MEMPTRS_FENCE;
        const uint32_t wait[4]={0,(uint32_t)at4,(uint32_t)(at4>>32),seqno_},all=RingResetAll,lrz=RingLrzFlushInvalidate,
            invalidate=RingCacheInvalidate;
        if(n)n=packet(words,n,RingWaitTimestamp,wait,4);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingResetContextState,&all,1);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingFenceOpcode,&lrz,1);
        if(n)n=packet(words,n,RingThreadControl,&br,1);
        n=type4(words,n,regs_.sram_init,1);
        if(regs_.slice_sram_init)n=type4(words,n,regs_.slice_sram_init,1);
        else if(n) {const uint32_t nop=0;n=packet(words,n,A8XX_PM4_CP_NOP,&nop,1);}   /* keeps the entry 28 words */
        if(n)n=packet(words,n,RingFenceOpcode,&invalidate,1);
        const uint32_t sram[6]={RingWaitWriteEq,regs_.sram_init_status,0,1,1,0};
        if(n)n=packet(words,n,RingWaitRegMem,sram,6);
    }
    const uint32_t flush=RingCacheFlush7,invalidate7=RingCacheInvalidate7;
    if(n)n=packet(words,n,RingFenceOpcode,&flush,1);
    if(n)n=packet(words,n,RingFenceOpcode,&invalidate7,1);
    if(n)n=packet(words,n,RingWaitForIdle,nullptr,0);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n!=count)return poison();
    for(uint32_t i=0;i<count;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+count)&mask;return kIOReturnSuccess;
}
/* The timestamp fence of a kernel submission: the number into CP_SCRATCH_GLOBAL[2], then CACHE_CLEAN with the timestamp
 * write to memptrs+4 and the interrupt (RBBM_INT_0 bit 20 CP_CACHE_FLUSH_TS). */
IOReturn NativeAdrenoRing::emitFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[NativeAdrenoRingFenceWords]={0};
    const uint64_t at4=fence_iova_+A830_MEMPTRS_FENCE;
    const uint32_t event[4]={RingFenceEvent|RingFenceEnable|RingFenceIrq,(uint32_t)at4,(uint32_t)(at4>>32),seqno};
    if(AdrenoPM4Type4(regs_.scratch,1,&words[0])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    if(packet(words,2,RingFenceOpcode,event,4)!=NativeAdrenoRingFenceWords)return poison();
    for(uint32_t i=0;i<NativeAdrenoRingFenceWords;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+NativeAdrenoRingFenceWords)&mask;return kIOReturnSuccess;
}
/* The fence of a framed submission (Linux a7xx_submit): CP_SCRATCH_GLOBAL[2]; BR invalidates the CCU depth and color caches;
 * BV writes its timestamp (CACHE_CLEAN) to memptrs+12; BR waits for it, so BR cannot report the submission while BV still
 * works on it; BR writes the ring timestamp with the interrupt to memptrs+4; both threads and IFPC back on. CACHE_CLEAN
 * writes UCHE/CCU dirty data back before the timestamp, so a CPU that sees fence == seqno sees the GPU's writes. */
IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[NativeAdrenoRingFramedFenceWords]={0};
    if(AdrenoPM4Type4(regs_.scratch,1,&words[0])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    const uint64_t at4=fence_iova_+A830_MEMPTRS_FENCE,bv=fence_iova_+NativeAdrenoRingFenceBvOffset;
    const uint32_t br=RingThreadBr,bvThread=RingThreadBv,both=RingThreadBoth,depth=RingCcuInvalidateDepth,
        color=RingCcuInvalidateColor,marker=A830_CP_SET_MARKER_IFPC_ENABLE;
    const uint32_t bvWrite[4]={RingFenceEvent|RingFenceEnable,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t bvWait[4]={0,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t fence[4]={RingFenceEvent|RingFenceEnable|RingFenceIrq,(uint32_t)at4,(uint32_t)(at4>>32),seqno};
    size_t n=packet(words,2,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingFenceOpcode,&depth,1);
    if(n)n=packet(words,n,RingFenceOpcode,&color,1);
    if(n)n=packet(words,n,RingThreadControl,&bvThread,1);
    if(n)n=packet(words,n,RingFenceOpcode,bvWrite,4);
    if(n)n=packet(words,n,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingWaitTimestamp,bvWait,4);
    if(n)n=packet(words,n,RingFenceOpcode,fence,4);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n!=NativeAdrenoRingFramedFenceWords)return poison();
    for(uint32_t i=0;i<NativeAdrenoRingFramedFenceWords;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+NativeAdrenoRingFramedFenceWords)&mask;return kIOReturnSuccess;
}
/* Write-combined ring memory needs only the barrier before WPTR; a cache-maintaining client cleans exactly what was written. */
IOReturn NativeAdrenoRing::publish(uint32_t from,uint32_t to)
{
    if(!cache_.publish || from==to)return kIOReturnSuccess;
    const uint32_t words=(uint32_t)(ring_bytes_/4);
    if(from<to)return cache_.publish(cache_.cookie,ring_+from,size_t(to-from)*4)?poison():kIOReturnSuccess;
    if(cache_.publish(cache_.cookie,ring_+from,size_t(words-from)*4))return poison();
    if(to && cache_.publish(cache_.cookie,ring_,size_t(to)*4))return poison();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *cache,
    void *ring,size_t ring_bytes,uint64_t ring_iova,void *fence,size_t fence_bytes,uint64_t fence_iova,
    const NativeAdrenoRingRegs *regs)
{
    if(attached_ || poisoned_ || ring_ || fence_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !cache || !ring || !fence ||
        ring_bytes<8192 || ring_bytes>(1u<<20) || (ring_bytes&(ring_bytes-1)) ||
        ((ring_iova|ring_bytes)&(ring_bytes-1)) || (ring_iova&4095) ||
        fence_bytes<64 || (fence_iova&7) || fence_bytes>=(1u<<24))return kIOReturnBadArgument;
    const NativeAdrenoRingRegs &chosen=regs?*regs:NativeAdrenoRingGen8Regs;
    if(!chosen.base || !chosen.cntl || !chosen.rptr_addr || !chosen.wptr || !chosen.scratch || !chosen.sram_init ||
        !chosen.sram_init_status)return kIOReturnBadArgument;
    io_=*io;cache_=*cache;regs_=chosen;wptr_={};
    ring_=static_cast<volatile uint32_t *>(ring);ring_bytes_=ring_bytes;ring_iova_=ring_iova;
    fence_=static_cast<volatile uint32_t *>(fence);fence_bytes_=fence_bytes;fence_iova_=fence_iova;
    for(size_t i=0;i<ring_bytes_/4;++i)ring_[i]=0;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    next_=rptr_shadow_=seqno_=completed_=submissions_=0;
    reset_pending_=true;   /* the first user submission enters a fresh context */
    attached_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::detach()
{
    enter();
    IOReturn status=kIOReturnSuccess;
    if(poisoned_)status=kIOReturnBusy;
    else if(!attached_)status=kIOReturnNotReady;
    else if(outstanding())status=kIOReturnBusy;
    else {
        attached_=false;ring_=nullptr;fence_=nullptr;ring_bytes_=fence_bytes_=0;
        ring_iova_=fence_iova_=0;next_=rptr_shadow_=seqno_=completed_=submissions_=0;
    }
    leave();
    return status;
}
uint32_t NativeAdrenoRing::extraWords(bool framed,bool reset) const
{
    return framed?NativeAdrenoRingPrologueWords+(reset?NativeAdrenoRingResetWords:0)+NativeAdrenoRingFramedFenceWords:
        NativeAdrenoRingFenceWords;
}
IOReturn NativeAdrenoRing::submit(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    return submitWords(words,word_count,fence,RingFenceKernel,NativeAdrenoRingSourceKernel);
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence,uint32_t source)
{
    return submitWords(words,word_count,fence,RingFenceFramed,source);
}
IOReturn NativeAdrenoRing::submitRaw(const uint32_t *words,size_t word_count,uint32_t *wptr)
{
    return submitWords(words,word_count,wptr,RingFenceNone,NativeAdrenoRingSourceKernel);
}
IOReturn NativeAdrenoRing::submitWords(const uint32_t *words,size_t word_count,uint32_t *fence,int kind,uint32_t source)
{
    if(!fence)return kIOReturnBadArgument;
    *fence=0;
    if(!words || !word_count || word_count>SIZE_MAX/sizeof(uint32_t))return kIOReturnBadArgument;
    enter();
    IOReturn status=kIOReturnSuccess;
    do {
        if(!attached_ || poisoned_) {status=kIOReturnNotReady;break;}
        const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
        const bool framed=kind==RingFenceFramed,reset=framed && reset_pending_;
        const uint32_t extra=kind==RingFenceNone?0:extraWords(framed,reset);
        /* A stream that cannot fit an empty ring is refused for good; one that does not fit now waits for space. */
        if(word_count>size_words-NativeAdrenoRingGapWords-extra) {status=kIOReturnNoResources;break;}
        const uint32_t rptr=readShadow();
        if(poisoned_) {status=kIOReturnIOError;break;}
        if(rptr>=size_words) {status=poison();break;}   /* an impossible read pointer: the CP or the memory is lost */
        const uint32_t used=(next_-rptr)&mask;
        if(word_count+extra+NativeAdrenoRingGapWords>size_words-used) {status=kIOReturnBusy;break;}
        uint32_t at=next_;
        if(framed && (status=emitPrologue(at,reset,&at)))break;
        for(size_t i=0;i<word_count;++i)ring_[(at+i)&mask]=words[i];
        at=(at+(uint32_t)word_count)&mask;
        uint32_t end=at;
        if(kind==RingFenceKernel)status=emitFence(at,seqno_+1,&end);
        else if(kind==RingFenceFramed)status=emitFramedFence(at,seqno_+1,&end);
        if(status)break;
        if((status=publish(next_,end)))break;
        /* The queue meter's stamp (4.13.5) before the barrier: it is in memory before the CP can complete the work. It names whose
         * work this is and whether it enters a new context (a client switch). */
        if(kind!=RingFenceNone && waiter_.stamp)
            waiter_.stamp(waiter_.cookie,seqno_+1,(source&NativeAdrenoRingSourceMask)|(reset?NativeAdrenoRingStampContext:0u));
        if(io_.barrier(io_.cookie)) {status=poison();break;}   /* dsb: every ring word lands before the CP sees WPTR */
        if((status=writeWptr(end)))break;
        next_=end;++submissions_;
        if(kind==RingFenceNone)*fence=end;
        else {
            __atomic_store_n(&seqno_,seqno_+1,__ATOMIC_RELEASE);
            *fence=seqno_;
            if(reset)reset_pending_=false;
        }
    } while(false);
    leave();
    return status;
}
void NativeAdrenoRing::noteCompleted(uint32_t value)
{
    uint32_t seen=__atomic_load_n(&completed_,__ATOMIC_RELAXED);
    while((int32_t)(value-seen)>0 &&
        !__atomic_compare_exchange_n(&completed_,&seen,value,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {}
}
/* Without a waiter: the memptrs fence polled with the caller's clock (each round may sleep in clock->idle). */
NativeAdrenoRingFence NativeAdrenoRing::poll(uint32_t fence,const NativeAdrenoRingClock *clock,uint32_t timeout_us,
    uint32_t *observed,bool retire)
{
    const uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    const uint32_t issued=lastSubmitted();
    for(;;) {
        if(cache_.acquire && cache_.acquire(cache_.cookie,fence_,16)) {poison();return NativeAdrenoRingFencePoisoned;}
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t value=fence_[A830_MEMPTRS_FENCE/4];
        if(observed)*observed=value;
        if((int32_t)(value-issued)>0) {poison();return NativeAdrenoRingFencePoisoned;}   /* a fence never issued */
        if((int32_t)(value-fence)>=0) {
            if(retire)noteCompleted(value);
            return NativeAdrenoRingFenceComplete;
        }
        if(!clock || !clock->now_us)return NativeAdrenoRingFenceTimeout;
        if(clock->now_us(clock->cookie)-start>=timeout_us)return NativeAdrenoRingFenceTimeout;
        if(clock->idle && clock->idle(clock->cookie))return NativeAdrenoRingFenceTimeout;
    }
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence,const NativeAdrenoRingClock *clock,uint32_t timeout_us,
    uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || (int32_t)(fence-lastSubmitted())>0)return NativeAdrenoRingFenceArgument;
    if(!waiter_.wait)return poll(fence,clock,timeout_us,observed,true);
    uint32_t seen=0;
    const IOReturn result=waiter_.wait(waiter_.cookie,fence,timeout_us,&seen);
    if(observed)*observed=seen;
    if(result==kIOReturnSuccess) {
        if((int32_t)(seen-lastSubmitted())>0) {poison();return NativeAdrenoRingFencePoisoned;}
        noteCompleted((int32_t)(seen-fence)>=0?seen:fence);
        return NativeAdrenoRingFenceComplete;
    }
    return result==kIOReturnTimeout?NativeAdrenoRingFenceTimeout:NativeAdrenoRingFenceLost;
}
NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,uint32_t timeout_us,
    uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || (int32_t)(fence-lastSubmitted())>0)return NativeAdrenoRingFenceArgument;
    if(!waiter_.wait || !timeout_us)return poll(fence,clock,timeout_us,observed,false);
    uint32_t seen=0;
    const IOReturn result=waiter_.wait(waiter_.cookie,fence,timeout_us,&seen);
    if(observed)*observed=seen;
    if(result==kIOReturnSuccess)return NativeAdrenoRingFenceComplete;
    return result==kIOReturnTimeout?NativeAdrenoRingFenceTimeout:NativeAdrenoRingFenceLost;
}
IOReturn NativeAdrenoRing::waitForSpace(size_t word_count,bool framed,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
    const uint32_t extra=extraWords(framed,framed);
    if(!word_count || word_count>size_words-NativeAdrenoRingGapWords-extra)return kIOReturnNoResources;
    const uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        const uint32_t used=(next_-readShadow())&mask;
        if(poisoned_)return kIOReturnIOError;
        if(word_count+extra+NativeAdrenoRingGapWords<=size_words-used)return kIOReturnSuccess;
        const uint64_t spent=clock && clock->now_us?clock->now_us(clock->cookie)-start:timeout_us;
        if(spent>=timeout_us)return kIOReturnTimeout;
        /* The CP frees space as it retires work: sleep until the next fence completes (the oldest outstanding one). */
        const uint32_t next=completedFence()+1;
        if(waiter_.wait && (int32_t)(next-lastSubmitted())<=0) {
            uint32_t seen=0;
            const IOReturn result=waiter_.wait(waiter_.cookie,next,uint32_t(timeout_us-spent),&seen);
            if(result==kIOReturnSuccess)noteCompleted(seen);
            else if(result!=kIOReturnTimeout)return result;   /* the waiter's lost-device answer */
        } else if(!clock || !clock->idle || clock->idle(clock->cookie))return kIOReturnTimeout;
    }
}
IOReturn NativeAdrenoRing::recover(uint32_t *last)
{
    if(last)*last=0;
    enter();
    IOReturn status=kIOReturnSuccess;
    if(!attached_)status=kIOReturnNotReady;
    else {
        for(size_t i=0;i<ring_bytes_/4;++i)ring_[i]=0;
        const uint32_t issued=seqno_;
        fence_[A830_MEMPTRS_RPTR/4]=0;fence_[A830_MEMPTRS_BV_RPTR/4]=0;
        fence_[A830_MEMPTRS_FENCE/4]=issued;fence_[NativeAdrenoRingFenceBvOffset/4]=issued;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if(io_.barrier(io_.cookie))status=kIOReturnIOError;
        next_=rptr_shadow_=0;
        noteCompleted(issued);
        poisoned_=false;reset_pending_=true;
        if(last)*last=issued;
    }
    leave();
    return status;
}
