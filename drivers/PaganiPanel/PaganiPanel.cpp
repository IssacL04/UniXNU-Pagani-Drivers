// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalBacklightABI.h"
#include "../PaganiInput/Mapping.h"
#include <PaganiPeripherals.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "../BootProgress/PeripheralStatus.h"
#include "PanelPower.h"
#include "PanelBrightness.h"
static_assert(PAGANI_BRIGHTNESS_DEFAULT<=PAGANI_BRIGHTNESS_MAX && PAGANI_BRIGHTNESS_MAX<=PAGANI_PANEL_DBV_NORMAL && PAGANI_PANEL_DBV_NORMAL<=PAGANI_PANEL_DBV_MAX,"Brightness levels within the panel's");

// The display's power as the system set it last, for the screen-off policies of PaganiArmCpu (pagani-screenoff bit 0) and Adreno830
// (bits 1 and 3): bit 0 set while the display is off, the number of changes above it (a reader that missed an off and an on still
// sees the count move). Written here only; a weak zero word like PaganiInput's touch counter, so the kernel link keeps one word.
extern "C" {
__attribute__((weak)) volatile uint32_t pagani_screen_state;
}

// The display PM callback publishes a value, never a pointer into a detachable service; the timer only exists while the service
// runs, and a started service stays until reboot. Three threads arm that timer (CoreBrightness's in setProperties, the display PM's
// here, the tick itself), and IOTimerEventSource::wakeAtTime increments its generation and reads it back for the thread call without
// a lock: a thread overtaken between the two leaves the call with an old generation, and timeoutAndRelease then drops the tick. The
// dropped tick could be the one that sends 0x29, so every arm takes this lock.
static unsigned requestedPower=1;
static IOTimerEventSource *volatile panelTimer;
static IOSimpleLock *panelTimerLock;
static void panelArm(IOTimerEventSource *timer,uint32_t us) {
    IOSimpleLockLock(panelTimerLock);timer->setTimeoutUS(us);IOSimpleLockUnlock(panelTimerLock);
}
extern "C" void pagani_panel_set_power(unsigned on) {
    __atomic_store_n(&requestedPower,on?1:0,__ATOMIC_RELEASE);
    const uint32_t state=__atomic_load_n(&pagani_screen_state,__ATOMIC_RELAXED);
    if ((state&1u)!=(on?0u:1u)) __atomic_store_n(&pagani_screen_state,((state|1u)+1u)|(on?0u:1u),__ATOMIC_RELEASE);
    if (IOTimerEventSource *timer=__atomic_load_n(&panelTimer,__ATOMIC_ACQUIRE)) panelArm(timer,1);
}

// The events ring keeps the last 32 KiB and a run is read after up to 90 minutes: the first report comes when the boot's records are out,
// the next ones once a minute while the counters move and every ten minutes when they do not (pagani_panel_report_due).
enum { PANEL_REPORT_FIRST_MS=45000, PANEL_REPORT_MS=60000 };

