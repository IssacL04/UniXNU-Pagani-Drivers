/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/WifiProbe.cpp rddmScan (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e); WifiRddm.h says what
 * changed. */
#include "WifiRddm.h"
#include "WifiUtil.h"

static int printable(uint8_t c) {return c >= 0x20 && c < 0x7f;}

static void say(const PwRddmIo *io, PwRddmResult *r, const char *line)
{
    io->log(io->ctx, line);
    if (++r->lines % PW_RDDM_PACE_LINES == 0 && io->msleep) io->msleep(io->ctx, PW_RDDM_PACE_MS);
}

static int same_bytes(const uint8_t *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

/* A format string of the image is not a message; a source file with a line number, an assertion or a fatal error text is. */
static int looks_like_error(const uint8_t *t, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (t[i] == '%') return 0;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (t[i] == '.' && t[i + 1] == 'c' && t[i + 2] == ':' && t[i + 3] >= '0' && t[i + 3] <= '9') return 1;
        if (i + 5 <= n && (same_bytes(t + i, "ssert", 5) || same_bytes(t + i, "SSERT", 5) || same_bytes(t + i, "FATAL", 5))) return 1;
        if (i + 8 <= n && same_bytes(t + i, "rr_fatal", 8)) return 1;
    }
    return 0;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Segment e of the table in the window, or null. */
static const uint8_t *segment(const PwRddmIo *io, const PwRddmResult *r, uint32_t e, uint64_t *size)
{
    if ((uint64_t)(e + 1) * 16 > r->table_bytes) return 0;
    const uint64_t addr = get64(r->table + e * 16);
    *size = get64(r->table + e * 16 + 8);
    return *size ? io->to_va(io->ctx, addr, *size) : 0;
}

/* The text at seg + at in the line's tail, nonprintable ones already excluded. */
static void text_line(const PwRddmIo *io, PwRddmResult *r, const char *tag, uint32_t e, uint64_t at, const uint8_t *text, size_t n)
{
    char line[112];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, tag);
    pw_text_char(&t, ' ');
    pw_text_udec(&t, e);
    pw_text_char(&t, '+');
    pw_text_hex(&t, at);
    pw_text_str(&t, ": ");
    pw_text_printable(&t, (const char *)text, n < 84 ? n : 84);
    say(io, r, line);
}

/* The texts of a dump: at most PW_RDDM_MESSAGES different ones. */
static void scan_messages(const PwRddmIo *io, PwRddmResult *r)
{
    uint32_t seen[PW_RDDM_MESSAGES];
    for (uint32_t e = 0; (uint64_t)(e + 1) * 16 <= r->table_bytes && r->messages < PW_RDDM_MESSAGES; ++e) {
        uint64_t size = 0;
        const uint8_t *seg = segment(io, r, e, &size);
        if (!seg) continue;
        r->scanned += size;
        for (uint64_t i = 0; i < size && r->messages < PW_RDDM_MESSAGES;) {
            if (!printable(seg[i])) {++i; continue;}
            uint64_t j = i;
            while (j < size && printable(seg[j]) && j - i < 200) ++j;
            const size_t n = (size_t)(j - i);
            if (n >= 10 && looks_like_error(seg + i, n)) {
                uint32_t h = 2166136261u;
                for (size_t k = 0; k < n; ++k) h = (h ^ seg[i + k]) * 16777619u;
                int dup = 0;
                for (uint32_t k = 0; k < r->messages; ++k) if (seen[k] == h) dup = 1;
                if (!dup) {
                    seen[r->messages++] = h;
                    text_line(io, r, "rddm", e, i, seg + i, n);
                }
            }
            i = j;
        }
    }
}

/* Every text in the crash record (segment 0 up to 0x1000), and the runtime log the firmware kept before it (segment 1, 0x78000 on): the
 * first as it is, the second without format strings. */
static void scan_context(const PwRddmIo *io, PwRddmResult *r)
{
    static const struct {uint32_t seg; uint64_t from, to; uint32_t min; int formats;} ranges[] = {
        {0, 0, 0x1000, 4, 1}, {0, 0xec00, 0xf400, 4, 1}, {1, 0x78000, 0x80000, 12, 0},
    };
    for (unsigned k = 0; k < sizeof ranges / sizeof ranges[0]; ++k) {
        uint64_t size = 0;
        const uint8_t *seg = segment(io, r, ranges[k].seg, &size);
        if (!seg) continue;
        const uint64_t to = ranges[k].to < size ? ranges[k].to : size;
        for (uint64_t i = ranges[k].from; i < to && r->context_lines < PW_RDDM_CONTEXT_LINES;) {
            if (!printable(seg[i])) {++i; continue;}
            uint64_t j = i;
            while (j < to && printable(seg[j]) && j - i < 200) ++j;
            const size_t n = (size_t)(j - i);
            int format = 0;
            for (size_t m = 0; m < n; ++m) if (seg[i + m] == '%') format = 1;
            if (n >= ranges[k].min && (ranges[k].formats || !format)) {
                text_line(io, r, "rddm-ctx", ranges[k].seg, i, seg + i, n);
                ++r->context_lines;
            }
            i = j;
        }
    }
}

/* The crash record's first 0x200 bytes as words: registers and addresses sit there. */
static void record_words(const PwRddmIo *io, PwRddmResult *r)
{
    uint64_t size = 0;
    const uint8_t *seg = segment(io, r, 0, &size);
    if (!seg || size < 0x200) return;
    for (uint32_t o = 0; o < 0x200; o += 32) {
        char line[112];
        PwText t;
        pw_text_start(&t, line, sizeof line);
        pw_text_str(&t, "rddm-rec");
        pw_text_kx(&t, "at", o);
        for (unsigned w = 0; w < 8; ++w) {
            pw_text_char(&t, ' ');
            pw_text_hex(&t, get32(seg + o + 4 * w));
        }
        say(io, r, line);
        ++r->record_lines;
    }
}

