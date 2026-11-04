/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/QcomSmem.h (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). 13T changes: the
 * access guard below, and the partition table lookup split from the partition header check. */
/* Qualcomm shared memory (SMEM): the partition table at the end of the region, the private partitions two processors share, and the
 * items in them (uncached ones growing up from the partition header, cached ones down from its end). Every access is an aligned 32-bit
 * one, so the code works on a device mapping. Allocation must run with the hardware mutex held (the caller takes it); nothing here
 * waits.
 *
 * The access guard: on the OnePlus 13T only some SMEM pages can be read from XNU; the page at 0x9e000 (the first one behind the global
 * partition) takes a synchronous external abort (Adreno830 docs/ARCHITECTURE.md 4.1.2, tools patch_smem_copy.py). Linux only ever
 * touches the partition table, the global partition and the partitions the application processor is a host of. With ranges set, every
 * access outside them is refused without being issued: a read gives 0 (every structure check then fails) and is counted. */
#ifndef QCOM_SMEM_H
#define QCOM_SMEM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_SMEM_HOST_APPS 0u
#define QCOM_SMEM_HOST_GLOBAL 0xfffeu
#define QCOM_SMEM_TOC_BYTES 0x1000u      /* the partition table: the last 4 KiB of the region */
#define QCOM_SMEM_MAX_RANGES 4u

typedef struct {
    uint32_t offset, size;
} QcomSmemRange;

typedef struct {
    volatile uint8_t *base;
    uint32_t size;
    uint32_t ranges;                       /* 0: no guard, any aligned word inside size */
    QcomSmemRange range[QCOM_SMEM_MAX_RANGES];
    uint32_t *refused;                     /* optional: accesses the guard refused */
} QcomSmem;

typedef struct {
    uint32_t offset, size, cacheline;  /* offset of the partition header from the SMEM base */
    uint16_t host0, host1;
} QcomSmemPartition;

enum {
    QcomSmemOk = 0,
    QcomSmemNoTable = -1,
    QcomSmemNoPartition = -2,
    QcomSmemBadPartition = -3,
    QcomSmemCorrupt = -4,
    QcomSmemExists = -5,
    QcomSmemNoSpace = -6,
    QcomSmemMissing = -7,
    QcomSmemGuard = -8,                   /* the range cannot be added (full, or outside the region) */
};

/* Adds [offset, offset + size) to the readable ranges (the guard is on once one is added). */
int qcom_smem_allow(QcomSmem *smem, uint32_t offset, uint32_t size);
/* 1 when [offset, offset + size) lies inside the region and, with the guard on, inside one range. */
int qcom_smem_allowed(const QcomSmem *smem, uint32_t offset, uint32_t size);

/* The partition table: its entry count (QcomSmemNoTable when the table is not there), and entry index as a partition description
 * (offset 0 or size 0: an unused slot, QcomSmemMissing). Only the table page is read. */
int qcom_smem_toc_count(const QcomSmem *smem);
int qcom_smem_toc_entry(const QcomSmem *smem, uint32_t index, QcomSmemPartition *out);
/* The table entry of the partition the two hosts share (QCOM_SMEM_HOST_GLOBAL twice for the global one), from the table only: the
 * partition is checked to lie inside the region, below the table, 8-byte aligned. More than one entry: QcomSmemBadPartition. */
int qcom_smem_toc_find(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out);
/* qcom_smem_toc_find, then the partition's own header ("$PRT", hosts, size, free offsets) checked against it. */
int qcom_smem_partition(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out);
/* Offset of the item's data from the SMEM base and its size; QcomSmemMissing when the item is not there. */
int qcom_smem_find(const QcomSmem *smem, const QcomSmemPartition *part, uint16_t item, uint32_t *offset, uint32_t *size);
/* Allocates an uncached item of size bytes (rounded up to 8). QcomSmemExists if it is there already (not an error for callers that
 * only need it to exist). Hardware mutex held by the caller. */
int qcom_smem_alloc(const QcomSmem *smem, const QcomSmemPartition *part, uint16_t item, uint32_t size);

uint32_t qcom_smem_read32(const QcomSmem *smem, uint32_t offset);
void qcom_smem_write32(const QcomSmem *smem, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
