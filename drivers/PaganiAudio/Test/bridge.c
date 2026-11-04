// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the haptic actuator bridge (PaganiAudioHaptics.c) on the real engine's timeline (the 19.2 MHz timer: 400 ticks a frame)
// with a fake of the vibrator's stream API (PaganiHaptics.h: begin, write, end, and its runtime cap) that records every call and every
// sample. The plug-in's side (hal/PaganiAudioPlugIn.c writeActuator) is played here by writing mixed 16-bit frames into the actuator ring
// with its written position, as the plug-in leaves them. Checked: the header the plug-in validates; the scaling (a sample of 1.0 is the
// 7450 mV full scale, the stream's VMAX the cap: int8 = round(x * 127 * 7450 / cap), +-127, clips counted) and its rounding; the
// decimation to the FIFO rate (an average of 48000 / rate frames) for each rate; nothing sent while silent; a stream begun at the first
// loud sample with the cap as its VMAX, the silences inside it carried, its end with a drain after 100 ms of silence, the length limit;
// a refused begin not asked again within a second; samples the driver refuses counted; frames taken PAGANI_AUDIO_ACT_TAKE_FRAMES ahead
// of their time and cleared, the take position published; a stall longer than the ring; the run's end PAGANI_AUDIO_HAPTICS_KEEP_FRAMES
// after the last client stopped, with what the ring still held cleared; the latency from the plug-in's write of a sound to the begin; the
// sustained level: a short sound passes at the cap, a long one is clipped to 1260 mV after the 80 ms credit, the credit comes back in
// silence and between runs, repeated bursts cannot get round it, a cap at or below the level changes nothing, a boot argument moves it.
// Build: cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. bridge.c ../PaganiAudioEngine.c ../PaganiAudioHaptics.c -o bridge
#include "PaganiAudioHaptics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

enum {kAnchor = 1000000, kTicks = 400, R = PAGANI_AUDIO_ACT_RING_FRAMES};

// ---- the fake vibrator ---------------------------------------------------------------------------------------------------------------
typedef struct {
    int begins, ends, writes, begin_rc, last_drain, room;   // room: samples its ring takes per write call (-1 all)
    uint32_t rate, mv, cap;
    int8_t samples[1 << 20];
    uint32_t count;
    int write_rc;                                          // a negative answer to every write (0: none)
} Fake;
static Fake gFake;
static int fakeBegin(void *c, uint32_t rate, uint32_t mv) {(void)c; gFake.begins++; gFake.rate = rate; gFake.mv = mv; return gFake.begin_rc;}
static int fakeWrite(void *c, const int8_t *s, uint32_t n)
{
    (void)c;
    gFake.writes++;
    if (gFake.write_rc) return gFake.write_rc;
    const uint32_t take = gFake.room >= 0 && (uint32_t)gFake.room < n ? (uint32_t)gFake.room : n;
    for (uint32_t i = 0; i < take && gFake.count < sizeof gFake.samples; ++i) gFake.samples[gFake.count++] = s[i];
    return (int)take;
}
static int fakeEnd(void *c, int drain) {(void)c; gFake.ends++; gFake.last_drain = drain; return 0;}
static uint32_t fakeCap(void *c) {(void)c; return gFake.cap;}
static const PaganiAudioHapticsOps kOps = {fakeBegin, fakeWrite, fakeEnd, fakeCap, NULL};

// ---- the fixture: the engine (for its timeline and act_users), the actuator memory, the bridge -----------------------------------------
typedef struct {
    PaganiAudioEngine e;
    void *page, *act;
    int16_t *out, *in, *ring;
    PaganiAudioActuatorControl *ctl;
    PaganiAudioHaptics h;
} Fixture;
static uint64_t at(uint64_t frame) {return kAnchor + frame * kTicks;}

