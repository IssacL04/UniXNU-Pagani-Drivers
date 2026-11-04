/* SPDX-License-Identifier: MIT */
#include "Sm8750SocInfo.h"
#include <stddef.h>

/* Layout facts: Linux drivers/soc/qcom/smem.c (struct smem_ptable, smem_ptable_entry,
 * smem_partition_header, smem_private_entry) and include/linux/soc/qcom/socinfo.h. */
enum {
    TocMagic = 0x434f5424,                  /* "$TOC" */
    TocVersion = 1,
    TocHeader = 0x20,                       /* magic, version, num_entries, reserved[5] */
    TocEntry = 48,                          /* offset, size, flags, host0|host1, cacheline, reserved[7] */
    TocMaxEntries = (0x1000 - TocHeader) / TocEntry,
    PartMagic = 0x54525024,                 /* "$PRT" */
    PartHeader = 0x20,                      /* magic, host0|host1, size, free_uncached, free_cached, reserved[3] */
    HostGlobal = 0xfffe,
    ItemHeader = 16,                        /* canary|item, size, padding_data|padding_hdr, reserved */
    ItemCanary = 0xa5a5,
    MaxCacheline = 0x1000,
    SocInfoId = 0x04, SocInfoVersion = 0x08, SocInfoRawId = 0x2c, SocInfoRawVersion = 0x30,
    SocInfoFeatureCode = 0xb0, SocInfoPcode = 0xb4,
    FeatureInternalFirst = 0xf1,            /* SOCINFO_FC_Y0 */
    FeatureInternalLast = 0x100,            /* SOCINFO_FC_YF (= SOCINFO_FC_INT_MAX) */
    PcodeReserved = 0x7fffffff,             /* SOCINFO_PC_RESERVE */
};

/* The only pages of the PKX110 SMEM window that can be read (others abort). */
static const struct {
    uint32_t start, end;
} readable[] = {
    {Sm8750SmemGlobalStart, Sm8750SmemGlobalEnd},
    {Sm8750SmemTocOffset, Sm8750SmemBytes},
};

typedef struct {
    const Sm8750SocInfoIo *io;
    Sm8750SocInfo *info;
    uint32_t lo, hi;                        /* the structure being parsed; reads stay inside [lo, hi) */
} Ctx;

static int inside_readable(uint64_t start, uint64_t end)
{
    for (size_t i = 0; i < sizeof(readable) / sizeof(readable[0]); i++)
        if (start >= readable[i].start && end <= readable[i].end)
            return 1;
    return 0;
}

/* The single SMEM accessor: aligned, inside the current structure and inside a readable page. */
static Sm8750SocInfoStatus rd(Ctx *c, uint32_t offset, uint32_t *value)
{
    c->info->last_offset = offset;
    if ((offset & 3) || offset < c->lo || (uint64_t)offset + 4 > c->hi || !inside_readable(offset, (uint64_t)offset + 4))
        return Sm8750SocInfoOutOfBounds;
    c->info->reads++;
    return c->io->read32(c->io->cookie, offset, value) ? Sm8750SocInfoIoError : Sm8750SocInfoOk;
}

static void bound(Ctx *c, uint32_t lo, uint32_t hi)
{
    c->lo = lo;
    c->hi = hi;
}

/* The one global entry of the partition table. */
static Sm8750SocInfoStatus global_entry(Ctx *c)
{
    Sm8750SocInfo *info = c->info;
    uint32_t magic, version, count, found = 0;
    Sm8750SocInfoStatus s;
    bound(c, Sm8750SmemTocOffset, Sm8750SmemBytes);
    if ((s = rd(c, Sm8750SmemTocOffset, &magic)) || (s = rd(c, Sm8750SmemTocOffset + 4, &version)) ||
        (s = rd(c, Sm8750SmemTocOffset + 8, &count)))
        return s;
    if (magic != TocMagic || version != TocVersion || !count || count > TocMaxEntries)
        return Sm8750SocInfoNoTable;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = (uint32_t)Sm8750SmemTocOffset + TocHeader + i * TocEntry, hosts, offset, size, cacheline;
        if ((s = rd(c, e + 12, &hosts)))
            return s;
        if ((hosts & 0xffff) != HostGlobal || (hosts >> 16) != HostGlobal)
            continue;
        if ((s = rd(c, e, &offset)) || (s = rd(c, e + 4, &size)) || (s = rd(c, e + 16, &cacheline)))
            return s;
        if (!offset || !size)               /* unused slot, as Linux skips it */
            continue;
        if (found++)
            return Sm8750SocInfoBadPartition;
        info->partition_offset = offset;
        info->partition_bytes = size;
        info->cacheline = cacheline;
    }
    if (!found)
        return Sm8750SocInfoNoPartition;
    if ((info->partition_offset & 7) || info->partition_bytes < PartHeader ||
        (uint64_t)info->partition_offset + info->partition_bytes > Sm8750SmemTocOffset ||
        !info->cacheline || (info->cacheline & (info->cacheline - 1)) || info->cacheline > MaxCacheline)
        return Sm8750SocInfoBadPartition;
    return Sm8750SocInfoOk;
}

