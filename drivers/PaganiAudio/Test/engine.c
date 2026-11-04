// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the PaganiAudio engine (PaganiAudioEngine.c) on synthetic host clocks: the timeline (400 ticks per frame on 19.2 MHz,
// exact ratios on other timebases, the control page), per-client requests, start/stop with the keep time, the silent backend's clocked
// consumption and capture, period wrap at the ring's end, late and hole detection, stalls longer than the ring, backend switches and
// refusals, and the measured lead and drift resyncs of a completion-driven (DSP-like) backend.
// Build: cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. engine.c ../PaganiAudioEngine.c -o engine
#include "PaganiAudioEngine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

enum {P = PAGANI_AUDIO_PERIOD_FRAMES, R = PAGANI_AUDIO_RING_FRAMES};
static const uint64_t kAnchor = 1000000000ULL;   // host ticks at the anchor
static const uint32_t kNumer = 625, kDenom = 12; // 19.2 MHz: 52.083 ns per tick
#define TICKS(frames) ((uint64_t)(frames) * 400ULL)

typedef struct {
    PaganiAudioEngine e;
    PaganiAudioControl *ctl;
    int16_t *out, *in;
    void *page;
} Fixture;

static void setup(Fixture *f, const PaganiAudioBackend *backend, uint32_t options)
{
    memset(f, 0, sizeof *f);
    f->page = aligned_alloc(4096, PAGANI_AUDIO_CONTROL_BYTES);
    f->out = calloc((size_t)R * PAGANI_AUDIO_OUT_CHANNELS, sizeof(int16_t));
    f->in = calloc((size_t)R * PAGANI_AUDIO_IN_CHANNELS, sizeof(int16_t));
    memset(f->page, 0xa5, PAGANI_AUDIO_CONTROL_BYTES);
    memset(f->out, 0x5a, (size_t)R * PAGANI_AUDIO_OUT_CHANNELS * sizeof(int16_t));
    CHECK(pagani_audio_engine_init(&f->e, f->page, f->out, f->in, kAnchor, kNumer, kDenom, backend, options) == 0);
    f->ctl = f->page;
}

static void teardown(Fixture *f)
{
    free(f->page);
    free(f->out);
    free(f->in);
}

static uint64_t at_frame(uint64_t frame) {return kAnchor + TICKS(frame);}

// The plug-in's write: value v at frames [from, to) of both channels.
static void plugin_write(Fixture *f, uint64_t from, uint64_t to, int16_t v, uint64_t now)
{
    for (uint64_t t = from; t < to; ++t) f->out[2 * (t % R)] = f->out[2 * (t % R) + 1] = v;
    if (to > f->ctl->out_written) f->ctl->out_written = to;
    f->ctl->out_written_host = now;
}

static int period_clear(const Fixture *f, uint64_t n)
{
    for (uint64_t t = n * P; t < (n + 1) * P; ++t)
        if (f->out[2 * (t % R)] || f->out[2 * (t % R) + 1]) return 0;
    return 1;
}

