/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The allocator follows UniXNU-sheng package/WifiProbe.cpp arenaInit/arenaAlloc/arenaFree (https://github.com/UniXNU/UniXNU-sheng,
 * commit 08cde20e): first fit over a list of blocks kept in address order, neighbours merged on free, the first page kept for the
 * MSI target. Here it is portable C over a window the owner hands in, and the alignment is taken on the bus address (sheng's window
 * started on a large boundary; the 13T's is wherever the kernel found contiguous memory). */
/* The endpoint's DMA window. On the 13T the endpoint's stream (SID 0x1401) is a pass-through in the apps SMMU (Sm8750Platform
 * SmmuTakeover.h, the planner NativeUfs uses for the UFS stream), so a bus address is the physical address, and the window is one
 * physically contiguous range mapped write-combined: low memory outside XNU's window by default (PaganiWifiTables.h kDmaLowBase says
 * why), a kernel allocation above 4 GiB with pagani-wifi-dmapa=0.
 * Every buffer the endpoint reads or writes sits at an address aligned to its size rounded up to a power of two (at least 64 bytes;
 * from 4 KiB on, page sized and page aligned): sheng saw the boot loader refuse an image address that was only page aligned. */
#ifndef PAGANI_WIFI_DMA_H
#define PAGANI_WIFI_DMA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PW_DMA_BLOCKS 8192u
#define PW_DMA_RESERVED 4096u           /* the first page: the MSI target (the iMSI receiver takes those writes, memory never sees them) */

typedef struct {uint32_t off, size; uint32_t used;} PwDmaBlock;

typedef struct {
    uint8_t *va;
    uint64_t bus;
    uint32_t bytes;
    PwDmaBlock blocks[PW_DMA_BLOCKS];
    unsigned count;
    /* statistics for the log */
    uint32_t in_use, peak, allocs, frees, failures;
    uint32_t last_failed_size;
} PwDma;

/* The window: va mapped cached, bus the address the endpoint uses, bytes > PW_DMA_RESERVED. Zeroes it. 0, or -1. */
int pw_dma_init(PwDma *d, uint8_t *va, uint64_t bus, uint32_t bytes);
/* Zeroed memory of n bytes (rounded up as above) and its bus address; null when nothing fits. */
void *pw_dma_alloc(PwDma *d, size_t n, uint64_t *bus);
/* Returns a block given out by pw_dma_alloc; anything else is ignored. */
void pw_dma_free(PwDma *d, void *p);
/* The bus address of a CPU address inside the window (0 outside it), and back (null outside). */
uint64_t pw_dma_bus(const PwDma *d, const void *p);
uint8_t *pw_dma_va(const PwDma *d, uint64_t bus, uint64_t n);
/* Free bytes in all, and the largest free block. */
void pw_dma_free_bytes(const PwDma *d, uint32_t *total, uint32_t *largest);

#ifdef __cplusplus
}
#endif
#endif
