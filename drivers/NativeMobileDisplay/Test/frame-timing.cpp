// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/frame-timing.cpp drivers/BootProgress/FrameRate.h drivers/BootProgress/PeripheralStatus.h
// Host test of where a frame's time went (BootProgress FrameRate.h, pagani-fps): the display path reports each frame with its stages
// (PeripheralStatus.h pagani_frame_timing: arrival and the gap since the previous arrival, offer, taken, copied; NativeLegacyDisplay
// through Stage7Probe's native_legacy_frame_timed) and the 1 Hz observer prints the slowest frame interval of the last second with that
// frame's breakdown on text row 8 (the SLOW row) and in the ramoops J records. Checked here: microseconds from ticks (exact, saturated,
// missing stages, stamps of two CPUs), the breakdown words beside the ring's slots (packing, flags, generations, reuse, four writers
// against a reader), the frame the scan names for the largest interval and for every jitter, the stage word, the duration texts, the
// SLOW row (exact texts, " done" only while the row fits, never more than 65 characters on the screen) and the J record's breakdown.
#include "../../BootProgress/FrameRate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

static const uint64_t kHz = 19200000;   // SM8750 timebase (mach ticks per second; 19.2 a microsecond)
static const uint64_t kMs = kHz / 1000;

static frame_ring *fresh() {
    static frame_ring ring;
    memset(&ring, 0, sizeof ring);
    return &ring;
}

// Ticks of `us` microseconds at 19.2 MHz (whole ticks: multiples of 5 us are exact).
static uint64_t ticksUs(uint64_t us) {return us * kHz / 1000000;}

static std::string slowRow(const frame_slow &s, unsigned width = 65) {
    char text[128];
    const unsigned n = frame_slow_row(text, sizeof text, width, &s);
    assert(n == strlen(text));
    return text;
}

static std::string detail(const frame_breakdown *b, uint32_t fallbacks, uint32_t timeouts) {
    char text[160];
    const unsigned n = frame_jitter_detail(text, sizeof text, b, fallbacks, timeouts);
    assert(n == strlen(text));
    return text;
}

static std::string dur(uint32_t us) {
    char text[16];
    frame_text t = {text, 0, sizeof text};
    text[0] = 0;
    frame_putdur(&t, us);
    assert(t.used <= 4);
    return text;
}

static std::string age(uint64_t us) {
    char text[16];
    frame_text t = {text, 0, sizeof text};
    text[0] = 0;
    frame_putage(&t, us);
    assert(t.used <= 6);
    return text;
}

static frame_breakdown breakdown(uint32_t gap, uint32_t off, uint32_t take, uint32_t copy, uint32_t done, uint32_t flags) {
    frame_breakdown b;
    b.gap_us = gap; b.off_us = off; b.take_us = take; b.copy_us = copy; b.done_us = done; b.flags = flags;
    return b;
}

static frame_slow slow(bool have, uint64_t interval, const frame_breakdown *b, uint32_t fallbacks, uint32_t timeouts,
                       unsigned stage = PAGANI_STAGE_NONE, uint64_t stageUs = 0) {
    frame_slow s;
    memset(&s, 0, sizeof s);
    s.have = have; s.interval_us = interval; s.timed = b != nullptr;
    if (b) s.b = *b;
    s.fallbacks = fallbacks; s.timeouts = timeouts; s.stage = stage; s.stage_us = stageUs;
    return s;
}

