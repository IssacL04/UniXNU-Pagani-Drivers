/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The whole speaker path on the host: SpeakerCore.c and SpeakerBackend.c against the SoC models of model.h (pins, clocks, the serial
 * engine, two TFA9873) and a model of the DSP (GPR/APM/PRM answers, graph states, the I2S bit clock, a shared memory endpoint that
 * takes buffers into a queue of two and plays one every 10 ms), with PaganiAudio's real engine (../PaganiAudio/PaganiAudioEngine.c) as
 * the iOS service. Checked: every stage and its log, every packet field the DSP reads, the order of clocks and amplifiers, the shared
 * GCC vote register written under its lock word, the tone's frequency, level, fades and length as the DSP played it, the tone walk
 * (both speakers, then each alone, a second apart), the iOS ring arriving at the DSP frame by frame after the limiter's 1 ms
 * look-ahead, the digital ceiling under full-scale input (tone at 0 dB, a 0 dBFS sine and a full-scale square from iOS: nothing above
 * -12 dBFS, no clamp, the gain back at unity and the signal bit for bit afterwards), the ceiling's argument, stop/restart with stale
 * answers, silent capture, and the failure paths (stop after each stage, no DSP, DSP failed, APM never ready, SMMU, buffer, refused
 * graphs and map, absent and misconfigured amplifiers, bad engine, stuck pins, PRM refused, START refused, full queue, garbage, a
 * stalled DSP with its restarts and the fallback to the silent backend, the channel closing mid-run). */
#include "model.h"
#include "PaganiAdsp.h"
#include "PaganiAudioEngine.h"
#include "SpeakerBackend.h"
#include "SpeakerCore.h"

typedef struct {uint8_t data[GPR_MAX_PACKET]; uint32_t length;} Packet;
typedef struct {uint32_t token; uint64_t address;} Queued;

typedef struct {
    int adsp_off;
    int registered, up, down;
    uint64_t up_at, down_at;               /* us; ~0 never */
    int32_t stage;
    uint32_t crashed;
    int full_sends;
    Packet inbox[64];
    unsigned in_head, in_count;
    Packet out[128];
    unsigned out_head, out_count;
    unsigned spf_not_ready, spf_queries;
    int spf_silent;
    unsigned open, prepared, started;      /* bit 0 device graph, bit 1 stream graph */
    uint32_t refuse_op, refuse_sg, refuse_status;
    uint32_t sd_line, ws, ep_rate, ep_bits, ep_channels, ep_format, factor;
    uint32_t dec_bits, dec_q, dec_inter, cnv_bits, cnv_q, cnv_inter, cnv_channels, shm_rate, shm_bits, shm_channels;
    unsigned configs, links_ok, closes;
    uint32_t handle;
    uint64_t map_addr;
    uint32_t map_bytes;
    int refuse_map;
    unsigned prm_requests, prm_releases;
    uint32_t prm_id, prm_hz, prm_attr, prm_root, prm_port, prm_refuse;
    Packet pending[32];
    unsigned pend_head, pend_count;
    Queued queue[2];
    unsigned q_count;
    uint64_t next_tick;
    int stall, return_on_stop;
    int16_t *played;
    uint64_t played_frames, capacity;
    unsigned underruns, writes, returned, errors;
    char error[160];
    uint64_t start_dev_us, stop_dev_us;
} Dsp;

typedef struct {
    Model m;
    Dsp d;
    SpeakerCore c;
    SpeakerOps ops;
    int rail_rc;
    unsigned rail_calls;
    int smmu_rc[4];
    unsigned smmu_calls;
    uint16_t smmu_sids[8];
    int no_buffer;
    uint8_t buffer[SPEAKER_BUFFER_BYTES];
    char lines[96][200];
    unsigned line_count, publishes;
} Sim;

static int gVerbose;
static PaganiAudioEngine gEngine;
static int gEngineLive;

/* PaganiAudio.cpp's two entry points the backend uses: the real service is not linked into this test. */
int pagani_audio_set_backend(const PaganiAudioBackend *backend)
{
    if (!gEngineLive || !backend) return -1;
    pagani_audio_engine_set_backend(&gEngine, backend);
    return 0;
}
void pagani_audio_wake(void) {}

/* ---- the DSP model ---- */

#define DSP_ERROR(s, ...) do {(s)->d.errors++; snprintf((s)->d.error, sizeof (s)->d.error, __VA_ARGS__);} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}

static void answer(Sim *s, const GprPacket *q, uint32_t opcode, const uint32_t *words, unsigned n)
{
    CHECK(s->d.out_count < 128);
    GprPacket p;
    memset(&p, 0, sizeof p);
    p.dst_domain = GPR_DOMAIN_APPS; p.src_domain = GPR_DOMAIN_ADSP; p.src_port = q->dst_port; p.dst_port = q->src_port;
    p.token = q->token; p.opcode = opcode;
    Packet *o = &s->d.out[(s->d.out_head + s->d.out_count++) % 128];
    o->length = pagani_gpr_encode(o->data, sizeof o->data, &p, words, 4 * n);
    CHECK(o->length);
}
static void basic(Sim *s, const GprPacket *q, uint32_t status)
{
    const uint32_t w[2] = {q->opcode, status};
    answer(s, q, GPR_OP_BASIC_RESULT, w, 2);
}
static int refused(Sim *s, const GprPacket *q, uint32_t sg)
{
    if (!s->d.refuse_op || s->d.refuse_op != q->opcode || (s->d.refuse_sg && s->d.refuse_sg != sg)) return 0;
    basic(s, q, s->d.refuse_status);
    return 1;
}
static unsigned sg_bit(uint32_t sg) {return sg == AR_SG_SPEAKER ? 1u : sg == AR_SG_PLAY ? 2u : 0u;}

static void write_done(Sim *s, const Queued *b, uint32_t status)
{
    GprPacket q;
    memset(&q, 0, sizeof q);
    q.src_port = SpeakerGprPort; q.dst_port = AR_IID_PLAY_SHMEM; q.token = b->token;
    const uint32_t w[8] = {(uint32_t)b->address, (uint32_t)(b->address >> 32), s->d.handle, status, 0, 0, 0, 0};
    answer(s, &q, AR_OP_WRITE_DONE, w, 8);
}

/* Pending writes go into the queue of two while the stream runs; each is copied (read through the SMMU) and answered then. */
static void accept(Sim *s)
{
    Dsp *d = &s->d;
    while (d->pend_count && d->q_count < 2 && !d->stall && (d->started & 3u) == 3u) {
        const Packet *p = &d->pending[d->pend_head];
        d->pend_head = (d->pend_head + 1) % 32;
        d->pend_count--;
        GprPacket g;
        CHECK(pagani_gpr_decode(p->data, p->length, &g) == GprOk);
        Queued b = {g.token, le32(g.payload) | (uint64_t)le32(g.payload + 4) << 32};
        d->queue[d->q_count++] = b;
        write_done(s, &b, 0);
    }
}

/* One period of the I2S sink: the oldest queued buffer is played (its frames copied out of the shared buffer now). */
static void tick(Sim *s)
{
    Dsp *d = &s->d;
    if ((d->started & 3u) != 3u) return;
    if (!d->q_count) {d->underruns++; return;}
    const Queued b = d->queue[0];
    d->queue[0] = d->queue[1];
    d->q_count--;
    const uint64_t offset = (b.address & 0xffffffffu) - (d->map_addr & 0xffffffffu);
    CHECK(offset + 1920 <= SPEAKER_BUFFER_BYTES);
    if (d->played_frames + 480 <= d->capacity) {
        memcpy(d->played + d->played_frames * 2, s->buffer + offset, 1920);
        d->played_frames += 480;
    }
    accept(s);
}

