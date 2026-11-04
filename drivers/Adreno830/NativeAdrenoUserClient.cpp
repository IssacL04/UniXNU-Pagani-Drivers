/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMeter.h"
#include <IOKit/IOLib.h>
#include <string.h>

static_assert(uint32_t(NativeAdrenoSubmitRangeQueues)==uint32_t(NativeAdrenoAbiMaxQueues),"one submit range per client's queues");
static_assert(NativeAdrenoClientMaxObjects<=UINT32_MAX-1,"tokens are slot + 1 in 32 bits");
extern "C" {volatile uint64_t native_adreno_contig_backoff[2];}
/* 0 keeps user BOs cacheable and maintained by their user (int-070..097); 1 (default) write-combines them, as the GPU maps them
 * non-cacheable. The board's GPU context reads it to write-combine the BO pool once. */
extern "C" {volatile uint32_t native_adreno_wc_enabled=1;}
/* ARCHITECTURE 6.5 step 3: the slot encoding of handles stays off until the Turnip order fix and the FIFO quarantine ran on the device. */
extern "C" {volatile uint32_t native_adreno_slot_handles=0;}
/* BoImport record: [0] calls, [1] refused, [2..5] last refusal (status, stage, bytes, address), [6..9] first refusal. Stages 1..3 are
 * this file's checks, 4 the memory object's own import (importTask / importDevice), 5 a scanout import. */
extern "C" {volatile uint64_t native_adreno_import_diag[10];}
/* Imports and their release (int-233/234: Core Animation's render thread waited in vm_deallocate for pages a kernel wiring held): [0] imports
 * alive, [1] frees of an import refused, [2] last refusal status; then the 8 newest imports as {address, pid<<40|bytes, handle, state}, state
 * 1 alive, 2 released, 3 free refused, 4 scanout. */
extern "C" {volatile uint64_t native_adreno_import_live[3+8*4];}
/* Per ABI method (1..16): [0] calls, [1] calls that returned non-zero, [2] last status. */
extern "C" {volatile uint32_t native_adreno_method_stats[20][3];}
/* Why BoMap fails: up to five distinct statuses with their counts, then the last failed request's iova page and handle. */
extern "C" {volatile uint32_t native_adreno_map_fail[6][2];}
/* {count, last size KiB, last end MiB} of the mappings that fell outside each client window (one row per submit range, which is the
 * publish shell's connection slot). */
extern "C" {volatile uint32_t native_adreno_window_overflow[NativeAdrenoSubmitRanges][3];}
extern "C" int proc_selfpid(void);
namespace {
unsigned gImportNext;
void noteImportState(uint64_t handle,uint64_t state)
{
    for(unsigned i=0;i<8;++i)if(native_adreno_import_live[3+i*4+2]==handle) {native_adreno_import_live[3+i*4+3]=state;return;}
}
void noteImport(IOReturn status,uint32_t stage,uint64_t bytes,uint64_t address)
{
    if(!status)return;
    native_adreno_import_diag[1]=native_adreno_import_diag[1]+1;
    native_adreno_import_diag[2]=(uint32_t)status;native_adreno_import_diag[3]=stage;native_adreno_import_diag[4]=bytes;
    native_adreno_import_diag[5]=address;
    if(native_adreno_import_diag[1]==1) {
        native_adreno_import_diag[6]=(uint32_t)status;native_adreno_import_diag[7]=stage;native_adreno_import_diag[8]=bytes;
        native_adreno_import_diag[9]=address;
    }
}
}
/* Per client slot (NativeAdrenoSubmitRanges of them, the kernel's own client last): [0] live objects now, [1] their peak, [2] refused
 * BoAllocs, [3] last refusal status, [4] bytes of the last refused request, [5] the window's highest mapped end in MiB above its start.
 * Turnip turns any refusal into VK_ERROR_OUT_OF_DEVICE_MEMORY, which its command streams do not check (int-237: backboardd wrote
 * through a NULL stream pointer). */
extern "C" {volatile uint32_t native_adreno_client_stats[NativeAdrenoSubmitRanges+1][6];}
static volatile uint32_t *clientStats(uint32_t queue_base,uint64_t window_bytes) {
    const uint32_t slot=window_bytes?queue_base/NativeAdrenoAbiMaxQueues:NativeAdrenoSubmitRanges;
    return native_adreno_client_stats[slot<NativeAdrenoSubmitRanges?slot:NativeAdrenoSubmitRanges];
}
static void noteLive(uint32_t queue_base,uint64_t window_bytes,size_t live) {
    volatile uint32_t *stats=clientStats(queue_base,window_bytes);
    stats[0]=(uint32_t)live;if(live>stats[1])stats[1]=(uint32_t)live;
}
/* The contiguous attempt of a BO outside the pool (upstream Adreno740 01ed692; pagani-gpu-kopt bit 1). A request the free memory cannot
 * serve scans every free page before it gives up, under the VM's page-queue and free-page locks (vm_page_find_contiguous), so a failed
 * search delays every page allocation of the system, not only this call. Upstream's 124 searches took 163 ms each at the median, and
 * every success was 64 to 128 KiB: so larger BOs never ask, and after a failure no request of that size or larger asks for 5 s. The
 * free memory is everyone's, so the backoff is too; the connections get here without a common lock and a race between its two words
 * costs at most one search more or fewer. */