// PanelProbe.h on the kernel: pagani-panel bits 7, 9 and 10, on a thread of their own, with their sleeps and busy waits away from the work loop, and bit 8 in start. Nothing
// is shared with the service: the windows are this thread's, read only but for A1's one page, and go with it. A line goes to the log; `keep` (may be null) also takes a copy.
// What goes wrong before the first stage waits for that moment to say so: at 1.5 s the GPU's trace pushes a line out of the events ring within milliseconds.
static void panelLog(void *keep,const char *format,...) {
    char line[PANEL_PROBE_LINE];
    va_list arguments;
    va_start(arguments,format);vsnprintf(line,sizeof line,format,arguments);va_end(arguments);
    boot_progress_trace(line);
    if (keep) strlcpy(static_cast<char *>(keep),line,PANEL_PROBE_LINE);
}
static void panelProbeSleep(void *,unsigned ms) {IOSleep(ms);}
extern "C" volatile uint32_t pagani_panel_probe_quiet;
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;
// Bit 0 of the word: UfsLog.c's writer stops while it is not 0, and PaganiBattery's USB probe, whose reads fall in A1's seconds, holds it with bit 1 (BatteryUsb.h).
static void panelProbeQuiet(void *,unsigned on) {
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet,1u,__ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet,~1u,__ATOMIC_RELEASE);
}
static uint64_t panelProbeTicks(void *) {uint64_t value;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(value));return value;}
static unsigned panelProbeDisplay(void *) {return __atomic_load_n(&requestedPower,__ATOMIC_ACQUIRE);}
static const panel_probe_window *panelProbeMap(PeripheralWindow *windows,panel_probe_host &host,unsigned stages);
static void panelProbeThread(void *parameter,wait_result_t) {
    const unsigned stages=(unsigned)(uintptr_t)parameter;
    PeripheralWindow windows[PANEL_PROBE_BLOCKS];
    panel_probe_host host{};
    const panel_probe_window *unmapped=panelProbeMap(windows,host,stages);
    if (!unmapped) panel_probe_run(&host,stages);
    else {IOSleep(panel_probe_first_ms(stages));boot_progress_trace_values("panel: probe map failed",unmapped->base,unmapped->size);}
    for (PeripheralWindow &window:windows) window.release();
}
// What the kernel gives PanelProbe.h, and the windows of `stages`: 0, or the one that could not be mapped.
static void panelProbeHost(panel_probe_host &host) {
    host.log=panelLog;host.sleep_ms=panelProbeSleep;host.ticks=panelProbeTicks;host.display_on=panelProbeDisplay;host.quiet=panelProbeQuiet;
}
static const panel_probe_window *panelProbeMap(PeripheralWindow *windows,panel_probe_host &host,unsigned stages) {
    panelProbeHost(host);
    for (unsigned i=0;i<PANEL_PROBE_BLOCKS;++i) {
        const panel_probe_window &window=panel_probe_windows[i];
        if (!(window.stages&stages)) continue;
        if (windows[i].map(window.base,window.size,i==PANEL_BLOCK_AHB?0:kIOMapReadOnly)) host.io[i]=windows[i].io();
        else return &window;
    }
    return nullptr;
}