static void test_timeline(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, PaganiAudioOptionTraceRouting);
    CHECK(f.e.tick_num == 400 && f.e.tick_den == 1);
    CHECK(f.ctl->ticks_per_frame_q32 == 400ULL << 32);
    CHECK(f.ctl->magic == PAGANI_AUDIO_MAGIC && f.ctl->version == PAGANI_AUDIO_VERSION);
    CHECK(f.ctl->rate == 48000 && f.ctl->out_channels == 2 && f.ctl->in_channels == 1);
    CHECK(f.ctl->period_frames == P && f.ctl->ring_frames == R && f.ctl->lead_frames == PAGANI_AUDIO_LEAD_FRAMES);
    CHECK(f.ctl->out_anchor_host == kAnchor && f.ctl->in_anchor_host == kAnchor);
    CHECK(f.ctl->backend == PaganiAudioBackendSilent && f.ctl->options == PaganiAudioOptionTraceRouting && f.ctl->state == 0);
    // The rest of the page (the plug-in's diagnostic half) and both rings start cleared.
    const uint8_t *page = f.page;
    int clear = 1;
    for (unsigned i = sizeof(PaganiAudioControl); i < PAGANI_AUDIO_CONTROL_BYTES; ++i) clear &= page[i] == 0;
    CHECK(clear && f.out[0] == 0 && f.out[2 * R - 1] == 0);
    CHECK(f.e.ticks_100ms == 1920000);
    CHECK(pagani_audio_engine_frame_at(&f.e, kAnchor) == 0 && pagani_audio_engine_frame_at(&f.e, kAnchor - 5) == 0);
    CHECK(pagani_audio_engine_frame_at(&f.e, kAnchor + 399) == 0 && pagani_audio_engine_frame_at(&f.e, kAnchor + 400) == 1);
    CHECK(pagani_audio_engine_frame_at(&f.e, at_frame(480)) == 480);
    CHECK(pagani_audio_engine_host_at(&f.e, 480) == kAnchor + 192000);
    // A day of frames stays exact.
    const uint64_t day = 48000ULL * 86400;
    CHECK(pagani_audio_engine_frame_at(&f.e, pagani_audio_engine_host_at(&f.e, day)) == day);
    teardown(&f);

    // Apple's 24 MHz timer (125 / 3): 500 ticks a frame; a nanosecond timebase (1 / 1): 62500 / 3 ticks a frame.
    PaganiAudioEngine e;
    static uint8_t page2[PAGANI_AUDIO_CONTROL_BYTES];
    static int16_t out[R * 2], in[R];
    CHECK(pagani_audio_engine_init(&e, page2, out, in, 7, 125, 3, &pagani_audio_silent_backend, 0) == 0);
    CHECK(e.tick_num == 500 && e.tick_den == 1 && ((PaganiAudioControl *)page2)->ticks_per_frame_q32 == 500ULL << 32);
    CHECK(pagani_audio_engine_init(&e, page2, out, in, 0, 1, 1, &pagani_audio_silent_backend, 0) == 0);
    CHECK(e.tick_num == 62500 && e.tick_den == 3);
    CHECK(((PaganiAudioControl *)page2)->ticks_per_frame_q32 == (62500ULL << 32) / 3);
    CHECK(pagani_audio_engine_frame_at(&e, 62500) == 3 && pagani_audio_engine_frame_at(&e, 62499) == 2);
    CHECK(pagani_audio_engine_host_at(&e, 3) == 62500 && pagani_audio_engine_host_at(&e, 1) == 20833);
    CHECK(pagani_audio_engine_frame_at(&e, 1000000000ULL) == 48000);
    // Refused: no timebase, no memory or no backend.
    CHECK(pagani_audio_engine_init(&e, page2, out, in, 0, 0, 1, &pagani_audio_silent_backend, 0) == -1);
    CHECK(pagani_audio_engine_init(&e, NULL, out, in, 0, 1, 1, &pagani_audio_silent_backend, 0) == -1);
    CHECK(pagani_audio_engine_init(&e, page2, out, in, 0, 1, 1, NULL, 0) == -1);
}

