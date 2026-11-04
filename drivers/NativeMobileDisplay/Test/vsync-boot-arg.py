#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The four places that read boot-arg sheng-vsync in NativeLegacyDisplay.cpp, cut out of the source as they are and run against stubs of the family: whether the tick
starts, at what period, what the display reports, whether the refresh properties go out. The arithmetic is display-timing.cpp's; this is the wiring.

    python3 Test/vsync-boot-arg.py [<NativeMobileDisplay directory>]
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
source = (folder / 'NativeLegacyDisplay.cpp').read_text()


def cut(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


parts = (cut('    unsigned refreshHz() const {', "    // Selector 72's registration"), cut('    void startVsync() {', '    void stopVsync() {'),
         cut('    void registerService(IOOptionBits options=0) override {', '    bool isMainDisplay()'),
         cut('    bool getRefreshAttributes(uint64_t *words,bool fixed) override {', '    // A swap the client'))
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "NativeDisplayTiming.h"
struct OSObject {};
using IOReturn = int;
using IOOptionBits = unsigned;
constexpr int kIOReturnSuccess = 0;
static unsigned tickArg, tickSet, rateArg, rateSet, fixedArg = 1, properties, started, observedHz;
static bool PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(size == 4);
    if (!strcmp(name, "sheng-vsync") && tickSet) {*static_cast<unsigned *>(value) = tickArg; return true;}
    if (!strcmp(name, "pagani-vsync") && rateSet) {*static_cast<unsigned *>(value) = rateArg; return true;}
    if (!strcmp(name, "sheng-vsync-fixed")) {*static_cast<unsigned *>(value) = fixedArg; return true;}
    if (!strcmp(name, "offline-legacy-register")) {*static_cast<unsigned *>(value) = 1; return true;}
    return false;
}
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *ticks) {*ticks = ns * 96 / 5000;}     // 19.2 MHz
static void native_legacy_vsync_observed(unsigned event, unsigned hz) {if (event == 4) {++started; observedHz = hz;} else assert(event == 5);}
static void native_legacy_backend_guard(unsigned, void *, void *) {assert(false);}
static void native_legacy_registration_observed(void *) {}
static bool native_legacy_set_refresh_properties(void *) {++properties; return true;}
struct IOTimerEventSource {
    static IOTimerEventSource *timerEventSource(OSObject *, void (*)(OSObject *, IOTimerEventSource *)) {return new IOTimerEventSource;}
    void release() {delete this;}
};
struct IOWorkLoop {
    int addEventSource(IOTimerEventSource *) {return kIOReturnSuccess;}
    IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *), OSObject *owner) {return action(owner, nullptr, nullptr, nullptr, nullptr);}
};
struct IOService {virtual ~IOService() {} virtual void registerService(IOOptionBits = 0) {}};
struct Family : OSObject, IOService {virtual bool getRefreshAttributes(uint64_t *words, bool) {if (!words) return false; for (unsigned i = 0; i < 6; ++i) words[i] = 100 + i; return true;}};
#define __builtin_return_address(n) nullptr
#define override
#define IOMobileFramebufferLegacy Family
struct NativeLegacyDisplay : Family {
    bool primary = true, bootMapping = true;
    IOTimerEventSource *vsyncTimer = nullptr;
    NativeTeLock te;
    uint64_t vsyncInterval = 0, ticksPerSecond = 19200000;
    unsigned vsyncHz = 0;
    IOWorkLoop loop;
    IOWorkLoop *getWorkLoop() {return &loop;}
    static void vsyncFired(OSObject *, IOTimerEventSource *) {}
    void armVsync() {}
@PARTS@
};
#undef override
#undef IOMobileFramebufferLegacy
using Display = NativeLegacyDisplay;
struct Result {bool on; uint64_t interval; unsigned hz, properties; uint64_t words[4];};
static Result boot(bool setTick, unsigned tick, bool setRate = false, unsigned rate = 0, bool locked = false) {
    tickSet = setTick; tickArg = tick; rateSet = setRate; rateArg = rate; properties = started = observedHz = 0;
    Display d;
    if (locked) {d.te.valid = true; d.te.periodQ16 = uint64_t(159341) << 16;}
    d.startVsync();
    d.registerService();
    Result r = {d.vsyncTimer != nullptr, d.vsyncInterval, observedHz, properties, {0, 0, 0, 0}};
    uint64_t words[6] = {};
    assert(d.getRefreshAttributes(words, false));
    memcpy(r.words, words, sizeof r.words);
    if (d.vsyncTimer) d.vsyncTimer->release();
    return r;
}
int main() {
    // Off, as ever: absent, 0, out of both ranges. The family's own words stay.
    for (unsigned off : {0u, 23u, 241u, 4166u, 41668u}) {
        const Result r = boot(true, off);
        assert(!r.on && !r.properties && !started && r.words[0] == 100);
    }
    assert(!boot(false, 0).on);
    // The package's sheng-vsync=120 and what it always meant, pagani-vsync on top of it.
    Result r = boot(true, 120);
    assert(r.on && r.interval == 159999 && r.hz == 120 && r.properties == 1 && r.words[0] == (1ull << 32) / 120 && r.words[2] == r.words[0]);
    r = boot(true, 120, true, 90);
    assert(r.on && r.interval == 213333 && r.hz == 90 && r.words[0] == (1ull << 32) / 90);
    // The period: the tick at 8299 us, the display still reports 120, whatever pagani-vsync would have said.
    for (bool rate : {false, true}) {
        r = boot(true, 8299, rate, 90);
        assert(r.on && r.interval == 159340 && r.hz == 120 && r.properties == 1 && r.words[0] == (1ull << 32) / 120 && r.words[2] == r.words[0]);
    }
    // A 60 Hz panel's period reports 60.
    r = boot(true, 16600);
    assert(r.on && r.interval == 318720 && r.hz == 60 && r.words[0] == (1ull << 32) / 60);
    // A TE lock outranks it: the measured period.
    r = boot(true, 8299, false, 0, true);
    assert(r.on && r.interval == 159341 && r.hz == 120);
    puts("PASS: sheng-vsync at its four places");
}
'''
with tempfile.TemporaryDirectory(prefix='vsync-boot-arg-') as temporary:
    work = Path(temporary)
    (work / 'wiring.cpp').write_text(program.replace('@PARTS@', ''.join(parts)))
    subprocess.run([*shlex.split(os.environ.get('CXX', 'xcrun clang++' if sys.platform == 'darwin' else 'c++')), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-private-field', '-fsanitize=address,undefined', '-I', str(folder),
                    str(work / 'wiring.cpp'), '-o', str(work / 'wiring')], check=True)
    subprocess.run([str(work / 'wiring')], check=True)
