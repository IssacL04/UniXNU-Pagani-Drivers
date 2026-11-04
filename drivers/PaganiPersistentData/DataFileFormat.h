// SPDX-License-Identifier: BSD-2-Clause-Patent
// The descriptor of the iOS Data file: a pinned, fully allocated f2fs file in Android's /data whose UFS blocks XNU reads and writes
// directly (never through f2fs, never through Android's encryption). Shared by the Android helper (writes it), the XNU store
// (validates it before any use) and the host tests. C and C++, freestanding: no allocation, the SHA-256 is passed in.
//
// Where it lives: two slots in the unixnu partition (LUN 0), in the zero padding between the root manifest (block 0) and the first
// root window (1 MiB, scripts/unixnu/userdata.py): slot A at unixnu blocks 16..135, slot B at 136..255 (4096-byte blocks). Nothing
// reads those blocks today (RootLayout.h hashes 4064 bytes of block 0; the windows start at 0x100000). Re-flashing the root image
// with split_sparse.py writes zeros there (zero blocks are FILL chunks): the descriptor is gone and XNU refuses the file until Android
// publishes it again (after re-verifying everything). A writer always replaces the slot that does not hold the newest valid descriptor
// and uses generation = newest + 1, so a torn write leaves the previous one in force.
//
// Slot layout (little endian):
//   block 0             header (offsets below), SHA-256 of bytes [0, 4064) at 4064
//   blocks 1..          extent table, extent_count entries of 16 bytes: { u64 first_rel, u32 segments, u32 reserved = 0 } in FILE
//                       order (FIEMAP order). first_rel is the partition-relative 4096-byte block of the extent's first f2fs segment;
//                       an extent is `segments` consecutive whole segments of 512 blocks.
// Every segment of the file: block 0 is a canary (df_canary_block), blocks 1..511 are data. The Data window is the data blocks of the
// segments in file order: window block w is segment w / 511, block 1 + w % 511.
//
// Header block:
//     0  "UNXDFIL1"           8  u32 version = 1     12  u32 state (1 ACTIVE, 2 READONLY, 3 REVOKED)
//    16  u64 generation      24  u32 flags = 1 (canaries)                  28  u32 extent_count
//    32  file_id[16]         48  nonce[32] (random per file; seeds the canaries)
//    80  u32 lun = 0         84  u32 block_bytes = 4096                     88  u32 seg_blocks = 512     92  u32 0
//    96  u64 part_first     104  u64 part_last     (userdata: absolute LBAs on the LUN, as the GPT had them at publication)
//   112  u64 fs_main        120  u64 fs_blocks     (f2fs main_blkaddr and block_count, partition-relative blocks)
//   128  u64 segment_count  136  u64 data_blocks   144  u64 file_bytes     152  u64 file_ino     160  u64 created (unix time)
//   168  table_sha[32]      200..4063 zero         4064  SHA-256 of [0, 4064)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DF_BLOCK 4096u
#define DF_SEG_BLOCKS 512u
#define DF_SEG_DATA (DF_SEG_BLOCKS - 1u)
#define DF_SLOT_A 16u
#define DF_SLOT_B 136u
#define DF_SLOT_BLOCKS 120u
#define DF_SLOTS_END (DF_SLOT_B + DF_SLOT_BLOCKS)   /* first unixnu block past the slots: 256 (1 MiB), where the root windows begin */
#define DF_HASHED 4064u
#define DF_EXTENT_BYTES 16u
#define DF_MAX_EXTENTS ((DF_SLOT_BLOCKS - 1u) * DF_BLOCK / DF_EXTENT_BYTES)   /* 30464 */
#define DF_STATE_ACTIVE 1u
#define DF_STATE_READONLY 2u                    /* XNU may read and verify, never write */
#define DF_STATE_REVOKED 3u                     /* the file is gone or suspect: refuse it */
#define DF_FLAG_CANARY 1u                       /* block 0 of every segment is a canary (required in version 1) */
#define DF_VERSION 1u

