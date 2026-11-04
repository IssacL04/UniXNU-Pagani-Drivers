// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of the FastRPC reverse file service (SensorsRpc.c) and the pack (SensorsPack.c), against a simulated DSP written from the
// public facts on its own (the message and the metadata as Linux's fastrpc.c lays them out, the listener's blobs as Qualcomm's
// inc/listener_buf.h does): it shares no code with the driver.
//
// The DSP is the test's main thread and the service runs on a second thread, strictly in turn, as on the phone: the service blocks in
// next2 until the DSP has a call for it. The DSP's script reads like the sensor PD's work: dsp_call() is one reverse call, returning
// what the service answered. Covered: the bring-up's order and bytes, refused and unanswered attaches, every apps_std call, exact out
// lengths and the alignment, unaligned length words, a call beyond any buffer, bad descriptors, write refusals, an unknown interface,
// apps_mem's nested mmap, next2 failing, the link going away and coming back, a fuzz of the DSP's blobs under ASan with the arena
// allocated exactly, and the pack: paths, lookups, directory listings, corrupted tables. With an argument: a pack made by
// scripts/make-sensor-pack.py, read back through the service.
#include <pthread.h>
#include "check.h"
#include "../SensorsRpc.h"

/* a failing check also shows the log */
static void dump_log(void);
#undef CHECK
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); dump_log(); exit(1);} ++checks;} while (0)

#define ARENA_PA 0xd0100000ull
#define PROCESS 4242u

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn = PTHREAD_COND_INITIALIZER;

static uint8_t *arena;
static uint8_t bounce[SSR_BOUNCE];
static SsrCore core;
static uint64_t now_ms = 1000;
static char logbuf[1 << 20];
static size_t logused;

static const uint8_t *pack_bytes;
static uint32_t pack_length;
static int pack_answer = SsrPackOk, pack_not_yet;

/* the simulated DSP's state */
static struct {
    uint32_t messages, attaches, opens, registers, closes, inits, mmaps;
    int refuse_attach, ignore_attach, refuse_open, refuse_send;
    int next2_error[4], next2_errors;
    uint32_t attached;                 /* the process id */
    int waiting;                       /* a next2 is in: its fields below */
    uint64_t ctx;
    uint32_t prev_ctx, prev_length, in_capacity;
    int32_t prev_result;
    uint8_t prev[0x100000];            /* the answer's blob, as the DSP saw it */
    uint8_t *prim_out, *in_blob;
    uint32_t next_rctx;
    uint32_t last_order[8];
    unsigned order;
} dsp;

static int link_stop;
static uint8_t first_message[40], first_metadata[64];
static int have_first;

/* ---- the operations ---- */
static uint64_t op_now(void *x) {(void)x; return __atomic_load_n(&now_ms, __ATOMIC_ACQUIRE);}
static void op_trace(void *x, const char *line)
{
    (void)x;
    const size_t n = strlen(line);
    CHECK(!strncmp(line, "[sensors] ", 10) && n < 240);
    pthread_mutex_lock(&lock);
    if (logused + n + 2 < sizeof logbuf) {memcpy(logbuf + logused, line, n); logused += n; logbuf[logused++] = '\n'; logbuf[logused] = 0;}
    pthread_mutex_unlock(&lock);
}
static void dump_log(void) {fprintf(stderr, "---- log (end) ----\n%s", logused > 4000 ? logbuf + logused - 4000 : logbuf);}
static int logged(const char *text)
{
    pthread_mutex_lock(&lock);
    const int found = strstr(logbuf, text) != 0;
    pthread_mutex_unlock(&lock);
    return found;
}
static void op_publish(void *x) {(void)x;}
static int op_pack_size(void *x, uint64_t *size)
{
    (void)x;
    if (pack_not_yet > 0) {pack_not_yet--; return SsrPackNotYet;}
    if (pack_answer) return pack_answer;
    *size = pack_length;
    return SsrPackOk;
}
static int op_pack_read(void *x, uint64_t offset, void *to, uint32_t length)
{
    (void)x;
    CHECK(length <= SSR_BOUNCE);
    if (offset + length > pack_length) return -1;
    memcpy(to, pack_bytes + offset, length);
    return 0;
}
static void *op_alloc(void *x, uint32_t bytes) {(void)x; return malloc(bytes);}

/* The clock runs on while the service waits for something that is no next2; a next2 waits for the DSP's turn. */
static void op_wait(void *x, uint32_t ms)
{
    (void)x;
    pthread_mutex_lock(&lock);
    if (dsp.waiting) {
        pthread_cond_wait(&turn, &lock);
    } else {
        __atomic_fetch_add(&now_ms, (uint64_t)ms, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
}

/* ---- the DSP's view of an invoke ---- */
typedef struct {
    uint8_t *p;
    uint64_t length;
} View;

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));}

/* What the DSP's memory manager does with a bus address: the context bank above bit 32, the rest inside the arena. */
static uint32_t expected_bank = SSR_CONTEXT_BANK;
static uint8_t *memory(uint64_t address, uint64_t length)
{
    CHECK(address >> 32 == expected_bank);
    const uint64_t pa = address & 0xffffffffu;
    CHECK(pa >= ARENA_PA && pa + length <= ARENA_PA + SSR_ARENA_BYTES);
    return arena + (pa - ARENA_PA);
}

/* The buffers of an invoke, from its metadata, the way Linux's fastrpc lays it out. */
static void views(uint64_t addr, uint64_t size, uint32_t n, View *out)
{
    CHECK(n <= 8 && size >= 0x1000 && !(size & 0xfff) && !(addr & 0xfff));
    const uint8_t *meta = memory(addr, size);
    const uint8_t *list = meta + n * 16, *pages = list + n * 8, *fds = pages + n * 16, *crcs = fds + 16 * 8;
    for (unsigned i = 0; i < 16 * 8 + 64 * 4; ++i) CHECK(fds[i] == 0);
    (void)crcs;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t pv = rd64(meta + i * 16), length = rd64(meta + i * 16 + 8);
        const uint32_t num = rd32(list + i * 8), index = rd32(list + i * 8 + 4);
        CHECK(index == i);
        out[i].length = length;
        out[i].p = 0;
        if (!length) {
            CHECK(num == 0 && pv == 0);
            continue;
        }
        CHECK(num == 1);
        const uint64_t page = rd64(pages + i * 16), bytes = rd64(pages + i * 16 + 8);
        CHECK(!(page & 0xfff) && !(bytes & 0xfff) && bytes);
        CHECK((pv & 0xfff) + length <= bytes);                  /* the whole buffer inside its pages */
        CHECK(bytes - (pv & 0xfff) - length < 0x1000);          /* whole pages, no more than it needs */
        out[i].p = memory(page, bytes) + (pv & 0xfff);
        CHECK((pv >> 32) == 0 && (pv & ~0xfffull) == (page & 0xffffffffu));
    }
}

static void respond(uint64_t ctx, int32_t value)
{
    uint8_t r[12];
    wr64(r, ctx);
    wr32(r + 8, (uint32_t)value);
    ssr_core_receive(&core, r, sizeof r);
}

