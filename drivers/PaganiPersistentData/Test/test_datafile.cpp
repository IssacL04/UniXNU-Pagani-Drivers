// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the iOS Data file design: descriptor format, slot choice, FIEMAP conversion, window -> LBA map, the write gate,
// canaries, read-back verification, the write self-test, and an end-to-end persistent Data disk (NativeRamBlock's
// PersistOverlay/DirectDisk/SeedDisk, unchanged) on a simulated LUN 0 with the PKX110 geometry. Nothing here touches a device.
//   c++ -std=c++17 -O1 -g -fsanitize=address,undefined -I.. -I<NativeRamBlock> test_datafile.cpp && ./a.out [sim-extents.txt]
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "DfSha256.h"
#include "DataFileFormat.h"
#include "DataFileBuild.h"
#include "DataFileStore.h"
#include "DirectDisk.h"     // NativeRamBlock (read-only use): SparseDisk.h, SeedDisk.h, PersistOverlay.h

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// PKX110 LUN 0 after the dual-system change: GPT of UniXNU/out/dual_sys/applied2/gpt-primary.after (userdata 4020972..107409151,
// unixnu 107409152..124186367, backup header at 124186623); f2fs superblock of the 10-29 metadata dump (main_blkaddr 297984,
// block_count 103387919, one 512-block segment per section).
static const uint64_t kUserFirst = 4020972, kUserLast = 107409151;
static const uint64_t kUnixnuFirst = 107409152, kUnixnuLast = 124186367;
static const uint64_t kFsMain = 297984, kFsBlocks = 103387919;
static const uint64_t kLunLast = 124186623;

// ---- simulated LUN 0: sparse; never-written blocks read as stale "ciphertext" -------------------------------------------------------
struct Lun {
  std::unordered_map<uint64_t, std::array<uint8_t, DF_BLOCK>> blocks;
  std::vector<std::pair<uint64_t, uint32_t>> writes;     // every WRITE that reached the medium
  uint64_t readCount = 0, writeCount = 0, failReadAt = ~0ULL, failWriteAt = ~0ULL;
  uint64_t corruptWriteAt = ~0ULL, misdirectWriteAt = ~0ULL;   // silent faults: one byte flipped / written one block off
  static void stale (uint64_t lba, uint8_t *out) { std::mt19937_64 r (lba * 0x9E3779B97F4A7C15ULL + 1); for (unsigned i = 0; i < DF_BLOCK; i += 8) { uint64_t v = r (); memcpy (out + i, &v, 8); } }
  bool read (uint64_t lba, uint32_t n, void *d) {
    if (readCount++ == failReadAt || !n || lba + n - 1 > kLunLast) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = blocks.find (lba + i);
      if (it == blocks.end ()) stale (lba + i, (uint8_t *) d + (uint64_t) i * DF_BLOCK); else memcpy ((uint8_t *) d + (uint64_t) i * DF_BLOCK, it->second.data (), DF_BLOCK);
    }
    return true;
  }
  bool write (uint64_t lba, uint32_t n, const void *s) {
    const uint64_t index = writeCount++;
    if (index == failWriteAt || !n || lba + n - 1 > kLunLast) return false;
    if (index == misdirectWriteAt) lba += 1;
    writes.push_back ({lba, n});
    for (uint32_t i = 0; i < n; ++i) memcpy (blocks[lba + i].data (), (const uint8_t *) s + (uint64_t) i * DF_BLOCK, DF_BLOCK);
    if (index == corruptWriteAt) blocks[lba + n - 1][77] ^= 0x20;
    return true;
  }
};

