// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/display-timing.cpp drivers/NativeMobileDisplay/NativeDisplayTiming.h
// Host test of NativeDisplayTiming.h (ARCHITECTURE 5.3.3 D0, M12; M9 early retirement): the TE lock from dense samples, the locked tick
// replayed over a simulated panel (true TE times, read-pointer frame counter, tick delays, a period error that forces relocks, the TE
// stopping), the refresh class and interval words, the software cadence's timestamps (PERF-4), late-frame detection, and the end of an
// offered or copied frame (early retirement) driven through a model of the display's swap queue.
#include "../NativeDisplayTiming.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>

static const uint64_t kHz = 19200000;   // SM8750 timebase (mach ticks per second)

// A panel whose TE n comes at start + n * periodNum / periodDen ticks, read through INTF_TEAR_INT_COUNT_VAL: [31:16] frames, [15:0] the
// read pointer's line count, loaded with `init` at each TE and counting `vtotal` lines per frame (lines = false: a count that never moves).
struct Panel {
    uint64_t start, periodNum, periodDen;
    bool stopped = false, lines = true;
    uint64_t stopAt = 0;
    uint32_t init = 2640, vtotal = 2652;
    uint64_t te(uint64_t n) const {return start + n * periodNum / periodDen;}
    uint64_t frames(uint64_t t) const {     // TEs at or before t
        if (stopped && t > stopAt) t = stopAt;
        if (t < start) return 0;
        uint64_t n = (t - start) * periodDen / periodNum;
        while (te(n + 1) <= t) ++n;
        while (n && te(n) > t) --n;
        return n + 1;
    }
    uint32_t read(uint64_t t) const {
        const uint64_t n = frames(t);
        uint32_t line = 0x123;
        if (lines && n) line = uint32_t(init + (t - te(n - 1)) * vtotal * periodDen / periodNum) & 0xffff;
        return uint32_t((n & 0xffff) << 16) | line;
    }
    uint64_t latest(uint64_t t) const {uint64_t n = frames(t); return n ? te(n - 1) : 0;}
};

static uint64_t rng = 88172645463325252ull;
static uint64_t next(uint64_t bound) {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return bound ? rng % bound : 0;}

// 100 ms of samples every 50 us (plus up to 5 us of loop overhead), from `t`.
static NativeTeLock lockOn(const Panel &panel, uint64_t *t, bool *ok) {
    NativeTeSampler sampler;
    const uint64_t until = *t + kHz / 10;
    for (; *t < until; *t += 960 + next(96)) sampler.sample(*t, panel.read(*t));
    NativeTeLock lock;
    *ok = sampler.lock(&lock, kHz);
    return lock;
}

struct ReplayResult {unsigned ticks, relocks, worstUs, duplicates; bool lost; uint64_t period;};
// Ticks as the display runs them: wake `lead` after the predicted TE plus a delay (mostly microseconds, sometimes a busy gate of a few
// ms, rarely three TEs late); every reported stamp is compared with the true TE the tick follows.
static ReplayResult replay(const Panel &panel, NativeTeLock lock, uint64_t t, unsigned ticks) {
    ReplayResult r = {};
    uint64_t index = 0, wake = lock.next(t, &index) + 960, lastStamp = 0;
    for (unsigned i = 0; i < ticks; ++i) {
        const uint64_t roll = next(1000);
        uint64_t clock = wake + (roll < 950 ? next(400) : roll < 998 ? next(60000) : 3 * 160000 + next(1000));
        bool relocked = false;
        uint64_t reportedAt = 0;
        const uint64_t stamp = native_te_cadence(&lock, &index, [&] {return clock;}, [&] {return panel.read(clock);},
            [&](unsigned us, uint64_t *time, uint16_t *count) {
                const uint64_t until = clock + us * (kHz / 1000000);
                uint64_t previous = clock;
                const uint16_t frame = uint16_t(panel.read(clock) >> 16);
                while (clock < until) {
                    clock += 192;   // 10 us
                    const uint16_t now = uint16_t(panel.read(clock) >> 16);
                    if (now != frame) {*time = previous + (clock - previous) / 2; *count = now; return true;}
                    previous = clock;
                }
                return false;
            }, 5000, 960, &wake, &reportedAt, &relocked);
        if (!lock.valid) {r.lost = true; return r;}
        r.relocks += relocked;
        const uint64_t truth = panel.latest(reportedAt);
        const uint64_t error = stamp > truth ? stamp - truth : truth - stamp;
        const unsigned us = unsigned(error * 1000000 / kHz);
        if (us > r.worstUs) r.worstUs = us;
        if (stamp <= lastStamp) ++r.duplicates;
        lastStamp = stamp;
        assert(stamp <= reportedAt && wake > reportedAt);
        ++r.ticks;
    }
    r.period = lock.period();
    return r;
}

