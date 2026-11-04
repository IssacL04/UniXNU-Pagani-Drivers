/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The FastRPC reverse file service of the sensor PD: see SensorsRpc.h. Single threaded (the service's own thread), except the two
 * entry points the DSP thread calls, which only record. The arena is uncached memory the DSP reads and writes: every access goes through
 * the byte helpers below, nothing assumes alignment. */
#include "SensorsRpc.h"
#include "SensorsText.h"

enum {
    /* the arena */
    kMeta = 0,
    kPrimIn = 0x400,
    kPrimOut = 0x800,
    kInBlob = 0x1000,
    kInCapacity = 0xf000,
    kOutBlob = 0x10000,
    /* the protocol */
    kHandleRemotectl = 0, kHandleInit = 1, kHandleListener = 3,
    kLocalRemotectl = 0, kLocalStd = 1, kLocalMem = 2,
    kMethodAttach = 0, kMethodMmap = 4, kMethodInit2 = 3, kMethodNext2 = 4, kMethodOpen = 0, kMethodClose = 1,
    kExtended = 31,                    /* the method id is the first word of in buffer 0 */
    kFdBytes = 16 * 8, kCrcBytes = 64 * 4,
    kPage = 0x1000,
    kAddPages = 0x1000,                /* apps_mem rflags: the application processor gives the pages */
    kMapMethod = 2, kUnmapMethod = 3,
    kMaxBuffers = 8,
    /* the patience */
    kInvokeMs = 5000,
    kAttachTries = 120,
    kOpenTries = 16,
    kRetryMs = 1000,
    /* The event ring is shared and small (32 KiB, about 250 records): what is missing matters most. Lines per boot: */
    kBudget = 40, kOpenLines = 32, kMissingLines = 64, kSummaries = 6,
    kSummaryMs = 10000,                /* the DSP quiet this long after some work: one summary line */
    kErrorLength = 64,
};
enum {PhaseChannel, PhasePack, PhaseAttach, PhaseOpen, PhaseRegister, PhaseClose, PhaseInit, PhaseServe, PhaseStopped};
enum {InvokeOk = 0, InvokeRefused = -1, InvokeTimeout = -2, InvokeLink = -3};

#define SC(method, in, out) ((uint32_t)(method) << 24 | (uint32_t)(in) << 16 | (uint32_t)(out) << 8)
#define CTX (c->ops->context)

typedef struct {
    uint32_t offset, length;
} Arg;
typedef struct {
    uint8_t *p;
    uint32_t n;
} Buf;
typedef struct {
    Buf in[kMaxBuffers], out[kMaxBuffers];
    unsigned in_count, out_count;
    uint32_t method;
} Call;

static void put32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void put64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t get64(const uint8_t *p) {return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;}
static void clear(uint8_t *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) p[i] = 0;}
static void copy(uint8_t *to, const uint8_t *from, uint32_t n) {for (uint32_t i = 0; i < n; ++i) to[i] = from[i];}
static int same(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] && a[i] == b[i]; ++i) {}
    return a[i] == b[i];
}
static uint64_t now(SsrCore *c) {return c->ops->now_ms(CTX);}

/* ---- the log ---- */
static SntText *begin(SsrCore *c, SntText *t, const char *what)
{
    snt_start(t, c->line, sizeof c->line);
    snt_str(t, "[sensors] rpc ");
    snt_str(t, what);
    return t;
}
static void end(SsrCore *c)
{
    if (c->ops->trace) c->ops->trace(CTX, c->line);
}
static int ordinary(SsrCore *c)
{
    if (c->cfg.log >= 2) return 1;
    if (!c->cfg.log || !c->budget) return 0;
    c->budget--;
    return 1;
}
/* Within a counter of its own (always with pagani-sensors-log=2, never with 0). */
static int rationed(SsrCore *c, unsigned *lines, unsigned limit)
{
    if (c->cfg.log >= 2) return 1;
    if (!c->cfg.log || *lines >= limit) return 0;
    (*lines)++;
    return 1;
}
static void text_of(SntText *t, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n && p[i]; ++i) snt_char(t, p[i] >= 32 && p[i] < 127 ? (char)p[i] : '.');
}

static void summary(SsrCore *c)
{
    SntText t;
    c->summary_due = 0;
    if (!rationed(c, &c->summary_lines, kSummaries)) return;
    begin(c, &t, "quiet");
    snt_kd(&t, "calls", c->status.calls);
    snt_kd(&t, "opens", c->status.opens);
    snt_kd(&t, "missing", c->status.missing);
    snt_kd(&t, "reads", c->status.reads);
    snt_kd(&t, "bytes", (int64_t)c->status.bytes);
    snt_kd(&t, "refused", c->status.refused);
    snt_kd(&t, "unknown", c->status.unknown);
    snt_kd(&t, "heap", c->status.heap_used);
    end(c);
}

/* The service stops for good: the stage, the reason. */
static int stop(SsrCore *c, int32_t stage, int32_t error, const char *why)
{
    SntText t;
    c->status.stage = -stage;
    c->status.error = error;
    c->phase = PhaseStopped;
    begin(c, &t, "FAIL");
    snt_kd(&t, "st", -stage);
    snt_kd(&t, "err", error);
    snt_char(&t, ' ');
    snt_str(&t, why);
    end(c);
    return 0;
}

/* ---- the DSP thread's two entry points ---- */
void ssr_core_channel(SsrCore *c, int up)
{
    __atomic_store_n(&c->link_up, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&c->link_changes, 1u, __ATOMIC_RELEASE);
}

