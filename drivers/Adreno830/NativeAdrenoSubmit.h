/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SUBMIT_H
#define NATIVE_ADRENO_SUBMIT_H
#include "NativeAdrenoRing.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoStatus.h"

/* IBs of one submission: a whole frame, so the ring prologue (cache flush, idle wait, thread switch) runs once per frame. */
enum : size_t {NativeAdrenoSubmitMaxBuffers=128};
/* Job slots of the one shared ring (ARCHITECTURE 5.4): every client range (queue / NativeAdrenoSubmitRangeQueues = 0..31, the
 * connection slots of the publish shell, NativeAdrenoMaxClients) owns one reserved slot, the other twelve are shared. Range 0, the
 * render server's, may also take the shared ones; every other range has at most its reserved job in flight (admission control in
 * place of preemption, ARCHITECTURE 4.17). So a caller with nothing in flight always finds its own slot free and is never told Busy
 * for want of a slot. 32 ranges since 2026-10-30 (every app may be a Metal client); a recovery's range masks are 32 bits. */
enum : uint32_t {
    NativeAdrenoSubmitJobs=44,
    NativeAdrenoSubmitRanges=32,
    NativeAdrenoSubmitRangeQueues=8,                       /* NativeAdrenoAbiMaxQueues: one client's queues */
    NativeAdrenoSubmitSharedJobs=NativeAdrenoSubmitJobs-NativeAdrenoSubmitRanges,
    NativeAdrenoSubmitRetryLimitUs=200000,                 /* the longest in-kernel wait for ring space (ARCHITECTURE 5.4) */
    NativeAdrenoSubmitHistory=64,                          /* fences whose client range is remembered (page-fault attribution) */
};
/* Internal status, never returned to user space: the ring has no room for this submission now and the caller has nothing in flight,
 * so it must not be told Busy (Turnip could only spin 64 x 100 us and then declare the device lost). The caller of submit() drops
 * every lock, sleeps until *retry_fence completes (or its 200 ms bound passes) and submits again; NativeAdrenoSubmitWithRetry below is
 * that loop. The value is kIOReturnNoSpace's, so a leak would still be an error, never a success. */
static constexpr IOReturn NativeAdrenoSubmitRetry=IOReturn(0xe00002dbu);
#if defined(kIOReturnNoSpace)
static_assert(NativeAdrenoSubmitRetry==kIOReturnNoSpace,"NativeAdrenoSubmitRetry is kIOReturnNoSpace's value");
#endif
struct NativeAdrenoSubmitBuffer {
    uint64_t handle;
    uint64_t iova;
    size_t bytes;      /* Complete indirect-buffer range in bytes. */
};
struct NativeAdrenoSubmitStats {
    uint32_t submissions,completed,timeouts,poisoned_jobs;
    uint32_t busy,retries,abandoned,sweeps;   /* Busy answers, NativeAdrenoSubmitRetry answers, jobs dropped by recover(), on-demand retires */
    size_t references_held,references_retired,peak_jobs;
};
/* The M10 lock split (ARCHITECTURE 5.4): the publish shell installs IOLock hooks; without hooks nothing is serialised here (host
 * tests, or a caller that serialises everything itself). Ring covers this object's job slots and every submission into the ring
 * (held for microseconds; never while sleeping). Memory covers the BO pool behind NativeAdrenoMemory, which the user-client cores
 * share (a pooled BO's allocation and release), and user imports; a BO outside the pool is allocated, write-combined and released
 * without it (NativeAdrenoMemory::allocatePooled: it touches nothing shared). The VM serialises itself (NativeAdrenoVm's own lock)
 * and is called with or without these. Order: a connection's own lock, then Ring or Memory (never both), then the VM's. */
enum NativeAdrenoSubmitLock : uint32_t {NativeAdrenoSubmitLockRing,NativeAdrenoSubmitLockMemory};
struct NativeAdrenoSubmitLocks {
    void *cookie;
    void (*lock)(void *cookie,NativeAdrenoSubmitLock which);
    void (*unlock)(void *cookie,NativeAdrenoSubmitLock which);
};
/* Kernel-side execbuf: turns validated indirect buffers into one framed ring submission and owns a device reference per distinct
 * buffer until the timestamp fence is actually observed (the CP-written memptrs fence, read with acquire semantics; the one ring
 * completes fences in sequence). Each buffer's mapping also learns the fence of its use (NativeAdrenoVm::noteUse). This is not a
 * userspace ABI and not a scheduler: one in-order ring, no reordering or isolation between queues. Buffers may be sub-ranges of a
 * live VM mapping but never a different IOVA, an unmapped handle, a table pool or a range that includes unreadable bytes. The
 * submit path does no TLB invalidation, no logging, no CPU wait and no cache maintenance beyond the VM's maintained list (GPU-read-
 * only imports mapped non-cacheable, empty from coherence level 1; ARCHITECTURE 2.2, 4.14.1, 5.4): IB validation, references,
 * the ring words and the fenced WPTR write. A failed ring commit that poisons the ring keeps every reference and the submit object
 * unusable until recover() (M10). */