/* Every invoke arrives here, on the service's thread. */
static int op_send(void *x, const uint8_t *m, uint32_t length)
{
    (void)x;
    CHECK(length == 40);
    if (dsp.refuse_send) {dsp.refuse_send--; return -1;}
    const uint32_t pid = rd32(m), tid = rd32(m + 4), handle = rd32(m + 16), sc = rd32(m + 20);
    const uint64_t ctx = rd64(m + 8), addr = rd64(m + 24), size = rd64(m + 32);
    const uint32_t in = (sc >> 16) & 0xff, out = (sc >> 8) & 0xff, method = (sc >> 24) & 0x1f;
    View v[8];
    dsp.messages++;
    if (!have_first) {
        have_first = 1;
        memcpy(first_message, m, 40);
        memcpy(first_metadata, arena, 40);
    }
    CHECK((ctx & 0xff) == SSR_PD_SENSORS && ctx >> 8 && ctx >> 16 == 0 && tid == PROCESS && !(sc & 0xff) && !(sc >> 29));
    if (in + out) views(addr, size, in + out, v);
    else CHECK(!addr && !size);
    if (dsp.order < 8) dsp.last_order[dsp.order++] = handle << 8 | method;
    if (handle == 1 && method == 0) {                    /* attach */
        CHECK(pid == 0 && in == 1 && out == 0 && v[0].length == 4);
        dsp.attaches++;
        if (dsp.ignore_attach) {dsp.ignore_attach--; return 0;}
        if (dsp.refuse_attach) {dsp.refuse_attach--; respond(ctx, 0x8000);  return 0;}
        dsp.attached = rd32(v[0].p);
        respond(ctx, 0);
        return 0;
    }
    if (handle == 1 && method == 4) {                    /* mmap: 2 in, 1 out; the pages the service took from its heap */
        CHECK(pid == 0 && in == 2 && out == 1 && v[0].length == 24 && v[1].length == 16 && v[2].length == 8);
        CHECK(rd32(v[0].p) == PROCESS && (rd32(v[0].p + 4) & 0x1000) && rd64(v[0].p + 8) == 0 && rd32(v[0].p + 16) == 16);
        const uint64_t page = rd64(v[1].p), bytes = rd64(v[1].p + 8);
        CHECK(bytes && !(bytes & 0xfff) && !(page & 0xfff));
        const uint8_t *heap = memory(page, bytes);
        CHECK(heap >= arena + SSR_CALL_BYTES);
        dsp.mmaps++;
        wr64(v[2].p, 0xe0000000ull + (page & 0xfffff));
        respond(ctx, 0);
        return 0;
    }
    CHECK(pid == PROCESS && dsp.attached == PROCESS);
    if (handle == 0 && method == 0) {                    /* remotectl open */
        CHECK(in == 2 && out == 2 && v[0].length == 8 && v[2].length == 8);
        const uint32_t name_length = rd32(v[0].p), error_length = rd32(v[0].p + 4);
        CHECK(v[1].length == name_length && v[3].length == error_length && error_length);
        CHECK(!strcmp((const char *)v[1].p, "adsp_default_listener") && name_length == 22);
        dsp.opens++;
        if (dsp.refuse_open) {
            dsp.refuse_open--;
            wr32(v[2].p + 4, 7);
            strcpy((char *)v[3].p, "dlopen failed");
            respond(ctx, 0);
            return 0;
        }
        wr32(v[2].p, 0x77);
        respond(ctx, 0);
        return 0;
    }
    if (handle == 0 && method == 1) {                    /* remotectl close */
        CHECK(in == 1 && out == 2 && v[0].length == 8 && rd32(v[0].p) == 0x77 && rd32(v[0].p + 4) == 0 && v[1].length == 4 && !v[2].length);
        dsp.closes++;
        respond(ctx, 0);
        return 0;
    }
    if (handle == 0x77) {                                /* register */
        CHECK(method == 0 && !in && !out);
        dsp.registers++;
        respond(ctx, 0);
        return 0;
    }
    CHECK(handle == 3);
    if (method == 3) {
        CHECK(!in && !out && dsp.registers);
        dsp.inits++;
        respond(ctx, 0);
        return 0;
    }
    CHECK(method == 4 && in == 2 && out == 2 && v[0].length == 16 && v[2].length == 16);
    const uint32_t out_bytes = rd32(v[0].p + 8), capacity = rd32(v[0].p + 12);
    CHECK(v[1].length == out_bytes && v[3].length == capacity && capacity >= 0x1000 && dsp.inits);
    pthread_mutex_lock(&lock);
    dsp.prev_ctx = rd32(v[0].p);
    dsp.prev_result = (int32_t)rd32(v[0].p + 4);
    dsp.prev_length = out_bytes;
    CHECK(out_bytes <= sizeof dsp.prev);
    if (out_bytes) memcpy(dsp.prev, v[1].p, out_bytes);
    dsp.in_capacity = capacity;
    dsp.prim_out = v[2].p;
    dsp.in_blob = v[3].p;
    dsp.ctx = ctx;
    dsp.waiting = 1;
    pthread_cond_broadcast(&turn);
    pthread_mutex_unlock(&lock);
    return 0;
}

static int prepare_error, prepared_at_attach;
static int op_prepare(void *x)
{
    (void)x;
    prepared_at_attach = (int)dsp.messages + 1;        /* before the first message of the bring-up */
    return prepare_error;
}

static const SsrOps ops = {0, op_now, op_send, op_wait, op_trace, op_publish, op_prepare, op_pack_size, op_pack_read, op_alloc};

/* ---- the service's thread ---- */
static int service_stopped;
static void *service(void *x)
{
    (void)x;
    for (;;) {
        pthread_mutex_lock(&lock);
        const int stop_now = link_stop;
        pthread_mutex_unlock(&lock);
        if (stop_now) break;
        if (!ssr_core_step(&core)) break;
    }
    pthread_mutex_lock(&lock);
    service_stopped = 1;
    pthread_cond_broadcast(&turn);
    pthread_mutex_unlock(&lock);
    return 0;
}

/* The DSP's thread waits for the next next2 (or the service stopping). */
static int await_next2(void)
{
    pthread_mutex_lock(&lock);
    while (!dsp.waiting && !service_stopped) pthread_cond_wait(&turn, &lock);
    const int got = dsp.waiting;
    pthread_mutex_unlock(&lock);
    return got;
}

static void release_next2(int32_t value, const uint8_t *blob, uint32_t length, uint32_t handle, uint32_t sc)
{
    const uint64_t ctx = dsp.ctx;
    if (!value) {
        CHECK(length <= dsp.in_capacity);
        if (length) memcpy(dsp.in_blob, blob, length);
        dsp.next_rctx++;
        wr32(dsp.prim_out, dsp.next_rctx);
        wr32(dsp.prim_out + 4, handle);
        wr32(dsp.prim_out + 8, sc);
        wr32(dsp.prim_out + 12, length);
    }
    pthread_mutex_lock(&lock);
    dsp.waiting = 0;
    respond(ctx, value);
    pthread_cond_broadcast(&turn);
    pthread_mutex_unlock(&lock);
}

/* One call of the DSP: packs the buffers the way listener_buf.h does, hands it over, takes the answer with its checks (each out buffer
 * exactly the length asked, aligned from the blob's start, or nothing at all with a nonzero result). */