void ssr_core_receive(SsrCore *c, const uint8_t *data, uint32_t length)
{
    if (length < 12) return;
    uint64_t ctx = 0;
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; ++i) ctx |= (uint64_t)data[i] << (8 * i);
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)data[8 + i] << (8 * i);
    __atomic_store_n(&c->response_value, (int32_t)value, __ATOMIC_RELEASE);
    __atomic_store_n(&c->response_ctx, ctx, __ATOMIC_RELEASE);
    __atomic_fetch_add(&c->responses, 1u, __ATOMIC_RELEASE);
}

/* ---- one invoke ---- */
static uint64_t bus(const SsrCore *c, uint64_t pa) {return (uint64_t)c->cfg.bank << 32 | pa;}

/* The metadata of n buffers at the start of the arena; the buffers (arena offsets) in place. */
static void metadata(SsrCore *c, const Arg *args, uint32_t n)
{
    uint8_t *a = c->cfg.arena + kMeta;
    const uint32_t list = n * 16, pages = list + n * 8;
    clear(a, pages + n * 16 + kFdBytes + kCrcBytes);
    for (uint32_t i = 0; i < n; ++i) {
        put32(a + list + i * 8 + 4, i);
        if (!args[i].length) continue;
        const uint64_t pa = c->cfg.arena_pa + args[i].offset;
        const uint64_t first = pa & ~(uint64_t)(kPage - 1), last = (pa + args[i].length - 1) & ~(uint64_t)(kPage - 1);
        put64(a + i * 16, pa);
        put64(a + i * 16 + 8, args[i].length);
        put32(a + list + i * 8, 1);
        put64(a + pages + i * 16, bus(c, first));
        put64(a + pages + i * 16 + 8, last - first + kPage);
    }
}

/* kernel: the bring-up's invokes carry process id 0. timeout 0: as long as it takes (next2). */
static int invoke(SsrCore *c, int kernel, uint32_t handle, uint32_t sc, const Arg *args, uint32_t timeout_ms, int32_t *value)
{
    const uint32_t n = ((sc >> 16) & 0xff) + ((sc >> 8) & 0xff);
    uint8_t message[SSR_MESSAGE];
    c->next_id = c->next_id >= 255 ? 1 : c->next_id + 1;
    const uint64_t ctx = (uint64_t)c->next_id << 8 | SSR_PD_SENSORS;
    if (n) metadata(c, args, n);
    put32(message, kernel ? 0 : c->cfg.process);
    put32(message + 4, c->cfg.process);
    put64(message + 8, ctx);
    put32(message + 16, handle);
    put32(message + 20, sc);
    put64(message + 24, n ? bus(c, c->cfg.arena_pa + kMeta) : 0);
    put64(message + 32, n ? kPage : 0);
    uint32_t seen = __atomic_load_n(&c->responses, __ATOMIC_ACQUIRE);
    if (c->ops->publish) c->ops->publish(CTX);
    if (c->ops->send(CTX, message, sizeof message)) return InvokeRefused;
    const uint64_t since = now(c);
    for (;;) {
        const uint32_t responses = __atomic_load_n(&c->responses, __ATOMIC_ACQUIRE);
        if (responses != seen) {
            seen = responses;
            if (__atomic_load_n(&c->response_ctx, __ATOMIC_ACQUIRE) == ctx) {
                *value = __atomic_load_n(&c->response_value, __ATOMIC_ACQUIRE);
                return InvokeOk;
            }
            c->stray++;
            continue;
        }
        if (__atomic_load_n(&c->link_changes, __ATOMIC_ACQUIRE) != c->seen_changes) return InvokeLink;
        uint32_t ms = 1000;
        const uint64_t waited = now(c) - since;
        if (timeout_ms) {
            if (waited >= timeout_ms) return InvokeTimeout;
            if (timeout_ms - waited < ms) ms = (uint32_t)(timeout_ms - waited);
        } else if (c->summary_due && waited >= kSummaryMs) {
            summary(c);
        }
        c->ops->wait(CTX, ms);
    }
}

/* ---- the pack ---- */
static void empty_pack(SsrCore *c, const char *why)
{
    SntText t;
    c->pack.table = 0;
    c->pack.count = 0;
    begin(c, &t, "no file pack (");
    snt_str(&t, why);
    snt_str(&t, "): every open fails, the paths the DSP asks for are logged");
    end(c);
}

static void load_pack(SsrCore *c, uint64_t size)
{
    SspHeader h;
    SntText t;
    uint8_t *header = c->cfg.bounce;
    if (size < SSP_HEADER || c->ops->pack_read(CTX, 0, header, SSP_HEADER)) return empty_pack(c, "unreadable");
    if (ssp_header(header, SSP_HEADER, &h) || h.total != size) return empty_pack(c, "not a pack, or truncated");
    const uint32_t bytes = ssp_table_bytes(&h);
    uint8_t *table = (uint8_t *)c->ops->alloc(CTX, bytes);
    if (!table) return empty_pack(c, "no memory for its table");
    for (uint32_t done = 0; done < bytes;) {
        const uint32_t chunk = bytes - done < SSR_BOUNCE ? bytes - done : SSR_BOUNCE;
        if (c->ops->pack_read(CTX, SSP_HEADER + done, table + done, chunk)) return empty_pack(c, "table unreadable");
        done += chunk;
    }
    if (ssp_open(&c->pack, &h, table)) return empty_pack(c, "bad table");
    const int override = ssp_find(&c->pack, SSR_LIBRARY_PATH_FILE);
    if (override >= 0) {
        SspEntry e;
        ssp_entry(&c->pack, (uint32_t)override, &e);
        if (e.size && e.size < sizeof c->library_path && !c->ops->pack_read(CTX, e.offset, c->library_path, (uint32_t)e.size)) {
            uint32_t n = (uint32_t)e.size;
            while (n && (c->library_path[n - 1] == '\n' || !c->library_path[n - 1])) n--;
            c->library_path[n] = 0;
        }
    }
    if (ordinary(c)) {
        begin(c, &t, "pack");
        snt_kd(&t, "entries", h.count);
        snt_kd(&t, "bytes", (int64_t)h.total);
        snt_ks(&t, "libraries", c->library_path);
        end(c);
    }
}

