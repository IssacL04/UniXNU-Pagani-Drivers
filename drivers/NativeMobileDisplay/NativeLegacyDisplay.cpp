#include "NativeLegacyRequest.h"
#include "NativeLegacySourceCache.h"
#include "NativeDisplayTiming.h"
#include "NativeScanoutFlip.h"
#include "NativeDpu.h"
#include "../BootProgress/PeripheralStatus.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
// Every frame that reached the panel, once, with its path and the mach time it got there (frameDone and the blank fill): PAGANI_FRAME_CPU
// when a CPU copy or fill into the scanout finished, PAGANI_FRAME_GPU when the GPU presenter's copy finished or, with page flips, the flip
// latched (a frame replaced in the mailbox before it latched never reached the panel and is not counted). Stage7Probe hands it to
// BootProgress's frame-rate ring (pagani-fps): two atomic adds and a store. This driver reports through the timed form below, which also
// says where the frame's time went (PeripheralStatus.h pagani_frame_timing: its arrival and the gap since the previous arrival, offer,
// taken, copied; mach ticks, 0 where a stage did not happen); with it the display path's stage (PAGANI_STAGE_*: what it waits for, since
// when) and its events (PAGANI_DISPLAY_*, counted per second). BootProgress (pagani-fps) shows the slowest frame of each second with its
// breakdown on text row 8 (the SLOW row) and logs it with each late frame (J records). No lock: atomic stores and adds.
extern "C" void native_legacy_frame_presented(unsigned path,uint64_t when);
extern "C" void native_legacy_frame_timed(unsigned path,uint64_t when,const pagani_frame_timing *timing);
extern "C" void native_legacy_frame_stage(unsigned stage,uint64_t when);
extern "C" void native_legacy_display_event(unsigned event);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client takes it (early retirement, sheng-early-retire, default 1) or when it reports the GPU copy done (native_legacy_gpu_complete). A
// refused offer or a failed copy uses the CPU copy below and is counted. native_legacy_gpu_offer_frame also passes the surface (the
// presenter's mapping cache key) and the byte offset of the scanout buffer the GPU writes (page flips, 0 otherwise).
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" bool native_legacy_gpu_offer_frame(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,
                                              OSObject *surface,uint64_t targetOffset);