static void set_cfg(Sim *s, const GprPacket *q)
{
    Dsp *d = &s->d;
    const uint8_t *p = q->payload;
    if (refused(s, q, 0)) return;
    if (q->payload_bytes < 16 || le32(p + 12) != q->payload_bytes - 16) {DSP_ERROR(s, "set_cfg header"); basic(s, q, 2); return;}
    for (uint32_t at = 16; at + 16 <= q->payload_bytes;) {
        const uint32_t iid = le32(p + at), id = le32(p + at + 4), size = le32(p + at + 8);
        const uint8_t *b = p + at + 16;
        if (at + 16 + size > q->payload_bytes) {DSP_ERROR(s, "param overruns"); basic(s, q, 2); return;}
        if (id == 0x08001019 && iid == AR_IID_SPEAKER_I2S) {
            if (le32(b) != 0 || le32(b + 4) != 1) DSP_ERROR(s, "i2s interface");
            d->sd_line = le16(b + 8); d->ws = le16(b + 10);
        } else if (id == 0x08001017 && iid == AR_IID_SPEAKER_I2S) {
            d->ep_rate = le32(b); d->ep_bits = le16(b + 4); d->ep_channels = le16(b + 6); d->ep_format = le32(b + 8);
        } else if (id == 0x08001018 && iid == AR_IID_SPEAKER_I2S) {
            d->factor = le32(b);
        } else if (id == 0x08001008 && (iid == AR_IID_PLAY_DEC || iid == AR_IID_PLAY_CNV)) {
            if (le32(b) != 1 || le32(b + 4) != 0x09001000) DSP_ERROR(s, "pcm output header");
            const uint32_t bits = le16(b + 12), q15 = le16(b + 18), inter = le16(b + 22), channels = le16(b + 26);
            if (le16(b + 16) != bits || le16(b + 14) != 1 || le16(b + 20) != 1 || b[28] != 1 || (channels == 2 && b[29] != 2))
                DSP_ERROR(s, "pcm output body");
            if (iid == AR_IID_PLAY_DEC) {d->dec_bits = bits; d->dec_q = q15; d->dec_inter = inter;}
            else {d->cnv_bits = bits; d->cnv_q = q15; d->cnv_inter = inter; d->cnv_channels = channels;}
        } else if (id == 0x0800100C && iid == AR_IID_PLAY_SHMEM) {
            if (q->dst_port != AR_IID_PLAY_SHMEM) DSP_ERROR(s, "media format not sent to the endpoint");
            d->shm_rate = le32(b + 12); d->shm_bits = le16(b + 16); d->shm_channels = le16(b + 26);
            if (le16(b + 22) != d->shm_bits - 1 || b[28] != 1 || b[29] != 2) DSP_ERROR(s, "media format body");
        } else {
            DSP_ERROR(s, "unexpected parameter %x for %x", id, iid);
        }
        at += 16 + size;
    }
    d->configs++;
    basic(s, q, 0);
}

static void graph_open(Sim *s, const GprPacket *q)
{
    Dsp *d = &s->d;
    const uint8_t *p = q->payload;
    const uint32_t sg = le32(p + 16 + 16 + 4);
    if (refused(s, q, sg)) return;
    if (!sg_bit(sg) || (d->open & sg_bit(sg))) {DSP_ERROR(s, "open of %x", sg); basic(s, q, 0x12); return;}
    /* the stream graph's connections: shmem -> dec -> cnv -> the device graph's I2S sink, which must be open */
    if (sg == AR_SG_PLAY) {
        uint32_t at = 16;
        for (int i = 0; i < 4; ++i) at += 16 + le32(p + at + 8);
        const uint8_t *c = p + at + 16;
        if (le32(p + at + 4) == 0x08001004 && le32(c) == 3 && le32(c + 4) == AR_IID_PLAY_SHMEM && le32(c + 36) == AR_IID_PLAY_CNV &&
            le32(c + 44) == AR_IID_SPEAKER_I2S)
            d->links_ok++;
        if (!(d->open & 1u)) {basic(s, q, 0x0f); return;}   /* the sink is not there */
    }
    d->open |= sg_bit(sg);
    basic(s, q, 0);
}

static void graph_state(Sim *s, const GprPacket *q)
{
    Dsp *d = &s->d;
    const uint32_t sg = le32(q->payload + 16 + 16 + 4), bit = sg_bit(sg);
    if (refused(s, q, sg)) return;
    if (!bit || !(d->open & bit)) {DSP_ERROR(s, "state of %x", sg); basic(s, q, 0x12); return;}
    switch (q->opcode) {
    case AR_OP_GRAPH_PREPARE: d->prepared |= bit; break;
    case AR_OP_GRAPH_START:
        if (!(d->prepared & bit)) DSP_ERROR(s, "start before prepare");
        if (bit == 2u && !(d->started & 1u)) DSP_ERROR(s, "stream started before the device");
        d->started |= bit;
        if (bit == 1u) {s->m.bit_clock = 1; d->start_dev_us = s->m.us;}
        if ((d->started & 3u) == 3u) d->next_tick = s->m.us + 10000;
        break;
    case AR_OP_GRAPH_STOP:
        if (bit == 1u && (d->started & 2u)) DSP_ERROR(s, "device stopped before the stream");
        d->started &= ~bit;
        d->prepared &= ~bit;
        if (bit == 1u) {s->m.bit_clock = 0; d->stop_dev_us = s->m.us;}
        if (bit == 2u) {
            /* the endpoint drops what it queued (answered already) and gives back the buffers it had not taken */
            while (d->pend_count) {
                GprPacket g;
                const Packet *pp = &d->pending[d->pend_head];
                CHECK(pagani_gpr_decode(pp->data, pp->length, &g) == GprOk);
                const Queued b = {g.token, le32(g.payload) | (uint64_t)le32(g.payload + 4) << 32};
                if (d->return_on_stop) {write_done(s, &b, 0); d->returned++;}
                d->pend_head = (d->pend_head + 1) % 32;
                d->pend_count--;
            }
            d->q_count = 0;
        }
        break;
    case AR_OP_GRAPH_CLOSE:
        if (d->started & bit) DSP_ERROR(s, "close of a running graph");
        d->open &= ~bit;
        d->closes++;
        break;
    }
    basic(s, q, 0);
}