/* ---- the file calls ---- */
static SsrFile *file_of(SsrCore *c, uint32_t fd)
{
    const uint32_t i = fd - 16;
    return i < SSR_FILES && c->file[i].used ? &c->file[i] : 0;
}

static void missing(SsrCore *c, const char *what, const char *path)
{
    SntText t;
    c->status.missing++;
    if (!c->status.note[0]) {
        snt_start(&t, c->status.note, sizeof c->status.note);
        snt_str(&t, path);
    }
    if (rationed(c, &c->missing_lines, kMissingLines)) {
        begin(c, &t, what);
        snt_char(&t, ' ');
        snt_str(&t, path);
        end(c);
    }
}

static int32_t open_entry(SsrCore *c, const char *path, int index, uint32_t *fd)
{
    SntText t;
    SspEntry e;
    ssp_entry(&c->pack, (uint32_t)index, &e);
    if (e.flags & SSP_DIRECTORY) return SSR_EFAILED;
    for (uint32_t i = 0; i < SSR_FILES; ++i) {
        if (c->file[i].used) continue;
        c->file[i].used = 1;
        c->file[i].entry = (uint32_t)index;
        c->file[i].position = 0;
        *fd = i + 16;
        c->status.opens++;
        if (rationed(c, &c->open_lines, kOpenLines)) {
            begin(c, &t, "open ");
            snt_str(&t, path);
            snt_kd(&t, "bytes", (int64_t)e.size);
            end(c);
        }
        return SSR_OK;
    }
    return SSR_ENOMEMORY;
}

/* A string argument: its bytes end with their NUL. */
static int text_arg(const Buf *b)
{
    return b->n > 0 && b->n < SSP_PATH * 4 && b->p[b->n - 1] == 0;
}

static int write_mode(const Buf *mode)
{
    for (uint32_t i = 0; i + 1 < mode->n; ++i) if (mode->p[i] == 'w' || mode->p[i] == 'a' || mode->p[i] == '+') return 1;
    return 0;
}

static int32_t refuse(SsrCore *c, const char *what, const Buf *name)
{
    SntText t;
    c->status.refused++;
    if (ordinary(c)) {
        begin(c, &t, "refused ");
        snt_str(&t, what);
        if (name) {snt_char(&t, ' '); text_of(&t, name->p, name->n);}
        end(c);
    }
    return SSR_EUNSUPPORTED;
}

static int32_t std_open(SsrCore *c, const Buf *name, const Buf *mode, uint32_t *fd)
{
    char path[SSP_PATH];
    if (!text_arg(name) || !text_arg(mode)) return SSR_EBADPARM;
    if (write_mode(mode)) return refuse(c, "write open", name);
    if (ssp_path((const char *)name->p, name->n, path, sizeof path) < 0) return SSR_EFAILED;
    const int index = ssp_find(&c->pack, path);
    if (index < 0) {
        missing(c, "missing", path);
        return SSR_EFAILED;
    }
    return open_entry(c, path, index, fd);
}

static int32_t std_open_env(SsrCore *c, const Buf *env, const Buf *name, const Buf *mode, uint32_t *fd)
{
    char joined[SSP_PATH], path[SSP_PATH];
    if (!text_arg(env) || !text_arg(name) || !text_arg(mode)) return SSR_EBADPARM;
    if (name->p[0] == '/') return std_open(c, name, mode, fd);
    if (write_mode(mode)) return refuse(c, "write open", name);
    const char *list = same((const char *)env->p, "ADSP_AVS_CFG_PATH") ? "/vendor/etc/acdbdata" : c->library_path;
    for (uint32_t at = 0;;) {
        uint32_t n = 0;
        while (list[at] && list[at] != ';' && n + 1 < sizeof joined) joined[n++] = list[at++];
        if (n + 1 + name->n <= sizeof joined) {
            joined[n++] = '/';
            copy((uint8_t *)joined + n, name->p, name->n);
            if (ssp_path(joined, n + name->n, path, sizeof path) >= 0) {
                const int index = ssp_find(&c->pack, path);
                if (index >= 0) return open_entry(c, path, index, fd);
            }
        }
        while (list[at] && list[at] != ';') at++;
        if (!list[at]) break;
        at++;
    }
    SntText t;
    snt_start(&t, joined, sizeof joined);
    snt_char(&t, '$');
    text_of(&t, env->p, env->n);
    snt_char(&t, '/');
    text_of(&t, name->p, name->n);
    missing(c, "missing", joined);
    return SSR_EFAILED;
}

/* n bytes of the pack at offset into the arena. */
static int32_t read_bytes(SsrCore *c, uint64_t offset, uint8_t *to, uint32_t n)
{
    for (uint32_t done = 0; done < n;) {
        const uint32_t chunk = n - done < SSR_BOUNCE ? n - done : SSR_BOUNCE;
        if (c->ops->pack_read(CTX, offset + done, c->cfg.bounce, chunk)) return SSR_EFAILED;
        copy(to + done, c->cfg.bounce, chunk);
        done += chunk;
    }
    return SSR_OK;
}

static int32_t std_read(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 8 || k->out_count < 2 || k->out[0].n < 8) return SSR_EBADPARM;
    SsrFile *f = file_of(c, get32(k->in[0].p));
    const uint32_t asked = get32(k->in[0].p + 4);
    if (!f || asked > k->out[1].n) return SSR_EBADPARM;
    SspEntry e;
    ssp_entry(&c->pack, f->entry, &e);
    const uint64_t left = f->position < e.size ? e.size - f->position : 0;
    const uint32_t n = left < asked ? (uint32_t)left : asked;
    const int32_t rc = read_bytes(c, e.offset + f->position, k->out[1].p, n);
    if (rc) return rc;
    f->position += n;
    c->status.reads++;
    c->status.bytes += n;
    put32(k->out[0].p, n);
    put32(k->out[0].p + 4, n < asked);
    return SSR_OK;
}