// NativeUfs stand-in: hostRead4k/hostWrite4k with the gate hostInWindow -> DfStore::writeAllowed; at most 32 blocks per command.
struct MockIo;
using Store = DfStore<MockIo>;
struct MockIo {
  Lun *lun = nullptr;
  Store *gate = nullptr;
  uint64_t refused = 0, syncs = 0;
  bool read4k (uint64_t lba, uint32_t n, void *d) { return n && n <= kDfMaxRun && lun->read (lba, n, d); }
  bool write4k (uint64_t lba, uint32_t n, const void *s) {
    if (!n || n > kDfMaxRun || !gate || !gate->writeAllowed (lba, n)) { ++refused; return false; }
    return lun->write (lba, n, s);
  }
  bool sync () { ++syncs; return true; }
};

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
static df_header baseHeader (uint64_t generation = 1) {
  df_header h;
  memset (&h, 0, sizeof h);
  h.state = DF_STATE_ACTIVE;
  h.generation = generation;
  for (int i = 0; i < 16; ++i) h.file_id[i] = (uint8_t) (0x40 + i);
  for (int i = 0; i < 32; ++i) h.nonce[i] = (uint8_t) (0x90 ^ i);
  h.part_first = kUserFirst;
  h.part_last = kUserLast;
  h.fs_main = kFsMain;
  h.fs_blocks = kFsBlocks;
  h.file_ino = 12345;
  h.created = 1793000000;
  return h;
}
static std::vector<uint8_t> slotOf (df_header &h, const std::vector<df_extent> &e) {
  std::vector<uint8_t> slot ((size_t) DF_SLOT_BLOCKS * DF_BLOCK);
  CHECK (df_build_slot (&h, e.data (), (uint32_t) e.size (), slot.data (), df_sha256) > 0);
  return slot;
}
static void rehash (uint8_t *block) { df_sha256 (block, DF_HASHED, block + DF_HASHED); }
static uint64_t seg (uint64_t k) { return kFsMain + k * DF_SEG_BLOCKS; }   // partition-relative block of main-area segment k
// Publish a descriptor the way the Android helper does: canaries raw into the file's segments, slot into unixnu (table first).
static void publish (Lun &lun, df_header &h, const std::vector<df_extent> &e, int slot = 0, bool canaries = true) {
  auto image = slotOf (h, e);
  uint32_t blocks = 1 + (uint32_t) (((uint64_t) e.size () * DF_EXTENT_BYTES + DF_BLOCK - 1) / DF_BLOCK);
  uint64_t first = kUnixnuFirst + (slot ? DF_SLOT_B : DF_SLOT_A);
  for (uint32_t b = 1; b < blocks; ++b) lun.write (first + b, 1, &image[(size_t) b * DF_BLOCK]);
  lun.write (first, 1, image.data ());
  if (canaries) {
    uint8_t c[DF_BLOCK];
    uint64_t index = 0;
    for (auto &x : e)
      for (uint32_t s = 0; s < x.segments; ++s, ++index) {
        df_canary_block (&h, index, x.first_rel + (uint64_t) s * DF_SEG_BLOCKS, c, df_sha256);
        lun.write (kUserFirst + x.first_rel + (uint64_t) s * DF_SEG_BLOCKS, 1, c);
      }
  }
  lun.writes.clear ();
}
struct Opened {
  MockIo io;
  Store store;
  std::vector<uint8_t> slot = std::vector<uint8_t> ((size_t) DF_SLOT_BLOCKS * DF_BLOCK), memory;
  df_header h = {};
  int load = -1, rc = -1, pick = 0;
  uint64_t newest = 0;
};
static void openStore (Opened &o, Lun &lun, uint64_t window, unsigned mode, bool eager, uint64_t head = 0, uint64_t headBlocks = 0,
                       uint64_t gptFirst = kUserFirst, uint64_t gptLast = kUserLast, bool verify = false) {
  o.io.lun = &lun;
  o.io.gate = &o.store;
  o.load = dfLoadDescriptor (o.io, kUnixnuFirst, o.slot.data (), &o.h, df_sha256, &o.pick, &o.newest);
  if (o.load != DF_OK) { o.rc = o.load; return; }
  o.memory.assign (dfStoreMemoryBytes (o.h.extent_count, o.h.segment_count), 0);
  o.rc = o.store.open (o.io, o.h, o.slot.data () + DF_BLOCK, o.memory.data (), o.memory.size (), gptFirst, gptLast, head, headBlocks,
                       window, mode, eager, df_sha256, nullptr, verify);
}
// Reference set of every block the store may write: data blocks of the file's segments (and the head).
static bool refAllowed (const std::vector<df_extent> &e, uint64_t lba, uint32_t n, uint64_t head, uint64_t headBlocks) {
  if (!n) return false;
  if (headBlocks && lba >= head && lba + n <= head + headBlocks) return true;
  for (auto &x : e)
    for (uint32_t s = 0; s < x.segments; ++s) {
      uint64_t a = kUserFirst + x.first_rel + (uint64_t) s * DF_SEG_BLOCKS;
      if (lba > a && lba + n <= a + DF_SEG_BLOCKS) return true;
    }
  return false;
}

// ---- tests -----------------------------------------------------------------------------------------------------------------------
static void testSha () {
  uint8_t d[32];
  df_sha256 ("abc", 3, d);
  static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  CHECK (!memcmp (d, abc, 32));
  std::string m (1000003, 'a');
  df_sha256 (m.data (), m.size (), d);   // compared with Python hashlib by run.sh
  printf ("sha256(a*1000003)=");
  for (int i = 0; i < 32; ++i) printf ("%02x", d[i]);
  printf ("\n");
}

// df_window_blocks is PersistOverlay's windowBlocks, and df_max_disk_pages the largest disk a store of that many blocks holds.
static void testDiskSizing () {
  for (uint64_t p : {1ULL, 2ULL, 32767ULL, 32768ULL, 32769ULL, 524288ULL, 33487873ULL, 33488896ULL, 50331648ULL}) CHECK (df_window_blocks (p) == persist::windowBlocks (p));
  std::mt19937_64 r (2);
  for (int i = 0; i < 20000; ++i) {
    uint64_t cap = 3 + r () % (1ULL << (r () % 2 ? 16 : 36));
    uint64_t p = df_max_disk_pages (cap);
    if (!(p && persist::windowBlocks (p) <= cap && persist::windowBlocks (p + 1) > cap)) { CHECK (false); break; }
  }
  CHECK (df_max_disk_pages (2) == 0 && df_max_disk_pages (3) == 1);
  // A 128 GiB file: 65536 segments of 511 data blocks: the largest Data disk is 127.74 GiB (its seed must not be larger).
  const uint64_t cap = 65536ULL * DF_SEG_DATA;
  printf ("128 GiB file: %llu data blocks, largest disk %llu pages (%.3f GiB)\n", (unsigned long long) cap,
          (unsigned long long) df_max_disk_pages (cap), df_max_disk_pages (cap) * 4096.0 / (1ULL << 30));
  CHECK (df_max_disk_pages (cap) == cap - 1 - 1022);
}