static bool contiguousAllowed(uint64_t bytes,const NativeAdrenoRingClock *clock)
{
    if(bytes<NativeAdrenoClientContiguousFirstBytes)return false;
    if(!NativeAdrenoKoptOn(NativeAdrenoKoptContiguous))return true;
    if(bytes>NativeAdrenoClientContiguousMaxBytes)return false;
    const uint64_t failed=__atomic_load_n(&native_adreno_contig_backoff[0],__ATOMIC_RELAXED);
    if(!failed)return true;
    if(clock && clock->now_us && clock->now_us(clock->cookie)-__atomic_load_n(&native_adreno_contig_backoff[1],__ATOMIC_RELAXED)>=
        NativeAdrenoClientContiguousBackoffUs) {
        __atomic_store_n(&native_adreno_contig_backoff[0],uint64_t(0),__ATOMIC_RELAXED);
        return true;
    }
    return bytes<failed;
}
static void contiguousFailed(uint64_t bytes,const NativeAdrenoRingClock *clock)
{
    __atomic_fetch_add(&native_adreno_meter_contig[1],1u,__ATOMIC_RELAXED);
    if(!NativeAdrenoKoptOn(NativeAdrenoKoptContiguous) || !clock || !clock->now_us)return;
    __atomic_store_n(&native_adreno_contig_backoff[1],clock->now_us(clock->cookie),__ATOMIC_RELAXED);
    __atomic_store_n(&native_adreno_contig_backoff[0],bytes,__ATOMIC_RELAXED);
}

NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::find(uint64_t handle)
{
    const uint32_t index=entries_?index_.find(handle):UINT32_MAX;
    if(index>=entry_count_)return nullptr;
    Entry &entry=entries_[index];
    return entry.used && entry.handle==handle?&entry:nullptr;
}
const NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::find(uint64_t handle) const
{
    return const_cast<NativeAdrenoUserClient *>(this)->find(handle);
}
/* A slot for a new object (ARCHITECTURE 6.5 step 2): the oldest freed slot once NativeAdrenoClientSlotQuarantine allocations passed
 * since it was freed, else a slot never handed out, else (no fresh slot left) the oldest freed one anyway. */
NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::free()
{
    if(!entries_ || !fifo_)return nullptr;
    const bool quarantined=fifo_count_ && allocations_-entries_[fifo_[fifo_head_]].freed_at<NativeAdrenoClientSlotQuarantine;
    if(!fifo_count_ || quarantined) {
        if(fresh_<entry_count_)return &entries_[fresh_++];
        if(!fifo_count_)return nullptr;
        ++early_reuses_;
    }
    Entry &entry=entries_[fifo_[fifo_head_]];
    fifo_head_=(fifo_head_+1)%entry_count_;--fifo_count_;
    return &entry;
}
/* A new live entry with a handle: counted and findable. */
void NativeAdrenoUserClient::publish(Entry &entry)
{
    const uint32_t index=(uint32_t)(&entry-entries_);
    ++allocations_;
    if(!++entry.generation)entry.generation=1;
    entry.used=true;
    entry.handle=native_adreno_slot_handles?(uint64_t)entry.generation<<32|(index+1):next_handle_++;
    entry.token=index+1;++live_;
    index_.insert(entry.handle,index);
    account(entry,true);
}
/* The live bytes by kind, from the backing object itself (valid from a successful allocate/import until its release). */
void NativeAdrenoUserClient::account(const Entry &entry,bool add)
{
    const NativeAdrenoMemory &memory=entry.memory;
    uint64_t *counter=memory.imported() || memory.scanout()?&import_bytes_:memory.pooled()?&pool_bytes_:&other_bytes_;
    const uint64_t bytes=memory.bytes(),now=__atomic_load_n(counter,__ATOMIC_RELAXED);
    __atomic_store_n(counter,add?now+bytes:now>=bytes?now-bytes:0,__ATOMIC_RELAXED);
}
void NativeAdrenoUserClient::usage(uint64_t *pool,uint64_t *other,uint64_t *imported) const
{
    if(pool)*pool=__atomic_load_n(&pool_bytes_,__ATOMIC_RELAXED);
    if(other)*other=__atomic_load_n(&other_bytes_,__ATOMIC_RELAXED);
    if(imported)*imported=__atomic_load_n(&import_bytes_,__ATOMIC_RELAXED);
}
/* Back to the free state, at the tail of the FIFO. */
void NativeAdrenoUserClient::clear(Entry &entry)
{
    if(entry.used && live_)--live_;
    index_.erase(entry.handle);
    entry.handle=0;entry.mapped=0;entry.iova=0;entry.used=false;
    entry.mapped_state=false;entry.unreleasable=false;entry.imported=false;entry.scanout=false;entry.metadata_bytes=0;
    entry.token=0;entry.mapping_leases=0;entry.freed_at=allocations_;
    fifo_[(fifo_head_+fifo_count_)%entry_count_]=(uint32_t)(&entry-entries_);++fifo_count_;
}
/* A slot whose allocation or import failed (never published): released, or kept out of circulation for good when even that fails. */
void NativeAdrenoUserClient::releaseEntry(Entry &entry)
{
    if(entry.memory.release()!=kIOReturnSuccess) {
        entry.used=true;entry.unreleasable=true;entry.handle=0;++live_;
        return;
    }
    entry.used=false;
    clear(entry);
}
size_t NativeAdrenoUserClient::liveObjects() const
{
    return live_;
}
bool NativeAdrenoUserClient::mapped(uint64_t handle)
{
    Entry *entry=find(handle);return entry && entry->mapped_state;
}
NativeAdrenoMemory *NativeAdrenoUserClient::memoryOf(uint64_t handle)
{
    Entry *entry=find(handle);return entry && !entry->unreleasable?&entry->memory:nullptr;
}
IOReturn NativeAdrenoUserClient::open(uint32_t abi_version,NativeAdrenoVm *vm,
    NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,uint32_t input_bits,uint32_t output_bits,size_t objects)
{
    if(open_ || vm_ || submit_)return kIOReturnBusy;
    if(abi_version!=NativeAdrenoAbiVersion)return kIOReturnUnsupported;
    if(!vm || !submit || !ring || input_bits<31 || input_bits>48 ||
        output_bits<32 || output_bits>48 || vm->poisoned() || submit->poisoned() ||
        ring->poisoned() || objects<NativeAdrenoClientMinObjects || objects>NativeAdrenoClientMaxObjects ||
        (objects&(objects-1)))return kIOReturnBadArgument;
    entries_=IONewData(Entry,objects);
    if(!entries_)return kIOReturnNoMemory;
    memset(static_cast<void *>(entries_),0,sizeof(Entry)*objects);
    fifo_=IONewData(uint32_t,objects);
    if(!fifo_ || !index_.allocate(objects)) {
        if(fifo_)IODeleteData(fifo_,uint32_t,objects);
        IODeleteData(entries_,Entry,objects);
        entries_=nullptr;fifo_=nullptr;
        return kIOReturnNoMemory;
    }
    entry_count_=objects;fifo_head_=fifo_count_=fresh_=live_=0;allocations_=early_reuses_=0;keep_objects_=keep_spans_=keep_pool_=0;
    __atomic_store_n(&pool_bytes_,uint64_t(0),__ATOMIC_RELAXED);__atomic_store_n(&other_bytes_,uint64_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(&import_bytes_,uint64_t(0),__ATOMIC_RELAXED);
    vm_=vm;submit_=submit;ring_=ring;input_bits_=input_bits;output_bits_=output_bits;
    next_handle_=1;open_=true;retry_fence_=0;scanout_={};source_=NativeAdrenoRingSourceExternal;
    queues_[0]={0,true};
    /* Each client is a new GPU context for the CP (the ring's context entry when the submitting client changes). */
    static uint32_t contexts;
    do context_=__atomic_add_fetch(&contexts,1,__ATOMIC_RELAXED); while(!context_);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::setReserve(size_t keep_objects,size_t keep_spans,size_t keep_pool)
{
    if(!open_)return kIOReturnNotReady;
    keep_objects_=keep_objects;keep_spans_=keep_spans;keep_pool_=keep_pool;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::setScope(uint32_t queue_base,uint64_t window_start,uint64_t window_bytes)
{
    if(!open_)return kIOReturnNotReady;
    if(queue_base%NativeAdrenoAbiMaxQueues || queue_base>=uint32_t(NativeAdrenoSubmitRanges)*uint32_t(NativeAdrenoAbiMaxQueues) ||
        (window_start&4095) || (window_bytes&4095) ||
        (window_bytes && (window_start+window_bytes<window_start || window_start+window_bytes>(UINT64_C(1)<<input_bits_))))
        return kIOReturnBadArgument;
    /* A client that owns no range yet has none to drain: its queue_base_ is the default 0, the render server's range, and the render server
     * nearly always has a job in flight, so every other process was refused here with kIOReturnBusy (int-200: WidgetRenderer_Default). */
    if(liveObjects() || ownRangeOutstanding() || submit_->rangeOutstanding(queue_base,NativeAdrenoAbiMaxQueues))return kIOReturnBusy;
    queue_base_=queue_base;scoped_=true;window_start_=window_start;window_bytes_=window_bytes;window_peak_=0;
    return kIOReturnSuccess;
}
bool NativeAdrenoUserClient::ownRangeOutstanding() const
{
    return (scoped_ || used_) && submit_->rangeOutstanding(queue_base_,NativeAdrenoAbiMaxQueues);
}
IOReturn NativeAdrenoUserClient::close()
{
    if(!open_)return kIOReturnNotReady;
    if(liveObjects() || ownRangeOutstanding())return kIOReturnBusy;
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)if(queues_[i].active)return kIOReturnBusy;
    IODeleteData(entries_,Entry,entry_count_);IODeleteData(fifo_,uint32_t,entry_count_);entry_count_=0;index_.release();
    fifo_head_=fifo_count_=fresh_=0;
    queue_base_=0;scoped_=used_=false;window_start_=window_bytes_=window_peak_=0;scanout_={};retry_fence_=0;
    keep_objects_=keep_spans_=keep_pool_=0;source_=NativeAdrenoRingSourceExternal;
    open_=false;for(auto &queue:queues_)queue=Queue{};vm_=nullptr;submit_=nullptr;ring_=nullptr;timer_=nullptr;status_=nullptr;info_=nullptr;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::releaseAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!open_)return kIOReturnNotReady;
    if(!clock)return kIOReturnBadArgument;
    IOReturn status=(scoped_ || used_)?submit_->waitRange(queue_base_,NativeAdrenoAbiMaxQueues,clock,timeout_us):kIOReturnSuccess;
    if(status)return status;
    /* Every object that can go goes; the first refusal is the answer. Objects never handed out (unreleasable, handle 0) and objects
     * whose release failed before stay, so the connection stays an orphan, but it no longer keeps everything after them. */
    IOReturn first=kIOReturnSuccess;
    for(size_t index=0;index<fresh_;++index) {
        Entry &entry=entries_[index];
        if(!entry.used)continue;
        if(entry.unreleasable || !entry.handle) {if(!first)first=kIOReturnBusy;continue;}
        entry.mapping_leases=0;
        NativeAdrenoAbiBoFree request{entry.handle};
        status=freeBo(&request);
        if(status && !first)first=status;
    }
    if(first)return first;
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)queues_[i]=Queue{};
    return close();
}
NativeAdrenoMemory *NativeAdrenoUserClient::memoryForToken(uint32_t token)
{
    if(!open_ || !token || token>entry_count_)return nullptr;
    Entry &entry=entries_[token-1];
    return entry.used && !entry.unreleasable && entry.token==token && entry.mapping_leases?&entry.memory:nullptr;
}
uint32_t NativeAdrenoUserClient::mappingLeases(uint32_t token) const
{
    if(!open_ || !token || token>entry_count_)return 0;
    const Entry &entry=entries_[token-1];
    return entry.used && entry.token==token?entry.mapping_leases:0;
}
uint32_t NativeAdrenoUserClient::tokenOf(uint64_t handle) const
{
    if(!open_)return 0;
    const Entry *entry=find(handle);
    return entry && !entry->unreleasable?entry->token:0;
}
IOReturn NativeAdrenoUserClient::query(NativeAdrenoAbiParams *params)
{
    if(!params)return kIOReturnBadArgument;
    *params={};
    if(!open_)return kIOReturnNotReady;
    params->abi_version=NativeAdrenoAbiVersion;
    params->input_bits=input_bits_;params->output_bits=output_bits_;
    params->flags=NativeAdrenoParamsHostImport;params->max_bo_bytes=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::allocBo(const NativeAdrenoAbiBoAlloc *request,
    NativeAdrenoAbiBoAllocReply *reply,const NativeAdrenoRingClock *clock)
{
    const IOReturn status=allocBoInner(request,reply,clock);
    if(status && request) {
        volatile uint32_t *stats=clientStats(queue_base_,window_bytes_);
        stats[2]=stats[2]+1;stats[3]=(uint32_t)status;stats[4]=(uint32_t)request->bytes;
    }
    if(entries_)noteLive(queue_base_,window_bytes_,liveObjects());
    return status;
}
IOReturn NativeAdrenoUserClient::zeroPooled(Entry &entry,const NativeAdrenoRingClock *clock)
{
    const bool timed=clock && clock->now_us && NativeAdrenoKoptOn(NativeAdrenoKoptMeter);
    const uint64_t began=timed?clock->now_us(clock->cookie):0;
    const IOReturn status=entry.memory.finishPooled();
    if(timed) {
        __atomic_fetch_add(&native_adreno_meter_zero[0],uint32_t(entry.memory.bytes()>>10),__ATOMIC_RELAXED);
        __atomic_fetch_add(&native_adreno_meter_zero[1],uint32_t(clock->now_us(clock->cookie)-began),__ATOMIC_RELAXED);
    }
    return status;
}
IOReturn NativeAdrenoUserClient::allocBoInner(const NativeAdrenoAbiBoAlloc *request,
    NativeAdrenoAbiBoAllocReply *reply,const NativeAdrenoRingClock *clock)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->handle=0;
    if(!open_)return kIOReturnNotReady;
    const uint64_t limit=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    if(!request->bytes || (request->bytes&4095) || request->bytes>limit ||
        request->align_log2<12 || request->align_log2>24 || request->contiguous>1)
        return kIOReturnBadArgument;
    Entry *entry=free();
    if(!entry)return kIOReturnNoResources;
    IOReturn status=kIOReturnNoResources;
    const bool zero_outside=NativeAdrenoKoptOn(NativeAdrenoKoptPoolZero);
    {
        /* The BO pool is shared by every connection: a pooled allocation runs under the Memory lock. A client with a pool reserve
         * (setReserve: every one but the render server) leaves keep_pool_ bytes of the pool free (read under the same lock, which every
         * pool allocation and release holds). */
        NativeAdrenoSubmitGuard memory(submit_,NativeAdrenoSubmitLockMemory);
        if(!keep_pool_ || NativeAdrenoMemory::poolFreeBytes()>=keep_pool_+(size_t)request->bytes) {
            status=entry->memory.allocatePooled((size_t)request->bytes,output_bits_);
            if(!status && !zero_outside)status=zeroPooled(*entry,clock);
        }
    }
    /* Outside the pool (none, full, or the render server's share) the BO is this connection's own pages: no Memory lock, so a client's
     * contiguous search, scattered pages and page attribute changes (int-152: 90-130 ms for a surface-sized BO) never hold up the
     * render server's BoAlloc/BoImport/BoFree. A scattered allocation costs one VM span per physical page (int-030: 16 MiB came back
     * as 4096 single-page spans, the whole span table), so small objects try one contiguous run first (contiguousAllowed) and fall back
     * to scattered pages unless the caller demanded contiguity. */
    if(status==kIOReturnNoResources) {
        /* Every BO that comes here: most never make a contiguous attempt, and what they cost in spans shows nowhere else. */
        __atomic_fetch_add(&native_adreno_meter_contig[2],1u,__ATOMIC_RELAXED);
        status=kIOReturnUnsupported;
        if(request->contiguous==0 && contiguousAllowed(request->bytes,clock)) {
            __atomic_fetch_add(&native_adreno_meter_contig[0],1u,__ATOMIC_RELAXED);
            status=entry->memory.allocate((size_t)request->bytes,output_bits_,true,(size_t)request->bytes);
            if(status)contiguousFailed(request->bytes,clock);
            if(status && !entry->memory.quarantined() && entry->memory.release()==kIOReturnSuccess)status=kIOReturnUnsupported;
        }
        if(status)status=entry->memory.allocate((size_t)request->bytes,output_bits_,request->contiguous!=0,(size_t)request->bytes);
    }
    /* A pooled BO's zero fill: its pages are nobody else's, so the zeros of a large BO hold up no other process's BoAlloc, BoImport or
     * BoFree (pagani-gpu-kopt bit 0; a failure releases it below, under the Memory lock). The BO is published only after this. */
    else if(!status && zero_outside)status=zeroPooled(*entry,clock);
    /* User BOs are write-combined for the CPU, as the GPU maps them non-cacheable: no per-submission cache maintenance
     * (ARCHITECTURE 4.14.1). Turnip maps every BO asking kIOMapWriteCombineCache (kIOMapDefaultCache only with CachedCoherent).
     * The object's own pages (a write-combined pool's BO only takes the flag): no Memory lock either. */
    if(!status && native_adreno_wc_enabled)status=entry->memory.makeWriteCombined();
    if(status) {
        /* A pooled object gives its pages back to the pool under the Memory lock; one from outside it needs none. */
        NativeAdrenoSubmitGuard memory(entry->memory.pooled()?submit_:nullptr,NativeAdrenoSubmitLockMemory);
        releaseEntry(*entry);
        return status;
    }
    publish(*entry);
    entry->mapped=0;entry->iova=0;entry->mapped_state=false;
    reply->handle=entry->handle;
    return kIOReturnSuccess;
}
void NativeAdrenoUserClient::setScanout(const NativeAdrenoScanoutMapping *mapping)
{
    const bool valid=mapping && mapping->task && mapping->memory && mapping->bytes && !((mapping->start|mapping->bytes)&4095) &&
        mapping->start+mapping->bytes>mapping->start;
    scanout_=valid?*mapping:NativeAdrenoScanoutMapping{};
}
bool NativeAdrenoUserClient::isScanoutAddress(void *task,uint64_t address,uint64_t bytes,uint64_t *offset) const
{
    if(offset)*offset=0;
    if(!task || task!=scanout_.task || !scanout_.memory || !bytes)return false;
    if(address<scanout_.start || address-scanout_.start>scanout_.bytes || bytes>scanout_.bytes-(address-scanout_.start))return false;
    if(offset)*offset=address-scanout_.start;
    return true;
}
IOReturn NativeAdrenoUserClient::importBo(const NativeAdrenoAbiBoImport *request,void *task,
    NativeAdrenoAbiBoAllocReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->handle=0;
    if(!open_)return kIOReturnNotReady;
    native_adreno_import_diag[0]=native_adreno_import_diag[0]+1;
    const uint64_t limit=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    if(!task || !request->address || ((request->address|request->bytes)&4095) || !request->bytes || request->bytes>limit) {
        noteImport(kIOReturnBadArgument,1,request->bytes,request->address);
        return kIOReturnBadArgument;
    }
    /* M9: the hand-off scanout lies outside the XNU memory window and has no managed pages to wire: its BO is the presenter's own
     * physical range (ARCHITECTURE 5.3.2). */
    uint64_t offset=0;
    const bool scanout=isScanoutAddress(task,request->address,request->bytes,&offset);
    Entry *entry=free();
    if(!entry) {noteImport(kIOReturnNoResources,2,request->bytes,request->address);return kIOReturnNoResources;}
    IOReturn status;
    {
        NativeAdrenoSubmitGuard memory(submit_,NativeAdrenoSubmitLockMemory);
        status=scanout?entry->memory.importDevice(scanout_.memory,offset,(size_t)request->bytes):
            entry->memory.importTask(task,request->address,(size_t)request->bytes,output_bits_,(size_t)request->bytes);
        if(status)releaseEntry(*entry);
    }
    if(status) {
        noteImport(status,scanout?5:4,request->bytes,request->address);
        return status;
    }
    publish(*entry);
    entry->mapped=0;entry->iova=0;entry->mapped_state=false;entry->imported=true;entry->scanout=scanout;
    reply->handle=entry->handle;
    native_adreno_import_live[0]=native_adreno_import_live[0]+1;
    {
        const unsigned slot=3+(gImportNext++%8)*4;
        native_adreno_import_live[slot]=request->address;native_adreno_import_live[slot+1]=(uint64_t)proc_selfpid()<<40|request->bytes;
        native_adreno_import_live[slot+2]=entry->handle;native_adreno_import_live[slot+3]=scanout?4:1;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::mapBo(const NativeAdrenoAbiBoMap *request,
    NativeAdrenoAbiBoMapReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->iova=0;
    if(!open_)return kIOReturnNotReady;
    if(!(request->permissions&1) || (request->permissions&~3u) || request->writeback>1 ||
        (request->shareability!=2 && request->shareability!=3) || request->reserved ||
        (request->iova&4095))return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(entry->mapped_state)return kIOReturnBusy;
    const uint64_t end=request->iova+entry->memory.bytes();
    if(window_bytes_ && (request->iova<window_start_ || end<request->iova || end>window_start_+window_bytes_)) {
        /* Per window: how often a mapping fell outside it, and the last one's size and end (MiB). */
        const uint32_t slot=queue_base_/NativeAdrenoAbiMaxQueues;
        if(slot<NativeAdrenoSubmitRanges) {
            native_adreno_window_overflow[slot][0]=native_adreno_window_overflow[slot][0]+1;
            native_adreno_window_overflow[slot][1]=uint32_t(entry->memory.bytes()>>10);
            native_adreno_window_overflow[slot][2]=uint32_t(end>>20);
        }
        return kIOReturnBadArgument;
    }
    /* The scanout is a GPU write target (the hand-off copy): it must be mapped writable; the VM maps it non-cacheable. */
    if(entry->scanout && !(request->permissions&2u))return kIOReturnBadArgument;
    /* Memory the owner maps read-only could only be pinned for reading: the GPU gets the read half of what was asked. */
    const unsigned permissions=entry->memory.readOnly()?(request->permissions&~2u):request->permissions;
    /* The VM chooses the attributes of ARCHITECTURE 4.14.1 for this memory at the current coherence level (NativeAdrenoVm::attributes):
     * GPU-read-only imports write-back inner-shareable from level 1, GPU-written imports and write-back requests for own BOs from
     * level 2, the scanout and everything else non-cacheable. */
    uint64_t handle=0;
    const IOReturn status=vm_->mapClient(&entry->memory,request->iova,permissions,request->writeback,request->shareability,&handle,
        keep_objects_,keep_spans_);
    if(status)return status;
    entry->mapped=handle;entry->iova=request->iova;entry->mapped_state=true;
    if(end>window_peak_)window_peak_=end;
    if(window_bytes_ && window_peak_>window_start_)clientStats(queue_base_,window_bytes_)[5]=(uint32_t)((window_peak_-window_start_)>>20);
    reply->iova=request->iova;
    return kIOReturnSuccess;
}
static void noteFreeRefused(bool imported,uint64_t handle,IOReturn status)
{
    if(!imported)return;
    native_adreno_import_live[1]=native_adreno_import_live[1]+1;native_adreno_import_live[2]=(uint32_t)status;noteImportState(handle,3);
}
IOReturn NativeAdrenoUserClient::freeBo(const NativeAdrenoAbiBoFree *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    Entry *entry=find(request->handle);
    if(!entry)return kIOReturnBadArgument;
    if(entry->unreleasable || entry->mapping_leases) {noteFreeRefused(entry->imported,entry->handle,kIOReturnBusy);return kIOReturnBusy;}
    if(entry->mapped_state) {
        /* Completed jobs must not pin the buffer: retire what the CP has passed before the unmap decides (one fence read; a
         * poisoned execbuf refuses and the unmap below stays refused too). */
        submit_->retireCompleted();
        const IOReturn unmap=vm_->unmap(entry->mapped);   /* refused while a job holds it or its last use is not complete */
        if(unmap) {noteFreeRefused(entry->imported,entry->handle,unmap);return unmap;}
        entry->mapped_state=false;entry->mapped=0;entry->iova=0;
    }
    IOReturn release;
    account(*entry,false);   /* while the backing object still describes itself */
    {
        /* A pooled BO goes back to the pool and an import is unwired under the Memory lock; an own BO from outside the pool takes its
         * pages' default attribute back without it (allocBoInner). */
        NativeAdrenoSubmitGuard memory(entry->memory.pooled() || entry->imported?submit_:nullptr,NativeAdrenoSubmitLockMemory);
        release=entry->memory.release();
    }
    if(release) {account(*entry,true);noteFreeRefused(entry->imported,entry->handle,release);return release;}
    if(entry->imported) {native_adreno_import_live[0]=native_adreno_import_live[0]-1;noteImportState(entry->handle,2);}
    clear(*entry);
    noteLive(queue_base_,window_bytes_,liveObjects());
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::validateWaitFence(uint64_t fence) const
{
    if(!fence)return kIOReturnSuccess;
    if(!open_)return kIOReturnNotReady;
    return submit_->issued(fence)?kIOReturnSuccess:kIOReturnBadArgument;
}
IOReturn NativeAdrenoUserClient::submit(const NativeAdrenoAbiSubmit *request,
    NativeAdrenoAbiSubmitReply *reply,const NativeAdrenoRingClock *)
{
    if(!request || !reply)return kIOReturnBadArgument;
    reply->fence=0;retry_fence_=0;
    if(!open_)return kIOReturnNotReady;
    if(!request->buffer_count || request->buffer_count>NativeAdrenoAbiMaxBuffers ||
        request->reserved)return kIOReturnBadArgument;
    if(request->queue>=NativeAdrenoAbiMaxQueues || !queues_[request->queue].active)
        return kIOReturnBadArgument;
    /* No CPU wait for an earlier fence (ARCHITECTURE 4.13.3): the one ring completes in order and every framed submission starts with
     * the BR/BV sync and a cache flush, so an issued fence is already satisfied by ring order. Only a fence never issued is refused. */
    const IOReturn waitable=validateWaitFence(request->wait_fence);
    if(waitable)return waitable;
    NativeAdrenoSubmitBuffer *buffers=submit_buffers_;
    for(uint32_t i=0;i<request->buffer_count;++i) {
        const NativeAdrenoAbiSubmitBuffer &wire=request->buffers[i];
        if(!wire.bytes || (wire.bytes&3))return kIOReturnBadArgument;
        Entry *entry=find(wire.handle);
        if(!entry || !entry->mapped_state)return kIOReturnBadArgument;
        buffers[i]={entry->mapped,wire.iova,(size_t)wire.bytes};
    }
    uint32_t fence=0,retry=0;
    used_=true;
    const IOReturn status=submit_->submit(buffers,request->buffer_count,queue_base_+static_cast<uint32_t>(request->queue),&fence,&retry,
        context_,source_);
    if(status==NativeAdrenoSubmitRetry)retry_fence_=retry;
    if(status)return status;
    reply->fence=fence;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::attachTimer(const NativeAdrenoTimer *timer)
{
    if(!open_)return kIOReturnNotReady;
    if(!timer || timer_ )return kIOReturnBadArgument;
    timer_=timer;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::timestamp(NativeAdrenoAbiTimestamp *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!timer_)return kIOReturnUnsupported;
    return timer_->read(&reply->ticks);
}
IOReturn NativeAdrenoUserClient::attachDeviceInfo(const NativeAdrenoAbiDeviceInfo *info)
{
    if(!open_)return kIOReturnNotReady;
    if(!info || info_ || info->reserved || !info->chip_id || !info->va_size)return kIOReturnBadArgument;
    info_=info;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::deviceInfo(NativeAdrenoAbiDeviceInfo *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!info_)return kIOReturnUnsupported;
    *reply=*info_;
    if(window_bytes_){reply->va_start=window_start_;reply->va_size=window_bytes_;}
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::attachStatus(const NativeAdrenoStatus *status)
{
    if(!open_)return kIOReturnNotReady;
    if(!status || status_)return kIOReturnBadArgument;
    status_=status;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::status(NativeAdrenoAbiStatus *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!status_)return kIOReturnUnsupported;
    const NativeAdrenoStatusCounts counts=status_->counts();
    reply->faults=counts.faults;reply->suspends=counts.suspends;
    return kIOReturnSuccess;
}
size_t NativeAdrenoUserClient::inputSize(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodQuery:
    case NativeAdrenoMethodGetTimestamp:
    case NativeAdrenoMethodGetStatus:
    case NativeAdrenoMethodGetDeviceInfo:return 0;
    case NativeAdrenoMethodBoAlloc:return sizeof(NativeAdrenoAbiBoAlloc);
    case NativeAdrenoMethodBoMap:return sizeof(NativeAdrenoAbiBoMap);
    case NativeAdrenoMethodBoImport:return sizeof(NativeAdrenoAbiBoImport);
    case NativeAdrenoMethodBoFree:return sizeof(NativeAdrenoAbiBoFree);
    case NativeAdrenoMethodSubmit:return sizeof(NativeAdrenoAbiSubmit);
    case NativeAdrenoMethodSetMetadata:
    case NativeAdrenoMethodGetMetadata:return sizeof(NativeAdrenoAbiMetadata);
    case NativeAdrenoMethodCreateQueue:
    case NativeAdrenoMethodCloseQueue:return sizeof(NativeAdrenoAbiQueue);
    case NativeAdrenoMethodBoCpuMap:return sizeof(NativeAdrenoAbiBoCpuMap);
    case NativeAdrenoMethodBoCpuUnmap:return sizeof(NativeAdrenoAbiBoCpuUnmap);
    case NativeAdrenoMethodWaitFence:return sizeof(NativeAdrenoAbiWaitFence);
    default:return SIZE_MAX;
    }
}
size_t NativeAdrenoUserClient::outputSize(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodQuery:return sizeof(NativeAdrenoAbiParams);
    case NativeAdrenoMethodBoAlloc:return sizeof(NativeAdrenoAbiBoAllocReply);
    case NativeAdrenoMethodBoMap:return sizeof(NativeAdrenoAbiBoMapReply);
    case NativeAdrenoMethodBoFree:return 0;
    case NativeAdrenoMethodSubmit:return sizeof(NativeAdrenoAbiSubmitReply);
    case NativeAdrenoMethodWaitFence:return 0;
    case NativeAdrenoMethodGetTimestamp:return sizeof(NativeAdrenoAbiTimestamp);
    case NativeAdrenoMethodGetStatus:return sizeof(NativeAdrenoAbiStatus);
    case NativeAdrenoMethodGetDeviceInfo:return sizeof(NativeAdrenoAbiDeviceInfo);
    case NativeAdrenoMethodGetMetadata:return sizeof(NativeAdrenoAbiMetadata);
    case NativeAdrenoMethodSetMetadata:return 0;
    case NativeAdrenoMethodCreateQueue:return sizeof(NativeAdrenoAbiQueueReply);
    case NativeAdrenoMethodCloseQueue:return 0;
    case NativeAdrenoMethodBoCpuMap:return sizeof(NativeAdrenoAbiBoCpuMapReply);
    case NativeAdrenoMethodBoCpuUnmap:return 0;
    case NativeAdrenoMethodBoImport:return sizeof(NativeAdrenoAbiBoAllocReply);
    default:return SIZE_MAX;
    }
}
IOReturn NativeAdrenoUserClient::dispatch(uint32_t method,const void *input,
    size_t input_size,void *output,size_t output_size,const NativeAdrenoRingClock *clock)
{
    const size_t expected_in=inputSize(method),expected_out=outputSize(method);
    if(expected_in==SIZE_MAX || expected_out==SIZE_MAX)return kIOReturnUnsupported;
    IOReturn status=kIOReturnSuccess;
    if(input_size!=expected_in || output_size!=expected_out)status=kIOReturnBadArgument;
    else if((expected_in && !input) || (expected_out && !output))status=kIOReturnBadArgument;
    else switch(method) {
    case NativeAdrenoMethodQuery: {
        NativeAdrenoAbiParams reply{};
        status=query(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoAlloc: {
        NativeAdrenoAbiBoAlloc request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoAllocReply reply{};
        status=allocBo(&request,&reply,clock);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoImport:
        status=kIOReturnUnsupported;   /* needs the calling task: the IOKit wrapper calls importBo itself */
        break;
    case NativeAdrenoMethodBoMap: {
        NativeAdrenoAbiBoMap request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoMapReply reply{};
        status=mapBo(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        else {
            /* Why maps fail (int-135: 55 of 191, and the translator answers each as a device out-of-memory). */
            unsigned at=0;while(at<4 && native_adreno_map_fail[at][0] && native_adreno_map_fail[at][0]!=(uint32_t)status)++at;
            native_adreno_map_fail[at][0]=(uint32_t)status;native_adreno_map_fail[at][1]=native_adreno_map_fail[at][1]+1;
            native_adreno_map_fail[5][0]=(uint32_t)(request.iova>>12);native_adreno_map_fail[5][1]=(uint32_t)(request.handle);
        }
        break;
    }
    case NativeAdrenoMethodBoFree: {
        NativeAdrenoAbiBoFree request{};
        memcpy(&request,input,sizeof(request));
        status=freeBo(&request);
        break;
    }
    case NativeAdrenoMethodSubmit: {
        NativeAdrenoAbiSubmit &request=submit_request_;
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiSubmitReply reply{};
        status=submit(&request,&reply,clock);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodWaitFence: {
        NativeAdrenoAbiWaitFence request{};
        memcpy(&request,input,sizeof(request));
        status=waitFence(&request,clock);
        break;
    }
    case NativeAdrenoMethodGetDeviceInfo: {
        NativeAdrenoAbiDeviceInfo reply{};
        status=deviceInfo(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodGetTimestamp: {
        NativeAdrenoAbiTimestamp reply{};
        status=timestamp(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoCpuMap: {
        NativeAdrenoAbiBoCpuMap request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoCpuMapReply reply{};
        status=cpuMap(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoCpuUnmap: {
        NativeAdrenoAbiBoCpuUnmap request{};
        memcpy(&request,input,sizeof(request));
        status=cpuUnmap(&request);
        break;
    }
    case NativeAdrenoMethodCreateQueue: {
        NativeAdrenoAbiQueueReply reply{};
        status=createQueue(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodCloseQueue: {
        NativeAdrenoAbiQueue request{};
        memcpy(&request,input,sizeof(request));
        status=closeQueue(&request);
        break;
    }
    case NativeAdrenoMethodSetMetadata: {
        NativeAdrenoAbiMetadata request{};
        memcpy(&request,input,sizeof(request));
        status=setMetadata(&request);
        break;
    }
    case NativeAdrenoMethodGetMetadata: {
        NativeAdrenoAbiMetadata request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiMetadata reply{};
        status=getMetadata(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodGetStatus: {
        NativeAdrenoAbiStatus reply{};
        status=this->status(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    default:return kIOReturnUnsupported;
    }
    if(method<20) {
        native_adreno_method_stats[method][0]=native_adreno_method_stats[method][0]+1;
        if(status)native_adreno_method_stats[method][1]=native_adreno_method_stats[method][1]+1;
        native_adreno_method_stats[method][2]=(uint32_t)status;
    }
    return status;
}
IOReturn NativeAdrenoUserClient::cpuMap(const NativeAdrenoAbiBoCpuMap *request,
    NativeAdrenoAbiBoCpuMapReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    /* An imported BO (and the scanout) is the caller's own memory, already mapped where it came from. */
    if(entry->memory.imported() || entry->imported)return kIOReturnUnsupported;
    if(entry->mapping_leases==UINT32_MAX)return kIOReturnNoResources;
    ++entry->mapping_leases;
    reply->token=entry->token;reply->bytes=entry->memory.bytes();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::cpuUnmap(const NativeAdrenoAbiBoCpuUnmap *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(request->reserved)return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable || !entry->token)return kIOReturnBadArgument;
    if(request->token!=entry->token)return kIOReturnBadArgument;
    if(!entry->mapping_leases)return kIOReturnBusy;
    --entry->mapping_leases;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::createQueue(NativeAdrenoAbiQueueReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)if(!queues_[i].active) {
        queues_[i]={static_cast<uint32_t>(i),true};
        reply->queue=i;
        return kIOReturnSuccess;
    }
    return kIOReturnNoResources;
}
IOReturn NativeAdrenoUserClient::closeQueue(const NativeAdrenoAbiQueue *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(!request->queue || request->queue>=NativeAdrenoAbiMaxQueues)return kIOReturnBadArgument;
    Queue &queue=queues_[request->queue];
    if(!queue.active)return kIOReturnBadArgument;
    /* A queue must be drained by the caller before it disappears. */
    if(submit_->queueOutstanding(queue_base_+static_cast<uint32_t>(request->queue)))
        return kIOReturnBusy;
    queue={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::setMetadata(const NativeAdrenoAbiMetadata *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(request->reserved || request->bytes>NativeAdrenoAbiMetadataBytes)return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(request->bytes)memcpy(entry->metadata,request->text,request->bytes);
    entry->metadata_bytes=request->bytes;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::getMetadata(const NativeAdrenoAbiMetadata *request,
    NativeAdrenoAbiMetadata *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;
    /* Snapshot the request so an in/out alias is still well defined. */
    const uint64_t handle=request->handle;
    const uint32_t asked=request->bytes;
    const uint32_t reserved=request->reserved;
    *reply={};
    if(!open_)return kIOReturnNotReady;
    if(reserved || asked>NativeAdrenoAbiMetadataBytes)return kIOReturnBadArgument;
    Entry *entry=find(handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(asked<entry->metadata_bytes)return kIOReturnBadArgument;
    reply->handle=entry->handle;reply->bytes=entry->metadata_bytes;
    if(entry->metadata_bytes)memcpy(reply->text,entry->metadata,entry->metadata_bytes);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::waitFence(const NativeAdrenoAbiWaitFence *request,
    const NativeAdrenoRingClock *clock)
{
    if(!request || !clock)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(!request->fence || request->fence>UINT32_MAX)return kIOReturnBadArgument;
    const uint32_t timeout_us=request->timeout_ns==0?0:
        (request->timeout_ns>UINT64_C(0xffffffff)*1000u?0xffffffffu:
        (uint32_t)(request->timeout_ns/1000));
    return submit_->wait((uint32_t)request->fence,clock,timeout_us);
}