static void process(Sim *s, const Packet *pk)
{
    Dsp *d = &s->d;
    GprPacket q;
    if (pagani_gpr_decode(pk->data, pk->length, &q) != GprOk || q.dst_domain != GPR_DOMAIN_ADSP || q.src_domain != GPR_DOMAIN_APPS) {
        DSP_ERROR(s, "bad packet");
        return;
    }
    switch (q.opcode) {
    case GPR_OP_APM_GET_SPF_STATE: {
        d->spf_queries++;
        if (d->spf_silent) return;
        uint32_t state = 1;
        if (d->spf_not_ready) {d->spf_not_ready--; state = 0;}
        answer(s, &q, GPR_OP_APM_SPF_STATE, &state, 1);
        return;
    }
    case AR_OP_GRAPH_OPEN: graph_open(s, &q); return;
    case AR_OP_SET_CFG: set_cfg(s, &q); return;
    case AR_OP_GRAPH_PREPARE: case AR_OP_GRAPH_START: case AR_OP_GRAPH_STOP: case AR_OP_GRAPH_CLOSE: graph_state(s, &q); return;
    case AR_OP_MEM_MAP: {
        const uint64_t address = le32(q.payload + 8) | (uint64_t)le32(q.payload + 12) << 32;
        if (le16(q.payload) != 3 || le16(q.payload + 2) != 1) DSP_ERROR(s, "map pool");
        if ((address >> 32) != 1 || (address & 0xffffffffu) != (s->c.buffer_pa & 0xffffffffu)) DSP_ERROR(s, "map address %llx", (unsigned long long)address);
        if (d->refuse_map) {basic(s, &q, 1); return;}
        d->map_addr = address; d->map_bytes = le32(q.payload + 16); d->handle = 0x77;
        answer(s, &q, AR_OP_MEM_MAP_DONE, &d->handle, 1);
        return;
    }
    case AR_OP_PRM_REQUEST: case AR_OP_PRM_RELEASE: {
        if (q.dst_port != AR_PRM_IID) DSP_ERROR(s, "prm port");
        d->prm_port = q.src_port;
        d->prm_id = le32(q.payload + 16 + 16 + 4);
        if (q.opcode == AR_OP_PRM_REQUEST) {
            d->prm_requests++;
            d->prm_hz = le32(q.payload + 40); d->prm_attr = le32(q.payload + 44); d->prm_root = le32(q.payload + 48);
        } else {
            d->prm_releases++;
        }
        const uint32_t w[2] = {q.opcode, d->prm_refuse};
        answer(s, &q, q.opcode == AR_OP_PRM_REQUEST ? AR_OP_PRM_REQUEST_DONE : AR_OP_PRM_RELEASE_DONE, w, 2);
        return;
    }
    case AR_OP_WRITE: {
        d->writes++;
        const uint64_t address = le32(q.payload) | (uint64_t)le32(q.payload + 4) << 32;
        if (q.dst_port != AR_IID_PLAY_SHMEM || le32(q.payload + 8) != d->handle || le32(q.payload + 12) != 1920 || (address >> 32) != 1 ||
            (address & 0xffffffffu) < (d->map_addr & 0xffffffffu) || (address & 0xffffffffu) + 1920 > (d->map_addr & 0xffffffffu) + d->map_bytes)
            DSP_ERROR(s, "write fields");
        if (!(d->started & 2u)) DSP_ERROR(s, "write to a stopped stream");
        CHECK(d->pend_count < 32);
        d->pending[(d->pend_head + d->pend_count++) % 32] = *pk;
        accept(s);
        return;
    }
    default:
        DSP_ERROR(s, "opcode %x", q.opcode);
    }
}

/* ---- the platform operations ---- */

static uint64_t op_now(void *ctx) {return ((Sim *)ctx)->m.us / 1000u;}
static uint64_t op_ticks(void *ctx) {return ((Sim *)ctx)->m.us * 192u / 10u;}    /* 19.2 MHz */
static void op_trace(void *ctx, const char *line)
{
    Sim *s = (Sim *)ctx;
    CHECK(strlen(line) < 200 && !strncmp(line, "[spk] ", 6));
    if (s->line_count < 96) strcpy(s->lines[s->line_count++], line);
    if (gVerbose) printf("%8llu %s\n", (unsigned long long)(s->m.us / 1000u), line);
}
/* The UFS log's writer as the stretches hold it: a watched run checks each first touch. */
static int gQuietWatch, gQuietHeld;
static unsigned gQuietHolds;
static void op_quiet(void *ctx, unsigned on)
{
    (void)ctx;
    CHECK(gQuietHeld != (int)on);
    gQuietHeld = (int)on;
    gQuietHolds += on;
}
static void quiet_touch(int wanted) {CHECK(!gQuietWatch || gQuietHeld == wanted);}
static int op_channel(void *ctx)
{
    Sim *s = (Sim *)ctx;
    quiet_touch(0);
    if (s->d.adsp_off) return PaganiAdspOff;
    CHECK(!s->d.registered);
    s->d.registered = 1;
    return 0;
}
static int op_send(void *ctx, const uint8_t *packet, uint32_t length)
{
    Sim *s = (Sim *)ctx;
    CHECK(s->d.registered && length && length <= GPR_MAX_PACKET);
    if (s->d.down) return PaganiAdspDown;
    if (s->d.full_sends > 0) {s->d.full_sends--; return PaganiAdspFull;}
    CHECK(s->d.up && s->d.in_count < 64);
    Packet *p = &s->d.inbox[(s->d.in_head + s->d.in_count++) % 64];
    memcpy(p->data, packet, length);
    p->length = length;
    return PaganiAdspOk;
}
static int op_dsp_state(void *ctx, int32_t *stage, uint32_t *crashed)
{
    Sim *s = (Sim *)ctx;
    if (s->d.adsp_off) return PaganiAdspOff;
    *stage = s->d.stage;
    *crashed = s->d.crashed;
    return 0;
}
static int op_rail(void *ctx, uint32_t mv, uint32_t *detail)
{
    Sim *s = (Sim *)ctx;
    quiet_touch(1);
    CHECK(mv == 1800);
    s->rail_calls++;
    *detail = 0x41e00;
    return s->rail_rc;
}
static int op_smmu(void *ctx, uint16_t sid, uint64_t pa, uint32_t bytes, uint32_t *detail)
{
    Sim *s = (Sim *)ctx;
    static const uint16_t sids[4] = SPEAKER_SIDS;
    quiet_touch(1);
    CHECK(pa == s->c.buffer_pa && bytes == SPEAKER_BUFFER_BYTES && s->smmu_calls < 8);
    s->smmu_sids[s->smmu_calls++] = sid;
    for (int i = 0; i < 4; ++i) if (sids[i] == sid) {*detail = (uint32_t)i << 8 | 0x7f; return s->smmu_rc[i];}
    CHECK(0);
    return -1;
}
static uint8_t *op_buffer(void *ctx, uint64_t pa, uint32_t bytes)
{
    Sim *s = (Sim *)ctx;
    CHECK(pa == s->c.buffer_pa && bytes == SPEAKER_BUFFER_BYTES);
    return s->no_buffer ? NULL : s->buffer;
}
static void op_publish(void *ctx) {((Sim *)ctx)->publishes++;}

/* Delivers what the DSP has to say: 1 when anything was delivered. */
static int deliver(Sim *s)
{
    Dsp *d = &s->d;
    int any = 0;
    if (!d->up && !d->down && d->registered && s->m.us >= d->up_at) {d->up = 1; speaker_core_state(&s->c, 1); any = 1;}
    if (d->up && !d->down && s->m.us >= d->down_at) {d->down = 1; d->up = 0; speaker_core_state(&s->c, 0); any = 1;}
    while (d->in_count && !d->down) {
        const Packet p = d->inbox[d->in_head];
        d->in_head = (d->in_head + 1) % 64;
        d->in_count--;
        process(s, &p);
    }
    while (d->out_count && !d->down) {
        const Packet *p = &d->out[d->out_head];
        speaker_core_receive(&s->c, p->data, p->length);
        d->out_head = (d->out_head + 1) % 128;
        d->out_count--;
        any = 1;
    }
    return any;
}
static void op_wait(void *ctx, uint32_t ms)
{
    Sim *s = (Sim *)ctx;
    for (uint32_t i = 0;; ++i) {
        if (deliver(s)) return;
        if (i >= ms) return;
        s->m.us += 1000;
        if ((s->d.started & 3u) == 3u && s->m.us >= s->d.next_tick) {
            s->d.next_tick += 10000;
            tick(s);
        }
    }
}

static void sim_init(Sim *s, const SpeakerConfig *k)
{
    int16_t *played = s->d.played;
    memset(s, 0, sizeof *s);
    model_init(&s->m);
    s->m.us = 5000000;                                        /* boot time: 5 s */
    s->d.up_at = s->m.us + 3000000;                           /* the DSP opens adsp_apps 3 s after registration */
    s->d.down_at = ~0ull;
    s->d.stage = PaganiAdspStGlink;
    s->d.spf_not_ready = 2;
    s->d.return_on_stop = 1;
    s->d.capacity = 48000ull * 40;
    s->d.played = played ? played : calloc(s->d.capacity * 2, sizeof(int16_t));
    CHECK(s->d.played);
    s->ops.context = s;
    s->ops.now_ms = op_now;
    s->ops.wait = op_wait;
    s->ops.trace = op_trace;
    s->ops.channel = op_channel;
    s->ops.send = op_send;
    s->ops.dsp_state = op_dsp_state;
    s->ops.rail = op_rail;
    s->ops.smmu = op_smmu;
    s->ops.buffer = op_buffer;
    s->ops.publish = op_publish;
    s->ops.io = model_io(&s->m);
    speaker_core_init(&s->c, &s->ops, k);
}