static Sm8750SocInfoStatus note_item(Ctx *c, uint32_t data, uint32_t bytes, uint32_t flags)
{
    Sm8750SocInfo *info = c->info;
    if (data & 3)
        return Sm8750SocInfoCorrupt;
    info->item_offset = info->partition_offset + data;
    info->item_bytes = bytes;
    info->flags |= flags;
    return Sm8750SocInfoOk;
}

/* Linux qcom_smem_get_private(): the uncached list first, the cached list only if the item is not there,
 * each walk stopping at the first item 137. Reading no more than that keeps away from the end of the
 * partition unless it is needed; every read still goes through rd(). Offsets are relative to the
 * partition; every entry and item must lie between its list's ends. */
static Sm8750SocInfoStatus find_item(Ctx *c)
{
    Sm8750SocInfo *info = c->info;
    const uint32_t base = info->partition_offset, size = info->partition_bytes;
    const uint32_t stride = info->cacheline < ItemHeader ? ItemHeader : info->cacheline;   /* ALIGN(16, cacheline) */
    uint32_t magic, hosts, part_size, free_uncached, free_cached, w0, w1, w2;
    Sm8750SocInfoStatus s;

    bound(c, base, base + size);
    if ((s = rd(c, base, &magic)) || (s = rd(c, base + 4, &hosts)) || (s = rd(c, base + 8, &part_size)) ||
        (s = rd(c, base + 12, &free_uncached)) || (s = rd(c, base + 16, &free_cached)))
        return s;
    if (magic != PartMagic || hosts != (HostGlobal | (uint32_t)HostGlobal << 16) || part_size != size ||
        free_uncached < PartHeader || free_uncached > free_cached || free_cached > size || size < stride)
        return Sm8750SocInfoBadPartition;

    /* Uncached items grow up from the header: header, padding_hdr, data. */
    for (uint32_t at = PartHeader; at < free_uncached;) {
        if ((at & 3) || free_uncached - at < ItemHeader)
            return Sm8750SocInfoCorrupt;
        if ((s = rd(c, base + at, &w0)) || (s = rd(c, base + at + 4, &w1)) || (s = rd(c, base + at + 8, &w2)))
            return s;
        uint64_t data = (uint64_t)at + ItemHeader + (w2 >> 16), end = data + w1;
        if ((w0 & 0xffff) != ItemCanary || (w2 & 0xffff) > w1 || end > free_uncached)
            return Sm8750SocInfoCorrupt;
        if ((w0 >> 16) == Sm8750SmemSocInfoItem)
            return note_item(c, (uint32_t)data, w1 - (w2 & 0xffff), Sm8750SocInfoFlagUncached);
        at = (uint32_t)end;
    }

    /* Cached items grow down from the end: each header sits in its own cache line, data below it. */
    for (uint32_t e = size - stride; e > free_cached;) {
        if (e & 3)
            return Sm8750SocInfoCorrupt;
        if ((s = rd(c, base + e, &w0)) || (s = rd(c, base + e + 4, &w1)) || (s = rd(c, base + e + 8, &w2)))
            return s;
        if ((w0 & 0xffff) != ItemCanary || (w2 & 0xffff) > w1 || w1 > e - free_cached)
            return Sm8750SocInfoCorrupt;
        const uint32_t data = e - w1;
        if ((w0 >> 16) == Sm8750SmemSocInfoItem)
            return note_item(c, data, w1 - (w2 & 0xffff), 0);
        if (data <= free_cached + stride)
            break;
        e = data - stride;
    }
    return Sm8750SocInfoMissing;
}

