/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The PaganiAudio ring and timeline engine; see PaganiAudioEngine.h. The playback period handling, the lead measurement and the drift
 * resyncs are ported from UniXNU-sheng package/AudioProbe.cpp (revision 08cde20); the silent backend and the per-client requests are
 * new. Freestanding: no headers beyond the engine's own, so the kernel and the host tests compile this one file. */
#include "PaganiAudioEngine.h"

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define INT32_LARGEST 0x7fffffff
#define INT32_SMALLEST (-0x7fffffff - 1)

enum {
    kInFlight = 4,                  /* periods a clocked backend keeps ahead of the play position (as many as sheng keeps at the DSP) */
    kKeepMs = 5000,                 /* output stays up this long after the last client stopped it (an amplifier's power-up cuts sound) */
    kRetryMs = 1000,                /* a direction the backend refused is not asked again sooner */
    kLeadMost = 24000,              /* the largest lead the service publishes (500 ms) */
    kIdleWaitMs = 1000,
};

static uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void set_state(PaganiAudioEngine *e, uint32_t bits, int on)
{
    if (on) __atomic_or_fetch(&e->ctl->state, bits, __ATOMIC_ACQ_REL);
    else __atomic_and_fetch(&e->ctl->state, ~bits, __ATOMIC_ACQ_REL);
}