int pw_rddm_scan(const PwRddmIo *io, PwRddmResult *r)
{
    pw_zero(r, sizeof *r);
    const uint32_t bhie = io->read(io->ctx, PW_RDDM_BHIEOFF);
    if (!bhie || bhie > PW_RDDM_BHIE_MAX || (bhie & 3)) {
        char line[80];
        PwText t;
        pw_text_start(&t, line, sizeof line);
        pw_text_str(&t, "rddm: no BHIe block");
        pw_text_kx(&t, "bhieoff", bhie);
        say(io, r, line);
        return PwRddmNoBhie;
    }
    uint32_t waited = 0;
    for (; waited < PW_RDDM_WAIT_STEPS; ++waited) {
        r->status = io->read(io->ctx, bhie + PW_RDDM_RXVECSTATUS);
        if ((r->status >> PW_RDDM_STATUS_SHIFT) >= PW_RDDM_DONE) break;
        io->msleep(io->ctx, 10);
    }
    r->waited_ms = waited * 10;
    r->table_bus = io->read(io->ctx, bhie + PW_RDDM_RXVECADDR_LOW) | (uint64_t)io->read(io->ctx, bhie + PW_RDDM_RXVECADDR_HIGH) << 32;
    r->table_bytes = io->read(io->ctx, bhie + PW_RDDM_RXVECSIZE);
    char line[112];
    PwText t;
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "rddm upload:");
    pw_text_kx(&t, "status", r->status);
    pw_text_kd(&t, "ms", r->waited_ms);
    pw_text_kx(&t, "table", r->table_bus);
    pw_text_kd(&t, "bytes", r->table_bytes);
    say(io, r, line);
    if (r->status == 0xffffffffu || (r->status >> PW_RDDM_STATUS_SHIFT) != PW_RDDM_DONE) return PwRddmNotDone;
    if (r->table_bytes < 16 || r->table_bytes > PW_RDDM_TABLE_MAX || (r->table_bytes & 15)) return PwRddmNoTable;
    r->table = io->to_va(io->ctx, r->table_bus, r->table_bytes);
    if (!r->table) return PwRddmNoTable;
    scan_messages(io, r);
    pw_text_start(&t, line, sizeof line);
    pw_text_str(&t, "rddm scan:");
    pw_text_kd(&t, "bytes", (int64_t)r->scanned);
    pw_text_kd(&t, "messages", r->messages);
    say(io, r, line);
    scan_context(io, r);
    record_words(io, r);
    return PwRddmOk;
}

/* ---- the export ---- */
static const char kExportMagic[] = "SHEXR001", kStreamMagic[] = "M2VCACH1", kRoot[] = "/rddm";
enum {kFileHeadBytes = 24, kTableName = 9, kSegName = 9};       /* "table.bin", "seg00.bin" */

uint64_t pw_rddm_export_bytes(const PwRddmIo *io, const PwRddmResult *r)
{
    if (!r->table) return 0;
    uint64_t total = 8 + 4 + (sizeof kRoot - 1) + 4 + kFileHeadBytes + kTableName + r->table_bytes;
    for (uint32_t e = 0; (uint64_t)(e + 1) * 16 <= r->table_bytes; ++e) {
        uint64_t size = 0;
        if (segment(io, r, e, &size)) total += kFileHeadBytes + kSegName + size;
    }
    return 16 + total <= PW_RDDM_EXPORT_MAX ? 16 + total : 0;
}

typedef struct {uint8_t *to; uint64_t left; int short_of_room;} Out;
static void put(Out *o, const void *data, uint64_t n)
{
    if (n > o->left) {o->short_of_room = 1; return;}
    pw_copy(o->to, data, (size_t)n);
    o->to += n;
    o->left -= n;
}
static void put32(Out *o, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(o, b, 4);
}
static void put_file(Out *o, const char *name, uint32_t name_bytes, const uint8_t *data, uint64_t bytes)
{
    put32(o, name_bytes);
    put32(o, 0644);
    put32(o, 0);
    put32(o, 0);
    put32(o, (uint32_t)bytes);
    put32(o, (uint32_t)(bytes >> 32));
    put(o, name, name_bytes);
    put(o, data, bytes);
}

uint64_t pw_rddm_export_fill(const PwRddmIo *io, const PwRddmResult *r, uint8_t *blob, uint64_t bytes)
{
    if (!r->table || bytes < 16 || bytes != pw_rddm_export_bytes(io, r)) return 0;
    Out o = {blob, bytes, 0};
    uint32_t files = 1;
    for (uint32_t e = 0; (uint64_t)(e + 1) * 16 <= r->table_bytes; ++e) {
        uint64_t size = 0;
        if (segment(io, r, e, &size)) ++files;
    }
    put(&o, kExportMagic, 8);
    put32(&o, (uint32_t)(bytes - 16));
    put32(&o, 0);
    put(&o, kStreamMagic, 8);
    put32(&o, sizeof kRoot - 1);
    put(&o, kRoot, sizeof kRoot - 1);
    put32(&o, files);
    put_file(&o, "table.bin", kTableName, r->table, r->table_bytes);
    for (uint32_t e = 0; (uint64_t)(e + 1) * 16 <= r->table_bytes; ++e) {
        uint64_t size = 0;
        const uint8_t *seg = segment(io, r, e, &size);
        if (!seg) continue;
        char name[10] = "seg00.bin";
        name[3] = (char)('0' + (e / 10) % 10);
        name[4] = (char)('0' + e % 10);
        put_file(&o, name, kSegName, seg, size);
    }
    return o.short_of_room || o.left ? 0 : bytes;
}