static SpeakerConfig defaults(void)
{
    SpeakerConfig k;
    memset(&k, 0, sizeof k);
    k.prm_clock = 1; k.i2c_pull = 1; k.rail = 1; k.sids = 0xf;
    return k;
}

static int logged(const Sim *s, const char *text)
{
    for (unsigned i = 0; i < s->line_count; ++i) if (strstr(s->lines[i], text)) return 1;
    return 0;
}

static int line_of(const Sim *s, const char *text)
{
    for (unsigned i = 0; i < s->line_count; ++i) if (strstr(s->lines[i], text)) return (int)i;
    return -1;
}

/* ---- scenarios ---- */

/* The stretches around first touches (SpeakerCore.c): each opened by its line before the touch, the writer held at the touch, let go at its end,
 * at a stop and after a failure; without the UFS log (crumb 0) nothing is held; no quiet op, not even the lines. */
static void stretches(void)
{
    static Sim s;
    SpeakerConfig k = defaults();
    k.crumb_ms = 3500;
    sim_init(&s, &k);
    s.ops.quiet = op_quiet;
    gQuietWatch = 1;
    gQuietHolds = 0;
    const uint64_t began = s.m.us;
    CHECK(speaker_core_bringup(&s.c) == 1 && !gQuietHeld && gQuietHolds == 2 && !s.c.held);
    CHECK(s.m.us - began >= 2 * (3500 + SPEAKER_QUIET_MS) * 1000ull);
    CHECK(line_of(&s, "[spk] guard i2c") >= 0 && line_of(&s, "[spk] guard i2c") < line_of(&s, "[spk] pins"));
    CHECK(line_of(&s, "[spk] dsp up") < line_of(&s, "[spk] guard smmu") && line_of(&s, "[spk] guard smmu") < line_of(&s, "[spk] smmu start"));
    /* the first tone of the boot: held from before its bit clock to its end, one hold for the whole walk's first tone */
    gQuietWatch = 0;
    const uint64_t tone_at = s.m.us;
    CHECK(speaker_core_tones(&s.c, 2, 440, 100, 30) == 0 && !gQuietHeld && gQuietHolds == 3);
    CHECK(s.m.us - tone_at >= (3500 + SPEAKER_QUIET_MS) * 1000ull && logged(&s, "[spk] guard tone"));
    CHECK(line_of(&s, "[spk] guard tone") < line_of(&s, "[spk] play start"));
    /* a stop inside a stretch lets go, and a failure inside one */
    static const uint32_t stops[] = {SpeakerStPins, SpeakerStAmps, SpeakerStAmpConfig, SpeakerStSmmu, SpeakerStBuffer, SpeakerStGraphs};
    for (unsigned i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
        k.stop_after = stops[i];
        sim_init(&s, &k);
        s.ops.quiet = op_quiet;
        CHECK(speaker_core_bringup(&s.c) == 0 && !gQuietHeld && !s.c.held && s.c.stage == (int32_t)stops[i]);
    }
    k.stop_after = 0;
    sim_init(&s, &k);
    s.ops.quiet = op_quiet;
    s.smmu_rc[0] = -16;
    CHECK(speaker_core_bringup(&s.c) == 0 && !gQuietHeld && !s.c.held && s.c.stage == -SpeakerStSmmu);
    /* no UFS log: the lines, no hold and no wait */
    k.crumb_ms = 0;
    sim_init(&s, &k);
    s.ops.quiet = op_quiet;
    gQuietHolds = 0;
    CHECK(speaker_core_bringup(&s.c) == 1 && gQuietHolds == 0 && logged(&s, "[spk] guard smmu"));
    /* no quiet op: not even the lines (the rest of this file) */
    sim_init(&s, &k);
    CHECK(speaker_core_bringup(&s.c) == 1 && !logged(&s, "[spk] guard"));
    free(s.d.played);
}

static void bringup(Sim *s)
{
    const SpeakerConfig k = defaults();
    sim_init(s, &k);
    CHECK(speaker_core_bringup(&s->c) == 1 && s->c.ready && s->c.stage == SpeakerStGraphs && s->d.errors == 0);
    CHECK(s->c.amps_found == 3 && s->c.amps_ready == 3 && s->c.rev[0] == 0x0b73 && s->c.rev[1] == 0x0b73);
    CHECK(s->m.amp[0].key1_ok && s->m.amp[1].key2_ok && !(s->m.amp[0].reg[0x00] & 8) && (s->m.amp[0].reg[0x00] & 1));
    CHECK((s->m.amp[0].reg[0x26] & 0xf) == 0 && (s->m.amp[1].reg[0x26] & 0xf) == 1 && !(s->m.amp[0].reg[0x23] & 0x18));
    CHECK(s->rail_calls == 1 && s->smmu_calls == 4 && s->smmu_sids[0] == 0x1001 && s->smmu_sids[3] == 0x1061 && s->c.sid_mask == 0xf);
    CHECK(s->d.spf_queries == 3 && s->d.open == 3 && s->d.links_ok == 1 && s->d.configs == 3 && s->d.handle == 0x77 && s->c.handle == 0x77);
    CHECK(s->d.sd_line == AR_I2S_SD0 && s->d.ws == 1 && s->d.ep_rate == 48000 && s->d.ep_bits == 32 && s->d.ep_channels == 2 &&
          s->d.ep_format == 1 && s->d.factor == 1);
    CHECK(s->d.dec_bits == 16 && s->d.dec_q == 15 && s->d.dec_inter == 1 && s->d.cnv_bits == 32 && s->d.cnv_q == 31 && s->d.cnv_inter == 3);
    CHECK(s->d.shm_rate == 48000 && s->d.shm_bits == 16 && s->d.shm_channels == 2 && s->d.map_bytes == 0x10000);
    CHECK(s->c.dsp_address == 0x1d0000000ull && s->m.ctl[SpeakerPinSck] == 1u && (s->m.ctl[SpeakerPinSda] & SpeakerCtlI2cPull));
    CHECK(logged(s, "[spk] dsp up") && logged(s, "[spk] apm state=ready queries=3") && logged(s, "config ok") &&
          logged(s, "[spk] graphs handle=0x77 bits=32 sd=sd0 prm_port=2 clock=3072000"));
    /* the shared GCC vote register written once under the lock word ('SPKR'), which is free again; the ceiling at -12 dBFS */
    CHECK(logged(s, "[spk] bus clk=0 vote=0x4180 lock=1 waits=0") && s->m.vote_writes == 1 && s->m.vote_owner_at_write == SPEAKER_GCC_OWNER);
    CHECK(sm8750_gcc_vote_owner == 0 && logged(s, "ceil_db=-12 ceil=8231") && s->c.limit.ceiling == 8231 && s->c.limit.knee == 5827);
}

