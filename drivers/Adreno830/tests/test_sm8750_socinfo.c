/* SPDX-License-Identifier: MIT */
/* Host model of the PKX110 SMEM window (0x81D00000, 2 MiB) laid out as boot firmware leaves it: the
 * SMEM header in page 0, the "$TOC" partition table in the last 4 KiB, the "$PRT" global partition at
 * 0x1000 with uncached and cached items (canary 0xA5A5), and private partitions above 0x9E000. Exactly
 * as on the phone, only [0x1000, 0x9E000) and [0x1FF000, 0x200000) can be read: any other access is a
 * synchronous external abort there and aborts this test. Also covers the qfprom speed-bin read and the
 * GPU level table selection on the tables generated from the phone's device tree. */
#include "../platform/sm8750/Sm8750GpuLevels.h"
#include "../platform/sm8750/Sm8750SocInfo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                    \
            fprintf(stderr, __VA_ARGS__);                                                                      \
            fputc('\n', stderr);                                                                               \
            abort();                                                                                           \
        }                                                                                                      \
    } while (0)

enum {
    SmemBytes = 0x200000, Toc = 0x1ff000, Global = 0x1000, GlobalBytes = 0x9d000, Cacheline = 0x40,
    SocInfoBytes = 0xe0,
};

typedef struct {
    uint8_t *mem;
    uint32_t reads;
    uint8_t touched[SmemBytes / 0x1000];
    uint32_t fail_at;      /* the read32 op reports failure at this read (1-based), 0 = never */
} Smem;

static int readable(uint32_t offset)
{
    return (offset >= 0x1000 && offset + 4 <= 0x9e000) || (offset >= 0x1ff000 && offset + 4 <= 0x200000);
}

static int smem_read(void *cookie, uint32_t offset, uint32_t *value)
{
    Smem *m = cookie;
    if ((offset & 3) || offset >= SmemBytes || !readable(offset)) {
        fprintf(stderr, "FAIL: SMEM read at %#x (page %#x): synchronous external abort on PKX110\n", offset,
                offset & ~0xfffu);
        abort();
    }
    m->reads++;
    if (m->fail_at && m->reads == m->fail_at)
        return -1;
    memcpy(value, m->mem + offset, 4);
    m->touched[offset >> 12] = 1;
    return 0;
}

static void put32(Smem *m, uint32_t at, uint32_t value)
{
    memcpy(m->mem + at, &value, 4);
}

static uint32_t get32(const Smem *m, uint32_t at)
{
    uint32_t value;
    memcpy(&value, m->mem + at, 4);
    return value;
}

typedef struct {
    uint32_t fmt, id, ver, feature_code, pcode, bytes;
    int cached;            /* place item 137 in the cached list instead */
    int duplicate;         /* also put an item 137 in the cached list */
    int missing;           /* no item 137 at all */
    unsigned filler;       /* uncached items before it */
    uint32_t part_bytes;   /* global partition size, 0 = 0x9D000 (ends at 0x9E000) */
} Layout;

/* An uncached item; returns the data offset (relative to the SMEM base). */
static uint32_t add_uncached(Smem *m, uint32_t *free_uncached, uint16_t item, uint32_t bytes, uint16_t pad_data)
{
    const uint32_t at = Global + *free_uncached;
    put32(m, at, 0xa5a5u | (uint32_t)item << 16);
    put32(m, at + 4, bytes);
    put32(m, at + 8, pad_data);              /* padding_hdr 0 */
    put32(m, at + 12, 0);
    *free_uncached += 16 + bytes;
    return at + 16;
}

/* A cached item: header at the top of its cache line slot, data right below. Returns the data offset. */
static uint32_t add_cached(Smem *m, uint32_t *free_cached, uint16_t item, uint32_t bytes)
{
    const uint32_t header = Global + *free_cached - Cacheline;
    put32(m, header, 0xa5a5u | (uint32_t)item << 16);
    put32(m, header + 4, bytes);
    put32(m, header + 8, 0);
    put32(m, header + 12, 0);
    *free_cached -= Cacheline + bytes;
    return header - bytes;
}

