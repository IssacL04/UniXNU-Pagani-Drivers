/* SPDX-License-Identifier: MIT */
#ifndef SM8750_SOC_INFO_H
#define SM8750_SOC_INFO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* SoC identity, GPU speed bin and GPU level table of the SM8750 (A830) for board input stage S0.
 *
 * SMEM (reserved region 0x81D00000, 2 MiB) is read the way Linux drivers/soc/qcom/smem.c reads a
 * partitioned SMEM (version 12): the partition table ("$TOC") in the last 4 KiB, the global partition
 * (host0 == host1 == 0xFFFE, "$PRT") it points at, then item 137 (SMEM_HW_SW_BUILD_ID, struct socinfo):
 * first in the uncached list, then, only if it is not there, in the cached list at the partition's end.
 *
 * On PKX110 only [0x1000, 0x9E000) (the global partition) and [0x1FF000, 0x200000) (the partition
 * table) of the window can be read; any other page takes a synchronous external abort. Every access
 * here is an aligned 32-bit read through one guard that refuses, without issuing it, any read outside
 * both the readable pages and the structure being parsed. The SMEM header page (offset 0) is never read:
 * the partition table alone identifies the partitioned layout. The board maps the window Device-nGnRE
 * (kIOMapInhibitCache), or maps just the two readable ranges and dispatches read32 on the offset: a
 * cacheable or write-combined mapping of the whole window would let the CPU prefetch the faulting pages.
 *
 * The speed bin is the qfprom nvmem cell of the GPU node (0x221C8000 + 0x138, bits [8:0]), read as one
 * aligned 32-bit word (Device mapping).
 * The level table follows KGSL adreno_of_get_pwrlevels for the bin match; by default the driver uses
 * only the levels every bin shares, pagani-gpu-speedbin=1 appends the matched bin's faster levels
 * (docs/ARCHITECTURE.md 4.1.3, 4.1.4). */

enum {
    Sm8750SmemBytes = 0x200000,
    Sm8750SmemTocOffset = 0x1ff000,            /* partition table: last 4 KiB */
    Sm8750SmemGlobalStart = 0x1000,            /* readable on PKX110: the global partition */
    Sm8750SmemGlobalEnd = 0x9e000,
    Sm8750SmemSocInfoItem = 137,               /* SMEM_HW_SW_BUILD_ID */
    Sm8750QfpromBytes = 0x1000,
    Sm8750SpeedBinOffset = 0x138,              /* gpu_speed_bin@138: reg <0x138 2>, bits <0 9> */
    Sm8750SpeedBinShift = 0,
    Sm8750SpeedBinBits = 9,
};

/* One mapped window. Offsets are bytes from the window base; read32 is only ever called with an
 * aligned offset the guard accepted. Nonzero return = the access failed (host models). */
typedef struct {
    void *cookie;
    int (*read32)(void *cookie, uint32_t offset, uint32_t *value);
} Sm8750SocInfoIo;

typedef enum {
    Sm8750SocInfoOk,
    Sm8750SocInfoArgument,
    Sm8750SocInfoIoError,
    Sm8750SocInfoOutOfBounds,     /* a read would leave the readable pages or the structure: not issued */
    Sm8750SocInfoNoTable,         /* no "$TOC", unknown version, bad entry count */
    Sm8750SocInfoNoPartition,     /* no global entry in the table */
    Sm8750SocInfoBadPartition,    /* entry or "$PRT" header inconsistent, or more than one global entry */
    Sm8750SocInfoCorrupt,         /* item list: canary, sizes, alignment */
    Sm8750SocInfoMissing,         /* item 137 is not in the global partition */
    Sm8750SocInfoShort,           /* item 137 too small for id and version */
} Sm8750SocInfoStatus;

/* struct socinfo fields (Linux include/linux/soc/qcom/socinfo.h); fmt = major << 16 | minor. */
enum {
    Sm8750SocInfoFmtRaw = 0x2,                 /* raw_id, raw_ver */
    Sm8750SocInfoFmtFeature = 0x10,            /* feature_code, pcode (0.16) */
    Sm8750SocInfoFlagNoFeature = 1u << 0,      /* fmt < 0.16 or the item ends before pcode */
    Sm8750SocInfoFlagUncached = 1u << 1,       /* item found in the uncached list (else cached) */
};

typedef struct {
    uint32_t fmt, id, revision;                /* socinfo fmt, id (618 on PKX110), ver */
    uint32_t raw_id, raw_revision;             /* 0 when fmt < 0.2 */
    uint32_t feature_code, pcode;              /* as stored; 0 when absent */
    uint32_t soc_code;                         /* KGSL adreno_read_soc_code(): pcode << 16 | feature code */
    uint32_t flags;
    uint32_t item_offset, item_bytes;          /* item 137 data, from the SMEM base */
    uint32_t partition_offset, partition_bytes, cacheline;
    uint32_t reads;                            /* 32-bit reads issued */
    uint32_t last_offset;                      /* last offset read or refused */
} Sm8750SocInfo;