enum {
  DF_OK = 0,
  DF_ERR_MAGIC = 1,            /* blank or foreign block */
  DF_ERR_VERSION = 2,
  DF_ERR_HASH = 3,
  DF_ERR_STATE = 4,
  DF_ERR_GEOMETRY = 5,         /* LUN, block or segment size, partition or f2fs bounds */
  DF_ERR_COUNTS = 6,           /* segment, data, byte or extent counts disagree */
  DF_ERR_PADDING = 7,          /* nonzero reserved bytes */
  DF_ERR_TABLE_HASH = 8,
  DF_ERR_EXTENT_ALIGN = 9,     /* an extent does not start on an f2fs segment boundary */
  DF_ERR_EXTENT_RANGE = 10,    /* an extent leaves the f2fs main area */
  DF_ERR_EXTENT_SUM = 11,
  DF_ERR_OVERLAP = 12,
  DF_ERR_PARTITION = 13,       /* the live GPT's userdata differs from the descriptor's */
  DF_ERR_REVOKED = 14,
  DF_ERR_AMBIGUOUS = 15,       /* two different valid descriptors with one generation */
  DF_ERR_CAPACITY = 16,        /* the window asked for does not fit */
  DF_ERR_IO = 17,
  DF_ERR_CANARY = 18,          /* a segment's canary is not the expected block: Android reused or discarded it */
  DF_ERR_ARG = 19,
  DF_ERR_READONLY = 20,
  DF_ERR_VERIFY = 21,          /* a block read back after its write differs from what was written */
  DF_ERR_GATE = 22,            /* a write was about to leave the file's data blocks (never reaches the medium) */
  DF_ERR_LAYOUT = 23,          /* the unixnu layout leaves no room for the slots, or the head would overlap the root image */
};

typedef void (*df_sha256_fn) (const void *data, size_t bytes, uint8_t out[32]);

typedef struct {
  uint64_t first_rel;
  uint32_t segments;
  uint32_t reserved;
} df_extent;

typedef struct {
  uint32_t version, state, flags, extent_count;
  uint64_t generation;
  uint8_t file_id[16], nonce[32];
  uint32_t lun, block_bytes, seg_blocks;
  uint64_t part_first, part_last;              /* userdata, absolute 4096-byte LBAs on the LUN (GPT first/last) */
  uint64_t fs_main, fs_blocks;                 /* f2fs main_blkaddr and block_count, partition-relative blocks */
  uint64_t segment_count, data_blocks, file_bytes;
  uint64_t file_ino, created;                  /* informational */
  uint8_t table_sha[32];
} df_header;

static inline uint32_t df_get32 (const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static inline uint64_t df_get64 (const uint8_t *p) { return (uint64_t) df_get32 (p) | (uint64_t) df_get32 (p + 4) << 32; }
static inline void df_put32 (uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8 * i)); }
static inline void df_put64 (uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t) (v >> (8 * i)); }

static inline const char *df_error_name (int e)
{
  static const char *const names[] = {"ok", "magic", "version", "hash", "state", "geometry", "counts", "padding", "table-hash",
                                      "extent-align", "extent-range", "extent-sum", "overlap", "partition", "revoked", "ambiguous",
                                      "capacity", "io", "canary", "argument", "read-only", "verify", "gate", "layout"};
  return e >= 0 && e < (int) (sizeof names / sizeof names[0]) ? names[e] : "?";
}

/* Whole f2fs segments of the main area: segment k (k >= 0) is partition blocks [fs_main + 512k, fs_main + 512k + 512). */
static inline uint64_t df_main_segments (const df_header *h) { return (h->fs_blocks - h->fs_main) / DF_SEG_BLOCKS; }

/* NativeRamBlock's store window for a disk of `pages` 4096-byte pages (PersistOverlay.h windowBlocks: header, presence bitmap with
   one bit per page, the pages), and the largest disk a store of `capacity` window blocks holds: what the Data seed may be at most. */