/* struct socinfo, only the fields that fit in the item. */
static void socinfo(Smem *m, uint32_t at, const Layout *l)
{
    const struct {
        uint32_t offset, value;
    } fields[] = {
        {0x00, l->fmt}, {0x04, l->id}, {0x08, l->ver}, {0x2c, 0x24c /* raw_id */}, {0x30, 2 /* raw_ver */},
        {0xb0, l->feature_code}, {0xb4, l->pcode},
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (fields[i].offset + 4 <= l->bytes)
            put32(m, at + fields[i].offset, fields[i].value);
    if (l->bytes >= 0x0c + 20)
        memcpy(m->mem + at + 0x0c, "PKX110.SM8750.BUILD", 20);
}

static void build(Smem *m, const Layout *l)
{
    const uint32_t part = l->part_bytes ? l->part_bytes : GlobalBytes;
    memset(m->mem, 0x5a, SmemBytes);
    memset(m->touched, 0, sizeof(m->touched));
    m->reads = 0;
    /* smem_header in page 0 as SBL leaves it (version 12 = partitioned); never readable on PKX110. */
    memset(m->mem, 0, 0x1000);
    put32(m, 64 + 7 * 4, 0x000c0000);       /* version[SMEM_MASTER_SBL_VERSION_INDEX] */
    put32(m, 0xc0, 1);                       /* initialized */

    /* "$TOC": private partitions (APPS with MODEM/ADSP/CDSP) above 0x9E000, an empty slot, the global one. */
    memset(m->mem + Toc, 0, 0x1000);
    put32(m, Toc, 0x434f5424);
    put32(m, Toc + 4, 1);
    static const uint32_t entries[][4] = {
        {0x0a0000, 0x40000, 0x00010000, 0x40},   /* offset, size, host0 | host1 << 16, cacheline */
        {0x0e0000, 0x20000, 0x00020000, 0x40},
        {0, 0, 0, 0},
        {Global, GlobalBytes, 0xfffefffe, Cacheline},
        {0x100000, 0x20000, 0x00050000, 0x40},
    };
    const uint32_t n = sizeof(entries) / sizeof(entries[0]);
    put32(m, Toc + 8, n);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t e = Toc + 0x20 + 48 * i;
        put32(m, e, entries[i][0]);
        put32(m, e + 4, entries[i][1]);
        put32(m, e + 8, 0);
        put32(m, e + 12, entries[i][2]);
        put32(m, e + 16, entries[i][3]);
    }
    put32(m, Toc + 0x20 + 48 * 3 + 4, part);
    /* Private partitions with their own valid headers: present in the image, unreadable from XNU. */
    for (uint32_t i = 0; i < n; i++)
        if (entries[i][0] > 0x9e000) {
            put32(m, entries[i][0], 0x54525024);
            put32(m, entries[i][0] + 4, entries[i][2]);
            put32(m, entries[i][0] + 8, entries[i][1]);
        }

    /* "$PRT" global partition: items as SBL allocates them (sizes 8-aligned, padding_data the slack). */
    memset(m->mem + Global, 0, part);
    uint32_t free_uncached = 0x20, free_cached = part;
    add_uncached(m, &free_uncached, 1, 0x40, 0);
    add_uncached(m, &free_uncached, 3, 0x88, 4);
    for (unsigned i = 0; i < l->filler; i++)
        add_uncached(m, &free_uncached, (uint16_t)(400 + i), 0x18 + 8 * (i % 5), (uint16_t)(i % 8));
    uint32_t item = 0;
    if (!l->missing && !l->cached)
        item = add_uncached(m, &free_uncached, 137, l->bytes, 0);
    add_uncached(m, &free_uncached, 418, 0x100, 0);
    add_cached(m, &free_cached, 500, 0x80);
    if (!l->missing && (l->cached || l->duplicate)) {
        const uint32_t at = add_cached(m, &free_cached, 137, l->bytes);
        if (!item)
            item = at;
        else
            socinfo(m, at, l);
    }
    add_cached(m, &free_cached, 501, 0x40);
    if (item)
        socinfo(m, item, l);
    put32(m, Global, 0x54525024);
    put32(m, Global + 4, 0xfffefffe);
    put32(m, Global + 8, part);
    put32(m, Global + 12, free_uncached);
    put32(m, Global + 16, free_cached);
}

static const Layout retail = {0x14, 618, 0x20000, 0x1, 0, SocInfoBytes, 0, 0, 0, 0, 0};

static Sm8750SocInfoStatus parse(Smem *m, Sm8750SocInfo *info)
{
    const Sm8750SocInfoIo io = {m, smem_read};
    return Sm8750SocInfoRead(&io, info);
}

/* Toc entry 3 is the global one in build(). */
static uint32_t global_entry(void)
{
    return Toc + 0x20 + 48 * 3;
}

static uint32_t rng_state = 0x8750830u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef struct {
    uint32_t words[0x1000 / 4];
    int fail;
} Qfprom;

static int qfprom_read(void *cookie, uint32_t offset, uint32_t *value)
{
    Qfprom *q = cookie;
    if ((offset & 3) || offset >= 0x1000) {
        fprintf(stderr, "FAIL: qfprom read at %#x\n", offset);
        abort();
    }
    if (q->fail)
        return -1;
    *value = q->words[offset / 4];
    return 0;
}

