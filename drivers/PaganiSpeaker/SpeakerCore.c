/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * See SpeakerCore.h. Ported structure: UniXNU-sheng package/AudioProbe.cpp (commit 08cde20e); the 13T hardware, the TFA9873, the
 * staged bring-up, the run-tagged write tokens, the stall detection and the portable form are new. Freestanding C: no C library. */
#include "SpeakerCore.h"
#include "PaganiAdsp.h"

enum {
    kCommandMs = 2000,            /* one APM/PRM command's answer (sheng: 2 s) */
    kApmAnswerMs = 1000,
    kApmPauseMs = 100,
    kStallMs = 1000,              /* a running stream whose DSP answers nothing for this long has stalled */
    kStatusAfter = 10,            /* amplifier status read once per run after this many periods (100 ms) */
    kToneTail = 8,                /* silent periods after the tone before the stop (80 ms through the DSP's queue) */
    kToneGapMs = 1000,            /* between the tones of the walk */
    kRunLogs = 8,                 /* runs that get start/stop lines; then every 64th */
    kSendTries = 50,
};
/* The limiter works in whole 1 ms blocks of the path's stereo periods. */
_Static_assert(SpeakerPeriodFrames % SPEAKER_LIMIT_BLOCK == 0, "a period is whole limiter blocks");
_Static_assert(SpeakerChannels == SPEAKER_LIMIT_CHANNELS, "the limiter is stereo-linked like the path");

/* ---- small freestanding helpers ----------------------------------------------------------------------------------------------- */

static void copy(void *to, const void *from, uint32_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
}
static void zero(void *to, uint64_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (uint64_t i = 0; i < n; ++i) d[i] = 0;
}

typedef struct {char *buf; uint32_t size, used;} Text;
static void t_start(Text *t, char *buf, uint32_t size) {t->buf = buf; t->size = size; t->used = 0; if (size) buf[0] = 0;}
static void t_char(Text *t, char c) {if (t->used + 1 < t->size) {t->buf[t->used++] = c; t->buf[t->used] = 0;}}
static void t_str(Text *t, const char *s) {for (uint32_t i = 0; s && s[i]; ++i) t_char(t, s[i]);}
static void t_udec(Text *t, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do {d[n++] = (char)('0' + v % 10); v /= 10;} while (v && n < sizeof d);
    while (n) t_char(t, d[--n]);
}
static void t_hex(Text *t, uint64_t v)
{
    char d[16];
    unsigned n = 0;
    do {d[n++] = "0123456789abcdef"[v & 15]; v >>= 4;} while (v && n < sizeof d);
    t_str(t, "0x");
    while (n) t_char(t, d[--n]);
}
static void t_kd(Text *t, const char *k, int64_t v)
{
    t_char(t, ' '); t_str(t, k); t_char(t, '=');
    if (v < 0) {t_char(t, '-'); t_udec(t, (uint64_t)0 - (uint64_t)v);} else t_udec(t, (uint64_t)v);
}
static void t_kx(Text *t, const char *k, uint64_t v) {t_char(t, ' '); t_str(t, k); t_char(t, '='); t_hex(t, v);}
static void t_ks(Text *t, const char *k, const char *s) {t_char(t, ' '); t_str(t, k); t_char(t, '='); t_str(t, s);}

#define CTX (c->ops->context)
static uint64_t now_ms(SpeakerCore *c) {return c->ops->now_ms(CTX);}
static void wait_ms(SpeakerCore *c, uint32_t ms) {c->ops->wait(CTX, ms ? ms : 1);}
static Text line(SpeakerCore *c, const char *head)
{
    Text t;
    t_start(&t, c->line, sizeof c->line);
    t_str(&t, "[spk] ");
    t_str(&t, head);
    return t;
}
static void emit(SpeakerCore *c, Text *t) {(void)t; c->ops->trace(CTX, c->line);}
static uint8_t amp_address(unsigned amp) {return amp ? TFA9873_ADDRESS_BOTTOM : TFA9873_ADDRESS_TOP;}

/* No UFS write is on the wire while something is touched for the first time: a reset nothing sees coming is a power cut in the middle of a
 * write, and this flash can lose a whole logical unit to that (UfsLog.h). Three stretches, each opened by a line that has time to reach the
 * UFS log, then the writer held and a round under way given its time to end: "i2c" the pins, the rail, serial engine 4 and the amplifiers
 * (stages 1..5), "smmu" the stream routing, the first write to the low buffer and the DSP's first mapping (8..10; the board reset of sheng's
 * translating contexts), "tone" the first DMA and bit clock of the first tone. iOS's first run on a boot without a tone is not one: it runs
 * on the audio service's thread, which must not sleep this long, and the tone rounds have been through the same path. let_go ends a
 * stretch; one that hangs never gets there and its line is the last one read back. */
static void let_go(SpeakerCore *c)
{
    if (!c->held) return;
    c->held = 0;
    c->ops->quiet(CTX, 0);
}
static void sleep_for(SpeakerCore *c, uint32_t ms)
{
    const uint64_t end = now_ms(c) + ms;
    for (uint64_t now = now_ms(c); now < end; now = now_ms(c)) wait_ms(c, (uint32_t)(end - now));
}
static void guard(SpeakerCore *c, const char *what)
{
    if (!c->ops->quiet) return;
    let_go(c);
    Text t = line(c, "guard ");
    t_str(&t, what);
    emit(c, &t);
    if (!c->config.crumb_ms) return;
    sleep_for(c, c->config.crumb_ms);
    c->held = 1;
    c->ops->quiet(CTX, 1);
    sleep_for(c, SPEAKER_QUIET_MS);
}