class PaganiPanel : public AppleARMBacklight {
    OSDeclareDefaultStructors(PaganiPanel)
    PeripheralWindow dsi;
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr,*reporter=nullptr;
    bool added=false,running=false,failed=false;
    // boot-arg pagani-panel (PanelBrightness.h).
    unsigned options=0,sendMode=0;
    pagani_panel_reports reports{};
    pagani_panel_run run{};
    // The newest brightness asked for (PanelBrightness.h), taken by the tick.
    volatile uint64_t request=0;
    uint32_t requests=0;
    int lastError=0;
    // What only start says (A0 or bit 8's outcome, the gate of pagani_panel_dcs_gate, the probe's thread) comes at boot, where the events ring does not keep it: it goes
    // again with the first report.
    char earlyLines[3][PANEL_PROBE_LINE]={};
    // pagani-screenoff bit 2: the vendor's off and on sequences with sleep mode (PanelPower.h) instead of 0x28/0x29 alone.
    bool sleepMode=false;
    void observe(const char *stage,int rc,uint32_t value) {
        boot_progress_peripheral(PAGANI_PANEL,stage,rc,value,run.power.packets+run.writes);
    }
    static uint64_t nowNs() {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
    static int sendPacket(void *context,uint8_t type,const uint8_t *payload,unsigned length) {
        auto self=static_cast<PaganiPanel *>(context);auto io=self->dsi.io();
        return pagani_panel_send_packet(&self->run,&io,self->sendMode,type,payload,length);
    }
    static void forcing(void *context) {static_cast<PaganiPanel *>(context)->logState("forced");}
    // The controller as it is now, in two U records: the first DSI read of this port is on no log (the events ring keeps the last 32 KiB).
    void logState(const char *when,dsi_panel_state *seen=nullptr) {
        auto io=dsi.io();dsi_panel_state s;dsi_panel_snapshot(&io,&s);
        if (seen) *seen=s;
        char line[120];
        snprintf(line,sizeof line,"panel: %s ver %x ctrl %x st %x fifo %x dma %x trig %x",when,s.version,s.ctrl,s.status,s.fifo,s.dma,s.trigger);
        boot_progress_trace(line);
        snprintf(line,sizeof line,"panel: %s lanes %x int %x clk %x tpg %x ack %x to %x sec %x split %x",when,s.lanes,s.ints,s.clocks,s.tpg,s.ack,s.timeout,s.secure,s.split);
        boot_progress_trace(line);
    }
    void logCounters() {
        char line[120];
        snprintf(line,sizeof line,"panel: requests %u writes %u retried %u shown %u goal %u rc %d mode %u power %u/%u off %u on %u%s",
                 __atomic_load_n(&requests,__ATOMIC_RELAXED),run.writes,run.retried,run.bright.shown,run.bright.to,lastError,sendMode,run.shown,
                 __atomic_load_n(&requestedPower,__ATOMIC_RELAXED),run.power.offs,run.power.ons,__atomic_load_n(&failed,__ATOMIC_RELAXED)?" stopped":"");
        boot_progress_trace(line);
    }
    // The ramoops log (U records): each finished sequence of the sleep option once, and the failure that disabled the panel.
    void noteSequence(bool woke) {
        char line[96];
        const pagani_panel_power &sequence=run.power;
        const char *what=sequence.last==PAGANI_PANEL_ASLEEP?"display off, sleep in":sequence.last==PAGANI_PANEL_BLANK?"display off":
                         woke?"sleep out, vendor writes, display on":"display on";
        snprintf(line,sizeof line,"panel: %s in %llu ms (asleep %u, woken %u, packets %u)",what,
                 (unsigned long long)(sequence.lastNs/1000000),sequence.sleeps,sequence.wakes,sequence.packets);
        boot_progress_trace(line);
    }
    void stopCommands(int rc) {
        __atomic_store_n(&failed,true,__ATOMIC_RELEASE);setProperty("PanelError",(uint32_t)rc,32);
        char line[96];
        snprintf(line,sizeof line,"panel: DSI failed %d at packet %u of the %s sequence; requests disabled",rc,run.power.next,
                 !run.power.seq?"brightness":run.power.seq==pagani_panel_wake?"wake":run.power.seq==pagani_panel_off?"off":"on");
        boot_progress_trace(line);
        logState("failed");logCounters();
        IOLog("Pagani panel command failed: %d; requests disabled\n",rc);
    }
    void publish() {
        auto b=OSDictionary::withCapacity(3),p=OSDictionary::withCapacity(1);
        auto lo=OSNumber::withNumber(0ULL,32),hi=OSNumber::withNumber(PAGANI_BRIGHTNESS_MAX,32),v=OSNumber::withNumber(run.bright.shown,32);
        if (b && p && lo && hi && v) {
            b->setObject("min",lo);b->setObject("max",hi);b->setObject("value",v);
            p->setObject("brightness",b);setProperty("IODisplayParameters",p);
        }
        OSSafeReleaseNULL(lo);OSSafeReleaseNULL(hi);OSSafeReleaseNULL(v);OSSafeReleaseNULL(b);OSSafeReleaseNULL(p);
        setProperty("brightness",run.bright.shown,32);setProperty("PanelDisplayOn",run.shown!=0);
        if (sleepMode) setProperty("PanelAsleep",run.power.state==PAGANI_PANEL_ASLEEP);
    }
    // All the panel's DSI traffic runs here, one tick at a time on the service's own work loop (the first DCS in start comes before the
    // timer can fire). What the tick does is PanelBrightness.h; here only what it leaves for the kernel side.
    static void tick(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<PaganiPanel *>(owner);
        if (!__atomic_load_n(&self->running,__ATOMIC_ACQUIRE) || __atomic_load_n(&self->failed,__ATOMIC_ACQUIRE)) return;
        const uint32_t sent=self->run.power.packets+self->run.writes;
        pagani_panel_tick_result result;
        pagani_panel_tick(&self->run,nowNs(),pagani_panel_want(self->options,__atomic_load_n(&requestedPower,__ATOMIC_ACQUIRE)),
                          __atomic_exchange_n(&self->request,0ull,__ATOMIC_ACQUIRE),sendPacket,self,&result);
        if (result.rc || self->run.power.packets+self->run.writes!=sent) self->observe(result.rc?"DSI not sent":"DSI sent",result.rc,self->run.bright.shown);
        if (result.finished && self->sleepMode) self->noteSequence(result.woke);
        if (result.rc) self->lastError=result.rc;
        if (result.stopped) {self->stopCommands(result.rc);return;}
        if (result.rc && self->run.retries==1) {self->logState("retry");self->logCounters();}
        if (result.publish) self->publish();
        if (result.next_ms) panelArm(self->timer,result.next_ms*1000);
    }
    // Also on a service that stayed unpublished: the first read of a controller that refuses must reach the log.
    static void report(OSObject *owner,IOTimerEventSource *sender) {
        auto self=static_cast<PaganiPanel *>(owner);
        const uint32_t seen=__atomic_load_n(&self->requests,__ATOMIC_RELAXED)+self->run.writes+self->run.retried+self->run.power.offs+self->run.power.ons+(uint32_t)self->lastError;
        for (char *line:self->earlyLines) if (line[0]) {boot_progress_trace(line);line[0]=0;}
        const unsigned due=pagani_panel_report_due(&self->reports,seen,__atomic_load_n(&requestedPower,__ATOMIC_ACQUIRE),__atomic_load_n(&self->failed,__ATOMIC_ACQUIRE));
        if (due&PAGANI_PANEL_REPORT_REGISTERS) self->logState("report");
        if (due&PAGANI_PANEL_REPORT_COUNTERS) self->logCounters();
        sender->setTimeoutMS(PANEL_REPORT_MS);
    }
    // pagani-panel bit 11. Its window goes with the call: the only writable one of the probe, and only for this.
    void setGccAhb() {
        PeripheralWindow gcc;
        const panel_probe_window &window=panel_probe_windows[PANEL_BLOCK_GCC];
        if (!gcc.map(window.base,window.size)) {panelLog(earlyLines[0],"panel: a0 map failed");return;}
        auto clock=gcc.io();auto io=dsi.io();
        panel_probe_a0(&clock,&io,panelLog,earlyLines[0]);
        gcc.release();
    }
    // pagani-panel bit 8, the formal path (PanelProbe.h panel_probe_ahb). Its windows go with the call, the writable page first of all. The writer stays held: pagani_panel_ahb_after lets it go.
    bool setDispAhb() {
        PeripheralWindow windows[PANEL_PROBE_BLOCKS];
        panel_probe_host host{};
        bool on=false;
        if (const panel_probe_window *unmapped=panelProbeMap(windows,host,PANEL_PROBE_AHB)) panelLog(earlyLines[0],"panel: ahb map failed %s",unmapped->name);
        else {panel_probe probe{};probe.host=&host;on=panel_probe_ahb(&probe,pagani_panel_ahb_careful(options)?1u:0u)!=0;}
        for (PeripheralWindow &window:windows) window.release();
        return on;
    }
    void startProbe(unsigned stages) {
        thread_t thread;
        if (kernel_thread_start(panelProbeThread,reinterpret_cast<void *>((uintptr_t)stages),&thread)==KERN_SUCCESS) thread_deallocate(thread);
        else panelLog(earlyLines[2],"panel: probe thread failed");
    }
    void cleanup() {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        __atomic_store_n(&panelTimer,(IOTimerEventSource *)nullptr,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (added) loop->removeEventSource(timer);}
        if (reporter) {reporter->cancelTimeout();if (added) loop->removeEventSource(reporter);}
        added=false;OSSafeReleaseNULL(timer);OSSafeReleaseNULL(reporter);OSSafeReleaseNULL(loop);dsi.release();
    }
public:
    // The original constructor supplies storage and ancestry; no Apple hardware lifecycle is called.
    bool init(OSDictionary *properties=nullptr) override {return IOService::init(properties);}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    bool start(IOService *provider) override {
        observe("service start",0,0);
        if (!IOService::start(provider)) {observe("service start",kIOReturnError,0);return false;}
        if (!dsi.map(PAGANI_DSI_BASE,0x1000)) {
            observe("DSI map",kIOReturnNoMemory,0);boot_progress_trace("panel: DSI map failed");cleanup();IOService::stop(provider);return false;
        }
        PE_parse_boot_argn("pagani-panel",&options,sizeof options);
        if (pagani_panel_gcc_ahb(options)) setGccAhb();
        const bool formal=pagani_panel_ahb_start(options),ahb=formal && setDispAhb();
        // In DRAM before the access: a read that stalls the CPU leaves this as the last line.
        boot_progress_trace("panel: first DSI read next");
        dsi_panel_state seen;
        logState("start",&seen);
        // pagani-screenoff bit 2: display off also enters sleep mode, display on leaves it with the vendor's writes (PanelPower.h).
        unsigned screenoff=0;PE_parse_boot_argn("pagani-screenoff",&screenoff,sizeof screenoff);
        sleepMode=(screenoff&4u)!=0;pagani_panel_run_init(&run,PAGANI_BRIGHTNESS_DEFAULT,sleepMode);
        run.ahb=ahb;
        if (sleepMode) setProperty("PanelSleepMode",true);
        sendMode=pagani_panel_send_mode(options);
        // Not the default thread call group of every IOTimerEventSource, the display's vsync timer among them, at priority 92: a command
        // polls for the gap between two frames with IODelay, which blocks on this port (no cpu_idle_latency) and would come back every
        // 100 us ahead of whatever runs on its CPU. At the 31 of the user group a poll or a fade step takes a CPU nothing else wants;
        // running late under load shows nowhere.
        loop=IOWorkLoop::workLoop();timer=IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityUser,this,tick);
        reporter=IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityUser,this,report);
        if (!panelTimerLock) panelTimerLock=IOSimpleLockAlloc();
        added=loop && timer && reporter && panelTimerLock && loop->addEventSource(timer)==kIOReturnSuccess && loop->addEventSource(reporter)==kIOReturnSuccess;
        if (!added) {if (formal) panelProbeQuiet(nullptr,0);observe("timer",kIOReturnNoResources,0);boot_progress_trace("panel: timers failed");cleanup();IOService::stop(provider);return false;}
        boot_progress_trace("panel: first DCS next");
        // Bit 8: the first DCS with a look at what it leaves. Its last line comes with the first report: the GPU's trace is next, which takes the boot's lines out of the events ring.
        panel_probe_host host{};
        uint32_t frames=0;
        if (formal) {
            panelProbeHost(host);host.context=earlyLines[0];host.io[PANEL_BLOCK_DSI]=dsi.io();
            frames=pagani_panel_ahb_before(&host,&run,options);
        }
        int rc=pagani_panel_start_dcs(&run,options,&seen,&sendMode,sendPacket,forcing,this);
        if (formal) rc=pagani_panel_ahb_after(frames,&host,&run,options,rc,&seen);
        if (run.held) panelLog(earlyLines[1],"panel: dcs held %s ver %x ctrl %x trig %x",pagani_panel_held_name(run.held),seen.version,seen.ctrl,seen.trigger);
        lastError=rc;observe("first DCS",rc,run.bright.shown);
        logState(rc?"no dcs":"dcs ok");logCounters();
        reporter->setTimeoutMS(PANEL_REPORT_FIRST_MS);
        if (const unsigned stages=pagani_panel_probe_stages(options)) startProbe(stages);
        if (!pagani_panel_publishes(options,rc)) {
            // The system runs without a backlight, as it does without this driver: the screen keeps the boot loader's level. The service
            // stays, never registered (no class match, nothing for CoreBrightness), for its reports.
            observe(rc==-19?"DSI disabled":rc?"DSI no answer":"DSI confirmed",rc,0);
            setProperty("PanelError",(uint32_t)rc,32);
            __atomic_store_n(&failed,true,__ATOMIC_RELEASE);
            return true;
        }
        // Before anything can look: CoreBrightness refuses a display without IODisplayParameters, and QuartzCore waits for the libEDR
        // server CoreBrightness starts while opening that display as soon as backlight-control makes CADeviceHasBacklight true.
        publish();
        const bool backlight=pagani_panel_backlight(options);
        if (options&PAGANI_PANEL_OPTION_NO_DISPLAY_POWER) boot_progress_trace("panel: no 0x28/0x29 (pagani-panel bit 6)");
        if (backlight) {
            // The profiles of this kernelcache have no FadePeriod literal (a string search, which a pattern rule would pass), CoreBrightness
            // sends it with every brightness, and sheng saw the sandbox refuse that whole dictionary before setProperties runs, with the
            // same ancestry (int-276).
            setProperty("NativeSandboxGrantProcess","backboardd");
            // Publish only with real AppleARMBacklight ancestry: CoreBrightness uses IOObjectConformsTo.
            setProperty("backlight-control",true);setProperty("Built-In",true);
        } else boot_progress_trace("panel: backlight not published (pagani-panel bit 4)");
        setProperty("PanelMaxBrightness",PAGANI_BRIGHTNESS_MAX,32);
        setProperty("PanelRefreshSource","firmware timing; software presentation at 60 Hz");
        __atomic_store_n(&running,true,__ATOMIC_RELEASE);
        __atomic_store_n(&panelTimer,timer,__ATOMIC_RELEASE);
        if (backlight) registerService();
        // A power change that came before panelTimer was set found no timer to arm.
        panelArm(timer,1);
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        auto dict=OSDynamicCast(OSDictionary,properties);
        if (!dict) return kIOReturnBadArgument;
        auto value=OSDynamicCast(OSNumber,dict->getObject("brightness"));
        if (!value) return kIOReturnSuccess;
        auto fade=OSDynamicCast(OSNumber,dict->getObject("FadePeriod"));
        __atomic_add_fetch(&requests,1,__ATOMIC_RELAXED);
        const uint32_t period=fade?pagani_panel_fade(options,fade->unsigned32BitValue()):0;
        // Handed to the work loop without waiting for it: the caller is CoreBrightness's brightness thread (sheng int-277). A later
        // request replaces one not yet taken. The timer carries it (sheng int-278: an interrupt source signalled here wrote nothing).
        __atomic_store_n(&request,pagani_brightness_request(value->unsigned64BitValue(),period,PAGANI_BRIGHTNESS_MAX),__ATOMIC_RELEASE);
        if (__atomic_load_n(&running,__ATOMIC_ACQUIRE) && !__atomic_load_n(&failed,__ATOMIC_ACQUIRE)) panelArm(timer,1);
        return kIOReturnSuccess;
    }
    void stop(IOService *provider) override {cleanup();IOService::stop(provider);}
    void free() override {cleanup();IOService::free();}
    void unqualifiedBacklightMethod() override {
        panic("PaganiPanel: unsupported AppleARMBacklight slot 168");
    }
};
OSDefineMetaClassAndStructors(PaganiPanel,AppleARMBacklight)
extern "C" IOService *pagani_panel_allocate(void) {return new PaganiPanel;}