static void smem_tests(void)
{
    Smem smem = {calloc(1, SmemBytes), 0, {0}, 0}, *m = &smem;
    Sm8750SocInfo info;
    Sm8750SocInfoStatus s;
    Layout l;
    CHECK(m->mem != NULL, "allocation");

    /* 1. Retail part, item in the uncached list. */
    build(m, &retail);
    s = parse(m, &info);
    CHECK(s == Sm8750SocInfoOk && info.id == 618 && info.revision == 0x20000 && info.fmt == 0x14, "retail: %d id %u",
          s, info.id);
    CHECK(info.raw_id == 0x24c && info.raw_revision == 2 && info.feature_code == 1 && info.pcode == 0 &&
              info.soc_code == 1 && (info.flags & Sm8750SocInfoFlagUncached) && !(info.flags & Sm8750SocInfoFlagNoFeature),
          "retail fields: fc %#x soc_code %#x flags %#x", info.feature_code, info.soc_code, info.flags);
    CHECK(info.partition_offset == Global && info.partition_bytes == GlobalBytes && info.cacheline == Cacheline &&
              info.item_bytes == SocInfoBytes && info.item_offset > Global && info.item_offset < 0x9e000,
          "partition %#x+%#x item %#x", info.partition_offset, info.partition_bytes, info.item_offset);
    CHECK(!m->touched[0] && m->touched[Toc >> 12] && m->touched[Global >> 12], "pages: header page must not be read");
    for (unsigned page = 0x9e; page < 0x1ff; page++)
        CHECK(!m->touched[page], "page %#x read", page << 12);
    CHECK(info.reads == m->reads && m->reads < 200, "reads %u", m->reads);
    printf("smem: soc %u rev %#x fmt %#x fc %#x soc_code %#x item %#x+%#x in %u reads\n", info.id, info.revision,
           info.fmt, info.feature_code, info.soc_code, info.item_offset, info.item_bytes, info.reads);

    /* 2. The item in the cached list (header at the top of a cache line, data below). */
    l = retail;
    l.cached = 1;
    build(m, &l);
    s = parse(m, &info);
    CHECK(s == Sm8750SocInfoOk && info.id == 618 && !(info.flags & Sm8750SocInfoFlagUncached) &&
              info.item_offset + info.item_bytes <= Global + GlobalBytes - Cacheline,
          "cached: %d", s);

    /* 3. KGSL soc_code: internal feature codes Y0..YF carry the pcode, retail ones do not. */
    l = retail;
    l.feature_code = 0xf2;
    l.pcode = 0x1234;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.soc_code == 0x123400f2, "internal: %#x", info.soc_code);
    l.feature_code = 0x100;              /* SOCINFO_FC_YF is still internal */
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.soc_code == 0x12340100, "YF: %#x", info.soc_code);
    l.feature_code = 0x101;              /* beyond SOCINFO_FC_INT_MAX: unknown */
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.soc_code == 0 && info.feature_code == 0x101, "beyond: %#x",
          info.soc_code);
    l.feature_code = 0x3;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.soc_code == 0x3, "retail pcode ignored: %#x", info.soc_code);
    CHECK(Sm8750SocCode(0x14, 0xf1, 0x7fffffff) == 0xf1 && Sm8750SocCode(0x14, 0xf1, 0x80000001) == 0xf1 &&
              Sm8750SocCode(0xf, 0xf1, 5) == 0 && Sm8750SocCode(0x10, 0xf1, 0x10005) == 0x500f1,
          "Sm8750SocCode");

    /* 4. Formats before 0.16 and items that end early have no feature code; too small to hold id: Short. */
    l = retail;
    l.fmt = 0xf;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && (info.flags & Sm8750SocInfoFlagNoFeature) && info.soc_code == 0 &&
              info.feature_code == 0,
          "fmt 0.15");
    l = retail;
    l.bytes = 0x40;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && (info.flags & Sm8750SocInfoFlagNoFeature) && info.id == 618, "short item");
    l.bytes = 8;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoShort, "8-byte item");

    /* 5. Missing and duplicate items. */
    l = retail;
    l.missing = 1;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoMissing, "missing");
    l = retail;
    l.duplicate = 1;                     /* Linux takes the uncached one and never walks the cached list */
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && (info.flags & Sm8750SocInfoFlagUncached) &&
              !m->touched[(Global + GlobalBytes - 1) >> 12],
          "duplicate");

    /* 6. A long uncached list. */
    l = retail;
    l.filler = 300;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.id == 618 && info.reads < 1200, "filler: reads %u", info.reads);

    /* 7. Partition table damage. */
    build(m, &retail);
    put32(m, Toc, 0x434f5425);
    CHECK(parse(m, &info) == Sm8750SocInfoNoTable && m->reads <= 3, "magic");
    build(m, &retail);
    put32(m, Toc + 4, 2);
    CHECK(parse(m, &info) == Sm8750SocInfoNoTable, "version");
    build(m, &retail);
    put32(m, Toc + 8, 85);
    CHECK(parse(m, &info) == Sm8750SocInfoNoTable, "85 entries");
    build(m, &retail);
    put32(m, Toc + 8, 0);
    CHECK(parse(m, &info) == Sm8750SocInfoNoTable, "no entries");
    build(m, &retail);
    put32(m, global_entry() + 12, 0x0000fffe);
    CHECK(parse(m, &info) == Sm8750SocInfoNoPartition, "no global entry");
    build(m, &retail);
    put32(m, Toc + 0x20 + 48 * 2 + 12, 0xfffefffe);       /* the empty slot becomes global but has no offset */
    CHECK(parse(m, &info) == Sm8750SocInfoOk, "empty global slot is skipped");
    put32(m, Toc + 0x20 + 48 * 2, 0x2000);
    put32(m, Toc + 0x20 + 48 * 2 + 4, 0x1000);
    CHECK(parse(m, &info) == Sm8750SocInfoBadPartition, "two global entries");

    /* 8. Global entries that would make us touch unreadable pages are refused before any such read. */
    build(m, &retail);
    put32(m, global_entry(), 0xa0000);                    /* points into the modem partition */
    CHECK(parse(m, &info) == Sm8750SocInfoOutOfBounds, "global in private pages");
    build(m, &retail);
    put32(m, global_entry() + 4, GlobalBytes + 0x1000);   /* table and "$PRT" header disagree */
    CHECK(parse(m, &info) == Sm8750SocInfoBadPartition && m->reads < 20, "entry size");
    /* A partition that really reaches the unreadable page at 0x9E000: an uncached item 137 is found without
     * going near it; a cached one would need that page and is refused, not read. */
    l = retail;
    l.part_bytes = GlobalBytes + 0x1000;
    build(m, &l);
    CHECK(parse(m, &info) == Sm8750SocInfoOk && info.partition_bytes == GlobalBytes + 0x1000 && info.id == 618 &&
              !m->touched[0x9d],
          "long partition, uncached item");
    l.cached = 1;
    build(m, &l);
    s = parse(m, &info);
    CHECK(s == Sm8750SocInfoOutOfBounds && info.last_offset == Global + GlobalBytes + 0x1000 - Cacheline,
          "long partition, cached item: %d at %#x", s, info.last_offset);
    build(m, &retail);
    put32(m, global_entry(), 0x1ff000 - 0x20);            /* overlaps the table */
    put32(m, global_entry() + 4, 0x40);
    CHECK(parse(m, &info) == Sm8750SocInfoBadPartition, "overlaps the table");
    build(m, &retail);
    put32(m, global_entry(), 0);                          /* would be the header page: entry is unused */
    CHECK(parse(m, &info) == Sm8750SocInfoNoPartition, "offset 0");
    build(m, &retail);
    put32(m, global_entry(), 0x1004);
    CHECK(parse(m, &info) == Sm8750SocInfoBadPartition, "unaligned partition");
    static const uint32_t bad_cachelines[] = {0, 3, 0x30, 0x2000};
    for (unsigned i = 0; i < 4; i++) {
        build(m, &retail);
        put32(m, global_entry() + 16, bad_cachelines[i]);
        CHECK(parse(m, &info) == Sm8750SocInfoBadPartition, "cacheline %#x", bad_cachelines[i]);
    }

    /* 9. Partition header damage. */
    static const struct {
        uint32_t at, value;
        const char *what;
    } header_damage[] = {
        {0, 0x54525025, "magic"},          {4, 0xfffe0000, "hosts"},           {8, GlobalBytes - 8, "size"},
        {12, 0x10, "free_uncached < 0x20"}, {12, GlobalBytes - 0x10, "uncached past cached"},
        {16, GlobalBytes + 8, "free_cached past end"},
    };
    for (unsigned i = 0; i < sizeof(header_damage) / sizeof(header_damage[0]); i++) {
        build(m, &retail);
        put32(m, Global + header_damage[i].at, header_damage[i].value);
        CHECK(parse(m, &info) == Sm8750SocInfoBadPartition, "header %s", header_damage[i].what);
    }

    /* 10. Item list damage: canary, padding, sizes running past the list end, unaligned walk. */
    build(m, &retail);
    put32(m, Global + 0x20, 0xa5a40001);
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "canary");
    build(m, &retail);
    put32(m, Global + 0x28, 0x41);                        /* padding_data > size */
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "padding");
    build(m, &retail);
    put32(m, Global + 0x24, 0x9d000);                     /* size runs past free_uncached */
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "size");
    build(m, &retail);
    put32(m, Global + 0x24, 0x42);                        /* next entry unaligned */
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "alignment");
    build(m, &retail);
    CHECK(parse(m, &info) == Sm8750SocInfoOk, "retail again");
    put32(m, info.item_offset - 16 + 8, 2u << 16);        /* padding_hdr 2: item 137's data no longer aligned */
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "unaligned item");
    l = retail;
    l.cached = 1;                                         /* the cached list is walked only for a cached item */
    build(m, &l);
    put32(m, Global + GlobalBytes - Cacheline + 4, GlobalBytes);   /* cached item larger than the free gap */
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "cached size");
    build(m, &l);
    put32(m, Global + GlobalBytes - Cacheline, 0xa5a40000u | 500u << 16);
    CHECK(parse(m, &info) == Sm8750SocInfoCorrupt, "cached canary");

    /* 11. Fuzz: random damage to the table and the partition's lists. Whatever the outcome, the walk ends and
     *     never reads outside the readable pages (smem_read aborts otherwise). */
    unsigned outcomes[16] = {0}, max_reads = 0;
    for (unsigned i = 0; i < 20000; i++) {
        build(m, (i & 1) ? &retail : &(Layout){0x14, 618, 0x20000, 0xf1, 7, SocInfoBytes, 1, 0, 0, 3, 0});
        const unsigned damage = 1 + rnd() % 4;
        for (unsigned d = 0; d < damage; d++) {
            const uint32_t pick = rnd() % 4, value = (rnd() & 1) ? rnd() : rnd() % 0x200000;
            uint32_t at;
            if (pick == 0)
                at = Toc + 4 * (rnd() % (0x20 / 4 + 5 * 12));                 /* table header and entries */
            else if (pick == 1)
                at = Global + 4 * (rnd() % 0x80);                             /* partition header, first items */
            else if (pick == 2)
                at = Global + GlobalBytes - 4 * (1 + rnd() % 0x80);           /* cached items */
            else
                at = Global + 4 * (rnd() % (0x400 / 4));
            put32(m, at, value);
        }
        s = parse(m, &info);
        CHECK((unsigned)s < 16 && info.reads < 250000, "fuzz %u: status %d reads %u", i, s, info.reads);
        if (info.reads > max_reads)
            max_reads = info.reads;
        outcomes[s]++;
    }
    printf("fuzz (max %u reads): ok %u no-table %u no-partition %u bad-partition %u corrupt %u missing %u short %u out-of-bounds %u\n",
           max_reads, outcomes[Sm8750SocInfoOk], outcomes[Sm8750SocInfoNoTable], outcomes[Sm8750SocInfoNoPartition],
           outcomes[Sm8750SocInfoBadPartition], outcomes[Sm8750SocInfoCorrupt], outcomes[Sm8750SocInfoMissing],
           outcomes[Sm8750SocInfoShort], outcomes[Sm8750SocInfoOutOfBounds]);
    CHECK(outcomes[Sm8750SocInfoOk] && outcomes[Sm8750SocInfoCorrupt] && outcomes[Sm8750SocInfoBadPartition],
          "fuzz covers the outcomes");

    /* 12. A read the window op reports as failed stops the walk at that read, wherever it is. */
    build(m, &retail);
    const uint32_t total = m->reads, clean = (parse(m, &info), m->reads);
    CHECK(total == 0 && clean == info.reads, "clean read count");
    for (uint32_t at = 1; at <= clean; at++) {
        build(m, &retail);
        m->fail_at = at;
        CHECK(parse(m, &info) == Sm8750SocInfoIoError && m->reads == at && info.reads == at, "read %u fails", at);
    }
    m->fail_at = 0;
    /* A partition whose size is not a multiple of 4 puts the cached list's first header off alignment: refused
     * before it is read. */
    l = retail;
    l.cached = 1;
    l.part_bytes = GlobalBytes - 2;
    build(m, &l);
    s = parse(m, &info);
    CHECK(s == Sm8750SocInfoCorrupt && info.last_offset < Global + GlobalBytes - 0x80, "unaligned cached list: %d at %#x",
          s, info.last_offset);

    /* 13. Arguments. */
    const Sm8750SocInfoIo none = {m, NULL};
    CHECK(Sm8750SocInfoRead(&none, &info) == Sm8750SocInfoArgument && Sm8750SocInfoRead(NULL, &info) ==
              Sm8750SocInfoArgument,
          "arguments");
    CHECK(get32(m, 0) == 0, "image");
    free(m->mem);
}