static int32_t std_gets(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 8 || k->out_count < 2 || k->out[0].n < 4) return SSR_EBADPARM;
    SsrFile *f = file_of(c, get32(k->in[0].p));
    uint32_t asked = get32(k->in[0].p + 4);
    if (!f || asked > k->out[1].n) return SSR_EBADPARM;
    SspEntry e;
    ssp_entry(&c->pack, f->entry, &e);
    if (!asked || f->position >= e.size) {
        put32(k->out[0].p, 1);
        return SSR_OK;
    }
    if (asked > SSR_BOUNCE) asked = SSR_BOUNCE;
    const uint64_t left = e.size - f->position;
    uint32_t n = left < asked - 1 ? (uint32_t)left : asked - 1;
    if (n && c->ops->pack_read(CTX, e.offset + f->position, c->cfg.bounce, n)) return SSR_EFAILED;
    for (uint32_t i = 0; i < n; ++i) if (c->cfg.bounce[i] == '\n') {n = i + 1; break;}
    copy(k->out[1].p, c->cfg.bounce, n);
    k->out[1].p[n] = 0;
    f->position += n;
    put32(k->out[0].p, 0);
    return SSR_OK;
}

static int32_t std_seek(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 12) return SSR_EBADPARM;
    SsrFile *f = file_of(c, get32(k->in[0].p));
    if (!f) return SSR_EBADPARM;
    SspEntry e;
    ssp_entry(&c->pack, f->entry, &e);
    const int64_t offset = (int32_t)get32(k->in[0].p + 4);
    const uint32_t whence = get32(k->in[0].p + 8);
    const int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->position : whence == 2 ? (int64_t)e.size : -1;
    if (base < 0 || base + offset < 0) return SSR_EFAILED;
    f->position = (uint64_t)(base + offset);
    return SSR_OK;
}

static int32_t std_position(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 4) return SSR_EBADPARM;
    SsrFile *f = file_of(c, get32(k->in[0].p));
    if (!f) return SSR_EBADPARM;
    SspEntry e;
    ssp_entry(&c->pack, f->entry, &e);
    switch (k->method) {
    case 8:                                       /* ftell */
        if (k->out_count < 1 || k->out[0].n < 4) return SSR_EBADPARM;
        put32(k->out[0].p, (uint32_t)f->position);
        return SSR_OK;
    case 10:                                      /* flen */
        if (k->out_count < 1 || k->out[0].n < 8) return SSR_EBADPARM;
        put64(k->out[0].p, e.size);
        return SSR_OK;
    case 11:                                      /* rewind */
        f->position = 0;
        return SSR_OK;
    case 12:                                      /* feof */
        if (k->out_count < 1 || k->out[0].n < 4) return SSR_EBADPARM;
        put32(k->out[0].p, f->position >= e.size);
        return SSR_OK;
    default:                                      /* ferror (13): there are none */
        if (k->out_count < 1 || k->out[0].n < 4) return SSR_EBADPARM;
        put32(k->out[0].p, 0);
        return SSR_OK;
    }
}

static int32_t std_close(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 4) return SSR_EBADPARM;
    SsrFile *f = file_of(c, get32(k->in[0].p));
    if (!f) return SSR_EBADPARM;
    f->used = 0;
    return SSR_OK;
}

static int32_t std_getenv(SsrCore *c, const Call *k)
{
    if (k->in_count < 2 || !text_arg(&k->in[1]) || k->out_count < 2 || k->out[0].n < 4) return SSR_EBADPARM;
    const char *name = (const char *)k->in[1].p;
    const char *value = same(name, "ADSP_LIBRARY_PATH") || same(name, "DSP_LIBRARY_PATH") ? c->library_path :
                        same(name, "ADSP_AVS_CFG_PATH") ? "/vendor/etc/acdbdata" : 0;
    if (!value) return -1;                        /* as a libc getenv failure with errno 0 reads on Android */
    uint32_t n = 0;
    while (value[n]) n++;
    put32(k->out[0].p, n + 1);
    if (k->out[1].n) {
        const uint32_t room = n + 1 < k->out[1].n ? n + 1 : k->out[1].n;
        copy(k->out[1].p, (const uint8_t *)value, room - 1);
        k->out[1].p[room - 1] = 0;
    }
    return SSR_OK;
}

static int32_t std_opendir(SsrCore *c, const Call *k)
{
    char path[SSP_PATH];
    SspEntry e;
    if (k->in_count < 2 || !text_arg(&k->in[1]) || k->out_count < 1 || k->out[0].n < 8) return SSR_EBADPARM;
    if (ssp_path((const char *)k->in[1].p, k->in[1].n, path, sizeof path) < 0) return SSR_EFAILED;
    const int index = ssp_find(&c->pack, path);
    if (index < 0) {
        missing(c, "missing dir", path);
        return SSR_EFAILED;
    }
    ssp_entry(&c->pack, (uint32_t)index, &e);
    if (!(e.flags & SSP_DIRECTORY)) return SSR_EFAILED;
    for (uint32_t i = 0; i < SSR_DIRS; ++i) {
        if (c->dir[i].used) continue;
        c->dir[i].used = 1;
        c->dir[i].entry = (uint32_t)index;
        c->dir[i].cursor = 0;
        put64(k->out[0].p, i + 1);
        return SSR_OK;
    }
    return SSR_ENOMEMORY;
}