static void tone(void)
{
    static Sim s;
    bringup(&s);
    const uint64_t frames0 = s.d.played_frames;
    CHECK(speaker_core_tone(&s.c, 440, 500, 30) == 0 && s.c.stage == SpeakerStTone && !s.c.running && s.d.errors == 0);
    /* The clock and the graphs around the run, the amplifiers inside it. */
    CHECK(s.d.prm_requests == 1 && s.d.prm_releases == 1 && s.d.prm_id == 0x102 && s.d.prm_hz == 3072000 && s.d.prm_attr == 1 &&
          s.d.prm_root == 0 && s.d.prm_port == 2);
    CHECK(s.d.started == 0 && s.d.open == 3 && !s.m.bit_clock && s.c.amps_on == 0);
    for (int i = 0; i < 2; ++i) {
        CHECK(s.m.amp[i].power_ups == 1 && s.m.amp[i].power_downs == 1);
        CHECK(s.m.amp[i].ampe_on_us > s.d.start_dev_us && s.m.amp[i].ampe_off_us < s.d.stop_dev_us);
        CHECK((s.m.amp[i].reg[0x00] & 1) && (s.m.amp[i].reg[0x01] & 4));   /* powered down, configuration kept */
    }
    CHECK(s.m.ctl[SpeakerPinSck] == 1u && s.m.ctl[SpeakerPinData0] == 1u);   /* pins back asleep */
    /* What the DSP played: 0.5 s of 440 Hz at 30 dB below full scale with 10 ms fades, then silence. */
    const int16_t *p = s.d.played + frames0 * 2;
    const uint64_t n = s.d.played_frames - frames0;
    CHECK(n >= 24000 + 8 * 480 - 2 * 480);
    uint64_t first = n, last = 0;
    unsigned peak = 0, crossings = 0;
    for (uint64_t i = 0; i < n; ++i) {
        CHECK(p[2 * i] == p[2 * i + 1]);
        if (p[2 * i]) {if (first == n) first = i; last = i;}
        if ((unsigned)abs(p[2 * i]) > peak) peak = (unsigned)abs(p[2 * i]);
        if (i && (p[2 * i - 2] < 0) != (p[2 * i] < 0)) ++crossings;
    }
    /* one 1 ms block of look-ahead first; below the ceiling's knee the tone is carried bit for bit */
    CHECK(first >= SPEAKER_LIMIT_BLOCK && first < 480 && last - first >= 23900 && last - first < 24000 && peak >= 1030 && peak <= 1036);
    CHECK(crossings >= 436 && crossings <= 442);
    CHECK(logged(&s, "[spk] tone rc=0 hz=440 ms=500 atten=30 gain=1036 periods=50"));
    CHECK(logged(&s, "clks_failed=0 ch=0x3 out=") && logged(&s, " ceil=8231") && logged(&s, "lim=0 gmin=1000 in="));
    CHECK(s.c.limit.peak_in == peak && s.c.limit.peak_out == peak && s.c.limit.limited == 0 && s.c.limit.clamps == 0);
    CHECK(logged(&s, "[spk] amp 0x34 state=9 clks=1 plls=1 sws=1 ampen=1 noclk=0 tdmerr=0") && logged(&s, "[spk] play start run=1 amps=0x3"));
    /* At full scale (atten 0) the tone meets the ceiling: nothing played above -12 dBFS, the final clamp never cut. */
    const uint64_t frames1 = s.d.played_frames;
    CHECK(speaker_core_tone(&s.c, 440, 300, 0) == 0 && s.c.limit.clamps == 0 && s.c.limit.peak_in >= 32760);
    unsigned loud = 0;
    for (uint64_t i = frames1; i < s.d.played_frames; ++i) if ((unsigned)abs(s.d.played[2 * i]) > loud) loud = (unsigned)abs(s.d.played[2 * i]);
    CHECK(loud <= 8231 && loud >= 8150 && s.c.limit.peak_out == loud && logged(&s, "atten=0 gain=32767"));
    /* The next run starts from silence at unity gain: nothing of the loud one is held into it, its statistics are its own. */
    CHECK(speaker_core_tone(&s.c, 440, 100, 30) == 0 && s.c.limit.peak_in <= 1036 && s.c.limit.limited == 0 && s.c.limit.peak_out == s.c.limit.peak_in);
    free(s.d.played);
}

/* Separates the played frames from from on into bursts of sound (gaps of at least a period of silence); per burst whether the left
 * (slot 0, 0x34) and the right (slot 1, 0x35) channels carried anything. Returns the number of bursts. */
static unsigned bursts(const Sim *s, uint64_t from, unsigned max, int left[], int right[], uint64_t length[])
{
    unsigned n = 0, quiet = 480;
    uint64_t start = 0;
    for (uint64_t i = from; i < s->d.played_frames; ++i) {
        const int16_t l = s->d.played[2 * i], r = s->d.played[2 * i + 1];
        if (!l && !r) {
            ++quiet;
            continue;
        }
        if (quiet >= 480) {
            CHECK(n < max);
            left[n] = right[n] = 0;
            start = i;
            n++;
        }
        quiet = 0;
        if (l) left[n - 1] = 1;
        if (r) right[n - 1] = 1;
        length[n - 1] = i + 1 - start;
    }
    return n;
}

/* pagani-audio-tone=2: the walk, both speakers, then 0x34 alone, then 0x35 alone, a second apart; =1: one tone on both. */
static void walk(void)
{
    static Sim s;
    bringup(&s);
    const uint64_t f0 = s.d.played_frames, t0 = s.m.us;
    CHECK(speaker_core_tones(&s.c, 2, 1000, 200, 20) == 0 && s.c.runs == 3 && s.c.stage == SpeakerStTone && s.d.errors == 0);
    CHECK(s.m.us - t0 >= 2000000 + 3 * 200000);
    CHECK(logged(&s, "[spk] tone walk: 1 both speakers, 2 amplifier 0x34 alone") && logged(&s, "ch=0x3") && logged(&s, "ch=0x1") &&
          logged(&s, "ch=0x2"));
    int left[4], right[4];
    uint64_t length[4];
    CHECK(bursts(&s, f0, 4, left, right, length) == 3);
    CHECK(left[0] && right[0] && left[1] && !right[1] && !left[2] && right[2]);
    for (int i = 0; i < 3; ++i) CHECK(length[i] > 9500 && length[i] <= 9600);
    for (int i = 0; i < 2; ++i) CHECK(s.m.amp[i].power_ups == 3 && s.m.amp[i].power_downs == 3);   /* both amplifiers run each time */
    /* mode 1 (and any other value the glue passes as 1): one tone on both, no walk line */
    const unsigned lines = s.line_count;
    const uint64_t f1 = s.d.played_frames;
    CHECK(speaker_core_tones(&s.c, 1, 440, 100, 30) == 0 && s.c.runs == 4);
    for (unsigned i = lines; i < s.line_count; ++i) CHECK(!strstr(s.lines[i], "tone walk"));
    CHECK(bursts(&s, f1, 4, left, right, length) == 1 && left[0] && right[0]);
    /* no channel: refused before anything runs */
    CHECK(speaker_core_tone_on(&s.c, 440, 100, 30, 0) == SpeakerErrArgument && speaker_core_tone_on(&s.c, 440, 100, 30, 4) == SpeakerErrArgument);
    CHECK(s.c.runs == 4);
    free(s.d.played);
}

/* The ceiling's boot argument: 0 is the default (12 dB), the rest clamped to 3..40 dB; the knee 3 dB below it. */
static void ceilings(void)
{
    static Sim s;
    static const struct {uint32_t db, want_db; int32_t ceiling, knee;} cases[] = {
        {0, 12, 8231, 5827}, {6, 6, 16422, 11626}, {1, 3, 23197, 16422}, {3, 3, 23197, 16422}, {12, 12, 8231, 5827},
        {40, 40, 328, 232}, {41, 40, 328, 232}, {1000, 40, 328, 232},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        SpeakerConfig k = defaults();
        k.ceiling_db = cases[i].db;
        sim_init(&s, &k);
        CHECK(s.c.config.ceiling_db == cases[i].want_db && s.c.limit.ceiling == cases[i].ceiling && s.c.limit.knee == cases[i].knee);
        CHECK(speaker_limit_need(&s.c.limit, 32768) == (uint32_t)(((uint64_t)cases[i].ceiling << 16) / 32768));
        free(s.d.played);
        s.d.played = NULL;
    }
}