static void speed_bin_tests(void)
{
    Qfprom q;
    const Sm8750SocInfoIo io = {&q, qfprom_read};
    uint32_t bin = 0;
    memset(&q, 0xff, sizeof(q));
    q.fail = 0;
    q.words[0x138 / 4] = 0xfe0000be;           /* gpu_speed_bin@138 = 0xBE, neighbours set */
    CHECK(Sm8750SpeedBinRead(&io, Sm8750SpeedBinOffset, Sm8750SpeedBinShift, Sm8750SpeedBinBits, &bin) ==
              Sm8750SocInfoOk && bin == 0xbe,
          "bin %#x", bin);
    q.words[0x138 / 4] = 0xffff01f2;           /* bit 8 belongs to the cell */
    CHECK(Sm8750SpeedBinRead(&io, 0x138, 0, 9, &bin) == Sm8750SocInfoOk && bin == 0x1f2, "bin %#x", bin);
    q.words[0x13c / 4] = 0x00c00000;           /* ssip_config@13e: bits 7..8 of the halfword at 0x13e */
    CHECK(Sm8750SpeedBinRead(&io, 0x13e, 7, 2, &bin) == Sm8750SocInfoOk && bin == 1, "ssip %#x", bin);
    CHECK(Sm8750SpeedBinRead(&io, 0x1000, 0, 9, &bin) == Sm8750SocInfoOutOfBounds, "past window");
    CHECK(Sm8750SpeedBinRead(&io, 0x13e, 8, 9, &bin) == Sm8750SocInfoArgument, "spans two words");
    CHECK(Sm8750SpeedBinRead(&io, 0x138, 0, 0, &bin) == Sm8750SocInfoArgument, "no bits");
    CHECK(Sm8750SpeedBinRead(&io, 0x138, 0, 32, &bin) == Sm8750SocInfoOk && bin == q.words[0x138 / 4], "32 bits");
    bin = 0x5a5;
    q.fail = 1;
    CHECK(Sm8750SpeedBinRead(&io, 0x138, 0, 9, &bin) == Sm8750SocInfoIoError && bin == 0x5a5, "failed read keeps *bin");
    q.fail = 0;
    const Sm8750SocInfoIo no_read = {&q, NULL};
    CHECK(Sm8750SpeedBinRead(&no_read, 0x138, 0, 9, &bin) == Sm8750SocInfoArgument &&
              Sm8750SpeedBinRead(NULL, 0x138, 0, 9, &bin) == Sm8750SocInfoArgument &&
              Sm8750SpeedBinRead(&io, 0x138, 32, 1, &bin) == Sm8750SocInfoArgument &&
              Sm8750SpeedBinRead(&io, 0x138, 0, 33, &bin) == Sm8750SocInfoArgument &&
              Sm8750SpeedBinRead(&io, 0x138, 0, 9, NULL) == Sm8750SocInfoArgument,
          "speed bin arguments");
    /* The generated cell geometry is the one the kernel reads. */
    CHECK(Sm8750DtSpeedBinOffset == Sm8750SpeedBinOffset && Sm8750DtSpeedBinShift == Sm8750SpeedBinShift &&
              Sm8750DtSpeedBinBits == Sm8750SpeedBinBits && SM8750_DT_QFPROM_PHYS == 0x221c8000ull &&
              SM8750_DT_QFPROM_BYTES == Sm8750QfpromBytes && SM8750_DT_SMEM_PHYS == 0x81d00000ull &&
              SM8750_DT_SMEM_BYTES == Sm8750SmemBytes,
          "generated geometry");
}

