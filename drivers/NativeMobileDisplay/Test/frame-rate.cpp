// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/frame-rate.cpp drivers/BootProgress/FrameRate.h
// Host test of the frame-rate ring behind the FPS row and the ramoops F/J records (BootProgress FrameRate.h, pagani-fps), which the
// display path feeds with every frame that reached the panel (NativeLegacyDisplay frameDone and the blank fill, through Stage7Probe's
// native_legacy_frame_presented): the counters and slot generations (also with four writers at once), the 99th percentile from the
// largest values against a full sort, the one-second statistics, the ten windows and what the ring still covers, jitter and idle gaps,
// a frame still being stored, lost frames, the row (exact text, at most 65 characters) and the jitter records' rate limit.
#include "../../BootProgress/FrameRate.h"
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>

static const uint64_t kHz = 19200000;   // SM8750 timebase (mach ticks per second)
static const uint64_t kMs = kHz / 1000;

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(uint64_t bound) {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return bound ? rng % bound : 0;}

static frame_ring *fresh() {
    static frame_ring ring;
    memset(&ring, 0, sizeof ring);
    return &ring;
}

static frame_scan scan(const frame_ring *ring, uint64_t now, uint64_t from) {
    frame_scan s;
    memset(&s, 0xa5, sizeof s);     // every field is the pass's own
    frame_ring_scan(ring, __atomic_load_n(&ring->count, __ATOMIC_ACQUIRE), now, kHz, from, &s);
    return s;
}

static std::string row(const frame_scan &s) {
    char text[80];
    const unsigned n = frame_row(text, sizeof text, &s);
    assert(n == strlen(text) && n <= 65);
    return text;
}

// numpy's default percentile (linear between closest ranks) in the same integer arithmetic, from a full sort.
static uint64_t p99Sorted(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    const uint64_t n = v.size(), h = (n - 1) * 99, low = h / 100, fraction = h % 100;
    const uint64_t a = v[low], b = low + 1 < n ? v[low + 1] : a;
    return a + (b - a) * fraction / 100;
}