static inline uint64_t df_window_blocks (uint64_t pages) { return 1 + (pages + 32767) / 32768 + pages; }
static inline uint64_t df_max_disk_pages (uint64_t capacity)
{
  if (capacity < 3) return 0;
  uint64_t p = capacity - 1 - (capacity + 32767) / 32768;   /* never above the answer: ceil(p / 32768) <= ceil(capacity / 32768) */
  while (df_window_blocks (p + 1) <= capacity) ++p;
  while (p && df_window_blocks (p) > capacity) --p;
  return p;
}

static inline void df_header_encode (const df_header *h, uint8_t block[DF_BLOCK], df_sha256_fn sha)
{
  memset (block, 0, DF_BLOCK);
  memcpy (block, "UNXDFIL1", 8);
  df_put32 (block + 8, h->version);
  df_put32 (block + 12, h->state);
  df_put64 (block + 16, h->generation);
  df_put32 (block + 24, h->flags);
  df_put32 (block + 28, h->extent_count);
  memcpy (block + 32, h->file_id, 16);
  memcpy (block + 48, h->nonce, 32);
  df_put32 (block + 80, h->lun);
  df_put32 (block + 84, h->block_bytes);
  df_put32 (block + 88, h->seg_blocks);
  df_put64 (block + 96, h->part_first);
  df_put64 (block + 104, h->part_last);
  df_put64 (block + 112, h->fs_main);
  df_put64 (block + 120, h->fs_blocks);
  df_put64 (block + 128, h->segment_count);
  df_put64 (block + 136, h->data_blocks);
  df_put64 (block + 144, h->file_bytes);
  df_put64 (block + 152, h->file_ino);
  df_put64 (block + 160, h->created);
  memcpy (block + 168, h->table_sha, 32);
  sha (block, DF_HASHED, block + DF_HASHED);
}

/* Decode and check one header block on its own. A REVOKED header is DF_OK here (its fields beyond the identity are not checked); the
   caller refuses it. */
static inline int df_header_decode (const uint8_t block[DF_BLOCK], df_header *h, df_sha256_fn sha)
{
  uint8_t digest[32];
  if (memcmp (block, "UNXDFIL1", 8)) return DF_ERR_MAGIC;
  if (df_get32 (block + 8) != DF_VERSION) return DF_ERR_VERSION;
  sha (block, DF_HASHED, digest);
  if (memcmp (digest, block + DF_HASHED, 32)) return DF_ERR_HASH;
  for (unsigned i = 200; i < DF_HASHED; ++i) if (block[i]) return DF_ERR_PADDING;
  if (df_get32 (block + 92)) return DF_ERR_PADDING;
  memset (h, 0, sizeof *h);
  h->version = DF_VERSION;
  h->state = df_get32 (block + 12);
  h->generation = df_get64 (block + 16);
  h->flags = df_get32 (block + 24);
  h->extent_count = df_get32 (block + 28);
  memcpy (h->file_id, block + 32, 16);
  memcpy (h->nonce, block + 48, 32);
  h->lun = df_get32 (block + 80);
  h->block_bytes = df_get32 (block + 84);
  h->seg_blocks = df_get32 (block + 88);
  h->part_first = df_get64 (block + 96);
  h->part_last = df_get64 (block + 104);
  h->fs_main = df_get64 (block + 112);
  h->fs_blocks = df_get64 (block + 120);
  h->segment_count = df_get64 (block + 128);
  h->data_blocks = df_get64 (block + 136);
  h->file_bytes = df_get64 (block + 144);
  h->file_ino = df_get64 (block + 152);
  h->created = df_get64 (block + 160);
  memcpy (h->table_sha, block + 168, 32);
  if (h->state != DF_STATE_ACTIVE && h->state != DF_STATE_READONLY && h->state != DF_STATE_REVOKED) return DF_ERR_STATE;
  if (h->state == DF_STATE_REVOKED) return DF_OK;
  if (h->flags != DF_FLAG_CANARY || h->lun != 0 || h->block_bytes != DF_BLOCK || h->seg_blocks != DF_SEG_BLOCKS) return DF_ERR_GEOMETRY;
  if (h->part_last < h->part_first || h->part_last - h->part_first + 1 < h->fs_blocks || !h->fs_main ||
      h->fs_main >= h->fs_blocks || h->fs_blocks - h->fs_main < DF_SEG_BLOCKS)
    return DF_ERR_GEOMETRY;
  if (!h->extent_count || h->extent_count > DF_MAX_EXTENTS || !h->segment_count || h->segment_count > df_main_segments (h) ||
      h->data_blocks != h->segment_count * DF_SEG_DATA || h->file_bytes != h->segment_count * DF_SEG_BLOCKS * (uint64_t) DF_BLOCK)
    return DF_ERR_COUNTS;
  return DF_OK;
}