int pagani_audio_engine_init(PaganiAudioEngine *e, void *control, int16_t *out, int16_t *in, uint64_t now, uint32_t numer,
                             uint32_t denom, const PaganiAudioBackend *backend, uint32_t options)
{
    if (!e || !control || !out || !in || !numer || !denom || !backend) return -1;
    /* One frame lasts 1e9 / rate ns, that is (1e9 * denom) / (rate * numer) ticks: 400 / 1 with the 19.2 MHz timer (625 / 12),
     * 500 / 1 with Apple's 24 MHz one (125 / 3). Both terms fit 64 bits for any 32-bit timebase. */
    uint64_t num = 1000000000ULL * denom, den = (uint64_t)PAGANI_AUDIO_RATE * numer;
    const uint64_t g = gcd64(num, den);
    num /= g;
    den /= g;
    if (!num || num > 0xffffffffULL || den > 0xffffffffULL) return -1;
    __builtin_memset(e, 0, sizeof *e);
    e->ctl = (PaganiAudioControl *)control;
    e->out = out;
    e->in = in;
    e->anchor = now;
    e->tick_num = num;
    e->tick_den = den;
    e->ticks_100ms = 100000000ULL * denom / numer;
    e->period = PAGANI_AUDIO_PERIOD_FRAMES;
    e->ring = PAGANI_AUDIO_RING_FRAMES;
    e->out_channels = PAGANI_AUDIO_OUT_CHANNELS;
    e->in_channels = PAGANI_AUDIO_IN_CHANNELS;
    e->in_flight = kInFlight;
    e->keep_ms = kKeepMs;
    e->lead = PAGANI_AUDIO_LEAD_FRAMES;
    e->backend = backend;
    e->stats.drift_min = INT32_LARGEST;
    e->stats.drift_max = INT32_SMALLEST;
    __builtin_memset(control, 0, PAGANI_AUDIO_CONTROL_BYTES);
    __builtin_memset(out, 0, (uint64_t)PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS * sizeof(int16_t));
    __builtin_memset(in, 0, (uint64_t)PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS * sizeof(int16_t));
    PaganiAudioControl *c = e->ctl;
    c->rate = PAGANI_AUDIO_RATE;
    c->out_channels = PAGANI_AUDIO_OUT_CHANNELS;
    c->in_channels = PAGANI_AUDIO_IN_CHANNELS;
    c->period_frames = PAGANI_AUDIO_PERIOD_FRAMES;
    c->ring_frames = PAGANI_AUDIO_RING_FRAMES;
    c->lead_frames = e->lead;
    c->ticks_per_frame_q32 = (num << 32) / den;
    c->out_anchor_host = now;
    c->in_anchor_host = now;
    c->backend = backend->id;
    c->options = options;
    c->version = PAGANI_AUDIO_VERSION;
    __atomic_store_n(&c->magic, PAGANI_AUDIO_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

uint64_t pagani_audio_engine_frame_at(const PaganiAudioEngine *e, uint64_t now)
{
    if (now <= e->anchor) return 0;
    const uint64_t dt = now - e->anchor;
    return dt / e->tick_num * e->tick_den + dt % e->tick_num * e->tick_den / e->tick_num;
}

uint64_t pagani_audio_engine_host_at(const PaganiAudioEngine *e, uint64_t frame)
{
    return e->anchor + frame / e->tick_den * e->tick_num + frame % e->tick_den * e->tick_num / e->tick_den;
}

int pagani_audio_engine_request(PaganiAudioEngine *e, PaganiAudioClientState *client, uint32_t selector)
{
    uint32_t bit;
    int start;
    switch (selector) {
    case PaganiAudioStartOutput: bit = PaganiAudioOutputRunning; start = 1; break;
    case PaganiAudioStopOutput: bit = PaganiAudioOutputRunning; start = 0; break;
    case PaganiAudioStartInput: bit = PaganiAudioInputRunning; start = 1; break;
    case PaganiAudioStopInput: bit = PaganiAudioInputRunning; start = 0; break;
    case PaganiAudioStartActuator: bit = PaganiAudioActuatorRunning; start = 1; break;
    case PaganiAudioStopActuator: bit = PaganiAudioActuatorRunning; start = 0; break;
    default:
        __atomic_add_fetch(&e->stats.refused_requests, 1, __ATOMIC_RELAXED);
        return -1;
    }
    volatile uint32_t *users = bit == PaganiAudioOutputRunning ? &e->out_users : bit == PaganiAudioInputRunning ? &e->in_users : &e->act_users;
    /* Each client counts once per direction, however often it asks (a client's threads may ask at once). The bit changes in one
     * compare-and-swap with the released flag beside it: a request either lands before the release (which then takes its count back)
     * or sees the flag and is refused, so a direction cannot stay counted after its client closed. */
    uint32_t bits = __atomic_load_n(&client->bits, __ATOMIC_ACQUIRE), wanted;
    do {
        if (bits & PaganiAudioClientReleased) {
            __atomic_add_fetch(&e->stats.refused_requests, 1, __ATOMIC_RELAXED);
            return -1;
        }
        wanted = start ? bits | bit : bits & ~bit;
    } while (wanted != bits && !__atomic_compare_exchange_n(&client->bits, &bits, wanted, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    __atomic_add_fetch(&e->stats.requests, 1, __ATOMIC_RELAXED);
    if (wanted != bits) {
        if (start) __atomic_add_fetch(users, 1, __ATOMIC_ACQ_REL);
        else __atomic_sub_fetch(users, 1, __ATOMIC_ACQ_REL);
    }
    return 0;
}

void pagani_audio_engine_release(PaganiAudioEngine *e, PaganiAudioClientState *client)
{
    const uint32_t held = __atomic_exchange_n(&client->bits, PaganiAudioClientReleased, __ATOMIC_ACQ_REL);
    if (held & PaganiAudioClientReleased) return;
    if (held & PaganiAudioOutputRunning) __atomic_sub_fetch(&e->out_users, 1, __ATOMIC_ACQ_REL);
    if (held & PaganiAudioInputRunning) __atomic_sub_fetch(&e->in_users, 1, __ATOMIC_ACQ_REL);
    if (held & PaganiAudioActuatorRunning) __atomic_sub_fetch(&e->act_users, 1, __ATOMIC_ACQ_REL);
}

void pagani_audio_engine_set_backend(PaganiAudioEngine *e, const PaganiAudioBackend *backend)
{
    __atomic_store_n(&e->pending, backend, __ATOMIC_RELEASE);
}

/* Clears playback periods [from, to) in the ring without taking them; at most one ring's worth. */
static void clear_periods(PaganiAudioEngine *e, uint64_t from, uint64_t to)
{
    const uint64_t ring_periods = e->ring / e->period + 1;
    if (to <= from) return;
    if (to - from > ring_periods) from = to - ring_periods;
    for (uint64_t n = from; n < to; ++n) {
        const uint32_t at = (uint32_t)(n * e->period % e->ring), head = e->ring - at < e->period ? e->ring - at : e->period;
        __builtin_memset(e->out + (uint64_t)at * e->out_channels, 0, (uint64_t)head * e->out_channels * sizeof(int16_t));
        if (head < e->period) __builtin_memset(e->out, 0, (uint64_t)(e->period - head) * e->out_channels * sizeof(int16_t));
    }
}

/* The backend plays the first period at once, so it is the period of the timeline that plays now (rounded up: a period whose start
 * already passed would be taken late); the ones queued behind it follow in order. What lies before it counts as taken. */
static void align(PaganiAudioEngine *e, uint64_t now)
{
    e->next = e->done = e->first = (pagani_audio_engine_frame_at(e, now) + e->period - 1) / e->period;
    e->ctl->out_taken = e->next * e->period;
}

void pagani_audio_engine_align_output(PaganiAudioEngine *e, uint64_t now)
{
    /* A backend whose start took time: the periods that passed meanwhile are cleared (the plug-in may have mixed into them), as a
     * resync skips them. */
    const uint64_t before = e->next;
    align(e, now);
    if (e->next > before) {
        clear_periods(e, before, e->next);
        e->stats.skipped += (uint32_t)(e->next - before);
    }
}

/* Clears the playback ring except timeline frames [from, to) (at most the ring less one period). */
static void clear_ring_except(PaganiAudioEngine *e, uint64_t from, uint64_t to)
{
    const uint64_t ring = e->ring, channels = e->out_channels;
    if (to > from + ring - e->period) to = from + ring - e->period;
    const uint64_t start = to % ring, count = to > from ? ring - (to - from) : ring, first = ring - start < count ? ring - start : count;
    __builtin_memset(e->out + start * channels, 0, first * channels * sizeof(int16_t));
    if (count > first) __builtin_memset(e->out, 0, (count - first) * channels * sizeof(int16_t));
}

static void start_output(PaganiAudioEngine *e, uint64_t now, uint64_t now_ms)
{
    if (e->out_refused_ms && now_ms - e->out_refused_ms < kRetryMs) return;
    /* The plug-in adds each IO context's mix to the ring, so frames left from the last run must not be in it; but the audio server
     * starts mixing as soon as StartIO returns, before this thread runs: what the plug-in wrote ahead of the play position in the last
     * 100 ms belongs to this run and stays. (After a run every frame the service passed is clear: it takes each period, and the
     * plug-in drops what comes too late.) */
    const uint64_t frame = pagani_audio_engine_frame_at(e, now), written = e->ctl->out_written;
    if (now - e->ctl->out_written_host < e->ticks_100ms && written > frame) clear_ring_except(e, frame, written);
    else clear_ring_except(e, 0, 0);
    align(e, now);
    e->base_lead = 0;
    e->ctl->out_frames = 0;
    const PaganiAudioBackend *b = e->backend;
    if (b->start_output && b->start_output(e, b->context, now)) {
        e->stats.out_refused++;
        e->out_refused_ms = now_ms ? now_ms : 1;
        return;
    }
    e->out_refused_ms = 0;
    e->running = 1;
    e->stop_pending = 0;
    set_state(e, PaganiAudioOutputRunning, 1);
    e->stats.out_starts++;
}

static void stop_output(PaganiAudioEngine *e)
{
    if (!e->running) return;
    e->running = 0;
    e->stop_pending = 0;
    const PaganiAudioBackend *b = e->backend;
    if (b->stop_output) b->stop_output(e, b->context);
    set_state(e, PaganiAudioOutputRunning, 0);
    e->stats.out_stops++;
}

static void start_input(PaganiAudioEngine *e, uint64_t now, uint64_t now_ms)
{
    if (e->in_refused_ms && now_ms - e->in_refused_ms < kRetryMs) return;
    __builtin_memset(e->in, 0, (uint64_t)e->ring * e->in_channels * sizeof(int16_t));
    e->cap_pos = pagani_audio_engine_frame_at(e, now);
    e->cap_done = 0;
    e->cap_frames = 0;
    e->base_capture = 0;
    e->ctl->in_written = e->cap_pos;
    e->ctl->in_frames = 0;
    const PaganiAudioBackend *b = e->backend;
    if (b->start_input && b->start_input(e, b->context, now)) {
        e->stats.in_refused++;
        e->in_refused_ms = now_ms ? now_ms : 1;
        return;
    }
    e->in_refused_ms = 0;
    e->capturing = 1;
    set_state(e, PaganiAudioInputRunning, 1);
    e->stats.in_starts++;
}

static void stop_input(PaganiAudioEngine *e)
{
    if (!e->capturing) return;
    e->capturing = 0;
    const PaganiAudioBackend *b = e->backend;
    if (b->stop_input) b->stop_input(e, b->context);
    set_state(e, PaganiAudioInputRunning, 0);
    e->stats.in_stops++;
}

/* The running directions stop on the old backend and start again on the new one in the same poll. A backend that cannot open is not
 * taken: the old one goes on. */
static void switch_backend(PaganiAudioEngine *e, const PaganiAudioBackend *b)
{
    stop_output(e);
    stop_input(e);
    if (b->open && b->open(e, b->context)) {
        e->stats.switch_failed++;
        return;
    }
    e->backend = b;
    e->ctl->backend = b->id;
    e->stats.switches++;
    e->out_refused_ms = e->in_refused_ms = 0;
}

uint32_t pagani_audio_engine_poll(PaganiAudioEngine *e, uint64_t now, uint64_t now_ms)
{
    const PaganiAudioBackend *b = __atomic_exchange_n(&e->pending, (const PaganiAudioBackend *)0, __ATOMIC_ACQ_REL);
    if (b && b != e->backend) switch_backend(e, b);
    const int want_out = LOAD(&e->out_users) != 0, want_in = LOAD(&e->in_users) != 0;
    if (want_out) {
        e->stop_pending = 0;
        if (!e->running) start_output(e, now, now_ms);
    } else if (e->running && !e->stop_pending) {
        e->stop_pending = 1;
        e->stop_asked_ms = now_ms;
    } else if (e->running && now_ms - e->stop_asked_ms >= e->keep_ms) {
        stop_output(e);
    }
    if (want_in && !e->capturing) start_input(e, now, now_ms);
    else if (!want_in && e->capturing) stop_input(e);
    uint32_t wait = e->backend->pump ? e->backend->pump(e, e->backend->context, now) : kIdleWaitMs;
    if (e->running && e->stop_pending) {
        const uint64_t passed = now_ms - e->stop_asked_ms, left = passed < e->keep_ms ? e->keep_ms - passed : 0;
        if (left < wait) wait = (uint32_t)left;
    }
    if ((want_out && !e->running) || (want_in && !e->capturing)) wait = wait < kRetryMs ? wait : kRetryMs;
    return wait ? wait : 1;
}

uint32_t pagani_audio_engine_take(PaganiAudioEngine *e, uint64_t n, int16_t *dst, uint64_t now)
{
    const uint32_t frames = e->period, channels = e->out_channels;
    const uint32_t at = (uint32_t)(n * frames % e->ring), head = e->ring - at < frames ? e->ring - at : frames;
    int16_t *const first = e->out + (uint64_t)at * channels;
    if (dst) {
        __builtin_memcpy(dst, first, (uint64_t)head * channels * sizeof(int16_t));
        if (head < frames) __builtin_memcpy(dst + (uint64_t)head * channels, e->out, (uint64_t)(frames - head) * channels * sizeof(int16_t));
    }
    /* The period's longest silent run, its sounding frames and its peak, before it is cleared. */
    uint32_t run = 0, longest = 0, loud = 0, peak = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t *frame = i < head ? first + (uint64_t)i * channels : e->out + (uint64_t)(i - head) * channels;
        int quiet = 1;
        for (uint32_t c = 0; c < channels; ++c) {
            const int32_t v = frame[c] < 0 ? -(int32_t)frame[c] : frame[c];
            if (v) quiet = 0;
            if ((uint32_t)v > peak) peak = (uint32_t)v;
        }
        run = quiet ? run + 1 : 0;
        if (run > longest) longest = run;
        if (!quiet) loud++;
    }
    /* Late: a client runs and the plug-in wrote recently but not up to this period's end (the keep time after a stop does not count).
     * A hole: the plug-in wrote past it, yet it holds 64 silent frames in a row between sound. Both positions come from the plug-in and
     * are only counted. */
    uint32_t result = 0;
    if (LOAD(&e->out_users)) {
        const uint64_t written = e->ctl->out_written, written_host = e->ctl->out_written_host, end = (n + 1) * frames;
        if (now - written_host < e->ticks_100ms && written < end) result = PaganiAudioTakeLate;
        else if (written >= end && longest >= 64 && loud) result = PaganiAudioTakeHole;
    }
    if (result & PaganiAudioTakeLate) e->ctl->out_late = ++e->stats.late;
    if (result & PaganiAudioTakeHole) e->ctl->out_holes = ++e->stats.holes;
    if (peak > 0x7fff) peak = 0x7fff;
    if (peak > e->stats.peak_out) e->stats.peak_out = peak;
    __builtin_memset(first, 0, (uint64_t)head * channels * sizeof(int16_t));
    if (head < frames) __builtin_memset(e->out, 0, (uint64_t)(frames - head) * channels * sizeof(int16_t));
    __atomic_store_n(&e->ctl->out_taken, (n + 1) * frames, __ATOMIC_RELEASE);
    e->stats.taken++;
    return result;
}

/* sheng's syncPlayback: the backend takes buffers into a queue of its own, so a period is taken about base_lead frames before it plays.
 * The audio server must have written it by then: the published lead (the plug-in's latency and safety offset follow it) is that plus two
 * periods, at least PAGANI_AUDIO_LEAD_FRAMES. An unclocked backend whose clock drifts three periods from the host's is set back to it;
 * the periods that skips are cleared, so the plug-in never mixes onto what they held a ring turn earlier. */
static void sync_playback(PaganiAudioEngine *e, uint64_t now)
{
    const uint64_t frame = pagani_audio_engine_frame_at(e, now), played = e->done - e->first;
    const int64_t lead = (int64_t)(e->next * e->period) - (int64_t)frame;
    const int64_t limit = 3 * (int64_t)e->period;
    if (played < 2 * (uint64_t)e->in_flight) return;
    if (played == 2 * (uint64_t)e->in_flight) {
        e->base_lead = lead;
        e->stats.base_lead = (int32_t)lead;
        const int64_t want = lead + 2 * (int64_t)e->period;
        e->lead = (uint32_t)(want < PAGANI_AUDIO_LEAD_FRAMES ? PAGANI_AUDIO_LEAD_FRAMES : want > kLeadMost ? kLeadMost : want);
        e->ctl->lead_frames = e->lead;
        return;
    }
    const int64_t drift = lead - e->base_lead;
    const int32_t d = drift > INT32_LARGEST ? INT32_LARGEST : drift < INT32_SMALLEST ? INT32_SMALLEST : (int32_t)drift;
    if (d < e->stats.drift_min) e->stats.drift_min = d;
    if (d > e->stats.drift_max) e->stats.drift_max = d;
    if (e->backend->clocked || (drift <= limit && drift >= -limit)) return;
    const int64_t target = ((int64_t)frame + e->base_lead + (int64_t)e->period / 2) / (int64_t)e->period;
    const uint64_t to = target > 0 ? (uint64_t)target : 0;
    if (to > e->next) {
        clear_periods(e, e->next, to);
        e->stats.skipped += (uint32_t)(to - e->next);
    }
    e->next = to;
    __atomic_store_n(&e->ctl->out_taken, to * e->period, __ATOMIC_RELEASE);
    e->stats.play_resyncs++;
}

void pagani_audio_engine_played(PaganiAudioEngine *e, uint64_t now)
{
    e->done++;
    e->stats.played++;
    e->ctl->out_frames = (e->done - e->first) * e->period;
    if (e->running) sync_playback(e, now);
}

/* sheng's syncCapture: the distance between the capture position and the play position settles once per run; the position follows when
 * it drifts three periods from that. Frames a forward move skips are cleared: the plug-in reads every frame before in_written. */
static void sync_capture(PaganiAudioEngine *e, uint64_t now)
{
    const int64_t gap = (int64_t)pagani_audio_engine_frame_at(e, now) - (int64_t)e->cap_pos;
    const int64_t limit = 3 * (int64_t)e->period;
    if (e->cap_done < 2 * (uint64_t)e->in_flight) return;
    if (e->cap_done == 2 * (uint64_t)e->in_flight) {
        e->base_capture = gap;
        return;
    }
    const int64_t shift = gap - e->base_capture;
    if (shift <= limit && shift >= -limit) return;
    if (shift > 0) {
        const uint64_t count = (uint64_t)shift < e->ring ? (uint64_t)shift : e->ring;
        for (uint64_t i = 0; i < count; ++i)
            for (uint32_t c = 0; c < e->in_channels; ++c) e->in[(e->cap_pos + i) % e->ring * e->in_channels + c] = 0;
        e->stats.cap_skipped += (uint32_t)count;
    }
    e->cap_pos = (uint64_t)((int64_t)e->cap_pos + shift);
    e->stats.cap_resyncs++;
}

void pagani_audio_engine_captured(PaganiAudioEngine *e, const int16_t *src, uint32_t count, uint64_t now)
{
    if (!e->capturing) return;
    if (count > e->ring) count = e->ring;
    if (!e->backend->clocked) sync_capture(e, now);
    const uint64_t t = e->cap_pos;
    uint32_t peak = 0;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t c = 0; c < e->in_channels; ++c) {
            const int16_t v = src ? src[(uint64_t)i * e->in_channels + c] : 0;
            e->in[(t + i) % e->ring * e->in_channels + c] = v;
            const uint32_t m = (uint32_t)(v < 0 ? -(int32_t)v : v);
            if (m > peak) peak = m;
        }
    }
    if (peak > 0x7fff) peak = 0x7fff;
    if (peak > e->stats.peak_in) e->stats.peak_in = peak;
    e->cap_pos = t + count;
    e->cap_frames += count;
    e->stats.captured += count;
    if (count < e->period) e->stats.cap_short++;
    e->cap_done++;
    e->ctl->in_frames = e->cap_frames;
    __atomic_store_n(&e->ctl->in_written, e->cap_pos, __ATOMIC_RELEASE);
}

/* ---- The silent backend ------------------------------------------------------------------------------------------------------- */

/* Milliseconds until frames more frames have played, rounded up, 1 to kIdleWaitMs. */
static uint32_t ms_for(uint64_t frames)
{
    const uint64_t ms = (frames * 1000 + PAGANI_AUDIO_RATE - 1) / PAGANI_AUDIO_RATE;
    return ms ? (uint32_t)(ms < kIdleWaitMs ? ms : kIdleWaitMs) : 1;
}

/* Playback: the periods up to in_flight ahead of the play position are taken (cleared, counted) as a DSP would queue them, and each has
 * played once its start passed. Capture: every period the timeline completed is delivered as silence. After a stall longer than the ring
 * only the last ring's worth is walked: older periods alias the same frames. */
static uint32_t silent_pump(PaganiAudioEngine *e, void *context, uint64_t now)
{
    (void)context;
    const uint64_t frame = pagani_audio_engine_frame_at(e, now), period = e->period, ring_periods = e->ring / period;
    uint32_t wait = kIdleWaitMs;
    if (e->running) {
        const uint64_t play = frame / period, until = play + e->in_flight;
        if (until > e->next + ring_periods) {
            /* Skipped, not played: the played count goes on from the first period walked again. */
            e->stats.skipped += (uint32_t)(until - ring_periods - e->next);
            e->next = until - ring_periods;
            if (e->done < e->next) e->done = e->next;
        }
        while (e->next < until) {
            pagani_audio_engine_take(e, e->next, (int16_t *)0, now);
            e->next++;
        }
        while (e->done < e->next && e->done < play) pagani_audio_engine_played(e, now);
        const uint32_t w = ms_for((play + 1) * period - frame);
        if (w < wait) wait = w;
    }
    if (e->capturing) {
        if (frame > e->cap_pos + e->ring) {
            /* The ring is silence anyway; the position moves on to one ring behind the timeline. */
            __builtin_memset(e->in, 0, (uint64_t)e->ring * e->in_channels * sizeof(int16_t));
            const uint64_t jump = (frame - e->cap_pos) / period * period - (ring_periods - 1) * period;
            e->stats.cap_skipped += (uint32_t)jump;
            e->cap_pos += jump;
        }
        while (e->cap_pos + period <= frame) pagani_audio_engine_captured(e, (const int16_t *)0, (uint32_t)period, now);
        const uint32_t w = ms_for(e->cap_pos + period - frame);
        if (w < wait) wait = w;
    }
    return wait;
}

const PaganiAudioBackend pagani_audio_silent_backend = {
    .name = "silent",
    .id = PaganiAudioBackendSilent,
    .clocked = 1,
    .pump = silent_pump,
};