static void setup(Fixture *f, uint32_t rate, uint32_t cap)
{
    memset(f, 0, sizeof *f);
    memset(&gFake, 0, sizeof gFake);
    gFake.room = -1;
    gFake.cap = cap;
    f->page = aligned_alloc(4096, PAGANI_AUDIO_CONTROL_BYTES);
    f->out = calloc(PAGANI_AUDIO_RING_FRAMES * 2, sizeof(int16_t));
    f->in = calloc(PAGANI_AUDIO_RING_FRAMES, sizeof(int16_t));
    f->act = aligned_alloc(4096, PAGANI_AUDIO_ACT_BYTES);
    memset(f->act, 0xa5, PAGANI_AUDIO_ACT_BYTES);   // what the allocator leaves: init clears it
    CHECK(pagani_audio_engine_init(&f->e, f->page, f->out, f->in, kAnchor, 625, 12, &pagani_audio_silent_backend, PaganiAudioOptionActuator) == 0);
    CHECK(pagani_audio_haptics_init(&f->h, &f->e, f->act, &kOps, rate, 0, 0) == 0);
    f->ctl = f->act;
    f->ring = (int16_t *)((uint8_t *)f->act + PAGANI_AUDIO_ACT_HEADER_BYTES);
}
static void teardown(Fixture *f) {free(f->page); free(f->out); free(f->in); free(f->act);}

// The plug-in's mix, already summed: frames [t, t + n) with the value v(i), written at host time `host`.
static void plug(Fixture *f, uint64_t t, uint32_t n, int16_t (*v)(uint32_t), uint64_t host)
{
    int loud = 0;
    for (uint32_t i = 0; i < n; ++i) {
        f->ring[(t + i) & (R - 1)] = v(i);
        loud |= v(i) != 0;
    }
    if (loud && !f->ctl->sound_host) {f->ctl->sound_frame = t; f->ctl->sound_host = host;}
    if (t + n > f->ctl->written) f->ctl->written = t + n;
    f->ctl->written_host = host;
}
static int16_t sine170(uint32_t i) {return (int16_t)lrint(0.3 * 32767.0 * sin(2 * M_PI * 170.0 * i / 48000.0));}
static int16_t full(uint32_t i) {return (int16_t)(i / 141 % 2 ? 32767 : -32767);}   // a 170 Hz square wave at full scale
static int16_t nyquist(uint32_t i) {return (int16_t)(i % 2 ? 32767 : -32767);}       // 24 kHz: the decimation's average removes it
static int16_t quiet(uint32_t i) {(void)i; return 0;}
static int16_t tiny(uint32_t i) {(void)i; return 40;}   // 0.0012: rounds to 0 at the default cap

// One service round every 2 ms from frame `from` to frame `to`.
static void run(Fixture *f, uint64_t from, uint64_t to)
{
    for (uint64_t t = from; t <= to; t += 96) pagani_audio_haptics_poll(&f->h, &f->e, at(t));
}

static void test_header(void)
{
    Fixture f;
    setup(&f, 8000, 3600);
    const PaganiAudioActuatorControl *c = f.ctl;
    CHECK(c->magic == PAGANI_AUDIO_ACT_MAGIC && c->version == PAGANI_AUDIO_ACT_VERSION && c->rate == 48000 && c->ring_frames == R);
    CHECK(c->io_frames == 256 && c->safety_frames == 128 && c->take_frames == 64 && c->anchor_host == kAnchor);
    CHECK(c->ticks_per_frame_q32 == ((const PaganiAudioControl *)f.page)->ticks_per_frame_q32 && c->ticks_per_frame_q32 == 400ULL << 32);
    CHECK(c->state == 0 && c->written == 0 && c->taken == 0 && f.ring[0] == 0 && f.ring[R - 1] == 0);
    CHECK(f.h.rate == 8000 && f.h.decimate == 6 && f.h.full_mv == 7450 && f.h.silence_samples == 800 && f.h.stream_samples == 200000);
    // The rates the PMIC FIFO takes for streams that divide 48 kHz; anything else is 8 kHz. The full scale at most the board's.
    const uint32_t rates[][2] = {{16000, 3}, {24000, 2}, {48000, 1}, {44100, 6}, {0, 6}, {12345, 6}};
    for (unsigned i = 0; i < 6; ++i) {
        teardown(&f);
        setup(&f, rates[i][0], 3600);
        CHECK(f.h.decimate == rates[i][1] && f.h.rate * f.h.decimate == 48000);
    }
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &kOps, 8000, 9000, 0) == 0 && f.h.full_mv == 7450);
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &kOps, 8000, 5000, 0) == 0 && f.h.full_mv == 5000);
    PaganiAudioHapticsOps none = kOps;
    none.begin = NULL;
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &none, 8000, 0, 0) == -1 && pagani_audio_haptics_init(&f.h, &f.e, NULL, &kOps, 8000, 0, 0) == -1);
    teardown(&f);
}