/* The audio server's plug-in: writes timeline frames ahead of the play position. The pattern (L = frame mod 2^13, R = 0x1fff - L)
 * stays below the limiter's knee, where the path must carry every sample unchanged; gLoud switches to a full-scale signal instead
 * (1: a 1 kHz sine at 0 dBFS; 2: a full-scale 500 Hz square, the worst a client can send). */
typedef struct {int16_t out[PAGANI_AUDIO_RING_FRAMES * 2], in[PAGANI_AUDIO_RING_FRAMES]; uint8_t control[PAGANI_AUDIO_CONTROL_BYTES];} Rings;
static Rings gRings;
static int gLoud;
#define PATTERN 0x0fff
static void plugin_write(Sim *s, uint64_t ahead)
{
    PaganiAudioControl *c = gEngine.ctl;
    const uint64_t now = op_ticks(s), until = pagani_audio_engine_frame_at(&gEngine, now) + ahead;
    for (uint64_t f = c->out_written; f < until; ++f) {
        int16_t l = (int16_t)(f & PATTERN), r = (int16_t)(PATTERN - l);
        if (gLoud == 1) l = r = (int16_t)speaker_tone_sine((uint32_t)(f * 89478485u));   /* 2^32 x 1000 / 48000 per frame */
        if (gLoud == 2) l = r = (f / 48) & 1 ? (int16_t)-32768 : (int16_t)32767;
        gRings.out[f % PAGANI_AUDIO_RING_FRAMES * 2] = l;
        gRings.out[f % PAGANI_AUDIO_RING_FRAMES * 2 + 1] = r;
    }
    if (until > c->out_written) {c->out_written = until; c->out_written_host = now;}
}
static void serve(Sim *s, uint32_t ms, int plugin)
{
    const uint64_t end = s->m.us + (uint64_t)ms * 1000;
    while (s->m.us < end) {
        uint32_t wait = pagani_audio_engine_poll(&gEngine, op_ticks(s), s->m.us / 1000);
        if (plugin) plugin_write(s, gEngine.ctl->lead_frames + 960);
        op_wait(s, wait < 5 ? wait : 5);
    }
}
/* The played frames of a run that began at from: first the limiter's look-ahead (one block of silence), then the pattern; how many
 * frame-to-frame steps break it (count: the pattern frames). */
static unsigned breaks(const Sim *s, uint64_t from, uint64_t *count)
{
    unsigned b = 0;
    CHECK(s->d.played_frames >= from + SPEAKER_LIMIT_BLOCK);
    for (uint64_t i = from; i < from + SPEAKER_LIMIT_BLOCK; ++i) CHECK(s->d.played[2 * i] == 0 && s->d.played[2 * i + 1] == 0);
    from += SPEAKER_LIMIT_BLOCK;
    *count = s->d.played_frames - from;
    for (uint64_t i = from; i < s->d.played_frames; ++i) {
        const int16_t l = s->d.played[2 * i], r = s->d.played[2 * i + 1];
        CHECK(r == (int16_t)(PATTERN - l));
        if (i > from && l != (int16_t)((s->d.played[2 * i - 2] + 1) & PATTERN)) ++b;
    }
    return b;
}

static void ios(void)
{
    static Sim s;
    bringup(&s);
    memset(&gRings, 0, sizeof gRings);
    CHECK(pagani_audio_engine_init(&gEngine, gRings.control, gRings.out, gRings.in, op_ticks(&s), 625, 12, &pagani_audio_silent_backend,
                                   0) == 0);
    gEngineLive = 1;
    SpeakerBackend sb;
    PaganiAudioBackend adsp;
    speaker_backend_init(&sb, &adsp, &s.c, op_ticks, &s);
    CHECK(adsp.clocked == 0 && adsp.id == PaganiAudioBackendAdsp && !strcmp(adsp.name, "adsp"));
    CHECK(pagani_audio_set_backend(&adsp) == 0);
    serve(&s, 20, 0);
    CHECK(gEngine.backend == &adsp && gEngine.ctl->backend == PaganiAudioBackendAdsp && gEngine.stats.switches == 1);

    /* A client plays: the ring reaches the DSP frame by frame, both channels in order. */
    PaganiAudioClientState client = {0};
    gEngine.ctl->out_written = pagani_audio_engine_frame_at(&gEngine, op_ticks(&s));
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStartOutput) == 0);
    plugin_write(&s, gEngine.ctl->lead_frames + 960);         /* the audio server mixes as soon as StartIO returns */
    const uint64_t from = s.d.played_frames;
    serve(&s, 3000, 1);
    CHECK(gEngine.running && s.c.running && s.c.runs == 1 && s.d.errors == 0);
    uint64_t count = 0;
    CHECK(breaks(&s, from, &count) == 0 && count + SPEAKER_LIMIT_BLOCK >= 290 * 480 && count + SPEAKER_LIMIT_BLOCK <= 300 * 480);
    CHECK(s.c.limit.limited == 0 && s.c.limit.peak_out == PATTERN && s.c.limit.clamps == 0);   /* below the knee: untouched */
    CHECK(gEngine.stats.late == 0 && gEngine.stats.holes == 0 && gEngine.stats.played >= 290 && s.c.played == gEngine.stats.played);
    CHECK(gEngine.stats.base_lead > 0 && gEngine.lead >= PAGANI_AUDIO_LEAD_FRAMES && s.c.in_flight <= SpeakerInFlight);
    CHECK(s.d.underruns <= 1 && s.m.amp[0].power_ups == 1 && (s.m.amp[1].reg[0x00] & 8));

    /* Capture: silence by the clock. */
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStartInput) == 0);
    serve(&s, 1000, 1);
    CHECK(gEngine.capturing && gEngine.stats.captured >= 47000 && gEngine.stats.captured <= 48500 && gEngine.stats.peak_in == 0);
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStopInput) == 0);

    /* Stop: kept 5 s, then amplifiers down, graphs stopped, clock released; the buffers the DSP returns at the stop are stale. */
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStopOutput) == 0);
    serve(&s, 4000, 0);
    CHECK(s.c.running && gEngine.running);
    serve(&s, 2000, 0);
    CHECK(!s.c.running && !gEngine.running && s.d.started == 0 && s.d.prm_releases == 1 && s.m.amp[0].power_downs == 1 && !s.m.bit_clock);
    /* stale: the buffers the stop gave back, and answers that came in just before the stop (sent = played + those) */
    CHECK(s.d.returned > 0 && s.c.stale >= s.d.returned && s.c.played + s.c.stale == s.c.sent);

    /* Again: run 2 plays continuously too. */
    gEngine.ctl->out_written = pagani_audio_engine_frame_at(&gEngine, op_ticks(&s));
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStartOutput) == 0);
    plugin_write(&s, gEngine.ctl->lead_frames + 960);
    const uint64_t from2 = s.d.played_frames;
    serve(&s, 1000, 1);
    CHECK(s.c.runs == 2 && breaks(&s, from2, &count) == 0 && count + SPEAKER_LIMIT_BLOCK >= 90 * 480 && s.d.errors == 0);

    /* The DSP stops taking buffers: after 1 s a restart (stop, start), three times, then the silent backend takes over. */
    s.d.stall = 1;
    serve(&s, 6000, 1);
    CHECK(sb.restarts == 3 && sb.fallbacks == 1 && gEngine.backend == &pagani_audio_silent_backend && gEngine.running);
    CHECK(!s.c.running && s.c.stalls >= 4 && s.d.started == 0 && logged(&s, "[spk] stall: the DSP stopped answering"));
    CHECK(gEngine.stats.switches == 2);
    gEngineLive = 0;
    free(s.d.played);
}

