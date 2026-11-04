/* SPDX-License-Identifier: MIT */
/* The kernel half of the NativeAdreno stack on the host: the real GPU context bring-up (a permissive software SMMU programmer over a
 * register array), VM, ring, submit object and user-client core over the authored memory doubles, with a software "CP" that retires
 * every submission at once. It runs no shader and touches no hardware; it exists so the Turnip darwin backend can be driven through
 * a real Vulkan instance before the userspace side ever runs on the device. Narrow C interface for the IOKit shim translation unit.
 * HOSTSIM_GPU=a830 (IMPLEMENTATION_PLAN 2.2, docs/inputs/userspace/patches/adreno830-hostsim-a830.diff) selects the A830 device
 * facts of ARCHITECTURE 6.4 {0x44050001, 12 MiB GMEM at 0x4000000, UCHE trap 0x1FFFFFFFFF000, VA 0x20000000 + 0xE0000000, HBB 16,
 * swizzle 6, macrotile 1, UBWC 5, ClientIova}, the 4 GiB aperture with slot 0's window (0x20000000 + 0x60000000, as the kernel
 * gives the render server), IDR2.OAS 36 and the GMU always-on counter; anything else (the default) keeps the A740 facts and the
 * 2 GiB aperture, so the a740 run is unchanged. Submissions take the kernel's path: a caller with nothing in flight whose
 * submission finds the ring full sleeps (here without the service lock) and submits again, never seeing the internal retry. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoTimer.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>

IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state) {*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *,size_t,NativeAdrenoCacheOperation) {return kIOReturnSuccess;}
/* The kernel's process query the user client records with each import (BSD proc_selfpid). */
extern "C" int proc_selfpid(void);
extern "C" int proc_selfpid(void) {return 100;}