/* The descriptor against the live partition table: the userdata entry the GPT has now (absolute first/last LBA on LUN 0). */
static inline int df_check_partition (const df_header *h, uint64_t gpt_first, uint64_t gpt_last)
{
  return h->part_first == gpt_first && h->part_last == gpt_last ? DF_OK : DF_ERR_PARTITION;
}

/* In-place heap sort of an index array by extent start (no recursion, no allocation: kernel safe). */
static inline void df_sort_by_start (const df_extent *e, uint32_t *idx, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) idx[i] = i;
  if (n < 2) return;
  uint32_t start = n / 2, end = n;
  for (;;) {
    uint32_t root;
    if (start > 0) {
      root = --start;
    } else {
      if (--end == 0) break;
      uint32_t t = idx[0]; idx[0] = idx[end]; idx[end] = t;
      root = 0;
    }
    for (;;) {
      uint32_t child = 2 * root + 1;
      if (child >= end) break;
      if (child + 1 < end && e[idx[child + 1]].first_rel > e[idx[child]].first_rel) ++child;
      if (e[idx[root]].first_rel >= e[idx[child]].first_rel) break;
      uint32_t t = idx[root]; idx[root] = idx[child]; idx[child] = t;
      root = child;
    }
  }
}

/* Parse and check the extent table (count entries in `table`, file order) against a decoded ACTIVE/READONLY header: its hash, every
   extent on whole main-area segments, the segment sum, and no two extents sharing a block. out: count entries; idx: count uint32
   scratch, left sorted by physical start. */
static inline int df_table_parse (const df_header *h, const uint8_t *table, df_extent *out, uint32_t *idx, df_sha256_fn sha)
{
  uint8_t digest[32];
  const uint32_t n = h->extent_count;
  if (!n || n > DF_MAX_EXTENTS || !out || !idx) return DF_ERR_ARG;
  sha (table, (size_t) n * DF_EXTENT_BYTES, digest);
  if (memcmp (digest, h->table_sha, 32)) return DF_ERR_TABLE_HASH;
  const uint64_t mainEnd = h->fs_main + df_main_segments (h) * DF_SEG_BLOCKS;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t *p = table + (size_t) i * DF_EXTENT_BYTES;
    out[i].first_rel = df_get64 (p);
    out[i].segments = df_get32 (p + 8);
    out[i].reserved = df_get32 (p + 12);
    if (out[i].reserved) return DF_ERR_PADDING;
    if (!out[i].segments) return DF_ERR_EXTENT_RANGE;
    if (out[i].first_rel < h->fs_main || out[i].first_rel >= mainEnd) return DF_ERR_EXTENT_RANGE;
    if ((out[i].first_rel - h->fs_main) % DF_SEG_BLOCKS) return DF_ERR_EXTENT_ALIGN;
    if ((uint64_t) out[i].segments > (mainEnd - out[i].first_rel) / DF_SEG_BLOCKS) return DF_ERR_EXTENT_RANGE;
    sum += out[i].segments;
  }
  if (sum != h->segment_count) return DF_ERR_EXTENT_SUM;
  df_sort_by_start (out, idx, n);
  for (uint32_t i = 1; i < n; ++i) {
    const df_extent *a = &out[idx[i - 1]], *b = &out[idx[i]];
    if (a->first_rel + (uint64_t) a->segments * DF_SEG_BLOCKS > b->first_rel) return DF_ERR_OVERLAP;
  }
  return DF_OK;
}

