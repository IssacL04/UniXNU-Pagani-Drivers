/* SPDX-License-Identifier: MIT */
/* The call meter's own logic (NativeAdrenoMeter.h, ARCHITECTURE 8.8; pagani-gpu-kopt bit 4): which methods are timed, the timer's scope, the
 * wait of a lock only when it was contended, longest-so-far figures under concurrent writers, and the tick taking everything once,
 * the VM's running counters as differences. The figures are defined here; on the device in NativeAdrenoMemory.cpp and
 * NativeAdrenoVm.cpp. No kernel code runs. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoMeter.h"
#include <cstdio>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
extern "C" {
volatile uint32_t native_adreno_kopt=NativeAdrenoKoptAll;
volatile uint32_t native_adreno_meter_calls[Adreno830CallKinds],native_adreno_meter_waits[Adreno830WaitKinds];
volatile uint32_t native_adreno_meter_zero[2],native_adreno_meter_contig[3],native_adreno_meter_fence;
volatile uint64_t native_adreno_vm_kind_us[NativeAdrenoVmKinds][3];
uint64_t (*native_adreno_vm_now_us)(void)=nullptr;
}
static uint64_t ticks;
static unsigned reads;
static uint64_t readClock(void *cookie) {CHECK(cookie==&ticks);++reads;return ticks;}
static uint64_t plainClock() {++reads;return ticks+=7;}
static unsigned kinds()
{
    unsigned checks=0;
    const struct {uint32_t method;unsigned kind;} table[]={
        {NativeAdrenoMethodBoAlloc,Adreno830CallAlloc},{NativeAdrenoMethodBoMap,Adreno830CallMap},{NativeAdrenoMethodBoFree,Adreno830CallFree},
        {NativeAdrenoMethodBoImport,Adreno830CallImport},{NativeAdrenoMethodSubmit,Adreno830CallSubmit},
        {NativeAdrenoMethodWaitFence,Adreno830CallKinds},{NativeAdrenoMethodQuery,Adreno830CallKinds},{NativeAdrenoMethodBoCpuMap,Adreno830CallKinds},
        {0,Adreno830CallKinds},{0xffffffffu,Adreno830CallKinds}};
    for(const auto &row:table) {CHECK(NativeAdrenoMeterKindOf(row.method)==row.kind);++checks;}
    return checks;
}
static unsigned timers()
{
    unsigned checks=0;
    NativeAdrenoRingClock clock{&ticks,readClock,nullptr};
    {reads=0;ticks=1000;NativeAdrenoCallTimer timer(&clock,Adreno830CallMap);CHECK(reads==1);ticks=1250;}
    CHECK(reads==2 && native_adreno_meter_calls[Adreno830CallMap]==250);++checks;
    {NativeAdrenoCallTimer timer(&clock,Adreno830CallMap);ticks+=100;}        /* shorter: stays */
    CHECK(native_adreno_meter_calls[Adreno830CallMap]==250);++checks;
    {NativeAdrenoCallTimer timer(&clock,Adreno830CallMap);ticks+=300;}        /* longer: raised */
    CHECK(native_adreno_meter_calls[Adreno830CallMap]==300);++checks;
    /* Nothing without a clock, for a kind the meter does not time, or with the meter off: no read of the clock either. */
    reads=0;
    {NativeAdrenoCallTimer timer(nullptr,Adreno830CallAlloc);}
    {NativeAdrenoRingClock none{&ticks,nullptr,nullptr};NativeAdrenoCallTimer timer(&none,Adreno830CallAlloc);}
    {NativeAdrenoCallTimer timer(&clock,Adreno830CallKinds);ticks+=5000;}
    native_adreno_kopt=NativeAdrenoKoptAll&~NativeAdrenoKoptMeter;
    {NativeAdrenoCallTimer timer(&clock,Adreno830CallAlloc);ticks+=5000;}
    native_adreno_kopt=NativeAdrenoKoptAll;
    CHECK(!reads && !native_adreno_meter_calls[Adreno830CallAlloc]);checks+=4;
    native_adreno_meter_calls[Adreno830CallMap]=0;
    return checks;
}
static unsigned locks()
{
    unsigned checks=0;
    unsigned attempts=0,takes=0;
    auto taken=[&](bool free) {
        attempts=takes=0;reads=0;
        NativeAdrenoMeterLock(Adreno830WaitRing,plainClock,[&] {++attempts;return free;},[&] {++takes;});
    };
    taken(true);   /* free: taken by the try, no clock */
    CHECK(attempts==1 && !takes && !reads && !native_adreno_meter_waits[Adreno830WaitRing]);++checks;
    taken(false);  /* contended: two clock reads around the blocking take */
    CHECK(attempts==1 && takes==1 && reads==2 && native_adreno_meter_waits[Adreno830WaitRing]==7);++checks;
    /* The meter off, or no clock: the plain lock, as before. */
    native_adreno_kopt=NativeAdrenoKoptAll&~NativeAdrenoKoptMeter;
    native_adreno_meter_waits[Adreno830WaitRing]=0;
    taken(false);
    CHECK(!attempts && takes==1 && !reads && !native_adreno_meter_waits[Adreno830WaitRing]);++checks;
    native_adreno_kopt=NativeAdrenoKoptAll;
    attempts=takes=0;
    NativeAdrenoMeterLock(Adreno830WaitVm,nullptr,[&] {++attempts;return false;},[&] {++takes;});
    CHECK(!attempts && takes==1 && !native_adreno_meter_waits[Adreno830WaitVm]);++checks;
    return checks;
}
static unsigned raises()
{
    /* Eight writers racing the same figure: the largest value survives. */
    volatile uint32_t figure=0;
    std::vector<std::thread> writers;
    for(unsigned t=0;t<8;++t)writers.emplace_back([&,t] {
        for(uint32_t i=0;i<20000;++i)NativeAdrenoMeterRaise(&figure,i*8+t);
    });
    for(auto &writer:writers)writer.join();
    CHECK(figure==19999*8+7);
    /* A time past 32 bits saturates. */
    volatile uint32_t wide=0;
    NativeAdrenoMeterRaise(&wide,UINT64_C(1)<<40);CHECK(wide==UINT32_MAX);
    return 2;
}
/* The per-second line's classes: every method has one, the two CPU methods share theirs, the rest (timestamps, status, metadata, queues)
 * go to the last. */
