/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/QcomSmem.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e); 13T: access guard and
 * table-only lookups (QcomSmem.h). */
#include "QcomSmem.h"

#define TOC_MAGIC 0x434f5424u   /* "$TOC" */
#define PART_MAGIC 0x54525024u  /* "$PRT" */
#define CANARY 0xa5a5u
#define TOC_HEADER 32u
#define TOC_ENTRY 48u
#define PART_HEADER 32u
#define ENTRY 16u

static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}

int qcom_smem_allowed(const QcomSmem *smem, uint32_t offset, uint32_t size)
{
    if (!smem || !smem->base || !fits(offset, size, smem->size)) return 0;
    if (!smem->ranges) return 1;
    for (uint32_t i = 0; i < smem->ranges && i < QCOM_SMEM_MAX_RANGES; ++i)
        if (offset >= smem->range[i].offset && fits(offset - smem->range[i].offset, size, smem->range[i].size)) return 1;
    return 0;
}

int qcom_smem_allow(QcomSmem *smem, uint32_t offset, uint32_t size)
{
    if (!smem || !size || !fits(offset, size, smem->size) || smem->ranges >= QCOM_SMEM_MAX_RANGES) return QcomSmemGuard;
    for (uint32_t i = 0; i < smem->ranges; ++i)
        if (smem->range[i].offset == offset && smem->range[i].size == size) return QcomSmemOk;
    smem->range[smem->ranges].offset = offset;
    smem->range[smem->ranges].size = size;
    smem->ranges++;
    return QcomSmemOk;
}

static void refuse(const QcomSmem *smem)
{
    if (smem && smem->refused) (*smem->refused)++;
}

uint32_t qcom_smem_read32(const QcomSmem *smem, uint32_t offset)
{
    if ((offset & 3) || !qcom_smem_allowed(smem, offset, 4)) {refuse(smem); return 0;}
    return *(volatile uint32_t *)(smem->base + offset);
}
void qcom_smem_write32(const QcomSmem *smem, uint32_t offset, uint32_t value)
{
    if ((offset & 3) || !qcom_smem_allowed(smem, offset, 4)) {refuse(smem); return;}
    *(volatile uint32_t *)(smem->base + offset) = value;
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dmb oshst" ::: "memory");
#endif
}
static uint32_t rd(const QcomSmem *s, uint32_t at) {return qcom_smem_read32(s, at);}
static uint32_t align_up(uint32_t value, uint32_t to) {return (value + to - 1) & ~(to - 1);}

static int toc_offset(const QcomSmem *smem, uint32_t *toc)
{
    if (!smem || !smem->base || smem->size < 0x2000 || (smem->size & 0xfff)) return QcomSmemNoTable;
    *toc = smem->size - QCOM_SMEM_TOC_BYTES;
    if (rd(smem, *toc) != TOC_MAGIC || rd(smem, *toc + 4) != 1) return QcomSmemNoTable;
    return QcomSmemOk;
}

int qcom_smem_toc_count(const QcomSmem *smem)
{
    uint32_t toc;
    if (toc_offset(smem, &toc)) return QcomSmemNoTable;
    const uint32_t count = rd(smem, toc + 8);
    if (count > (QCOM_SMEM_TOC_BYTES - TOC_HEADER) / TOC_ENTRY) return QcomSmemNoTable;
    return (int)count;
}

int qcom_smem_toc_entry(const QcomSmem *smem, uint32_t index, QcomSmemPartition *out)
{
    uint32_t toc;
    const int count = qcom_smem_toc_count(smem);
    if (count < 0) return count;
    if (index >= (uint32_t)count || toc_offset(smem, &toc)) return QcomSmemMissing;
    const uint32_t e = toc + TOC_HEADER + index * TOC_ENTRY;
    const uint32_t offset = rd(smem, e), size = rd(smem, e + 4), hosts = rd(smem, e + 12), cacheline = rd(smem, e + 16);
    *out = (QcomSmemPartition){offset, size, cacheline, (uint16_t)hosts, (uint16_t)(hosts >> 16)};
    return offset && size ? QcomSmemOk : QcomSmemMissing;
}

int qcom_smem_toc_find(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out)
{
    uint32_t toc;
    const int count = qcom_smem_toc_count(smem);
    if (count < 0 || toc_offset(smem, &toc)) return QcomSmemNoTable;
    int found = 0;
    for (uint32_t i = 0; i < (uint32_t)count; ++i) {
        QcomSmemPartition p;
        if (qcom_smem_toc_entry(smem, i, &p)) continue;
        if (!((p.host0 == host && p.host1 == remote) || (p.host0 == remote && p.host1 == host))) continue;
        if (found || !fits(p.offset, p.size, toc) || p.size < PART_HEADER + 2 * ENTRY || (p.offset & 7)) return QcomSmemBadPartition;
        if (p.cacheline & (p.cacheline - 1)) return QcomSmemBadPartition;
        if (!p.cacheline) p.cacheline = 8u;
        *out = p;
        found = 1;
    }
    return found ? QcomSmemOk : QcomSmemNoPartition;
}