/* Choose between the two slots: the valid one with the higher generation. rcA/rcB are df_header_decode results. Returns 1 (A),
   2 (B) or 0 with *err set (DF_ERR_MAGIC when both are blank, else the more telling error). A newest REVOKED header wins and makes
   the whole descriptor refused (DF_ERR_REVOKED). Equal generations must carry the identical header. */
static inline int df_pick_slot (const df_header *a, int rcA, const uint8_t blockA[DF_BLOCK], const df_header *b, int rcB,
                                const uint8_t blockB[DF_BLOCK], int *err)
{
  int pick = 0;
  if (rcA == DF_OK && rcB == DF_OK) {
    if (a->generation == b->generation) {
      if (memcmp (blockA, blockB, DF_BLOCK)) { *err = DF_ERR_AMBIGUOUS; return 0; }
      pick = 1;
    } else {
      pick = a->generation > b->generation ? 1 : 2;
    }
  } else if (rcA == DF_OK) {
    pick = 1;
  } else if (rcB == DF_OK) {
    pick = 2;
  } else {
    *err = rcA == DF_ERR_MAGIC ? rcB : rcA;
    return 0;
  }
  const df_header *h = pick == 1 ? a : b;
  if (h->state == DF_STATE_REVOKED) { *err = DF_ERR_REVOKED; return 0; }
  *err = DF_OK;
  return pick;
}

/* A REVOKED header (one block) that supersedes `previous` (its file_id is kept for the record): generation must exceed every
   generation on the medium. Written by the Android helper (revoke/remove/guard) and by XNU when it finds a canary gone. */
static inline void df_build_revoked (const df_header *previous, uint64_t generation, uint8_t block[DF_BLOCK], df_sha256_fn sha)
{
  df_header r;
  memset (&r, 0, sizeof r);
  r.version = DF_VERSION;
  r.state = DF_STATE_REVOKED;
  r.generation = generation;
  if (previous) memcpy (r.file_id, previous->file_id, 16);
  df_header_encode (&r, block, sha);
}

/* Deterministic filler of a 4096-byte block from an 80-byte head: bytes 0..47 identify the block, 48..79 are a SHA-256 over the
   file's nonce and that identity, the rest repeats the digest with a position byte mixed in. */
static inline void df_fill_block (const df_header *h, const char tag[8], uint64_t a, uint64_t b, uint8_t out[DF_BLOCK], df_sha256_fn sha)
{
  uint8_t in[80];
  memset (out, 0, DF_BLOCK);
  memcpy (out, tag, 8);
  df_put32 (out + 8, DF_VERSION);
  memcpy (out + 16, h->file_id, 16);
  df_put64 (out + 32, a);
  df_put64 (out + 40, b);
  memcpy (in, h->nonce, 32);
  memcpy (in + 32, out, 48);
  sha (in, sizeof in, out + 48);
  for (unsigned i = 80; i < DF_BLOCK; ++i) out[i] = (uint8_t) (out[48 + (i - 80) % 32] ^ (uint8_t) ((i - 80) / 32));
}

/* The expected content of block 0 of the file's segment `index` (file order) at partition-relative block lba_rel. Android writes
   it once, raw, after it proved the mapping; XNU compares the whole block before it touches the segment. Anything Android writes
   there later (ciphertext) or a discard (zeros) makes it differ. */
static inline void df_canary_block (const df_header *h, uint64_t index, uint64_t lba_rel, uint8_t out[DF_BLOCK], df_sha256_fn sha)
{
  df_fill_block (h, "UNXCANRY", index, lba_rel, out, sha);
}

/* The block XNU's write self-test leaves at store block `rel` (window numbering, past the window NativeRamBlock uses) on
   partition-relative block lba_rel; the Android helper's selftest-check reads it back raw: proof that XNU wrote exactly there. */
static inline void df_selftest_block (const df_header *h, uint64_t rel, uint64_t lba_rel, uint8_t out[DF_BLOCK], df_sha256_fn sha)
{
  df_fill_block (h, "UNXSELFT", rel, lba_rel, out, sha);
}