static void test_requests(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    PaganiAudioClientState a = {0}, b = {0};
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0 && f.e.out_users == 1);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0 && f.e.out_users == 1);   // counts once
    CHECK(pagani_audio_engine_request(&f.e, &b, PaganiAudioStartOutput) == 0 && f.e.out_users == 2);
    CHECK(pagani_audio_engine_request(&f.e, &b, PaganiAudioStartInput) == 0 && f.e.in_users == 1);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0 && f.e.out_users == 1);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0 && f.e.out_users == 1);    // not twice
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopInput) == 0 && f.e.in_users == 1);      // a never asked
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioSetHostClock) == -1 && pagani_audio_engine_request(&f.e, &a, 99) == -1);
    CHECK(f.e.stats.requests == 7 && f.e.stats.refused_requests == 2);
    pagani_audio_engine_release(&f.e, &b);   // b's process went away: both its directions
    CHECK(f.e.out_users == 0 && f.e.in_users == 0 && b.bits == PaganiAudioClientReleased);
    pagani_audio_engine_release(&f.e, &b);
    CHECK(f.e.out_users == 0 && f.e.in_users == 0 && b.bits == PaganiAudioClientReleased);
    // A request that reaches the service after its client closed (another thread of the dying process) is refused: no direction stays
    // counted that no close will release.
    CHECK(pagani_audio_engine_request(&f.e, &b, PaganiAudioStartOutput) == -1 && pagani_audio_engine_request(&f.e, &b, PaganiAudioStartInput) == -1);
    CHECK(f.e.out_users == 0 && f.e.in_users == 0 && f.e.stats.requests == 7 && f.e.stats.refused_requests == 4);
    // A request that landed before the close is taken back by it; the other client's count is untouched.
    PaganiAudioClientState c = {0};
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0 && pagani_audio_engine_request(&f.e, &c, PaganiAudioStartOutput) == 0);
    CHECK(pagani_audio_engine_request(&f.e, &c, PaganiAudioStartInput) == 0 && f.e.out_users == 2 && f.e.in_users == 1);
    pagani_audio_engine_release(&f.e, &c);
    CHECK(f.e.out_users == 1 && f.e.in_users == 0 && a.bits == PaganiAudioOutputRunning);
    CHECK(pagani_audio_engine_request(&f.e, &c, PaganiAudioStopOutput) == -1 && f.e.out_users == 1);
    teardown(&f);
}

static void test_silent_output(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    PaganiAudioClientState a = {0};
    // Idle: nothing runs, the thread sleeps a second.
    CHECK(pagani_audio_engine_poll(&f.e, at_frame(100), 0) == 1000 && !f.e.running && f.e.stats.taken == 0);
    // Start at frame 1000 (inside period 2): the run begins with period 3 and takes four ahead of the play position (2): 3..5.
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    uint32_t wait = pagani_audio_engine_poll(&f.e, at_frame(1000), 10);
    CHECK(f.e.running && (f.ctl->state & PaganiAudioOutputRunning) && f.e.stats.out_starts == 1);
    CHECK(f.e.first == 3 && f.e.next == 6 && f.e.done == 3 && f.e.stats.taken == 3);
    CHECK(wait == 10);   // 440 frames to the next period: 9.17 ms, rounded up
    // The ring was cleared at the start (it held 0x5a5a).
    CHECK(period_clear(&f, 3) && f.out[0] == 0 && f.out[2 * R - 1] == 0);
    // The plug-in writes periods 6..40 (300 ms ahead) with a tone; the service takes each four periods before it plays, and clears it.
    plugin_write(&f, 6 * P, 41 * P, 1000, at_frame(1000));
    pagani_audio_engine_poll(&f.e, at_frame(10 * P + 5), 110);   // play position: period 10
    CHECK(f.e.next == 14 && f.e.done == 10 && f.ctl->out_frames == 7 * P);
    for (uint64_t n = 6; n < 14; ++n) CHECK(period_clear(&f, n));
    CHECK(!period_clear(&f, 14) && !period_clear(&f, 40));
    CHECK(f.e.stats.peak_out == 1000 && f.e.stats.late == 0 && f.e.stats.holes == 0);
    // 2 x in_flight played: the lead is measured (about 3.5 periods for the silent queue) and the published lead stays 300 ms.
    pagani_audio_engine_poll(&f.e, at_frame(11 * P + 5), 120);
    CHECK(f.e.done == 11 && f.e.stats.base_lead > 3 * P && f.e.stats.base_lead <= 4 * P && f.ctl->lead_frames == PAGANI_AUDIO_LEAD_FRAMES);
    for (uint64_t n = 12; n < 30; ++n) pagani_audio_engine_poll(&f.e, at_frame(n * P + 77), 120 + n);
    CHECK(f.e.stats.play_resyncs == 0 && f.e.stats.drift_min >= -P && f.e.stats.drift_max <= P);
    // Stop: the path stays up 5 s (the keep time), then stops; a start within it keeps it running.
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(30 * P), 1000);
    CHECK(f.e.running && f.e.stop_pending);
    wait = pagani_audio_engine_poll(&f.e, at_frame(31 * P), 5990);
    CHECK(f.e.running && wait == 10);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(32 * P), 5995);
    CHECK(f.e.running && !f.e.stop_pending && f.e.stats.out_starts == 1);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(33 * P), 7000);
    wait = pagani_audio_engine_poll(&f.e, at_frame(34 * P), 11999);
    CHECK(f.e.running && wait == 1);
    pagani_audio_engine_poll(&f.e, at_frame(35 * P), 12000);
    CHECK(!f.e.running && !(f.ctl->state & PaganiAudioOutputRunning) && f.e.stats.out_stops == 1);
    // Taking periods while nobody wants output counts neither late nor holes (the keep time after a stop).
    CHECK(f.e.stats.late == 0);
    teardown(&f);
}