static SsrDir *dir_of(SsrCore *c, const Call *k)
{
    if (k->in_count < 1 || k->in[0].n < 8) return 0;
    const uint64_t handle = get64(k->in[0].p);
    if (!handle || handle > SSR_DIRS || !c->dir[handle - 1].used) return 0;
    return &c->dir[handle - 1];
}

static int32_t std_readdir(SsrCore *c, const Call *k)
{
    SsrDir *d = dir_of(c, k);
    SspEntry e;
    const char *leaf = 0;
    if (!d || k->out_count < 1 || k->out[0].n < 264) return SSR_EBADPARM;
    if (ssp_child(&c->pack, d->entry, &d->cursor, &e, &leaf) != 1) {
        put32(k->out[0].p + 260, 1);
        return SSR_OK;
    }
    uint32_t n = 0;
    for (; leaf[n] && n < 255; ++n) k->out[0].p[4 + n] = (uint8_t)leaf[n];
    return SSR_OK;
}

static int32_t std_closedir(SsrCore *c, const Call *k)
{
    SsrDir *d = dir_of(c, k);
    if (!d) return SSR_EBADPARM;
    d->used = 0;
    return SSR_OK;
}

static int32_t std_stat(SsrCore *c, const Call *k)
{
    char path[SSP_PATH];
    SspEntry e;
    if (k->in_count < 2 || !text_arg(&k->in[1]) || k->out_count < 1 || k->out[0].n < 96) return SSR_EBADPARM;
    if (ssp_path((const char *)k->in[1].p, k->in[1].n, path, sizeof path) < 0) return SSR_EFAILED;
    const int index = ssp_find(&c->pack, path);
    if (index < 0) {
        missing(c, "missing stat", path);
        return SSR_EFAILED;
    }
    ssp_entry(&c->pack, (uint32_t)index, &e);
    /* tsz, dev, ino (u64), mode, nlink (u32), rdev, size, then six times (u64): only these matter, the rest stays zero */
    put32(k->out[0].p + 24, e.flags & SSP_DIRECTORY ? 040555 : 0100444);
    put32(k->out[0].p + 28, 1);
    put64(k->out[0].p + 40, e.size);
    return SSR_OK;
}

static int32_t std_exists(SsrCore *c, const Call *k)
{
    char path[SSP_PATH];
    if (k->in_count < 2 || !text_arg(&k->in[1]) || k->out_count < 1 || k->out[0].n < 1) return SSR_EBADPARM;
    if (ssp_path((const char *)k->in[1].p, k->in[1].n, path, sizeof path) < 0) return SSR_EFAILED;
    const int index = ssp_find(&c->pack, path);
    k->out[0].p[0] = index >= 0;
    if (index < 0 && c->cfg.log >= 2) missing(c, "exists no", path);
    return SSR_OK;
}

static int32_t std_mkdir(SsrCore *c, const Call *k)
{
    char path[SSP_PATH];
    SspEntry e;
    if (k->in_count < 2 || !text_arg(&k->in[1])) return SSR_EBADPARM;
    if (ssp_path((const char *)k->in[1].p, k->in[1].n, path, sizeof path) < 0) return SSR_EFAILED;
    const int index = ssp_find(&c->pack, path);
    if (index >= 0) {
        ssp_entry(&c->pack, (uint32_t)index, &e);
        if (e.flags & SSP_DIRECTORY) return SSR_OK;
    }
    return refuse(c, "mkdir", &k->in[1]);
}

static int32_t std_print(SsrCore *c, const Call *k)
{
    SntText t;
    if (k->in_count < 2 || !k->in[1].n) return SSR_EBADPARM;
    if (ordinary(c)) {
        snt_start(&t, c->line, sizeof c->line);
        snt_str(&t, "[sensors] dsp: ");
        text_of(&t, k->in[1].p, k->in[1].n < 180 ? k->in[1].n : 180);
        end(c);
    }
    return SSR_OK;
}

static int32_t apps_std(SsrCore *c, const Call *k)
{
    SntText t;
    uint32_t fd = 0;
    int32_t rc;
    switch (k->method) {
    case 0:                                       /* fopen(name, mode) */
        if (k->in_count < 3 || k->out_count < 1 || k->out[0].n < 4) return SSR_EBADPARM;
        rc = std_open(c, &k->in[1], &k->in[2], &fd);
        if (!rc) put32(k->out[0].p, fd);
        return rc;
    case 2: case 14: case 17: case 18: case 23:   /* fflush, clearerr, setenv, unsetenv, fsync */
        return SSR_OK;
    case 3: return std_close(c, k);
    case 4: return std_read(c, k);
    case 5: return refuse(c, "fwrite", 0);
    case 8: case 10: case 11: case 12: case 13: return std_position(c, k);
    case 9: return std_seek(c, k);
    case 15: return std_print(c, k);
    case 16: return std_getenv(c, k);
    case 19:                                      /* fopen_with_env(env, delimiter, name, mode) */
        if (k->in_count < 5 || k->out_count < 1 || k->out[0].n < 4) return SSR_EBADPARM;
        rc = std_open_env(c, &k->in[1], &k->in[3], &k->in[4], &fd);
        if (!rc) put32(k->out[0].p, fd);
        return rc;
    case 20: return std_gets(c, k);
    case 22: return std_exists(c, k);
    case 24: return refuse(c, "fremove", k->in_count > 1 ? &k->in[1] : 0);
    case 26: return std_opendir(c, k);
    case 27: return std_closedir(c, k);
    case 28: return std_readdir(c, k);
    case 29: return std_mkdir(c, k);
    case 30: return refuse(c, "rmdir", k->in_count > 1 ? &k->in[1] : 0);
    case 31: return std_stat(c, k);
    case 32: return refuse(c, "ftrunc", 0);
    case 33: return refuse(c, "frename", k->in_count > 1 ? &k->in[1] : 0);
    default:
        c->status.unknown++;
        if (ordinary(c)) {
            begin(c, &t, "apps_std method");
            snt_kd(&t, "id", k->method);
            snt_str(&t, " unsupported");
            end(c);
        }
        return SSR_EUNSUPPORTED;
    }
}