typedef struct {
    int32_t result;
    uint8_t out[4][0x40000];
} Reply;
static Reply reply;

static uint32_t pack_call(uint8_t *blob, uint32_t cap, const void *const *in, const uint32_t *in_n, unsigned in_count, const uint32_t *out_n,
                          unsigned out_count)
{
    uint32_t at = 0;
    for (unsigned i = 0; i < in_count; ++i) {
        CHECK(at + 4 + 8 + in_n[i] <= cap);
        wr32(blob + at, in_n[i]);
        at += 4;
        if (in_n[i]) {
            while (at & 7) blob[at++] = 0xa5;
            memcpy(blob + at, in[i], in_n[i]);
            at += in_n[i];
        }
    }
    for (unsigned i = 0; i < out_count; ++i) {
        CHECK(at + 4 <= cap);
        wr32(blob + at, out_n[i]);
        at += 4;
    }
    return at;
}

static void take_answer(const uint32_t *out_n, unsigned out_count, uint32_t rctx)
{
    CHECK(dsp.prev_ctx == rctx);
    reply.result = dsp.prev_result;
    if (reply.result) {
        CHECK(dsp.prev_length == 0);
        return;
    }
    uint32_t at = 0;
    for (unsigned i = 0; i < out_count; ++i) {
        CHECK(at + 4 <= dsp.prev_length);
        CHECK(rd32(dsp.prev + at) == out_n[i]);
        at += 4;
        if (out_n[i]) {
            at = (at + 7) & ~7u;
            CHECK(at + out_n[i] <= dsp.prev_length);
            memcpy(reply.out[i], dsp.prev + at, out_n[i]);
            at += out_n[i];
        }
    }
    CHECK(at == dsp.prev_length);
}

static uint8_t call_blob[0x10000];

static int32_t dsp_call(uint32_t handle, uint32_t method, const void *const *in, const uint32_t *in_n, unsigned in_count, const uint32_t *out_n,
                        unsigned out_count)
{
    const uint32_t n = pack_call(call_blob, sizeof call_blob, in, in_n, in_count, out_n, out_count);
    const uint32_t sc = (method > 30 ? 31u : method) << 24 | in_count << 16 | out_count << 8;
    CHECK(await_next2());
    const uint32_t rctx = dsp.next_rctx + 1;
    release_next2(0, call_blob, n, handle, sc);
    CHECK(await_next2());
    take_answer(out_n, out_count, rctx);
    return reply.result;
}

/* ---- the file calls, as the DSP's stubs make them ---- */
#define STD 1
static int32_t remotectl_open(const char *name, uint32_t *handle)
{
    const uint32_t prim[2] = {(uint32_t)strlen(name) + 1, 32};
    const void *in[2] = {prim, name};
    const uint32_t in_n[2] = {8, prim[0]}, out_n[2] = {8, 32};
    const int32_t rc = dsp_call(0, 0, in, in_n, 2, out_n, 2);
    if (!rc) *handle = rd32(reply.out[0]);
    return rc;
}
static int32_t std_fopen_env(const char *env, const char *name, const char *mode, uint32_t *fd)
{
    const uint32_t prim[4] = {(uint32_t)strlen(env) + 1, 2, (uint32_t)strlen(name) + 1, (uint32_t)strlen(mode) + 1};
    const void *in[5] = {prim, env, ";", name, mode};
    const uint32_t in_n[5] = {16, prim[0], prim[1], prim[2], prim[3]}, out_n[1] = {4};
    const int32_t rc = dsp_call(STD, 19, in, in_n, 5, out_n, 1);
    if (!rc) *fd = rd32(reply.out[0]);
    return rc;
}
static int32_t std_fopen(const char *name, const char *mode, uint32_t *fd)
{
    const uint32_t prim[2] = {(uint32_t)strlen(name) + 1, (uint32_t)strlen(mode) + 1};
    const void *in[3] = {prim, name, mode};
    const uint32_t in_n[3] = {8, prim[0], prim[1]}, out_n[1] = {4};
    const int32_t rc = dsp_call(STD, 0, in, in_n, 3, out_n, 1);
    if (!rc) *fd = rd32(reply.out[0]);
    return rc;
}
static int32_t std_fread(uint32_t fd, uint32_t n, uint32_t *got, uint32_t *eof)
{
    const uint32_t prim[2] = {fd, n};
    const void *in[1] = {prim};
    const uint32_t in_n[1] = {8}, out_n[2] = {8, n};
    const int32_t rc = dsp_call(STD, 4, in, in_n, 1, out_n, 2);
    if (!rc) {*got = rd32(reply.out[0]); *eof = rd32(reply.out[0] + 4);}
    return rc;
}
static int32_t std_one(uint32_t method, uint32_t fd)     /* fflush, fclose, rewind, fsync, clearerr: one word in, nothing out */
{
    const void *in[1] = {&fd};
    const uint32_t in_n[1] = {4};
    return dsp_call(STD, method, in, in_n, 1, 0, 0);
}
static int32_t std_word(uint32_t method, uint32_t fd, uint32_t out_bytes)   /* ftell, flen, feof, ferror */
{
    const void *in[1] = {&fd};
    const uint32_t in_n[1] = {4}, out_n[1] = {out_bytes};
    return dsp_call(STD, method, in, in_n, 1, out_n, 1);
}
static int32_t std_fseek(uint32_t fd, int32_t offset, uint32_t whence)
{
    const uint32_t prim[3] = {fd, (uint32_t)offset, whence};
    const void *in[1] = {prim};
    const uint32_t in_n[1] = {12};
    return dsp_call(STD, 9, in, in_n, 1, 0, 0);
}
static int32_t std_named(uint32_t method, const char *name, uint32_t out_bytes)    /* opendir, stat, fileExists: name in, one out buffer */
{
    uint32_t prim[2];
    uint32_t n = 0;
    if (method > 30) prim[n++] = method;
    prim[n++] = (uint32_t)strlen(name) + 1;
    const void *in[2] = {prim, name};
    const uint32_t in_n[2] = {n * 4, (uint32_t)strlen(name) + 1}, out_n[1] = {out_bytes};
    return dsp_call(STD, method, in, in_n, 2, out_n, 1);
}

/* ---- the pack built here ---- */
typedef struct {
    const char *name;
    const char *text;
    uint32_t size;                     /* with no text: this many bytes of a pattern */
} Item;
static uint8_t pattern(uint32_t i) {return (uint8_t)(i * 7 + (i >> 8));}