static void test_scale(void)
{
    int clipped = 0;
    // 1.0 is 7450 mV; at the default 3600 mV cap that is 2.07 times the stream's full scale: clipped.
    CHECK(pagani_audio_haptics_scale(32767, 1, 7450, 3600, &clipped) == 127 && clipped);
    CHECK(pagani_audio_haptics_scale(-32767, 1, 7450, 3600, &clipped) == -127 && clipped);
    // 0.25: 0.25 * 127 * 7450 / 3600 = 65.7 -> 66; the same at a cap of 7450 is 31.75 -> 32; 1.0 at 7450 is exactly 127.
    CHECK(pagani_audio_haptics_scale(8192, 1, 7450, 3600, &clipped) == 66 && !clipped);
    CHECK(pagani_audio_haptics_scale(-8192, 1, 7450, 3600, &clipped) == -66 && !clipped);
    CHECK(pagani_audio_haptics_scale(8192, 1, 7450, 7450, &clipped) == 32 && pagani_audio_haptics_scale(32767, 1, 7450, 7450, &clipped) == 127 && !clipped);
    // The average of the decimated frames; rounding half away from zero, symmetric.
    CHECK(pagani_audio_haptics_scale(6 * 8192, 6, 7450, 3600, &clipped) == 66);
    CHECK(pagani_audio_haptics_scale(0, 6, 7450, 3600, &clipped) == 0 && pagani_audio_haptics_scale(62, 1, 7450, 3600, &clipped) == 0);
    CHECK(pagani_audio_haptics_scale(63, 1, 7450, 3600, &clipped) == 1 && pagani_audio_haptics_scale(-63, 1, 7450, 3600, &clipped) == -1);
    CHECK(pagani_audio_haptics_scale(100, 0, 7450, 3600, &clipped) == 0 && pagani_audio_haptics_scale(100, 1, 7450, 0, &clipped) == 0);
    // The extremes of a 48 kHz sum stay inside 64 bits: 6 frames at full scale, at the lowest cap.
    CHECK(pagani_audio_haptics_scale(6 * 32767, 6, 7450, 50, &clipped) == 127 && clipped);
}

static void test_idle_and_silence(void)
{
    Fixture f;
    setup(&f, 8000, 3600);
    // No client: idle, nothing taken.
    CHECK(pagani_audio_haptics_poll(&f.h, &f.e, at(1000)) == 1000 && f.h.stats.frames == 0 && !f.h.running && f.ctl->state == 0);
    // A client: the run starts at the frame that plays now, frames are taken 64 ahead, the position published, rounds 2 ms apart.
    f.e.act_users = 1;
    CHECK(pagani_audio_haptics_poll(&f.h, &f.e, at(2000)) == PAGANI_AUDIO_HAPTICS_POLL_MS);
    CHECK(f.h.running && f.h.stats.runs == 1 && (f.ctl->state & PaganiAudioActuatorRunning) && f.ctl->taken == 2064 && f.h.taken == 2064);
    // Silence (and a sample too small to drive anything) is taken and cleared, nothing goes to the vibrator.
    plug(&f, 2064, 2000, tiny, at(2000));
    run(&f, 2000, 5000);
    CHECK(gFake.begins == 0 && gFake.writes == 0 && f.h.stats.loud_frames == 2000 && f.h.stats.peak_out == 0);
    CHECK(f.ring[(2064 + 100) & (R - 1)] == 0 && f.ctl->taken > 5000);
    teardown(&f);
}