class NativeAdrenoSubmit final {
    struct Job {
        uint32_t fence=0;
        uint32_t queue=0;
        uint32_t count=0;
        bool active=false;
        uint64_t handles[NativeAdrenoSubmitMaxBuffers]{};
    } jobs_[NativeAdrenoSubmitJobs];
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    NativeAdrenoSubmitLocks locks_{};
    NativeAdrenoSubmitStats stats_{};
    bool attached_=false,poisoned_=false,halted_=false;
    /* submit()'s distinct buffers and ring words, used under the Ring lock: too large for the kernel stack at 128 IBs */
    uint64_t scratch_handles_[NativeAdrenoSubmitMaxBuffers]{};
    uint32_t scratch_words_[NativeAdrenoSubmitMaxBuffers*4]{};
    /* The client range of the latest NativeAdrenoSubmitHistory fences this object issued, at fence % NativeAdrenoSubmitHistory:
     * fence | range << 32 | 1 << 63 (0: none). Written and read under the Ring lock; kept across completion and recovery. */
    uint64_t history_[NativeAdrenoSubmitHistory]{};
    /* The unlocked bodies; every public method takes the Ring lock around one of them. */
    Job *findLocked(uint32_t fence);
    Job *slotLocked(uint32_t range);
    bool rangeOutstandingLocked(uint32_t first,uint32_t count) const;
    size_t outstandingLocked() const;
    IOReturn releaseLocked(Job &);
    IOReturn retireCompletedLocked();
    IOReturn poison();
    void dropReferences(const uint64_t *handles,size_t count);
    /* The CP's completed fence (memptrs + 4) and a fence's state without waiting: Complete, Timeout (not yet) or Argument (never
     * issued). Lock-free reads: the ring publishes its issued count atomically. */
    uint32_t completedValue() const;
    NativeAdrenoRingFence state(uint32_t fence,uint32_t *observed) const;
public:
    NativeAdrenoSubmit()=default;
    ~NativeAdrenoSubmit()=default;
    NativeAdrenoSubmit(const NativeAdrenoSubmit &)=delete;
    NativeAdrenoSubmit &operator=(const NativeAdrenoSubmit &)=delete;
    IOReturn attach(NativeAdrenoVm *,NativeAdrenoRing *);
    /* Refuses while jobs are outstanding or poisoned; the caller must have stopped the GPU and observed every fence before calling this. */
    IOReturn detach();
    /* Installs (or with null clears) the lock hooks; only while no job is outstanding. */
    IOReturn attachLocks(const NativeAdrenoSubmitLocks *);
    void lock(NativeAdrenoSubmitLock which) const {if(locks_.lock)locks_.lock(locks_.cookie,which);}
    void unlock(NativeAdrenoSubmitLock which) const {if(locks_.unlock)locks_.unlock(locks_.cookie,which);}
    /* One submission into the ring for client range queue / NativeAdrenoSubmitRangeQueues (0..NativeAdrenoSubmitRanges - 1, i.e. 0..31;
     * queue is bookkeeping, it never reorders). Returns:
     *   kIOReturnSuccess, *fence nonzero;
     *   kIOReturnBusy: no job slot or no ring room, and the caller's range HAS a job in flight (Turnip waits for its oldest fence);
     *   NativeAdrenoSubmitRetry: no ring room and the caller's range has nothing in flight; *retry_fence (when given) is the fence
     *     whose completion frees room: sleep on it without any lock, then submit again (at most NativeAdrenoSubmitRetryLimitUs);
     *   kIOReturnNoResources: the submission cannot fit even into the empty ring (never for <= 128 IBs and a ring >= 8 KiB);
     *   kIOReturnBadArgument / NotReady / IOError (poisoned) as before. Nothing is referenced or written unless it succeeds.
     * context (nonzero) is the submitting client's GPU context: NativeAdrenoRing::useContext runs under the Ring lock right before
     * the words, so a client switch carries the ring's context entry exactly in front of that client's submission. source
     * (NativeAdrenoRingSourceRender for the render server's connections, else External) goes with the words to the ring's stamp:
     * the queue meter's attribution (ARCHITECTURE 4.13.5); it changes nothing else. */
    IOReturn submit(const NativeAdrenoSubmitBuffer *,size_t,uint32_t queue,uint32_t *fence,uint32_t *retry_fence=nullptr,
        uint32_t context=0,uint32_t source=NativeAdrenoRingSourceExternal);
    bool queueOutstanding(uint32_t queue) const;
    /* Queues [first, first+count): whether any of their jobs is outstanding, and a wait for every one of them (oldest first) that
     * leaves other queues' jobs alone. Each client of the shared ring owns one such range. The waits poll with the clock's idle hook
     * between zero-timeout checks and hold no lock while idle (connection teardown only; fence waits of clients sleep on the fence
     * event in the publish shell instead). */
    bool rangeOutstanding(uint32_t first,uint32_t count) const;
    IOReturn waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Retires (without waiting) every job whose fence the CP has already passed, so completed work never pins buffers or job slots
     * just because nobody called wait(). The fence interrupt's retire_call runs it (native_adreno_retire_completed). */
    IOReturn retireCompleted();
    /* Waits for every outstanding job in submission order and retires its references. */
    IOReturn waitAll(const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Checks the fence (timeout_us 0: once, which is all the kernel's WaitFence does after its lock-free sleep) and on completion
     * retires the job's references. A fence whose job was already retired succeeds once the CP has passed it; a fence the ring never
     * issued is kIOReturnBadArgument. */
    IOReturn wait(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Whether the ring issued this fence (nonzero, at most the newest handed out): the only check a submission's wait_fence needs, as
     * the one ring executes in order and every framed submission starts with the BR/BV sync (ARCHITECTURE 4.13.3). */
    bool issued(uint64_t fence) const;
    /* Whether the CP has passed the fence (false for a fence never issued). */
    bool completed(uint32_t fence) const;
    uint32_t lastSubmitted() const;
    /* The jobs in flight when a GPU page fault's top half read the completed fence and the last one issued (M14 P0, ARCHITECTURE
     * 4.12.3): *ranges gets bit r for the client range of every fence in (completed, issued] (0 when the interval is empty: nothing
     * was in flight). kIOReturnUnsupported when the interval is longer than NativeAdrenoSubmitHistory or holds a fence this object
     * did not issue or no longer remembers (a kernel submission, an older numbering): nothing can be said then. Ring lock; thread
     * context only (the fault's bottom half). */
    IOReturn rangesInFlight(uint32_t completed,uint32_t issued,uint32_t *ranges) const;
    /* M10 recovery (ARCHITECTURE 4.13.4). halt(): from now on submit() queues nothing (NativeAdrenoSubmitRetry without a retry
     * fence, or Busy for a range with a job in flight) until resume(); *ranges_in_flight gets bit r for every client range with a job
     * in flight at that moment, under the same lock, so no submission slips between the snapshot and the halt. recover(), after the
     * GPU was power cycled and the ring restarted (nothing of the outstanding jobs runs any more): every job is dropped and its
     * references released, and a poisoned submit object becomes usable again; *abandoned_ranges gets bit r for every client range
     * that lost a job. Both are refused (NotReady) while not attached. *oldest_range (optional) gets the range of the job the CP stands
     * in: the nearest one after the completed fence (on the one in-order ring, the job a hang stopped in), never a job the CP already
     * passed nor a poisoned commit's (fence 0, never issued); UINT32_MAX without such a job. A poisoned object retires nothing first,
     * so its passed jobs and the poisoned commit still count in *ranges_in_flight (recover() drops them). */
    IOReturn halt(uint32_t *ranges_in_flight,uint32_t *oldest_range=nullptr);
    void resume();
    bool halted() const {return __atomic_load_n(&halted_,__ATOMIC_ACQUIRE);}
    IOReturn recover(uint32_t *abandoned_ranges);
    bool poisoned() const {return poisoned_;}
    size_t outstanding() const;
    NativeAdrenoSubmitStats stats() const {return stats_;}
};
/* RAII hold of one of the submit object's locks (Memory for the user-client core's pooled BOs and imports; a null submit object
 * holds none, for a BO outside the pool). */
class NativeAdrenoSubmitGuard final {
    const NativeAdrenoSubmit *submit_;
    NativeAdrenoSubmitLock which_;
public:
    NativeAdrenoSubmitGuard(const NativeAdrenoSubmit *submit,NativeAdrenoSubmitLock which):submit_(submit),which_(which)
    {if(submit_)submit_->lock(which_);}
    ~NativeAdrenoSubmitGuard() {if(submit_)submit_->unlock(which_);}
    NativeAdrenoSubmitGuard(const NativeAdrenoSubmitGuard &)=delete;
    NativeAdrenoSubmitGuard &operator=(const NativeAdrenoSubmitGuard &)=delete;
};
/* The bounded wait of ARCHITECTURE 5.4 around one submission: call(&retry_fence) submits under the caller's locks and returns the
 * status; on NativeAdrenoSubmitRetry, sleep(retry_fence, deadline_us) waits without any lock (on the fence event in the kernel) and
 * the submission is made again, until it is something else or limit_us passed since the first attempt, which turns it into
 * kIOReturnBusy. now() is microseconds. NativeAdrenoSubmitRetry never leaves this function. */
template<class Call,class Sleep,class Now>
static inline IOReturn NativeAdrenoSubmitWithRetry(Call call,Sleep sleep,Now now,uint64_t limit_us=NativeAdrenoSubmitRetryLimitUs)
{
    const uint64_t began=now();
    for(;;) {
        uint32_t retry_fence=0;
        const IOReturn status=call(&retry_fence);
        if(status!=NativeAdrenoSubmitRetry)return status;
        const uint64_t at=now();
        if(at-began>=limit_us)return kIOReturnBusy;
        sleep(retry_fence,began+limit_us);
    }
}
#endif