static void testHeader () {
  std::vector<df_extent> e = {{seg (100), 3, 0}, {seg (10), 2, 0}, {seg (5000), 1, 0}};
  df_header h = baseHeader ();
  auto slot = slotOf (h, e);
  df_header d;
  CHECK (df_header_decode (slot.data (), &d, df_sha256) == DF_OK);
  CHECK (d.segment_count == 6 && d.data_blocks == 6 * 511 && d.file_bytes == 6ULL * 512 * 4096 && d.extent_count == 3);
  CHECK (d.part_first == kUserFirst && d.fs_main == kFsMain && !memcmp (d.nonce, h.nonce, 32));
  std::vector<df_extent> out (3);
  std::vector<uint32_t> idx (3);
  CHECK (df_table_parse (&d, slot.data () + DF_BLOCK, out.data (), idx.data (), df_sha256) == DF_OK);
  CHECK (out[0].first_rel == seg (100) && out[1].segments == 2 && idx[0] == 1 && idx[1] == 0 && idx[2] == 2);
  CHECK (df_check_partition (&d, kUserFirst, kUserLast) == DF_OK && df_check_partition (&d, kUserFirst, kUserLast + 1) == DF_ERR_PARTITION);
  // Header rejections: each mutation re-hashed, so the field check (not the hash) must catch it.
  struct M { unsigned off; uint64_t v; unsigned w; int want; } muts[] = {
    {8, 2, 4, DF_ERR_VERSION}, {12, 0, 4, DF_ERR_STATE}, {12, 7, 4, DF_ERR_STATE}, {24, 0, 4, DF_ERR_GEOMETRY}, {80, 1, 4, DF_ERR_GEOMETRY},
    {84, 512, 4, DF_ERR_GEOMETRY}, {88, 1024, 4, DF_ERR_GEOMETRY}, {92, 1, 4, DF_ERR_PADDING}, {104, kUserFirst - 1, 8, DF_ERR_GEOMETRY},
    {120, kUserLast - kUserFirst + 2, 8, DF_ERR_GEOMETRY}, {112, 0, 8, DF_ERR_GEOMETRY}, {112, kFsBlocks, 8, DF_ERR_GEOMETRY},
    {28, 0, 4, DF_ERR_COUNTS}, {28, DF_MAX_EXTENTS + 1, 4, DF_ERR_COUNTS}, {128, 0, 8, DF_ERR_COUNTS}, {136, 6 * 511 + 1, 8, DF_ERR_COUNTS},
    {144, 6ULL * 512 * 4096 - 4096, 8, DF_ERR_COUNTS}, {128, (kFsBlocks - kFsMain) / 512 + 1, 8, DF_ERR_COUNTS}, {300, 1, 1, DF_ERR_PADDING}};
  for (auto &m : muts) {
    auto b = slot;
    if (m.w == 1) b[m.off] = (uint8_t) m.v; else if (m.w == 4) df_put32 (&b[m.off], (uint32_t) m.v); else df_put64 (&b[m.off], m.v);
    rehash (b.data ());
    int rc = df_header_decode (b.data (), &d, df_sha256);
    if (rc != m.want) fprintf (stderr, "  mutation at %u: got %s want %s\n", m.off, df_error_name (rc), df_error_name (m.want));
    CHECK (rc == m.want);
  }
  { auto b = slot; b[0] = 'X'; CHECK (df_header_decode (b.data (), &d, df_sha256) == DF_ERR_MAGIC); }
  { auto b = slot; b[100] ^= 1; CHECK (df_header_decode (b.data (), &d, df_sha256) == DF_ERR_HASH); }
  { std::vector<uint8_t> zero (DF_BLOCK, 0); CHECK (df_header_decode (zero.data (), &d, df_sha256) == DF_ERR_MAGIC); }
}

static int parseWith (std::vector<df_extent> e, int tweak = 0) {
  df_header h = baseHeader ();
  auto slot = slotOf (h, e);
  if (tweak == 1) slot[DF_BLOCK + 12] = 1;                       // reserved word of extent 0 (hash then fails first)
  df_header d;
  if (df_header_decode (slot.data (), &d, df_sha256) != DF_OK) return -1;
  std::vector<df_extent> out (e.size ());
  std::vector<uint32_t> idx (e.size ());
  return df_table_parse (&d, slot.data () + DF_BLOCK, out.data (), idx.data (), df_sha256);
}

static void testTable () {
  const uint64_t mainSegs = (kFsBlocks - kFsMain) / 512;
  CHECK (parseWith ({{seg (0), 1, 0}, {seg (mainSegs - 1), 1, 0}}) == DF_OK);
  CHECK (parseWith ({{seg (3) + 1, 1, 0}}) == DF_ERR_EXTENT_ALIGN);
  CHECK (parseWith ({{kFsMain - 512, 1, 0}}) == DF_ERR_EXTENT_RANGE);
  CHECK (parseWith ({{seg (mainSegs), 1, 0}}) == DF_ERR_EXTENT_RANGE);
  CHECK (parseWith ({{seg (mainSegs - 2), 3, 0}}) == DF_ERR_EXTENT_RANGE);
  CHECK (parseWith ({{seg (7), 4, 0}, {seg (10), 1, 0}}) == DF_ERR_OVERLAP);       // 7..10 and 10
  CHECK (parseWith ({{seg (7), 1, 0}, {seg (7), 1, 0}}) == DF_ERR_OVERLAP);
  CHECK (parseWith ({{seg (7), 3, 0}, {seg (10), 1, 0}}) == DF_OK);                // touching is fine
  CHECK (parseWith ({{seg (7), 1, 0}}, 1) == DF_ERR_TABLE_HASH);
  // Reserved word with a matching hash, and a wrong segment sum, through hand-built tables.
  {
    df_header h = baseHeader ();
    std::vector<df_extent> e = {{seg (1), 2, 0}};
    auto slot = slotOf (h, e);
    df_put32 (&slot[DF_BLOCK + 12], 9);
    df_sha256 (&slot[DF_BLOCK], 16, h.table_sha);
    df_header_encode (&h, slot.data (), df_sha256);
    df_header d; CHECK (df_header_decode (slot.data (), &d, df_sha256) == DF_OK);
    df_extent out[1]; uint32_t idx[1];
    CHECK (df_table_parse (&d, slot.data () + DF_BLOCK, out, idx, df_sha256) == DF_ERR_PADDING);
    df_put32 (&slot[DF_BLOCK + 12], 0);
    df_put32 (&slot[DF_BLOCK + 8], 3);                            // 3 segments listed, header says 2
    df_sha256 (&slot[DF_BLOCK], 16, h.table_sha);
    df_header_encode (&h, slot.data (), df_sha256);
    CHECK (df_header_decode (slot.data (), &d, df_sha256) == DF_OK);
    CHECK (df_table_parse (&d, slot.data () + DF_BLOCK, out, idx, df_sha256) == DF_ERR_EXTENT_SUM);
  }
  // Large random tables: the sort-based overlap check agrees with a brute-force one.
  std::mt19937_64 r (11);
  for (int round = 0; round < 200; ++round) {
    std::vector<df_extent> e;
    unsigned n = 1 + r () % 40;
    for (unsigned i = 0; i < n; ++i) e.push_back ({seg (r () % 400), (uint32_t) (1 + r () % 12), 0});
    bool overlap = false;
    for (unsigned i = 0; i < n; ++i)
      for (unsigned j = i + 1; j < n; ++j)
        if (e[i].first_rel < e[j].first_rel + e[j].segments * 512ULL && e[j].first_rel < e[i].first_rel + e[i].segments * 512ULL) overlap = true;
    CHECK (parseWith (e) == (overlap ? DF_ERR_OVERLAP : DF_OK));
  }
}