static void test_stream(void)
{
    Fixture f;
    setup(&f, 8000, 3600);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(10000));
    // A 20 ms burst of a 170 Hz sine at 0.3, written 384 frames ahead (as the plug-in writes), followed by silence.
    plug(&f, 10400, 960, sine170, at(10016));
    plug(&f, 11360, 9600, quiet, at(10016));
    run(&f, 10016, 10400 - 64 - 1);
    CHECK(gFake.begins == 0);                              // not due yet
    run(&f, 10400 - 64, 10400 + 960 + 4800 + 2000);
    // One stream: begun with the FIFO rate and the cap as its VMAX, the burst and its silence carried, ended with a drain after 100 ms.
    CHECK(gFake.begins == 1 && gFake.rate == 8000 && gFake.mv == 3600 && gFake.ends == 1 && gFake.last_drain == 1);
    CHECK(f.h.stats.streams == 1 && f.h.stats.stream_ends == 1 && f.h.stats.rollovers == 0 && !f.h.streaming);
    // 160 samples of the burst (960 / 6) minus the leading ones that round to 0, then 800 silent ones: the end.
    CHECK(gFake.count >= 155 + 800 && gFake.count <= 160 + 800 && f.h.stats.samples == gFake.count && f.h.stats.refused == 0);
    int peak = 0, zeros_after = 0;
    for (uint32_t i = 0; i < gFake.count; ++i) {
        const int m = abs(gFake.samples[i]);
        if (m > peak) peak = m;
    }
    for (uint32_t i = gFake.count - 800; i < gFake.count; ++i) zeros_after += gFake.samples[i] == 0;
    // 0.3 * 127 * 7450 / 3600 = 78.8; the 6-frame average of a 170 Hz sine loses 0.04 %.
    CHECK(peak >= 77 && peak <= 79 && f.h.stats.peak_out == (uint32_t)peak && zeros_after == 800 && gFake.samples[0] != 0);
    CHECK(f.h.stats.peak_in >= 9829 && f.h.stats.peak_in <= 9831 && f.h.stats.clips == 0);
    // The latency: from the plug-in's write of the sound (frame 10016) to the begin (the round after frame 10400 - 64 came due).
    CHECK(f.h.stats.latency_us > 0 && f.h.stats.latency_us <= (int32_t)((10400 - 10016 + 96) * 1000000ull / 48000));
    // The frames are cleared behind the take position.
    CHECK(f.ring[10400 & (R - 1)] == 0 && f.ring[(10400 + 480) & (R - 1)] == 0);
    // A second burst begins a second stream.
    const uint64_t t = f.h.taken + 384;
    f.ctl->sound_host = 0;
    plug(&f, t, 960, full, at(t - 384));
    run(&f, t - 384, t + 960 + 4800 + 2000);
    CHECK(gFake.begins == 2 && gFake.ends == 2 && f.h.stats.clips > 0 && f.h.stats.peak_out == 127);
    // Far above anything an actuator follows (24 kHz at full scale): the 6-frame average is silence, no stream.
    const uint64_t u = f.h.taken + 384;
    plug(&f, u, 4800, nyquist, at(u - 384));
    run(&f, u - 384, u + 4800 + 2000);
    CHECK(gFake.begins == 2 && f.h.stats.loud_frames > 4800);
    teardown(&f);
}

// The plug-in writing sound continuously, 4096 frames at a time ahead of the rounds, from frame `from` to `to`.
static void sound(Fixture *f, uint64_t from, uint64_t to)
{
    for (uint64_t t = from; t < to; t += 4096) {
        plug(f, t + 64, 4096, full, at(t));
        run(f, t, t + 4096 - 1);
    }
}

static void test_refusals(void)
{
    Fixture f;
    setup(&f, 8000, 0);   // the driver says no cap: the bridge's default 3600
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    gFake.begin_rc = -19;   // the vibrator is not running
    sound(&f, 0, 24576);    // half a second of sound: one begin refused, not asked again within a second
    CHECK(gFake.begins == 1 && f.h.stats.begin_failed == 1 && f.h.stats.last_rc == -19 && gFake.writes == 0 && !f.h.streaming);
    sound(&f, 24576, 49152);
    CHECK(gFake.begins == 2 && f.h.stats.begin_failed == 2);
    // The driver comes up: the next try begins a stream at the default cap.
    gFake.begin_rc = 0;
    sound(&f, 49152, 98304 + 4096);
    CHECK(gFake.begins == 3 && gFake.mv == 3600 && f.h.streaming && f.h.stats.streams == 1 && f.h.stats.refused == 0);
    // Its ring takes only 10 samples a call: the rest is counted as refused; a failing write too.
    gFake.room = 10;
    sound(&f, 98304 + 4096, 98304 + 3 * 4096);
    CHECK(f.h.stats.refused > 0);
    gFake.write_rc = -19;
    sound(&f, 98304 + 3 * 4096, 98304 + 4 * 4096);
    CHECK(f.h.stats.last_rc == -19);
    teardown(&f);
}