static unsigned classes()
{
    const struct {uint32_t method;unsigned which;} table[]={
        {NativeAdrenoMethodBoAlloc,Adreno830ClassAlloc},{NativeAdrenoMethodBoMap,Adreno830ClassMap},{NativeAdrenoMethodBoFree,Adreno830ClassFree},
        {NativeAdrenoMethodBoImport,Adreno830ClassImport},{NativeAdrenoMethodSubmit,Adreno830ClassSubmit},
        {NativeAdrenoMethodWaitFence,Adreno830ClassWait},{NativeAdrenoMethodBoCpuMap,Adreno830ClassCpu},
        {NativeAdrenoMethodBoCpuUnmap,Adreno830ClassCpu},{NativeAdrenoMethodGetTimestamp,Adreno830ClassOther},
        {NativeAdrenoMethodGetStatus,Adreno830ClassOther},{NativeAdrenoMethodQuery,Adreno830ClassOther},{0,Adreno830ClassOther}};
    unsigned checks=0;
    for(const auto &row:table) {CHECK(NativeAdrenoCallClassOf(row.method)==row.which);++checks;}
    return checks;
}
static unsigned takes()
{
    unsigned checks=0;
    Adreno830StatusCalls calls{};uint64_t seen[NativeAdrenoVmKinds][2]={};
    native_adreno_meter_calls[Adreno830CallFree]=11;native_adreno_meter_waits[Adreno830WaitMemory]=22;
    native_adreno_meter_zero[0]=33;native_adreno_meter_zero[1]=44;native_adreno_meter_contig[0]=5;native_adreno_meter_contig[1]=4;native_adreno_meter_contig[2]=9;
    native_adreno_vm_kind_us[0][0]=10;native_adreno_vm_kind_us[0][1]=100;
    native_adreno_vm_kind_us[1][0]=20;native_adreno_vm_kind_us[1][1]=200;
    native_adreno_vm_kind_us[2][0]=3;native_adreno_vm_kind_us[2][1]=300;
    native_adreno_vm_kind_us[3][0]=1;native_adreno_vm_kind_us[3][1]=40;
    native_adreno_vm_kind_us[4][0]=1;native_adreno_vm_kind_us[4][1]=50;
    native_adreno_meter_fence=77;
    NativeAdrenoMeterTake(&calls,seen);
    CHECK(calls.fence_us==77 && !native_adreno_meter_fence);++checks;
    CHECK(calls.max_us[Adreno830CallFree]==11 && calls.wait_us[Adreno830WaitMemory]==22 && calls.zero_kib==33 && calls.zero_us==44 &&
          calls.contig_tries==5 && calls.contig_fails==4 && calls.outside_bos==9 && calls.import_maps==23 && calls.shared_maps==3 && calls.shared_us==300 &&
          calls.map_us==600 && calls.unmap_us==90);++checks;
    /* The figures are gone after the take; the VM's counters keep running and come as differences. */
    CHECK(!native_adreno_meter_calls[Adreno830CallFree] && !native_adreno_meter_waits[Adreno830WaitMemory] && !native_adreno_meter_zero[0] &&
          !native_adreno_meter_zero[1] && !native_adreno_meter_contig[0] && !native_adreno_meter_contig[1] && !native_adreno_meter_contig[2] && native_adreno_vm_kind_us[1][0]==20);++checks;
    native_adreno_vm_kind_us[2][0]=5;native_adreno_vm_kind_us[2][1]=500;native_adreno_vm_kind_us[4][1]=51;
    NativeAdrenoMeterTake(&calls,seen);
    CHECK(!calls.max_us[Adreno830CallFree] && !calls.zero_kib && calls.import_maps==2 && calls.shared_maps==2 && calls.shared_us==200 &&
          calls.map_us==200 && calls.unmap_us==1);++checks;
    NativeAdrenoMeterTake(&calls,seen);
    CHECK(!calls.import_maps && !calls.map_us && !calls.unmap_us);++checks;
    return checks;
}
int main()
{
    const unsigned methods=kinds(),classed=classes(),scopes=timers(),waits=locks(),races=raises(),ticksTaken=takes();
    std::printf("PASS: call meter: %u method kinds (a fence wait is none), %u call classes, %u timer scopes (no clock read without a clock, for another kind or with "
                "the meter off), %u lock paths (a free lock reads no clock), %u longest-so-far races, %u ticks (everything once, the VM's counters as "
                "differences)\n",methods,classed,scopes,waits,races,ticksTaken);
}