extern "C" void native_legacy_gpu_fallback_observed(int status);
// The presenter's completion (NativeGOPPresenter completeSwap, a failed hand-over, an abandoned frame): the status, and when the client took
// the frame and reported its copy (mach ticks, 0: did not happen). native_legacy_gpu_complete is the same without the times.
extern "C" void native_legacy_gpu_complete(void *display,int status);
extern "C" void native_legacy_gpu_complete_timed(void *display,int status,uint64_t taken,uint64_t copied);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
// Display refresh tick (boot-arg sheng-vsync=<Hz or us>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate, 6 TE locked, 7 TE relocked,
// 8 TE lost (software cadence from here on), 9 panel switched to its 120 Hz timing.
// The rate: with pagani-te=1 the panel's measured TE (rounded to 120/90/60, ARCHITECTURE 5.3.3); otherwise pagani-vsync (default 120,
// the AA594 panel's default sa_fhd_120 timing: the DSC command-mode panel refreshes at 120 Hz). sheng-vsync as a rate only switches the tick on; as a
// period (NativeDisplayTiming.h, native_vsync_period_us) it also sets the tick's period, with no TE lock, for a panel measured in the log.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void pagani_panel_set_power(unsigned);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
// Display cadence for the GPU's frequency governor (Adreno830 A830Dcvs, ARCHITECTURE 4.16): every tick reports the TE it follows and the
// period (mach ticks); a frame that reached the scanout after the TE it aimed at reports how late it was (ns). Weak: kernels without the
// GPU driver keep these empty defaults.
extern "C" __attribute__((weak)) void pagani_display_vsync(uint64_t,uint64_t) {}
extern "C" __attribute__((weak)) void pagani_display_late(uint64_t) {}
// A generic DSI DCS write to the panel (data type 0x15 short or 0x39 long, payload bytes), provided by the panel driver; the default says
// unsupported (nonzero). Used only to move a panel found at 60 Hz to its 120 Hz timing (ARCHITECTURE 5.3.3 item 4).
extern "C" __attribute__((weak)) int pagani_panel_dcs_write(uint8_t,const uint8_t *,unsigned) {return -45;}
// Rows of the scanout the status overlay keeps (BootProgress, design 8.5); builds without it have none.
extern "C" __attribute__((weak)) int boot_progress_gpu_band(unsigned *,unsigned *) {return 0;}
static void gpuBand(uint32_t height,uint32_t *y0,uint32_t *y1) {
    unsigned top=0,bottom=0;
    if(!boot_progress_gpu_band(&top,&bottom) || top>=bottom || top>=height){*y0=*y1=height;return;}
    *y0=top;*y1=bottom<height?bottom:height;
}
extern "C" uint64_t mach_continuous_time(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
static bool bootArg(const char *name) {unsigned value=0;return PE_parse_boot_argn(name,&value,sizeof value) && value==1;}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    // The scanout is mapped write-combined unless pagani-fb-wc=0: Device-nGnRnE turned every frame into 1.6 million
    // ungathered 8-byte stores (tens to hundreds of milliseconds per frame).
    bool combined=false;
    uint32_t scanBudget=0;
    // Kernel mappings of the surfaces CoreAnimation rotates through (NativeLegacySourceCache.h).
    struct SourceOps {
        IOMemoryMap *map(IOMemoryDescriptor *memory) {
            IOMemoryMap *mapping=memory->map(kIOMapAnywhere|kIOMapReadOnly);
            if(mapping && !mapping->getVirtualAddress())OSSafeReleaseNULL(mapping);
            return mapping;
        }
        uint64_t length(IOMemoryMap *mapping) {return mapping->getLength();}
        void retain(IOMemoryDescriptor *memory) {memory->retain();}
        void release(IOMemoryDescriptor *memory,IOMemoryMap *mapping) {OSSafeReleaseNULL(mapping);OSSafeReleaseNULL(memory);}
    };
    NativeLegacySourceCache<IOMemoryDescriptor,IOMemoryMap> sources;
    IOMemoryMap *sourceMapping(IOMemoryDescriptor *memory,uint64_t allocation) {SourceOps ops;return sources.lookup(ops,memory,allocation);}
    void dropSources() {SourceOps ops;sources.drop(ops);}
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    // Write-combined frames are copied by a kernel thread call, outside the family's command gate (DF item 6; pagani-fb-async=0 keeps the
    // copy in the gate). The swap retires before the copy, like the GPU path's early retirement: the next swap still waits for the copy
    // (transferred), and this frame's surface stays the family's current one until that next swap retires.
    struct CopyJob {
        const OriginalLegacySwapRequest *request;
        void *dst;uint64_t dstBytes,dstStride;
        const void *src;uint64_t srcBytes,srcStride;
        uint64_t length,stored,arrival,done;
        uint32_t width,height,y0,y1;
        bool ok;
        pagani_frame_timing timing;     // queued at offer; the copy stamps taken and copied
    };
    CopyJob job={};
    thread_call_t copyCall=nullptr;
    bool copyEarly=false,stopping=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0,gpuArrival=0;
    bool gpuBypass=false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired=false;
    uint8_t gpuTarget=0;
    // sheng-fb-gpu=1: a refused offer is a CPU fallback and counted (PAGANI_DISPLAY_REFUSED); without it every frame takes the CPU copy.
    bool gpuWanted=false;
    // Where each frame's time goes (pagani-fps; reported with the frame by frameDone). A kick notes the head swap's arrival the first time
    // it sees it there, also while the previous frame is still on its way, with the gap since the previous arrival (the compositor's
    // cadence); `arrived` keeps it and its flags (refused, failed, bypass, deferred) until the frame starts its way, offered to the GPU
    // client or copied by the CPU (startTiming). arrivedRequest is forgotten when a swap retires: its request may come back for a later
    // swap. gpuTiming: the frame the GPU client has; latchTiming: the frame whose page flip waits for its latch. Work loop only, like the
    // rest of the swap state.
    const OriginalLegacySwapRequest *arrivedRequest=nullptr;
    pagani_frame_timing arrived={},gpuTiming={},latchTiming={};
    uint64_t lastArrival=0;
    bool arrivedWaiting=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0,ticksPerSecond=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    bool displayOn=true;
    // TE lock (pagani-te=1): the cadence of INTF_1's read-pointer frame count; the tick wakes 50 us after each predicted TE.
    bool teWanted=false;
    NativeTeLock te;
    uint64_t teIndex=0,teRelocks=0;
    uint32_t savedAutorefresh=0;
    // Page flips (pagani-fb-flip=1, ARCHITECTURE 5.3.4): three buffers in the splash region, the pipes that fetch them, the flip state.
    NativeDpuState dpu={};
    NativeDpuScanout scanout={};
    NativeScanoutFlip flip;
    uint64_t frameStride=0;
    IOMemoryMap *flipMapping=nullptr;          // CPU view of buffers 1 and 2 (first-flip trial)
    IOTimerEventSource *latchTimer=nullptr;
    uint64_t latchStart=0,latchArrival=0;
    bool latchWaiting=false,flipDeferred=false;
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    static uint64_t ticksToNs(uint64_t ticks) {uint64_t ns=0;absolutetime_to_nanoseconds(ticks,&ns);return ns;}
    uint64_t usToTicks(uint64_t us) const {return ticksPerSecond*us/1000000;}
    // The refresh this display reports and ticks at (see the comment at native_legacy_vsync_notify).
    unsigned refreshHz() const {
        if(te.valid){unsigned hz=native_refresh_class(te.millihertz(ticksPerSecond));if(hz)return hz;}
        unsigned tick=0;PE_parse_boot_argn("sheng-vsync",&tick,sizeof(tick));
        if(const unsigned hz=native_vsync_class(tick))return hz;
        unsigned hz=120;PE_parse_boot_argn("pagani-vsync",&hz,sizeof(hz));
        return hz>=24 && hz<=240?hz:120;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!displayOn || !vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;
        const uint64_t now=mach_absolute_time();
        if(te.valid){uint64_t index=0;vsyncNext=te.next(now,&index)+usToTicks(nativedpu::kTickLeadUs);teIndex=index;}
        else vsyncNext=now+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    // Dense sampling of the frame field for at most `us`: the edge's time (middle of the two readings around it) and frame field.
    bool teEdge(unsigned us,uint64_t *edge,uint16_t *count) {
        const uint64_t until=mach_absolute_time()+usToTicks(us);
        uint64_t previous=mach_absolute_time();
        uint16_t frame=uint16_t(native_dpu_read(nativedpu::kIntf1+nativedpu::kTearIntCount)>>16);
        while(previous<until) {
            IODelay(10);
            const uint64_t now=mach_absolute_time();
            const uint16_t next=uint16_t(native_dpu_read(nativedpu::kIntf1+nativedpu::kTearIntCount)>>16);
            if(next!=frame){*edge=previous+(now-previous)/2;*count=next;return true;}
            previous=now;
        }
        return false;
    }
    void publishTe() {
        native_dpu_te_stats[0]=te.valid;native_dpu_te_stats[1]=te.valid?ticksToNs(te.period()):0;
        native_dpu_te_stats[2]=teRelocks;native_dpu_te_stats[3]=te.valid?refreshHz():0;
    }
    // 100 ms of 50 us samples (ARCHITECTURE 5.3.3 item 2).
    bool lockTe() {
        if(!native_dpu_map())return false;
        NativeTeSampler sampler;
        const uint64_t until=mach_absolute_time()+usToTicks(nativedpu::kLockWindowUs);
        for(uint64_t now=mach_absolute_time();now<until;now=mach_absolute_time()) {
            sampler.sample(now,native_dpu_read(nativedpu::kIntf1+nativedpu::kTearIntCount));
            IODelay(nativedpu::kLockSampleUs);
        }
        NativeTeLock lock;
        if(!sampler.lock(&lock,ticksPerSecond)){te.valid=false;publishTe();return false;}
        te=lock;teIndex=0;publishTe();
        native_legacy_vsync_observed(6,refreshHz());
        return true;
    }
    // A TE-locked tick (native_te_cadence): the TE it follows, the next wake-up 50 us after the following TE. On time: the predicted TE;
    // late by whole TEs: the latest; early (the phase drifted): a 5 ms dense relock; no edge then: the TE is gone, software cadence.
    void vsyncTick() {
        vsyncArmed=false;
        if(!displayOn || !vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        uint64_t now=mach_absolute_time(),stamp;
        if(te.valid) {
            bool relocked=false;
            stamp=native_te_cadence(&te,&teIndex,[]{return mach_absolute_time();},
                [&]{return native_dpu_read(nativedpu::kIntf1+nativedpu::kTearIntCount);},
                [&](unsigned us,uint64_t *time,uint16_t *count){return teEdge(us,time,count);},nativedpu::kRelockUs,
                usToTicks(nativedpu::kTickLeadUs),&vsyncNext,&now,&relocked);
            if(relocked){++teRelocks;publishTe();native_legacy_vsync_observed(7,vsyncHz);}
            if(te.valid)vsyncInterval=te.period();
            else {publishTe();native_legacy_vsync_observed(8,vsyncHz);vsyncNext=now+vsyncInterval;}
        } else {
            // The scheduled tick, not the late now (a gate holder delays delivery; that must not shift the cadence the render server sees).
            stamp=native_vsync_stamp(vsyncNext,now,vsyncInterval);
            vsyncNext+=vsyncInterval;
            if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        }
        native_legacy_vsync_notify(this,stamp,mach_continuous_time()-(mach_absolute_time()-stamp),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        pagani_display_vsync(stamp,vsyncInterval);
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
        // A failed CPU copy left its swap at the head (copyDone): kick it again now. The tick is armed, so the kick's armVsync returns at once.
        if(retryKick){retryKick=false;kickSwapQueue();}
    }
    void startVsync() {
        unsigned tick=0;PE_parse_boot_argn("sheng-vsync",&tick,sizeof(tick));
        if(!native_vsync_on(tick) || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        const unsigned rate=refreshHz();
        if(te.valid)vsyncInterval=te.period();else nanoseconds_to_absolutetime(native_vsync_ns(tick,rate),&vsyncInterval);
        vsyncHz=rate;vsyncTimer=timer;native_legacy_vsync_observed(4,rate);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
    // A panel found at 60 Hz goes to its 120 Hz timing (DT timing@sa_fhd_120 qcom,mdss-dsi-timing-switch-command), sent after a TE while the
    // write pointer is outside the frame (line count 0 or >= 2640). Needs the panel driver's DCS write; then the cadence is locked again.
    void panelTo120() {
        if(!te.valid || native_refresh_class(te.millihertz(ticksPerSecond))!=60)return;
        struct Dcs {uint8_t type,length,payload[4];};
        static const Dcs packets[8]={{0x39,4,{0xff,0x5a,0xa5,0x53}},{0x39,3,{0x85,0xcc,0xe1,0}},{0x39,4,{0xff,0x5a,0xa5,0x00}},
                                     {0x15,2,{0x60,0x00,0,0}},{0x39,4,{0xff,0x5a,0xa5,0x2d}},{0x39,3,{0x83,0x00,0x00,0}},
                                     {0x15,2,{0xf2,0x00,0,0}},{0x39,4,{0xff,0x5a,0xa5,0x00}}};
        uint64_t edge=0;uint16_t count=0;
        if(!teEdge(25000,&edge,&count))return;
        const uint32_t line=native_dpu_read(nativedpu::kIntf1+nativedpu::kTearLineCount)&0xffff;
        if(line && line<nativedpu::kPanelLines)return;
        for(const Dcs &packet: packets)if(pagani_panel_dcs_write(packet.type,packet.payload,packet.length))return;
        native_legacy_vsync_observed(9,120);
        lockTe();
    }
    // Page flips --------------------------------------------------------------------------------------------------------------------
    uint64_t bufferBase(uint8_t b) const {return bootMapping->getPhysicalAddress()+uint64_t(b)*frameStride;}
    void writeFlip(uint8_t b) {
        if(!bootMapping || !scanout.pipes)return;
        NativeDpuWrite writes[2*nativedpu::kPipeCount+1];
        const unsigned n=native_dpu_flip_writes(scanout,bufferBase(b),writes,sizeof(writes)/sizeof(writes[0]));
        if(!n)return;
        for(unsigned i=0;i+1<n;++i)native_dpu_write(writes[i].offset,writes[i].value);
        __asm__ volatile("dsb sy" ::: "memory");
        native_dpu_write(writes[n-1].offset,writes[n-1].value);    // the CTL flush, last
        __asm__ volatile("dsb sy" ::: "memory");
    }
    bool flushPending() const {return native_dpu_read(nativedpu::kCtl0+nativedpu::kCtlStride*scanout.ctl+nativedpu::kCtlFlush)&scanout.flush;}
    // Spins (in the gate) until the flush bits clear; used only by the trial and the way back to buffer 0.
    bool waitLatch(uint64_t *latencyUs) {
        const uint64_t start=mach_absolute_time(),until=start+usToTicks(nativedpu::kLatchTimeoutUs);
        while(flushPending()) {
            if(mach_absolute_time()>until)return false;
            IODelay(200);
        }
        if(latencyUs)*latencyUs=ticksToNs(mach_absolute_time()-start)/1000;
        return true;
    }
    void noteLatch(uint64_t us) {
        native_dpu_flip_stats[1]=native_dpu_flip_stats[1]+1;native_dpu_flip_stats[3]=native_dpu_flip_stats[3]+us;
        if(us>native_dpu_flip_stats[2])native_dpu_flip_stats[2]=us;
    }
    // First write trial (picture unchanged): B0 copied to B1, flip to B1; the frame count goes on and the underrun bit stays as it was; flip
    // back to B0. Returns 1 when MDP fetches B1 fine, else the failing check. Check 6 (the MDP streams' context-bank fault status) read the
    // apps SMMU, which the Gunyah hypervisor owns, and is gone (dpu-safety, 2026-10-30: the snapshot that read it hung the 13T).
    uint32_t flipTrial() {
        using namespace nativedpu;
        // Whole rows of the boot mapping (write-combined: plain loads are fine) into buffer 1.
        PE_Video video={};
        if(!flipMapping || !combined || PE_current_console(&video) || !native_legacy_copy_rows(
               reinterpret_cast<void *>(flipMapping->getVirtualAddress()),frameStride,video.v_rowBytes,
               reinterpret_cast<const void *>(bootMapping->getVirtualAddress()),bootMapping->getLength(),video.v_rowBytes,
               uint32_t(video.v_rowBytes/4),uint32_t(video.v_height)))return 2;
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t underrun=native_dpu_read(kMdpIntrStatus)&kIntf1Underrun;
        const uint32_t frames=native_dpu_read(kIntf1+kIntfFrameCount);
        uint64_t us=0;
        writeFlip(1);
        if(!waitLatch(&us)){writeFlip(0);waitLatch(nullptr);return 3;}
        noteLatch(us);
        IODelay(40000);                     // a few TEs on B1
        uint32_t result=1;
        if(native_dpu_read(kIntf1+kIntfFrameCount)==frames)result=4;
        else if((native_dpu_read(kMdpIntrStatus)&kIntf1Underrun)!=underrun)result=5;
        writeFlip(0);
        if(!waitLatch(&us))return 7;
        noteLatch(us);
        return result;
    }
    void startFlips() {
        using namespace nativedpu;
        // Flips serve the GPU path only: the CPU path writes buffer 0 (5.3.4), so without sheng-fb-gpu=1 there is nothing to flip to.
        if(!bootArg("pagani-fb-flip") || !bootArg("sheng-fb-gpu") || !primary || !bootMapping)return;
        PE_Video video={};
        const uint64_t frame=bootMapping->getLength();
        frameStride=(frame+4095)&~4095ull;
        if(PE_current_console(&video) || video.v_baseAddr!=kSplashBase || frameStride*kScanoutBuffers>kSplashBytes){native_dpu_flip_stats[7]=8;return;}
        if(!native_dpu_snapshot(&dpu) || !native_dpu_scanout(dpu,kSplashBase,frame,&scanout)){native_dpu_flip_stats[7]=9;return;}
        IODeviceMemory *buffers=IODeviceMemory::withRange(kSplashBase+frameStride,frameStride*(kScanoutBuffers-1));
        flipMapping=buffers?buffers->map(kIOMapAnywhere|kIOMapWriteCombineCache):nullptr;
        OSSafeReleaseNULL(buffers);
        if(flipMapping && !flipMapping->getVirtualAddress())OSSafeReleaseNULL(flipMapping);
        IOWorkLoop *loop=getWorkLoop();
        if(!flipMapping || !loop){native_dpu_flip_stats[7]=10;return;}
        latchTimer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired);
        if(!latchTimer || loop->addEventSource(latchTimer)!=kIOReturnSuccess){OSSafeReleaseNULL(latchTimer);native_dpu_flip_stats[7]=10;return;}
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            auto self=static_cast<NativeLegacyDisplay *>(owner);
            const uint32_t result=self->flipTrial();
            native_dpu_flip_stats[7]=result;
            if(result==1)self->flip.enable(nativedpu::kScanoutBuffers);
            return kIOReturnSuccess;
        },this);
    }
    void stopFlips() {
        if(latchTimer) {
            latchTimer->cancelTimeout();
            if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(latchTimer);
            OSSafeReleaseNULL(latchTimer);
        }
        latchWaiting=false;
        if(flip.on() || !flip.atHome()){flip.reset();writeFlip(0);}
        OSSafeReleaseNULL(flipMapping);
    }
    // The GPU wrote `b`: flush it to the pipes and poll for the latch (first at the next TE when locked). The frame's breakdown waits with
    // it (its "done" is the wait for the latch); a flip replaced in the mailbox takes the place of the frame before it.
    void flipTo(uint8_t b,uint64_t arrival,const pagani_frame_timing *timing) {
        if(!flip.complete(b))return;
        latchTiming=*timing;latchTiming.flags|=PAGANI_TIMING_FLIP;
        // Mailbox: never rewrite the pipes right at a TE, where half of them could latch the old addresses.
        if(te.valid && latchWaiting) {
            uint64_t index=0;const uint64_t next=te.next(mach_absolute_time(),&index);
            if(next-mach_absolute_time()<usToTicks(100))IODelay(150);
        }
        writeFlip(b);
        native_dpu_flip_stats[0]=flip.flips;native_dpu_flip_stats[6]=flip.mailboxes;
        latchStart=mach_absolute_time();latchArrival=arrival;latchWaiting=true;
        uint64_t wait=nativedpu::kLatchPollUs;
        if(te.valid){uint64_t index=0;const uint64_t next=te.next(latchStart,&index);wait=ticksToNs(next-latchStart)/1000+200;}
        latchTimer->setTimeoutUS(uint32_t(wait));
    }
    static void latchFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->latchPoll();}
    void latchPoll() {
        if(!latchWaiting)return;
        const uint64_t now=mach_absolute_time();
        if(!flushPending()) {
            latchWaiting=false;flip.latched();noteLatch(ticksToNs(now-latchStart)/1000);
            frameDone(latchArrival,now,PAGANI_FRAME_GPU,&latchTiming);
            if(!transferred)stageIdle(now);     // otherwise the next frame is on its way and the stage is its own
            if(flipDeferred){flipDeferred=false;kickSwapQueue();}
            return;
        }
        if(now-latchStart>usToTicks(nativedpu::kLatchTimeoutUs)) {
            // Never latched: back to buffer 0 and no more flips.
            latchWaiting=false;native_dpu_flip_stats[5]=native_dpu_flip_stats[5]+1;
            flip.reset();writeFlip(0);
            if(!transferred)stageIdle(now);
            if(flipDeferred){flipDeferred=false;kickSwapQueue();}
            return;
        }
        latchTimer->setTimeoutUS(nativedpu::kLatchPollUs);
    }
    // The CPU path writes buffer 0 only: flip back to it and wait for the latch first (ARCHITECTURE 5.3.4).
    void flipHome() {
        if(flip.atHome() && !latchWaiting)return;
        latchWaiting=false;
        if(latchTimer)latchTimer->cancelTimeout();
        flip.home();writeFlip(0);
        uint64_t us=0;
        if(waitLatch(&us))noteLatch(us);
        else {native_dpu_flip_stats[5]=native_dpu_flip_stats[5]+1;flip.reset();}
    }
    // The head swap seen by a kick: noted the first time (see arrivedRequest). While another frame is on its way the stage stays that
    // frame's; this one's "off" (arrival to offer) then shows how long it waited for it.
    void noteArrival(const OriginalLegacySwapRequest *request) {
        if(request==arrivedRequest)return;
        const uint64_t now=mach_absolute_time();
        arrivedRequest=request;arrived={};
        arrived.arrival=now;arrived.gap=lastArrival && now>lastArrival?now-lastArrival:0;
        lastArrival=now;arrivedWaiting=true;
        if(!transferred)native_legacy_frame_stage(PAGANI_STAGE_ARRIVED,now);
    }
    // Flags for the breakdown of the noted head frame (refused, failed, bypass, deferred).
    void flagArrival(const OriginalLegacySwapRequest *request,uint32_t flags) {if(request==arrivedRequest)arrived.flags|=flags;}
    // The noted frame starts its way (offered, or its CPU copy queued or done): its breakdown so far. A request no kick noted starts here.
    pagani_frame_timing startTiming(const OriginalLegacySwapRequest *request) {
        pagani_frame_timing timing={};
        if(request==arrivedRequest)timing=arrived;
        arrivedWaiting=false;
        return timing;
    }
    // A frame is done (presented or lost): the path waits for the next one, at the head already (since its arrival) or still to come.
    void stageIdle(uint64_t now) {
        if(arrivedWaiting)native_legacy_frame_stage(PAGANI_STAGE_ARRIVED,arrived.arrival);
        else native_legacy_frame_stage(PAGANI_STAGE_IDLE,now);
    }
    // A frame reached the scanout on `path` at `done`, with its breakdown: counted for the frame rate; late against the TE it aimed at?
    // (counted, and reported to the GPU governor)
    void frameDone(uint64_t arrival,uint64_t done,unsigned path,const pagani_frame_timing *timing) {
        native_legacy_frame_timed(path,done,timing);
        uint64_t reference=0,period=0;
        if(te.valid){reference=te.anchor;period=te.period();}
        else if(vsyncTimer){reference=vsyncNext;period=vsyncInterval;}
        const uint64_t late=native_frame_late(reference,period,arrival,done);
        if(!late)return;
        native_dpu_flip_stats[4]=native_dpu_flip_stats[4]+1;
        pagani_display_late(ticksToNs(late));
    }
    // Screen off (ARCHITECTURE 5.3.3 item 6, with pagani-te=1 and pagani-te-ar-off=1): autorefresh off in Linux's order (external TE off,
    // autorefresh bit off, the write pointer leaves the frame, external TE back); screen on restores the boot loader's value. Opt-in since
    // 2026-10-30 (dpu-safety): once the MDP idles nothing proves it stays clocked, and screen on writes into it again. By default the MDP
    // keeps refreshing the panel that DCS 0x28 blanked (PaganiPanel), so INTF_1 stays live for the TE lock.
    void autorefresh(bool on) {
        using namespace nativedpu;
        if(!teWanted || !bootArg("pagani-te-ar-off") || !native_dpu_map())return;
        if(on) {
            if(savedAutorefresh)native_dpu_write(kIntf1+kTearAutorefresh,savedAutorefresh);
            savedAutorefresh=0;return;
        }
        const uint32_t value=native_dpu_read(kIntf1+kTearAutorefresh);
        if(!(value&kAutorefreshEnable))return;
        savedAutorefresh=value;
        const uint32_t vsync=native_dpu_read(kIntf1+kTearSyncVsync);
        native_dpu_write(kIntf1+kTearSyncVsync,vsync&~kTearExternalTe);
        native_dpu_write(kIntf1+kTearAutorefresh,value&~kAutorefreshEnable);
        for(unsigned n=0;n<42;++n) {
            const uint32_t line=native_dpu_read(kIntf1+kTearLineCount)&0xffff;
            if(!line || line>=kPanelLines)break;
            IODelay(2000);
        }
        native_dpu_write(kIntf1+kTearSyncVsync,vsync|kTearExternalTe);
    }
    // The CPU copy outside the gate ---------------------------------------------------------------------------------------------------
    static void copyEntry(thread_call_param_t owner,thread_call_param_t) {
        auto self=static_cast<NativeLegacyDisplay *>(owner);
        CopyJob &j=self->job;
        uint64_t t0,t1;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0));
        j.timing.taken=mach_absolute_time();
        native_legacy_frame_stage(PAGANI_STAGE_TAKEN,j.timing.taken);
        j.ok=native_legacy_copy_band(j.dst,j.dstBytes,j.dstStride,j.src,j.srcBytes,j.srcStride,j.width,j.height,j.y0,j.y1,1);
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t1));
        j.done=mach_absolute_time();
        j.timing.copied=j.done;
        if(j.ok)native_legacy_frame_stage(PAGANI_STAGE_COPIED,j.done);
        if(j.ok)native_legacy_copy_timing_observed(t1-t0,j.stored);
        if(IOWorkLoop *loop=self->getWorkLoop())loop->runAction([](OSObject *o,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(o)->copyDone();return kIOReturnSuccess;},self);
        self->release();
    }
    void copyDone() {
        const OriginalLegacySwapRequest *request=job.request;job.request=nullptr;
        if(!request)return;
        native_legacy_transfer_observed(job.ok?kIOReturnSuccess:kIOReturnBadArgument,request,job.ok?job.length:0);
        if(job.ok)frameDone(job.arrival,job.done,PAGANI_FRAME_CPU,&job.timing);
        else if(request==arrivedRequest)arrivedWaiting=true;   // still the head (not retired early): copied again at the next kick
        stageIdle(mach_absolute_time());
        if(!displayOn)dropSources();            // the screen went off during the copy
        const NativeSwapEnd end=native_copy_end(copyEarly,job.ok,stopping);
        copyEarly=false;
        switch(end.action) {
        case kSwapReleaseNext:transferred=false;copyRetries=0;kickSwapQueue();break;
        case kSwapRetire:copyRetries=0;finishTransfer(request);break;
        case kSwapRetry:
            // A failed copy leaves the swap at the head. The family kicks only for the client's next swap (int-143), and a client that waits
            // for this one sends none: copied again at the next refresh tick, a bounded number of times, then left to the family's next kick.
            transferred=false;
            if(copyRetries<kCopyRetries){++copyRetries;retryKick=true;}
            break;
        default:transferred=false;break;        // stopped
        }
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop. `taken` and `copied`: when
    // the client took the frame and reported its copy (0: did not happen), for the frame's breakdown.
    void gpuComplete(int status,uint64_t taken=0,uint64_t copied=0) {
        struct Args {NativeLegacyDisplay *display;int status;uint64_t taken,copied;} args={this,status,taken,copied};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status,a->taken,a->copied);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status,taken,copied);
    }
    void free() override {
        if(copyCall){thread_call_free(copyCall);copyCall=nullptr;}
        stopVsync();stopFlips();if(logicalPM){PMstop();logicalPM=false;}primary=false;dropSources();OSSafeReleaseNULL(bootMapping);
        IOMobileFramebufferLegacy::free();
    }
    void stop(IOService *provider) override {
        // Outside the family gate: a running copy finishes (its completion takes the gate); a queued one is dropped with its reference.
        if(IOWorkLoop *loop=getWorkLoop())loop->runAction([](OSObject *o,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(o)->stopping=true;return kIOReturnSuccess;},this);
        else stopping=true;
        if(copyCall && thread_call_cancel_wait(copyCall)){job.request=nullptr;release();}
        stopVsync();stopFlips();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;gpuRetired=false;retryKick=false;copyRetries=0;
        arrivedRequest=nullptr;arrivedWaiting=false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);dropSources();OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        nanoseconds_to_absolutetime(1000000000ull,&ticksPerSecond);
        gpuWanted=bootArg("sheng-fb-gpu");
        // The TE is measured before the family's start registers the display: registerService reports the measured refresh.
        teWanted=bootArg("pagani-te");
        if(teWanted && lockTe())panelTo120();
        if(!startDisplay(provider))return false;
        unsigned async=1;PE_parse_boot_argn("pagani-fb-async",&async,sizeof(async));
        if(async && combined && !copyCall)
            copyCall=thread_call_allocate_with_options(&NativeLegacyDisplay::copyEntry,this,THREAD_CALL_PRIORITY_KERNEL_HIGH,
                                                       THREAD_CALL_OPTIONS_ONCE);
        startFlips();
        startVsync();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(status==kIOReturnSuccess) {
            pagani_panel_set_power((unsigned)ordinal);
            if(IOWorkLoop *loop=getWorkLoop())loop->runAction([](OSObject *owner,void *value,void *,void *,void *)->IOReturn {
                auto self=static_cast<NativeLegacyDisplay *>(owner);
                self->displayOn=(uintptr_t)value!=0;
                if(self->displayOn){self->autorefresh(true);native_dpu_idle=0;self->armVsync();}
                else {
                    native_dpu_idle=1;          // the display probe stops reading before autorefresh may go off (DpuProbe.cpp)
                    if(self->vsyncTimer){self->vsyncTimer->cancelTimeout();self->vsyncArmed=false;}
                    // Under the work-loop gate, serialized with kickOnce; a copy still running drops them when it ends.
                    if(!self->job.request)self->dropSources();
                    self->autorefresh(false);
                }
                return kIOReturnSuccess;
            },this,reinterpret_cast<void *>(ordinal));
        }
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && native_vsync_on(hz))native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // GOP timing is firmware-owned; report our logical scanout state.
    bool isTimingEnabled() override {return primary && bootMapping && displayOn;}
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!native_vsync_on(hz))return true;
        // The measured TE (pagani-te=1) or this panel's rate (pagani-vsync, default 120): the render server paces itself by the minimum.
        hz=refreshHz();
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=native_refresh_interval(hz);
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    // copyDone's retry of a failed CPU copy at the next refresh tick, and how many in a row (kCopyRetries at most).
    static constexpr uint8_t kCopyRetries=8;
    bool retryKick=false;
    uint8_t copyRetries=0;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(stopping || enabled!=1 || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // The frame's arrival: the first kick that sees it at the head, also while the previous frame is still on its way (the swap
        // queue's head moves when that one retires; with early retirement that is at its offer).
        noteArrival(request);
        if(transferred)return;
        const uint64_t arrival=mach_absolute_time();
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            flipHome();
            uint32_t y0,y1;gpuBand(bh,&y0,&y1);
            const uint64_t fillStart=mach_absolute_time();
            native_legacy_frame_stage(PAGANI_STAGE_TAKEN,fillStart);
            if(!native_legacy_fill_band(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,y0,y1,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            {
                pagani_frame_timing timing=startTiming(request);
                timing.offer=timing.taken=fillStart;timing.copied=mach_absolute_time();
                native_legacy_frame_timed(PAGANI_FRAME_CPU,timing.copied,&timing);
                stageIdle(timing.copied);
            }
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            // With page flips the GPU writes a buffer that is neither scanned nor waiting to latch; none free: wait for the next latch.
            uint8_t target=0;
            if(flip.on() && (target=flip.take())==NativeScanoutFlip::kNone) {
                flipDeferred=true;flagArrival(request,PAGANI_TIMING_DEFERRED);return;
            }
            // Offered: the stage first, so the client's "taken" (another CPU, right after the offer) is never overwritten by it.
            const uint64_t offered=mach_absolute_time();
            native_legacy_frame_stage(PAGANI_STAGE_OFFERED,offered);
            if(native_legacy_gpu_offer_frame(this,memory,sourceStride,width,height,surface,uint64_t(target)*frameStride)) {
                // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
                native_legacy_swap_stage(1);
                gpuTiming=startTiming(request);gpuTiming.offer=offered;
                shadowValid=false;gpuRequest=request;gpuLength=length;gpuArrival=arrival;gpuTarget=target;transferred=true;
                // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
                // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
                // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
                // the copy because transferred stays set until finishGpuSwap.
                unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof(early));
                if(early==1 && retireSwap(request,true)) {
                    gpuRetired=true;native_legacy_early_retires=native_legacy_early_retires+1;
                }
                return;
            }
            if(flip.on())flip.abandon(target);
            // Refused (no client waiting yet, or it went away): the CPU copies this frame.
            if(gpuWanted){flagArrival(request,PAGANI_TIMING_REFUSED);native_legacy_display_event(PAGANI_DISPLAY_REFUSED);}
        } else flagArrival(request,PAGANI_TIMING_BYPASS);
        flipHome();
        IOMemoryMap *source=sourceMapping(memory,allocation);
        if(!source) {
            native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint32_t y0,y1;gpuBand(height,&y0,&y1);
        if(combined && copyCall) {
            // Queued for the copy thread: its "take" is the wait for that thread, its "copy" the copy (copyEntry stamps both).
            pagani_frame_timing timing=startTiming(request);
            timing.offer=mach_absolute_time();
            native_legacy_frame_stage(PAGANI_STAGE_OFFERED,timing.offer);
            job={request,reinterpret_cast<void *>(bootMapping->getVirtualAddress()),bootMapping->getLength(),video.v_rowBytes,
                 reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,
                 length,(uint64_t)width*4*(height-(y1-y0)),arrival,0,width,height,y0,y1,false,timing};
            transferred=true;
            copyEarly=retireSwap(request,false);
            retain();
            if(thread_call_enter(copyCall))release();
            return;
        }
        // The copy in the gate: offered and taken when it starts.
        const uint64_t copyBegin=mach_absolute_time();
        native_legacy_frame_stage(PAGANI_STAGE_TAKEN,copyBegin);
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        if(combined || y1>y0) {
            if(!native_legacy_copy_band(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height,y0,y1,combined)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            if(!combined)shadowValid=false;     // the differential copy's shadow no longer covers the scanout
            stored=(uint64_t)width*4*(height-(y1-y0));
        }
        if(stored==UINT64_MAX) {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        transferred=true;
        pagani_frame_timing timing=startTiming(request);
        timing.offer=timing.taken=copyBegin;timing.copied=mach_absolute_time();
        frameDone(arrival,timing.copied,PAGANI_FRAME_CPU,&timing);
        stageIdle(timing.copied);
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy (or, retired already, releases
    // the next frame) and with page flips flushes the buffer it wrote; anything else uses the CPU copy. `taken` and `copied` complete the
    // frame's breakdown (0: did not happen).
    void finishGpuSwap(int status,uint64_t taken=0,uint64_t copied=0) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        const uint64_t now=mach_absolute_time();
        gpuTiming.taken=taken;gpuTiming.copied=copied;
        if(flip.on()) {
            if(status==0)flipTo(gpuTarget,gpuArrival,&gpuTiming);       // the stage stays "copied" until the latch
            else {flip.abandon(gpuTarget);flipHome();flip.reset();}     // a failed GPU copy ends the flips
        } else if(status==0){frameDone(gpuArrival,now,PAGANI_FRAME_GPU,&gpuTiming);stageIdle(now);}
        // Retired already when the client took it: its copy ending only releases the next frame, and a failed copy loses this frame (the
        // next one takes the CPU copy). Otherwise success retires it now and a failure re-kicks it through the CPU copy.
        const NativeSwapEnd end=native_gpu_swap_end(gpuRetired,status);
        gpuRetired=false;
        if(end.reportSuccess)native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
        if(end.earlyFailure) {
            native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_display_event(PAGANI_DISPLAY_EARLY_FAILED);
        }
        if(status){native_legacy_gpu_fallback_observed(status);native_legacy_display_event(PAGANI_DISPLAY_FAILED);}
        if(end.action==kSwapRetire) {
            if(finishTransfer(request,true))return;
            // Refused: the request is no longer the family's pending head (the original family expires and clears swaps that waited too
            // long). Nothing is left to retire here; keeping the latch set would end every later kick at `transferred` for the boot.
            removeValidated(request);
            if(request==arrivedRequest){arrivedRequest=nullptr;arrivedWaiting=false;}
            transferred=false;kickSwapQueue();return;
        }
        if(end.bypassGpu)gpuBypass=true;
        if(status) {
            // Not retired: still the head, copied by the CPU right away (its breakdown keeps its arrival). Retired: this frame is lost.
            if(!end.earlyFailure && request==arrivedRequest){arrived.flags|=PAGANI_TIMING_FAILED;arrivedWaiting=true;}
            stageIdle(now);
        }
        transferred=false;kickSwapQueue();
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        // Only after the real copy and store barrier (or once the GPU client holds the frame): select the actual pending head and let the
        // unchanged family release it and wake waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        // Out of the queue: its request may come back for a later swap, which is then a new arrival.
        if(request==arrivedRequest){arrivedRequest=nullptr;arrivedWaiting=false;}
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    // false: not retired (retirement disabled, or the request is not the pending head); the latch is left to the caller.
    bool finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        if(!retireSwap(request,force))return false;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
        return true;
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch,
    // so without the property the panel's own (native_legacy_dot_pitch).
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property) {
            uint32_t argument=0;const bool given=PE_parse_boot_argn("pagani-dot-pitch",&argument,sizeof(argument));
            *pitch=native_legacy_dot_pitch(given,argument);
            return *pitch?kIOReturnSuccess:kIOReturnNotFound;
        }
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        unsigned writeCombine=1,scans=0;
        PE_parse_boot_argn("pagani-fb-wc",&writeCombine,sizeof(writeCombine));
        PE_parse_boot_argn("pagani-fb-scan",&scans,sizeof(scans));
        scanBudget=scans;
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,writeCombine?kIOMapWriteCombineCache:kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;combined=writeCombine!=0;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
extern "C" void native_legacy_gpu_complete_timed(void *display,int status,uint64_t taken,uint64_t copied) {
    if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status,taken,copied);
}
