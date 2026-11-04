/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_METER_H
#define NATIVE_ADRENO_METER_H
#include "Adreno830Status.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoKopt.h"
#include "NativeAdrenoRing.h"

/* The call meter (ARCHITECTURE 8.8, pagani-gpu-kopt bit 4; the idea of upstream Adreno740 59171b2, which records every call into a ring
 * for an export this kernel does not have): what the jitter analysis lacks is how long the calls took and who waited for which lock.
 * So only that is kept: the longest call of each kind and the longest wait for each lock since the last tick, in microseconds, plus
 * a few counts. Relaxed atomics, a clock read per call of the five kinds below and none at all for a lock taken at once; nothing is
 * formatted here. The publish shell's tick takes the figures once a second (NativeAdrenoMeterTake) and the status block folds them
 * into the meter line's window. Defined in NativeAdrenoMemory.cpp, which everything that writes them links. */
enum : unsigned {NativeAdrenoVmKinds=5};
extern "C" {
extern volatile uint32_t native_adreno_meter_calls[Adreno830CallKinds];   /* the longest call of each kind, us */
extern volatile uint32_t native_adreno_meter_waits[Adreno830WaitKinds];   /* the longest wait for each lock, us */
extern volatile uint32_t native_adreno_meter_zero[2];                     /* pooled BOs zeroed: [0] KiB, [1] us */
extern volatile uint32_t native_adreno_meter_contig[3];                   /* outside the pool: contiguous attempts [0] made, [1] failed, [2] BOs asked for */
extern volatile uint32_t native_adreno_meter_fence;                       /* the longest WaitFence as its caller saw it, us */
extern volatile uint64_t native_adreno_vm_kind_us[NativeAdrenoVmKinds][3];   /* NativeAdrenoVm.cpp: {count, us, spans} per kind of table transaction */
extern uint64_t (*native_adreno_vm_now_us)(void);                         /* NativeAdrenoVm.cpp: the VM's clock, null while pagani-gpu-kopt bit 5 is off */
}
/* Raises a longest-so-far figure. */
static inline void NativeAdrenoMeterRaise(volatile uint32_t *slot,uint64_t us)
{
    const uint32_t value=us<UINT32_MAX?uint32_t(us):UINT32_MAX;
    uint32_t seen=__atomic_load_n(slot,__ATOMIC_RELAXED);
    while(value>seen && !__atomic_compare_exchange_n(slot,&seen,value,true,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
/* The meter line's kind of an ABI method; Adreno830CallKinds for one it does not time (a fence wait's length is the GPU's). */
static inline unsigned NativeAdrenoMeterKindOf(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodBoAlloc:return Adreno830CallAlloc;
    case NativeAdrenoMethodBoMap:return Adreno830CallMap;
    case NativeAdrenoMethodBoFree:return Adreno830CallFree;
    case NativeAdrenoMethodBoImport:return Adreno830CallImport;
    case NativeAdrenoMethodSubmit:return Adreno830CallSubmit;
    default:return Adreno830CallKinds;
    }
}
/* The per-second line's class of an ABI method (Adreno830Class*): every method has one. */
static inline unsigned NativeAdrenoCallClassOf(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodBoAlloc:return Adreno830ClassAlloc;
    case NativeAdrenoMethodBoMap:return Adreno830ClassMap;
    case NativeAdrenoMethodBoFree:return Adreno830ClassFree;
    case NativeAdrenoMethodBoImport:return Adreno830ClassImport;
    case NativeAdrenoMethodSubmit:return Adreno830ClassSubmit;
    case NativeAdrenoMethodWaitFence:return Adreno830ClassWait;
    case NativeAdrenoMethodBoCpuMap:
    case NativeAdrenoMethodBoCpuUnmap:return Adreno830ClassCpu;
    default:return Adreno830ClassOther;
    }
}
/* Times one call of a metered kind from here to the end of the scope; nothing without a clock, for another kind or with the meter off. */
class NativeAdrenoCallTimer final {
    const NativeAdrenoRingClock *clock_;
    unsigned kind_;
    uint64_t began_=0;
public:
    NativeAdrenoCallTimer(const NativeAdrenoRingClock *clock,unsigned kind):
        clock_(kind<Adreno830CallKinds && clock && clock->now_us && NativeAdrenoKoptOn(NativeAdrenoKoptMeter)?clock:nullptr),kind_(kind)
    {if(clock_)began_=clock_->now_us(clock_->cookie);}
    ~NativeAdrenoCallTimer() {if(clock_)NativeAdrenoMeterRaise(&native_adreno_meter_calls[kind_],clock_->now_us(clock_->cookie)-began_);}
    NativeAdrenoCallTimer(const NativeAdrenoCallTimer &)=delete;
    NativeAdrenoCallTimer &operator=(const NativeAdrenoCallTimer &)=delete;
};
/* Takes a lock through its try (attempt) and, only when that fails, through take with a clock read on each side (now null or the
 * meter off: take alone), the wait going to the longest of its kind: an uncontended lock costs no clock read. */
template<class Attempt,class Take>
static inline void NativeAdrenoMeterLock(unsigned which,uint64_t (*now)(void),Attempt attempt,Take take)
{
    if(!now || !NativeAdrenoKoptOn(NativeAdrenoKoptMeter)) {take();return;}
    if(attempt())return;
    const uint64_t began=now();
    take();
    NativeAdrenoMeterRaise(&native_adreno_meter_waits[which],now()-began);
}
/* Everything since the last call (one caller: the tick), the VM's kinds as differences from seen, its copy of the running counters
 * (written under the VM lock, read here without it: a figure may land in the next period, none is lost). */
static inline void NativeAdrenoMeterTake(Adreno830StatusCalls *out,uint64_t seen[NativeAdrenoVmKinds][2])
{
    for(unsigned i=0;i<Adreno830CallKinds;++i)out->max_us[i]=__atomic_exchange_n(&native_adreno_meter_calls[i],0u,__ATOMIC_RELAXED);
    for(unsigned i=0;i<Adreno830WaitKinds;++i)out->wait_us[i]=__atomic_exchange_n(&native_adreno_meter_waits[i],0u,__ATOMIC_RELAXED);
    out->zero_kib=__atomic_exchange_n(&native_adreno_meter_zero[0],0u,__ATOMIC_RELAXED);
    out->zero_us=__atomic_exchange_n(&native_adreno_meter_zero[1],0u,__ATOMIC_RELAXED);
    out->contig_tries=__atomic_exchange_n(&native_adreno_meter_contig[0],0u,__ATOMIC_RELAXED);
    out->contig_fails=__atomic_exchange_n(&native_adreno_meter_contig[1],0u,__ATOMIC_RELAXED);
    out->outside_bos=__atomic_exchange_n(&native_adreno_meter_contig[2],0u,__ATOMIC_RELAXED);
    out->fence_us=__atomic_exchange_n(&native_adreno_meter_fence,0u,__ATOMIC_RELAXED);
    uint64_t kinds[NativeAdrenoVmKinds][2];
    for(unsigned kind=0;kind<NativeAdrenoVmKinds;++kind)for(unsigned word=0;word<2;++word) {
        const uint64_t now=native_adreno_vm_kind_us[kind][word];
        kinds[kind][word]=now-seen[kind][word];seen[kind][word]=now;
    }
    /* Kinds: 0 map of driver memory, 1 import map without preflight's alias pass, 2 with it, 3 and 4 unmaps. */
    out->import_maps=uint32_t(kinds[1][0]+kinds[2][0]);out->shared_maps=uint32_t(kinds[2][0]);out->shared_us=uint32_t(kinds[2][1]);
    out->map_us=uint32_t(kinds[0][1]+kinds[1][1]+kinds[2][1]);out->unmap_us=uint32_t(kinds[3][1]+kinds[4][1]);
}
#endif