static void testSlots () {
  Lun lun;
  std::vector<df_extent> e = {{seg (50), 2, 0}};
  Opened o;
  openStore (o, lun, 10, 1, false);
  CHECK (o.load == DF_ERR_MAGIC);                                  // both slots blank: refused (fall back to the RAM disk)
  df_header h1 = baseHeader (1);
  publish (lun, h1, e, 0);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_OK && a.h.generation == 1 && a.pick == 1 && a.newest == 1); }
  df_header h2 = baseHeader (2);
  std::vector<df_extent> e2 = {{seg (60), 2, 0}};
  publish (lun, h2, e2, 1);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_OK && a.h.generation == 2 && a.rc == DF_OK && a.pick == 2); }
  // A torn newer slot (bad hash) leaves the older one in force.
  uint8_t blk[DF_BLOCK];
  lun.read (kUnixnuFirst + DF_SLOT_B, 1, blk);
  blk[40] ^= 0x55;
  lun.write (kUnixnuFirst + DF_SLOT_B, 1, blk);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_OK && a.h.generation == 1 && a.newest == 1); }
  // A newest REVOKED header refuses the file even though an ACTIVE one is still in the other slot; `newest` reports it.
  df_build_revoked (&h1, 3, blk, df_sha256);
  lun.write (kUnixnuFirst + DF_SLOT_B, 1, blk);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_ERR_REVOKED && a.newest == 3); }
  // Two different valid descriptors with one generation are refused.
  df_header h3 = baseHeader (1);
  h3.file_id[0] ^= 1;
  publish (lun, h3, e2, 1);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_ERR_AMBIGUOUS); }
  // The same descriptor in both slots is fine.
  df_header h4 = baseHeader (1);
  publish (lun, h4, e, 1);
  { Opened a; openStore (a, lun, 10, 1, false); CHECK (a.load == DF_OK); }
}