static void test_stop_and_stall(void)
{
    Fixture f;
    setup(&f, 24000, 7450);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    plug(&f, 64, 4800, full, at(0));
    run(&f, 0, 2400);
    CHECK(f.h.streaming && gFake.mv == 7450 && f.h.stats.clips == 0 && f.h.decimate == 2);
    // The last client stops: frames still come due for PAGANI_AUDIO_HAPTICS_KEEP_FRAMES (the plug-in wrote ahead), then the run ends,
    // the stream drains, and what the ring still holds is cleared and counted.
    f.e.act_users = 0;
    const uint64_t stop = 2400 + 96;
    pagani_audio_haptics_poll(&f.h, &f.e, at(stop));
    CHECK(f.h.running && f.h.stop_pending && f.h.stop_frame == stop + PAGANI_AUDIO_HAPTICS_KEEP_FRAMES);
    f.ring[(stop + 3000) & (R - 1)] = 5;                         // written after the clients stopped: never played
    run(&f, stop + 96, stop + 700);
    CHECK(!f.h.running && f.h.stats.run_ends == 1 && gFake.ends == 1 && gFake.last_drain == 1 && f.h.stats.stale >= 1);
    CHECK(f.ctl->state == 0 && f.ring[(stop + 3000) & (R - 1)] == 0 && f.h.taken == stop + PAGANI_AUDIO_HAPTICS_KEEP_FRAMES);
    CHECK(pagani_audio_haptics_poll(&f.h, &f.e, at(stop + 800)) == 1000);
    // A client again within the keep time cancels the stop.
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(10000));
    f.e.act_users = 0;
    pagani_audio_haptics_poll(&f.h, &f.e, at(10096));
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(10192));
    CHECK(f.h.running && !f.h.stop_pending && f.h.stats.runs == 2);
    // A stall longer than the ring: the frames that alias are skipped (counted), the walk stays one ring long.
    pagani_audio_haptics_poll(&f.h, &f.e, at(10192 + 3 * R));
    CHECK(f.h.stats.skipped >= 2 * R - 200 && f.h.taken == 10192 + 3 * R + 64);
    teardown(&f);
}

static void test_rollover(void)
{
    Fixture f;
    setup(&f, 8000, 3600);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    // 26 s of continuous sound: the stream ends at 25 s (the driver stops one at 30) and the next sample begins another.
    sound(&f, 0, 48000ull * 26);
    CHECK(f.h.stats.rollovers == 1 && gFake.begins == 2 && gFake.ends == 1 && f.h.streaming);
    teardown(&f);
}

// A loud sine from frame `from` for `frames` frames, written and taken as the plug-in and the rounds do.
static void buzz(Fixture *f, uint64_t from, uint64_t frames)
{
    for (uint64_t t = from; t < from + frames; t += 2048) {
        const uint32_t n = from + frames - t < 2048 ? (uint32_t)(from + frames - t) : 2048;
        plug(f, t, n, full, at(t > 384 ? t - 384 : 0));
        run(f, t, t + n);
    }
}
static int peak_of(uint32_t from, uint32_t to)
{
    int peak = 0;
    for (uint32_t i = from; i < to && i < gFake.count; ++i) if (abs(gFake.samples[i]) > peak) peak = abs(gFake.samples[i]);
    return peak;
}

