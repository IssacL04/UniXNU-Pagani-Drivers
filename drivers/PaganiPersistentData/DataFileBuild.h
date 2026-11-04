// SPDX-License-Identifier: BSD-2-Clause-Patent
// Writer side of DataFileFormat.h, for the Android helper and the host tests (XNU only reads descriptors, and writes at most a
// REVOKED header): FIEMAP extents -> the segment table, and the slot image (header + table).
#pragma once
#include "DataFileFormat.h"

/* FIEMAP extent flags (linux/fiemap.h). f2fs sets MERGED on every extent it reports and DATA_ENCRYPTED on encrypted files. */
#define DF_FIEMAP_LAST 0x1u
#define DF_FIEMAP_DATA_ENCRYPTED 0x80u
#define DF_FIEMAP_MERGED 0x1000u
/* Anything else (UNKNOWN, DELALLOC, ENCODED, NOT_ALIGNED, DATA_INLINE, DATA_TAIL, UNWRITTEN, SHARED) means the blocks are not plain,
   allocated, fixed blocks of this file: refused. */
#define DF_FIEMAP_ALLOWED (DF_FIEMAP_LAST | DF_FIEMAP_DATA_ENCRYPTED | DF_FIEMAP_MERGED)

typedef struct {
  uint64_t logical, physical, length;   /* bytes, as struct fiemap_extent reports them (physical: from the start of the device) */
  uint32_t flags;
} df_fiemap_extent;

enum {
  DF_FM_OK = 0,
  DF_FM_FLAGS = 1,         /* an extent with a refused flag */
  DF_FM_GAP = 2,           /* the extents do not cover [0, size) contiguously */
  DF_FM_ALIGN = 3,         /* an extent is not whole f2fs segments, or not on segment boundaries */
  DF_FM_RANGE = 4,         /* outside the f2fs main area */
  DF_FM_TOO_MANY = 5,      /* more extents than a slot holds */
  DF_FM_SIZE = 6,          /* the file size is not whole segments */
  DF_FM_LAST = 7,          /* no extent carries FIEMAP_EXTENT_LAST, or one before the end does */
};

static inline const char *df_fiemap_error_name (int e)
{
  static const char *const names[] = {"ok", "flags", "gap", "align", "range", "too-many", "size", "last"};
  return e >= 0 && e < (int) (sizeof names / sizeof names[0]) ? names[e] : "?";
}

/* One merged run -> one segment extent (whole segments on main-area segment boundaries). */
static inline int df_fiemap_flush (uint64_t runL, uint64_t runP, uint64_t runN, uint64_t fs_main, uint64_t mainEnd, df_extent *out,
                                   uint32_t max_out, uint32_t *count)
{
  const uint64_t seg = (uint64_t) DF_SEG_BLOCKS * DF_BLOCK;
  if (runL % seg || runN % seg) return DF_FM_ALIGN;
  const uint64_t b = runP / DF_BLOCK;
  if (b < fs_main || b >= mainEnd) return DF_FM_RANGE;
  if ((b - fs_main) % DF_SEG_BLOCKS) return DF_FM_ALIGN;
  if (runN / DF_BLOCK > mainEnd - b) return DF_FM_RANGE;
  if (*count >= max_out) return DF_FM_TOO_MANY;
  out[*count].first_rel = b;
  out[*count].segments = (uint32_t) (runN / seg);
  out[*count].reserved = 0;
  ++*count;
  return DF_FM_OK;
}

/* Convert the file's FIEMAP extents (in logical order, as the kernel returns them) into segment extents in file order, merging
   neighbours that continue both logically and physically. fs_main/fs_blocks: the f2fs main_blkaddr and block_count (4096-byte blocks,
   relative to the device FIEMAP's physical offsets are relative to). Returns DF_FM_OK and *count, or an error with *bad = the input
   index that failed. */