static uint32_t build_pack(uint8_t *out, uint32_t cap, const Item *items, unsigned count)
{
    /* sorted input; directories are the items without text and size 0 */
    uint32_t names = 0;
    for (unsigned i = 0; i < count; ++i) names += (uint32_t)strlen(items[i].name) + 1;
    const uint32_t table = count * SSP_ENTRY, area = SSP_HEADER + table;
    uint32_t at = area + names, name_at = 0;
    CHECK(at <= cap);
    memset(out, 0, area + names);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t n = items[i].text ? (uint32_t)strlen(items[i].text) : items[i].size;
        const int directory = !items[i].text && !items[i].size;
        uint8_t *e = out + SSP_HEADER + i * SSP_ENTRY;
        wr64(e, directory ? 0 : at);
        wr64(e + 8, directory ? 0 : n);
        wr32(e + 16, name_at);
        wr32(e + 20, directory);
        strcpy((char *)out + area + name_at, items[i].name);
        name_at += (uint32_t)strlen(items[i].name) + 1;
        if (!directory) {
            CHECK(at + n <= cap);
            for (uint32_t k = 0; k < n; ++k) out[at + k] = items[i].text ? (uint8_t)items[i].text[k] : pattern(k);
            at += n;
        }
    }
    uint32_t sum = 0;
    for (uint32_t i = SSP_HEADER; i < area + names; ++i) sum += out[i];
    memcpy(out, "SSCPACK1", 8);
    wr32(out + 8, count);
    wr32(out + 12, names);
    wr64(out + 16, at);
    wr32(out + 24, sum);
    return at;
}

static const Item kItems[] = {
    {"/", 0, 0}, {"/.pagani", 0, 0}, {"/.pagani/library_path", "/vendor/dsp/sensors;/vendor/lib/rfsa/adsp\n", 0},
    {"/mnt", 0, 0}, {"/mnt/vendor", 0, 0}, {"/mnt/vendor/persist", 0, 0}, {"/mnt/vendor/persist/sensors", 0, 0},
    {"/mnt/vendor/persist/sensors/registry", 0, 0}, {"/mnt/vendor/persist/sensors/registry/registry", 0, 0},
    {"/mnt/vendor/persist/sensors/registry/registry/sensors_registry", 0, 70000},
    {"/mnt/vendor/persist/sensors/registry/sns_reg_version", "version=0\n", 0},
    {"/sys", 0, 0}, {"/sys/devices", 0, 0}, {"/sys/devices/soc0", 0, 0}, {"/sys/devices/soc0/hw_platform", "MTP\n", 0},
    {"/sys/devices/soc0/soc_id", "618\n", 0},
    {"/vendor", 0, 0}, {"/vendor/dsp", 0, 0}, {"/vendor/dsp/sensors", 0, 0}, {"/vendor/dsp/sensors/libsns.so", "ELF", 0},
    {"/vendor/etc", 0, 0}, {"/vendor/etc/sensors", 0, 0}, {"/vendor/etc/sensors-other", "not a child", 0},
    {"/vendor/etc/sensors/config", 0, 0}, {"/vendor/etc/sensors/config/a.json", "{\"a\":1}\n{\"b\":2}\nlast", 0},
    {"/vendor/etc/sensors/config/b.json", "{}", 0}, {"/vendor/etc/sensors/config/empty.json", "", 0},
    {"/vendor/etc/sensors/sns_reg_config", "version=1\nfile=input=json.lst\n", 0},
};
static uint8_t pack_store[0x40000];

static uint32_t start_log = 2;

static void start(int pack)
{
    static const SsrConfig base = {0, ARENA_PA, SSR_ARENA_BYTES, bounce, PROCESS, SSR_CONTEXT_BANK, 2};
    SsrConfig cfg = base;
    cfg.arena = arena;
    cfg.bank = expected_bank;
    cfg.log = start_log;
    memset(&dsp, 0, sizeof dsp);
    memset(arena, 0x5a, SSR_ARENA_BYTES);
    if (pack) {
        pack_length = build_pack(pack_store, sizeof pack_store, kItems, sizeof kItems / sizeof kItems[0]);
        pack_bytes = pack_store;
    }
    pack_answer = pack ? SsrPackOk : SsrPackAbsent;
    logused = 0;
    logbuf[0] = 0;
    link_stop = 0;
    service_stopped = 0;
    ssr_core_init(&core, &ops, &cfg);
}

static void finish(pthread_t thread)
{
    pthread_mutex_lock(&lock);
    link_stop = 1;
    dsp.waiting = 0;
    pthread_mutex_unlock(&lock);
    ssr_core_channel(&core, 0);
    pthread_cond_broadcast(&turn);
    pthread_join(thread, 0);
}