/* ---- the other local interfaces ---- */
static int32_t apps_remotectl(SsrCore *c, const Call *k)
{
    SntText t;
    if (k->out_count < 2) return SSR_EBADPARM;
    if (k->method == kMethodOpen) {
        if (k->in_count < 2 || !text_arg(&k->in[1]) || k->out[0].n < 8) return SSR_EBADPARM;
        const char *name = (const char *)k->in[1].p;
        const uint32_t handle = same(name, "apps_std") ? kLocalStd : same(name, "apps_mem") ? kLocalMem : 0;
        if (ordinary(c) || !handle) {
            begin(c, &t, "dsp opens ");
            text_of(&t, k->in[1].p, k->in[1].n);
            if (!handle) snt_str(&t, ": no such interface here");
            end(c);
        }
        if (!handle) {
            c->status.unknown++;
            return -5;                                /* Android's answer for an unknown name */
        }
        put32(k->out[0].p, handle);
        return SSR_OK;
    }
    if (k->method == kMethodClose) return SSR_OK;
    return SSR_EUNSUPPORTED;
}

/* map64: pages of the arena's heap, made known to the DSP's memory manager with the mmap method of the init handle. */
static int32_t apps_mem(SsrCore *c, const Call *k)
{
    SntText t;
    if (k->method == kUnmapMethod) return SSR_OK;     /* the pages stay the DSP's until reboot */
    if (k->method != kMapMethod || k->in_count < 1 || k->in[0].n < 32 || k->out_count < 1 || k->out[0].n < 16) return SSR_EUNSUPPORTED;
    const uint32_t rflags = get32(k->in[0].p + 8);
    const uint64_t asked = get64(k->in[0].p + 24);
    const uint64_t length = (asked + kPage - 1) & ~(uint64_t)(kPage - 1);
    const uint32_t heap = c->cfg.arena_size > SSR_CALL_BYTES ? c->cfg.arena_size - SSR_CALL_BYTES : 0;
    if (!(rflags & kAddPages) || !length || length > heap - c->heap_used) {
        if (ordinary(c)) {
            begin(c, &t, "apps_mem refused");
            snt_kx(&t, "rflags", rflags);
            snt_kx(&t, "bytes", asked);
            snt_kx(&t, "used", c->heap_used);
            end(c);
        }
        return SSR_ENOMEMORY;
    }
    uint8_t *a = c->cfg.arena;
    const uint64_t pa = c->cfg.arena_pa + SSR_CALL_BYTES + c->heap_used;
    clear(a + kPrimIn, 0x80);
    put32(a + kPrimIn, c->cfg.process);
    put32(a + kPrimIn + 4, rflags);
    put32(a + kPrimIn + 16, 16);
    put64(a + kPrimIn + 32, bus(c, pa));
    put64(a + kPrimIn + 40, length);
    const Arg args[3] = {{kPrimIn, 24}, {kPrimIn + 32, 16}, {kPrimOut, 8}};
    int32_t value = 0;
    const int rc = invoke(c, 1, kHandleInit, SC(kMethodMmap, 2, 1), args, kInvokeMs, &value);
    if (rc || value) {
        begin(c, &t, "apps_mem mmap failed");
        snt_kd(&t, "rc", rc);
        snt_kd(&t, "value", value);
        end(c);
        return SSR_EFAILED;
    }
    c->heap_used += (uint32_t)length;
    c->status.heap_used = c->heap_used;
    put64(k->out[0].p, 0);
    put64(k->out[0].p + 8, get64(a + kPrimOut) & 0xffffffffu);
    if (ordinary(c)) {
        begin(c, &t, "apps_mem");
        snt_kx(&t, "bytes", length);
        snt_kx(&t, "pa", pa);
        snt_kx(&t, "dsp", get64(a + kPrimOut));
        end(c);
    }
    return SSR_OK;
}

/* ---- one call of the DSP ---- */
/* The call's blob, length bytes at the in area: in buffers, then the out lengths; the answer's blob laid out, zeroed. */
static int32_t unpack(SsrCore *c, uint32_t sc, uint32_t length, Call *k)
{
    uint8_t *blob = c->cfg.arena + kInBlob, *out = c->cfg.arena + kOutBlob;
    const uint32_t capacity = SSR_CALL_BYTES - kOutBlob;
    uint32_t at = 0, written = 0;
    k->in_count = (sc >> 16) & 0xff;
    k->out_count = (sc >> 8) & 0xff;
    k->method = (sc >> 24) & 0x1f;
    if (k->in_count > kMaxBuffers || k->out_count > kMaxBuffers || (sc & 0xff)) return SSR_EBADPARM;
    for (unsigned i = 0; i < k->in_count; ++i) {
        if (length - at < 4 || at > length) return SSR_EBADPARM;
        const uint32_t n = get32(blob + at);
        at += 4;
        k->in[i].n = n;
        k->in[i].p = blob;
        if (!n) continue;
        at = (at + 7) & ~7u;
        if (at > length || n > length - at) return SSR_EBADPARM;
        k->in[i].p = blob + at;
        at += n;
    }
    for (unsigned i = 0; i < k->out_count; ++i) {
        if (at > length || length - at < 4) return SSR_EBADPARM;
        const uint32_t n = get32(blob + at);
        at += 4;
        if (written > capacity || capacity - written < 4) return SSR_ENOMEMORY;
        put32(out + written, n);
        written += 4;
        k->out[i].n = n;
        k->out[i].p = out;
        if (!n) continue;
        written = (written + 7) & ~7u;
        if (written > capacity || n > capacity - written) return SSR_ENOMEMORY;
        k->out[i].p = out + written;
        clear(out + written, n);
        written += n;
    }
    c->out_length = written;
    if (k->method == kExtended) {
        if (!k->in_count || k->in[0].n < 4) return SSR_EBADPARM;
        k->method = get32(k->in[0].p);
    }
    return SSR_OK;
}