static void test_late_holes_wrap(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    PaganiAudioClientState a = {0};
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    int16_t period[P * 2];
    // Late: the plug-in wrote 10 ms ago but only up to the middle of period 50.
    const uint64_t now = at_frame(46 * P);
    plugin_write(&f, 50 * P, 50 * P + 200, 300, now - 400 * 480);
    CHECK(pagani_audio_engine_take(&f.e, 50, period, now) == PaganiAudioTakeLate && f.e.stats.late == 1 && f.ctl->out_late == 1);
    CHECK(period[0] == 300 && period[2 * 199 + 1] == 300 && period[2 * 200] == 0 && period_clear(&f, 50));
    // Not late when the plug-in's last write is older than 100 ms (it stopped writing: silence is expected).
    CHECK(pagani_audio_engine_take(&f.e, 51, period, now + 1920000) == 0 && f.e.stats.late == 1);
    // A hole: written past the period, sound, then 100 silent frames, then sound again.
    plugin_write(&f, 52 * P, 53 * P + 10, 500, now);
    for (uint64_t t = 52 * P + 100; t < 52 * P + 200; ++t) f.out[2 * (t % R)] = f.out[2 * (t % R) + 1] = 0;
    CHECK(pagani_audio_engine_take(&f.e, 52, period, now) == PaganiAudioTakeHole && f.e.stats.holes == 1 && f.ctl->out_holes == 1);
    // A period with a 63-frame gap is no hole; an all-silent written period is no hole either.
    plugin_write(&f, 53 * P, 55 * P, 500, now);
    for (uint64_t t = 53 * P + 10; t < 53 * P + 73; ++t) f.out[2 * (t % R)] = f.out[2 * (t % R) + 1] = 0;
    CHECK(pagani_audio_engine_take(&f.e, 53, period, now) == 0);
    for (uint64_t t = 54 * P; t < 55 * P; ++t) f.out[2 * (t % R)] = f.out[2 * (t % R) + 1] = 0;
    CHECK(pagani_audio_engine_take(&f.e, 54, period, now) == 0 && f.e.stats.holes == 1);
    // The plug-in's diagnostic page may hold anything: counts only, nothing breaks.
    f.ctl->out_written = ~0ULL;
    f.ctl->out_written_host = ~0ULL;
    CHECK(pagani_audio_engine_take(&f.e, 55, period, now) == 0);
    // Period 136 wraps: its first 256 frames are the ring's last ones, the other 224 its first.
    CHECK((136ULL * P) % R == R - 256);
    for (uint32_t i = 0; i < 256; ++i) f.out[2 * (R - 256 + i)] = f.out[2 * (R - 256 + i) + 1] = (int16_t)(i + 1);
    for (uint32_t i = 0; i < 224; ++i) f.out[2 * i] = f.out[2 * i + 1] = (int16_t)(-(int32_t)i - 1);
    pagani_audio_engine_take(&f.e, 136, period, now);
    CHECK(period[0] == 1 && period[2 * 255] == 256 && period[2 * 256] == -1 && period[2 * 479 + 1] == -224);
    CHECK(period_clear(&f, 136) && f.out[2 * 224] == 0 && f.e.stats.peak_out == 500);
    // -32768 is counted as the largest magnitude without overflow.
    f.out[2 * (137 * P % R)] = -32768;
    pagani_audio_engine_take(&f.e, 137, NULL, now);
    CHECK(f.e.stats.peak_out == 0x7fff);
    teardown(&f);
}