/* ---- the scenarios ---- */
static void bring_up(void)
{
    pthread_t thread;
    start(1);
    dsp.refuse_attach = 2;
    dsp.ignore_attach = 1;
    pack_not_yet = 3;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2());
    /* the order, the process, and the first next2 answering nothing: ctx 0, result -1 */
    CHECK(dsp.attaches == 4 && dsp.attached == PROCESS && dsp.opens == 1 && dsp.registers == 1 && dsp.closes == 1 && dsp.inits == 1);
    CHECK(prepared_at_attach == 1);
    CHECK(dsp.prev_ctx == 0 && dsp.prev_result == -1 && dsp.prev_length == 0);
    CHECK(logged("attach refused value=32768 tries=2") && logged("attach unanswered value=0 tries=1") && logged("attached to the sensors PD"));
    CHECK(logged("pack entries=28") && logged("libraries=/vendor/dsp/sensors;/vendor/lib/rfsa/adsp") && logged("listening"));
    CHECK(core.status.stage == SsrStListen);

    /* the byte layout of the attach: the message and its metadata, as the facts say them (the context bank in bits 39:32, one in buffer) */
    {
        uint8_t expected[64];
        const uint32_t n = hex("00 00 00 00  92 10 00 00  02 01 00 00 00 00 00 00  01 00 00 00  00 00 01 00"
                               "00 00 10 d0 04 00 00 00  00 10 00 00 00 00 00 00", expected, sizeof expected);
        CHECK(same_bytes(first_message, 40, expected, n));
        const uint32_t k = hex("00 04 10 d0 00 00 00 00  04 00 00 00 00 00 00 00  01 00 00 00 00 00 00 00"
                               "00 00 10 d0 04 00 00 00  00 10 00 00 00 00 00 00", expected, sizeof expected);
        CHECK(same_bytes(first_metadata, 40, expected, k));
    }
    uint32_t std = 0, mem = 0, fd = 0, got = 0, eof = 0;
    CHECK(remotectl_open("apps_std", &std) == 0 && std == 1 && core.status.stage == SsrStServing);
    CHECK(remotectl_open("apps_mem", &mem) == 0 && mem == 2);
    CHECK(remotectl_open("adspmsgd_apps", &fd) == -5 && logged("dsp opens adspmsgd_apps: no such interface here"));

    /* absolute, through the env call; one read in pieces; EOF only on the short read */
    CHECK(std_fopen_env("ADSP_LIBRARY_PATH", "/vendor/etc/sensors/sns_reg_config", "r", &fd) == 0);
    CHECK(std_fread(fd, 10, &got, &eof) == 0 && got == 10 && !eof && !memcmp(reply.out[1], "version=1\n", 10));
    CHECK(std_fread(fd, 100, &got, &eof) == 0 && got == 20 && eof && !memcmp(reply.out[1], "file=input=json.lst\n", 20));
    for (unsigned i = 20; i < 100; ++i) CHECK(reply.out[1][i] == 0);
    CHECK(std_fread(fd, 8, &got, &eof) == 0 && got == 0 && eof);
    CHECK(std_word(12, fd, 4) == 0 && rd32(reply.out[0]) == 1);          /* feof */
    CHECK(std_one(11, fd) == 0);                                          /* rewind */
    CHECK(std_word(12, fd, 4) == 0 && rd32(reply.out[0]) == 0);
    CHECK(std_fseek(fd, 5, 0) == 0 && std_word(8, fd, 4) == 0 && rd32(reply.out[0]) == 5);
    CHECK(std_fseek(fd, -2, 1) == 0 && std_word(8, fd, 4) == 0 && rd32(reply.out[0]) == 3);
    CHECK(std_fseek(fd, -1, 2) == 0 && std_fread(fd, 8, &got, &eof) == 0 && got == 1 && eof && reply.out[1][0] == '\n');
    CHECK(std_fseek(fd, -100, 2) == SSR_EFAILED && std_fseek(fd, 0, 3) == SSR_EFAILED);
    CHECK(std_word(10, fd, 8) == 0 && rd64(reply.out[0]) == 30);          /* flen */
    CHECK(std_word(13, fd, 4) == 0 && rd32(reply.out[0]) == 0);           /* ferror */
    CHECK(std_one(2, fd) == 0 && std_one(23, fd) == 0 && std_one(14, fd) == 0);
    CHECK(std_one(3, fd) == 0 && std_one(3, fd) == SSR_EBADPARM);         /* closed twice */
    CHECK(std_fread(fd, 4, &got, &eof) == SSR_EBADPARM);

    /* a large file, across the bounce buffer's size */
    CHECK(std_fopen("/persist/sensors/registry/registry/sensors_registry", "rb", &fd) == 0);     /* the alias */
    CHECK(std_fread(fd, 0x18000, &got, &eof) == 0 && got == 70000 && eof);
    for (uint32_t i = 0; i < 70000; ++i) if (reply.out[1][i] != pattern(i)) CHECK(0);
    CHECK(std_one(3, fd) == 0);

    /* the library path from the pack: a relative name, the second directory misses, the first hits */
    CHECK(std_fopen_env("ADSP_LIBRARY_PATH", "libsns.so", "r", &fd) == 0 && logged("open /vendor/dsp/sensors/libsns.so bytes=3"));
    CHECK(std_one(3, fd) == 0);
    CHECK(std_fopen_env("ADSP_LIBRARY_PATH", "libnone.so", "r", &fd) == SSR_EFAILED);
    CHECK(logged("missing $ADSP_LIBRARY_PATH/libnone.so") && core.status.missing == 1 && !strcmp(core.status.note, "$ADSP_LIBRARY_PATH/libnone.so"));

    /* lines */
    CHECK(std_fopen("/system/vendor/etc/sensors/config/a.json", "r", &fd) == 0);
    {
        const uint32_t prim[2] = {fd, 32};
        const void *in[1] = {prim};
        const uint32_t in_n[1] = {8}, out_n[2] = {4, 32};
        CHECK(dsp_call(STD, 20, in, in_n, 1, out_n, 2) == 0 && !strcmp((char *)reply.out[1], "{\"a\":1}\n") && !rd32(reply.out[0]));
        CHECK(dsp_call(STD, 20, in, in_n, 1, out_n, 2) == 0 && !strcmp((char *)reply.out[1], "{\"b\":2}\n"));
        CHECK(dsp_call(STD, 20, in, in_n, 1, out_n, 2) == 0 && !strcmp((char *)reply.out[1], "last"));
        CHECK(dsp_call(STD, 20, in, in_n, 1, out_n, 2) == 0 && rd32(reply.out[0]) == 1);
    }
    CHECK(std_one(3, fd) == 0);

    /* an empty file, a zero-length read */
    CHECK(std_fopen("/vendor/etc/sensors/config/empty.json", "r", &fd) == 0);
    CHECK(std_fread(fd, 0, &got, &eof) == 0 && got == 0 && !eof);
    CHECK(std_one(3, fd) == 0);

    /* a directory: its children only, not the sibling with the same prefix, not grandchildren */
    CHECK(std_named(26, "/vendor/etc/sensors/", 8) == 0);
    {
        const uint64_t dir = rd64(reply.out[0]);
        const void *in[1] = {&dir};
        const uint32_t in_n[1] = {8}, out_n[1] = {264};
        char seen[3][64] = {{0}};
        unsigned count = 0;
        for (;;) {
            CHECK(dsp_call(STD, 28, in, in_n, 1, out_n, 1) == 0);
            if (rd32(reply.out[0] + 260)) break;
            CHECK(count < 3);
            strcpy(seen[count++], (char *)reply.out[0] + 4);
        }
        CHECK(count == 2 && !strcmp(seen[0], "config") && !strcmp(seen[1], "sns_reg_config"));
        CHECK(!reply.out[0][4]);
        CHECK(dsp_call(STD, 27, in, in_n, 1, 0, 0) == 0);
        CHECK(dsp_call(STD, 27, in, in_n, 1, 0, 0) == SSR_EBADPARM);
    }
    CHECK(std_named(26, "/vendor/etc/nothing", 8) == SSR_EFAILED && logged("missing dir /vendor/etc/nothing"));
    CHECK(std_named(26, "/vendor/etc/sensors/sns_reg_config", 8) == SSR_EFAILED);

    /* stat: a method above 30, 96 bytes */
    CHECK(std_named(31, "/vendor/etc/sensors/sns_reg_config", 96) == 0);
    CHECK(rd32(reply.out[0] + 24) == 0100444 && rd32(reply.out[0] + 28) == 1 && rd64(reply.out[0] + 40) == 30);
    CHECK(std_named(31, "/vendor/etc/sensors", 96) == 0 && rd32(reply.out[0] + 24) == 040555 && rd64(reply.out[0] + 40) == 0);
    CHECK(std_named(31, "/no/such", 96) == SSR_EFAILED);

    /* fileExists: one byte */
    CHECK(std_named(22, "/sys/devices/soc0/soc_id", 1) == 0 && reply.out[0][0] == 1);
    CHECK(std_named(22, "/sys/devices/soc0/nope", 1) == 0 && reply.out[0][0] == 0);

    /* getenv */
    {
        const char name[] = "ADSP_LIBRARY_PATH";
        const uint32_t prim[2] = {sizeof name, 64};
        const void *in[2] = {prim, name};
        const uint32_t in_n[2] = {8, sizeof name}, out_n[2] = {4, 64};
        CHECK(dsp_call(STD, 16, in, in_n, 2, out_n, 2) == 0 && rd32(reply.out[0]) == 42);
        CHECK(!strcmp((char *)reply.out[1], "/vendor/dsp/sensors;/vendor/lib/rfsa/adsp"));
        const char other[] = "HOME";
        const uint32_t prim2[2] = {sizeof other, 64};
        const void *in2[2] = {prim2, other};
        const uint32_t in_n2[2] = {8, sizeof other};
        CHECK(dsp_call(STD, 16, in2, in_n2, 2, out_n, 2) == -1);
    }

    /* what is refused: writes of every kind */
    CHECK(std_fopen("/mnt/vendor/persist/sensors/registry/registry/x", "w", &fd) == SSR_EUNSUPPORTED);
    CHECK(std_fopen_env("ADSP_LIBRARY_PATH", "/mnt/vendor/persist/sensors/x", "a+", &fd) == SSR_EUNSUPPORTED);
    CHECK(logged("refused write open /mnt/vendor/persist/sensors/registry/registry/x"));
    {
        const char name[] = "/mnt/vendor/persist/sensors/registry/registry";
        const uint32_t prim[2] = {sizeof name, 0755};
        const void *in[2] = {prim, name};
        const uint32_t in_n[2] = {8, sizeof name};
        CHECK(dsp_call(STD, 29, in, in_n, 2, 0, 0) == 0);                 /* mkdir of what exists */
        const char other[] = "/mnt/vendor/persist/sensors/registry/new";
        const uint32_t prim2[2] = {sizeof other, 0755};
        const void *in2[2] = {prim2, other};
        const uint32_t in_n2[2] = {8, sizeof other};
        CHECK(dsp_call(STD, 29, in2, in_n2, 2, 0, 0) == SSR_EUNSUPPORTED);
        const uint32_t write_prim[2] = {16, 4};
        const void *in3[2] = {write_prim, "data"};
        const uint32_t in_n3[2] = {8, 4}, out_n[1] = {8};
        CHECK(dsp_call(STD, 5, in3, in_n3, 2, out_n, 1) == SSR_EUNSUPPORTED);
    }

    /* an unsupported method, a method id above 31 in the first word, an unknown handle */
    CHECK(std_one(25, 16) == SSR_EUNSUPPORTED && logged("apps_std method id=25 unsupported"));
    {
        const uint32_t prim[2] = {36, 5};
        const void *in[2] = {prim, "name"};
        const uint32_t in_n[2] = {8, 5}, out_n[1] = {8};
        CHECK(dsp_call(STD, 36, in, in_n, 2, out_n, 1) == SSR_EUNSUPPORTED);
    }
    {
        const uint32_t word = 0;
        const void *in[1] = {&word};
        const uint32_t in_n[1] = {4};
        CHECK(dsp_call(9, 0, in, in_n, 1, 0, 0) == SSR_EUNSUPPORTED && logged("call on an unknown handle handle=9"));
    }

    /* print_string */
    {
        const char text[] = "sns_registry: done";
        const uint32_t prim[1] = {sizeof text};
        const void *in[2] = {prim, text};
        const uint32_t in_n[2] = {4, sizeof text};
        CHECK(dsp_call(STD, 15, in, in_n, 2, 0, 0) == 0 && logged("[sensors] dsp: sns_registry: done"));
    }

    /* apps_mem: the nested mmap between two next2, the heap grows and runs out */
    {
        uint64_t prim[4];
        memset(prim, 0, sizeof prim);
        ((uint32_t *)prim)[2] = 0x1000;              /* rflags: add pages */
        prim[3] = 0x4100;
        const void *in[1] = {prim};
        const uint32_t in_n[1] = {32}, out_n[1] = {16};
        CHECK(dsp_call(2, 2, in, in_n, 1, out_n, 1) == 0 && dsp.mmaps == 1 && rd64(reply.out[0]) == 0);
        CHECK((rd64(reply.out[0] + 8) & 0xfffff00000ull) == 0xe0000000ull && core.status.heap_used == 0x5000);
        prim[3] = SSR_ARENA_BYTES;
        CHECK(dsp_call(2, 2, in, in_n, 1, out_n, 1) == SSR_ENOMEMORY && dsp.mmaps == 1);
        prim[3] = 0x1000;
        ((uint32_t *)prim)[2] = 0;                   /* the DSP's own buffer: not supported */
        CHECK(dsp_call(2, 2, in, in_n, 1, out_n, 1) == SSR_ENOMEMORY);
        const void *in2[1] = {prim};
        const uint32_t in_n2[1] = {16};
        CHECK(dsp_call(2, 3, in2, in_n2, 1, 0, 0) == 0);    /* unmap */
    }

    /* descriptors run out, come back */
    {
        uint32_t fds[SSR_FILES + 1];
        for (unsigned i = 0; i < SSR_FILES; ++i) CHECK(std_fopen("/sys/devices/soc0/soc_id", "r", &fds[i]) == 0);
        CHECK(std_fopen("/sys/devices/soc0/soc_id", "r", &fds[SSR_FILES]) == SSR_ENOMEMORY);
        for (unsigned i = 0; i < SSR_FILES; ++i) CHECK(std_one(3, fds[i]) == 0);
    }

    /* a call that cannot be answered: an out buffer beyond the arena */
    {
        const uint32_t prim[2] = {16, 0x400000};
        const void *in[1] = {prim};
        const uint32_t in_n[1] = {8}, out_n[2] = {8, 0x400000};
        CHECK(dsp_call(STD, 4, in, in_n, 1, out_n, 2) == SSR_ENOMEMORY);
    }

    /* next2 fails once: the service tells the DSP on the next one, and goes on; INTERRUPTED restarts the context */
    CHECK(await_next2());
    release_next2(77, 0, 0, 0, 0);
    CHECK(await_next2() && dsp.prev_result == 77 && dsp.prev_length == 0);
    release_next2(SSR_EINTERRUPTED, 0, 0, 0, 0);
    CHECK(await_next2() && dsp.prev_ctx == 0 && dsp.prev_result == -1);

    /* the link goes: the session is over, files forgotten; it comes back: everything again, the pack kept */
    CHECK(std_fopen("/sys/devices/soc0/soc_id", "r", &fd) == 0);
    pthread_mutex_lock(&lock);
    const uint32_t attaches = dsp.attaches;
    dsp.waiting = 0;
    ssr_core_channel(&core, 0);
    pthread_cond_broadcast(&turn);
    pthread_mutex_unlock(&lock);
    for (;;) {
        pthread_mutex_lock(&lock);
        const int done = core.status.stage == SsrStChannel;
        pthread_mutex_unlock(&lock);
        if (done) break;
    }
    CHECK(logged("channel went away"));
    ssr_core_channel(&core, 1);
    CHECK(await_next2());
    CHECK(dsp.attaches == attaches + 1 && dsp.prev_ctx == 0 && dsp.prev_result == -1);
    CHECK(std_fread(fd, 4, &got, &eof) == SSR_EBADPARM);
    CHECK(std_fopen("/sys/devices/soc0/hw_platform", "r", &fd) == 0 && std_fread(fd, 16, &got, &eof) == 0 && got == 4);
    CHECK(!memcmp(reply.out[1], "MTP\n", 4));

    /* next2 fails twice in a row: stopped for good, and said */
    CHECK(await_next2());
    release_next2(77, 0, 0, 0, 0);
    CHECK(await_next2());
    release_next2(78, 0, 0, 0, 0);
    pthread_join(thread, 0);
    CHECK(core.status.stage == -SsrStServing && core.status.error == 78 && logged("FAIL st=-7 err=78 next2 failed twice"));
}