int main() {
    unsigned checks = 0;
    {   // Lock at 120 Hz: period within a tick, refresh class 120, interval words.
        Panel panel = {1000000, kHz * 100, 12004};       // 120.04 Hz
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok && lock.valid);
        const double period = double(panel.periodNum) / panel.periodDen;
        // Edges are known to half a sample (about 25 us) over about 12 periods: the period to within 0.1%.
        printf("  lock: period %llu ticks (true %.1f), %llu mHz\n", (unsigned long long)lock.period(), period,
               (unsigned long long)lock.millihertz(kHz));
        assert(lock.period() + 160 >= period && lock.period() <= period + 160);
        assert(native_refresh_class(lock.millihertz(kHz)) == 120);
        assert(lock.millihertz(kHz) > 119900 && lock.millihertz(kHz) < 120200);
        // Line dating: about 2652 lines per frame, the count's TE value close to 2640.
        assert(lock.lineQ16 && lock.period() * 65536 / lock.lineQ16 > 2600 && lock.period() * 65536 / lock.lineQ16 < 2700);
        assert(lock.lineInit + 8 >= 2640 && lock.lineInit <= 2640 + 8);
        uint64_t dated = 0;
        assert(lock.dated(lock.anchor + 100000, panel.read(lock.anchor + 100000), &dated));
        const uint64_t lastTe = panel.latest(lock.anchor + 100000);
        assert((dated > lastTe ? dated - lastTe : lastTe - dated) < 300);
        // The anchor is an edge within the sampling interval.
        const uint64_t truth = panel.latest(lock.anchor + 600);
        assert((lock.anchor > truth ? lock.anchor - truth : truth - lock.anchor) <= 600);
        uint64_t index = 0;
        const uint64_t n = lock.next(lock.anchor, &index);
        assert(index == 1 && n > lock.anchor && lock.at(-1) < lock.anchor && lock.at(0) == lock.anchor);
        checks += 3;
    }
    {   // A 60 Hz panel locks at class 60; 90 and odd rates; the interval words are 2^32/Hz.
        Panel panel = {0, kHz * 100, 5998};
        uint64_t t = 5000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok && native_refresh_class(lock.millihertz(kHz)) == 60);
        assert(native_refresh_class(89800) == 90 && native_refresh_class(119500) == 120 && native_refresh_class(75000) == 75);
        assert(native_refresh_class(10000) == 0 && native_refresh_class(0) == 0 && native_refresh_class(300000) == 0);
        assert(native_refresh_interval(120) == 0x2222222 && native_refresh_interval(60) == 0x4444444 && !native_refresh_interval(0));
        checks += 2;
    }
    {   // No TE (autorefresh off, panel asleep): no lock; a counter that never moves gives no edge.
        Panel panel = {0, kHz * 100, 12000};
        panel.stopped = true; panel.stopAt = 1;
        uint64_t t = 100000; bool ok = true;
        lockOn(panel, &t, &ok);
        assert(!ok);
        NativeTeSampler two;   // two edges are not enough by default
        two.sample(0, 0); two.sample(10, 1u << 16); two.sample(160010, 2u << 16);
        NativeTeLock lock;
        assert(!two.lock(&lock, kHz));
        checks++;
    }
    {   // Replay: 20000 ticks (about 167 s) of a 120.04 Hz panel; stamps stay within 60 us of the true TE, never repeat, lock holds.
        Panel panel = {1000000, kHz * 100, 12004};
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok);
        ReplayResult r = replay(panel, lock, t, 20000);
        printf("  replay 120.04 Hz: %u ticks, %u relocks, worst %u us, %u duplicates\n", r.ticks, r.relocks, r.worstUs, r.duplicates);
        assert(!r.lost && r.ticks == 20000 && r.worstUs <= 60 && !r.duplicates);
        checks++;
    }
    {   // Without a usable line count (it never moves) the lock still holds on the frame count alone; nothing is dated.
        Panel panel = {1000000, kHz * 100, 12004};
        panel.lines = false;
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        uint64_t dated = 0;
        assert(ok && !lock.lineQ16 && !lock.dated(t, panel.read(t), &dated));
        checks++;
    }
    {   // A lock taken with a period 0.3% off drifts by 25 us a frame: line dating moves the anchor at every tick that is off by more than
        // period/400 and refines the period over the baseline, so the stamps stay right without dense relocks.
        Panel panel = {1000000, kHz * 100, 12004};
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok);
        lock.periodQ16 = lock.periodQ16 * 1003 / 1000;
        ReplayResult r = replay(panel, lock, t, 5000);
        printf("  replay with a 0.3%% period error: %u relocks, worst %u us, period now %llu\n", r.relocks, r.worstUs,
               (unsigned long long)r.period);
        assert(!r.lost && r.worstUs <= 60 && !r.duplicates && r.period + 40 >= 159946 && r.period <= 159946 + 40);
        checks++;
    }
    {   // Without line dating, a prediction that runs early (the TE comes later than predicted) is caught by the frame count and
        // relocked densely; the stamps stay within the relock's precision.
        Panel panel = {1000000, kHz * 100, 12004};
        panel.lines = false;
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok && !lock.lineQ16);
        lock.periodQ16 = lock.periodQ16 * 999 / 1000;      // predictions early by 8 us a frame
        ReplayResult r = replay(panel, lock, t, 3000);
        printf("  replay without line dating, prediction early: %u relocks, worst %u us\n", r.relocks, r.worstUs);
        assert(!r.lost && r.relocks >= 1 && !r.duplicates);
        checks++;
    }
    {   // The TE stops (panel asleep) while locked: the next early tick finds no edge within 5 ms and gives the lock up.
        Panel panel = {1000000, kHz * 100, 12004};
        uint64_t t = 2000000; bool ok = false;
        NativeTeLock lock = lockOn(panel, &t, &ok);
        assert(ok);
        panel.stopped = true; panel.stopAt = t + 500000;
        ReplayResult r = replay(panel, lock, t, 1000);
        assert(r.lost);
        checks++;
    }
    {   // native_te_check and native_te_relock directly.
        assert(native_te_check(5, 5u << 16) == 0 && native_te_check(5, 8u << 16) == 3 && native_te_check(5, 4u << 16) == -1);
        assert(native_te_check(0xffff, 2u << 16) == 3);   // wraps
        NativeTeLock lock; lock.valid = true; lock.anchor = 1000; lock.periodQ16 = 160000ull << 16; lock.count = 7;
        native_te_relock(&lock, 1000 + 100 * 160010, 107);   // 100 frames later, 10 ticks per frame longer: refined
        assert(lock.anchor == 1000 + 100 * 160010 && lock.count == 107 && (lock.periodQ16 >> 16) == 160010);
        const uint64_t q = lock.periodQ16;
        native_te_relock(&lock, lock.anchor + 10 * 170000, 117);   // 6% off: the period is kept, the anchor moves
        assert(lock.periodQ16 == q && lock.count == 117);
        checks++;
    }
    {   // Software cadence (PERF-4): the scheduled deadline, unless a whole interval late.
        assert(native_vsync_stamp(1000, 1300, 1000) == 1000 && native_vsync_stamp(1000, 2000, 1000) == 2000 &&
               native_vsync_stamp(1000, 900, 1000) == 900);
        checks++;
    }
    {   // sheng-vsync as a rate is what it always was; as a period, that period to the microsecond; anything else no tick.
        for (unsigned hz : {24u, 60u, 120u, 240u}) {
            assert(native_vsync_on(hz) && !native_vsync_period_us(hz) && !native_vsync_class(hz));
            assert(native_vsync_ns(hz, hz) == 1000000000ull / hz);
        }
        assert(native_vsync_ns(120, 120) == 8333333 && native_vsync_ns(120, 90) == 11111111);       // pagani-vsync says how fast
        for (unsigned off : {0u, 1u, 23u, 241u, 999u, 4166u, 41668u, 100000u}) assert(!native_vsync_on(off) && !native_vsync_period_us(off) && !native_vsync_class(off));
        for (unsigned edge : {4167u, 41667u}) assert(native_vsync_on(edge) && native_vsync_period_us(edge) == edge);
        assert(native_vsync_ns(8299, 120) == 8299000 && native_vsync_ns(8299, 60) == 8299000);      // pagani-vsync has no say in that one
        assert(native_vsync_class(8299) == 120 && native_vsync_class(8333) == 120 && native_vsync_class(11111) == 90 && native_vsync_class(16600) == 60);
        assert(native_vsync_class(4167) == 240 && native_vsync_class(41667) == 24);
        // What it is for: TEs of the first measured period, with a tick every 8333 us and every 8299. A TE with no tick since the one before repeats a frame.
        const auto repeats = [](uint64_t ns, unsigned seconds) {
            const uint64_t te = 159341, interval = ns * 96 / 5000;                                  // ticks of 19.2 MHz: 8299 us, and the software tick
            unsigned none = 0;
            uint64_t tick = 40000;
            for (uint64_t at = te; at < seconds * kHz; at += te) {
                if (tick >= at) ++none;
                while (tick < at) tick += interval;
            }
            return none;
        };
        const unsigned before = repeats(native_vsync_ns(120, 120), 60), after = repeats(native_vsync_ns(8299, 120), 60);
        printf("  cadence: over 60 s, %u repeated frames at 120 Hz, %u at 8299 us\n", before, after);
        assert(before >= 26 && before <= 32 && after <= 1);                                         // about one every 2 s, then none
        checks += 2;
    }
    {   // Late frames: aimed at the first TE after arrival.
        const uint64_t te = 1000000, period = 160000;
        assert(native_frame_late(te, period, te + 10, te + 150000) == 0);            // made it
        assert(native_frame_late(te, period, te + 10, te + 170000) == 10000);        // missed by 10000 ticks
        assert(native_frame_late(te, period, te - 1000, te + 5) == 5);               // arrived just before a TE and missed it
        assert(native_frame_late(te, period, te - 300000, te - 200000) == 0);        // before the reference TE, on time
        assert(native_frame_late(te, 0, 1, 2) == 0);
        checks++;
    }
    {   // Ending a frame: early retirement only releases the next frame; a failed GPU copy makes the next one take the CPU path.
        NativeSwapEnd e = native_gpu_swap_end(true, 0);
        assert(e.action == kSwapReleaseNext && e.reportSuccess && !e.earlyFailure && !e.bypassGpu);
        e = native_gpu_swap_end(true, -5);
        assert(e.action == kSwapReleaseNext && !e.reportSuccess && e.earlyFailure && e.bypassGpu);
        e = native_gpu_swap_end(false, 0);
        assert(e.action == kSwapRetire && e.reportSuccess);
        e = native_gpu_swap_end(false, -5);
        assert(e.action == kSwapFallback && e.bypassGpu && !e.reportSuccess);
        assert(native_copy_end(true, true, false).action == kSwapReleaseNext && native_copy_end(false, true, false).action == kSwapRetire);
        assert(native_copy_end(false, false, false).action == kSwapRetry && native_copy_end(true, true, true).action == kSwapStopped);
        // Model of the display's queue (kickOnce/finishGpuSwap): early retirement lets the compositor render frame n+1 while the GPU copies
        // frame n, but frame n+1 is offered only after frame n's copy ended (transferred), and a failed copy sends n+1 through the CPU.
        struct Queue {
            unsigned queued = 0, retired = 0, offered = 0, cpu = 0, inFlight = 0;
            bool transferred = false, bypass = false, retiredEarly = false;
            void kick(bool accept) {
                if (transferred || queued == retired) return;
                if (!bypass && accept) {++offered; ++inFlight; transferred = true; retiredEarly = true; ++retired; return;}
                ++cpu; ++retired;                       // CPU copy and retirement (synchronous here)
            }
            void complete(int status) {
                --inFlight;
                const NativeSwapEnd end = native_gpu_swap_end(retiredEarly, status);
                retiredEarly = false;
                if (end.bypassGpu) bypass = true;
                transferred = false;
                kick(true);
                bypass = false;
            }
        } q;
        q.queued = 1; q.kick(true);
        assert(q.offered == 1 && q.retired == 1 && q.transferred);      // retired at the offer
        q.queued = 2; q.kick(true);
        assert(q.offered == 1);                                         // n+1 waits for n's copy
        q.complete(0);
        assert(q.offered == 2 && q.retired == 2 && q.inFlight == 1);    // n+1 offered when n's copy ended
        q.queued = 3; q.complete(-5);
        assert(q.cpu == 1 && q.retired == 3 && q.offered == 2);         // failed copy: the next frame took the CPU path
        q.queued = 4; q.kick(true);
        assert(q.offered == 3);                                         // and the GPU path is back afterwards
        checks += 2;
    }
    printf("PASS: display timing, %u checks: TE lock, locked tick replay, refresh class, software cadence, late frames, swap ends\n", checks);
    return 0;
}
