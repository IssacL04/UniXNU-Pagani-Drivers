/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The haptic actuator bridge; see PaganiAudioHaptics.h. Freestanding: no headers beyond the service's own, so the kernel and the host
 * tests compile this one file. Integer arithmetic only (the kernel builds with -mgeneral-regs-only). */
#include "PaganiAudioHaptics.h"

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)

enum {kIdleWaitMs = 1000};

static uint32_t clamp_rate(uint32_t rate)
{
    return rate == 16000 || rate == 24000 || rate == 48000 ? rate : PAGANI_AUDIO_HAPTICS_RATE;
}

int pagani_audio_haptics_init(PaganiAudioHaptics *h, const PaganiAudioEngine *e, void *memory, const PaganiAudioHapticsOps *ops,
                              uint32_t rate, uint32_t full_mv, uint32_t sustain_mv)
{
    if (!h || !e || !e->ctl || !e->tick_num || !e->tick_den || !memory || !ops || !ops->begin || !ops->write || !ops->end) return -1;
    __builtin_memset(h, 0, sizeof *h);
    h->ctl = (PaganiAudioActuatorControl *)memory;
    h->ring = (int16_t *)((uint8_t *)memory + PAGANI_AUDIO_ACT_HEADER_BYTES);
    h->ring_frames = PAGANI_AUDIO_ACT_RING_FRAMES;
    h->take_frames = PAGANI_AUDIO_ACT_TAKE_FRAMES;
    h->rate = clamp_rate(rate);
    h->decimate = PAGANI_AUDIO_ACT_RATE / h->rate;
    h->full_mv = full_mv && full_mv < PAGANI_AUDIO_HAPTICS_FULL_MV ? full_mv : PAGANI_AUDIO_HAPTICS_FULL_MV;
    h->cap_mv = PAGANI_AUDIO_HAPTICS_CAP_MV;
    if (!sustain_mv) sustain_mv = PAGANI_AUDIO_HAPTICS_SUSTAIN_MV;
    if (sustain_mv < PAGANI_AUDIO_HAPTICS_MIN_MV) sustain_mv = PAGANI_AUDIO_HAPTICS_MIN_MV;
    h->sustain_mv = sustain_mv < h->full_mv ? sustain_mv : h->full_mv;
    h->sustain_q = 127;
    h->credit_max = h->rate / 1000 * PAGANI_AUDIO_HAPTICS_BURST_MS * PAGANI_AUDIO_HAPTICS_RECOVERY;
    h->silence_samples = h->rate / 1000 * PAGANI_AUDIO_HAPTICS_SILENCE_MS;
    h->stream_samples = h->rate / 1000 * PAGANI_AUDIO_HAPTICS_STREAM_MS;
    h->ops = *ops;
    h->stats.latency_us = -1;
    __builtin_memset(memory, 0, PAGANI_AUDIO_ACT_BYTES);
    PaganiAudioActuatorControl *c = h->ctl;
    c->version = PAGANI_AUDIO_ACT_VERSION;
    c->rate = PAGANI_AUDIO_ACT_RATE;
    c->ring_frames = PAGANI_AUDIO_ACT_RING_FRAMES;
    c->io_frames = PAGANI_AUDIO_ACT_IO_FRAMES;
    c->safety_frames = PAGANI_AUDIO_ACT_SAFETY_FRAMES;
    c->take_frames = PAGANI_AUDIO_ACT_TAKE_FRAMES;
    c->ticks_per_frame_q32 = e->ctl->ticks_per_frame_q32;
    c->anchor_host = e->anchor;
    __atomic_store_n(&c->magic, PAGANI_AUDIO_ACT_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

int8_t pagani_audio_haptics_scale(int32_t sum, uint32_t count, uint32_t full_mv, uint32_t cap_mv, int *clipped)
{
    *clipped = 0;
    if (!count || !cap_mv) return 0;
    const int64_t num = (int64_t)sum * (int64_t)full_mv * 127, den = (int64_t)32767 * cap_mv * count;
    int64_t q = (num >= 0 ? num + den / 2 : num - den / 2) / den;
    if (q > 127) {q = 127; *clipped = 1;}
    if (q < -127) {q = -127; *clipped = 1;}
    return (int8_t)q;
}

/* Host ticks to microseconds on the engine's timeline (one frame = tick_num / tick_den ticks = 1e6 / 48000 us); ticks below a second. */
static int32_t ticks_us(const PaganiAudioEngine *e, uint64_t ticks)
{
    return (int32_t)(ticks * e->tick_den * 125 / (e->tick_num * 6));
}

static void flush(PaganiAudioHaptics *h)
{
    if (!h->pending) return;
    const int taken = h->ops.write(h->ops.context, h->out, h->pending);
    if (taken < 0) {
        h->stats.last_rc = taken;
        h->stats.refused += h->pending;
    } else {
        h->stats.samples += (uint32_t)taken;
        if ((uint32_t)taken < h->pending) h->stats.refused += h->pending - (uint32_t)taken;
    }
    h->pending = 0;
}

static void end_stream(PaganiAudioHaptics *h, int rollover)
{
    if (!h->streaming) return;
    flush(h);
    const int rc = h->ops.end(h->ops.context, 1);
    if (rc < 0) h->stats.last_rc = rc;
    h->streaming = 0;
    h->stats.stream_ends++;
    if (rollover) h->stats.rollovers++;
}

/* A new stream at the first loud sample: the driver's runtime cap (at most the full scale) becomes the stream's VMAX. */
static int begin_stream(PaganiAudioHaptics *h, const PaganiAudioEngine *e, uint64_t now)
{
    if (h->retry_host && now < h->retry_host) return 0;
    uint32_t cap = h->ops.cap ? h->ops.cap(h->ops.context) : 0;
    if (!cap) cap = PAGANI_AUDIO_HAPTICS_CAP_MV;
    if (cap > h->full_mv) cap = h->full_mv;
    if (cap < 50) cap = 50;
    const int rc = h->ops.begin(h->ops.context, h->rate, cap);
    if (rc) {
        h->stats.begin_failed++;
        h->stats.last_rc = rc;
        h->retry_host = now + e->ticks_100ms * (PAGANI_AUDIO_HAPTICS_RETRY_MS / 100);
        return 0;
    }
    h->retry_host = 0;
    h->cap_mv = cap;
    const uint32_t q = 127 * h->sustain_mv / cap;
    h->sustain_q = q > 127 ? 127 : q ? q : 1;
    h->streaming = 1;
    h->quiet = 0;
    h->length = 0;
    h->stats.streams++;
    h->stats.cap_mv = cap;
    h->stats.full_mv = h->full_mv;
    h->stats.rate = h->rate;
    /* From the plug-in's write of the first loud frame (it records the write's host time) to this begin. */
    const uint64_t sound = LOAD(&h->ctl->sound_host);
    const uint64_t second = e->ticks_100ms * 10;
    h->stats.latency_us = sound && sound <= now && now - sound < second ? ticks_us(e, now - sound) : -1;
    if (h->stats.latency_us > h->stats.latency_max_us) h->stats.latency_max_us = h->stats.latency_us;
    return 1;
}

/* Sustained drive (PaganiAudioHaptics.h): a sample above the sustained level uses PAGANI_AUDIO_HAPTICS_RECOVERY of the credit, or is clipped
 * to that level when it is used up; any other sample gives one back. */
static int8_t limit(PaganiAudioHaptics *h, int8_t v)
{
    const uint32_t m = (uint32_t)(v < 0 ? -v : v);
    if (m <= h->sustain_q) {
        if (h->credit) h->credit--;
        return v;
    }
    if (h->credit + PAGANI_AUDIO_HAPTICS_RECOVERY > h->credit_max) {
        h->stats.limited++;
        return (int8_t)(v < 0 ? -(int32_t)h->sustain_q : (int32_t)h->sustain_q);
    }
    h->credit += PAGANI_AUDIO_HAPTICS_RECOVERY;
    return v;
}

/* One FIFO sample of a running stream: silence is carried until it lasts silence_samples, then the stream ends (as at its length limit). */
static void emit(PaganiAudioHaptics *h, int8_t v)
{
    h->out[h->pending++] = v;
    if (h->pending == PAGANI_AUDIO_HAPTICS_CHUNK) flush(h);
    const uint32_t m = (uint32_t)(v < 0 ? -v : v);
    if (m > h->stats.peak_out) h->stats.peak_out = m;
    h->quiet = v ? 0 : h->quiet + 1;
    h->length++;
    if (h->quiet >= h->silence_samples) end_stream(h, 0);
    else if (h->length >= h->stream_samples) end_stream(h, 1);
}

static void take(PaganiAudioHaptics *h, const PaganiAudioEngine *e, uint64_t until, uint64_t now)
{
    const uint64_t mask = h->ring_frames - 1;
    if (until > h->taken + h->ring_frames) {
        /* A stall longer than the ring: older frames alias the same slots; they are cleared below as the walk passes them. */
        h->stats.skipped += until - h->ring_frames - h->taken;
        h->taken = until - h->ring_frames;
    }
    for (; h->taken < until; h->taken++) {
        int16_t *slot = &h->ring[h->taken & mask];
        const int16_t s = *slot;
        *slot = 0;
        const uint32_t m = (uint32_t)(s < 0 ? -(int32_t)s : s);
        if (m > h->stats.peak_in) h->stats.peak_in = m > 32767 ? 32767 : m;
        if (s) h->stats.loud_frames++;
        h->stats.frames++;
        h->sum += s;
        if (++h->summed < h->decimate) continue;
        /* Outside a stream nothing is sent while silent (by the last stream's cap, at first the driver's default); a loud sample
         * begins a stream, and is scaled again with the cap that stream runs at. */
        int clipped = 0;
        int8_t v = pagani_audio_haptics_scale(h->sum, h->summed, h->full_mv, h->cap_mv, &clipped);
        if (!h->streaming && v && begin_stream(h, e, now)) v = pagani_audio_haptics_scale(h->sum, h->summed, h->full_mv, h->cap_mv, &clipped);
        h->sum = 0;
        h->summed = 0;
        if (!h->streaming) {
            if (h->credit) h->credit--;
            continue;
        }
        emit(h, limit(h, v));
        if (clipped) h->stats.clips++;
    }
    flush(h);
    __atomic_store_n(&h->ctl->taken, h->taken, __ATOMIC_RELEASE);
}

/* A run starts at the frame that plays now. Nothing is cleared here: the audio server mixes as soon as StartIO returns, possibly before
 * this round, and what it wrote ahead belongs to the run; the ring was cleared when the last run ended. */
static void start_run(PaganiAudioHaptics *h, uint64_t frame)
{
    const uint64_t rest = frame > h->ended_frame ? (frame - h->ended_frame) / h->decimate : 0;
    h->credit = rest < h->credit ? h->credit - (uint32_t)rest : 0;
    h->taken = frame;
    h->sum = 0;
    h->summed = 0;
    h->pending = 0;
    h->running = 1;
    h->stop_pending = 0;
    __atomic_store_n(&h->ctl->taken, frame, __ATOMIC_RELEASE);
    __atomic_or_fetch(&h->ctl->state, PaganiAudioActuatorRunning, __ATOMIC_ACQ_REL);
    h->stats.runs++;
}

/* A run ends (its frames taken up to stop_frame, past the plug-in's lead): the stream drains, and whatever the ring still holds (written
 * after the clients stopped, which a run never plays) is cleared and counted, so the next run cannot meet it a ring turn later. */
static void end_run(PaganiAudioHaptics *h)
{
    end_stream(h, 0);
    for (uint32_t i = 0; i < h->ring_frames; ++i) {
        if (h->ring[i]) {
            h->ring[i] = 0;
            h->stats.stale++;
        }
    }
    h->running = 0;
    h->stop_pending = 0;
    h->ended_frame = h->taken;
    __atomic_and_fetch(&h->ctl->state, ~(uint32_t)PaganiAudioActuatorRunning, __ATOMIC_ACQ_REL);
    h->stats.run_ends++;
}

uint32_t pagani_audio_haptics_poll(PaganiAudioHaptics *h, const PaganiAudioEngine *e, uint64_t now)
{
    if (!h || !h->ctl) return kIdleWaitMs;
    const int want = LOAD(&e->act_users) != 0;
    const uint64_t frame = pagani_audio_engine_frame_at(e, now);
    if (want && !h->running) start_run(h, frame);
    else if (want) h->stop_pending = 0;
    else if (h->running && !h->stop_pending) {
        h->stop_pending = 1;
        h->stop_frame = frame + PAGANI_AUDIO_HAPTICS_KEEP_FRAMES;
    }
    if (!h->running) return kIdleWaitMs;
    const uint64_t due = frame + h->take_frames;
    take(h, e, h->stop_pending && due > h->stop_frame ? h->stop_frame : due, now);
    if (h->stop_pending && h->taken >= h->stop_frame) {
        end_run(h);
        return kIdleWaitMs;
    }
    return PAGANI_AUDIO_HAPTICS_POLL_MS;
}