static void failures(void)
{
    pthread_t thread;
    /* no pack: the service still serves, every open fails and the paths are logged */
    start(0);
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("no file pack (no such file)"));
    uint32_t fd = 0;
    CHECK(std_fopen_env("ADSP_LIBRARY_PATH", "/vendor/etc/sensors/sns_reg_config", "r", &fd) == SSR_EFAILED);
    CHECK(logged("missing /vendor/etc/sensors/sns_reg_config") && !strcmp(core.status.note, "/vendor/etc/sensors/sns_reg_config"));
    finish(thread);

    /* a Data volume that never comes: after the wait, the same */
    start(0);
    pack_answer = SsrPackOk;
    pack_not_yet = 1 << 30;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("no file pack (the Data volume never came)"));
    pack_not_yet = 0;
    finish(thread);

    /* a truncated pack: refused, the same */
    start(1);
    pack_length -= 1;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("no file pack (not a pack, or truncated)"));
    finish(thread);

    /* the attach never taken: stopped after two minutes of tries */
    start(1);
    dsp.refuse_attach = 1 << 30;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    pthread_join(thread, 0);
    CHECK(core.status.stage == -SsrStAttach && dsp.attaches == 120 && logged("FAIL st=-4 err=32768 the sensors PD never took the attach"));

    /* the listener does not open */
    start(1);
    dsp.refuse_open = 1 << 30;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    pthread_join(thread, 0);
    CHECK(core.status.stage == -SsrStRegister && dsp.opens == 16 && logged("dlerr=7 dlopen failed"));

    /* sends refused: an attach is counted as a try */
    start(1);
    dsp.refuse_send = 3;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("attach not sent value=0 tries=3") && dsp.attached == PROCESS);
    finish(thread);

    /* another context bank: in every address */
    expected_bank = 3;
    start(1);
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("bank=3"));
    finish(thread);
    expected_bank = SSR_CONTEXT_BANK;

    /* the required stream refused: stopped before the first message */
    start(1);
    prepare_error = -16;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    pthread_join(thread, 0);
    prepare_error = 0;
    CHECK(core.status.stage == -SsrStSmmu && core.status.error == -16 && dsp.messages == 0 && logged("FAIL st=-3 err=-16"));
}