int qcom_smem_partition(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out)
{
    QcomSmemPartition p;
    const int rc = qcom_smem_toc_find(smem, host, remote, &p);
    if (rc) return rc;
    const uint32_t hosts = p.host0 | (uint32_t)p.host1 << 16;
    if (rd(smem, p.offset) != PART_MAGIC || rd(smem, p.offset + 4) != hosts || rd(smem, p.offset + 8) != p.size)
        return QcomSmemBadPartition;
    const uint32_t free_uncached = rd(smem, p.offset + 12), free_cached = rd(smem, p.offset + 16);
    if (free_uncached < PART_HEADER || free_uncached > free_cached || free_cached > p.size) return QcomSmemBadPartition;
    *out = p;
    return QcomSmemOk;
}

/* Walks the uncached list; returns the offset (relative to the partition) where it ends, or the item's entry when found. */
static int walk_uncached(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t *at_out, int *found)
{
    const uint32_t end = rd(smem, p->offset + 12);
    uint32_t at = PART_HEADER;
    *found = 0;
    if (end > p->size) return QcomSmemCorrupt;
    while (at < end) {
        if (!fits(at, ENTRY, end)) return QcomSmemCorrupt;
        const uint32_t w0 = rd(smem, p->offset + at), size = rd(smem, p->offset + at + 4), pads = rd(smem, p->offset + at + 8);
        if ((w0 & 0xffff) != CANARY) return QcomSmemCorrupt;
        const uint32_t padding_header = pads >> 16;
        if ((uint64_t)at + ENTRY + padding_header + size > end) return QcomSmemCorrupt;
        if ((w0 >> 16) == item) {*found = 1; *at_out = at; return QcomSmemOk;}
        at += ENTRY + padding_header + size;
    }
    if (at != end) return QcomSmemCorrupt;
    *at_out = at;
    return QcomSmemOk;
}

int qcom_smem_find(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t *offset, uint32_t *size)
{
    uint32_t at;
    int found, rc = walk_uncached(smem, p, item, &at, &found);
    if (rc) return rc;
    if (found) {
        const uint32_t bytes = rd(smem, p->offset + at + 4), pads = rd(smem, p->offset + at + 8);
        if ((pads & 0xffff) > bytes) return QcomSmemCorrupt;
        *offset = p->offset + at + ENTRY + (pads >> 16);
        *size = bytes - (pads & 0xffff);
        return QcomSmemOk;
    }
    /* Cached items: each entry sits at the end of its cache line slot, its data right below it. */
    const uint32_t header = align_up(ENTRY, p->cacheline), last = rd(smem, p->offset + 16);
    if (p->size < header || last > p->size) return QcomSmemCorrupt;
    uint32_t e = p->size - header;
    while (e > last) {
        const uint32_t w0 = rd(smem, p->offset + e), bytes = rd(smem, p->offset + e + 4), pads = rd(smem, p->offset + e + 8);
        if ((w0 & 0xffff) != CANARY || bytes > e || (pads & 0xffff) > bytes) return QcomSmemCorrupt;
        if ((w0 >> 16) == item) {*offset = p->offset + e - bytes; *size = bytes - (pads & 0xffff); return QcomSmemOk;}
        if (e < bytes + header) return QcomSmemCorrupt;
        e -= bytes + header;
    }
    return QcomSmemMissing;
}

int qcom_smem_alloc(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t size)
{
    uint32_t at;
    int found, rc = walk_uncached(smem, p, item, &at, &found);
    if (rc) return rc;
    if (found) return QcomSmemExists;
    uint32_t offset, bytes;
    rc = qcom_smem_find(smem, p, item, &offset, &bytes);
    if (rc == QcomSmemOk) return QcomSmemExists;
    if (rc != QcomSmemMissing) return rc;
    const uint32_t aligned = align_up(size, 8), need = ENTRY + aligned;
    if (!size || aligned < size || (uint64_t)at + need > rd(smem, p->offset + 16)) return QcomSmemNoSpace;
    qcom_smem_write32(smem, p->offset + at, CANARY | (uint32_t)item << 16);
    qcom_smem_write32(smem, p->offset + at + 4, aligned);
    qcom_smem_write32(smem, p->offset + at + 8, aligned - size);  /* padding_data, padding_header 0 */
    qcom_smem_write32(smem, p->offset + at + 12, 0);
    for (uint32_t i = 0; i < aligned; i += 4) qcom_smem_write32(smem, p->offset + at + ENTRY + i, 0);
    /* The entry is complete before the free mark moves over it. */
    qcom_smem_write32(smem, p->offset + 12, at + need);
    return QcomSmemOk;
}