static int fail(SpeakerCore *c, int32_t stage, int32_t error, const char *what)
{
    c->stage = -stage;
    c->error = error;
    Text t = line(c, "FAIL");
    t_kd(&t, "st", -stage);
    t_kd(&t, "err", error);
    t_char(&t, ' ');
    t_str(&t, what);
    if (stage >= SpeakerStDsp) {
        t_kx(&t, "op", c->last_opcode);
        t_kx(&t, "dsp", c->last_status);
    }
    emit(c, &t);
    return 0;
}
/* 1: go on; 0: stop here as configured. */
static int reached(SpeakerCore *c, int32_t stage)
{
    c->stage = stage;
    if (c->config.stop_after != (uint32_t)stage) return 1;
    c->error = SpeakerErrStopped;
    Text t = line(c, "stopped as asked (pagani-speaker-stop)");
    t_kd(&t, "st", stage);
    emit(c, &t);
    return 0;
}
void speaker_core_reached(SpeakerCore *c, int32_t stage) {c->stage = stage;}

/* ---- answers ------------------------------------------------------------------------------------------------------------------- */

void speaker_core_receive(SpeakerCore *c, const uint8_t *data, uint32_t length)
{
    const uint32_t head = __atomic_load_n(&c->head, __ATOMIC_RELAXED), tail = __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
    if (!data || !length) return;
    if (head - tail >= SPEAKER_ANSWERS) {
        __atomic_add_fetch(&c->dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    const uint32_t n = length < SPEAKER_ANSWER_BYTES ? length : SPEAKER_ANSWER_BYTES;
    c->ring[head % SPEAKER_ANSWERS].length = n;
    copy(c->ring[head % SPEAKER_ANSWERS].data, data, n);
    __atomic_store_n(&c->head, head + 1, __ATOMIC_RELEASE);
}

void speaker_core_state(SpeakerCore *c, int up)
{
    if (up) {
        __atomic_store_n(&c->up, 1u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_exchange_n(&c->up, 0u, __ATOMIC_ACQ_REL)) __atomic_add_fetch(&c->downs, 1, __ATOMIC_RELEASE);
}

/* The next raw answer; 1, or 0 when none is queued. */
static int pop_raw(SpeakerCore *c, uint8_t *data, uint32_t *length)
{
    const uint32_t tail = __atomic_load_n(&c->tail, __ATOMIC_RELAXED), head = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0;
    *length = c->ring[tail % SPEAKER_ANSWERS].length;
    copy(data, c->ring[tail % SPEAKER_ANSWERS].data, *length);
    __atomic_store_n(&c->tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}
static int pop(SpeakerCore *c, ArAnswer *a)
{
    uint8_t data[SPEAKER_ANSWER_BYTES];
    uint32_t length = 0;
    while (pop_raw(c, data, &length)) {
        if (pagani_ar_parse(data, length, a) != ArBad) return 1;
        c->bad++;
    }
    return 0;
}
static int is_dead(SpeakerCore *c)
{
    if (!c->dead && __atomic_load_n(&c->downs, __ATOMIC_ACQUIRE)) {
        c->dead = 1;
        c->fault = SpeakerFaultDown;
    }
    return c->dead;
}
/* 1 with an answer, 0 when ms passed, -1 when the channel is down. */
static int next_answer(SpeakerCore *c, ArAnswer *a, uint32_t ms)
{
    const uint64_t end = now_ms(c) + ms;
    for (;;) {
        if (pop(c, a)) return 1;
        if (is_dead(c)) return -1;
        const uint64_t now = now_ms(c);
        if (now >= end) return 0;
        wait_ms(c, (uint32_t)(end - now));
    }
}

static void dispatch(SpeakerCore *c, const ArAnswer *a);

static int send_packet(SpeakerCore *c, uint32_t length)
{
    if (!length) return SpeakerErrArgument;
    if (is_dead(c)) return SpeakerErrDown;
    for (unsigned tries = 0;; ++tries) {
        const int rc = c->ops->send(CTX, c->packet, length);
        if (rc == PaganiAdspOk) return 0;
        if (rc == PaganiAdspFull && tries < kSendTries) {wait_ms(c, 1); continue;}
        c->send_failures++;
        if (rc == PaganiAdspDown) {
            c->dead = 1;
            c->fault = SpeakerFaultDown;
            return SpeakerErrDown;
        }
        return SpeakerErrIo;
    }
}

/* Sends the packet in c->packet and waits for its answer: a basic result with its opcode, the memory map's handle or the PRM's done.
 * Buffer answers that arrive meanwhile belong to the running stream and are handled (sheng: dropping them starved the stream). */
static int command(SpeakerCore *c, uint32_t length, uint32_t opcode)
{
    c->last_opcode = opcode;
    const int rc = send_packet(c, length);
    if (rc) return rc;
    const uint64_t end = now_ms(c) + kCommandMs;
    for (;;) {
        const uint64_t now = now_ms(c);
        ArAnswer a;
        const int got = now < end ? next_answer(c, &a, (uint32_t)(end - now)) : 0;
        if (got < 0) return SpeakerErrDown;
        if (!got) {
            c->last_status = 0xffffffffu;
            return SpeakerErrTimeout;
        }
        uint32_t status = 0;
        int match = 0;
        if (a.opcode == GPR_OP_BASIC_RESULT && a.result_opcode == opcode) {match = 1; status = a.status;}
        else if (opcode == AR_OP_MEM_MAP && a.opcode == AR_OP_MEM_MAP_DONE) {match = 1; c->handle = a.handle; status = a.handle ? 0 : 1;}
        else if ((opcode == AR_OP_PRM_REQUEST && a.opcode == AR_OP_PRM_REQUEST_DONE) ||
                 (opcode == AR_OP_PRM_RELEASE && a.opcode == AR_OP_PRM_RELEASE_DONE)) {match = 1; status = a.status;}
        if (!match) {
            dispatch(c, &a);
            continue;
        }
        c->answers++;
        c->last_status = status;
        return status ? SpeakerErrRefused : 0;
    }
}

/* ---- playback ------------------------------------------------------------------------------------------------------------------ */

static int submit(SpeakerCore *c)
{
    const uint32_t slot = c->sent % SpeakerSlots;
    uint8_t *dst = c->buffer + (uint64_t)slot * SpeakerPeriodBytes;
    /* the source's period, then the digital ceiling on its way into the DSP slot (written once, in order: write-combined memory) */
    c->source->take(c->source->context, c->period);
    speaker_limit_run(&c->limit, c->period, (int16_t *)(void *)dst, SpeakerPeriodFrames);
    c->ops->publish(CTX);
    const uint32_t token = (c->runs & 0xffffu) << 16 | (c->sent & 0xffffu);
    const uint32_t n = pagani_ar_write(c->packet, sizeof c->packet, SpeakerGprPort, token, c->dsp_address + (uint64_t)slot * SpeakerPeriodBytes,
                                       c->handle, SpeakerPeriodBytes);
    c->sent++;
    const int rc = c->ops->send(CTX, c->packet, n);
    if (rc != PaganiAdspOk) {
        c->send_failures++;
        if (rc == PaganiAdspDown) {c->dead = 1; c->fault = SpeakerFaultDown;}
        return -1;
    }
    c->in_flight++;
    return 0;
}
static void refill(SpeakerCore *c)
{
    while (c->running && !c->dead && c->in_flight < SpeakerInFlight && !submit(c)) {}
}

static void dispatch(SpeakerCore *c, const ArAnswer *a)
{
    const int ours = c->running && (a->token >> 16) == (c->runs & 0xffffu);
    if (a->opcode == AR_OP_WRITE_DONE) {
        if (!ours) {c->stale++; return;}
        if (c->in_flight) c->in_flight--;
        c->played++;
        c->last_played_ms = now_ms(c);
        if (a->status) c->write_errors++;
        c->source->played(c->source->context);
        refill(c);
        return;
    }
    if (a->opcode == GPR_OP_BASIC_RESULT && a->result_opcode == AR_OP_WRITE && a->status) {
        /* a refused buffer never comes back: it leaves the queue here */
        if (!ours) {c->stale++; return;}
        if (c->in_flight) c->in_flight--;
        c->write_errors++;
        c->last_status = a->status;
        return;
    }
    if (a->opcode == GPR_OP_BASIC_RESULT && a->status) c->last_status = a->status;
    c->stray++;
}

static uint32_t clock_hz(const SpeakerCore *c) {return (uint32_t)SpeakerRate * SpeakerChannels * c->config.device_bits;}

static int graph(SpeakerCore *c, uint32_t opcode, uint32_t sg)
{
    return command(c, pagani_ar_graph_command(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token, opcode, sg), opcode);
}

static int prm_clock(SpeakerCore *c, int on)
{
    const uint32_t n = pagani_ar_prm_clock(c->packet, sizeof c->packet, c->config.prm_port, ++c->token, AR_CLK_SEC_MI2S_IBIT,
                                           on ? clock_hz(c) : 0, AR_CLK_ATTRIBUTE_COUPLE_NO);
    return command(c, n, on ? AR_OP_PRM_REQUEST : AR_OP_PRM_RELEASE);
}

static void amps_program(SpeakerCore *c, uint32_t mask, unsigned (*program)(Tfa9873Step *), uint32_t *ok, uint32_t *polls_failed)
{
    Tfa9873Step steps[TFA9873_MAX_STEPS];
    const unsigned n = program(steps);
    if (ok) *ok = 0;
    for (unsigned amp = 0; amp < SpeakerAmps; ++amp) {
        if (!(mask & (1u << amp))) continue;
        Tfa9873Run run;
        if (speaker_amp_run(&c->bus, amp_address(amp), steps, n, &run)) continue;
        if (ok) *ok |= 1u << amp;
        if (polls_failed && run.polls_failed) *polls_failed |= 1u << amp;
        if (program == tfa9873_status_program)
            for (unsigned i = 0; i < Tfa9873StCount && i < run.read_count; ++i) c->amp_status[amp][i] = run.reads[i];
    }
}

static int run_logged(const SpeakerCore *c) {return c->runs <= kRunLogs || !(c->runs % 64u);}

static void status_line(SpeakerCore *c)
{
    uint32_t ok = 0;
    amps_program(c, c->amps_on, tfa9873_status_program, &ok, 0);
    for (unsigned amp = 0; amp < SpeakerAmps; ++amp) {
        if (!(ok & (1u << amp))) continue;
        const uint16_t *s = c->amp_status[amp];
        Text t = line(c, "amp");
        t_char(&t, ' ');
        t_hex(&t, amp_address(amp));
        t_kd(&t, "state", TFA9873_MANSTATE(s[Tfa9873StManager]));
        t_kd(&t, "clks", !!(s[Tfa9873StStatus] & TFA9873_CLKS));
        t_kd(&t, "plls", !!(s[Tfa9873StStatus2] & TFA9873_PLLS));
        t_kd(&t, "sws", !!(s[Tfa9873StStatus2] & TFA9873_SWS));
        t_kd(&t, "ampen", !!(s[Tfa9873StStatus2] & TFA9873_AMPS));
        t_kd(&t, "noclk", !!(s[Tfa9873StStatus] & TFA9873_NOCLK));
        t_kd(&t, "tdmerr", !!(s[Tfa9873StStatus] & TFA9873_TDMERR));
        t_kx(&t, "st", s[Tfa9873StStatus]);
        t_kx(&t, "st2", s[Tfa9873StStatus2]);
        t_kx(&t, "st3", s[Tfa9873StStatus3]);
        t_kx(&t, "man", s[Tfa9873StManager]);
        t_kx(&t, "irq", s[Tfa9873StIrq]);
        t_kd(&t, "bat", s[Tfa9873StBattery] & 0x3ffu);
        t_kd(&t, "temp", s[Tfa9873StTemperature] & 0x1ffu);
        emit(c, &t);
    }
}

/* A run that could not start: whatever it took is given back. */
static int refuse(SpeakerCore *c, int rc)
{
    if (c->clock_on && !c->dead) {
        const uint32_t op = c->last_opcode, status = c->last_status;
        prm_clock(c, 0);
        c->last_opcode = op;
        c->last_status = status;
    }
    c->clock_on = 0;
    uint32_t ctl[4];
    speaker_pins_i2s(c->ops->io, 0, ctl);
    c->source = 0;
    Text t = line(c, "play refused");
    t_kd(&t, "run", c->runs);
    t_kd(&t, "rc", rc);
    t_kx(&t, "op", c->last_opcode);
    t_kx(&t, "dsp", c->last_status);
    emit(c, &t);
    return rc ? rc : SpeakerErrIo;
}

int speaker_core_start(SpeakerCore *c, const SpeakerSource *source)
{
    if (!source || !source->take || !source->played || !source->align) return SpeakerErrArgument;
    if (is_dead(c)) return SpeakerErrDown;
    if (!c->ready) return SpeakerErrNoDevice;
    if (c->running) return 0;
    const uint64_t t0 = now_ms(c);
    c->runs++;
    c->source = source;
    c->in_flight = c->sent = c->played = 0;
    c->status_read = 0;
    c->fault = SpeakerFaultNone;
    uint32_t ctl[4];
    const int pins = speaker_pins_i2s(c->ops->io, 1, ctl);
    int rc = 0;
    c->clock_on = 0;
    c->prm_status = 0;
    if (c->config.prm_clock) {
        rc = prm_clock(c, 1);
        c->clock_on = rc == 0;
        c->prm_status = rc ? c->last_status : 0;   /* the DSP may drive the clock itself (sheng's speaker never asked) */
        if (rc == SpeakerErrDown) return refuse(c, rc);
    }
    if ((rc = graph(c, AR_OP_GRAPH_PREPARE, AR_SG_SPEAKER)) || (rc = graph(c, AR_OP_GRAPH_PREPARE, AR_SG_PLAY)) ||
        (rc = graph(c, AR_OP_GRAPH_START, AR_SG_SPEAKER)) || (rc = graph(c, AR_OP_GRAPH_START, AR_SG_PLAY))) {
        const uint32_t op = c->last_opcode, status = c->last_status;
        if (!c->dead) {
            graph(c, AR_OP_GRAPH_STOP, AR_SG_PLAY);
            graph(c, AR_OP_GRAPH_STOP, AR_SG_SPEAKER);
        }
        c->last_opcode = op;
        c->last_status = status;
        return refuse(c, rc);
    }
    /* the bit clock and word select run: the amplifiers' PLLs can lock */
    uint32_t on = 0, clks = 0;
    amps_program(c, c->amps_ready, tfa9873_power_up_program, &on, &clks);
    c->amps_on = on;
    if (clks) c->clks_failed++;
    source->align(source->context);
    speaker_limit_reset(&c->limit);   /* a run starts from silence at unity gain: nothing of the last run is held back into it */
    c->running = 1;
    c->run_start_ms = c->last_played_ms = now_ms(c);
    refill(c);
    if (run_logged(c)) {
        Text t = line(c, "play start");
        t_kd(&t, "run", c->runs);
        t_kx(&t, "amps", on);
        t_kx(&t, "clks_wait_failed", clks);
        t_kd(&t, "prm", c->clock_on ? 0 : (int64_t)c->prm_status);
        t_kd(&t, "pins", pins);
        t_kd(&t, "queued", c->in_flight);
        t_kd(&t, "ms", (int64_t)(now_ms(c) - t0));
        emit(c, &t);
    }
    return 0;
}

void speaker_core_stop(SpeakerCore *c)
{
    if (!c->running) return;
    c->running = 0;
    amps_program(c, c->amps_on, tfa9873_power_down_program, 0, 0);
    c->amps_on = 0;
    if (!is_dead(c)) {
        graph(c, AR_OP_GRAPH_STOP, AR_SG_PLAY);
        graph(c, AR_OP_GRAPH_STOP, AR_SG_SPEAKER);
        if (c->clock_on) prm_clock(c, 0);
    }
    c->clock_on = 0;
    uint32_t ctl[4];
    speaker_pins_i2s(c->ops->io, 0, ctl);
    c->source = 0;
    if (run_logged(c)) {
        Text t = line(c, "play stop");
        t_kd(&t, "run", c->runs);
        t_kd(&t, "sent", c->sent);
        t_kd(&t, "played", c->played);
        t_kd(&t, "werr", c->write_errors);
        t_kd(&t, "sendfail", c->send_failures);
        t_kd(&t, "stale", c->stale);
        t_kd(&t, "stalls", c->stalls);
        t_kd(&t, "ms", (int64_t)(now_ms(c) - c->run_start_ms));
        t_kd(&t, "lim", c->limit.limited);
        t_kd(&t, "gmin", speaker_limit_permille(c->limit.min_gain));
        t_kd(&t, "in", c->limit.peak_in);
        t_kd(&t, "out", c->limit.peak_out);
        t_kd(&t, "clamp", c->limit.clamps);
        emit(c, &t);
    }
}

uint32_t speaker_core_pump(SpeakerCore *c)
{
    ArAnswer a;
    while (pop(c, &a)) dispatch(c, &a);
    if (is_dead(c)) {
        if (c->running) {
            /* nothing can reach the DSP any more: the amplifiers (I2C) and the pins still can */
            c->running = 0;
            amps_program(c, c->amps_on, tfa9873_power_down_program, 0, 0);
            c->amps_on = 0;
            uint32_t ctl[4];
            speaker_pins_i2s(c->ops->io, 0, ctl);
            c->source = 0;
            Text t = line(c, "play lost: the DSP channel closed");
            t_kd(&t, "run", c->runs);
            t_kd(&t, "played", c->played);
            emit(c, &t);
        }
        return 1000;
    }
    if (!c->running) return 1000;
    refill(c);
    if (!c->status_read && c->played >= kStatusAfter) {
        c->status_read = 1;
        if (c->runs <= 2 || run_logged(c)) status_line(c);
    }
    if (c->in_flight && !c->fault && now_ms(c) - c->last_played_ms > kStallMs) {
        c->fault = SpeakerFaultStall;
        c->stalls++;
        Text t = line(c, "stall: the DSP stopped answering");
        t_kd(&t, "run", c->runs);
        t_kd(&t, "sent", c->sent);
        t_kd(&t, "played", c->played);
        t_kd(&t, "inflight", c->in_flight);
        t_kd(&t, "dropped", c->dropped);
        emit(c, &t);
    }
    return 10;
}

/* ---- the tone ------------------------------------------------------------------------------------------------------------------ */

typedef struct {SpeakerTone tone; uint32_t periods, channels;} ToneSource;
static void tone_align(void *context) {(void)context;}
static void tone_take(void *context, int16_t *dst)
{
    ToneSource *s = (ToneSource *)context;
    speaker_tone_fill(&s->tone, dst, SpeakerPeriodFrames, SpeakerChannels);
    for (uint32_t ch = 0; ch < SpeakerChannels; ++ch) {
        if (s->channels & (1u << ch)) continue;
        for (uint32_t f = 0; f < SpeakerPeriodFrames; ++f) dst[f * SpeakerChannels + ch] = 0;   /* this speaker stays silent */
    }
    s->periods++;
}
static void tone_played(void *context) {(void)context;}

int speaker_core_tone(SpeakerCore *c, uint32_t hz, uint32_t ms, uint32_t atten_db)
{
    return speaker_core_tone_on(c, hz, ms, atten_db, (1u << SpeakerChannels) - 1u);
}

int speaker_core_tone_on(SpeakerCore *c, uint32_t hz, uint32_t ms, uint32_t atten_db, uint32_t channels)
{
    channels &= (1u << SpeakerChannels) - 1u;
    if (!channels) return SpeakerErrArgument;
    ToneSource s;
    zero(&s, sizeof s);
    speaker_tone_init(&s.tone, hz, ms, atten_db, SpeakerRate);
    s.channels = channels;
    const SpeakerSource source = {&s, tone_align, tone_take, tone_played};
    const uint32_t periods = (s.tone.frames + SpeakerPeriodFrames - 1) / SpeakerPeriodFrames;
    int rc = speaker_core_start(c, &source);
    if (rc) return rc;
    const uint64_t end = now_ms(c) + ms + 3000;
    while (c->running && !c->fault) {
        speaker_core_pump(c);
        if (c->played >= periods + kToneTail) break;
        if (now_ms(c) >= end) {rc = SpeakerErrTimeout; break;}
        wait_ms(c, 10);
    }
    if (c->fault) rc = c->fault == SpeakerFaultDown ? SpeakerErrDown : SpeakerErrTimeout;
    const uint32_t sent = c->sent, played = c->played, amps = c->amps_on;
    speaker_core_stop(c);
    Text t = line(c, "tone");
    t_kd(&t, "rc", rc);
    t_kd(&t, "hz", hz);
    t_kd(&t, "ms", ms);
    t_kd(&t, "atten", atten_db);
    t_kd(&t, "gain", speaker_tone_gain(atten_db));
    t_kd(&t, "periods", periods);
    t_kd(&t, "sent", sent);
    t_kd(&t, "played", played);
    t_kx(&t, "amps", amps);
    t_kd(&t, "clks_failed", c->clks_failed);
    t_kx(&t, "ch", channels);
    t_kd(&t, "out", c->limit.peak_out);
    t_kd(&t, "ceil", c->limit.ceiling);
    emit(c, &t);
    if (!rc) c->stage = SpeakerStTone;
    return rc;
}

/* A pause between tones that still takes the DSP's answers (stale ones of the run before are counted, never left queued). */
static void pause_ms(SpeakerCore *c, uint32_t ms)
{
    const uint64_t end = now_ms(c) + ms;
    for (uint64_t now = now_ms(c); now < end; now = now_ms(c)) {
        speaker_core_pump(c);
        wait_ms(c, (uint32_t)(end - now));
    }
}

int speaker_core_tones(SpeakerCore *c, uint32_t mode, uint32_t hz, uint32_t ms, uint32_t atten_db)
{
    static const uint32_t walk[3] = {3u, 1u, 2u};
    const uint32_t count = mode == 2 ? 3u : 1u;
    if (count > 1) {
        Text t = line(c, "tone walk: 1 both speakers, 2 amplifier 0x34 alone (slot 0, the top one?), 3 amplifier 0x35 alone (slot 1)");
        emit(c, &t);
    }
    guard(c, "tone");
    int first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) pause_ms(c, kToneGapMs);
        const int rc = speaker_core_tone_on(c, hz, ms, atten_db, walk[i]);
        let_go(c);
        if (rc && !first) first = rc;
        if (rc == SpeakerErrDown || c->dead) break;   /* nothing reaches the DSP any more */
    }
    return first;
}

/* ---- bring-up ------------------------------------------------------------------------------------------------------------------ */

void speaker_core_init(SpeakerCore *c, const SpeakerOps *ops, const SpeakerConfig *config)
{
    zero(c, sizeof *c);
    c->ops = ops;
    c->config = *config;
    SpeakerConfig *k = &c->config;
    if (!(k->amps & 3u)) k->amps = 3;
    k->amps &= 3u;
    if (k->sd_line != AR_I2S_SD1) k->sd_line = AR_I2S_SD0;
    if (k->device_bits != 16) k->device_bits = 32;
    if (k->prm_port != AR_PRM_PORT_ASM) k->prm_port = AR_PRM_IID;
    k->sids = (k->sids & ((1u << SPEAKER_SID_COUNT) - 1u)) | 1u;
    if (!k->buffer_pa) k->buffer_pa = SPEAKER_BUFFER_PA;
    if (!k->dsp_wait_ms) k->dsp_wait_ms = 120000;
    if (!k->apm_queries) k->apm_queries = 100;
    speaker_limit_init(&c->limit, k->ceiling_db);
    k->ceiling_db = c->limit.ceiling_db;
    c->buffer_pa = k->buffer_pa;
    /* the DSP takes a 32-bit address with the stream index (0x1001 & 0xf) in the upper word (sheng i491..i495) */
    c->dsp_address = (k->buffer_pa & 0xffffffffull) | (uint64_t)(0x1001u & 0xfu) << 32;
}

static int stage_pins(SpeakerCore *c)
{
    uint32_t i2c[2] = {0, 0}, i2s[4] = {0, 0, 0, 0};
    const int a = speaker_pins_i2c(c->ops->io, (int)c->config.i2c_pull, i2c), b = speaker_pins_i2s(c->ops->io, 0, i2s);
    Text t = line(c, "pins");
    t_kx(&t, "sda", i2c[0]);
    t_kx(&t, "scl", i2c[1]);
    t_kx(&t, "sck", i2s[0]);
    t_kx(&t, "d0", i2s[1]);
    t_kx(&t, "ws", i2s[2]);
    t_kx(&t, "d1", i2s[3]);
    emit(c, &t);
    if (a || b) return fail(c, SpeakerStPins, SpeakerErrIo, "pin readback");
    return reached(c, SpeakerStPins);
}

static int stage_rail(SpeakerCore *c)
{
    if (c->config.rail && c->ops->rail) {
        uint32_t detail = 0;
        const int rc = c->ops->rail(CTX, SPEAKER_RAIL_MV, &detail);
        Text t = line(c, "rail " SPEAKER_RAIL);
        t_kd(&t, "mv", SPEAKER_RAIL_MV);
        t_kd(&t, "rc", rc);
        t_kx(&t, "detail", detail);
        emit(c, &t);   /* not fatal: the rail may be on already; the amplifiers' answer decides */
    }
    return reached(c, SpeakerStRail);
}

static int stage_bus(SpeakerCore *c)
{
    const int clocks = speaker_clocks_on(c->ops->io, &c->clocks);
    Text t = line(c, "bus");
    t_kd(&t, "clk", clocks);
    t_kx(&t, "vote", c->clocks.vote);
    /* the shared vote register: lock=1 written under the lock word, 0 written without it (held elsewhere for 50 ms), - not written */
    t_ks(&t, "lock", !c->clocks.vote_wrote ? "-" : c->clocks.vote_locked ? "1" : "0");
    t_kd(&t, "waits", c->clocks.vote_waits);
    t_kx(&t, "ahb", c->clocks.sahb);
    t_kx(&t, "core", c->clocks.core);
    t_kx(&t, "s4", c->clocks.s4);
    t_kx(&t, "rcg", c->clocks.rcg_cfg);
    t_kd(&t, "rcg_fixed", c->clocks.rcg_fixed);
    if (clocks) {
        emit(c, &t);
        return fail(c, SpeakerStBus, clocks, "i2c hub clocks");
    }
    const int se = speaker_se_setup(c->ops->io, &c->se);
    uint32_t config = 0;
    const int bus = se ? se : speaker_bus_init(&c->bus, c->ops->io, &config);
    t_kx(&t, "fw", c->se.fw);
    t_kx(&t, "if", c->se.if_disable);
    t_kx(&t, "clkcfg", c->se.clk);
    t_kx(&t, "scl", c->se.scl);
    t_kd(&t, "set", c->se.configured);
    t_kd(&t, "abort", c->se.aborted);
    t_kd(&t, "rc", bus);
    emit(c, &t);
    if (bus) return fail(c, SpeakerStBus, bus, "serial engine 4");
    return reached(c, SpeakerStBus);
}

static void amp_failure(SpeakerCore *c, unsigned amp, const char *what, int rc, const Tfa9873Run *run)
{
    Text t = line(c, "amp");
    t_char(&t, ' ');
    t_hex(&t, amp_address(amp));
    t_char(&t, ' ');
    t_str(&t, what);
    t_kd(&t, "rc", rc);
    t_kd(&t, "step", run ? run->index : 0);
    t_kd(&t, "geni", c->bus.last_error);
    t_kx(&t, "irq", c->bus.last_status);
    emit(c, &t);
}

static int stage_amps(SpeakerCore *c)
{
    uint32_t ctl[2] = {0, 0};
    const int reset = speaker_reset_amps(c->ops->io, c->config.amps, ctl);
    if (reset) {
        Text t = line(c, "reset");
        t_kx(&t, "top", ctl[0]);
        t_kx(&t, "bottom", ctl[1]);
        emit(c, &t);
        return fail(c, SpeakerStAmps, reset, "reset pin readback");
    }
    Tfa9873Step steps[TFA9873_MAX_STEPS];
    const unsigned n = tfa9873_identify_program(steps);
    for (unsigned amp = 0; amp < SpeakerAmps; ++amp) {
        if (!(c->config.amps & (1u << amp))) continue;
        Tfa9873Run run;
        const int rc = speaker_amp_run(&c->bus, amp_address(amp), steps, n, &run);
        if (rc) {
            amp_failure(c, amp, "absent", rc, &run);
            continue;
        }
        c->rev[amp] = run.reads[0];
        Text t = line(c, "amp");
        t_char(&t, ' ');
        t_hex(&t, amp_address(amp));
        t_kx(&t, "rev", run.reads[0]);
        t_ks(&t, "type", tfa9873_known(run.reads[0]) ? "tfa9873" : "unknown");
        t_kd(&t, "table", tfa9873_table_size(run.reads[0]));
        emit(c, &t);
        if (tfa9873_known(run.reads[0])) c->amps_found |= 1u << amp;
    }
    if (!c->amps_found) return fail(c, SpeakerStAmps, SpeakerErrNoDevice, "no TFA9873 answered");
    return reached(c, SpeakerStAmps);
}

static int stage_amp_config(SpeakerCore *c)
{
    for (unsigned amp = 0; amp < SpeakerAmps; ++amp) {
        if (!(c->amps_found & (1u << amp))) continue;
        const Tfa9873Config k = {c->rev[amp], (uint8_t)amp, (uint8_t)c->config.device_bits, (uint8_t)(c->config.feedback != 0)};
        Tfa9873Step steps[TFA9873_MAX_STEPS];
        Tfa9873Run run;
        unsigned n = tfa9873_init_program(steps, &k);
        int rc = n ? speaker_amp_run(&c->bus, amp_address(amp), steps, n, &run) : SpeakerErrArgument;
        if (rc) {
            amp_failure(c, amp, "init failed", rc, n ? &run : 0);
            continue;
        }
        const uint32_t transfers = run.transfers;
        n = tfa9873_verify_program(steps);
        rc = speaker_amp_run(&c->bus, amp_address(amp), steps, n, &run);
        if (rc) {
            amp_failure(c, amp, "verify read failed", rc, &run);
            continue;
        }
        uint16_t got = 0, want = 0;
        const int bad = tfa9873_verify(&k, run.reads, run.read_count, &got, &want);
        Text t = line(c, "amp");
        t_char(&t, ' ');
        t_hex(&t, amp_address(amp));
        t_str(&t, bad ? " config MISMATCH" : " config ok");
        t_kd(&t, "transfers", transfers);
        t_kd(&t, "bits", c->config.device_bits);
        t_kd(&t, "slot", amp);
        t_kx(&t, "tdm", (uint64_t)run.reads[3] << 16 | run.reads[4]);
        t_kx(&t, "en", run.reads[6]);
        t_kx(&t, "slots", (uint64_t)run.reads[7] << 16 | run.reads[8]);
        if (bad) {
            t_kx(&t, "reg", (uint32_t)bad);
            t_kx(&t, "got", got);
            t_kx(&t, "want", want);
        }
        emit(c, &t);
        if (!bad) c->amps_ready |= 1u << amp;
    }
    if (!c->amps_ready) return fail(c, SpeakerStAmpConfig, SpeakerErrIo, "no amplifier configured");
    return reached(c, SpeakerStAmpConfig);
}

static int stage_dsp(SpeakerCore *c)
{
    const uint64_t t0 = now_ms(c);
    const int rc = c->ops->channel(CTX);
    if (rc) return fail(c, SpeakerStDsp, SpeakerErrNoDevice, "adsp_apps not registered (pagani-adsp=1 missing?)");
    uint64_t last_check = 0;
    while (!__atomic_load_n(&c->up, __ATOMIC_ACQUIRE)) {
        const uint64_t now = now_ms(c);
        if (now - t0 >= c->config.dsp_wait_ms) return fail(c, SpeakerStDsp, SpeakerErrTimeout, "the DSP never opened adsp_apps");
        if (now - last_check >= 500) {
            last_check = now;
            int32_t stage = 0;
            uint32_t crashed = 0;
            if (c->ops->dsp_state(CTX, &stage, &crashed) == 0 && (stage < 0 || crashed)) {
                c->last_status = (uint32_t)stage;
                return fail(c, SpeakerStDsp, SpeakerErrDown, "PaganiAdsp failed (see its [adsp] lines)");
            }
        }
        wait_ms(c, 100);
    }
    Text t = line(c, "dsp up");
    t_kd(&t, "waited_ms", (int64_t)(now_ms(c) - t0));
    emit(c, &t);
    return reached(c, SpeakerStDsp);
}

static int stage_apm(SpeakerCore *c)
{
    const uint64_t t0 = now_ms(c);
    GprSpfProbe probe;
    pagani_gpr_spf_init(&probe, SpeakerGprPort, c->config.apm_queries);
    for (;;) {
        uint8_t query[GPR_HEADER_BYTES];
        const uint32_t n = pagani_gpr_spf_next(&probe, query);
        if (!n) break;                                        /* ready, or out of queries */
        copy(c->packet, query, n);
        c->last_opcode = GPR_OP_APM_GET_SPF_STATE;
        const int sent = send_packet(c, n);
        if (sent == SpeakerErrDown) return fail(c, SpeakerStApm, sent, "the DSP channel closed");
        const uint64_t end = now_ms(c) + (sent ? 0u : (uint32_t)kApmAnswerMs);
        while (probe.state == SpfWaiting) {
            const uint64_t now = now_ms(c);
            if (now >= end) {
                pagani_gpr_spf_timeout(&probe);               /* the query counts as unanswered; the probe decides on another */
                break;
            }
            uint8_t data[SPEAKER_ANSWER_BYTES];
            uint32_t length = 0;
            if (!pop_raw(c, data, &length)) {
                if (is_dead(c)) return fail(c, SpeakerStApm, SpeakerErrDown, "the DSP channel closed");
                wait_ms(c, (uint32_t)(end - now));
                continue;
            }
            if (pagani_gpr_spf_receive(&probe, data, length) == SpfIgnored) {
                ArAnswer a;
                if (pagani_ar_parse(data, length, &a) != ArBad) dispatch(c, &a);
            }
        }
        c->last_status = probe.last_status;
        if (probe.state == SpfReady || probe.state == SpfGaveUp) break;
        wait_ms(c, kApmPauseMs);
    }
    Text t = line(c, "apm");
    t_ks(&t, "state", probe.state == SpfReady ? "ready" : "not-ready");
    t_kd(&t, "queries", probe.queries);
    t_kd(&t, "answers", probe.answers);
    t_kd(&t, "errors", probe.errors);
    t_kd(&t, "timeouts", probe.timeouts);
    t_kd(&t, "last", probe.last_status);
    t_kd(&t, "ms", (int64_t)(now_ms(c) - t0));
    emit(c, &t);
    if (probe.state != SpfReady) return fail(c, SpeakerStApm, SpeakerErrTimeout, "the audio framework never became ready");
    return reached(c, SpeakerStApm);
}

static int stage_smmu(SpeakerCore *c)
{
    static const uint16_t sids[SPEAKER_SID_COUNT] = SPEAKER_SIDS;
    Text t = line(c, "smmu start");
    t_kx(&t, "sids", c->config.sids);
    emit(c, &t);   /* in the log before the first write: a board reset here shows where it came from */
    t = line(c, "smmu");
    int required = 0;
    for (unsigned i = 0; i < SPEAKER_SID_COUNT; ++i) {
        if (!(c->config.sids & (1u << i))) continue;
        uint32_t detail = 0;
        const int rc = c->ops->smmu(CTX, sids[i], c->buffer_pa, SPEAKER_BUFFER_BYTES, &detail);
        if (!rc) c->sid_mask |= 1u << i;
        else if (!i) required = rc;
        t_char(&t, ' ');
        t_hex(&t, sids[i]);
        t_char(&t, '=');
        if (rc) {t_char(&t, '-'); t_udec(&t, (uint64_t)-(int64_t)rc);} else t_str(&t, "ok");
        t_char(&t, '/');
        t_hex(&t, detail);
    }
    emit(c, &t);
    if (required) return fail(c, SpeakerStSmmu, required, "stream 0x1001 not in pass-through");
    return reached(c, SpeakerStSmmu);
}

static int stage_buffer(SpeakerCore *c)
{
    c->buffer = c->ops->buffer(CTX, c->buffer_pa, SPEAKER_BUFFER_BYTES);
    if (!c->buffer) return fail(c, SpeakerStBuffer, SpeakerErrNoMemory, "period buffer not mapped (pagani-speaker-pa?)");
    zero(c->buffer, SPEAKER_BUFFER_BYTES);
    c->ops->publish(CTX);
    Text t = line(c, "buffer");
    t_kx(&t, "pa", c->buffer_pa);
    t_kx(&t, "dsp", c->dsp_address);
    t_kd(&t, "bytes", SPEAKER_BUFFER_BYTES);
    emit(c, &t);
    return reached(c, SpeakerStBuffer);
}

static void close_graphs(SpeakerCore *c)
{
    if (c->dead) return;
    if (c->graphs & 2u) graph(c, AR_OP_GRAPH_CLOSE, AR_SG_PLAY);
    if (c->graphs & 1u) graph(c, AR_OP_GRAPH_CLOSE, AR_SG_SPEAKER);
    c->graphs = 0;
}

static int stage_graphs(SpeakerCore *c)
{
    const ArFormat stream = {SpeakerRate, 16, SpeakerChannels};
    const ArFormat device = {SpeakerRate, (uint16_t)c->config.device_bits, SpeakerChannels};
    const char *step = "open device";
    int rc = command(c, pagani_ar_open_speaker_device(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token), AR_OP_GRAPH_OPEN);
    if (!rc) {
        c->graphs |= 1u;
        step = "configure device";
        rc = command(c, pagani_ar_config_speaker_device(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token, &device,
                                                        c->config.sd_line), AR_OP_SET_CFG);
    }
    if (!rc) {
        step = "open stream";
        rc = command(c, pagani_ar_open_playback_stream(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token), AR_OP_GRAPH_OPEN);
    }
    if (!rc) {
        c->graphs |= 2u;
        step = "configure stream";
        rc = command(c, pagani_ar_config_playback_stream(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token, &stream, &device),
                     AR_OP_SET_CFG);
    }
    if (!rc) {
        step = "configure shared memory";
        rc = command(c, pagani_ar_config_playback_shmem(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token, &stream), AR_OP_SET_CFG);
    }
    if (!rc) {
        step = "map buffer";
        rc = command(c, pagani_ar_map(c->packet, sizeof c->packet, SpeakerGprPort, ++c->token, c->dsp_address, SPEAKER_BUFFER_BYTES),
                     AR_OP_MEM_MAP);
    }
    if (rc) {
        const uint32_t op = c->last_opcode, status = c->last_status;
        close_graphs(c);
        c->last_opcode = op;
        c->last_status = status;
        return fail(c, SpeakerStGraphs, rc, step);
    }
    Text t = line(c, "graphs");
    t_kx(&t, "handle", c->handle);
    t_kd(&t, "bits", c->config.device_bits);
    t_ks(&t, "sd", c->config.sd_line == AR_I2S_SD1 ? "sd1" : "sd0");
    t_kd(&t, "prm_port", c->config.prm_port);
    t_kd(&t, "clock", c->config.prm_clock ? (int64_t)clock_hz(c) : 0);
    emit(c, &t);
    c->ready = 1;
    return reached(c, SpeakerStGraphs);
}

int speaker_core_bringup(SpeakerCore *c)
{
    Text t = line(c, "start");
    t_kx(&t, "amps", c->config.amps);
    t_kd(&t, "bits", c->config.device_bits);
    t_ks(&t, "sd", c->config.sd_line == AR_I2S_SD1 ? "sd1" : "sd0");
    t_kd(&t, "prm_port", c->config.prm_port);
    t_kd(&t, "clock", c->config.prm_clock);
    t_kd(&t, "feedback", c->config.feedback);
    t_kd(&t, "rail", c->config.rail);
    t_kx(&t, "sids", c->config.sids);
    t_kx(&t, "pa", c->config.buffer_pa);
    t_kd(&t, "stop", c->config.stop_after);
    t_kd(&t, "ceil_db", -(int64_t)c->config.ceiling_db);
    t_kd(&t, "ceil", c->limit.ceiling);
    emit(c, &t);
    guard(c, "i2c");
    const int amps = stage_pins(c) && stage_rail(c) && stage_bus(c) && stage_amps(c) && stage_amp_config(c);
    let_go(c);
    if (!amps || !stage_dsp(c) || !stage_apm(c)) return 0;
    guard(c, "smmu");
    const int graphs = stage_smmu(c) && stage_buffer(c) && stage_graphs(c);
    let_go(c);
    return graphs;
}
