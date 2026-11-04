/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address and IB_SIZE in dwords (bits 0..19), exactly as the
 * pinned adreno_pm4.xml domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxBuffers=(uint32_t)NativeAdrenoSubmitMaxBuffers,
    SubmitMaxIbBytes=0xfffffu*4u};
static_assert(NativeAdrenoSubmitJobs==NativeAdrenoSubmitRanges+NativeAdrenoSubmitSharedJobs && NativeAdrenoSubmitRanges<=32,
              "one reserved job slot per client range, the rest shared");

namespace {
/* Ring fences are 32-bit sequence numbers: a is at or after b. */
bool atOrAfter(uint32_t a,uint32_t b) {return a-b<0x80000000u;}
}

IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
uint32_t NativeAdrenoSubmit::completedValue() const
{
    /* memptrs + 4, the BR fence the CACHE_CLEAN timestamp writes (ARCHITECTURE 4.13.1): Normal-NC on both sides, so an acquire load
     * is the whole read; it orders this CPU's later accesses after the fence it saw. */
    const volatile uint32_t *memptrs=ring_?ring_->memptrs():nullptr;
    return memptrs?__atomic_load_n(const_cast<const uint32_t *>(&memptrs[1]),__ATOMIC_ACQUIRE):0;
}
NativeAdrenoRingFence NativeAdrenoSubmit::state(uint32_t fence,uint32_t *observed) const
{
    const uint32_t value=completedValue();
    if(observed)*observed=value;
    if(!ring_ || !fence || !atOrAfter(ring_->lastSubmitted(),fence))return NativeAdrenoRingFenceArgument;
    return atOrAfter(value,fence)?NativeAdrenoRingFenceComplete:NativeAdrenoRingFenceTimeout;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::findLocked(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::slotLocked(uint32_t range)
{
    if(!jobs_[range].active)return &jobs_[range];
    /* Admission control (ARCHITECTURE 4.17, 5.4): only the render server's range takes the shared slots. */
    if(range)return nullptr;
    for(uint32_t i=NativeAdrenoSubmitRanges;i<NativeAdrenoSubmitJobs;++i)if(!jobs_[i].active)return &jobs_[i];
    return nullptr;
}
bool NativeAdrenoSubmit::rangeOutstandingLocked(uint32_t first,uint32_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstandingLocked() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
void NativeAdrenoSubmit::dropReferences(const uint64_t *handles,size_t count)
{
    while(count) {vm_->retireJobAfterCompletion(handles[--count]);++stats_.references_retired;}
}
IOReturn NativeAdrenoSubmit::releaseLocked(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    for(uint32_t i=0;i<job.count;++i) {
        const IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    job.active=false;job.count=0;job.fence=0;job.queue=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompletedLocked()
{
    /* One read of the fence decides every job: the ring completes in order. */
    const uint32_t observed=completedValue();
    for(auto &job:jobs_) {
        if(!job.active || !job.fence || !atOrAfter(observed,job.fence))continue;
        const IOReturn status=releaseLocked(job);
        if(status)return status;
        ++stats_.completed;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};
    for(auto &job:jobs_) {job.active=false;job.count=0;job.fence=0;job.queue=0;}
    for(auto &entry:history_)entry=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(poisoned_ || outstandingLocked())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::attachLocks(const NativeAdrenoSubmitLocks *locks)
{
    /* No lock is held here: the hooks change only while nothing can be using them (no job outstanding, before publication). */
    if(outstandingLocked())return kIOReturnBusy;
    if(locks && (!locks->lock || !locks->unlock))return kIOReturnBadArgument;
    locks_=locks?*locks:NativeAdrenoSubmitLocks{};
    return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return rangeOutstandingLocked(first,count);
}
size_t NativeAdrenoSubmit::outstanding() const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return outstandingLocked();
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,uint32_t queue,uint32_t *fence,
    uint32_t *retry_fence,uint32_t context,uint32_t source)
{
    if(!fence)return kIOReturnBadArgument;
    *fence=0;
    if(retry_fence)*retry_fence=0;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    const uint32_t range=queue/NativeAdrenoSubmitRangeQueues;
    if(range>=NativeAdrenoSubmitRanges)return kIOReturnBadArgument;
    NativeAdrenoSubmitGuard ring_lock(this,NativeAdrenoSubmitLockRing);
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(halted_) {
        /* A recovery holds the ring: a caller with work in flight waits for its fence (the recovery completes it), any other sleeps. */
        if(rangeOutstandingLocked(range*NativeAdrenoSubmitRangeQueues,NativeAdrenoSubmitRangeQueues)) {++stats_.busy;return kIOReturnBusy;}
        ++stats_.retries;
        return NativeAdrenoSubmitRetry;
    }
    /* Every buffer is validated against the live VM before any reference: the IB check is the one thing the submit path keeps. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    static_assert(sizeof(scratch_words_)/sizeof(scratch_words_[0])>=SubmitMaxBuffers*(1+SubmitIbWords),"ring words");
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        const IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova || buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))
            return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    /* A job slot: the range's own one, or (range 0) a shared one. Completed jobs are retired here only when that finds none: the
     * fence interrupt's retire_call keeps the slots clean, the submit path does not walk the jobs (ARCHITECTURE 5.4). */
    Job *job=slotLocked(range);
    if(!job) {
        ++stats_.sweeps;
        const IOReturn swept=retireCompletedLocked();
        if(swept)return swept;
        job=slotLocked(range);
    }
    if(!job) {
        /* The range's reserved slot holds the range's own job: the caller has work in flight and waits for its oldest fence. */
        ++stats_.busy;
        return kIOReturnBusy;
    }
    for(size_t i=0;i<handle_count;++i) {
        const IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            dropReferences(handles,i);
            return status;
        }
        ++stats_.references_held;
    }
    /* GPU-read-only imports that are not mapped coherent (the maintained list; empty from coherence level 1, ARCHITECTURE 4.14.1). */
    IOReturn status=vm_->cleanForSubmission();
    if(status) {
        dropReferences(handles,handle_count);
        return status;
    }
    if(context)ring_->useContext(context);
    uint32_t value=0;
    status=ring_->submitFramed(words,word_count,&value,source);
    if(status==kIOReturnBusy) {
        /* No room in the ring now. Room comes back as the CP consumes: the oldest outstanding submission's fence is the event. */
        dropReferences(handles,handle_count);
        if(rangeOutstandingLocked(range*NativeAdrenoSubmitRangeQueues,NativeAdrenoSubmitRangeQueues)) {++stats_.busy;return kIOReturnBusy;}
        /* The ring completes in order: the next fence to complete is the one after the CP's completed fence. */
        const uint32_t done=completedValue(),newest=ring_->lastSubmitted();
        if(retry_fence)*retry_fence=atOrAfter(done,newest)?newest:done+1;
        ++stats_.retries;
        return NativeAdrenoSubmitRetry;
    }
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the referenced buffers cannot be retired here: the job is
         * kept (with no fence) until recover() after a GPU reset. */
        if(ring_->poisoned()) {
            job->fence=0;job->queue=queue;job->count=(uint32_t)handle_count;job->active=true;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            ++stats_.poisoned_jobs;
            return poison();
        }
        /* kIOReturnNoResources: not even the drained ring takes it (only a larger submission than this object builds). */
        dropReferences(handles,handle_count);
        return status;
    }
    job->fence=value;job->queue=queue;job->count=(uint32_t)handle_count;job->active=true;
    /* Whose job this fence is, for a page fault's attribution (rangesInFlight): kept after the job retires. */
    history_[value%NativeAdrenoSubmitHistory]=uint64_t(value)|uint64_t(range)<<32|uint64_t(1)<<63;
    for(size_t i=0;i<handle_count;++i) {
        job->handles[i]=handles[i];
        /* The fence of the buffer's last use, for the VM's idle check before an unmap (the job's reference covers it until then). */
        (void)vm_->noteUse(handles[i],value);
    }
    ++stats_.submissions;
    const size_t now_outstanding=outstandingLocked();
    if(now_outstanding>stats_.peak_jobs)stats_.peak_jobs=now_outstanding;
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    const uint64_t began=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            if(!attached_ || poisoned_)return kIOReturnNotReady;
            const NativeAdrenoRingFence result=state(fence,nullptr);
            if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
            if(result==NativeAdrenoRingFenceComplete) {
                if(Job *job=findLocked(fence)) {
                    const IOReturn status=releaseLocked(*job);
                    if(status)return status;
                    ++stats_.completed;
                }
                return kIOReturnSuccess;
            }
            if(!timeout_us || !clock || !clock->now_us || clock->now_us(clock->cookie)-began>=timeout_us) {
                ++stats_.timeouts;
                return kIOReturnTimeout;
            }
        }
        /* The pause is the clock's (until the next GPU interrupt or a short sleep), with no lock held. */
        if(!clock->idle || clock->idle(clock->cookie)) {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            ++stats_.timeouts;
            return kIOReturnTimeout;
        }
    }
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    const uint64_t began=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            if(!attached_ || poisoned_)return kIOReturnNotReady;
            const IOReturn swept=retireCompletedLocked();
            if(swept)return swept;
            if(!rangeOutstandingLocked(first,count))return kIOReturnSuccess;
            if(!clock || !clock->now_us || clock->now_us(clock->cookie)-began>=timeout_us) {
                ++stats_.timeouts;
                return kIOReturnTimeout;
            }
        }
        if(!clock->idle || clock->idle(clock->cookie)) {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            ++stats_.timeouts;
            return kIOReturnTimeout;
        }
    }
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    return waitRange(0,UINT32_MAX,clock,timeout_us);
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    return retireCompletedLocked();
}
bool NativeAdrenoSubmit::issued(uint64_t fence) const
{
    return attached_ && ring_ && fence && fence<=UINT32_MAX && atOrAfter(ring_->lastSubmitted(),(uint32_t)fence);
}
bool NativeAdrenoSubmit::completed(uint32_t fence) const
{
    return attached_ && state(fence,nullptr)==NativeAdrenoRingFenceComplete;
}
uint32_t NativeAdrenoSubmit::lastSubmitted() const
{
    return attached_ && ring_?ring_->lastSubmitted():0;
}
IOReturn NativeAdrenoSubmit::rangesInFlight(uint32_t completed,uint32_t issued,uint32_t *ranges) const
{
    if(!ranges)return kIOReturnBadArgument;
    *ranges=0;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    const uint32_t count=issued-completed;
    if(count>NativeAdrenoSubmitHistory)return kIOReturnUnsupported;   /* also a completed fence past the issued one */
    uint32_t mask=0;
    for(uint32_t i=1;i<=count;++i) {
        const uint32_t fence=completed+i;
        const uint64_t entry=history_[fence%NativeAdrenoSubmitHistory];
        if(!(entry>>63) || uint32_t(entry)!=fence)return kIOReturnUnsupported;
        mask|=1u<<((entry>>32)&(NativeAdrenoSubmitRanges-1));
    }
    *ranges=mask;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::halt(uint32_t *ranges_in_flight,uint32_t *oldest_range)
{
    if(ranges_in_flight)*ranges_in_flight=0;
    if(oldest_range)*oldest_range=UINT32_MAX;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    __atomic_store_n(&halted_,true,__ATOMIC_RELEASE);
    /* Jobs the CP already passed are not lost: retire them first (a poisoned object has nothing to retire safely). */
    if(!poisoned_)(void)retireCompletedLocked();
    uint32_t ranges=0,oldest=UINT32_MAX,distance=UINT32_MAX;
    const uint32_t done=completedValue();
    for(const auto &job:jobs_) {
        if(!job.active)continue;
        ranges|=1u<<(job.queue/NativeAdrenoSubmitRangeQueues);
        /* Fences are sequence numbers: the job the CP stands in is the one nearest after the completed fence. A job the CP already
         * passed (a poisoned object retires nothing) and a poisoned commit's job (fence 0, never issued) are not in flight on the CP:
         * never chosen, and with no other job there is no culprit. */
        if(!job.fence || atOrAfter(done,job.fence))continue;
        const uint32_t ahead=job.fence-done;
        if(ahead<distance) {distance=ahead;oldest=job.queue/NativeAdrenoSubmitRangeQueues;}
    }
    if(ranges_in_flight)*ranges_in_flight=ranges;
    if(oldest_range)*oldest_range=oldest;
    return kIOReturnSuccess;
}
void NativeAdrenoSubmit::resume()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    __atomic_store_n(&halted_,false,__ATOMIC_RELEASE);
}
IOReturn NativeAdrenoSubmit::recover(uint32_t *abandoned_ranges)
{
    if(abandoned_ranges)*abandoned_ranges=0;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    uint32_t ranges=0;
    IOReturn result=kIOReturnSuccess;
    for(auto &job:jobs_) {
        if(!job.active)continue;
        ranges|=1u<<(job.queue/NativeAdrenoSubmitRangeQueues);
        const IOReturn status=releaseLocked(job);
        if(status && !result)result=status;
        if(!status)++stats_.abandoned;
    }
    if(abandoned_ranges)*abandoned_ranges=ranges;
    if(result)return poison();
    poisoned_=false;
    return kIOReturnSuccess;
}