static void test_silent_capture_and_stalls(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    PaganiAudioClientState a = {0};
    for (uint32_t i = 0; i < R; ++i) f.in[i] = 0x1234;   // what an earlier run left
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartInput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(2000), 0);
    CHECK(f.e.capturing && (f.ctl->state & PaganiAudioInputRunning) && f.ctl->in_written == 2000 && f.e.cap_pos == 2000);
    CHECK(f.in[0] == 0 && f.in[R - 1] == 0);   // cleared at the start
    for (uint32_t i = 0; i < R; ++i) f.in[i] = 0x1234;
    uint32_t wait = pagani_audio_engine_poll(&f.e, at_frame(2000 + 3 * P + 7), 30);
    CHECK(f.ctl->in_written == 2000 + 3 * P && f.ctl->in_frames == 3 * P && f.e.cap_done == 3 && wait == 10);
    CHECK(f.in[2000] == 0 && f.in[2000 + 3 * P - 1] == 0 && f.in[2000 + 3 * P] == 0x1234);
    // A stall of 3 s (longer than the ring): one ring of silence, the position one ring behind the timeline, at most a ring walked.
    const uint64_t later = 2000 + 3 * P + 3 * 48000;
    pagani_audio_engine_poll(&f.e, at_frame(later), 3030);
    CHECK(f.ctl->in_written <= later && later - f.ctl->in_written < P && f.e.stats.cap_skipped > 0);
    CHECK(f.in[0] == 0 && f.in[R / 2] == 0 && f.in[R - 1] == 0);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopInput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(later + 10), 3031);
    CHECK(!f.e.capturing && !(f.ctl->state & PaganiAudioInputRunning) && f.e.stats.in_stops == 1);
    const uint64_t frozen = f.ctl->in_written;
    pagani_audio_engine_poll(&f.e, at_frame(later + 5 * P), 3100);
    CHECK(f.ctl->in_written == frozen);

    // Playback stall of 3 s: the walk is bounded to one ring, the queue ends four periods ahead again.
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(1000000), 4000);
    const uint64_t taken = f.e.stats.taken;
    pagani_audio_engine_poll(&f.e, at_frame(1000000 + 144000), 7000);
    const uint64_t play = (1000000 + 144000) / P;
    CHECK(f.e.next == play + 4 && f.e.done == play && f.e.stats.skipped > 0);
    CHECK(f.e.stats.taken - taken <= R / P + 1);
    teardown(&f);
}