uint32_t Sm8750SocCode(uint32_t fmt, uint32_t feature_code, uint32_t pcode)
{
    uint32_t fc = fmt >= Sm8750SocInfoFmtFeature ? feature_code : 0;
    if (fc > FeatureInternalLast)
        fc = 0;                             /* not a feature code: SOCINFO_FC_UNKNOWN */
    /* The pcode only distinguishes internal SKUs (Y0..YF); retail parts report SOCINFO_PCODE_UNKNOWN. */
    uint32_t pc = fc >= FeatureInternalFirst && pcode < PcodeReserved ? pcode : 0;
    return (pc & 0xffff) << 16 | fc;
}

Sm8750SocInfoStatus Sm8750SocInfoRead(const Sm8750SocInfoIo *smem, Sm8750SocInfo *info)
{
    Sm8750SocInfoStatus s;
    if (!smem || !smem->read32 || !info)
        return Sm8750SocInfoArgument;
    *info = (Sm8750SocInfo){0};
    Ctx ctx = {smem, info, 0, 0}, *c = &ctx;

    if ((s = global_entry(c)) || (s = find_item(c)))
        return s;

    const uint32_t item = info->item_offset, bytes = info->item_bytes;
    if (bytes < SocInfoVersion + 4)
        return Sm8750SocInfoShort;
    bound(c, item, item + bytes);
    if ((s = rd(c, item, &info->fmt)) || (s = rd(c, item + SocInfoId, &info->id)) ||
        (s = rd(c, item + SocInfoVersion, &info->revision)))
        return s;
    if (info->fmt >= Sm8750SocInfoFmtRaw && bytes >= SocInfoRawVersion + 4 &&
        ((s = rd(c, item + SocInfoRawId, &info->raw_id)) || (s = rd(c, item + SocInfoRawVersion, &info->raw_revision))))
        return s;
    if (info->fmt >= Sm8750SocInfoFmtFeature && bytes >= SocInfoPcode + 4) {
        if ((s = rd(c, item + SocInfoFeatureCode, &info->feature_code)) || (s = rd(c, item + SocInfoPcode, &info->pcode)))
            return s;
    } else {
        info->flags |= Sm8750SocInfoFlagNoFeature;
    }
    info->soc_code = Sm8750SocCode(info->flags & Sm8750SocInfoFlagNoFeature ? 0 : info->fmt, info->feature_code,
                                   info->pcode);
    return Sm8750SocInfoOk;
}

Sm8750SocInfoStatus Sm8750SpeedBinRead(const Sm8750SocInfoIo *qfprom, uint32_t offset, unsigned shift, unsigned bits,
                                       uint32_t *bin)
{
    uint32_t value;
    if (!qfprom || !qfprom->read32 || !bin || !bits || bits > 32 || shift >= 32)
        return Sm8750SocInfoArgument;
    const uint32_t word = offset & ~3u, at = (offset & 3) * 8 + shift;
    if (at + bits > 32)
        return Sm8750SocInfoArgument;       /* the cell would span two words */
    if (offset >= Sm8750QfpromBytes)
        return Sm8750SocInfoOutOfBounds;
    if (qfprom->read32(qfprom->cookie, word, &value))
        return Sm8750SocInfoIoError;
    value >>= at;
    *bin = bits == 32 ? value : value & ((1u << bits) - 1);
    return Sm8750SocInfoOk;
}

/* ---- GPU level tables ---- */

static int bin_valid(const Sm8750GpuBin *b)
{
    if (!b->levels || !b->count || b->count > Sm8750GpuMaxLevels || b->initial >= b->count ||
        b->sku_count > Sm8750GpuMaxSkus || ((b->flags & Sm8750GpuBinHasSku) && !b->sku_count))
        return 0;
    for (unsigned i = 0; i < b->count; i++)
        if (!b->levels[i].khz || (i && b->levels[i].khz < b->levels[i - 1].khz))
            return 0;
    return 1;
}

static const Sm8750GpuLevel *initial_level(const Sm8750GpuBin *b)
{
    return &b->levels[b->count - 1 - b->initial];
}

/* Appends unless it repeats the previous level's frequency and voltage level (or the table is full, which a
 * valid bin cannot cause: every table built here is at most as long as one bin). */