static int same_level(const Sm8750GpuLevel *a, const Sm8750GpuLevel *b)
{
    return a->khz == b->khz && a->vlvl == b->vlvl && a->bus_min == b->bus_min && a->bus == b->bus &&
           a->bus_max == b->bus_max && a->acd == b->acd;
}

static void level_tests(void)
{
    const Sm8750GpuBin *bins = Sm8750DtGpuBins;
    const unsigned n = Sm8750DtGpuBinCount;
    Sm8750GpuLevelTable t;

    /* KGSL bin match on the phone's six bins (DT order). */
    static const struct {
        uint32_t speed, soc_code;
        int bin;
    } matches[] = {
        {0xbe, 0, 0}, {0xbe, 1, 0}, {0xdd, 0, 1}, {0xe8, 3, 2}, {0xf2, 0, 4}, {0xf2, 1, 3}, {0xf2, 2, 3},
        {0x00, 1, 3}, {0x00, 0, 5}, {0x123, 3, 5}, {0x1be, 0x123400f2, 5},
    };
    for (unsigned i = 0; i < sizeof(matches) / sizeof(matches[0]); i++)
        CHECK(Sm8750GpuBinMatch(bins, n, matches[i].speed, matches[i].soc_code) == matches[i].bin,
              "speed %#x soc_code %#x -> %d", matches[i].speed, matches[i].soc_code,
              Sm8750GpuBinMatch(bins, n, matches[i].speed, matches[i].soc_code));
    CHECK(Sm8750GpuBinMatch(bins, 3, 0x00, 0) == -1 && Sm8750GpuBinMatch(NULL, 6, 0xbe, 0) == -1, "no match");

    /* The common table computed here is the generated one and docs/ARCHITECTURE.md 4.1.4. */
    CHECK(Sm8750GpuLevelsCommon(bins, n, &t) == Sm8750SocInfoOk && t.count == Sm8750DtGpuCommonCount && t.count == 11 &&
              t.initial == Sm8750DtGpuCommonInitial && t.initial == 1 && t.bin == -1 && !t.fused,
          "common: %u levels initial %u", t.count, t.initial);
    for (unsigned i = 0; i < t.count; i++)
        CHECK(same_level(&t.levels[i], &Sm8750DtGpuCommonLevels[i]) && !t.levels[i].reserved, "common level %u", i);
    static const Sm8750GpuLevel doc[11] = {
        {160000, 0x32, 2, 2, 2, 0, 0},           {222000, 0x34, 2, 2, 2, 0, 0},
        {342000, 0x38, 2, 3, 3, 0, 0},           {389000, 0x3c, 2, 4, 6, 0, 0xc8285ffd},
        {443000, 0x40, 2, 4, 6, 0, 0xc02e5ffd},  {525000, 0x50, 2, 4, 6, 0, 0xc02e5ffd},
        {607000, 0x80, 4, 6, 8, 0, 0xa82e5ffd},  {660000, 0x90, 4, 6, 8, 0, 0x882c5ffd},
        {734000, 0xc0, 6, 8, 10, 0, 0xa82a5ffd}, {832000, 0xe0, 7, 10, 10, 0, 0x882a5ffd},
        {900000, 0x100, 7, 10, 10, 0, 0x882a5ffd},
    };
    for (unsigned i = 0; i < 11; i++)
        CHECK(same_level(&t.levels[i], &doc[i]), "4.1.4 row %u", i + 1);
    static const uint32_t ddr[12] = {0,       781250,  2136718,  5285156,  6074218,  6671875,
                                     8171875, 10687500, 12449218, 14398437, 16500000, 18597656};
    CHECK(Sm8750DtDdrLevels == 12 && !memcmp(Sm8750DtDdrKbps, ddr, sizeof(ddr)) && Sm8750DtCnoc[1] == 100 &&
              Sm8750DtGmuKhz[0] == 500000 && Sm8750DtGmuKhz[1] == 650000 && Sm8750DtGmuVlvl[0] == 0x40 &&
              Sm8750DtGmuVlvl[1] == 0x80,
          "bus and GMU tables");
    CHECK(Sm8750DtSocIdCount == 2 && Sm8750DtSocIds[0] == 618 && Sm8750DtSocIds[1] == 639 &&
              Sm8750DtScmWaitqIntid == 994 && Sm8750DtScmWaitqEdge && Sm8750DtScmWaitqQueues == 2,
          "SoC ids and SCM wait queue");
    for (unsigned b = 0; b < n; b++)
        for (unsigned i = 0; i < bins[b].count; i++)
            CHECK(bins[b].levels[i].bus_max < Sm8750DtDdrLevels, "bin %u level %u bus", b, i);

    /* Default (fuse ignored): the common table whatever the bin; the match is only recorded. */
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0xdd, 0, 0, &t) == Sm8750SocInfoOk && t.bin == 1 && !t.fused && t.count == 11 &&
              t.initial == 1 && t.levels[10].khz == 900000 && t.levels[10].bus == 10,
          "default dd");
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0xbe, 0, 0, &t) == Sm8750SocInfoOk && t.bin == 0 && t.levels[10].bus_min == 7,
          "default be");

    /* Fused (pagani-gpu-speedbin=1, ARCHITECTURE 4.1.4): the common table with the matched bin's faster levels
     * appended, a repeated level once; the common levels, their bus votes and the initial level do not move. */
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0xdd, 0, 1, &t) == Sm8750SocInfoOk && t.fused && t.bin == 1 && t.count == 13 &&
              t.levels[12].khz == 1050000 && t.levels[11].khz == 967000 && t.initial == 1 &&
              t.levels[t.initial].khz == 222000,
          "fused dd: %u levels, initial %u", t.count, t.initial);
    for (unsigned i = 0; i < 11; i++)
        CHECK(same_level(&t.levels[i], &Sm8750DtGpuCommonLevels[i]), "fused dd keeps common level %u", i);
    CHECK(same_level(&t.levels[11], &Sm8750DtGpuBin1Levels[11]) && same_level(&t.levels[12], &Sm8750DtGpuBin1Levels[12]),
          "fused dd appends the bin's own levels");
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0xbe, 0, 1, &t) == Sm8750SocInfoOk && t.fused && t.bin == 0 && t.count == 11 &&
              t.levels[10].bus_min == 7 && t.levels[10].bus == 10 && t.levels[10].bus_max == 10 &&
              t.levels[t.initial].khz == 222000,
          "fused be: nothing above 900 MHz to append, 900 MHz keeps the common votes");
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0x77, 0x3, 1, &t) == Sm8750SocInfoOk && t.bin == 5 && t.count == 15 &&
              t.levels[14].khz == 1200000 && t.levels[14].vlvl == 0x1c4 && t.levels[t.initial].khz == 222000 &&
              t.levels[11].bus_min == 10 && t.levels[11].bus_max == 11,
          "fused sku 3");
    CHECK(Sm8750GpuLevelsSelect(bins, n, 0xf2, 0x0, 1, &t) == Sm8750SocInfoOk && t.bin == 4 && t.count == 14 &&
              t.levels[13].khz == 1150000,
          "fused f2");
    for (unsigned i = 1; i < t.count; i++)
        CHECK(t.levels[i].khz > t.levels[i - 1].khz, "ascending %u", i);
    for (unsigned b = 0; b < n; b++) {
        CHECK(Sm8750GpuLevelsSelect(bins, n, bins[b].speed_bin, bins[b].sku[0], 1, &t) == Sm8750SocInfoOk &&
                  t.count <= Sm8750GpuMaxLevels && t.levels[t.count - 1].khz == bins[b].levels[bins[b].count - 1].khz,
              "fused bin %u reaches its fastest level", b);
    }
    /* No bin: the common table, as with the fuse ignored. */
    CHECK(Sm8750GpuLevelsSelect(bins, 3, 0x00, 0, 1, &t) == Sm8750SocInfoOk && t.bin == -1 && !t.fused && t.count == 11,
          "fused without match");

    /* KGSL rules the phone's tree does not exercise: a bin with both a speed bin and sku codes needs both;
     * a repeated level below the initial one must not shift the initial level. */
    const Sm8750GpuLevel steps[4] = {{100000, 0x30, 1, 1, 1, 0, 0}, {100000, 0x30, 1, 1, 1, 0, 0},
                                     {200000, 0x40, 2, 2, 2, 0, 0}, {300000, 0x50, 3, 3, 3, 0, 0}};
    const Sm8750GpuBin both[2] = {
        {Sm8750GpuBinHasSpeed | Sm8750GpuBinHasSku, 0xaa, {5, 0, 0, 0}, 1, 1, 4, 0, steps},
        {Sm8750GpuBinHasSku, 0, {0, 0, 0, 0}, 1, 1, 4, 0, steps},
    };
    CHECK(Sm8750GpuBinMatch(both, 2, 0xaa, 5) == 0 && Sm8750GpuBinMatch(both, 2, 0xbb, 5) == 1 &&
              Sm8750GpuBinMatch(both, 2, 0xaa, 6) == 1,
          "speed bin and sku codes together");
    CHECK(Sm8750GpuLevelsSelect(both, 2, 0xaa, 5, 1, &t) == Sm8750SocInfoOk && t.bin == 0 && t.count == 3 &&
              t.levels[t.initial].khz == 200000 && t.initial == 1 && t.fused,
          "initial after a repeated level: %u", t.initial);
    CHECK(Sm8750GpuLevelsCommon(both, 2, &t) == Sm8750SocInfoOk && t.count == 3 && t.levels[t.initial].khz == 200000,
          "common initial after a repeated level");
    /* Only levels faster than the common top are appended: a same-frequency variant of it is no new level. */
    const Sm8750GpuLevel short_levels[2] = {{100000, 0x30, 1, 1, 1, 0, 0}, {200000, 0x40, 2, 2, 2, 0, 0}};
    const Sm8750GpuLevel long_levels[4] = {{100000, 0x30, 1, 1, 1, 0, 0}, {200000, 0x40, 2, 2, 2, 0, 0},
                                           {200000, 0x48, 2, 2, 2, 0, 0}, {300000, 0x50, 3, 3, 3, 0, 0}};
    const Sm8750GpuBin uneven[2] = {
        {Sm8750GpuBinHasSpeed, 0x10, {0, 0, 0, 0}, 0, 0, 2, 0, short_levels},
        {Sm8750GpuBinHasSpeed, 0x20, {0, 0, 0, 0}, 0, 0, 4, 0, long_levels},
    };
    CHECK(Sm8750GpuLevelsSelect(uneven, 2, 0x20, 0, 1, &t) == Sm8750SocInfoOk && t.bin == 1 && t.fused && t.count == 3 &&
              t.levels[1].vlvl == 0x40 && t.levels[2].khz == 300000 && t.levels[t.initial].khz == 200000,
          "append only faster levels: %u", t.count);

    /* Bad tables are refused. */
    Sm8750GpuLevel lv[3] = {{100000, 0x30, 1, 1, 1, 0, 0}, {200000, 0x40, 1, 1, 1, 0, 0}, {150000, 0x50, 1, 1, 1, 0, 0}};
    Sm8750GpuBin bad = {Sm8750GpuBinHasSpeed, 1, {0}, 0, 0, 3, 0, lv};
    CHECK(Sm8750GpuLevelsCommon(&bad, 1, &t) == Sm8750SocInfoArgument, "descending level");
    lv[2].khz = 300000;
    bad.initial = 3;
    CHECK(Sm8750GpuLevelsCommon(&bad, 1, &t) == Sm8750SocInfoArgument, "initial past table");
    bad.initial = 0;
    bad.count = 17;
    CHECK(Sm8750GpuLevelsCommon(&bad, 1, &t) == Sm8750SocInfoArgument, "17 levels");
    bad.count = 3;
    CHECK(Sm8750GpuLevelsCommon(&bad, 1, &t) == Sm8750SocInfoOk && t.count == 3 && t.initial == 2, "single bin");
    Sm8750GpuLevel other[1] = {{110000, 0x30, 1, 1, 1, 0, 0}};
    const Sm8750GpuBin disjoint[2] = {bad, {0, 0, {0}, 0, 0, 1, 0, other}};
    CHECK(Sm8750GpuLevelsCommon(disjoint, 2, &t) == Sm8750SocInfoArgument, "nothing shared");
    CHECK(Sm8750GpuLevelsCommon(NULL, 2, &t) == Sm8750SocInfoArgument && Sm8750GpuLevelsCommon(bins, 0, &t) ==
              Sm8750SocInfoArgument,
          "arguments");
    Sm8750GpuBin broken = bad;
    lv[1].khz = 0;
    CHECK(Sm8750GpuLevelsCommon(&broken, 1, &t) == Sm8750SocInfoArgument, "zero frequency");
    lv[1].khz = 200000;
    broken.flags = Sm8750GpuBinHasSku;
    CHECK(Sm8750GpuLevelsCommon(&broken, 1, &t) == Sm8750SocInfoArgument, "sku codes flagged but none given");
    broken.sku_count = 5;
    CHECK(Sm8750GpuLevelsCommon(&broken, 1, &t) == Sm8750SocInfoArgument, "five sku codes");
    broken = bad;
    broken.levels = NULL;
    CHECK(Sm8750GpuLevelsSelect(&broken, 1, 1, 0, 1, &t) == Sm8750SocInfoArgument, "no levels");
}

int main(void)
{
    smem_tests();
    speed_bin_tests();
    level_tests();
    puts("PASS: SM8750 SoC info reads only the readable SMEM pages, finds socinfo item 137 (SoC 618), reads the "
         "qfprom speed bin and picks the DT level table");
    return 0;
}
