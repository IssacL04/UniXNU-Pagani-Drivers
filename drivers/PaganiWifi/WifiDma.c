/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Follows UniXNU-sheng package/WifiProbe.cpp arenaAlloc/arenaFree (commit 08cde20e); see WifiDma.h. */
#include "WifiDma.h"
#include "WifiUtil.h"

int pw_dma_init(PwDma *d, uint8_t *va, uint64_t bus, uint32_t bytes)
{
    if (!d) return -1;
    pw_zero(d, sizeof *d);
    if (!va || bytes <= PW_DMA_RESERVED || (bus & 4095)) return -1;
    d->va = va;
    d->bus = bus;
    d->bytes = bytes;
    d->blocks[0] = (PwDmaBlock){PW_DMA_RESERVED, bytes - PW_DMA_RESERVED, 0};
    d->count = 1;
    pw_zero(va, bytes);
    return 0;
}

static uint32_t round_size(size_t n)
{
    if (!n) n = 1;
    n = (n + 63) & ~(size_t)63;
    if (n >= 4096) n = (n + 4095) & ~(size_t)4095;
    return (uint32_t)n;
}
static uint64_t alignment_of(uint32_t n)
{
    uint64_t align = 64;
    if (n >= 4096) {
        align = 4096;
        while (align < n) align <<= 1;
    } else {
        while (align < n) align <<= 1;
    }
    return align;
}
static void move_blocks(PwDma *d, unsigned to, unsigned from, unsigned count)
{
    if (to == from || !count) return;
    if (to > from) for (unsigned i = count; i-- > 0;) d->blocks[to + i] = d->blocks[from + i];
    else for (unsigned i = 0; i < count; ++i) d->blocks[to + i] = d->blocks[from + i];
}

void *pw_dma_alloc(PwDma *d, size_t want, uint64_t *bus)
{
    if (!d || !d->va || want > 0x40000000u) return 0;
    const uint32_t n = round_size(want);
    const uint64_t align = alignment_of(n);
    for (unsigned i = 0; i < d->count; ++i) {
        PwDmaBlock *b = &d->blocks[i];
        if (b->used) continue;
        const uint64_t at = d->bus + b->off;
        const uint64_t start = (at + align - 1) & ~(align - 1);
        const uint64_t pad = start - at;
        if ((uint64_t)b->size < pad + n) continue;
        const int tail = (uint64_t)b->size > pad + n;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        if (d->count + extra > PW_DMA_BLOCKS) continue;
        const PwDmaBlock whole = *b;
        move_blocks(d, i + extra, i, d->count - i);
        d->count += extra;
        unsigned k = i;
        if (pad) d->blocks[k++] = (PwDmaBlock){whole.off, (uint32_t)pad, 0};
        const uint32_t off = whole.off + (uint32_t)pad;
        d->blocks[k] = (PwDmaBlock){off, n, 1};
        if (tail) d->blocks[k + 1] = (PwDmaBlock){off + n, whole.size - (uint32_t)pad - n, 0};
        uint8_t *p = d->va + off;
        pw_zero(p, n);
        if (bus) *bus = d->bus + off;
        d->allocs++;
        d->in_use += n;
        if (d->in_use > d->peak) d->peak = d->in_use;
        return p;
    }
    d->failures++;
    d->last_failed_size = n;
    return 0;
}

void pw_dma_free(PwDma *d, void *p)
{
    if (!d || !p || (uint8_t *)p < d->va || (uint8_t *)p >= d->va + d->bytes) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - d->va);
    for (unsigned i = 0; i < d->count; ++i) {
        if (d->blocks[i].off != off || !d->blocks[i].used) continue;
        d->blocks[i].used = 0;
        d->in_use -= d->blocks[i].size;
        d->frees++;
        if (i + 1 < d->count && !d->blocks[i + 1].used) {
            d->blocks[i].size += d->blocks[i + 1].size;
            move_blocks(d, i + 1, i + 2, d->count - i - 2);
            --d->count;
        }
        if (i > 0 && !d->blocks[i - 1].used) {
            d->blocks[i - 1].size += d->blocks[i].size;
            move_blocks(d, i, i + 1, d->count - i - 1);
            --d->count;
        }
        return;
    }
}

uint64_t pw_dma_bus(const PwDma *d, const void *p)
{
    if (!d || !d->va || (const uint8_t *)p < d->va || (const uint8_t *)p >= d->va + d->bytes) return 0;
    return d->bus + (uint64_t)((const uint8_t *)p - d->va);
}

uint8_t *pw_dma_va(const PwDma *d, uint64_t bus, uint64_t n)
{
    if (!d || !d->va || bus < d->bus || n > d->bytes || bus - d->bus > d->bytes - n) return 0;
    return d->va + (bus - d->bus);
}

void pw_dma_free_bytes(const PwDma *d, uint32_t *total, uint32_t *largest)
{
    uint32_t t = 0, l = 0;
    for (unsigned i = 0; d && i < d->count; ++i) {
        if (d->blocks[i].used) continue;
        t += d->blocks[i].size;
        if (d->blocks[i].size > l) l = d->blocks[i].size;
    }
    if (total) *total = t;
    if (largest) *largest = l;
}