/* iOS at full volume through the ring: a 0 dBFS sine, then a full-scale square, then the quiet pattern again. Nothing above the
 * ceiling reaches the DSP, the final clamp never acts, and once the loud part is over the gain comes back to unity and the pattern
 * plays bit for bit again. */
static void loud(void)
{
    static Sim s;
    bringup(&s);
    memset(&gRings, 0, sizeof gRings);
    CHECK(pagani_audio_engine_init(&gEngine, gRings.control, gRings.out, gRings.in, op_ticks(&s), 625, 12, &pagani_audio_silent_backend,
                                   0) == 0);
    gEngineLive = 1;
    SpeakerBackend sb;
    PaganiAudioBackend adsp;
    speaker_backend_init(&sb, &adsp, &s.c, op_ticks, &s);
    CHECK(pagani_audio_set_backend(&adsp) == 0);
    serve(&s, 20, 0);
    PaganiAudioClientState client = {0};
    gEngine.ctl->out_written = pagani_audio_engine_frame_at(&gEngine, op_ticks(&s));
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStartOutput) == 0);
    gLoud = 1;
    plugin_write(&s, gEngine.ctl->lead_frames + 960);
    const uint64_t from = s.d.played_frames;
    serve(&s, 1000, 1);
    gLoud = 2;
    serve(&s, 1000, 1);
    gLoud = 0;
    serve(&s, 1500, 1);
    CHECK(s.c.running && s.c.runs == 1 && s.d.errors == 0 && s.c.limit.clamps == 0 && s.c.limit.limited > 1500);
    CHECK(s.c.limit.peak_in == 32768 && s.c.limit.peak_out <= 8231 && s.c.limit.peak_out >= 8200);
    CHECK(speaker_limit_permille(s.c.limit.min_gain) >= 249 && speaker_limit_permille(s.c.limit.min_gain) <= 252);
    unsigned peak = 0;
    for (uint64_t i = from; i < s.d.played_frames; ++i)
        for (int c = 0; c < 2; ++c) if ((unsigned)abs(s.d.played[2 * i + c]) > peak) peak = (unsigned)abs(s.d.played[2 * i + c]);
    CHECK(peak == s.c.limit.peak_out);
    /* the last half second: the pattern, bit for bit (the gain is back at unity) */
    const uint64_t tail = s.d.played_frames - 24000;
    for (uint64_t i = tail; i < s.d.played_frames; ++i) {
        const int16_t l = s.d.played[2 * i], r = s.d.played[2 * i + 1];
        CHECK(r == (int16_t)(PATTERN - l) && (i == tail || l == (int16_t)((s.d.played[2 * i - 2] + 1) & PATTERN)));
    }
    CHECK(s.c.limit.gain == SPEAKER_LIMIT_UNITY);
    /* the stop line reports what the ceiling did */
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStopOutput) == 0);
    serve(&s, 6000, 0);
    CHECK(!s.c.running && logged(&s, "gmin=25") && logged(&s, "in=32768 out=82") && logged(&s, "clamp=0"));
    gEngineLive = 0;
    free(s.d.played);
}

static void closed_mid_run(void)
{
    static Sim s;
    bringup(&s);
    memset(&gRings, 0, sizeof gRings);
    CHECK(pagani_audio_engine_init(&gEngine, gRings.control, gRings.out, gRings.in, op_ticks(&s), 625, 12, &pagani_audio_silent_backend,
                                   0) == 0);
    gEngineLive = 1;
    SpeakerBackend sb;
    PaganiAudioBackend adsp;
    speaker_backend_init(&sb, &adsp, &s.c, op_ticks, &s);
    CHECK(pagani_audio_set_backend(&adsp) == 0);
    PaganiAudioClientState client = {0};
    CHECK(pagani_audio_engine_request(&gEngine, &client, PaganiAudioStartOutput) == 0);
    serve(&s, 500, 1);
    CHECK(s.c.running);
    s.d.down_at = s.m.us + 100000;                            /* the DSP crashes: PaganiAdsp closes the channel */
    serve(&s, 1000, 1);
    CHECK(s.c.dead && !s.c.running && sb.fallbacks == 1 && gEngine.backend == &pagani_audio_silent_backend && gEngine.running);
    CHECK(s.m.amp[0].power_downs == 1 && s.m.ctl[SpeakerPinSck] == 1u && logged(&s, "[spk] play lost: the DSP channel closed"));
    /* Nothing more reaches the DSP; a start is refused at once. */
    const SpeakerSource none = {0, 0, 0, 0};
    CHECK(speaker_core_start(&s.c, &none) == SpeakerErrArgument && speaker_core_start(&s.c, &sb.source) == SpeakerErrDown);
    gEngineLive = 0;
    free(s.d.played);
}

/* One bring-up that should stop at stage (negative: fail there with error). */
static void expect(Sim *s, int32_t stage, int32_t error)
{
    CHECK(speaker_core_bringup(&s->c) == 0 && s->c.stage == stage && s->c.error == error && !s->c.ready);
}

