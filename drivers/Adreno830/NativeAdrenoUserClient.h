/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_USERCLIENT_H
#define NATIVE_ADRENO_USERCLIENT_H
#include "NativeAdrenoABI.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoStatus.h"
#include "NativeAdrenoHandleIndex.h"
class IOMemoryDescriptor;

/* Kernel-side body of the Adreno user client (one per connection), deliberately free of IOKit so it can be exercised on a host. The
 * IOKit wrapper (NativeAdrenoPublish.cpp) only copies one fixed ABI record in and out and calls these methods under the connection's
 * own lock; it adds no policy. Handles are this object's own identities, never VM, IOVA or pointer values. A mapping is retirable
 * only through freeBo, which unmaps first and then releases the backing object; both steps must succeed. The shared objects are
 * reached under their own serialisation, never this object's: the BO pool (a pooled BO's allocation and release) and imports under
 * the submit object's Memory lock (a BO outside the pool takes no lock: its pages are its own), the submit object under its Ring
 * lock (NativeAdrenoSubmitLocks), the VM under its own lock.
 * Up to NativeAdrenoClientMaxObjects live objects; the submit object's job-slot policy applies (ARCHITECTURE 5.4). */
/* The most objects one connection can hold (i567: backboardd crashed twice at 4096); open() takes a smaller power of two for a client
 * that is not the render server (NativeAdrenoClientObjectsFor, NativeAdrenoPublish.h), so 32 connections do not each pin 4 MiB. */
enum : size_t {NativeAdrenoClientMaxObjects=16384,NativeAdrenoClientMinObjects=256,NativeAdrenoClientContiguousFirstBytes=65536};
/* A BO outside the pool asks for one contiguous run from NativeAdrenoClientContiguousFirstBytes (scattered pages cost a span each) up to
 * NativeAdrenoClientContiguousMaxBytes, and after a failure no request of that size or larger asks for NativeAdrenoClientContiguousBackoffUs,
 * whichever connection failed (pagani-gpu-kopt bit 1; allocBoInner). Without the bit every size asks, every time. */
enum : uint64_t {NativeAdrenoClientContiguousMaxBytes=131072,NativeAdrenoClientContiguousBackoffUs=5000000};
/* The backoff: [0] the smallest size that failed (0: none), [1] when, in the clock's microseconds. */
extern "C" volatile uint64_t native_adreno_contig_backoff[2];
/* Object slots are recycled first in, first out, and a freed slot is reused only after this many allocations of the connection
 * (ARCHITECTURE 6.5 step 2, M10): a token (slot + 1, the clientMemoryForType selector) and, once the slot encoding is on, a handle's
 * low word stay unused long after their object is gone. Only when no fresh slot is left is the oldest freed one taken earlier. */
enum : uint32_t {NativeAdrenoClientSlotQuarantine=4096};
/* Handle encoding (ARCHITECTURE 6.5): 0 (default) monotonic handles 1, 2, 3, ... as Turnip's darwin_bo_finish order requires today;
 * 1 = low 32 bits slot + 1 (<= 16384), high 32 bits the slot's generation. Switch it on only after the Turnip fix (user-space patch
 * M10) and the FIFO quarantine ran on the device (M10 step 3). Read at each allocation. */
extern "C" volatile uint32_t native_adreno_slot_handles;
/* 0 keeps user BOs cacheable (maintained by their user); the board's GPU context reads it to write-combine the BO pool. Default 1. */
extern "C" volatile uint32_t native_adreno_wc_enabled;
/* {count, last size KiB, last end MiB} of the BoMaps refused for falling outside the client's window, one row per submit range (the
 * publish shell's connection slot); the shell's once-a-second tick sums the counts for the ramoops meter line (M14 P0). */
extern "C" volatile uint32_t native_adreno_window_overflow[NativeAdrenoSubmitRanges][3];
/* The display hand-off scanout (memory type 0x5343) as one task maps it (M9, ARCHITECTURE 5.3.2): [start, start + bytes) in task,
 * backed by memory (the presenter's IODeviceMemory; outside the XNU memory window, so its pages cannot be wired as user pages). */
