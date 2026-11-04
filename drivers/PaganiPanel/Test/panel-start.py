#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""PaganiPanel.cpp as it is, the whole file, against stubs of the kernel and a model of the 13T's registers on a virtual clock: what the header tests cannot reach, the IOService
lifecycle around them. PanelBrightness.h and PanelProbe.h are the real ones; only the kernel's headers, the window type of Mapping.h and the one line that reads the counter are not.

 - bit 8, one DCS (=321): the windows and how they are mapped, the order of everything in start, the writer's word (with PaganiBattery's bit held beside it), nothing published;
 - bit 8 with bit 12 (=4417): the same with one hold, the backlight published, CoreBrightness's dictionary down to the FIFO, a fade, the controller going away;
 - each way out of start: a window that does not map, timers that do not come, a gate closed; the writer free on all of them;
 - =65, the packages' value: its reads counted, nothing else.

    python3 Test/panel-start.py [<PaganiPanel directory> [<kits/Sm8750Platform> <device package>]]

With PANEL_START_TRACE=<file> it writes a hash of everything that happened (reads, writes, sleeps, windows, lines, properties, timers) for every value of pagani-panel without bit 8
on three controllers: two checkouts' files compare the default path register for register.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

if len(sys.argv) == 4:
    folder, platform, device = (Path(argument).resolve() for argument in sys.argv[1:])
else:
    folder = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]).resolve()
    platform = folder.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform'
    device = folder.parents[2] / 'UniXNU-Pagani_SM8750-gpu/package'

