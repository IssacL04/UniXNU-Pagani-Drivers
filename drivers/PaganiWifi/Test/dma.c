// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of WifiDma.c (the endpoint's DMA window): the reserved MSI page, the size rounding and the power-of-two alignment taken on
// the bus address (the window's bus base is only page aligned here on purpose), zeroed blocks, merging on free, exhaustion and the
// CPU/bus translations; a random allocate/free run checked against a shadow map of the window.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiDma.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static PwDma d;

int main(void)
{
    const uint32_t bytes = 16u << 20;
    uint8_t *va = malloc(bytes);
    CHECK(va);
    memset(va, 0xcc, bytes);
    const uint64_t bus = 0x8c0003000ull;   /* page aligned, not more */
    CHECK(pw_dma_init(&d, NULL, bus, bytes) == -1 && pw_dma_init(&d, va, bus + 1, bytes) == -1 && pw_dma_init(&d, va, bus, 4096) == -1);
    CHECK(pw_dma_init(&d, va, bus, bytes) == 0 && va[0] == 0 && va[bytes - 1] == 0);
    uint32_t total, largest;
    pw_dma_free_bytes(&d, &total, &largest);
    CHECK(total == bytes - PW_DMA_RESERVED && largest == total);

    /* small blocks: 64-byte granules, aligned to their rounded size */
    uint64_t b1, b2, b3;
    uint8_t *p1 = pw_dma_alloc(&d, 10, &b1), *p2 = pw_dma_alloc(&d, 100, &b2), *p3 = pw_dma_alloc(&d, 4096, &b3);
    CHECK(p1 && p2 && p3);
    CHECK(b1 >= bus + PW_DMA_RESERVED && b1 % 64 == 0 && b2 % 128 == 0 && b3 % 4096 == 0);
    CHECK(pw_dma_bus(&d, p1) == b1 && pw_dma_va(&d, b2, 100) == p2 && pw_dma_bus(&d, va - 1) == 0 && !pw_dma_va(&d, bus + bytes, 1));
    /* a 512 KiB buffer sits at a 512 KiB boundary of the bus address, a 3 MiB one at 4 MiB */
    uint64_t b4, b5;
    uint8_t *p4 = pw_dma_alloc(&d, 0x80000, &b4), *p5 = pw_dma_alloc(&d, 3u << 20, &b5);
    CHECK(p4 && p5 && b4 % 0x80000 == 0 && b5 % (4u << 20) == 0);
    memset(p4, 0x5a, 0x80000);
    /* freed and taken again: zeroed */
    pw_dma_free(&d, p4);
    uint64_t b6;
    uint8_t *p6 = pw_dma_alloc(&d, 0x80000, &b6);
    CHECK(p6 == p4 && b6 == b4 && p6[0] == 0 && p6[0x7ffff] == 0);
    /* everything back: one free block again */
    pw_dma_free(&d, p1); pw_dma_free(&d, p2); pw_dma_free(&d, p3); pw_dma_free(&d, p5); pw_dma_free(&d, p6);
    pw_dma_free(&d, p6);                 /* twice: ignored */
    pw_dma_free(&d, va + 12345);         /* not a block start: ignored */
    pw_dma_free_bytes(&d, &total, &largest);
    CHECK(d.count == 1 && total == bytes - PW_DMA_RESERVED && d.in_use == 0);
    /* exhaustion: a request larger than the window, then filling it */
    CHECK(!pw_dma_alloc(&d, bytes, NULL) && d.failures == 1);
    unsigned n = 0;
    while (pw_dma_alloc(&d, 0x100000, NULL)) ++n;
    CHECK(n == 15 && d.failures == 2);   /* 16 MiB less the reserved page: 15 aligned MiB fit */

    /* random run against a shadow map: blocks never overlap, never touch the reserved page, stay inside, keep their alignment */
    CHECK(pw_dma_init(&d, va, bus, bytes) == 0);
    static uint8_t owner[16u << 20 >> 6];   /* 64-byte granules: 0 free, else block tag */
    struct {uint8_t *p; uint64_t bus; uint32_t n; uint8_t tag;} live[512];
    unsigned count = 0;
    srand(8750);
    for (unsigned round = 0; round < 20000; ++round) {
        if (count < 512 && (rand() % 3)) {
            const uint32_t want = (rand() % 4) ? (uint32_t)(rand() % 5000 + 1) : (uint32_t)(rand() % (600 * 1024) + 1);
            uint64_t b = 0;
            uint8_t *p = pw_dma_alloc(&d, want, &b);
            if (!p) continue;
            uint32_t n2 = (want + 63) & ~63u;
            if (n2 >= 4096) n2 = (n2 + 4095) & ~4095u;
            uint64_t align = n2 >= 4096 ? 4096 : 64;
            while (align < n2) align <<= 1;
            CHECK(b % align == 0 && b >= bus + PW_DMA_RESERVED && b + n2 <= bus + bytes && p == va + (b - bus));
            const uint8_t tag = (uint8_t)(round % 250 + 1);
            for (uint64_t g = (b - bus) >> 6; g < (b - bus + n2) >> 6; ++g) {CHECK(!owner[g]); owner[g] = tag;}
            CHECK(p[0] == 0 && p[n2 - 1] == 0);
            memset(p, tag, n2);
            live[count].p = p; live[count].bus = b; live[count].n = n2; live[count].tag = tag; ++count;
        } else if (count) {
            const unsigned i = (unsigned)rand() % count;
            for (uint32_t k = 0; k < live[i].n; k += 997) CHECK(live[i].p[k] == live[i].tag);
            for (uint64_t g = (live[i].bus - bus) >> 6; g < (live[i].bus - bus + live[i].n) >> 6; ++g) owner[g] = 0;
            pw_dma_free(&d, live[i].p);
            live[i] = live[--count];
        }
    }
    while (count) pw_dma_free(&d, live[--count].p);
    CHECK(d.count == 1 && d.in_use == 0);
    free(va);
    printf("PASS DMA window: %u checks\n", checks);
    return 0;
}