static void test_sustained(void)
{
    Fixture f;
    setup(&f, 8000, 3600);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    // 127 * 1260 / 3600 = 44: the most a sustained sample may be; a credit of 80 ms is 640 samples at 8 kHz.
    CHECK(f.h.sustain_mv == 1260 && f.h.credit_max == 640 * PAGANI_AUDIO_HAPTICS_RECOVERY);
    // A tap of 50 ms: all of it at the cap.
    buzz(&f, 1000, 2400);
    CHECK(gFake.begins == 1 && f.h.sustain_q == 44 && peak_of(0, gFake.count) == 127 && f.h.stats.limited == 0 && f.h.credit > 0);
    // The silence after it gives the credit back (a quarter of the speed), through the stream's end and the rounds without a stream.
    const uint64_t rest = f.h.taken + 48000 * 2;
    run(&f, f.h.taken, rest);
    CHECK(!f.h.streaming && f.h.credit == 0);
    // Three seconds of a full scale buzz: the first 80 ms or so at the cap, everything later at the sustained level, and no sample above it.
    const uint32_t mark = gFake.count;
    buzz(&f, rest, 48000 * 3);
    const uint32_t burst = 8000 * 90 / 1000;
    CHECK(f.h.streaming && gFake.count >= mark + 8000 * 3 - 96 && peak_of(mark, mark + 400) == 127);
    CHECK(peak_of(mark + burst, gFake.count) <= 127 && f.h.stats.limited > 8000 * 2);
    uint32_t over = 0;
    for (uint32_t i = mark + burst; i < gFake.count; ++i) over += abs(gFake.samples[i]) > 44;
    CHECK(over * 50 <= gFake.count - mark - burst && peak_of(gFake.count - 1000, gFake.count) >= 44);   // at most a stray sample per cycle
    // Repeated bursts of 40 ms with 40 ms of silence: the credit runs out over them too (duty above the fifth it allows).
    setup(&f, 8000, 3600);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    uint64_t t = 1000;
    for (int i = 0; i < 12; ++i, t += 3840) buzz(&f, t, 1920);
    CHECK(f.h.stats.limited > 0 && f.h.streaming);
    // Between runs the credit comes back with the time that passed, no faster.
    f.e.act_users = 0;
    run(&f, f.h.taken, f.h.taken + 2000);
    CHECK(!f.h.running);
    const uint32_t used = f.h.credit;
    CHECK(used > 32);
    const uint64_t end = f.h.taken;
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(end + 6 * 5));
    // 5 FIFO samples of rest, and the 94 frames this round took after them (silent: 15 samples).
    CHECK(f.h.running && used - f.h.credit >= 5 && used - f.h.credit <= 5 + 16);
    f.e.act_users = 0;
    run(&f, f.h.taken, f.h.taken + 2000);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(f.h.taken + 6 * 100000));
    CHECK(f.h.running && f.h.credit == 0);
    teardown(&f);
    // A cap at the sustained level or below: nothing to limit.
    setup(&f, 8000, 1200);
    f.e.act_users = 1;
    pagani_audio_haptics_poll(&f.h, &f.e, at(0));
    buzz(&f, 1000, 48000);
    CHECK(f.h.sustain_q == 127 && f.h.stats.limited == 0 && peak_of(gFake.count - 1000, gFake.count) == 127);
    teardown(&f);
    // The boot argument's level, its floor and its ceiling, and the other FIFO rates' credit.
    setup(&f, 8000, 3600);
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &kOps, 16000, 0, 2000) == 0 && f.h.sustain_mv == 2000 && f.h.credit_max == 1280 * PAGANI_AUDIO_HAPTICS_RECOVERY);
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &kOps, 8000, 0, 9) == 0 && f.h.sustain_mv == 50);
    CHECK(pagani_audio_haptics_init(&f.h, &f.e, f.act, &kOps, 8000, 5000, 9000) == 0 && f.h.sustain_mv == 5000);
    teardown(&f);
}

int main(void)
{
    test_header();
    test_scale();
    test_idle_and_silence();
    test_stream();
    test_refusals();
    test_stop_and_stall();
    test_rollover();
    test_sustained();
    printf("PASS bridge: %u checks (scaling, decimation, silence, streams, refusals, stop, stall, rollover, sustained level)\n", checks);
    return 0;
}