static void test_taken_horizon(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    PaganiAudioClientState a = {0};
    // The audio server mixed its first cycle before the service thread ran the start: 300 ms ahead, written 1 ms ago. It stays; frames
    // the last run left behind the play position and far ahead are cleared.
    const uint64_t frame = 100000, now = at_frame(frame);
    plugin_write(&f, frame + 14400, frame + 15424, 900, now - 19200);
    f.out[2 * ((frame - 5000) % R)] = 77;
    f.out[2 * ((frame + 40000) % R)] = 77;
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    pagani_audio_engine_poll(&f.e, now, 10);
    CHECK(f.out[2 * ((frame + 14400) % R)] == 900 && f.out[2 * ((frame + 15423) % R) + 1] == 900);
    CHECK(f.out[2 * ((frame - 5000) % R)] == 0 && f.out[2 * ((frame + 40000) % R)] == 0);
    // The service publishes how far it has taken: the run's first period plus the four queued.
    CHECK(f.e.first == (frame + P - 1) / P && f.ctl->out_taken == f.e.next * P && f.e.next == frame / P + 4);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 3 * P), 40);
    CHECK(f.ctl->out_taken == (frame / P + 3 + 4) * P);
    pagani_audio_engine_take(&f.e, 500, NULL, now);
    CHECK(f.ctl->out_taken == 501ULL * P);
    // A stop and a later start without recent writes clears the whole ring.
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 4 * P), 100);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 5 * P), 5200);
    CHECK(!f.e.running);
    for (uint32_t i = 0; i < 2 * R; i += 997) f.out[i] = 5;
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 600000), 20000);
    int clear = 1;
    for (uint32_t i = 0; i < 2 * R; ++i) clear &= f.out[i] == 0;
    CHECK(clear && f.e.running && f.e.stats.out_starts == 2);
    // A plug-in position far ahead (or garbage) keeps at most the ring less one period.
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 600000 + P), 20100);
    pagani_audio_engine_poll(&f.e, at_frame(frame + 600000 + 2 * P), 25200);
    for (uint32_t i = 0; i < 2 * R; ++i) f.out[i] = 6;
    const uint64_t later = frame + 900000;
    f.ctl->out_written = ~0ULL >> 1;
    f.ctl->out_written_host = at_frame(later);
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(later), 30000);
    unsigned zeros = 0;
    for (uint32_t i = 0; i < R; ++i) zeros += f.out[2 * i] == 0;
    // One period cleared at the start (the one behind the play position), plus the three periods the start took (the run begins at
    // the next period boundary, 2084); the frames from the play position to it, and the periods after, were kept.
    const uint64_t first = (later + P - 1) / P;
    CHECK(first == 2084 && f.e.next == first + 3 && zeros == 4 * P);
    CHECK(f.out[2 * (later % R)] == 6 && f.out[2 * ((first * P) % R)] == 0 && f.out[2 * (((first + 3) * P) % R)] == 6);
    CHECK(f.out[2 * ((later - 1) % R)] == 0 && f.out[2 * ((later - P - 1) % R)] == 6);
    teardown(&f);
}

// A completion-driven backend, as a DSP is: it queues periods when asked and answers them later.
typedef struct {int opens, starts, stops, in_starts, in_stops, fail_open, fail_start; uint32_t pumps;} Mock;
static int mock_open(PaganiAudioEngine *e, void *c) {(void)e; Mock *m = c; m->opens++; return m->fail_open ? -1 : 0;}
static int mock_start(PaganiAudioEngine *e, void *c, uint64_t now) {(void)e; (void)now; Mock *m = c; m->starts++; return m->fail_start ? -1 : 0;}
static void mock_stop(PaganiAudioEngine *e, void *c) {(void)e; ((Mock *)c)->stops++;}
static int mock_in_start(PaganiAudioEngine *e, void *c, uint64_t now) {(void)e; (void)now; ((Mock *)c)->in_starts++; return 0;}
static void mock_in_stop(PaganiAudioEngine *e, void *c) {(void)e; ((Mock *)c)->in_stops++;}
static uint32_t mock_pump(PaganiAudioEngine *e, void *c, uint64_t now) {(void)e; (void)now; ((Mock *)c)->pumps++; return 50;}