static void push(Sm8750GpuLevelTable *t, const Sm8750GpuLevel *level)
{
    if (t->count >= Sm8750GpuMaxLevels ||
        (t->count && t->levels[t->count - 1].khz == level->khz && t->levels[t->count - 1].vlvl == level->vlvl))
        return;
    t->levels[t->count] = *level;
    t->levels[t->count].reserved = 0;
    t->count++;
}

/* Index of the fastest level not faster than khz (0 if all are). */
static uint8_t at_or_below(const Sm8750GpuLevelTable *t, uint32_t khz)
{
    uint8_t index = 0;
    for (uint8_t i = 0; i < t->count; i++)
        if (t->levels[i].khz <= khz)
            index = i;
    return index;
}

int Sm8750GpuBinMatch(const Sm8750GpuBin *bins, unsigned count, uint32_t speed_bin, uint32_t soc_code)
{
    if (!bins)
        return -1;
    for (unsigned i = 0; i < count && i < 128; i++) {
        const Sm8750GpuBin *b = &bins[i];
        uint32_t bin = 0;
        int match = 0;
        if (b->flags & Sm8750GpuBinHasSpeed) {
            bin = b->speed_bin;
            match = bin == speed_bin;
        }
        if (b->flags & Sm8750GpuBinHasSku) {
            if (bin && !match)
                continue;
            match = 0;
            for (unsigned k = 0; k < b->sku_count && k < Sm8750GpuMaxSkus; k++)
                if (b->sku[k] == 0 || b->sku[k] == soc_code) {
                    match = 1;
                    break;
                }
        }
        if (match)
            return (int)i;
    }
    return -1;
}

Sm8750SocInfoStatus Sm8750GpuLevelsCommon(const Sm8750GpuBin *bins, unsigned count, Sm8750GpuLevelTable *out)
{
    if (!bins || !out || !count || count > 128)
        return Sm8750SocInfoArgument;
    unsigned depth = Sm8750GpuMaxLevels;
    uint32_t start_khz = UINT32_MAX;
    for (unsigned b = 0; b < count; b++) {
        if (!bin_valid(&bins[b]))
            return Sm8750SocInfoArgument;
        if (bins[b].count < depth)
            depth = bins[b].count;
        if (initial_level(&bins[b])->khz < start_khz)
            start_khz = initial_level(&bins[b])->khz;
    }
    *out = (Sm8750GpuLevelTable){0};
    out->bin = -1;
    for (unsigned i = 0; i < depth; i++) {
        Sm8750GpuLevel level = bins[0].levels[i];
        for (unsigned b = 1; b < count; b++) {
            const Sm8750GpuLevel *other = &bins[b].levels[i];
            if (other->khz != level.khz || other->vlvl != level.vlvl || other->acd != level.acd)
                goto done;
            if (other->bus_min < level.bus_min)
                level.bus_min = other->bus_min;
            if (other->bus < level.bus)
                level.bus = other->bus;
            if (other->bus_max < level.bus_max)
                level.bus_max = other->bus_max;
        }
        push(out, &level);
    }
done:
    if (!out->count)
        return Sm8750SocInfoArgument;       /* the bins share no level */
    out->initial = at_or_below(out, start_khz);
    return Sm8750SocInfoOk;
}

Sm8750SocInfoStatus Sm8750GpuLevelsSelect(const Sm8750GpuBin *bins, unsigned count, uint32_t speed_bin,
                                          uint32_t soc_code, int fused, Sm8750GpuLevelTable *out)
{
    Sm8750SocInfoStatus s = Sm8750GpuLevelsCommon(bins, count, out);
    if (s)
        return s;
    const int bin = Sm8750GpuBinMatch(bins, count, speed_bin, soc_code);
    out->bin = (int8_t)bin;
    if (!fused || bin < 0)
        return Sm8750SocInfoOk;
    /* ARCHITECTURE 4.1.4: with pagani-gpu-speedbin=1 the matched bin's 967..1200 MHz levels are appended. The
     * common prefix is shared by every bin, so only the levels above its fastest one are new. */
    const Sm8750GpuBin *b = &bins[bin];
    const uint32_t top = out->levels[out->count - 1].khz;
    for (unsigned i = 0; i < b->count; i++)
        if (b->levels[i].khz > top)
            push(out, &b->levels[i]);
    out->fused = 1;
    return Sm8750SocInfoOk;
}