static unsigned count_lines(const char *text)
{
    unsigned n = 0;
    pthread_mutex_lock(&lock);
    for (const char *p = logbuf; (p = strstr(p, text)); ++p) n++;
    pthread_mutex_unlock(&lock);
    return n;
}

/* The ring is small: each kind of line has its own ration, what is missing the largest; a quiet DSP gets one summary. */
static void rationing(void)
{
    pthread_t thread;
    start_log = 1;
    start(1);
    start_log = 2;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2());
    uint32_t fd = 0;
    for (unsigned i = 0; i < 200; ++i) {
        CHECK(std_fopen("/sys/devices/soc0/soc_id", "r", &fd) == 0 && std_one(3, fd) == 0);
        CHECK(std_fopen("/no/such/file", "r", &fd) == SSR_EFAILED);
    }
    CHECK(count_lines("rpc open /sys") == 32 && count_lines("rpc missing /no") == 64 && core.status.opens == 200 && core.status.missing == 200);
    CHECK(!logged("rpc quiet"));
    __atomic_fetch_add(&now_ms, 11000, __ATOMIC_RELEASE);
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&turn);
    pthread_mutex_unlock(&lock);
    while (!logged("rpc quiet")) {
        pthread_mutex_lock(&lock);
        pthread_cond_broadcast(&turn);
        pthread_mutex_unlock(&lock);
    }
    CHECK(logged("rpc quiet calls=") && logged("opens=200 missing=200"));
    finish(thread);
}

/* The DSP's blobs mutated: never out of bounds (ASan watches the arena), always an answer of the shape checked by take_answer or an error. */
static void fuzz(void)
{
    pthread_t thread;
    uint32_t seed = 0x1234567;
    start(1);
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2());
    const uint32_t rounds = getenv("SENSORS_FUZZ") ? (uint32_t)atoi(getenv("SENSORS_FUZZ")) : 60000;
    for (uint32_t round = 0; round < rounds; ++round) {
        static const uint32_t methods[] = {0, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 15, 16, 19, 20, 22, 26, 27, 28, 29, 31};
        uint8_t blob[512];
        const uint32_t path_n = 35;
        const uint32_t prim[4] = {path_n, 2, 16, 8};
        const void *in[3] = {prim, "/vendor/etc/sensors/sns_reg_config", "r"};
        const uint32_t in_n[3] = {16, path_n, 2}, out_n[2] = {8, 64};
        uint32_t n = pack_call(blob, sizeof blob, in, in_n, 3, out_n, 2);
        seed = seed * 1103515245u + 12345u;
        const unsigned flips = (seed >> 16) % 6;
        for (unsigned i = 0; i < flips; ++i) {
            seed = seed * 1103515245u + 12345u;
            blob[(seed >> 8) % n] = (uint8_t)(seed >> 24);
        }
        seed = seed * 1103515245u + 12345u;
        if (!((seed >> 12) & 7)) n = (seed >> 16) % (n + 1);
        seed = seed * 1103515245u + 12345u;
        const uint32_t method = methods[(seed >> 16) % (sizeof methods / sizeof methods[0])];
        seed = seed * 1103515245u + 12345u;
        const uint32_t sc = (method > 30 ? 31u : method) << 24 | ((seed >> 8) & 7) << 16 | ((seed >> 12) & 7) << 8 | ((seed >> 20) & 31) == 0;
        const uint32_t handle = (seed >> 16) & 3;
        release_next2(0, blob, n, handle, sc);
        CHECK(await_next2());
        CHECK(dsp.prev_result || dsp.prev_length <= 0x100000);
        CHECK(!dsp.prev_result || dsp.prev_length == 0);
    }
    /* length words near the top of the range */
    static const uint32_t huge[] = {0xffffffffu, 0xfffffff8u, 0x7fffffffu, 0x80000000u, 0x10000u, 0xf000u};
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t blob[32];
        memset(blob, 0, sizeof blob);
        wr32(blob, huge[i]);
        release_next2(0, blob, sizeof blob, STD, 4u << 24 | 1 << 16 | 2 << 8);
        CHECK(await_next2() && dsp.prev_result);
        wr32(blob, 8);
        wr32(blob + 8, 16);
        wr32(blob + 16, huge[i]);
        wr32(blob + 20, huge[i]);
        release_next2(0, blob, 24, STD, 4u << 24 | 1 << 16 | 2 << 8);
        CHECK(await_next2());
    }
    /* a length beyond the capacity the DSP was told */
    {
        uint8_t blob[4] = {0};
        release_next2(0, blob, 0, STD, 0);
        CHECK(await_next2());
    }
    finish(thread);
}

static void paths(void)
{
    char out[SSP_PATH];
    static const struct {const char *in, *out;} table[] = {
        {"/a/b", "/a/b"}, {"//a///b/", "/a/b"}, {"/a/./b/.", "/a/b"}, {"/", "/"}, {"//", "/"},
        {"/system/vendor/etc/x", "/vendor/etc/x"}, {"/system/vendor", "/vendor"}, {"/system/vendorx/y", "/system/vendorx/y"},
        {"/persist/sensors", "/mnt/vendor/persist/sensors"}, {"/persist", "/mnt/vendor/persist"}, {"/persistent", "/persistent"},
        {"/mnt/vendor/persist/x", "/mnt/vendor/persist/x"}, {"/a...b/c", "/a...b/c"}, {"/a/.hidden", "/a/.hidden"},
    };
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; ++i) {
        const int n = ssp_path(table[i].in, (uint32_t)strlen(table[i].in) + 1, out, sizeof out);
        CHECK(n == (int)strlen(table[i].out) && !strcmp(out, table[i].out));
    }
    CHECK(ssp_path("/a/b", 2, out, sizeof out) == 2 && !strcmp(out, "/a"));         /* the length bounds it */
    CHECK(ssp_path("a/b", 4, out, sizeof out) == -1 && ssp_path("", 1, out, sizeof out) == -1 && ssp_path(0, 1, out, sizeof out) == -1);
    CHECK(ssp_path("/a/../b", 8, out, sizeof out) == -1 && ssp_path("/..", 4, out, sizeof out) == -1);
    char longer[600];
    memset(longer, 'a', sizeof longer);
    longer[0] = '/';
    longer[599] = 0;
    CHECK(ssp_path(longer, 600, out, sizeof out) == -1);
    CHECK(ssp_path("/persist/abc", 13, out, 8) == -1 && ssp_path("/abc", 5, out, 4) == -1 && ssp_path("/abc", 5, out, 5) == 4);
}