namespace {
constexpr size_t kSmmuBytes=0x40000;
alignas(8) uint32_t mmio[kSmmuBytes/4];
int sread32(void *,size_t at,uint32_t *v) {*v=mmio[at/4];return 0;}
int sread64(void *,size_t at,uint64_t *v) {std::memcpy(v,mmio+at/4,8);return 0;}
int swrite32(void *,size_t at,uint32_t v) {
    if((at&0xfff)==0x58 && at>=0x20000) {mmio[at/4]&=~v;return 0;}   /* context FSR is write-one-to-clear */
    if((at&0xfff)==0x7f0 && at>=0x20000)return 0;                     /* TLBSYNC: completes at once (TLBSTATUS stays 0) */
    mmio[at/4]=v;return 0;
}
int swrite64(void *,size_t at,uint64_t v) {std::memcpy(mmio+at/4,&v,8);return 0;}
int sbarrier(void *) {return 0;}
const ArmSmmu4KIo smmuIo{nullptr,kSmmuBytes,sread32,sread64,swrite32,swrite64,sbarrier};
/* A permissive programmer: the bank takes the kit's image as is, the routes are what was asked (the host has no hypervisor). */
IOReturn pclaim(void *,unsigned,const ArmSmmu4KStream *) {return kIOReturnSuccess;}
IOReturn pprogram(void *,unsigned bank,const ArmSmmu4KContextImage *image,uint64_t ttbr1,NativeAdrenoSmmuProgramReport *report)
{
    *report={};report->want=*image;report->want.ttbr1=ttbr1;report->got=report->want;report->step=14;
    report->context_offset=0x20000+uint64_t(bank)*0x1000;
    return kIOReturnSuccess;
}
IOReturn proute(void *,unsigned,const ArmSmmu4KStream *) {return kIOReturnSuccess;}
IOReturn pverify(void *,unsigned,const ArmSmmu4KStream *,size_t) {return kIOReturnSuccess;}
IOReturn pfault(void *,unsigned,const ArmSmmu4KStream *,size_t) {return kIOReturnSuccess;}
IOReturn pdisable(void *,unsigned) {return kIOReturnSuccess;}

bool a830()
{
    static const int chosen=[] {const char *gpu=std::getenv("HOSTSIM_GPU");return gpu && !std::strcmp(gpu,"a830")?1:0;}();
    return chosen!=0;
}

/* Software CP: a write of the ring write pointer retires everything up to it (the BR read pointer shadow and the completed fence in
 * the memptrs page), unless a test holds completions; the fence the ring reads is the number of submissions so far. */
std::vector<uint32_t> regs(0x10000);
volatile uint32_t *memptrs=nullptr;
unsigned submissions=0;
std::atomic<bool> holdCompletion{false};
std::recursive_mutex serviceLock;
unsigned calls[32]={},successes[32]={};
uint32_t faultRemaining[32]={};
IOReturn faultStatus[32]={};
uint64_t lastMapIova=0;
void completeIfReleased() {
    if(!holdCompletion.load(std::memory_order_acquire) && memptrs) {
        memptrs[0]=regs[0x0807];
        __atomic_store_n(const_cast<uint32_t *>(&memptrs[1]),submissions,__ATOMIC_RELEASE);
    }
}
int cwrite(void *,uint32_t reg,uint32_t v) {
    regs[reg]=v;
    if(reg==0x0807) {++submissions;completeIfReleased();}
    return 0;
}
int cread(void *,uint32_t reg,uint32_t *v) {completeIfReleased();*v=regs[reg];return 0;}
int cbarrier(void *) {return 0;}
int cpublish(void *,const volatile uint32_t *,size_t) {return 0;}
int cacquire(void *,volatile uint32_t *,size_t) {completeIfReleased();return 0;}
uint64_t cnow(void *) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
/* The kernel's idle hook sleeps until the next GPU interrupt: here a short sleep, after which a released CP completes. */
int cidle(void *) {std::this_thread::sleep_for(std::chrono::microseconds(50));completeIfReleased();return 0;}
/* Both always-on counters at 19.2 MHz: the A830's GMU AO pair (byte offsets 0x47100/0x47104 of the GMU window) and the CP pair. */
int timeread(void *,uint32_t address,uint32_t *value) {
    static uint64_t ticks=1000;
    const bool low=address==NativeAdrenoTimer::gmuLowOffset() || address==NativeAdrenoTimer::cpLowRegister();
    if(low)ticks+=1000;
    *value=low?uint32_t(ticks):uint32_t(ticks>>32);
    return 0;
}
const NativeAdrenoAbiDeviceInfo infoA740={0x43050a01u,3u<<20,0x1000000ull,0x1fffffffff000ull,0x20000000ull,0x60000000ull,
    16,6,1,4,NativeAdrenoDeviceClientIova,0};
/* The A830 (gen8_0_1, SM8750) facts the kernel reports: 12 MiB GMEM (KGSL gen8_0_1 .gmem_size, msm a8xx_catalog .gmem), GMEM base at
 * the KGSL 64 MiB alignment, UCHE trap base, the 4 GiB aperture's client VA, highest bank bit 16, swizzle levels 2+3, 8-channel
 * macrotiles, UBWC 5. */
const NativeAdrenoAbiDeviceInfo infoA830={0x44050001u,12u<<20,0x4000000ull,0x1fffffffff000ull,0x20000000ull,0xE0000000ull,
    16,6,1,5,NativeAdrenoDeviceClientIova,0};
const NativeAdrenoRingIo ringio{nullptr,cwrite,cread,cbarrier,nullptr,nullptr,nullptr};
const NativeAdrenoRingCache ringcache{nullptr,cpublish,cacquire};
const NativeAdrenoRingClock clock_{nullptr,cnow,cidle};
const NativeAdrenoTimeIo timeio{nullptr,timeread};

NativeAdrenoGpuContext *context;
NativeAdrenoRing ring;
NativeAdrenoSubmit execbuf;
NativeAdrenoUserClient client;
NativeAdrenoTimer timer;
bool built=false,opened=false;

bool build() {
    if(built)return true;
    /* HOSTSIM_SCATTER=1: non-contiguous BOs come back one physical page per span, as on the device (int-070's bridge ran out of
     * device memory there while the contiguous host model passed). */
    if(const char *scatter=std::getenv("HOSTSIM_SCATTER");scatter && *scatter=='1')fixture.mode=ScatterUnlessContiguous;
    /* HOSTSIM_CONTIGUOUS_MAX=<bytes>: larger physically contiguous allocations fail, so they fall back to scattered pages. */
    if(const char *limit=std::getenv("HOSTSIM_CONTIGUOUS_MAX"))fixture.contiguous_limit=std::strtoull(limit,nullptr,0);
    std::memset(mmio,0,sizeof(mmio));
    /* SM8750's GPU SMMU identity (IDR0 0x4C017E18, IDR1 0x40000016, IDR2 0x5511: 36-bit output) for a830; the A740 run keeps 40 bits. */
    mmio[0]=0x002f0406;mmio[0x20/4]=0x4c017e18;mmio[0x24/4]=0x40000016;mmio[0x28/4]=a830()?0x5511:0x1433;
    for(unsigned i=0;i<24;++i)mmio[(0xc00+4*i)/4]=0x20000;
    static const NativeAdrenoSmmuProgrammer programmer{nullptr,&smmuIo,mmio[0x20/4],mmio[0x24/4],mmio[0x28/4],
        pclaim,pprogram,proute,pverify,pfault,pdisable,false};
    context=new NativeAdrenoGpuContext;
    NativeAdrenoGpuContextConfig config{};
    config.programmer=&programmer;
    config.bank=0;config.asid=1;config.slot=0;config.lpac_slot=3;config.sid=0;config.lpac_sid=1;
    config.output_bits=0;config.aperture_gib=a830()?4:2;config.pool_mib=0;config.write_combined_pool=false;
    NativeAdrenoGmuContextReport report;
    const IOReturn up=context->bringUp(config,nullptr,&report);
    if(up!=kIOReturnSuccess) {std::fprintf(stderr,"host gpu: context bring-up failed %d at step %u\n",up,report.step);return false;}
    const NativeAdrenoMemory &ringmem=context->buffer(GpuBufferRing),&memory=context->buffer(GpuBufferMemptrs);
    memptrs=static_cast<volatile uint32_t *>(memory.mapping());
    if(ring.attach(&ringio,&ringcache,ringmem.mapping(),ringmem.bytes(),GpuRingIova,memory.mapping(),memory.bytes(),
        GpuMemptrsIova)!=kIOReturnSuccess) {std::fprintf(stderr,"host gpu: ring attach failed\n");return false;}
    context->vm()->setFenceSource(ring.memptrs()+1);
    if(execbuf.attach(context->vm(),&ring)!=kIOReturnSuccess) {std::fprintf(stderr,"host gpu: submit attach failed\n");return false;}
    if(timer.attach(&timeio,a830()?NativeAdrenoTimeGmu:NativeAdrenoTimeCp)!=kIOReturnSuccess)return false;
    submissions=0;memptrs[0]=0;memptrs[1]=0;
    built=true;
    return true;
}
IOReturn translate(IOReturn status) {return status;}
}