static void failures(void)
{
    static Sim s;
    SpeakerConfig k = defaults();
    /* Stop after every stage: nothing beyond it happens. */
    for (uint32_t st = SpeakerStPins; st <= SpeakerStGraphs; ++st) {
        k.stop_after = st;
        sim_init(&s, &k);
        if (st == SpeakerStGraphs) {
            CHECK(speaker_core_bringup(&s.c) == 0 && s.c.stage == SpeakerStGraphs && s.c.error == SpeakerErrStopped && s.c.ready);
        } else {
            expect(&s, (int32_t)st, SpeakerErrStopped);
        }
        CHECK(logged(&s, "stopped as asked") && s.d.errors == 0);
        CHECK(s.rail_calls == (st >= SpeakerStRail ? 1u : 0u) && (s.c.amps_found != 0) == (st >= SpeakerStAmps));
        CHECK(s.d.registered == (st >= SpeakerStDsp) && (s.d.spf_queries != 0) == (st >= SpeakerStApm) &&
              (s.smmu_calls != 0) == (st >= SpeakerStSmmu) && (s.d.open != 0) == (st >= SpeakerStGraphs));
        CHECK(!s.m.amp[0].power_ups && s.m.amp[0].writes == (st >= SpeakerStAmpConfig ? s.m.amp[0].writes : 0u));
    }
    k = defaults();
    /* Pins that do not take their state. */
    sim_init(&s, &k); s.m.stuck_pin = SpeakerPinData1; s.m.ctl[SpeakerPinData1] = 0x40;
    expect(&s, -SpeakerStPins, SpeakerErrIo);
    /* No rail: logged, the amplifiers decide. */
    sim_init(&s, &k); s.rail_rc = -16;
    CHECK(speaker_core_bringup(&s.c) == 1 && logged(&s, "[spk] rail ldob15 mv=1800 rc=-16"));
    /* The I2C hub core clock never runs; the engine is not I2C. */
    sim_init(&s, &k); s.m.core_off = 1;
    expect(&s, -SpeakerStBus, -110);
    CHECK(s.m.commands == 0);
    sim_init(&s, &k); s.m.se[GENI_FW_REVISION / 4] = 2u << 8;
    expect(&s, -SpeakerStBus, -19);
    /* Amplifiers: none answers; one does; one is misconfigured (its slot register ignores writes). */
    sim_init(&s, &k); s.m.amp[0].present = s.m.amp[1].present = 0;
    expect(&s, -SpeakerStAmps, SpeakerErrNoDevice);
    CHECK(logged(&s, "[spk] amp 0x34 absent rc=-5") && logged(&s, "FAIL st=-4 err=-19 no TFA9873 answered"));
    sim_init(&s, &k); s.m.amp[1].present = 0;
    CHECK(speaker_core_bringup(&s.c) == 1 && s.c.amps_found == 1 && s.c.amps_ready == 1);
    sim_init(&s, &k); s.m.amp[0].por[0x03] = s.m.amp[0].reg[0x03] = 0x0b74;
    CHECK(speaker_core_bringup(&s.c) == 1 && s.c.amps_found == 2 && logged(&s, "type=unknown"));
    sim_init(&s, &k); s.m.amp[0].ignore = 0x26;
    CHECK(speaker_core_bringup(&s.c) == 1 && s.c.amps_ready == 2 && logged(&s, "[spk] amp 0x34 config MISMATCH"));
    sim_init(&s, &k); s.m.amp[0].ignore = s.m.amp[1].ignore = 0x20;
    expect(&s, -SpeakerStAmpConfig, SpeakerErrIo);
    /* Only the bottom amplifier, 16-bit slots, data line 1, PRM from port 7, feedback kept, no strong pull. */
    k.amps = 2; k.device_bits = 16; k.sd_line = AR_I2S_SD1; k.prm_port = AR_PRM_PORT_ASM; k.feedback = 1; k.i2c_pull = 0;
    sim_init(&s, &k);
    CHECK(speaker_core_bringup(&s.c) == 1 && s.c.amps_found == 2 && s.m.amp[0].writes == 0 && (s.m.amp[1].reg[0x23] & 8));
    CHECK((s.m.amp[1].reg[0x20] & 0xf000) == 0 && (s.m.amp[1].reg[0x21] & 0x1f0) == (15u << 4) && s.d.ep_bits == 16 && s.d.cnv_bits == 16);
    CHECK(s.d.sd_line == AR_I2S_SD1 && !(s.m.ctl[SpeakerPinSda] & SpeakerCtlI2cPull));
    CHECK(speaker_core_tone(&s.c, 1000, 100, 40) == 0 && s.d.prm_port == 7 && s.d.prm_hz == 48000u * 2 * 16 && s.m.amp[0].power_ups == 0);
    free(s.d.played); s.d.played = NULL;
    k = defaults();
    /* The DSP side: PaganiAdsp off, never up, failed, the framework never ready, silent, gone during the query. */
    sim_init(&s, &k); s.d.adsp_off = 1;
    expect(&s, -SpeakerStDsp, SpeakerErrNoDevice);
    k.dsp_wait_ms = 2000;
    sim_init(&s, &k); s.d.up_at = ~0ull;
    expect(&s, -SpeakerStDsp, SpeakerErrTimeout);
    sim_init(&s, &k); s.d.up_at = ~0ull; s.d.stage = -PaganiAdspStDtb;
    expect(&s, -SpeakerStDsp, SpeakerErrDown);
    CHECK(s.m.us < 5000000 + 2000000 && logged(&s, "PaganiAdsp failed"));
    k = defaults(); k.apm_queries = 3;
    sim_init(&s, &k); s.d.spf_not_ready = 5;
    expect(&s, -SpeakerStApm, SpeakerErrTimeout);
    CHECK(s.d.spf_queries == 3 && logged(&s, "[spk] apm state=not-ready queries=3 answers=3"));
    sim_init(&s, &k); s.d.spf_silent = 1;
    expect(&s, -SpeakerStApm, SpeakerErrTimeout);
    CHECK(s.d.spf_queries == 3 && logged(&s, "timeouts=3"));
    sim_init(&s, &k); s.d.spf_silent = 1; s.d.down_at = s.d.up_at + 500000;
    expect(&s, -SpeakerStApm, SpeakerErrDown);
    k = defaults();
    /* Memory: the required stream id, an optional one, the buffer. */
    sim_init(&s, &k); s.smmu_rc[0] = -16;
    expect(&s, -SpeakerStSmmu, -16);
    sim_init(&s, &k); s.smmu_rc[1] = -16;
    CHECK(speaker_core_bringup(&s.c) == 1 && s.c.sid_mask == 0xd && logged(&s, "0x1081=-16"));
    k.sids = 1;
    sim_init(&s, &k);
    CHECK(speaker_core_bringup(&s.c) == 1 && s.smmu_calls == 1);
    k = defaults();
    sim_init(&s, &k); s.no_buffer = 1;
    expect(&s, -SpeakerStBuffer, SpeakerErrNoMemory);
    /* Graphs: refused opens and map; what was opened is closed again. */
    sim_init(&s, &k); s.d.refuse_op = AR_OP_GRAPH_OPEN; s.d.refuse_sg = AR_SG_SPEAKER; s.d.refuse_status = 0x0a;
    expect(&s, -SpeakerStGraphs, SpeakerErrRefused);
    CHECK(s.d.closes == 0 && s.c.last_status == 0x0a && logged(&s, "open device op=0x1001000 dsp=0xa"));
    sim_init(&s, &k); s.d.refuse_op = AR_OP_GRAPH_OPEN; s.d.refuse_sg = AR_SG_PLAY; s.d.refuse_status = 0x0b;
    expect(&s, -SpeakerStGraphs, SpeakerErrRefused);
    CHECK(s.d.closes == 1 && s.d.open == 0);
    sim_init(&s, &k); s.d.refuse_map = 1;
    expect(&s, -SpeakerStGraphs, SpeakerErrRefused);
    CHECK(s.d.closes == 2 && s.d.open == 0 && logged(&s, "map buffer"));
    sim_init(&s, &k); s.d.refuse_op = AR_OP_SET_CFG; s.d.refuse_status = 0x0c;
    expect(&s, -SpeakerStGraphs, SpeakerErrRefused);
    CHECK(s.d.closes == 1);

    /* A run: the PRM refuses the clock (the run plays; the DSP drives it); the device start is refused (nothing kept). */
    sim_init(&s, &k); s.d.prm_refuse = 0x0c;
    CHECK(speaker_core_bringup(&s.c) == 1 && speaker_core_tone(&s.c, 440, 100, 30) == 0 && s.d.prm_requests == 1 && s.d.prm_releases == 0);
    CHECK(logged(&s, "prm=12"));
    sim_init(&s, &k);
    CHECK(speaker_core_bringup(&s.c) == 1);
    s.d.refuse_op = AR_OP_GRAPH_START; s.d.refuse_sg = AR_SG_PLAY; s.d.refuse_status = 0x0d;
    CHECK(speaker_core_tone(&s.c, 440, 100, 30) == SpeakerErrRefused && !s.c.running && s.d.started == 0 && s.d.prm_releases == 1);
    CHECK(s.m.ctl[SpeakerPinSck] == 1u && s.m.amp[0].power_ups == 0 && logged(&s, "[spk] play refused run=1 rc=-71 op=0x1001002 dsp=0xd"));
    /* A full queue at PaganiAdsp is waited out; garbage from the DSP is counted and ignored. */
    s.d.refuse_op = 0;
    s.d.full_sends = 5;
    const uint8_t junk[30] = {0x61, 0x18};
    speaker_core_receive(&s.c, junk, sizeof junk);
    speaker_core_receive(&s.c, junk, 0);
    CHECK(speaker_core_tone(&s.c, 440, 100, 30) == 0 && s.c.bad == 1 && s.d.errors == 0);
    /* The answer ring overflowing drops (counted) rather than overwrite. */
    speaker_core_pump(&s.c);
    CHECK(s.c.head == s.c.tail);
    for (int i = 0; i < 40; ++i) speaker_core_receive(&s.c, junk, sizeof junk);
    CHECK(s.c.dropped == 8);
    free(s.d.played);
}

int main(int argc, char **argv)
{
    (void)argv;
    gVerbose = argc > 1;
    stretches();
    tone();
    walk();
    ios();
    loud();
    closed_mid_run();
    failures();
    ceilings();
    printf("PASS core: %u checks\n", checks);
    return 0;
}
