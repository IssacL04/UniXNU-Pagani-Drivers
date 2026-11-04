/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The read-only file pack: see SensorsPack.h. */
#include "SensorsPack.h"

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;}

static int compare(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] && a[i] == b[i]; ++i) {}
    return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
}

int ssp_header(const uint8_t *bytes, uint32_t length, SspHeader *out)
{
    static const char magic[] = "SSCPACK1";
    if (!bytes || length < SSP_HEADER) return -1;
    for (unsigned i = 0; i < 8; ++i) if (bytes[i] != (uint8_t)magic[i]) return -1;
    out->count = le32(bytes + 8);
    out->names = le32(bytes + 12);
    out->total = le64(bytes + 16);
    out->sum = le32(bytes + 24);
    if (le32(bytes + 28) || !out->count || out->count > SSP_MAX_ENTRIES || !out->names || out->names > SSP_MAX_NAMES) return -1;
    if (out->total < (uint64_t)SSP_HEADER + ssp_table_bytes(out)) return -1;
    return 0;
}

uint32_t ssp_table_bytes(const SspHeader *h)
{
    return h->count * SSP_ENTRY + h->names;
}

void ssp_entry(const SspPack *p, uint32_t index, SspEntry *out)
{
    const uint8_t *e = p->table + (uint64_t)index * SSP_ENTRY;
    out->offset = le64(e);
    out->size = le64(e + 8);
    out->name = (const char *)p->table + p->count * SSP_ENTRY + le32(e + 16);
    out->flags = le32(e + 20);
}

int ssp_open(SspPack *p, const SspHeader *h, const uint8_t *table)
{
    const uint32_t bytes = ssp_table_bytes(h);
    uint32_t sum = 0;
    p->table = 0;
    p->count = p->names = 0;
    p->total = 0;
    if (!table) return -1;
    for (uint32_t i = 0; i < bytes; ++i) sum += table[i];
    if (sum != h->sum) return -1;
    const uint8_t *names = table + h->count * SSP_ENTRY;
    if (names[h->names - 1]) return -1;                  /* every name below ends inside the area */
    const uint64_t data = (uint64_t)SSP_HEADER + bytes;
    const char *previous = 0;
    for (uint32_t i = 0; i < h->count; ++i) {
        const uint8_t *e = table + (uint64_t)i * SSP_ENTRY;
        const uint64_t offset = le64(e), size = le64(e + 8);
        const uint32_t name = le32(e + 16), flags = le32(e + 20);
        if (name >= h->names || flags > SSP_DIRECTORY || names[name] != '/') return -1;
        if (flags & SSP_DIRECTORY ? offset || size : offset < data || size > h->total || offset > h->total - size) return -1;
        const char *path = (const char *)names + name;
        if (previous && compare(previous, path) >= 0) return -1;
        previous = path;
    }
    p->table = table;
    p->count = h->count;
    p->names = h->names;
    p->total = h->total;
    return 0;
}

int ssp_find(const SspPack *p, const char *path)
{
    uint32_t low = 0, high = p->count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        SspEntry e;
        ssp_entry(p, middle, &e);
        const int order = compare(path, e.name);
        if (!order) return (int)middle;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return -1;
}

static int starts(const char *text, const char *prefix)
{
    uint32_t i = 0;
    for (; prefix[i]; ++i) if (text[i] != prefix[i]) return 0;
    return text[i] == 0 || text[i] == '/';
}

int ssp_path(const char *in, uint32_t length, char *out, uint32_t capacity)
{
    char clean[SSP_PATH];
    uint32_t used = 0, at = 0;
    if (!in || !length || in[0] != '/') return -1;
    while (at < length && in[at]) {
        while (at < length && in[at] == '/') at++;
        const uint32_t start = at;
        while (at < length && in[at] && in[at] != '/') at++;
        const uint32_t n = at - start;
        if (!n || (n == 1 && in[start] == '.')) continue;
        if (n == 2 && in[start] == '.' && in[start + 1] == '.') return -1;
        if (used + 1 + n + 1 > sizeof clean) return -1;
        clean[used++] = '/';
        for (uint32_t i = 0; i < n; ++i) clean[used++] = in[start + i];
    }
    if (!used) clean[used++] = '/';
    clean[used] = 0;
    /* the aliases */
    static const struct {const char *from, *to;} aliases[] = {{"/system/vendor", "/vendor"}, {"/persist", "/mnt/vendor/persist"}};
    const char *prefix = "", *rest = clean;
    for (unsigned k = 0; k < 2; ++k)
        if (starts(clean, aliases[k].from)) {
            prefix = aliases[k].to;
            uint32_t skip = 0;
            while (aliases[k].from[skip]) skip++;
            rest = clean + skip;
            break;
        }
    uint32_t n = 0;
    for (const char *from = prefix; *from; ++from) {
        if (n + 1 >= capacity) return -1;
        out[n++] = *from;
    }
    for (const char *from = rest; *from; ++from) {
        if (n + 1 >= capacity) return -1;
        out[n++] = *from;
    }
    if (!capacity) return -1;
    out[n] = 0;
    return (int)n;
}

int ssp_child(const SspPack *p, uint32_t dir, uint32_t *cursor, SspEntry *child, const char **leaf)
{
    SspEntry d;
    uint32_t n = 0;
    if (dir >= p->count) return 0;
    ssp_entry(p, dir, &d);
    while (d.name[n]) n++;
    if (n == 1) n = 0;                                   /* "/": the children's prefix is just the slash */
    uint32_t i = *cursor ? *cursor : dir + 1;
    for (; i < p->count; ++i) {
        SspEntry e;
        ssp_entry(p, i, &e);
        uint32_t k = 0;
        for (; k < n && e.name[k] == d.name[k]; ++k) {}
        if (k < n || e.name[n] != '/') {
            /* Sorted: names like "dir-x" and "dir.x" come before "dir/..."; once past that prefix nothing can follow. */
            if (k < n ? (uint8_t)e.name[k] > (uint8_t)d.name[k] : (uint8_t)e.name[n] > '/') break;
            continue;
        }
        const char *rest = e.name + n + 1;
        uint32_t at = 0;
        while (rest[at] && rest[at] != '/') at++;
        if (rest[at] || !at) continue;                   /* deeper */
        *child = e;
        *leaf = rest;
        *cursor = i + 1;
        return 1;
    }
    *cursor = i;
    return 0;
}