extern "C" {
/* Return codes are the fixture's IOReturn values; the shim translates them. */
int host_gpu_open() {
    std::lock_guard<std::recursive_mutex> guard(serviceLock);
    if(!build() || opened)return kIOReturnBusy;
    const uint32_t input_bits=a830()?32:31;
    /* HOSTSIM_SLOT=<n> (a830 only): the connection is mounted as NativeAdrenoPublish.cpp mounts a client in slot n, with
     * NativeAdrenoClientTermsFor's objects and render-server reserve and slot n's window of the 4 GiB table: 4 is an app's 124 MiB small
     * window, 1 a large client's 352 MiB (AdrenoMetal bridge/texture_atlas_host.mm). Unset or 0: the render server's slot 0, as before. */
    const unsigned slot=[] {const char *s=std::getenv("HOSTSIM_SLOT");return s?unsigned(std::strtoul(s,nullptr,0)):0u;}();
    const bool large=slot>=1 && slot<=NativeAdrenoLargeSlots;
    const NativeAdrenoClientTerms terms=NativeAdrenoClientTermsFor(false,slot==0,large,slot);
    IOReturn st=client.open(NativeAdrenoAbiVersion,context->vm(),&execbuf,&ring,input_bits,context->outputBits(),
        a830()?terms.objects:NativeAdrenoClientMaxObjects);
    if(st) {std::fprintf(stderr,"host gpu: client open failed %d\n",st);return kIOReturnNotReady;}
    std::setvbuf(stderr,nullptr,_IONBF,0);
    client.attachTimer(&timer);
    if(a830()) {
        /* The render server's slot of the kernel (ARCHITECTURE 6.2): its window is the VA GetDeviceInfo reports. */
        const NativeAdrenoClientWindow *windows=NativeAdrenoClientWindowsFor(infoA830.va_start+infoA830.va_size);
        client.attachDeviceInfo(&infoA830);
        if(slot>=NativeAdrenoMaxClients || !windows[slot].bytes) {
            std::fprintf(stderr,"host gpu: slot %u has no window\n",slot);client.releaseAll(&clock_,0);return kIOReturnNotReady;
        }
        if(terms.reserve)
            (void)client.setReserve(NativeAdrenoVmRenderReserveObjects,NativeAdrenoVmRenderReserveSpans,
                NativeAdrenoMemory::poolBytes()/NativeAdrenoPoolRenderShare);
        if(slot)std::fprintf(stderr,"host gpu: slot %u window 0x%llx + 0x%llx, %zu objects\n",slot,
            (unsigned long long)windows[slot].start,(unsigned long long)windows[slot].bytes,terms.objects);
        st=client.setScope(slot*NativeAdrenoAbiMaxQueues,windows[slot].start,windows[slot].bytes);
        if(st) {std::fprintf(stderr,"host gpu: client scope failed %d\n",st);client.releaseAll(&clock_,0);return kIOReturnNotReady;}
    } else client.attachDeviceInfo(&infoA740);
    opened=true;
    return kIOReturnSuccess;
}
int host_gpu_close() {
    std::lock_guard<std::recursive_mutex> guard(serviceLock);
    if(!opened)return kIOReturnNotReady;
    const IOReturn status=client.releaseAll(&clock_,2000000);
    if(!status)opened=false;
    return translate(status);
}
int host_gpu_call(uint32_t method,const void *in,size_t in_size,void *out,size_t out_size) {
    std::unique_lock<std::recursive_mutex> guard(serviceLock);
    if(!opened)return kIOReturnNotReady;
    completeIfReleased();
    if(method<32)++calls[method];
    if(method<32 && faultRemaining[method]){--faultRemaining[method];return faultStatus[method];}
    /* As the IOKit wrapper does (NativeAdrenoPublish.cpp): an import is the calling task's pages, here a stand-in task. */
    if(method==NativeAdrenoMethodBoImport) {
        if(!in || in_size!=sizeof(NativeAdrenoAbiBoImport) || !out || out_size!=sizeof(NativeAdrenoAbiBoAllocReply))return kIOReturnBadArgument;
        NativeAdrenoAbiBoImport request{};std::memcpy(&request,in,sizeof(request));
        NativeAdrenoAbiBoAllocReply reply{};
        const IOReturn status=client.importBo(&request,reinterpret_cast<void *>(0x1000),&reply);
        if(!status)std::memcpy(out,&reply,sizeof(reply));
        if(!status)++successes[method];
        return translate(status);
    }
    IOReturn status;
    if(method==NativeAdrenoMethodSubmit) {
        /* The kernel's bounded wait (ARCHITECTURE 5.4): the service lock is dropped while the caller sleeps for ring room. */
        status=NativeAdrenoSubmitWithRetry(
            [&](uint32_t *retry_fence) {
                const IOReturn result=client.dispatch(method,in,in_size,out,out_size,&clock_);
                if(result==NativeAdrenoSubmitRetry)*retry_fence=client.retryFence();
                return result;
            },
            [&](uint32_t,uint64_t) {
                guard.unlock();
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                guard.lock();
                completeIfReleased();
            },
            [] {return cnow(nullptr);});
    } else status=client.dispatch(method,in,in_size,out,out_size,&clock_);
    if(!status && method<32)++successes[method];
    if(!status && method==NativeAdrenoMethodBoMap && in)
        lastMapIova=static_cast<const NativeAdrenoAbiBoMap *>(in)->iova;
    /* HOSTSIM_TRACE=1: every failing call, and every allocation size, on stderr. */
    static const bool trace=std::getenv("HOSTSIM_TRACE") && *std::getenv("HOSTSIM_TRACE")=='1';
    if(trace && method==NativeAdrenoMethodBoAlloc && in)
        std::fprintf(stderr,"hostgpu alloc %llu -> %d spans-live %zu\n",(unsigned long long)static_cast<const NativeAdrenoAbiBoAlloc *>(in)->bytes,
            status,context->vm()->spanCount());
    else if(trace && status)std::fprintf(stderr,"hostgpu method %u -> %d\n",method,status);
    return translate(status);
}
int host_gpu_map(uint32_t token,void **address,uint64_t *bytes) {
    std::lock_guard<std::recursive_mutex> guard(serviceLock);
    NativeAdrenoMemory *memory=opened?client.memoryForToken(token):nullptr;
    if(!memory)return kIOReturnBadArgument;
    *address=memory->mapping();*bytes=memory->bytes();return kIOReturnSuccess;
}
unsigned host_gpu_submissions() {std::lock_guard<std::recursive_mutex> guard(serviceLock);return submissions;}
unsigned host_gpu_live_objects() {std::lock_guard<std::recursive_mutex> guard(serviceLock);return unsigned(client.liveObjects());}
unsigned host_gpu_calls(uint32_t method) {std::lock_guard<std::recursive_mutex> guard(serviceLock);return method<32?calls[method]:0;}
unsigned host_gpu_successes(uint32_t method) {std::lock_guard<std::recursive_mutex> guard(serviceLock);return method<32?successes[method]:0;}
uint64_t host_gpu_last_map_iova() {std::lock_guard<std::recursive_mutex> guard(serviceLock);return lastMapIova;}
// Only the atomic peer-release flag changes off-thread. The next service call, idle wait or ring read/acquire publishes the
// software peer's completion under the service lock, like the existing CP model.
void host_gpu_hold_completion(int hold) {holdCompletion.store(hold!=0,std::memory_order_release);}
void host_gpu_inject_failure(uint32_t method,int status,uint32_t count) {
    std::lock_guard<std::recursive_mutex> guard(serviceLock);
    if(method<32){faultStatus[method]=status;faultRemaining[method]=count;}
    else std::memset(faultRemaining,0,sizeof faultRemaining);
}
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