KERNEL = r'''
// The kernel as PaganiPanel.cpp sees it.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
typedef int IOReturn;
typedef uint32_t IOOptionBits;
enum { kIOReturnSuccess = 0, kIOReturnError = -1, kIOReturnNoMemory = -2, kIOReturnNoResources = -3, kIOReturnBadArgument = -4 };
enum { kIOMapAnywhere = 1, kIOMapInhibitCache = 2, kIOMapReadOnly = 0x1000, kIOTimerEventSourceOptionsPriorityUser = 8 };
void fake_note(char kind, uint64_t address, uint32_t value, const char *text);
uint64_t fake_now();
void fake_sleep_ms(unsigned ms);
uint64_t fake_ticks();
bool fake_timers_refused();
struct OSObject {
    virtual ~OSObject() {}
    void release() {}
    void retain() {}
};
struct OSNumber : OSObject {
    uint64_t value = 0;
    static OSNumber *withNumber(unsigned long long v, unsigned) { auto n = new OSNumber; n->value = v; return n; }
    uint32_t unsigned32BitValue() const { return (uint32_t)value; }
    uint64_t unsigned64BitValue() const { return value; }
};
struct OSString : OSObject {};
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> objects;
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    void setObject(const char *key, OSObject *object) { objects[key] = object; }
    OSObject *getObject(const char *key) { auto it = objects.find(key); return it == objects.end() ? nullptr : it->second; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSSafeReleaseNULL(object) do { (object) = nullptr; } while (0)
#define OSDeclareDefaultStructors(name)
#define OSDeclareAbstractStructors(name)
#define OSDefineMetaClassAndStructors(name, base)
struct IOWorkLoop;
struct IOService : OSObject {
    std::map<std::string, std::string> properties;
    bool registered = false;
    virtual bool init(OSDictionary * = nullptr) { return true; }
    virtual bool start(IOService *) { return true; }
    virtual void stop(IOService *) {}
    virtual void free() {}
    virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
    virtual IOReturn setProperties(OSObject *) { return kIOReturnSuccess; }
    void note(const char *key, std::string value) { properties[key] = value; fake_note('P', 0, 0, (std::string(key) + "=" + value).c_str()); }
    bool setProperty(const char *key, bool value) { note(key, value ? "true" : "false"); return true; }
    bool setProperty(const char *key, const char *value) { note(key, value); return true; }
    bool setProperty(const char *key, unsigned long long value, unsigned) { note(key, std::to_string(value)); return true; }
    bool setProperty(const char *key, OSObject *) { note(key, "object"); return true; }
    void registerService() { registered = true; fake_note('R', 0, 0, ""); }
};
struct IOTimerEventSource;
typedef void (*TimerAction)(OSObject *, IOTimerEventSource *);
struct IOTimerEventSource : OSObject {
    OSObject *owner = nullptr;
    TimerAction action = nullptr;
    uint64_t due = 0;                        // virtual ticks, 0: none
    static std::vector<IOTimerEventSource *> &all() { static std::vector<IOTimerEventSource *> list; return list; }
    static IOTimerEventSource *timerEventSource(unsigned, OSObject *owner, TimerAction action) {
        if (fake_timers_refused()) return nullptr;
        auto t = new IOTimerEventSource;
        t->owner = owner;
        t->action = action;
        all().push_back(t);
        return t;
    }
    void setTimeoutUS(uint32_t us) { due = fake_now() + (uint64_t)us * 192 / 10; fake_note('T', 0, us, ""); }
    void setTimeoutMS(uint32_t ms) { setTimeoutUS(ms * 1000); }
    void cancelTimeout() { due = 0; }
};
struct IOWorkLoop : OSObject {
    static IOWorkLoop *workLoop() { return new IOWorkLoop; }
    IOReturn addEventSource(IOTimerEventSource *) { return kIOReturnSuccess; }
    IOReturn removeEventSource(IOTimerEventSource *) { return kIOReturnSuccess; }
};
struct IOSimpleLock {};
inline IOSimpleLock *IOSimpleLockAlloc() { return new IOSimpleLock; }
inline void IOSimpleLockLock(IOSimpleLock *) {}
inline void IOSimpleLockUnlock(IOSimpleLock *) {}
inline void IOSleep(unsigned ms) { fake_sleep_ms(ms); }
inline void IOLog(const char *, ...) {}
#define panic(...) abort()
typedef int wait_result_t;
typedef int kern_return_t;
typedef void *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
enum { KERN_SUCCESS = 0 };
kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
inline void thread_deallocate(thread_t) {}
inline uint64_t mach_absolute_time() { return fake_now(); }
inline void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) { *ns = ticks * 625 / 12; }
bool PE_parse_boot_argn(const char *, void *, int);
'''
BACKLIGHT = r'''
#pragma once
#include <IOKit/IOService.h>
struct AppleARMBacklight : IOService { virtual void unqualifiedBacklightMethod() = 0; };
'''
# The window type of Mapping.h: the same methods, every access through the model.
MAPPING = r'''
#pragma once
#include <IOKit/IOLib.h>
#include <PeripheralIO.h>
bool fake_map(uint64_t address, unsigned size, IOOptionBits extra);
void fake_unmap(uint64_t address);
uint32_t fake_read(uint64_t address);
void fake_write(uint64_t address, uint32_t value);
void fake_delay(unsigned us);
struct PeripheralWindow {
    uint64_t address = 0;
    unsigned size = 0;
    bool writable = false;
    bool map(uint64_t where, unsigned bytes, IOOptionBits extra = 0) {
        if (!fake_map(where, bytes, extra)) return false;
        address = where; size = bytes; writable = !(extra & kIOMapReadOnly);
        return true;
    }
    void release() { if (size) fake_unmap(address); size = 0; }
    static uint32_t read(void *context, uint32_t reg) {
        auto &w = *static_cast<PeripheralWindow *>(context);
        if (reg >= w.size) abort();
        return fake_read(w.address + reg);
    }
    static void write(void *context, uint32_t reg, uint32_t value) {
        auto &w = *static_cast<PeripheralWindow *>(context);
        if (reg >= w.size || !w.writable) abort();
        fake_write(w.address + reg, value);
    }
    static void delay(void *, unsigned us) { fake_delay(us); }
    peripheral_io io() { return {this, read, write, delay}; }
};
'''
MAIN = r'''
#include "PaganiPanel/PaganiPanel.cpp"
#include <set>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } ++checks; } while (0)
enum : uint64_t { MS_TICKS = 19200 };
extern "C" void boot_progress_peripheral(unsigned, const char *stage, int32_t rc, uint32_t value, uint64_t) { fake_note('O', (uint64_t)(int64_t)rc, value, stage); }
extern "C" void boot_progress_trace(const char *text) { fake_note('L', 0, 0, text); }
extern "C" void boot_progress_trace_values(const char *text, uint64_t a, uint64_t b) { fake_note('L', a, (uint32_t)b, text); }

struct Event { char kind; uint64_t at, address; uint32_t value, quiet; std::string text; };
struct Thread { thread_continue_t body; void *parameter; };
// The 13T as stage15-smoke-1 read it: DISP_CC_MDSS_AHB_CLK off, and behind it DSI, reading as zero until it runs (the claim A1 tests). `open`: DSI answers without it, the siblings' way.
struct Model {
    uint64_t now = 28800000;                 // 1.5 s
    std::vector<Event> events;
    std::map<uint64_t, uint32_t> regs;
    std::map<std::string, unsigned> args;
    std::map<uint64_t, int> mapped;          // windows on each base now
    std::vector<std::pair<uint64_t, IOOptionBits>> maps;
    std::vector<Thread> threads;
    uint32_t ahb = 0x88000000;
    unsigned polls = 2;
    bool open = false, complete = true, noTimers = false, keep = true, gone = false;
    uint64_t refuse = 0;                     // a window that does not map
    uint64_t spoil = 0;                      // a register the first command leaves as `spoilt`
    uint32_t spoilt = 0;
    bool stops = false, stopped = false;     // the first command ends the frames
    uint64_t hash = 14695981039346656037ull, count = 0;
    Model() {
        const std::pair<uint64_t, uint32_t> read[] = {
            {0x127004, 0x88000003}, {0x127008, 0x8200001}, {0xaf09000, 0xfa22f800}, {0xaf09004, 0x4070000}, {0xaf0b000, 0x222f801}, {0xaf0b004, 0x4088000}, {0xaf08364, 0x100001},
            {0xaf0c004, 0x8000000}, {0xaf0c00c, 0x8000001}, {0xaf0c008, 0x8000001}, {0xaf08010, 0x8000001}, {0xaf08154, 0x100105}, {0xaf08020, 0x8000000}, {0xaf08030, 0x8000001},
            {0xaf00000, 0xe0000005}, {0xaf01000, 0x100}, {0xaf08034, 0x8000001}, {0xaf08038, 0x8000001}, {0xaf08004, 0x8000001}, {0xaf08044, 0x8000001}, {0xaf31000, 0xf0},
            {0xae94000, 0x20090000}, {0xae94004, 0x1f5}, {0xae94084, 0x4}, {0xae9411c, 0x23f}, {0xae9403c, 0x14000000}, {0xae94390, 2}, {0xae9404c, 32}, {0xae94110, 0x20010102}};
        for (auto &r : read) regs[r.first] = r.second;
    }
    bool runs() const { return !gone && (open || ((ahb & 1) && !(ahb >> 31))); }
    uint32_t value(uint64_t address) {
        if (address == 0xaf080b0) {
            if ((ahb & 1) && polls && !--polls) ahb &= ~0x80000000u;
            return ahb;
        }
        const bool behind = address >> 12 == 0xae94 || address >> 12 == 0xae95 || address >> 12 == 0xae36 || address >> 12 == 0xae00;
        if (behind && !runs()) return 0;
        if (address == 0xae94008) return !stopped && now % 159341 < 130560 ? 4u : 0u;
        if (address == 0xf156004) return now % 159341 < 4780 ? 1u : 0u;
        if (address == 0xae360ac) return (uint32_t)(now / 159341);
        auto r = regs.find(address);
        return r == regs.end() ? 0 : r->second;
    }
    void write(uint64_t address, uint32_t data) {
        if (address == 0xaf080b0) {ahb = data; return;}
        if (address >> 12 != 0xae94) abort();                 // the two pages that may be written, and no other
        uint32_t &word = regs[address];
        word = address == 0xae94110 ? (data & 0xa222aa02) | (word & 0x55555555 & ~data) : data;
        if (address == 0xae94090 && complete) regs[0xae94110] |= 1;
        if (address == 0xae9404c && !regs[0xae9415c]) {                 // the restore's last write
            if (spoil) regs[spoil] = spoilt;
            stopped = stops;
        }
    }
    void note(char kind, uint64_t address, uint32_t data, const char *text) {
        const uint32_t quiet = __atomic_load_n(&pagani_panel_probe_quiet, __ATOMIC_RELAXED);
        if (keep) {
            events.push_back({kind, now, address, data, quiet, text});
            return;
        }
        char line[256];
        const int n = snprintf(line, sizeof line, "%c %llu %llx %x %x %s", kind, (unsigned long long)now, (unsigned long long)address, data, quiet, text);
        for (int i = 0; i < n; ++i) hash = (hash ^ (uint8_t)line[i]) * 1099511628211ull;
        ++count;
    }
    std::vector<const Event *> lines(const char *text) const {
        std::vector<const Event *> found;
        for (auto &e : events) if (e.kind == 'L' && e.text.find(text) != std::string::npos) found.push_back(&e);
        return found;
    }
    bool logged(const char *text) const { return !lines(text).empty(); }
    const Event *line(const char *text) const { auto found = lines(text); if (found.empty()) {fprintf(stderr, "no line: %s\n", text); print(); abort();} return found[0]; }
    void print() const {
        for (auto &e : events) if (e.kind == 'L') fprintf(stderr, "%8.3f %x %s\n", (double)e.at / 19200000, e.quiet, e.text.c_str());
    }
    size_t count_of(char kind) const { size_t n = 0; for (auto &e : events) n += e.kind == kind; return n; }
    std::vector<uint32_t> fifo() const {
        std::vector<uint32_t> words;
        for (auto &e : events) if (e.kind == 'W' && e.address == 0xae9417c) words.push_back(e.value);
        return words;
    }
    unsigned triggers() const { unsigned n = 0; for (auto &e : events) n += e.kind == 'W' && e.address == 0xae94090; return n; }
};
static Model *m;
void fake_note(char kind, uint64_t address, uint32_t value, const char *text) { m->note(kind, address, value, text); }
uint64_t fake_now() { return m->now; }
uint64_t fake_ticks() { return ++m->now; }
void fake_sleep_ms(unsigned ms) { m->note('S', 0, ms, ""); m->now += ms * MS_TICKS; }
bool fake_timers_refused() { return m->noTimers; }
void fake_delay(unsigned us) { m->now += (uint64_t)us * 192 / 10; }
uint32_t fake_read(uint64_t address) { m->now += 4; const uint32_t v = m->value(address); m->note('r', address, v, ""); return v; }
void fake_write(uint64_t address, uint32_t value) { m->now += 4; m->note('W', address, value, ""); m->write(address, value); }
bool fake_map(uint64_t address, unsigned, IOOptionBits extra) {
    if (address == m->refuse) return false;
    ++m->mapped[address];
    m->maps.emplace_back(address, extra);
    m->note('M', address, extra, "");
    return true;
}
void fake_unmap(uint64_t address) { if (!--m->mapped[address]) m->mapped.erase(address); m->note('U', address, 0, ""); }
kern_return_t kernel_thread_start(thread_continue_t body, void *parameter, thread_t *thread) { m->threads.push_back({body, parameter}); *thread = nullptr; return KERN_SUCCESS; }
bool PE_parse_boot_argn(const char *name, void *value, int size) {
    auto it = m->args.find(name);
    if (it == m->args.end()) return false;
    memcpy(value, &it->second, (size_t)size < sizeof(unsigned) ? (size_t)size : sizeof(unsigned));
    return true;
}

struct Boot {
    Model model;
    IOService *service = nullptr, *provider = nullptr;
    bool started = false;
    Boot(unsigned options, void (*setup)(Model &) = nullptr, bool keep = true) {
        m = &model;
        model.keep = keep;
        model.args["pagani-panel"] = options;
        if (setup) setup(model);
        IOTimerEventSource::all().clear();
        pagani_panel_set_power(1);
        service = pagani_panel_allocate();
        provider = new IOService;
        service->init();
        // PaganiBattery's USB probe, holding the word beside this driver: a plain store here would let go of it.
        __atomic_store_n(&pagani_panel_probe_quiet, 2u, __ATOMIC_RELAXED);
        started = service->start(provider);
    }
    ~Boot() {
        service->stop(provider);
        CHECK(__atomic_load_n(&pagani_panel_probe_quiet, __ATOMIC_RELAXED) == 2);
        __atomic_store_n(&pagani_panel_probe_quiet, 0u, __ATOMIC_RELAXED);
    }
    // Time passes for ms, the timers that come due run on it.
    void run(unsigned ms) {
        const uint64_t end = model.now + ms * MS_TICKS;
        for (;;) {
            IOTimerEventSource *next = nullptr;
            for (auto t : IOTimerEventSource::all()) if (t->due && t->due <= end && (!next || t->due < next->due)) next = t;
            if (!next) break;
            if (next->due > model.now) model.now = next->due;
            next->due = 0;
            next->action(next->owner, next);
        }
        model.now = end;
    }
    void threads() {
        auto list = model.threads;
        model.threads.clear();
        for (auto &t : list) t.body(t.parameter, 0);
    }
    void ask(uint64_t level, uint32_t fade) {
        auto dict = OSDictionary::withCapacity(3);
        dict->setObject("brightness", OSNumber::withNumber(level, 32));
        dict->setObject("FadePeriod", OSNumber::withNumber(fade, 32));
        CHECK(service->setProperties(dict) == kIOReturnSuccess);
    }
    uint32_t quiet() const { return __atomic_load_n(&pagani_panel_probe_quiet, __ATOMIC_RELAXED); }
    bool free_writer() const { return quiet() == 2; }
    bool has(const char *key) const { return service->properties.count(key) != 0; }
};
static const uint64_t DSI = 0xae94000, DISPCC = 0xaf00000, GCC = 0x127000, PAGE = 0xaf08000;

static void careful() {
    Boot b(321);
    Model &s = b.model;
    CHECK(b.started && b.free_writer());
    // The service's own window first and for good; bit 8's go with the call, read only but for the one page, and no other: the PHY, INTF_1, MDSS and the TE stay unmapped.
    const std::vector<std::pair<uint64_t, IOOptionBits>> maps = {{DSI, 0}, {GCC, kIOMapReadOnly}, {DSI, kIOMapReadOnly}, {DISPCC, kIOMapReadOnly}, {PAGE, 0}};
    CHECK(s.maps == maps);
    CHECK(s.mapped.size() == 1 && s.mapped[DSI] == 1);
    CHECK(s.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}) && s.triggers() == 1);
    unsigned ahbWrites = 0;
    for (auto &e : s.events) if (e.kind == 'W' && e.address == 0xaf080b0) {++ahbWrites; CHECK(e.value == 0x88000001);}
    CHECK(ahbWrites == 1);
    // Every write under the writer's bit, with the USB probe's beside it; a read of DSI with the clock not yet on only as logState always did.
    for (auto &e : s.events) if (e.kind == 'W') CHECK(e.quiet == 3);
    // The order in start, each step's line a whole crumb before it.
    const char *order[] = {"panel: ahb dispcc next", "panel: ahb state gcc 88000003 ahb 88000000", "panel: a1 write next", "panel: a1 ahb 88000000->8000001", "panel: ahb dsi next", "panel: first DSI read next",
                           "panel: start ver 20090000 ctrl 1f5", "panel: first DCS next", "panel: ahb dcs rc 0 clean held one frames ", "panel: dcs held one ver 20090000 ctrl 1f5 trig 4", "panel: dcs ok ver 20090000"};
    uint64_t at = 0;
    for (auto text : order) {CHECK(s.line(text)->at >= at); at = s.line(text)->at;}
    CHECK(s.line("panel: ahb dcs rc 0")->quiet == 3 && s.line("panel: dcs ok ver")->quiet == 2);
    uint64_t first = 0;
    for (auto &e : s.events) if (e.kind == 'W' && e.address >> 12 == 0xae94 && !first) first = e.at;
    CHECK(first >= s.line("panel: first DCS next")->at + 4500 * MS_TICKS);
    CHECK(s.line("panel: dcs ok ver")->at - 28800000 >= 18000 * MS_TICKS);
    // Nothing for the system to find, no tick, and the reports go on.
    CHECK(!b.service->registered && !b.has("backlight-control") && !b.has("IODisplayParameters") && !b.has("NativeSandboxGrantProcess") && b.service->properties["PanelError"] == "0");
    CHECK(s.threads.empty());
    const size_t writes = s.count_of('W');
    b.ask(0x800, 0);
    pagani_panel_set_power(0);
    pagani_panel_set_power(1);
    b.run(46000);
    CHECK(s.count_of('W') == writes);
    // The first report: what the GPU's trace pushed out of the ring, and the registers.
    CHECK(s.lines("panel: ahb dcs rc 0").size() == 2 && s.lines("panel: dcs held one").size() == 2 && s.logged("panel: report ver 20090000 ctrl 1f5") && s.logged("panel: requests 1 writes 1 "));
    printf("ok  =321: its windows, one bit, one DCS, each step behind its line, the writer free, nothing published, the first report repeats the outcome\n");
}

static void slider() {
    Boot b(4417);
    Model &s = b.model;
    CHECK(b.started && b.free_writer() && b.service->registered);
    const std::vector<std::pair<uint64_t, IOOptionBits>> maps = {{DSI, 0}, {GCC, kIOMapReadOnly}, {DSI, kIOMapReadOnly}, {DISPCC, kIOMapReadOnly}, {PAGE, 0}};
    CHECK(s.maps == maps && s.count_of('S') == 1 && s.triggers() == 1);
    for (auto &e : s.events) if (e.kind == 'S') CHECK(e.value == 1000);
    for (auto &e : s.events) if (e.kind == 'W') CHECK(e.quiet == 3);
    // The publication in its order, the parameters first, as the plain path does it, and the level the panel shows.
    CHECK(b.has("IODisplayParameters") && b.service->properties["NativeSandboxGrantProcess"] == "backboardd" && b.service->properties["backlight-control"] == "true" &&
          b.service->properties["brightness"] == "1433" && b.service->properties["PanelMaxBrightness"] == "3515");
    size_t order[3] = {0, 0, 0}, i = 0;
    for (auto &e : s.events) {
        ++i;
        if (e.kind == 'P' && e.text.rfind("IODisplayParameters=", 0) == 0) order[0] = i;
        if (e.kind == 'P' && e.text.rfind("backlight-control=", 0) == 0) order[1] = i;
        if (e.kind == 'R') order[2] = i;
    }
    CHECK(order[0] && order[0] < order[1] && order[1] < order[2]);
    // CoreBrightness's dictionary: at once, a fade of 0.25 s in 16.16, over the maximum, a negative level.
    b.ask(1000, 0);
    b.run(20);
    CHECK(s.fifo().back() == 0xffe80351 && b.service->properties["brightness"] == "1000");
    const unsigned before = s.triggers();
    b.ask(2000, 0x4000);
    b.run(400);
    CHECK(s.fifo().back() == 0xffd00751 && s.triggers() >= before + 14 && s.triggers() <= before + 18 && b.service->properties["brightness"] == "2000");
    b.ask(0xffffff, 0);
    b.run(20);
    CHECK(s.fifo().back() == 0xffbb0d51 && b.service->properties["brightness"] == "3515");
    b.ask(0xffffffffull, 0);
    b.run(20);
    CHECK(s.fifo().back() == 0xff000051);
    // Bit 6: the system's display off and on send nothing.
    const unsigned sent = s.triggers();
    pagani_panel_set_power(0);
    b.run(100);
    pagani_panel_set_power(1);
    b.run(100);
    CHECK(s.triggers() == sent);
    // The controller goes away: nothing more is written, the requests stop in their time, the log says so, and it does not come back with the controller.
    s.gone = true;
    b.ask(0x700, 0);
    b.run(70000);
    CHECK(s.triggers() == sent && s.logged("panel: DSI failed -19 at packet 0 of the brightness sequence; requests disabled") && b.has("PanelError"));
    s.gone = false;
    b.ask(0x700, 0);
    b.run(1000);
    CHECK(s.triggers() == sent);
    printf("ok  =4417: one hold of 1 s, published in its order, the dictionary to the FIFO at once and faded, display off sends nothing, a controller that goes away ends the commands for the boot\n");
}

static void waysOut() {
    {   // The writable page does not map: no hold, nothing written, the boot as without the bit but for the commands.
        Boot b(4417, [](Model &s) { s.refuse = PAGE; });
        Model &s = b.model;
        CHECK(b.started && b.free_writer() && !s.count_of('W') && !s.count_of('S') && s.logged("panel: ahb map failed ahb") && !b.service->registered && s.mapped.size() == 1 && s.mapped[DSI] == 1);
        CHECK(s.logged("panel: ahb dcs rc -19 no read held shape") && s.logged("panel: dcs held shape ver 0"));
        b.run(46000);
        CHECK(s.lines("panel: dcs held shape").size() == 2);
    }
    {   // No timers: start gives up, and it does not leave the log's writer stopped for the boot.
        Boot b(321, [](Model &s) { s.noTimers = true; });
        CHECK(!b.started && b.free_writer() && b.model.logged("panel: timers failed") && b.model.triggers() == 0);
    }
    {   // A gate closed in the state: no write to anything, the writer free, DSI as ever.
        Boot b(321, [](Model &s) { s.regs[0xaf08360] = 0x80000000; });
        CHECK(b.started && b.free_writer() && !b.model.count_of('W') && b.model.logged("panel: a1 held root off") && b.model.logged("panel: no dcs ver 0") && !b.service->registered);
    }
    {   // The first DCS finds no answer: not published with bit 12, whatever bit 1 says, and no tick ever comes for it.
        Boot b(4419, [](Model &s) { s.complete = false; });
        CHECK(b.started && b.free_writer() && b.model.triggers() == 1 && !b.service->registered && !b.has("backlight-control") && b.model.logged("panel: ahb dcs rc -110 no read held lost"));
        b.ask(0x700, 0);
        b.run(70000);
        CHECK(b.model.triggers() == 1 && b.model.logged("panel: dcs held lost ver 20090000"));
    }
    {   // The command goes and leaves the controller otherwise: start takes that for what it is, nothing is published, no tick comes.
        Boot b(4417, [](Model &s) { s.spoil = 0xae9411c; s.spoilt = 0x3f; });
        CHECK(b.started && b.free_writer() && b.model.triggers() == 1 && !b.service->registered && !b.has("backlight-control") && b.service->properties["PanelError"] == "4294967291");
        CHECK(b.model.logged("panel: ahb dcs rc -5 config held lost") && b.model.logged("panel: no dcs ver 20090000") && b.model.logged("panel: dcs held lost"));
        b.ask(0x700, 0);
        b.run(70000);
        CHECK(b.model.triggers() == 1);
    }
    {   // Every register in its place and the frames gone: the look before it is what says so.
        Boot b(4417, [](Model &s) { s.stops = true; });
        CHECK(b.started && b.free_writer() && !b.service->registered && b.model.logged("panel: ahb dcs rc -5 stream held lost frames 2->0"));
    }
    {   // On already, bit 8 with bit 7's stages behind it: the probe's thread starts as ever.
        Boot b(321 | 128, [](Model &s) { s.ahb = 0x8000001; });
        CHECK(b.started && b.free_writer() && !b.model.logged("panel: a1 write next") && b.model.logged("panel: ahb 8000001 already on") && b.model.threads.size() == 1);
    }
    printf("ok  ways out of start: a window, the timers, a gate, a command without an answer, on already; the writer free on all of them\n");
}

static void packages() {
    Boot b(65);
    Model &s = b.model;
    CHECK(b.started && b.free_writer());
    // The service's window, 14 reads, one of 0x4 by the refused first DCS, 14 again; no write, no sleep, no thread, nothing published.
    CHECK(s.maps.size() == 1 && s.maps[0].first == DSI && !s.count_of('W') && !s.count_of('S') && s.threads.empty() && !b.service->registered && s.count_of('r') == 14 + 1 + 14);
    CHECK(b.service->properties["PanelError"] == "4294967277" && s.logged("panel: no dcs ver 0") && !s.logged("panel: ahb"));
    printf("ok  =65: 29 reads, no write, no sleep, no thread, nothing published\n");
}

// The default path as a number: every value without bit 8, on three controllers, through start, the probe's thread, a minute of timers with requests and a display off.
static void trace(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) abort();
    for (unsigned bits = 0; bits < 1u << 13; ++bits) {
        if (!(bits & 1) || (bits & 256)) continue;
        for (unsigned state = 0; state < 3; ++state) {
            static unsigned which;
            which = state;
            Boot b(bits, [](Model &s) {
                s.keep = false;
                if (which == 1) s.open = true;
                if (which == 2) {s.open = true; s.regs[0xae94004] = 0;}
            }, false);
            b.threads();
            b.ask(0x800, 0x4000);
            b.run(1000);
            pagani_panel_set_power(0);
            b.run(1000);
            b.ask(0x400, 0);
            pagani_panel_set_power(1);
            b.run(60000);
            fprintf(out, "%u %u %d %016llx %llu\n", bits, state, b.started, (unsigned long long)b.model.hash, (unsigned long long)b.model.count);
        }
    }
    fclose(out);
}

int main() {
    if (const char *path = getenv("PANEL_START_TRACE")) {trace(path); return 0;}
    careful(); slider(); waysOut(); packages();
    printf("PASS: PaganiPanel.cpp against the kernel's stubs, %u checks\n", checks);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='panel-start-') as work:
    work = Path(work)
    (work / 'PaganiPanel').mkdir()
    (work / 'PaganiInput').mkdir()
    (work / 'BootProgress').mkdir()
    (work / 'stubs/IOKit').mkdir(parents=True)
    source = (folder / 'PaganiPanel.cpp').read_text()
    counter = '__asm__ volatile("isb\\n\\tmrs %0, cntpct_el0":"=r"(value));'
    assert source.count(counter) == 1
    (work / 'PaganiPanel/PaganiPanel.cpp').write_text(source.replace(counter, 'value=fake_ticks();'))
    for name in ('PanelBrightness.h', 'PanelProbe.h', 'PanelPower.h'):
        shutil.copy(folder / name, work / 'PaganiPanel' / name)
    shutil.copy(folder.parent / 'BootProgress/PeripheralStatus.h', work / 'BootProgress/PeripheralStatus.h')
    (work / 'PaganiPanel/OriginalBacklightABI.h').write_text(BACKLIGHT)
    (work / 'PaganiInput/Mapping.h').write_text(MAPPING)
    (work / 'stubs/fake-kernel.h').write_text(KERNEL)
    for name in ('IOKit/IOService.h', 'IOKit/IOLib.h', 'IOKit/IOWorkLoop.h', 'IOKit/IOTimerEventSource.h', 'IOKit/IOLocks.h', 'kern/clock.h', 'kern/thread.h', 'pexpert/pexpert.h', 'libkern/c++/OSNumber.h'):
        path = work / 'stubs' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#pragma once\n#include "fake-kernel.h"\n')
    (work / 'main.cpp').write_text(MAIN)
    binary = work / 'panel-start'
    trace = os.environ.get('PANEL_START_TRACE')
    sanitize = [] if trace else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-overloaded-virtual', *sanitize, '-I', str(work), '-I', str(work / 'stubs'), '-I', str(platform), '-I', str(device),
                    str(work / 'main.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