int main() {
    unsigned checks = 0;
    {   // Counters and generations: a slot passes only for the frame number that wrote it.
        frame_ring *r = fresh();
        uint64_t stamp = 0; unsigned path = 9;
        assert(!frame_ring_get(r, 0, &stamp, &path));                     // zero is no frame 0
        frame_ring_record(r, FRAME_GPU, 100);
        frame_ring_record(r, FRAME_CPU, 200);
        frame_ring_record(r, 7, (1ull << 60) | 300);                       // path bit 0 only; stamp 53 bits
        assert(r->count == 3 && r->paths[FRAME_CPU] == 1 && r->paths[FRAME_GPU] == 2);
        assert(frame_ring_get(r, 0, &stamp, &path) && stamp == 100 && path == FRAME_GPU);
        assert(frame_ring_get(r, 1, &stamp, &path) && stamp == 200 && path == FRAME_CPU);
        assert(frame_ring_get(r, 2, &stamp, &path) && stamp == 300 && path == FRAME_GPU);
        assert(!frame_ring_get(r, 3, &stamp, &path));
        for (uint64_t n = 3; n < FRAME_RING + 5; ++n) frame_ring_record(r, FRAME_CPU, 1000 + n);
        assert(!frame_ring_get(r, 4, &stamp, &path));                     // reused by frame FRAME_RING + 4
        assert(frame_ring_get(r, FRAME_RING + 4, &stamp, &path) && stamp == 1000 + FRAME_RING + 4);
        // A writer that took a number but has not stored yet: its slot still holds the frame FRAME_RING before.
        r->count++;
        assert(!frame_ring_get(r, FRAME_RING + 5, &stamp, &path));
        // Generations wrap after 1024 * FRAME_RING frames: still distinct from the neighbours'.
        assert(frame_generation(0) == 1 && frame_generation(FRAME_RING) == 2 && frame_generation(1023ull * FRAME_RING) == 0);
        checks++;
    }
    {   // Four writers at once (two per path): every frame counted once, every slot of the last FRAME_RING frames valid.
        frame_ring *r = fresh();
        const unsigned per = 50000;
        std::vector<std::thread> writers;
        for (unsigned w = 0; w < 4; ++w)
            writers.emplace_back([r, w] {for (unsigned i = 0; i < per; ++i) frame_ring_record(r, w & 1, 1 + w * per + i);});
        for (auto &t: writers) t.join();
        assert(r->count == 4 * per && r->paths[0] == 2 * per && r->paths[1] == 2 * per);
        unsigned valid = 0;
        for (uint64_t n = r->count - FRAME_RING; n < r->count; ++n) {uint64_t s; unsigned p; valid += frame_ring_get(r, n, &s, &p);}
        assert(valid == FRAME_RING);
        checks++;
    }
    {   // The 99th percentile from the largest FRAME_TOP values equals the full sort's for every count up to FRAME_RING.
        for (unsigned round = 0; round < 3000; ++round) {
            const unsigned n = round < 2048 ? round + 1 : 1 + unsigned(next(FRAME_RING));
            std::vector<uint64_t> values(n);
            uint64_t top[FRAME_TOP]; unsigned tops = 0;
            for (auto &v: values) {
                const uint64_t roll = next(100);
                v = roll < 60 ? 160000 + next(4) : roll < 95 ? next(20 * kMs) : next(kHz);     // plateaus, noise, spikes
                frame_top_add(top, &tops, v);
            }
            assert(tops == std::min<unsigned>(n, FRAME_TOP));
            for (unsigned i = 1; i < tops; ++i) assert(top[i - 1] >= top[i]);
            assert(top[0] == *std::max_element(values.begin(), values.end()));
            assert(frame_p99(top, tops, n) == p99Sorted(values));
        }
        assert(frame_p99(nullptr, 0, 0) == 0);
        checks++;
    }
    {   // Steady 120 Hz for 12 s, all GPU: 120 a second over ten complete windows, 8.3 ms frame times, no jitter.
        frame_ring *r = fresh();
        const uint64_t t0 = 50 * kHz, step = kHz / 120;
        for (unsigned k = 0; k < 12 * 120; ++k) frame_ring_record(r, FRAME_GPU, t0 + k * step);
        const uint64_t now = t0 + 12 * kHz - step / 2;
        frame_scan s = scan(r, now, 0);
        assert(s.fps == 120 && s.fps_low == 120 && s.fps_high == 120 && s.windows == 10);
        assert(s.paths[FRAME_GPU] == 120 && !s.paths[FRAME_CPU] && s.intervals == 120);
        assert(s.avg_us == 8333 && s.p99_us == 8333 && s.max_us == 8333 && !s.jitters && !s.kept && !s.lost);
        assert(s.next == r->count);
        assert(row(s) == "FPS 120 (120-120) | ft 8.3 p99 8.3 max 8.3ms | gpu 120 cpu 0");
        // The next pass starts where this one ended: nothing new, nothing lost.
        frame_scan again = scan(r, now + kHz / 10, s.next);
        assert(!again.jitters && !again.lost && again.next == s.next);
        checks++;
    }
    {   // 60 Hz with one 88 ms stall on the CPU path in the last second: one jitter (path, interval, age), the spike in p99 and max.
        frame_ring *r = fresh();
        const uint64_t t0 = 10 * kHz, step = kHz / 60;
        uint64_t t = t0;
        for (unsigned k = 0; k < 120; ++k) {frame_ring_record(r, FRAME_GPU, t); t += step;}
        t += 88 * kMs - step;
        frame_ring_record(r, FRAME_CPU, t);
        const uint64_t stall = t;
        for (unsigned k = 0; k < 20; ++k) {t += step; frame_ring_record(r, FRAME_GPU, t);}
        const uint64_t now = t + kMs;
        frame_scan s = scan(r, now, 100);
        assert(s.jitters == 1 && s.kept == 1 && s.events[0].path == FRAME_CPU && s.events[0].interval_us == 88000 &&
               s.events[0].age == now - stall);
        assert(s.paths[FRAME_CPU] == 1 && s.paths[FRAME_CPU] + s.paths[FRAME_GPU] == s.fps && s.max_us == 88000);
        assert(s.p99_us > 16666 && s.p99_us < 88000);
        std::vector<uint64_t> times(s.intervals, step);
        times[0] = 88 * kMs;
        assert(s.p99_us == p99Sorted(times) * 1000000 / kHz);
        // The same stall seen from a later pass that starts after it: not reported again.
        assert(!scan(r, now, s.next).jitters);
        // A pass that starts at the stalled frame itself sees it; one that starts right after it does not.
        assert(scan(r, now, 120).jitters == 1 && scan(r, now, 121).jitters == 0);
        checks++;
    }
    {   // Idle gaps: 2 s without frames is neither a jitter nor a frame time; the empty seconds pull the range down to 0. Intervals
        // just above FRAME_JITTER_MS and just below FRAME_IDLE_MS are jitters; exactly FRAME_JITTER_MS is not.
        frame_ring *r = fresh();
        uint64_t t = 20 * kHz;
        frame_ring_record(r, FRAME_GPU, t);
        t += 2 * kHz; frame_ring_record(r, FRAME_GPU, t);                 // idle
        t += 50 * kMs; frame_ring_record(r, FRAME_GPU, t);                // exactly 50 ms
        t += 50 * kMs + 1; frame_ring_record(r, FRAME_GPU, t);            // just above
        t += kHz - 1; frame_ring_record(r, FRAME_CPU, t);                 // just below 1 s
        frame_scan s = scan(r, t + 2, 0);                                   // the frame before it is just outside the last second
        assert(s.jitters == 2 && s.events[0].path == FRAME_CPU && s.events[1].interval_us == 50000);
        assert(s.fps == 1 && s.intervals == 1 && s.max_us == 999999 && s.fps_low == 0 && s.fps_high == 3 && s.windows == 10);
        assert(row(s) == "FPS 1 (0-3) | ft 1000 p99 1000 max 1000ms | gpu 0 cpu 1");
        checks++;
    }
    {   // Writers on two CPUs can store out of order: such an interval counts as 0. A newest frame not stored yet is left to the next
        // pass (next stops before it), which then sees it.
        frame_ring *r = fresh();
        const uint64_t t0 = 30 * kHz;
        frame_ring_record(r, FRAME_GPU, t0);
        frame_ring_record(r, FRAME_CPU, t0 + 10 * kMs);
        frame_ring_record(r, FRAME_GPU, t0 + 5 * kMs);                    // stamped before the one stored ahead of it
        frame_scan s = scan(r, t0 + 20 * kMs, 0);
        assert(s.fps == 3 && s.intervals == 2 && s.max_us == 10000 && s.avg_us == 5000 && !s.jitters);
        r->count++;                                                         // frame 3 taken, not stored
        s = scan(r, t0 + 200 * kMs, s.next);
        assert(s.next == 3 && s.fps == 3 && !s.jitters);
        __atomic_store_n(&r->slot[3], (t0 + 180 * kMs) | frame_generation(3) << 54, __ATOMIC_RELEASE);   // stored now (CPU path)
        s = scan(r, t0 + 200 * kMs, s.next);
        assert(s.next == 4 && s.fps == 4 && s.jitters == 1 && s.events[0].interval_us == 175000 && s.events[0].path == FRAME_CPU);
        checks++;
    }
    {   // More than FRAME_RING frames in ten seconds (250 a second): only the windows the ring still covers count, and a pass that
        // last ran 12 s ago reports the frames it could not see.
        frame_ring *r = fresh();
        const uint64_t t0 = 40 * kHz, step = kHz / 250;
        for (unsigned k = 0; k < 12 * 250; ++k) frame_ring_record(r, k & 1, t0 + k * step);
        const uint64_t now = t0 + 12 * kHz - 1;
        frame_scan s = scan(r, now, 0);
        assert(s.fps == 250 && s.windows == 8 && s.fps_low == 250 && s.fps_high == 250);     // 2048 frames: 8.19 s
        assert(s.paths[0] == 125 && s.paths[1] == 125 && s.avg_us == 4000);
        assert(s.lost == r->count - FRAME_RING);                // frames 1 .. count - FRAME_RING: their intervals are gone
        assert(scan(r, now, r->count - 100).lost == 0);
        checks++;
    }
    {   // More jitters than one pass keeps: the newest FRAME_JITTER_EVENTS, newest first; all of them counted.
        frame_ring *r = fresh();
        uint64_t t = 60 * kHz;
        for (unsigned k = 0; k < 30; ++k) {frame_ring_record(r, FRAME_GPU, t); t += (60 + k) * kMs;}
        frame_scan s = scan(r, t, 0);
        assert(s.jitters == 29 && s.kept == FRAME_JITTER_EVENTS);
        for (unsigned i = 0; i < s.kept; ++i) assert(s.events[i].interval_us == (60 + 28 - i) * 1000);
        for (unsigned i = 1; i < s.kept; ++i) assert(s.events[i].age > s.events[i - 1].age);
        checks++;
    }
    {   // Nothing at all, and a ring without a clock.
        frame_ring *r = fresh();
        frame_scan s = scan(r, kHz, 0);
        assert(!s.fps && s.windows == 10 && !s.intervals && !s.lost && s.next == 0);
        assert(row(s) == "FPS 0 (0-0) | ft - | gpu 0 cpu 0");
        frame_ring_record(r, FRAME_GPU, kHz / 2);
        frame_ring_scan(r, 1, kHz, 0, 0, &s);
        assert(!s.fps && s.next == 1);
        checks++;
    }
    {   // The row: exact forms, milliseconds with one decimal below 100 ms, counts above 999 as 999, never above 65 characters,
        // and cut cleanly in a short buffer.
        frame_scan s = {};
        s.fps = 58; s.fps_low = 31; s.fps_high = 60; s.paths[FRAME_GPU] = 45; s.paths[FRAME_CPU] = 13;
        s.intervals = 58; s.avg_us = 17240; s.p99_us = 41460; s.max_us = 88049;
        assert(row(s) == "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13");
        s.avg_us = 99949; s.p99_us = 99950; s.max_us = 999500;
        assert(row(s) == "FPS 58 (31-60) | ft 99.9 p99 100 max 1000ms | gpu 45 cpu 13");
        s.fps = s.fps_low = s.fps_high = 5000; s.paths[0] = s.paths[1] = 4000; s.avg_us = s.p99_us = s.max_us = 999999;
        assert(row(s) == "FPS 999 (999-999) | ft 1000 p99 1000 max 1000ms | gpu 999 cpu 999" && row(s).size() == 65);
        for (unsigned round = 0; round < 100000; ++round) {
            s.fps = uint32_t(next(3000)); s.fps_low = uint32_t(next(3000)); s.fps_high = uint32_t(next(3000));
            s.paths[0] = uint32_t(next(3000)); s.paths[1] = uint32_t(next(3000)); s.intervals = uint32_t(next(3));
            s.avg_us = next(1000000); s.p99_us = next(1000000); s.max_us = next(1000000);
            (void)row(s);
        }
        char small[10];
        assert(frame_row(small, sizeof small, &s) == 9 && !strncmp(small, "FPS ", 4) && strlen(small) == 9);
        assert(frame_row(small, 0, &s) == 0);
        checks++;
    }
    {   // The jitter records' limit: four at once, then one per 5 s; the credit never exceeds four; refusals are counted.
        frame_limit l = {};
        const uint64_t t0 = 7 * kHz;
        for (unsigned i = 0; i < 4; ++i) assert(frame_limit_take(&l, t0, kHz));
        assert(!frame_limit_take(&l, t0, kHz) && !frame_limit_take(&l, t0 + 4 * kHz, kHz) && l.skipped == 2);
        assert(frame_limit_take(&l, t0 + 5 * kHz, kHz) && !frame_limit_take(&l, t0 + 5 * kHz, kHz));
        assert(frame_limit_take(&l, t0 + 10 * kHz, kHz));
        unsigned taken = 0;
        for (unsigned i = 0; i < 10; ++i) taken += frame_limit_take(&l, t0 + 1000 * kHz, kHz);
        assert(taken == 4 && l.skipped == 2 + 1 + 6);
        // Sustained: over 60 s of one jitter every 100 ms, the burst plus one per 5 s.
        frame_limit m = {};
        taken = 0;
        for (unsigned i = 0; i <= 600; ++i) taken += frame_limit_take(&m, t0 + i * (kHz / 10), kHz);
        assert(taken == 4 + 12 && m.skipped == 601 - taken);
        checks++;
    }
    printf("PASS: frame-rate ring (%u groups): counters and generations with four writers, p99 from the largest values equal to a full "
           "sort, one-second statistics and ten windows, jitter and idle gaps, unstored and out-of-order frames, ring overflow and "
           "lost frames, the row within 65 characters, the jitter rate limit\n", checks);
    return 0;
}