static int32_t call(SsrCore *c, uint32_t handle, uint32_t sc, uint32_t length)
{
    Call k;
    SntText t;
    c->out_length = 0;
    if (length > kInCapacity) return SSR_EBADSIZE;
    int32_t rc = unpack(c, sc, length, &k);
    if (rc) return rc;
    c->status.calls++;
    switch (handle) {
    case kLocalRemotectl: return apps_remotectl(c, &k);
    case kLocalStd: return apps_std(c, &k);
    case kLocalMem: return apps_mem(c, &k);
    default:
        c->status.unknown++;
        if (ordinary(c)) {
            begin(c, &t, "call on an unknown handle");
            snt_kd(&t, "handle", handle);
            snt_kx(&t, "sc", sc);
            end(c);
        }
        return SSR_EUNSUPPORTED;
    }
}

static int serve_one(SsrCore *c)
{
    SntText t;
    uint8_t *a = c->cfg.arena;
    int32_t value = 0;
    put32(a + kPrimIn, c->rctx);
    put32(a + kPrimIn + 4, (uint32_t)c->result);
    put32(a + kPrimIn + 8, c->out_length);
    put32(a + kPrimIn + 12, kInCapacity);
    const Arg args[4] = {{kPrimIn, 16}, {kOutBlob, c->out_length}, {kPrimOut, 16}, {kInBlob, kInCapacity}};
    const int rc = invoke(c, 0, kHandleListener, SC(kMethodNext2, 2, 2), args, 0, &value);
    if (rc == InvokeLink) return 1;
    if (rc) return stop(c, SsrStServing, rc, "next2 not sent");
    c->out_length = 0;
    if (value) {
        if (value == SSR_EINTERRUPTED) {          /* the DSP restarted the wait: its context is gone */
            c->rctx = 0;
            c->result = -1;
            return 1;
        }
        if (c->next_failures++) return stop(c, SsrStServing, value, "next2 failed twice");
        begin(c, &t, "next2 failed");
        snt_kd(&t, "value", value);
        end(c);
        c->result = value;
        return 1;
    }
    c->next_failures = 0;
    c->rctx = get32(a + kPrimOut);
    const uint32_t handle = get32(a + kPrimOut + 4), sc = get32(a + kPrimOut + 8), length = get32(a + kPrimOut + 12);
    if (c->status.stage == SsrStListen) {
        c->status.stage = SsrStServing;
        if (ordinary(c)) {
            begin(c, &t, "first call of the DSP");
            snt_kd(&t, "handle", handle);
            snt_kx(&t, "sc", sc);
            end(c);
        }
    }
    c->result = call(c, handle, sc, length);
    if (c->result) c->out_length = 0;
    c->summary_due = 1;
    return 1;
}

/* ---- the bring-up ---- */
static void forget_session(SsrCore *c)
{
    for (uint32_t i = 0; i < SSR_FILES; ++i) c->file[i].used = 0;
    for (uint32_t i = 0; i < SSR_DIRS; ++i) c->dir[i].used = 0;
    c->heap_used = 0;
    c->rctx = 0;
    c->result = -1;
    c->out_length = 0;
    c->next_failures = 0;
    c->tries = 0;
    c->retry_at = 0;
}

static int attach(SsrCore *c)
{
    SntText t;
    if (!c->prepared) {
        const int error = c->ops->prepare ? c->ops->prepare(CTX) : 0;
        if (error) return stop(c, SsrStSmmu, error, "the sensors PD's stream stays out of pass-through");
        c->prepared = 1;
        c->status.stage = SsrStSmmu;
    }
    const uint64_t at = now(c);
    if (at < c->retry_at) {
        c->ops->wait(CTX, (uint32_t)(c->retry_at - at));
        return 1;
    }
    int32_t value = 0;
    put32(c->cfg.arena + kPrimIn, c->cfg.process);
    const Arg arg = {kPrimIn, 4};
    c->status.attaches++;
    const int rc = invoke(c, 1, kHandleInit, SC(kMethodAttach, 1, 0), &arg, kInvokeMs, &value);
    if (rc == InvokeLink) return 1;
    if (!rc && !value) {
        c->status.stage = SsrStAttach;
        c->tries = 0;
        c->phase = PhaseOpen;
        if (ordinary(c)) {
            begin(c, &t, "attached to the sensors PD");
            snt_kx(&t, "pa", c->cfg.arena_pa);
            snt_kd(&t, "bank", c->cfg.bank);
            snt_kd(&t, "ms", (int64_t)(at - c->started_ms));
            end(c);
        }
        return 1;
    }
    c->tries++;
    c->retry_at = at + kRetryMs;
    if (c->tries <= 3 || !(c->tries % 16)) {
        begin(c, &t, "attach");
        snt_str(&t, rc == InvokeTimeout ? " unanswered" : rc ? " not sent" : " refused");
        snt_kd(&t, "value", value);
        snt_kd(&t, "tries", c->tries);
        end(c);
    }
    if (c->tries >= kAttachTries) return stop(c, SsrStAttach, rc ? rc : value, "the sensors PD never took the attach");
    return 1;
}