int main() {
    unsigned checks = 0;
    {   // Microseconds from ticks: exact, rounded down, saturated at FRAME_US_MAX (67 s), FRAME_US_NONE without a clock or a stage.
        assert(frame_us(0, kHz) == 0 && frame_us(1, kHz) == 0 && frame_us(19, kHz) == 0 && frame_us(20, kHz) == 1);
        assert(frame_us(kMs, kHz) == 1000 && frame_us(kMs * 88 + kMs / 10 * 4, kHz) == 88400);
        assert(frame_us(kHz * 67, kHz) == 67000000 && frame_us(kHz * 67 + kHz / 10, kHz) == 67100000);
        assert(frame_us(kHz * 67 + kHz / 2, kHz) == FRAME_US_MAX && FRAME_US_MAX == 67108862);      // 67.5 s: above the 26 bits
        assert(frame_us(kHz * 68, kHz) == FRAME_US_MAX && frame_us(UINT64_MAX, kHz) == FRAME_US_MAX);
        assert(frame_us(kMs, 0) == FRAME_US_NONE && FRAME_US_MAX < FRAME_US_NONE && FRAME_US_NONE == (1u << 26) - 1);
        assert(frame_span_us(0, 5, kHz) == FRAME_US_NONE && frame_span_us(5, 0, kHz) == FRAME_US_NONE);
        assert(frame_span_us(1000, 1000 + kMs * 17, kHz) == 17000);
        assert(frame_span_us(1000 + kMs, 1000, kHz) == 0);      // stamps of two CPUs out of order: no time, never negative
        checks++;
    }
    {   // A frame's breakdown from its stages (the presented stamp is the frame's own): every span, missing ones, the flags.
        pagani_frame_timing t;
        memset(&t, 0, sizeof t);
        const uint64_t arrival = 1000 * kHz;
        t.arrival = arrival;
        t.gap = ticksUs(70100);
        t.offer = arrival + ticksUs(200);
        t.taken = t.offer + ticksUs(300);
        t.copied = t.taken + ticksUs(17000);
        t.flags = PAGANI_TIMING_REFUSED | PAGANI_TIMING_FLIP | 0x80000000u;    // bits outside PAGANI_TIMING_FLAGS are dropped
        frame_breakdown b;
        frame_breakdown_of(&t, t.copied + ticksUs(100), kHz, &b);
        assert(b.gap_us == 70100 && b.off_us == 200 && b.take_us == 300 && b.copy_us == 17000 && b.done_us == 100);
        assert(b.flags == (FRAME_TIMED | PAGANI_TIMING_REFUSED | PAGANI_TIMING_FLIP));
        // The CPU copy in the gate: offered and taken at its start, presented when it ended; no previous arrival.
        memset(&t, 0, sizeof t);
        t.arrival = arrival; t.offer = t.taken = arrival + ticksUs(50); t.copied = t.taken + ticksUs(143000);
        frame_breakdown_of(&t, t.copied, kHz, &b);
        assert(b.gap_us == FRAME_US_NONE && b.off_us == 50 && b.take_us == 0 && b.copy_us == 143000 && b.done_us == 0);
        assert(b.flags == FRAME_TIMED);
        // A frame no kick noted (no arrival) and a lost copy (no taken, no copied).
        memset(&t, 0, sizeof t);
        t.offer = arrival;
        frame_breakdown_of(&t, arrival + kMs, kHz, &b);
        assert(b.off_us == FRAME_US_NONE && b.take_us == FRAME_US_NONE && b.copy_us == FRAME_US_NONE && b.done_us == FRAME_US_NONE);
        checks++;
    }
    {   // The breakdown words beside the slots: two 26-bit values each, the generation like the slot's; a frame without one, reuse.
        frame_ring *r = fresh();
        frame_breakdown b = breakdown(70100, 200, 300, 17000, 100, FRAME_TIMED | PAGANI_TIMING_FAILED), got;
        frame_ring_record_breakdown(r, FRAME_GPU, 100, &b);
        frame_ring_record(r, FRAME_CPU, 200);                            // no breakdown: words of "none", flags 0
        b = breakdown(FRAME_US_NONE, FRAME_US_MAX, 0, FRAME_US_MAX, FRAME_US_NONE, FRAME_TIMED | PAGANI_TIMING_FLAGS);
        frame_ring_record_breakdown(r, 7, 300, &b);                       // path bit 0 only
        assert(r->count == 3 && r->paths[FRAME_GPU] == 2 && r->paths[FRAME_CPU] == 1);
        assert(frame_ring_breakdown(r, 0, &got) && got.gap_us == 70100 && got.off_us == 200 && got.take_us == 300 &&
               got.copy_us == 17000 && got.done_us == 100 && got.flags == (FRAME_TIMED | PAGANI_TIMING_FAILED));
        assert(!frame_ring_breakdown(r, 1, &got) && got.flags == 0 && got.gap_us == FRAME_US_NONE);   // recorded without one
        assert(frame_ring_breakdown(r, 2, &got) && got.gap_us == FRAME_US_NONE && got.off_us == FRAME_US_MAX && got.take_us == 0 &&
               got.copy_us == FRAME_US_MAX && got.done_us == FRAME_US_NONE && got.flags == (FRAME_TIMED | PAGANI_TIMING_FLAGS));
        assert(!frame_ring_breakdown(r, 3, &got));                          // not recorded yet (zero words)
        assert(!frame_ring_breakdown(r, 2 + FRAME_RING, &got));             // the same words, another generation
        // Frame n + FRAME_RING takes the words of frame n: frame n's breakdown is gone, the new one reads back.
        for (uint64_t n = 3; n < FRAME_RING; n++) frame_ring_record(r, FRAME_GPU, 300 + n);
        b = breakdown(1, 2, 3, 4, 5, FRAME_TIMED);
        frame_ring_record_breakdown(r, FRAME_GPU, 5000, &b);               // frame FRAME_RING, in slot 0
        assert(!frame_ring_breakdown(r, 0, &got) && frame_ring_breakdown(r, FRAME_RING, &got) && got.gap_us == 1 && got.done_us == 5);
        checks++;
    }
    {   // Four writers recording breakdowns that encode their own frame while a reader checks every frame it can read: a breakdown that
        // reads back always belongs to the frame whose slot is there (or reads back not at all).
        frame_ring *r = fresh();
        static const unsigned writers = 4, each = 6000;
        bool stop = false;
        uint64_t consistent = 0, mismatched = 0;     // reader's counts
        std::thread reader([&] {
            while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
                const uint64_t count = __atomic_load_n(&r->count, __ATOMIC_ACQUIRE);
                for (uint64_t n = count > FRAME_RING ? count - FRAME_RING : 0; n < count; n++) {
                    uint64_t stamp = 0; unsigned path = 0; frame_breakdown got;
                    if (!frame_ring_get(r, n, &stamp, &path) || !frame_ring_breakdown(r, n, &got)) continue;
                    // The writer put stamp = (writer << 32 | k) + 1 and gap = k % 1000000, copy = writer: check the pair.
                    // Words that pass carry frame n's generation, and only frame n's writer stores that generation there: they are
                    // frame n's (a later frame that took them since carries another generation and fails the read).
                    const uint64_t k = (stamp - 1) & 0xffffffffu, writer = (stamp - 1) >> 32;
                    if (got.gap_us == k % 1000000 && got.copy_us == writer && got.flags == (FRAME_TIMED | (uint32_t)writer)) consistent++;
                    else mismatched++;
                }
            }
        });
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < writers; w++)
            pool.emplace_back([r, w] {
                for (unsigned k = 0; k < each; k++) {
                    const frame_breakdown b = breakdown(k % 1000000, 0, 0, w, 0, FRAME_TIMED | w);
                    frame_ring_record_breakdown(r, w & 1, ((uint64_t)w << 32 | k) + 1, &b);
                }
            });
        for (auto &t : pool) t.join();
        __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
        reader.join();
        assert(r->count == writers * each && !mismatched);
        // Afterwards every frame the ring holds reads back with its own breakdown.
        unsigned held = 0;
        for (uint64_t n = r->count - FRAME_RING; n < r->count; n++) {
            uint64_t stamp = 0; unsigned path = 0; frame_breakdown got;
            assert(frame_ring_get(r, n, &stamp, &path) && frame_ring_breakdown(r, n, &got));
            const uint64_t k = (stamp - 1) & 0xffffffffu, writer = (stamp - 1) >> 32;
            assert(got.gap_us == k % 1000000 && got.copy_us == writer && path == (writer & 1));
            held++;
        }
        assert(held == FRAME_RING);
        (void)consistent;
        checks++;
    }
    {   // The scan names the frame that ended the largest interval of the last second (the newest of equal ones) and each jitter's frame.
        frame_ring *r = fresh();
        const uint64_t t0 = 100 * kHz;
        uint64_t t = t0;
        for (unsigned k = 0; k < 30; k++) {frame_ring_record(r, FRAME_GPU, t); t += kMs * 16;}    // frames 0-29
        t += kMs * 72;                                                                              // frame 30: 88 ms after 29
        frame_ring_record(r, FRAME_GPU, t);
        t += kMs * 16;
        frame_ring_record(r, FRAME_GPU, t);                                                         // frame 31
        t += kMs * 88;
        frame_ring_record(r, FRAME_CPU, t);                                                         // frame 32: also 88 ms
        t += kMs * 10;
        frame_ring_record(r, FRAME_GPU, t);                                                         // frame 33
        frame_scan s;
        frame_ring_scan(r, r->count, t + kMs, kHz, 0, &s);
        assert(s.intervals == 33 && s.max_us == 88000 && s.max_n == 32);          // the newest of the two 88 ms intervals
        assert(s.jitters == 2 && s.kept == 2 && s.events[0].n == 32 && s.events[1].n == 30);
        assert(s.events[0].path == FRAME_CPU && s.events[0].interval_us == 88000 && s.events[1].interval_us == 88000);
        // Without frame times (a single frame, or only idle gaps) max_n says nothing (0) and the row has no slow frame.
        r = fresh();
        frame_ring_record(r, FRAME_GPU, t0);
        frame_ring_scan(r, r->count, t0 + kMs, kHz, 0, &s);
        assert(s.intervals == 0 && s.max_n == 0 && s.max_us == 0);
        checks++;
    }
    {   // The stage word: the stage in bits [58:56], the stamp in [52:0]; every stage round-trips; the stamp is masked.
        for (unsigned stage = 0; stage < PAGANI_STAGES; stage++) {
            const uint64_t word = frame_stage_word(stage, 0x1fffffffffffffULL - stage);
            assert(frame_stage_of(word) == stage && frame_stage_stamp(word) == 0x1fffffffffffffULL - stage);
        }
        assert(frame_stage_stamp(frame_stage_word(PAGANI_STAGE_TAKEN, (1ULL << 60) | 1234)) == 1234);
        assert(frame_stage_of(0) == PAGANI_STAGE_NONE && PAGANI_STAGES <= 8);
        assert(!frame_stage_name(PAGANI_STAGE_NONE) && !strcmp(frame_stage_name(PAGANI_STAGE_ARRIVED), "off") &&
               !strcmp(frame_stage_name(PAGANI_STAGE_OFFERED), "take") && !strcmp(frame_stage_name(PAGANI_STAGE_TAKEN), "copy") &&
               !strcmp(frame_stage_name(PAGANI_STAGE_COPIED), "done") && !strcmp(frame_stage_name(PAGANI_STAGE_IDLE), "idle") &&
               !frame_stage_name(PAGANI_STAGES));
        checks++;
    }
    {   // Durations: "-" without one, one decimal below 100 ms, whole milliseconds below 10 s, then whole seconds; at most four characters.
        assert(dur(FRAME_US_NONE) == "-" && dur(0) == "0.0" && dur(49) == "0.0" && dur(50) == "0.1" && dur(17249) == "17.2");
        assert(dur(99949) == "99.9" && dur(99950) == "100" && dur(412000) == "412" && dur(9999499) == "9999");
        assert(dur(9999500) == "10s" && dur(FRAME_US_MAX) == "67s");
        // How long the path has waited: "ms" below a second, then seconds with one decimal below 100 s, then whole ones (99999 at most).
        assert(age(0) == "0.0ms" && age(412000) == "412ms" && age(999499) == "999ms" && age(999500) == "1.0s");
        assert(age(2345678) == "2.3s" && age(99949999) == "99.9s" && age(99950000) == "100s" && age(UINT64_MAX / 2) == "99999s");
        checks++;
    }
    {   // The SLOW row, exactly: the task's example (with " done", which fits), without a breakdown, without a frame interval.
        const frame_breakdown b = breakdown(70100, 200, 300, 17000, 100, FRAME_TIMED);
        assert(slowRow(slow(true, 88000, &b, 0, 0)) == "SLOW 88.0ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0");
        assert(slowRow(slow(true, 88000, &b, 0, 0)).size() == 64);
        assert(slowRow(slow(true, 88000, nullptr, 2, 1)) == "SLOW 88.0ms fb2 to1");
        // A breakdown with missing stages (a GPU frame copied before a lost hand-off has no "take"); presented at its copy: no " done".
        const frame_breakdown cpu = breakdown(FRAME_US_NONE, 50, 0, 143000, FRAME_US_NONE, FRAME_TIMED | PAGANI_TIMING_REFUSED);
        assert(slowRow(slow(true, 285000, &cpu, 61, 0)) == "SLOW 285ms gap - off 0.1 take 0.0 copy 143 fb61 to0");
        // No frame interval in the last second: what the path waits for and since when; nothing before the first frame.
        assert(slowRow(slow(false, 0, nullptr, 0, 3, PAGANI_STAGE_TAKEN, 2345678)) == "SLOW - copy 2.3s fb0 to3");
        assert(slowRow(slow(false, 0, nullptr, 0, 10, PAGANI_STAGE_IDLE, 12300000)) == "SLOW - idle 12.3s fb0 to10");
        assert(slowRow(slow(false, 0, nullptr, 1, 0, PAGANI_STAGE_OFFERED, 51000)) == "SLOW - take 51.0ms fb1 to0");
        assert(slowRow(slow(false, 0, nullptr, 0, 0, PAGANI_STAGE_ARRIVED, 999999999999ULL)) == "SLOW - off 99999s fb0 to0");
        assert(slowRow(slow(false, 0, nullptr, 0, 0)) == "SLOW - fb0 to0");
        // The widest row: " done" goes (on the screen), the rest stays within 65; the log's width keeps it; counts stop at 999.
        const frame_breakdown wide = breakdown(9999499, 9999499, 9999499, 9999499, 9999499, FRAME_TIMED);
        const std::string screen = slowRow(slow(true, 999999, &wide, 5000, 5000));
        assert(screen == "SLOW 1000ms gap 9999 off 9999 take 9999 copy 9999 fb999 to999" && screen.size() <= 65);
        assert(slowRow(slow(true, 999999, &wide, 5000, 5000), 96) ==
               "SLOW 1000ms gap 9999 off 9999 take 9999 copy 9999 done 9999 fb999 to999");
        // " done" is the only part that depends on the width: in at exactly 65 characters, out at 66.
        const frame_breakdown edge = breakdown(10000, 1000, 1000, 10000, 1000, FRAME_TIMED);
        assert(slowRow(slow(true, 88000, &edge, 10, 0)) == "SLOW 88.0ms gap 10.0 off 1.0 take 1.0 copy 10.0 done 1.0 fb10 to0");
        assert(slowRow(slow(true, 88000, &edge, 10, 0)).size() == 65);
        assert(slowRow(slow(true, 88000, &edge, 10, 10)) == "SLOW 88.0ms gap 10.0 off 1.0 take 1.0 copy 10.0 fb10 to10");
        const frame_breakdown tens = breakdown(9999, 9999, 9999, 9999, 9999, FRAME_TIMED);   // "10.0" each: 67 with " done"
        assert(slowRow(slow(true, 88000, &tens, 0, 0)) == "SLOW 88.0ms gap 10.0 off 10.0 take 10.0 copy 10.0 fb0 to0");
        // Every combination of value widths: never more than 65 on the screen.
        const uint32_t samples[] = {FRAME_US_NONE, 0, 9949, 99949, 412000, 9999499, FRAME_US_MAX};
        const uint64_t intervals[] = {0, 16700, 99949, 999999};
        const uint32_t counts[] = {0, 9, 99, 999, 100000};
        unsigned rows = 0;
        for (uint32_t a : samples) for (uint32_t c : samples) for (uint32_t d : samples) for (uint64_t i : intervals)
            for (uint32_t f : counts) {
                const frame_breakdown x = breakdown(a, c, d, a, c, FRAME_TIMED);
                assert(slowRow(slow(true, i, &x, f, f)).size() <= 65);
                assert(slowRow(slow(true, i, &x, f, f), 96).size() <= 96);
                rows++;
            }
        for (unsigned stage = 0; stage < PAGANI_STAGES; stage++)
            for (uint64_t us : {0ULL, 999499ULL, 99949999ULL, UINT64_MAX / 3}) assert(slowRow(slow(false, 0, nullptr, 999, 999, stage, us)).size() <= 65);
        assert(rows == 7 * 7 * 7 * 4 * 5);
        // A short buffer cuts the row (and still terminates it).
        char small[12];
        const frame_slow cut = slow(true, 88000, &b, 0, 0);
        assert(frame_slow_row(small, sizeof small, 65, &cut) == 11 && !strcmp(small, "SLOW 88.0ms"));
        assert(frame_slow_row(small, 0, 65, &cut) == 0);
        checks++;
    }
    {   // The J record's breakdown of the late frame: all five spans, its flags in words, this second's fallbacks and empty waits.
        const frame_breakdown b = breakdown(70100, 200, 300, 17000, 100, FRAME_TIMED);
        assert(detail(&b, 0, 0) == "gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0");
        const frame_breakdown all = breakdown(FRAME_US_NONE, 141000, FRAME_US_NONE, FRAME_US_MAX, 0, FRAME_TIMED | PAGANI_TIMING_FLAGS);
        assert(detail(&all, 3, 1000) == "gap - off 141 take - copy 67s done 0.0 refused gpu-failed bypass deferred flip fb3 to999");
        assert(detail(nullptr, 2, 7) == "fb2 to7");
        char cut[10];
        assert(frame_jitter_detail(cut, sizeof cut, &b, 0, 0) == 9 && !strcmp(cut, "gap 70.1 "));
        checks++;
    }
    printf("PASS: frame timing (%u groups): microseconds from ticks, breakdowns from the stages, breakdown words with generations and "
           "four writers against a reader, the frame of the largest interval and of each jitter, the stage word, durations, the SLOW "
           "row (exact, \" done\" only while it fits, at most 65 characters), the J breakdown\n", checks);
    return 0;
}