static void testFiemap () {
  const uint64_t S = 512ULL * 4096;
  df_extent out[8]; uint32_t n = 0, bad = 0;
  auto P = [] (uint64_t k) { return (kFsMain + k * 512) * 4096; };
  // Contiguous pieces merge; FIEMAP may split a run anywhere on block boundaries. f2fs marks every extent MERGED.
  df_fiemap_extent a[] = {{0, P (10), S / 2, DF_FIEMAP_MERGED}, {S / 2, P (10) + S / 2, S + S / 2, DF_FIEMAP_MERGED | DF_FIEMAP_DATA_ENCRYPTED},
                          {2 * S, P (40), S, DF_FIEMAP_LAST | DF_FIEMAP_DATA_ENCRYPTED | DF_FIEMAP_MERGED}};
  CHECK (df_from_fiemap (a, 3, 3 * S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_OK && n == 2 && out[0].first_rel == seg (10) && out[0].segments == 2 &&
         out[1].first_rel == seg (40) && out[1].segments == 1);
  df_fiemap_extent unwritten[] = {{0, P (1), S, DF_FIEMAP_LAST | 0x800}};
  CHECK (df_from_fiemap (unwritten, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_FLAGS);
  df_fiemap_extent delalloc[] = {{0, 0, S, DF_FIEMAP_LAST | 0x4}};
  CHECK (df_from_fiemap (delalloc, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_FLAGS);
  df_fiemap_extent encoded[] = {{0, P (1), S, DF_FIEMAP_LAST | 0x8}};                       // a compressed cluster
  CHECK (df_from_fiemap (encoded, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_FLAGS);
  df_fiemap_extent gap[] = {{0, P (1), S, 0}, {2 * S, P (5), S, DF_FIEMAP_LAST}};
  CHECK (df_from_fiemap (gap, 2, 3 * S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_GAP);
  df_fiemap_extent shortf[] = {{0, P (1), S, DF_FIEMAP_LAST}};
  CHECK (df_from_fiemap (shortf, 1, 2 * S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_GAP);
  df_fiemap_extent mis[] = {{0, P (1) + 4096, S, DF_FIEMAP_LAST}};
  CHECK (df_from_fiemap (mis, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_ALIGN);
  df_fiemap_extent half[] = {{0, P (1), S / 2, 0}, {S / 2, P (9), S / 2, DF_FIEMAP_LAST}};   // a segment split across two places
  CHECK (df_from_fiemap (half, 2, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_ALIGN);
  df_fiemap_extent meta[] = {{0, 1536ULL * 4096, S, DF_FIEMAP_LAST}};                      // SIT area: outside main
  CHECK (df_from_fiemap (meta, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_RANGE);
  df_fiemap_extent nolast[] = {{0, P (1), S, 0}};
  CHECK (df_from_fiemap (nolast, 1, S, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_LAST);
  CHECK (df_from_fiemap (a, 3, 3 * S + 4096, kFsMain, kFsBlocks, out, 8, &n, &bad) == DF_FM_SIZE);
  df_fiemap_extent many[3] = {{0, P (1), S, 0}, {S, P (3), S, 0}, {2 * S, P (5), S, DF_FIEMAP_LAST}};
  CHECK (df_from_fiemap (many, 3, 3 * S, kFsMain, kFsBlocks, out, 2, &n, &bad) == DF_FM_TOO_MANY);
  // FIEMAP's physical offsets are relative to the device f2fs is mounted on (dm-default-key at offset 0 of userdata): LBA = first + off.
  CHECK (df_data_rel (out, 2, 0) == seg (1) + 1 && df_data_rel (out, 2, 511) == seg (3) + 1 && df_data_rel (out, 2, 3 * 511) == ~0ULL);
}

// Window map and gate on a fragmented file, against brute force.
static void testMapAndGate () {
  Lun lun;
  std::vector<df_extent> e = {{seg (900), 3, 0}, {seg (17), 1, 0}, {seg (18), 2, 0}, {seg (4000), 5, 0}};   // 11 segments, in file order
  df_header h = baseHeader ();
  publish (lun, h, e);
  const uint64_t head = kUnixnuFirst + 4000000, headBlocks = 1000;
  CHECK (head + headBlocks - 1 <= kUnixnuLast);
  Opened o;
  openStore (o, lun, headBlocks + 11 * 511, 2, true, head, headBlocks);
  CHECK (o.rc == DF_OK && o.store.canaryChecked == 11 && o.store.canaryBad == 0);
  // Brute-force expansion of the window.
  std::vector<uint64_t> lbaOf;
  for (uint64_t i = 0; i < headBlocks; ++i) lbaOf.push_back (head + i);
  for (auto &x : e)
    for (uint32_t s = 0; s < x.segments; ++s)
      for (uint32_t b = 1; b < 512; ++b) lbaOf.push_back (kUserFirst + x.first_rel + s * 512ULL + b);
  CHECK (lbaOf.size () == o.store.windowBlocks);
  for (uint64_t rel = 0; rel < lbaOf.size (); ++rel) {
    uint64_t lba, s; uint32_t run;
    bool ok = o.store.map (rel, &lba, &run, &s);
    if (!ok || lba != lbaOf[rel]) { CHECK (false); break; }
    // The run is contiguous in the reference too, and never longer than the window.
    if (rel + run - 1 >= lbaOf.size () || lbaOf[rel + run - 1] != lba + run - 1) { CHECK (false); break; }
  }
  uint64_t lba, s; uint32_t run;
  CHECK (!o.store.map (lbaOf.size (), &lba, &run, &s));
  // Gate fuzz around every extent and the head.
  std::mt19937_64 r (5);
  unsigned mismatches = 0;
  std::vector<uint64_t> probes = {0, head - 1, head, head + headBlocks - 1, head + headBlocks, kUserFirst, kUserLast, kUnixnuFirst, kLunLast};
  for (auto &x : e) for (int d = -3; d <= 3; ++d) { probes.push_back (kUserFirst + x.first_rel + d); probes.push_back (kUserFirst + x.first_rel + x.segments * 512ULL + d); }
  for (int i = 0; i < 20000; ++i) probes.push_back (lbaOf[r () % lbaOf.size ()] + (int) (r () % 7) - 3);
  for (uint64_t p : probes)
    for (uint32_t n : {1u, 2u, 31u, 32u, 511u, 512u}) if (o.store.writeAllowed (p, n) != refAllowed (e, p, n, head, headBlocks)) ++mismatches;
  CHECK (mismatches == 0);
  // After a downgrade nothing passes the gate.
  o.store.downgrade ();
  CHECK (!o.store.writeAllowed (lbaOf[headBlocks + 3], 1) && !o.store.writeAllowed (head, 1));
  // The head may not overlap userdata, nor run past the 32-bit LBA range of WRITE(10).
  { Opened b; openStore (b, lun, 100, 2, false, kUserLast - 10, 100); CHECK (b.rc == DF_ERR_GEOMETRY); }
  { Opened b; openStore (b, lun, 100, 2, false, 0xffffffffULL - 50, 100); CHECK (b.rc == DF_ERR_GEOMETRY); }
  { Opened b; openStore (b, lun, 100, 2, false, 0xffffffffULL - 99, 100); CHECK (b.rc == DF_OK); }
  // Capacity, partition mismatch, modes.
  { Opened b; openStore (b, lun, 11 * 511 + 1, 2, false); CHECK (b.rc == DF_ERR_CAPACITY); }
  { Opened b; openStore (b, lun, 100, 2, false, 0, 0, kUserFirst, kUserLast + 512); CHECK (b.rc == DF_ERR_PARTITION); }
  { Opened b; openStore (b, lun, 100, 1, false); CHECK (b.rc == DF_OK && !b.store.writable);
    std::vector<uint8_t> buf (DF_BLOCK, 1); CHECK (!b.store.write (0, 1, buf.data ()) && b.io.refused == 0); }
  df_header ro = baseHeader (9); ro.state = DF_STATE_READONLY; publish (lun, ro, e, 1);
  { Opened b; openStore (b, lun, 100, 2, false); CHECK (b.rc == DF_ERR_READONLY); }
  { Opened b; openStore (b, lun, 100, 1, false); CHECK (b.rc == DF_OK); }
}

// Random window I/O: every WRITE reaching the medium is a data block of the file (or the head); contents read back.
static void testIo () {
  Lun lun;
  std::vector<df_extent> e = {{seg (300), 4, 0}, {seg (9), 2, 0}, {seg (7777), 3, 0}};
  df_header h = baseHeader ();
  publish (lun, h, e);
  const uint64_t head = kUnixnuFirst + 9000000, headBlocks = 700, window = headBlocks + 9 * 511;
  Opened o;
  openStore (o, lun, window, 2, false, head, headBlocks, kUserFirst, kUserLast, true);
  CHECK (o.rc == DF_OK);
  std::vector<std::vector<uint8_t>> oracle (window);
  std::mt19937_64 r (3);
  std::vector<uint8_t> buf (600 * DF_BLOCK);
  for (int op = 0; op < 3000; ++op) {
    uint64_t rel = r () % window;
    uint32_t n = 1 + (uint32_t) (r () % (op % 9 == 0 ? 600 : 40));
    if (n > window - rel) n = (uint32_t) (window - rel);
    if (r () & 1) {
      for (auto &b : buf) b = (uint8_t) r ();
      CHECK (o.store.write (rel, n, buf.data ()));
      for (uint32_t i = 0; i < n; ++i) oracle[rel + i].assign (&buf[(size_t) i * DF_BLOCK], &buf[(size_t) (i + 1) * DF_BLOCK]);
    } else {
      CHECK (o.store.read (rel, n, buf.data ()));
      for (uint32_t i = 0; i < n; ++i)
        if (!oracle[rel + i].empty () && memcmp (oracle[rel + i].data (), &buf[(size_t) i * DF_BLOCK], DF_BLOCK)) { CHECK (false); i = n; }
    }
  }
  unsigned outside = 0;
  for (auto &w : lun.writes) if (!refAllowed (e, w.first, w.second, head, headBlocks)) ++outside;
  CHECK (outside == 0 && o.io.refused == 0 && o.store.gateRefusals == 0 && !lun.writes.empty () && o.store.verifyMismatches == 0);
  // Canaries untouched by all that I/O.
  uint8_t c[DF_BLOCK], got[DF_BLOCK]; uint64_t index = 0;
  for (auto &x : e) for (uint32_t s = 0; s < x.segments; ++s, ++index) {
    uint64_t rel = x.first_rel + s * 512ULL;
    df_canary_block (&h, index, rel, c, df_sha256); lun.read (kUserFirst + rel, 1, got); CHECK (!memcmp (c, got, DF_BLOCK));
  }
  // Out of window and transport failures.
  CHECK (!o.store.read (window, 1, buf.data ()) && !o.store.write (window - 1, 2, buf.data ()));
  lun.failWriteAt = lun.writeCount;
  CHECK (!o.store.write (headBlocks + 5, 1, buf.data ()));
  lun.failWriteAt = ~0ULL;
}

// Read-back verification: a write that reaches the medium damaged, or one block off, is caught and stops every later write. Without
// the read-back the same faults go unnoticed (why the first rounds run with pagani-data-file-verify=1).
static void testVerifyWrites () {
  std::vector<df_extent> e = {{seg (500), 2, 0}};
  for (int kind = 0; kind < 2; ++kind) {
    for (int verify = 0; verify < 2; ++verify) {
      Lun lun;
      df_header h = baseHeader ();
      publish (lun, h, e);
      Opened o;
      openStore (o, lun, 2 * 511, 2, false, 0, 0, kUserFirst, kUserLast, verify);
      CHECK (o.rc == DF_OK);
      std::vector<uint8_t> buf (8 * DF_BLOCK, 0x3c);
      CHECK (o.store.write (0, 8, buf.data ()));
      if (kind == 0) lun.corruptWriteAt = lun.writeCount; else lun.misdirectWriteAt = lun.writeCount;
      const bool ok = o.store.write (100, 4, buf.data ());
      CHECK (ok == !verify);
      if (verify) {
        CHECK (o.store.poisoned && o.store.error == DF_ERR_VERIFY && o.store.verifyMismatches == 1);
        CHECK (!o.store.write (0, 1, buf.data ()) && !o.store.writeAllowed (kUserFirst + seg (500) + 1, 1));
      }
    }
  }
}

// The write self-test: only spare blocks past the window, read back, and findable raw by the Android helper (selftest-check).
static void testSelfTest () {
  std::vector<df_extent> e = {{seg (700), 3, 0}, {seg (40), 2, 0}, {seg (9000), 4, 0}};   // 9 segments
  Lun lun;
  df_header h = baseHeader ();
  publish (lun, h, e);
  const uint64_t window = 5 * 511 + 17;                           // 4 segments and a bit of the file stay spare
  Opened o;
  openStore (o, lun, window, 2, false);
  CHECK (o.rc == DF_OK);
  CHECK (o.store.selfTest (32) == DF_OK && o.store.selfTested == 96);
  unsigned outside = 0, inWindow = 0;
  for (auto &w : lun.writes) {
    if (!refAllowed (e, w.first, w.second, 0, 0)) ++outside;
    for (uint64_t rel = 0; rel < window; ++rel) {
      const uint64_t a = kUserFirst + df_data_rel (e.data (), 3, rel);
      if (a >= w.first && a < w.first + w.second) { ++inWindow; break; }
    }
  }
  CHECK (outside == 0 && inWindow == 0 && !lun.writes.empty ());
  // What the Android helper's selftest-check does: the last 32 data blocks of the file, raw.
  uint8_t want[DF_BLOCK], got[DF_BLOCK];
  unsigned good = 0;
  for (uint64_t w = h.data_blocks - 32; w < h.data_blocks; ++w) {
    const uint64_t rel = df_data_rel (e.data (), 3, w);
    df_selftest_block (&h, w, rel, want, df_sha256);
    lun.read (kUserFirst + rel, 1, got);
    good += !memcmp (want, got, DF_BLOCK);
  }
  CHECK (good == 32);
  // The window still works and the canaries are intact.
  std::vector<uint8_t> buf (DF_BLOCK, 0x11);
  CHECK (o.store.write (window - 1, 1, buf.data ()) && o.store.read (0, 1, buf.data ()));
  // A self-test where the window fills the store, on a read-only store, and over a reused spare segment.
  { Opened f; openStore (f, lun, h.data_blocks, 2, false); CHECK (f.rc == DF_OK && f.store.selfTest (8) == DF_ERR_CAPACITY); }
  { Opened f; openStore (f, lun, window, 1, false); CHECK (f.rc == DF_OK && f.store.selfTest (8) == DF_ERR_READONLY); }
  {
    std::vector<uint8_t> blk (DF_BLOCK);
    Lun::stale (4242, blk.data ());
    lun.write (kUserFirst + seg (9003), 1, blk.data ());           // canary of the file's last segment
    lun.writes.clear ();
    Opened f; openStore (f, lun, window, 2, false);
    CHECK (f.rc == DF_OK && f.store.selfTest (8) == DF_ERR_CANARY && f.store.poisoned);
    for (auto &w : lun.writes) CHECK (w.first < kUserFirst + seg (9003) || w.first >= kUserFirst + seg (9004));
  }
}

// Android reused a segment (ciphertext over the canary) or discarded it (zeros): eager open refuses; lazy use poisons the store.
static void testCanaryLoss () {
  std::vector<df_extent> e = {{seg (40), 3, 0}, {seg (90), 2, 0}};
  for (int kind = 0; kind < 2; ++kind) {
    Lun lun;
    df_header h = baseHeader ();
    publish (lun, h, e);
    uint64_t victim = kUserFirst + seg (91);                                  // file segment 4
    std::vector<uint8_t> blk (DF_BLOCK, 0);
    if (kind == 0) Lun::stale (victim * 7, blk.data ());
    lun.write (victim, 1, blk.data ());
    lun.writes.clear ();
    { Opened o; openStore (o, lun, 5 * 511, 1, true); CHECK (o.rc == DF_ERR_CANARY && o.store.canaryBad == 1 && o.store.firstBadSegment == 4 && o.store.canaryChecked == 5); }
    Opened o;
    openStore (o, lun, 5 * 511, 2, false);
    CHECK (o.rc == DF_OK);
    std::vector<uint8_t> buf (DF_BLOCK * 4, 0x5a);
    CHECK (o.store.write (0, 4, buf.data ()));                                // segment 0: fine
    CHECK (!o.store.read (4 * 511 + 3, 1, buf.data ()));                      // segment 4: refused
    CHECK (o.store.poisoned && !o.store.writable && o.store.error == DF_ERR_CANARY);
    CHECK (!o.store.write (0, 1, buf.data ()));                               // and nothing is written any more
    for (auto &w : lun.writes) CHECK (w.first < victim || w.first >= victim + 512);
  }
}

// End to end: NativeRamBlock's overlay over the store, three boots, a big sparse seed, then a reused segment.
struct WindowOps {
  Store *s;
  bool read (uint64_t b, uint32_t n, void *d) { return s->read (b, n, d); }
  bool write (uint64_t b, uint32_t n, const void *src) { return s->write (b, n, src); }
  bool sync () { return s->sync (); }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read (uint64_t at, void *p, uint64_t n) { memcpy (p, &data[at], n); return n; }
  uint64_t copy (uint64_t at, const void *p, uint64_t n) { memcpy (&data[at], p, n); return n; }
};
static void testEndToEnd () {
  using namespace persist;
  const uint64_t pages = 40000;                                  // 156 MiB logical disk: window 1 + 2 + 40000 blocks
  const uint64_t needBlocks = windowBlocks (pages);
  const uint32_t segs = (uint32_t) ((needBlocks + 510) / 511);   // 79 segments of the file
  std::vector<df_extent> e;
  std::mt19937_64 r (21);
  uint64_t at = 60000;
  for (uint32_t left = segs; left;) {                            // fragmented like the simulated 128 GiB allocation
    uint32_t n = 1 + (uint32_t) (r () % 9); if (n > left) n = left;
    e.push_back ({seg (at), n, 0});
    at += n + 1 + r () % 50; left -= n;
  }
  Lun lun;
  df_header h = baseHeader ();
  publish (lun, h, e);
  // A sparse seed with a few nonzero pages (SeedDisk as NativeRamBlock builds it for disks above 2 GiB).
  std::vector<uint64_t> index = {0, 1, 7, 1000, pages - 1};
  std::vector<std::vector<uint8_t>> seedData;
  std::vector<uint8_t *> seedPtr;
  for (size_t i = 0; i < index.size (); ++i) { seedData.emplace_back (4096, (uint8_t) (0x30 + i)); }
  for (auto &d : seedData) seedPtr.push_back (d.data ());
  SeedDisk seed; seed.bytes = pages * 4096; seed.count = seed.capacity = index.size (); seed.index = index.data (); seed.page = seedPtr.data ();
  uint8_t digest[32] = {0x5e, 0xed};
  std::vector<uint8_t> oracle (pages * 4096, 0);
  for (size_t i = 0; i < index.size (); ++i) memcpy (&oracle[index[i] * 4096], seedData[i].data (), 4096);
  std::vector<uint8_t> io (200 * 1024), stage (32 * kBlock);
  for (int boot = 0; boot < 3; ++boot) {
    Opened o;
    openStore (o, lun, needBlocks, 2, boot == 2, 0, 0, kUserFirst, kUserLast, boot == 1);   // boot 3 checks every canary first, boot 2 reads back
    CHECK (o.rc == DF_OK);
    WindowOps ops = {&o.store};
    std::vector<uint8_t> bits (bitmapBlocks (pages) * kBlock);
    Overlay ov;
    Open got = ov.open (ops, pages, digest, false, bits.data ());
    CHECK (got == (boot == 0 ? Open::Fresh : Open::Resumed));
    bool storeError = false;
    if (boot > 0) {                                              // everything the earlier boots wrote is back
      for (uint64_t off = 0; off < seed.bytes; off += 100000) {
        uint64_t n = seed.bytes - off < 100000 ? seed.bytes - off : 100000; Buffer b {io};
        if (directDiskIO (seed, ov, ops, stage.data (), 32, false, off, n, b, &storeError) != n || memcmp (io.data (), &oracle[off], n)) { CHECK (false); break; }
      }
    }
    for (int op = 0; op < 700; ++op) {
      uint64_t length = 1 + r () % 60000, off = r () % (seed.bytes - length);
      if (op % 3 == 0) { off &= ~4095ULL; length = (length + 4095) & ~4095ULL; if (length > seed.bytes - off) length = seed.bytes - off; }
      Buffer b {io};
      if (r () % 3) {
        for (uint64_t k = 0; k < length; ++k) io[k] = (uint8_t) r ();
        CHECK (directDiskIO (seed, ov, ops, stage.data (), 32, true, off, length, b, &storeError) == length && !storeError);
        memcpy (&oracle[off], io.data (), length);
      } else {
        CHECK (directDiskIO (seed, ov, ops, stage.data (), 32, false, off, length, b, &storeError) == length && !storeError);
        CHECK (!memcmp (io.data (), &oracle[off], length));
      }
    }
    unsigned outside = 0;
    for (auto &w : lun.writes) if (!refAllowed (e, w.first, w.second, 0, 0)) ++outside;
    CHECK (outside == 0 && o.io.refused == 0 && o.store.verifyMismatches == 0);
    lun.writes.clear ();
  }
  // Android reuses the segment that holds the overlay header: the next boot refuses the store before reading anything from it.
  std::vector<uint8_t> blk (DF_BLOCK);
  Lun::stale (99, blk.data ());
  lun.write (kUserFirst + e[0].first_rel, 1, blk.data ());
  { Opened o; openStore (o, lun, needBlocks, 2, true); CHECK (o.rc == DF_ERR_CANARY); }
  { Opened o; openStore (o, lun, needBlocks, 2, false); CHECK (o.rc == DF_OK);
    WindowOps ops = {&o.store}; std::vector<uint8_t> bits (bitmapBlocks (pages) * kBlock); Overlay ov;
    CHECK (ov.open (ops, pages, digest, false, bits.data ()) == Open::Failed && o.store.poisoned); }
}

// The simulated 128 GiB pinned allocation of the 10-29 f2fs snapshot (f2fs_free_runs.py --export): build, parse, map, gate.
static void testSimulated (const char *path) {
  FILE *f = fopen (path, "r");
  if (!f) { printf ("skip: %s not found\n", path); return; }
  std::vector<df_extent> e;
  char line[256];
  while (fgets (line, sizeof line, f)) {
    if (line[0] == '#') continue;
    unsigned long long first; unsigned segs;
    if (sscanf (line, "%llu %u", &first, &segs) == 2) e.push_back ({first, segs, 0});
  }
  fclose (f);
  Lun lun;
  df_header h = baseHeader ();
  publish (lun, h, e);
  const uint64_t pages = 32ULL << 20;                             // a 128 GiB Data disk
  const uint64_t need = persist::windowBlocks (pages);
  Opened o;
  openStore (o, lun, need, 2, true);
  printf ("simulated: %zu extents, %llu segments, data %llu blocks (%.2f GiB), window for 128 GiB needs %llu blocks -> %s\n", e.size (),
          (unsigned long long) o.h.segment_count, (unsigned long long) o.h.data_blocks, o.h.data_blocks * 4096.0 / (1ULL << 30),
          (unsigned long long) need, df_error_name (o.rc));
  CHECK (o.rc == DF_ERR_CAPACITY);                                // 65536 segments hold 127.75 GiB of data: 128 GiB does not fit
  const uint64_t fits = df_max_disk_pages (o.h.data_blocks);
  Opened p;
  openStore (p, lun, persist::windowBlocks (fits), 2, true);
  CHECK (p.rc == DF_OK && p.store.canaryChecked == 65536 && p.memory.size () < 320 * 1024);
  printf ("simulated: largest disk %llu pages (%.3f GiB), store memory %zu bytes, %llu canaries checked\n", (unsigned long long) fits,
          fits * 4096.0 / (1ULL << 30), p.memory.size (), (unsigned long long) p.store.canaryChecked);
  std::mt19937_64 r (8);
  unsigned bad = 0;
  for (int i = 0; i < 300000; ++i) {
    uint64_t rel = r () % p.store.windowBlocks, lba, s; uint32_t run;
    if (!p.store.map (rel, &lba, &run, &s)) { ++bad; continue; }
    const uint64_t canary = kUserFirst + p.store.segmentRel (s);
    // The gate is physical: past the end of the segment (not of the window) it must refuse.
    const bool segmentEnd = lba + run == canary + DF_SEG_BLOCKS;
    if (!p.store.writeAllowed (lba, 1) || !p.store.writeAllowed (lba + run - 1, 1) || p.store.writeAllowed (canary, 1) ||
        (segmentEnd && p.store.writeAllowed (lba, run + 1)) || lba <= canary || lba + run > canary + DF_SEG_BLOCKS ||
        lba < kUserFirst + kFsMain || lba > kUserLast)
      ++bad;
  }
  CHECK (bad == 0);
}

// Cross-language vectors for df_decode.py: a slot image and the SHA-256 of two canary blocks and one self-test block.
static int emit (const char *path) {
  df_header h = baseHeader (7);
  std::vector<df_extent> e = {{seg (321), 5, 0}, {seg (11), 2, 0}};
  auto slot = slotOf (h, e);
  FILE *f = fopen (path, "wb");
  if (!f || fwrite (slot.data (), 1, slot.size (), f) != slot.size ()) return 1;
  fclose (f);
  uint8_t c[DF_BLOCK], d[32];
  for (uint64_t i : {0ULL, 6ULL}) {
    df_canary_block (&h, i, df_segment_rel (e.data (), 2, i), c, df_sha256);
    df_sha256 (c, DF_BLOCK, d);
    printf ("canary %llu %llu ", (unsigned long long) i, (unsigned long long) df_segment_rel (e.data (), 2, i));
    for (int k = 0; k < 32; ++k) printf ("%02x", d[k]);
    printf ("\n");
  }
  const uint64_t w = h.data_blocks - 1, rel = df_data_rel (e.data (), 2, w);
  df_selftest_block (&h, w, rel, c, df_sha256);
  df_sha256 (c, DF_BLOCK, d);
  printf ("selftest %llu %llu ", (unsigned long long) w, (unsigned long long) rel);
  for (int k = 0; k < 32; ++k) printf ("%02x", d[k]);
  printf ("\n");
  return 0;
}

int main (int argc, char **argv) {
  if (argc > 2 && !strcmp (argv[1], "--emit")) return emit (argv[2]);
  testSha ();
  testDiskSizing ();
  testHeader ();
  testTable ();
  testSlots ();
  testFiemap ();
  testMapAndGate ();
  testIo ();
  testVerifyWrites ();
  testSelfTest ();
  testCanaryLoss ();
  testEndToEnd ();
  testSimulated (argc > 1 ? argv[1] : "sim-128g-hint0.txt");
  printf ("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