static void test_backends(void)
{
    Fixture f;
    setup(&f, &pagani_audio_silent_backend, 0);
    Mock m = {0};
    PaganiAudioBackend dsp = {.name = "dsp", .id = PaganiAudioBackendAdsp, .clocked = 0, .context = &m, .open = mock_open,
                              .start_output = mock_start, .stop_output = mock_stop, .start_input = mock_in_start,
                              .stop_input = mock_in_stop, .pump = mock_pump};
    PaganiAudioClientState a = {0};
    CHECK(pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput) == 0 && pagani_audio_engine_request(&f.e, &a, PaganiAudioStartInput) == 0);
    pagani_audio_engine_poll(&f.e, at_frame(5000), 0);
    CHECK(f.e.running && f.e.capturing && f.e.stats.out_starts == 1 && f.e.stats.in_starts == 1);
    // A switch whose backend cannot open keeps the old one, restarted.
    m.fail_open = 1;
    pagani_audio_engine_set_backend(&f.e, &dsp);
    pagani_audio_engine_poll(&f.e, at_frame(6000), 10);
    CHECK(f.e.backend == &pagani_audio_silent_backend && f.e.stats.switch_failed == 1 && m.opens == 1 && f.e.running && f.e.capturing);
    CHECK(f.e.stats.out_stops == 1 && f.e.stats.out_starts == 2 && f.ctl->backend == PaganiAudioBackendSilent);
    // Now it opens: both directions stop on the silent backend and start on it in the same round.
    m.fail_open = 0;
    pagani_audio_engine_set_backend(&f.e, &dsp);
    uint32_t wait = pagani_audio_engine_poll(&f.e, at_frame(7000), 20);
    CHECK(f.e.backend == &dsp && f.ctl->backend == PaganiAudioBackendAdsp && f.e.stats.switches == 1 && m.starts == 1 && m.in_starts == 1);
    CHECK(f.e.running && f.e.capturing && wait == 50 && m.pumps == 1);
    // The DSP queues four periods and answers each; its queue settles 1800 frames ahead, so the published lead is 1800 + 960 frames,
    // under the 300 ms floor.
    // Its graphs took 13000 frames to start: it aligns to the play position, and the periods that passed meanwhile are cleared.
    CHECK(f.e.next == 15);
    plugin_write(&f, 20 * P, 21 * P, 444, at_frame(19000));
    pagani_audio_engine_align_output(&f.e, at_frame(20000));
    CHECK(f.e.next == 42 && f.e.first == 42 && f.e.done == 42 && f.ctl->out_taken == 42ULL * P);
    CHECK(f.e.stats.skipped == 27 && period_clear(&f, 20));
    uint64_t frame = 20000;
    for (int i = 0; i < 4; ++i) pagani_audio_engine_take(&f.e, f.e.next++, NULL, at_frame(frame));
    for (int i = 0; i < 40; ++i) {
        frame = f.e.next * P - 1800;   // the DSP answers when the play position is 1800 frames behind the next period to take
        pagani_audio_engine_played(&f.e, at_frame(frame));
        pagani_audio_engine_take(&f.e, f.e.next++, NULL, at_frame(frame));
    }
    CHECK(f.e.stats.base_lead == 1800 && f.ctl->lead_frames == PAGANI_AUDIO_LEAD_FRAMES && f.e.stats.play_resyncs == 0);
    // Its clock runs slow: answers come 1600 frames late, beyond three periods: the next period is set back to the lead, and the periods
    // it skips are cleared (the plug-in had written ahead into them).
    const uint64_t before = f.e.next, skipped = f.e.stats.skipped;
    frame = before * P - 1800 + 1600;
    plugin_write(&f, before * P, (before + 10) * P, 700, at_frame(frame));
    pagani_audio_engine_played(&f.e, at_frame(frame));
    CHECK(f.e.stats.play_resyncs == 1 && f.e.next == (frame + 1800 + P / 2) / P && f.e.next > before);
    CHECK(f.ctl->out_taken == f.e.next * P);
    CHECK(f.e.stats.skipped - skipped == f.e.next - before && period_clear(&f, before) && !period_clear(&f, f.e.next));
    // A large measured lead raises the published lead (lead + two periods), up to 500 ms.
    f.e.done = f.e.first;
    f.e.next = f.e.first + 40;
    for (int i = 0; i < 8; ++i) pagani_audio_engine_played(&f.e, at_frame(f.e.next * P - 16000));
    CHECK(f.e.stats.base_lead == 16000 && f.ctl->lead_frames == 16960);
    // Capture: the DSP returns a short buffer from an offset; only its frames go to the ring, at the capture position.
    int16_t samples[P];
    for (uint32_t i = 0; i < P; ++i) samples[i] = (int16_t)(i + 1);
    const uint64_t pos = f.e.cap_pos;
    pagani_audio_engine_captured(&f.e, samples, P - 2, at_frame(pos + P));
    CHECK(f.e.cap_pos == pos + P - 2 && f.ctl->in_written == pos + P - 2 && f.e.stats.cap_short == 1);
    CHECK(f.in[pos % R] == 1 && f.in[(pos + P - 3) % R] == P - 2 && f.e.stats.peak_in == P - 2);
    // Its capture settles 700 frames behind the play position; drifting four periods behind, the position moves on and the frames
    // it skips read as silence.
    for (int i = 0; i < 9; ++i) pagani_audio_engine_captured(&f.e, samples, P, at_frame(f.e.cap_pos + 700));
    const uint64_t cap = f.e.cap_pos;
    for (uint64_t t = cap; t < cap + 5 * P; ++t) f.in[t % R] = 0x2222;
    pagani_audio_engine_captured(&f.e, samples, P, at_frame(cap + 700 + 4 * P));
    CHECK(f.e.stats.cap_resyncs == 1 && f.e.cap_pos == cap + 4 * P + P && f.in[cap % R] == 0 && f.in[(cap + 4 * P - 1) % R] == 0);
    CHECK(f.in[(cap + 4 * P) % R] == 1);
    // A backend that refuses to start is not asked again within a second.
    pagani_audio_engine_request(&f.e, &a, PaganiAudioStopOutput);
    pagani_audio_engine_poll(&f.e, at_frame(900000), 100);
    pagani_audio_engine_poll(&f.e, at_frame(900100), 5200);
    CHECK(!f.e.running && m.stops == 1);
    m.fail_start = 1;
    pagani_audio_engine_request(&f.e, &a, PaganiAudioStartOutput);
    wait = pagani_audio_engine_poll(&f.e, at_frame(900200), 6000);
    CHECK(!f.e.running && f.e.stats.out_refused == 1 && m.starts == 2 && !(f.ctl->state & PaganiAudioOutputRunning));
    pagani_audio_engine_poll(&f.e, at_frame(900300), 6500);
    CHECK(m.starts == 2);
    m.fail_start = 0;
    pagani_audio_engine_poll(&f.e, at_frame(900400), 7000);
    CHECK(f.e.running && m.starts == 3 && f.e.out_refused_ms == 0);
    // Answers that arrive after a stop change nothing that was published as running.
    pagani_audio_engine_request(&f.e, &a, PaganiAudioStopInput);
    pagani_audio_engine_poll(&f.e, at_frame(900500), 7100);
    const uint64_t frozen = f.ctl->in_written;
    pagani_audio_engine_captured(&f.e, samples, P, at_frame(900600));
    CHECK(!f.e.capturing && f.ctl->in_written == frozen && m.in_stops == 1 && m.in_starts == 1);
    teardown(&f);
}

int main(void)
{
    test_timeline();
    test_requests();
    test_silent_output();
    test_late_holes_wrap();
    test_silent_capture_and_stalls();
    test_taken_horizon();
    test_backends();
    printf("PASS engine: %u checks\n", checks);
    return 0;
}