static void pack_tables(void)
{
    static uint8_t p[0x40000], q[0x40000];
    const uint32_t n = build_pack(p, sizeof p, kItems, sizeof kItems / sizeof kItems[0]);
    SspHeader h;
    SspPack pack;
    CHECK(!ssp_header(p, n, &h) && h.total == n && h.count == sizeof kItems / sizeof kItems[0]);
    CHECK(ssp_header(p, 31, &h) == -1);
    CHECK(!ssp_open(&pack, &h, p + SSP_HEADER));
    for (unsigned i = 0; i < h.count; ++i) {
        CHECK(ssp_find(&pack, kItems[i].name) == (int)i);
        SspEntry e;
        ssp_entry(&pack, i, &e);
        CHECK(!strcmp(e.name, kItems[i].name));
    }
    CHECK(ssp_find(&pack, "/vendor/etc/sensor") == -1 && ssp_find(&pack, "/zzz") == -1 && ssp_find(&pack, "") == -1);
    /* the root's children */
    uint32_t cursor = 0;
    SspEntry child;
    const char *leaf = 0;
    unsigned count = 0;
    while (ssp_child(&pack, 0, &cursor, &child, &leaf) == 1) count++;
    CHECK(count == 4);       /* .pagani, mnt, sys, vendor */
    /* the empty pack */
    SspPack none;
    memset(&none, 0, sizeof none);
    CHECK(ssp_find(&none, "/") == -1);
    cursor = 0;
    CHECK(ssp_child(&none, 0, &cursor, &child, &leaf) == 0);
    /* any byte of the header or the table changed: refused (the size in the header is held to the file's by the loader) */
    const uint32_t table = SSP_HEADER + ssp_table_bytes(&h);
    for (uint32_t i = 0; i < table; ++i) {
        if (i >= 16 && i < 24) continue;
        for (unsigned bit = 0; bit < 8; bit += 3) {
            memcpy(q, p, n);
            q[i] ^= (uint8_t)(1u << bit);
            SspHeader mutated;
            if (ssp_header(q, n, &mutated)) continue;
            if (ssp_table_bytes(&mutated) + SSP_HEADER > n) continue;
            CHECK(ssp_open(&pack, &mutated, q + SSP_HEADER) == -1);      /* the sum sees every single change */
        }
    }
    /* sums made right: the structural checks must hold by themselves */
    for (uint32_t i = SSP_HEADER; i < table; ++i) {
        memcpy(q, p, n);
        q[i] ^= 0x40;
        uint32_t sum = 0;
        for (uint32_t k = SSP_HEADER; k < table; ++k) sum += q[k];
        wr32(q + 24, sum);
        SspHeader mutated;
        if (ssp_header(q, n, &mutated)) continue;
        if (!ssp_open(&pack, &mutated, q + SSP_HEADER)) {
            for (unsigned e = 0; e < pack.count; ++e) {
                SspEntry x;
                ssp_entry(&pack, e, &x);
                CHECK(x.name[0] == '/' && (x.flags & 1 ? !x.offset && !x.size : x.offset + x.size <= n));
                CHECK(ssp_find(&pack, x.name) == (int)e);
            }
        }
    }
    /* unsorted, repeated names */
    static const Item bad[] = {{"/b", "x", 0}, {"/a", "y", 0}};
    const uint32_t m = build_pack(q, sizeof q, bad, 2);
    CHECK(!ssp_header(q, m, &h) && ssp_open(&pack, &h, q + SSP_HEADER) == -1);
    static const Item twice[] = {{"/a", "x", 0}, {"/a", "y", 0}};
    const uint32_t k = build_pack(q, sizeof q, twice, 2);
    CHECK(!ssp_header(q, k, &h) && ssp_open(&pack, &h, q + SSP_HEADER) == -1);
}

/* A pack from scripts/make-sensor-pack.py, as the runner's dump made it (the same contents every time). */
static void tool_pack(const char *path)
{
    pthread_t thread;
    static uint8_t bytes[1 << 22];
    FILE *f = fopen(path, "rb");
    CHECK(f);
    const size_t n = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    CHECK(n > SSP_HEADER && n < sizeof bytes);
    start(0);
    pack_bytes = bytes;
    pack_length = (uint32_t)n;
    pack_answer = SsrPackOk;
    pthread_create(&thread, 0, service, 0);
    ssr_core_channel(&core, 1);
    CHECK(await_next2() && logged("pack entries=") && logged("libraries=/vendor/dsp/sensors;/x"));
    uint32_t fd = 0, got = 0, eof = 0;
    CHECK(std_fopen("/vendor/etc/sensors/sns_reg_config", "r", &fd) == 0 && std_fread(fd, 64, &got, &eof) == 0 && got == 10);
    CHECK(!memcmp(reply.out[1], "version=1\n", 10));
    CHECK(std_fopen("/vendor/etc/sensors/alias.json", "r", &fd) == 0 && std_fread(fd, 64, &got, &eof) == 0 && got == 2);
    CHECK(!memcmp(reply.out[1], "{}", 2));
    CHECK(std_fopen("/mnt/vendor/persist/sensors/registry/registry/sensors_registry", "r", &fd) == 0);
    CHECK(std_fread(fd, 64, &got, &eof) == 0 && got == 5 && !memcmp(reply.out[1], "calib", 5));
    CHECK(std_fopen("/sys/devices/soc0/soc_id", "r", &fd) == 0 && std_fread(fd, 64, &got, &eof) == 0 && got == 4 && !memcmp(reply.out[1], "618\n", 4));
    CHECK(std_fopen("/vendor/extra/added", "r", &fd) == 0 && std_fread(fd, 64, &got, &eof) == 0 && got == 3 && !memcmp(reply.out[1], "add", 3));
    CHECK(std_fopen("/vendor/etc/sensors/escape", "r", &fd) == SSR_EFAILED);
    finish(thread);
}

int main(int argc, char **argv)
{
    arena = malloc(SSR_ARENA_BYTES);
    CHECK(arena);
    paths();
    pack_tables();
    bring_up();
    failures();
    rationing();
    fuzz();
    if (argc > 1) tool_pack(argv[1]);
    free(arena);
    printf("PASS FastRPC file service against a simulated DSP, the pack: %u checks\n", checks);
    return 0;
}