/* Fills *info (also on failure, as far as the walk got). */
Sm8750SocInfoStatus Sm8750SocInfoRead(const Sm8750SocInfoIo *smem, Sm8750SocInfo *info);

/* KGSL soc_code: the feature code (0 if above the internal range 0xF1..0x100); for internal SKUs the
 * pcode in bits [31:16]. Pure. */
uint32_t Sm8750SocCode(uint32_t fmt, uint32_t feature_code, uint32_t pcode);

/* One aligned 32-bit read of the qfprom window covering the nvmem cell at offset, then bits
 * [shift, shift + bits) of the cell. PKX110: (0x138, 0, 9). */
Sm8750SocInfoStatus Sm8750SpeedBinRead(const Sm8750SocInfoIo *qfprom, uint32_t offset, unsigned shift, unsigned bits,
                                       uint32_t *bin);

/* ---- GPU level tables ---- */

enum { Sm8750GpuMaxLevels = 16, Sm8750GpuMaxSkus = 4 };

/* One GPU level; the same layout as Adreno830Level (docs/ARCHITECTURE.md 3.5), so the board can hand the
 * generated tables to the driver as they are. */
typedef struct {
    uint32_t khz, vlvl;                        /* qcom,gpu-freq / 1000, qcom,level (RPMh voltage level) */
    uint8_t bus_min, bus, bus_max, reserved;   /* qcom,bus-min / bus-freq / bus-max: DDR table indices */
    uint32_t acd;                              /* qcom,acd-level, 0 = none */
} Sm8750GpuLevel;
#ifdef __cplusplus
static_assert(sizeof(Sm8750GpuLevel) == 16 && offsetof(Sm8750GpuLevel, acd) == 12, "Adreno830Level layout");
#else
_Static_assert(sizeof(Sm8750GpuLevel) == 16 && offsetof(Sm8750GpuLevel, acd) == 12, "Adreno830Level layout");
#endif

enum { Sm8750GpuBinHasSpeed = 1u << 0, Sm8750GpuBinHasSku = 1u << 1 };

/* One qcom,gpu-pwrlevels-N node. */
typedef struct {
    uint32_t flags;                            /* which of qcom,speed-bin / qcom,sku-codes the node has */
    uint32_t speed_bin;
    uint32_t sku[Sm8750GpuMaxSkus];
    uint8_t sku_count;
    uint8_t initial;                           /* qcom,initial-pwrlevel: DT index, 0 = fastest */
    uint8_t count;
    uint8_t reserved;
    const Sm8750GpuLevel *levels;              /* ascending: levels[0] is the slowest */
} Sm8750GpuBin;

typedef struct {
    Sm8750GpuLevel levels[Sm8750GpuMaxLevels]; /* ascending, no two with the same khz and vlvl */
    uint8_t count;
    uint8_t initial;                           /* index into levels; GMU level = index + 1 */
    int8_t bin;                                /* matched bin, -1 = none */
    uint8_t fused;                             /* 1: the matched bin's faster levels are appended */
} Sm8750GpuLevelTable;

/* KGSL adreno_of_get_pwrlevels: index of the first bin whose qcom,speed-bin equals speed_bin, or whose
 * qcom,sku-codes hold 0 or soc_code (a bin with both needs both); -1 if none. */
int Sm8750GpuBinMatch(const Sm8750GpuBin *bins, unsigned count, uint32_t speed_bin, uint32_t soc_code);

/* The ascending prefix on which every bin agrees in khz, vlvl and acd; bus votes are the lowest any bin
 * gives. initial is the slowest level any bin starts at. bin = -1, fused = 0. */
Sm8750SocInfoStatus Sm8750GpuLevelsCommon(const Sm8750GpuBin *bins, unsigned count, Sm8750GpuLevelTable *out);

/* The driver's table (docs/ARCHITECTURE.md 4.1.4): the common table, the matched bin only recorded; with
 * fused != 0 (pagani-gpu-speedbin=1) the matched bin's levels faster than the last common one are appended
 * (a repeated (khz, vlvl) level once). The common levels keep their bus votes and GMU numbering, and the
 * initial level stays the common one; without a matched bin the table is the common one. */
Sm8750SocInfoStatus Sm8750GpuLevelsSelect(const Sm8750GpuBin *bins, unsigned count, uint32_t speed_bin,
                                          uint32_t soc_code, int fused, Sm8750GpuLevelTable *out);

#ifdef __cplusplus
}
#endif
#endif