static int open_listener(SsrCore *c)
{
    static const char name[] = "adsp_default_listener";
    SntText t;
    uint8_t *a = c->cfg.arena;
    int32_t value = 0;
    clear(a + kPrimOut, 16 + kErrorLength);
    put32(a + kPrimIn, sizeof name);
    put32(a + kPrimIn + 4, kErrorLength);
    copy(a + kPrimIn + 16, (const uint8_t *)name, sizeof name);
    const Arg args[4] = {{kPrimIn, 8}, {kPrimIn + 16, sizeof name}, {kPrimOut, 8}, {kPrimOut + 16, kErrorLength}};
    const int rc = invoke(c, 0, kHandleRemotectl, SC(kMethodOpen, 2, 2), args, kInvokeMs, &value);
    if (rc == InvokeLink) return 1;
    if (!rc && !value && !get32(a + kPrimOut + 4)) {
        c->listener_handle = get32(a + kPrimOut);
        c->tries = 0;
        c->phase = PhaseRegister;
        return 1;
    }
    c->tries++;
    begin(c, &t, "remotectl open failed");
    snt_kd(&t, "rc", rc);
    snt_kd(&t, "value", value);
    snt_kd(&t, "dlerr", (int32_t)get32(a + kPrimOut + 4));
    snt_char(&t, ' ');
    text_of(&t, a + kPrimOut + 16, kErrorLength);
    end(c);
    if (c->tries >= kOpenTries) return stop(c, SsrStRegister, rc ? rc : value, "adsp_default_listener did not open");
    c->ops->wait(CTX, kRetryMs);
    return 1;
}

static int register_listener(SsrCore *c)
{
    int32_t value = 0;
    const int rc = invoke(c, 0, c->listener_handle, SC(0, 0, 0), 0, kInvokeMs, &value);
    if (rc == InvokeLink) return 1;
    if (rc || value) return stop(c, SsrStRegister, rc ? rc : value, "adsp_default_listener register failed");
    c->status.stage = SsrStRegister;
    c->phase = PhaseClose;
    return 1;
}

static int close_handle(SsrCore *c)
{
    uint8_t *a = c->cfg.arena;
    int32_t value = 0;
    put32(a + kPrimIn, c->listener_handle);
    put32(a + kPrimIn + 4, 0);
    const Arg args[3] = {{kPrimIn, 8}, {kPrimOut, 4}, {kPrimOut + 16, 0}};
    const int rc = invoke(c, 0, kHandleRemotectl, SC(kMethodClose, 1, 2), args, kInvokeMs, &value);
    if (rc == InvokeLink) return 1;
    c->phase = PhaseInit;                         /* a close that failed changes nothing */
    return 1;
}

static int listener_init(SsrCore *c)
{
    SntText t;
    int32_t value = 0;
    const int rc = invoke(c, 0, kHandleListener, SC(kMethodInit2, 0, 0), 0, kInvokeMs, &value);
    if (rc == InvokeLink) return 1;
    if (rc || value) return stop(c, SsrStListen, rc ? rc : value, "adsp_listener init2 failed");
    c->status.stage = SsrStListen;
    c->rctx = 0;
    c->result = -1;
    c->out_length = 0;
    c->phase = PhaseServe;
    if (ordinary(c)) {
        begin(c, &t, "listening");
        snt_kd(&t, "ms", (int64_t)(now(c) - c->started_ms));
        end(c);
    }
    return 1;
}

void ssr_core_init(SsrCore *c, const SsrOps *ops, const SsrConfig *config)
{
    uint8_t *b = (uint8_t *)c;
    for (uint32_t i = 0; i < sizeof *c; ++i) b[i] = 0;
    c->ops = ops;
    c->cfg = *config;
    c->budget = kBudget;
    c->phase = PhaseChannel;
    c->result = -1;
    c->started_ms = now(c);
    SntText t;
    snt_start(&t, c->library_path, sizeof c->library_path);
    snt_str(&t, SSR_LIBRARY_PATH);
}

int ssr_core_step(SsrCore *c)
{
    SntText t;
    const uint32_t changes = __atomic_load_n(&c->link_changes, __ATOMIC_ACQUIRE);
    if (changes != c->seen_changes) {
        c->seen_changes = changes;
        if (c->phase > PhaseChannel && c->phase != PhaseStopped) {
            begin(c, &t, "channel went away: the session is over");
            end(c);
            forget_session(c);
            c->phase = PhaseChannel;
            c->status.stage = SsrStChannel;
        }
    }
    switch (c->phase) {
    case PhaseChannel:
        if (!__atomic_load_n(&c->link_up, __ATOMIC_ACQUIRE)) {
            c->ops->wait(CTX, kRetryMs);
            return 1;
        }
        c->status.stage = SsrStChannel;
        if (ordinary(c)) {
            begin(c, &t, "channel up");
            snt_kd(&t, "ms", (int64_t)(now(c) - c->started_ms));
            end(c);
        }
        c->phase = c->pack_done ? PhaseAttach : PhasePack;
        c->pack_since = now(c);
        return 1;
    case PhasePack: {
        uint64_t size = 0;
        const int rc = c->ops->pack_size(CTX, &size);
        if (rc == SsrPackNotYet) {
            if (now(c) - c->pack_since < SSR_PACK_WAIT_MS) {
                c->ops->wait(CTX, kRetryMs);
                return 1;
            }
            empty_pack(c, "the Data volume never came");
        } else if (rc == SsrPackOk) {
            load_pack(c, size);
        } else {
            empty_pack(c, rc == SsrPackAbsent ? "no such file" : "unreadable");
        }
        c->pack_done = 1;
        c->status.stage = SsrStPack;
        c->phase = PhaseAttach;
        return 1;
    }
    case PhaseAttach: return attach(c);
    case PhaseOpen: return open_listener(c);
    case PhaseRegister: return register_listener(c);
    case PhaseClose: return close_handle(c);
    case PhaseInit: return listener_init(c);
    case PhaseServe: return serve_one(c);
    default: return 0;
    }
}