static inline int df_from_fiemap (const df_fiemap_extent *in, uint32_t n, uint64_t size, uint64_t fs_main, uint64_t fs_blocks,
                                  df_extent *out, uint32_t max_out, uint32_t *count, uint32_t *bad)
{
  const uint64_t seg = (uint64_t) DF_SEG_BLOCKS * DF_BLOCK;
  *count = 0;
  *bad = 0;
  if (!size || size % seg) return DF_FM_SIZE;
  if (fs_main >= fs_blocks) return DF_FM_RANGE;
  const uint64_t mainEnd = fs_main + (fs_blocks - fs_main) / DF_SEG_BLOCKS * DF_SEG_BLOCKS;
  uint64_t next = 0;         /* next expected logical byte */
  uint64_t runL = 0, runP = 0, runN = 0;
  for (uint32_t i = 0; i < n; ++i) {
    *bad = i;
    const df_fiemap_extent *e = &in[i];
    if (e->flags & ~DF_FIEMAP_ALLOWED) return DF_FM_FLAGS;
    if (((e->flags & DF_FIEMAP_LAST) != 0) != (i + 1 == n)) return DF_FM_LAST;
    if (e->logical != next || !e->length) return DF_FM_GAP;
    if (e->length > size - e->logical) return DF_FM_GAP;
    if (e->physical % DF_BLOCK || e->length % DF_BLOCK) return DF_FM_ALIGN;
    next = e->logical + e->length;
    if (runN && runP + runN == e->physical) { runN += e->length; continue; }
    if (runN) {
      int rc = df_fiemap_flush (runL, runP, runN, fs_main, mainEnd, out, max_out, count);
      if (rc) return rc;
    }
    runL = e->logical; runP = e->physical; runN = e->length;
  }
  *bad = n;
  if (next != size) return DF_FM_GAP;
  return runN ? df_fiemap_flush (runL, runP, runN, fs_main, mainEnd, out, max_out, count) : DF_FM_GAP;
}

/* Fill the derived counts and the table hash of `h` from the extents, and write the slot image: header block, then the table
   (slot: DF_SLOT_BLOCKS * 4096 bytes, zeroed past the table). Returns the slot blocks to write (header + table blocks), 0 on error.
   A writer puts the table blocks on the medium first and the header block last. */
static inline uint32_t df_build_slot (df_header *h, const df_extent *ext, uint32_t n, uint8_t *slot, df_sha256_fn sha)
{
  if (!n || n > DF_MAX_EXTENTS) return 0;
  memset (slot, 0, (size_t) DF_SLOT_BLOCKS * DF_BLOCK);
  uint64_t segments = 0;
  uint8_t *t = slot + DF_BLOCK;
  for (uint32_t i = 0; i < n; ++i) {
    df_put64 (t + (size_t) i * DF_EXTENT_BYTES, ext[i].first_rel);
    df_put32 (t + (size_t) i * DF_EXTENT_BYTES + 8, ext[i].segments);
    df_put32 (t + (size_t) i * DF_EXTENT_BYTES + 12, 0);
    segments += ext[i].segments;
  }
  h->version = DF_VERSION;
  h->flags = DF_FLAG_CANARY;
  h->lun = 0;
  h->block_bytes = DF_BLOCK;
  h->seg_blocks = DF_SEG_BLOCKS;
  h->extent_count = n;
  h->segment_count = segments;
  h->data_blocks = segments * DF_SEG_DATA;
  h->file_bytes = segments * DF_SEG_BLOCKS * (uint64_t) DF_BLOCK;
  sha (t, (size_t) n * DF_EXTENT_BYTES, h->table_sha);
  df_header_encode (h, slot, sha);
  return 1 + (uint32_t) (((uint64_t) n * DF_EXTENT_BYTES + DF_BLOCK - 1) / DF_BLOCK);
}

/* Partition-relative block 0 (the canary) of the file's segment `index` (file order), or ~0 past the end. */
static inline uint64_t df_segment_rel (const df_extent *ext, uint32_t n, uint64_t index)
{
  for (uint32_t i = 0; i < n; ++i) {
    if (index < ext[i].segments) return ext[i].first_rel + index * DF_SEG_BLOCKS;
    index -= ext[i].segments;
  }
  return ~0ULL;
}

/* Partition-relative block of file data block `w` (window numbering without a head: segment w / 511, block 1 + w % 511), or ~0. */
static inline uint64_t df_data_rel (const df_extent *ext, uint32_t n, uint64_t w)
{
  const uint64_t s = df_segment_rel (ext, n, w / DF_SEG_DATA);
  return s == ~0ULL ? s : s + 1 + w % DF_SEG_DATA;
}