struct NativeAdrenoScanoutMapping {
    void *task;
    uint64_t start,bytes;
    IOMemoryDescriptor *memory;
};
class NativeAdrenoUserClient final {
    struct Queue {uint32_t id=0;bool active=false;};
    struct Entry {
        NativeAdrenoMemory memory;
        uint64_t handle=0;
        uint64_t mapped=0;
        uint64_t iova=0;
        uint64_t freed_at=0;          /* allocation count when the slot was last freed (FIFO quarantine) */
        uint32_t generation=0;        /* times the slot was handed out */
        bool used=false;
        bool mapped_state=false;
        bool unreleasable=false;
        bool imported=false;
        bool scanout=false;           /* imported from the hand-off scanout (GPU writes it non-cacheable, never coherent) */
        uint32_t metadata_bytes=0;
        uint32_t token=0;
        uint32_t mapping_leases=0;
        char metadata[NativeAdrenoAbiMetadataBytes]{};
    };
    /* Allocated at open(), all-zero is the free state. */
    Entry *entries_=nullptr;
    size_t entry_count_=0;
    NativeAdrenoHandleIndex<NativeAdrenoClientMaxObjects> index_;   /* live handle -> its entries_ index */
    /* FIFO of freed slots (entry indices) and the slots never handed out yet: fresh_ is the first of them. */
    uint32_t *fifo_=nullptr;
    size_t fifo_head_=0,fifo_count_=0,fresh_=0,live_=0;
    uint64_t allocations_=0,early_reuses_=0;
    /* Bytes of the live objects by kind (ARCHITECTURE 8.4, the MEM row's client figures): own BOs from the BO pool, own BOs outside it
     * (contiguous or scattered fallbacks), imports (task pages and the scanout). Written under the connection's lock, read by the
     * publish shell's once-a-second tick without it (relaxed atomics). */
    uint64_t pool_bytes_=0,other_bytes_=0,import_bytes_=0;
    void account(const Entry &,bool add);
    Queue queues_[NativeAdrenoAbiMaxQueues]{};
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoSubmit *submit_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    const NativeAdrenoTimer *timer_=nullptr;
    const NativeAdrenoStatus *status_=nullptr;
    const NativeAdrenoAbiDeviceInfo *info_=nullptr;
    NativeAdrenoScanoutMapping scanout_{};
    uint32_t input_bits_=0,output_bits_=0;
    /* Scope on the shared ring and GPU space: this client's queues are the submit object's queues [queue_base_, +MaxQueues), and with a
     * window (window_bytes_ nonzero) every mapping must lie inside [window_start_, +window_bytes_), which GetDeviceInfo reports as the
     * VA and the publish shell uses to attribute a GPU page fault to this connection. */
    uint32_t queue_base_=0;
    uint32_t context_=0;   /* this client's GPU context for NativeAdrenoRing::useContext */
    uint32_t source_=NativeAdrenoRingSourceExternal;   /* whose jobs the queue meter counts them as (setRenderServer) */
    /* A client owns a queue range once setScope() gave it one or it has submitted into the default one (a single client that never scopes
     * owns range 0). Before either, queue_base_ is only the default 0, which a scoped client may own, so a check of "this client's own
     * range" would test someone else's jobs. */
    bool scoped_=false,used_=false;
    NativeAdrenoSubmitBuffer submit_buffers_[NativeAdrenoAbiMaxBuffers]{};   /* submit()'s list: too large for the kernel stack */
    NativeAdrenoAbiSubmit submit_request_{};   /* the submit request as copied in, for the same reason */
    uint32_t retry_fence_=0;   /* after a submit answered NativeAdrenoSubmitRetry: the fence to sleep on */
    uint64_t window_start_=0,window_bytes_=0,window_peak_=0;
    /* setReserve(): what this client's mappings leave of the VM's tables, and BO pool bytes its allocations leave (0: none kept). */
    size_t keep_objects_=0,keep_spans_=0,keep_pool_=0;
    uint64_t next_handle_=1;
    bool open_=false;
    Entry *find(uint64_t);
    const Entry *find(uint64_t) const;
    Entry *free();
    void clear(Entry &);
    void publish(Entry &);
    IOReturn allocBoInner(const NativeAdrenoAbiBoAlloc *,NativeAdrenoAbiBoAllocReply *,const NativeAdrenoRingClock *);
    IOReturn zeroPooled(Entry &,const NativeAdrenoRingClock *);   /* a pooled BO's zero fill, timed for the call meter */
    void releaseEntry(Entry &);   /* a failed allocation/import: release what it left, quarantine the slot if that fails */
public:
    NativeAdrenoUserClient()=default;
    ~NativeAdrenoUserClient()=default;
    NativeAdrenoUserClient(const NativeAdrenoUserClient &)=delete;
    NativeAdrenoUserClient &operator=(const NativeAdrenoUserClient &)=delete;
    /* Mounts one already-qualified context. input/output bits describe the GPU address space and the SMMU output range (IDR2.OAS, 36
     * on SM8750) and are reported, never guessed; output_bits also bounds every BO's physical address. objects: the most live objects
     * (a power of two from NativeAdrenoClientMinObjects to NativeAdrenoClientMaxObjects; its tables are allocated here). */
    IOReturn open(uint32_t abi_version,NativeAdrenoVm *,NativeAdrenoSubmit *,
        NativeAdrenoRing *,uint32_t input_bits,uint32_t output_bits,size_t objects=NativeAdrenoClientMaxObjects);
    /* The objects this connection can hold (0 while closed). */
    size_t capacity() const {return entry_count_;}
    /* For every client but the render server (2026-10-30, every app a Metal client): its BoMaps leave keep_objects mapping records and
     * keep_spans table ranges of the shared VM free (NativeAdrenoVm::mapClient), its BoAllocs take from the BO pool only while
     * keep_pool bytes stay free in it (else contiguous or scattered pages, as when the pool is full). Only while open. */
    IOReturn setReserve(size_t keep_objects,size_t keep_spans,size_t keep_pool);
    /* One client of several on the same VM, submit object and ring: its queue range (queue_base a multiple of NativeAdrenoAbiMaxQueues) and
     * GPU address window (4 KiB aligned, inside the input range; bytes 0 = unrestricted). Only while open with no live object or job. */
    IOReturn setScope(uint32_t queue_base,uint64_t window_start,uint64_t window_bytes);
    /* The publish shell names the render server's connections (whatever slot they got): their submissions go to the ring as the
     * render server's, every other client's as an external process's (the queue meter's attribution, ARCHITECTURE 4.13.5; nothing
     * else changes). Only while open; open() starts every connection as external. */
    void setRenderServer(bool render_server)
    {if(open_)source_=render_server?NativeAdrenoRingSourceRender:NativeAdrenoRingSourceExternal;}
    uint32_t source() const {return source_;}
    /* Highest end address (exclusive) any mapping of this client has reached, 0 before the first. */
    uint64_t windowPeak() const {return window_peak_;}
    uint64_t windowStart() const {return window_start_;}
    uint64_t windowBytes() const {return window_bytes_;}
    uint32_t queueBase() const {return queue_base_;}
    /* Whether a GPU address lies in this client's window (never without a window): the page-fault attribution of ARCHITECTURE 4.12.3. */
    bool windowContains(uint64_t iova) const {return window_bytes_ && iova>=window_start_ && iova-window_start_<window_bytes_;}
    /* Whether any job is unobserved in the range this client owns (never, while it owns none). */
    bool ownRangeOutstanding() const;
    /* Refuses while any object is live or any of this client's jobs is unobserved. */
    IOReturn close();
    /* Connection teardown: waits for every outstanding job, then unmaps and frees every object, closes every queue and
     * closes the client. An object that refuses (never released objects, a release that failed) stays live and the call answers the
     * first refusal, but every other object is still freed: one refusal no longer keeps every later object of the connection for
     * the rest of the boot. A refused wait frees nothing; nothing is forced. */
    IOReturn releaseAll(const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Backing object of a token returned by cpuMap while its lease is held (clientMemoryForType); null otherwise. */
    NativeAdrenoMemory *memoryForToken(uint32_t token);
    /* CPU mappings of the BO of this token that BoCpuMap handed out and BoCpuUnmap has not given back; 0 for no such BO. */
    uint32_t mappingLeases(uint32_t token) const;
    /* The token (clientMemoryForType selector) of a live handle, 0 for none: the IOKit wrapper drops the token's CPU view whenever
     * its BO is freed, so no view outlives its pages. */
    uint32_t tokenOf(uint64_t handle) const;
    /* Bytes of the live objects: own BOs from the BO pool, own BOs outside it, imports. Any thread (relaxed reads). */
    void usage(uint64_t *pool,uint64_t *other,uint64_t *imported) const;
    IOReturn query(NativeAdrenoAbiParams *);
    /* clock (dispatch's; optional): the contiguous backoff's time and the zero fill's. Without one nothing is remembered or timed. */
    IOReturn allocBo(const NativeAdrenoAbiBoAlloc *,NativeAdrenoAbiBoAllocReply *,const NativeAdrenoRingClock *clock=nullptr);
    IOReturn mapBo(const NativeAdrenoAbiBoMap *,NativeAdrenoAbiBoMapReply *);
    /* BoImport: pins the caller's pages. Only the IOKit wrapper can call it, with the calling task; dispatch() refuses the method
     * because it has no task (the kernel mailbox path cannot import). An address range inside the task's scanout mapping (setScanout)
     * becomes a BO of the scanout's own physical range instead (M9): nothing is wired, the GPU maps it non-cacheable and writes it. */
    IOReturn importBo(const NativeAdrenoAbiBoImport *,void *task,NativeAdrenoAbiBoAllocReply *);
    /* M9: the scanout mapping of the calling task, as the wrapper found it right before an import (memory null or bytes 0: none). */
    void setScanout(const NativeAdrenoScanoutMapping *);
    /* Whether [address, address + bytes) of task lies inside its scanout mapping; *offset is then its offset in the scanout. */
    bool isScanoutAddress(void *task,uint64_t address,uint64_t bytes,uint64_t *offset) const;
    /* Unmaps if still mapped, then releases the backing object. A failure at
     * either step keeps the entry live and unmapped where possible. */
    IOReturn freeBo(const NativeAdrenoAbiBoFree *);
    /* A submission. wait_fence is only validated (validateWaitFence): the ring orders it, there is no CPU wait. Besides the ABI's
     * answers it can return NativeAdrenoSubmitRetry (the caller has nothing in flight and the ring is full): the IOKit wrapper sleeps
     * on retryFence() without any lock and submits again (NativeAdrenoSubmitWithRetry); that status never reaches user space. */
    IOReturn submit(const NativeAdrenoAbiSubmit *,NativeAdrenoAbiSubmitReply *,const NativeAdrenoRingClock *);
    uint32_t retryFence() const {return retry_fence_;}
    /* A submission's wait_fence: zero, or a fence the ring already issued (at most the newest handed out); anything else is refused. */
    IOReturn validateWaitFence(uint64_t fence) const;
    IOReturn cpuMap(const NativeAdrenoAbiBoCpuMap *,NativeAdrenoAbiBoCpuMapReply *);
    IOReturn cpuUnmap(const NativeAdrenoAbiBoCpuUnmap *);
    IOReturn createQueue(NativeAdrenoAbiQueueReply *);
    IOReturn closeQueue(const NativeAdrenoAbiQueue *);
    IOReturn setMetadata(const NativeAdrenoAbiMetadata *);
    IOReturn getMetadata(const NativeAdrenoAbiMetadata *,NativeAdrenoAbiMetadata *);
    /* Checks the fence (timeout 0: once) and retires its job when passed. The kernel calls it only with timeout 0, after its lock-free
     * sleep on the fence event (ARCHITECTURE 4.13.3); a nonzero timeout polls with the clock's idle hook (host tests). */
    IOReturn waitFence(const NativeAdrenoAbiWaitFence *,const NativeAdrenoRingClock *);
    /* One external-method entry: exact record sizes are required, unknown
     * methods are unsupported and no output byte is modified on failure. The
     * IOKit wrapper should only copy these buffers and call this function. */
    IOReturn dispatch(uint32_t method,const void *input,size_t input_size,
        void *output,size_t output_size,const NativeAdrenoRingClock *);
    static size_t inputSize(uint32_t method);
    static size_t outputSize(uint32_t method);
    size_t liveObjects() const;
    /* Allocations so far and slot reuses that came before the quarantine passed (no fresh slot left). */
    uint64_t allocations() const {return allocations_;}
    uint64_t earlyReuses() const {return early_reuses_;}
    /* Optional GPU timestamp source; absent means the method stays unsupported. */
    IOReturn attachTimer(const NativeAdrenoTimer *);
    IOReturn timestamp(NativeAdrenoAbiTimestamp *);
    /* This connection's fault/suspend counters (the wrapper mounts one per connection); absent means the status method stays unsupported. */
    IOReturn attachStatus(const NativeAdrenoStatus *);
    IOReturn status(NativeAdrenoAbiStatus *);
    /* Optional constant device facts; absent means the method stays unsupported. */
    IOReturn attachDeviceInfo(const NativeAdrenoAbiDeviceInfo *);
    IOReturn deviceInfo(NativeAdrenoAbiDeviceInfo *);
    bool mapped(uint64_t handle);
    /* Backing object of a live handle, for the IOKit wrapper's clientMemoryForType and for kernel self-tests;
     * null for an unknown or unreleasable handle. The client keeps ownership. */
    NativeAdrenoMemory *memoryOf(uint64_t handle);
};
#endif
