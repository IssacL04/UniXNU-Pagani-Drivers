// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of DataFileUfs.h, the NativeUfs side of the iOS Data file, included the way integration/NativeUfs.patch includes it:
// inside a namespace that provides NativeUfs's transport (hostRead4k, hostWrite4k behind hostInWindow and the read-only command
// filter, hostSyncCache), PartitionTable.h's ufsPartFind, boot arguments, IOMallocData and libkern's SHA-256. On top of it runs
// NativeRamBlock's overlay (PersistOverlay.h/DirectDisk.h, unchanged) through the native_ufs_store_* calls the patch routes here.
// Checked: every WRITE that reaches the simulated LUN is a verified data block of the file, a head block, or the one REVOKED slot
// block; mode 1 writes nothing (except a requested self-test in the spare blocks); refusals leave the store closed; no leak.
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
#include "DataFileBuild.h"
#include "DirectDisk.h"     // NativeRamBlock (read-only use)

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint64_t kUserFirst = 4020972, kUserLast = 107409151, kUnixnuFirst = 107409152, kUnixnuLast = 124186367;
static const uint64_t kFsMain = 297984, kFsBlocks = 103387919, kLunLast = 124186623;
static const uint64_t kImageBytes = 9384755200ULL;          // ~8.74 GiB root image
static const uint64_t kReserve = 1024 * 256;                // NativeUfs default sheng-log-reserve (1 GiB)

namespace mock {
typedef int IOReturn;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -1, kIOReturnIOError = -2, kIOReturnNotReady = -3;
constexpr uint32_t kDataMax = 131072;
struct UfsPartition { uint32_t lun; uint64_t first, blocks; };
enum : int { UfsPartOk = 0, UfsPartNotFound = -5 };
constexpr uint32_t kUfsPartScratch = 4096 + 128 * 128;

struct SHA256_CTX { df_sha256_ctx c; };
static void SHA256_Init (SHA256_CTX *x) { df_sha256_init (&x->c); }
static void SHA256_Update (SHA256_CTX *x, const void *p, size_t n) { df_sha256_update (&x->c, p, n); }
static void SHA256_Final (uint8_t *out, SHA256_CTX *x) { df_sha256_final (&x->c, out); }
static uint64_t mach_absolute_time () { static uint64_t t; return t += 7; }

static std::map<std::string, uint32_t> bootArgs;
static bool PE_parse_boot_argn (const char *name, void *value, int size)
{
  auto it = bootArgs.find (name);
  if (it == bootArgs.end () || size != 4) return false;
  memcpy (value, &it->second, 4);
  return true;
}
static int64_t allocated = 0;
static void *IOMallocData (size_t n) { void *p = malloc (n); if (p) { memset (p, 0xA5, n); allocated += (int64_t) n; } return p; }   // not zeroed
static void IOFreeData (void *p, size_t n) { allocated -= (int64_t) n; free (p); }

// LUN 0: sparse, never-written blocks read as stale ciphertext.
struct Medium {
  std::unordered_map<uint64_t, std::array<uint8_t, DF_BLOCK>> blocks;
  std::vector<std::pair<uint64_t, uint32_t>> writes;
  static void stale (uint64_t lba, uint8_t *out) { std::mt19937_64 r (lba * 0x9E3779B97F4A7C15ULL + 3); for (unsigned i = 0; i < DF_BLOCK; i += 8) { uint64_t v = r (); memcpy (out + i, &v, 8); } }
  bool read (uint64_t lba, uint32_t n, void *d) {
    if (!n || lba + n - 1 > kLunLast) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = blocks.find (lba + i);
      if (it == blocks.end ()) stale (lba + i, (uint8_t *) d + (uint64_t) i * DF_BLOCK); else memcpy ((uint8_t *) d + (uint64_t) i * DF_BLOCK, it->second.data (), DF_BLOCK);
    }
    return true;
  }
  void put (uint64_t lba, const void *s) { memcpy (blocks[lba].data (), s, DF_BLOCK); }   // the Android helper's raw writes
};
static Medium lun;
struct Gpt { bool present = true; uint32_t lun = 0; uint64_t first = kUserFirst, blocks = kUserLast - kUserFirst + 1; };
static Gpt gpt;
static int ufsPartFind (const char *name, UfsPartition *out, uint8_t *scratch)
{
  memset (scratch, 0x5a, kUfsPartScratch);
  if (strcmp (name, "userdata") || !gpt.present) return UfsPartNotFound;
  *out = {gpt.lun, gpt.first, gpt.blocks};
  return UfsPartOk;
}

// RootCommands.h with the patch: hostInWindow asks dataFileWriteAllowed, the read-only filter asks dataFileWritesArmed.
static bool dataFileWriteAllowed (uint64_t lba, uint32_t blocks);
static bool dataFileWritesArmed ();
static uint64_t refused = 0, syncs = 0, nonFua = 0;
static IOReturn hostRead4k (uint8_t l, uint64_t lba, uint32_t blocks, void *dst)
{
  if (l || !dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) return kIOReturnBadArgument;
  return lun.read (lba, blocks, dst) ? kIOReturnSuccess : kIOReturnIOError;
}
static IOReturn hostWrite4k (uint8_t l, uint64_t lba, uint32_t blocks, const void *src, bool fua)
{
  if (l || !src || !blocks || blocks > kDataMax / 4096 || !dataFileWriteAllowed (lba, blocks)) { ++refused; return kIOReturnBadArgument; }
  if (!dataFileWritesArmed ()) { ++refused; return kIOReturnBadArgument; }
  if (!fua) ++nonFua;
  lun.writes.push_back ({lba, blocks});
  for (uint32_t i = 0; i < blocks; ++i) lun.put (lba + i, (const uint8_t *) src + (uint64_t) i * DF_BLOCK);
  return kIOReturnSuccess;
}
static IOReturn hostSyncCache () { if (!dataFileWritesArmed ()) return kIOReturnNotReady; ++syncs; return kIOReturnSuccess; }

#include "DataFileUfs.h"
}  // namespace mock

using namespace mock;

// ---- the Android helper's side, raw ------------------------------------------------------------------------------------------------
static uint64_t seg (uint64_t k) { return kFsMain + k * DF_SEG_BLOCKS; }
static df_header baseHeader (uint64_t generation, uint32_t state = DF_STATE_ACTIVE)
{
  df_header h;
  memset (&h, 0, sizeof h);
  h.state = state;
  h.generation = generation;
  for (int i = 0; i < 16; ++i) h.file_id[i] = (uint8_t) (0x21 * i + 3);
  for (int i = 0; i < 32; ++i) h.nonce[i] = (uint8_t) (0x77 ^ (i * 5));
  h.part_first = kUserFirst;
  h.part_last = kUserLast;
  h.fs_main = kFsMain;
  h.fs_blocks = kFsBlocks;
  return h;
}
static void publish (df_header &h, const std::vector<df_extent> &e, int slot, bool canaries = true)
{
  std::vector<uint8_t> image ((size_t) DF_SLOT_BLOCKS * DF_BLOCK);
  uint32_t blocks = df_build_slot (&h, e.data (), (uint32_t) e.size (), image.data (), df_sha256);
  CHECK (blocks > 0);
  const uint64_t first = kUnixnuFirst + (slot ? DF_SLOT_B : DF_SLOT_A);
  for (uint32_t b = 1; b < blocks; ++b) lun.put (first + b, &image[(size_t) b * DF_BLOCK]);
  lun.put (first, image.data ());
  if (canaries) {
    uint8_t c[DF_BLOCK];
    uint64_t index = 0;
    for (auto &x : e)
      for (uint32_t s = 0; s < x.segments; ++s, ++index) {
        df_canary_block (&h, index, x.first_rel + (uint64_t) s * DF_SEG_BLOCKS, c, df_sha256);
        lun.put (kUserFirst + x.first_rel + (uint64_t) s * DF_SEG_BLOCKS, c);
      }
  }
}
static DataFileRoot root (uint64_t minWindow = 0x100000)
{
  DataFileRoot r;
  r.first = kUnixnuFirst;
  r.last = kUnixnuLast;
  r.imageBytes = kImageBytes;
  r.minWindowOffset = minWindow;
  r.reserveBlocks = kReserve;
  return r;
}
// Every block of a write is a data block (not a canary) of one of the file's segments, or in [head, head + headBlocks), or the slot.
static bool writeOk (const std::vector<df_extent> &e, uint64_t lba, uint32_t n, uint64_t head, uint64_t headBlocks, uint64_t slotLba)
{
  if (n == 1 && lba == slotLba) return true;
  if (headBlocks && lba >= head && lba + n <= head + headBlocks) return true;
  for (auto &x : e)
    for (uint32_t s = 0; s < x.segments; ++s) {
      const uint64_t a = kUserFirst + x.first_rel + (uint64_t) s * DF_SEG_BLOCKS;
      if (lba > a && lba + n <= a + DF_SEG_BLOCKS) return true;
    }
  return false;
}
static void reset (std::map<std::string, uint32_t> args)
{
  dataFileClose ();
  bootArgs = args;
  dataFileConfigLoad ();
  lun.writes.clear ();
  refused = syncs = nonFua = 0;
}

struct WindowOps {   // NativeRamBlock's StoreOps through the patched native_ufs_store_*
  bool read (uint64_t b, uint32_t n, void *d) { return dataFileStoreRead (b, n, d); }
  bool write (uint64_t b, uint32_t n, const void *s) { return dataFileStoreWrite (b, n, s); }
  bool sync () { return dataFileStoreSync (); }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read (uint64_t at, void *p, uint64_t n) { memcpy (p, &data[at], n); return n; }
  uint64_t copy (uint64_t at, const void *p, uint64_t n) { memcpy (&data[at], p, n); return n; }
};

int main ()
{
  using namespace persist;
  std::vector<df_extent> e;
  for (uint64_t k = 0, at = 20000; k < 40; ++k, at += 7) e.push_back ({seg (at), (uint32_t) (1 + k % 4), 0});   // 100 segments
  df_header h = baseHeader (4);
  publish (h, e, 1);
  const char *why = nullptr;
  uint64_t first = 0;
  const uint64_t pages = 20000, window = windowBlocks (pages);   // a 78 MiB disk in a 100 * 511 block store

  // Off: nothing happens.
  reset ({});
  CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "off") && gDataFileDiag.state == 0 && lun.writes.empty ());

  // Mode 1 (the probe round): every canary checked, nothing written, no write gets through.
  reset ({{"pagani-data-file", 1}});
  CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1 && gDataFileDiag.state == 1 && gDataFileDiag.checked == 100 &&
         gDataFileDiag.bad == 0 && gDataFileDiag.generation == 4 && gDataFileDiag.slot == 2 && first == kUserFirst + seg (20000) + 1);
  CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "already open"));
  {
    std::vector<uint8_t> blk (DF_BLOCK, 7);
    CHECK (dataFileStoreRead (0, 1, blk.data ()) && !dataFileStoreWrite (0, 1, blk.data ()) && !dataFileStoreSync ());
    CHECK (hostWrite4k (0, kUserFirst + seg (20000) + 1, 1, blk.data (), true) != kIOReturnSuccess);
    CHECK (lun.writes.empty () && !dataFileWritesArmed ());
  }

  // Mode 2: NativeRamBlock's overlay over the store for three boots; every write is FUA, inside the file's data blocks, read back.
  {
    uint8_t digest[32] = {9, 9};
    std::vector<uint8_t> oracle (pages * 4096, 0), io (64 * 1024), stage (32 * kBlock);
    std::vector<uint64_t> index;
    std::vector<uint8_t *> pagesOf;
    SeedDisk seed; seed.bytes = pages * 4096; seed.count = seed.capacity = 0; seed.index = index.data (); seed.page = pagesOf.data ();
    std::mt19937_64 r (31);
    for (int boot = 0; boot < 3; ++boot) {
      reset ({{"pagani-data-file", 2}});
      CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1 && gDataFileDiag.state == 2);
      WindowOps ops;
      std::vector<uint8_t> bits (bitmapBlocks (pages) * kBlock);
      Overlay ov;
      CHECK (ov.open (ops, pages, digest, false, bits.data ()) == (boot ? Open::Resumed : Open::Fresh));
      bool storeError = false;
      for (int op = 0; op < 400; ++op) {
        uint64_t length = 1 + r () % 40000, off = r () % (seed.bytes - length);
        Buffer b {io};
        if (r () % 2) {
          for (uint64_t k = 0; k < length; ++k) io[k] = (uint8_t) r ();
          CHECK (directDiskIO (seed, ov, ops, stage.data (), 32, true, off, length, b, &storeError) == length && !storeError);
          memcpy (&oracle[off], io.data (), length);
        } else {
          CHECK (directDiskIO (seed, ov, ops, stage.data (), 32, false, off, length, b, &storeError) == length && !storeError);
          CHECK (!memcmp (io.data (), &oracle[off], length));
        }
      }
      unsigned bad = 0;
      for (auto &w : lun.writes) bad += !writeOk (e, w.first, w.second, 0, 0, 0);
      CHECK (bad == 0 && refused == 0 && nonFua == 0 && gDataFileDiag.mismatches == 0 && !lun.writes.empty ());
      CHECK (boot ? syncs == 0 : syncs > 0);   // the overlay syncs only while it makes a fresh header; its writes are FUA
    }
  }

  // Self-test in a read-only boot: writes only the spare blocks, then the store is read-only; the Android helper finds the last run.
  reset ({{"pagani-data-file", 1}, {"pagani-data-file-selftest", 8}});
  CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1 && gDataFileDiag.state == 1 && gDataFileDiag.selftested == 24 && !dataFileWritesArmed ());
  {
    unsigned bad = 0;
    for (auto &w : lun.writes) {
      bad += !writeOk (e, w.first, w.second, 0, 0, 0);
      for (uint64_t rel = 0; rel < window; rel += 1) if (kUserFirst + df_data_rel (e.data (), 40, rel) >= w.first && kUserFirst + df_data_rel (e.data (), 40, rel) < w.first + w.second) { ++bad; break; }
    }
    CHECK (bad == 0 && lun.writes.size () >= 3);
    uint8_t want[DF_BLOCK], got[DF_BLOCK];
    unsigned good = 0;
    for (uint64_t w = h.data_blocks - 8; w < h.data_blocks; ++w) {
      const uint64_t rel = df_data_rel (e.data (), 40, w);
      df_selftest_block (&h, w, rel, want, df_sha256);
      lun.read (kUserFirst + rel, 1, got);
      good += !memcmp (want, got, DF_BLOCK);
    }
    CHECK (good == 8);
  }
  // A self-test with no spare block (the window fills the store) is skipped, not a refusal; nothing is written.
  reset ({{"pagani-data-file", 1}, {"pagani-data-file-selftest", 8}});
  CHECK (dataFileStoreOpen (h.data_blocks, root (), &first, &why) == 1 && gDataFileDiag.selftested == 0 && gDataFileDiag.state == 1 &&
         lun.writes.empty ());

  // The head: the unixnu tail below the reserve, clear of the image. Window = head + file.
  {
    reset ({{"pagani-data-file", 2}, {"pagani-data-file-head", 64}});
    const uint64_t headBlocks = 64 * 256, head = kUnixnuLast + 1 - kReserve - headBlocks;
    const uint64_t bigWindow = windowBlocks (pages + headBlocks - 2000);
    CHECK (dataFileStoreOpen (bigWindow, root (), &first, &why) == 1 && first == head && gDataFileDiag.capacity == headBlocks + h.data_blocks);
    std::vector<uint8_t> buf (40 * DF_BLOCK, 0x6b);
    CHECK (dataFileStoreWrite (headBlocks - 20, 40, buf.data ()) && dataFileStoreWrite (0, 1, buf.data ()));
    unsigned bad = 0;
    for (auto &w : lun.writes) bad += !writeOk (e, w.first, w.second, head, headBlocks, 0);
    CHECK (bad == 0 && lun.writes.size () >= 3);
    // Too much head: it would reach into the root image.
    reset ({{"pagani-data-file", 2}, {"pagani-data-file-head", 60000}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "head overlaps the root image") && gDataFileDiag.state == 3);
  }

  // Refusals: slots overlapping root windows, userdata missing / on another LUN / overlapping unixnu / moved, window too large,
  // a READONLY descriptor in mode 2. None of them writes or leaks.
  {
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (0x80000), &first, &why) && gDataFileDiag.error == DF_ERR_LAYOUT);
    gpt.present = false;
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "no userdata on LUN 0"));
    gpt = Gpt (); gpt.lun = 3;
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "no userdata on LUN 0"));
    gpt = Gpt (); gpt.blocks += 600;                                         // userdata grown into unixnu
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && gDataFileDiag.error == DF_ERR_LAYOUT);
    gpt = Gpt (); gpt.first -= 512; gpt.blocks += 512;                       // userdata moved: the descriptor no longer matches
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "userdata moved"));
    gpt = Gpt ();
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (h.data_blocks + 1, root (), &first, &why) && !strcmp (why, "window too large"));
    df_header ro = baseHeader (5, DF_STATE_READONLY);
    publish (ro, e, 0, false);
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "descriptor read-only"));
    reset ({{"pagani-data-file", 1}});
    CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1 && gDataFileDiag.generation == 5);
    CHECK (lun.writes.empty () && refused == 0);
    df_header back = baseHeader (6);
    publish (back, e, 1, false);                                             // canaries already carry this file_id and nonce
  }

  // A canary gone (Android reused segment 37): mode 1 reports and writes nothing; mode 2 refuses on first touch, writes one REVOKED
  // block into the free slot, and the next boot refuses the file before reading anything from it.
  {
    std::vector<uint8_t> junk (DF_BLOCK);
    Medium::stale (777, junk.data ());
    const uint64_t victim = kUserFirst + df_segment_rel (e.data (), 40, 37);
    lun.put (victim, junk.data ());
    reset ({{"pagani-data-file", 1}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "canary gone") && gDataFileDiag.bad == 1 && lun.writes.empty ());
    reset ({{"pagani-data-file", 2}});
    CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1);
    std::vector<uint8_t> buf (DF_BLOCK, 0x42);
    CHECK (dataFileStoreWrite (0, 1, buf.data ()));
    CHECK (!dataFileStoreWrite (37 * 511 + 9, 1, buf.data ()) && gDataFileDiag.state == 4 && gDataFileDiag.revokedGeneration == 7);
    CHECK (!dataFileStoreWrite (0, 1, buf.data ()));                         // poisoned: no more writes
    const uint64_t slotA = kUnixnuFirst + DF_SLOT_A;                         // the slot not holding generation 6
    unsigned bad = 0, slotWrites = 0;
    for (auto &w : lun.writes) { bad += !writeOk (e, w.first, w.second, 0, 0, slotA); slotWrites += w.first == slotA; }
    CHECK (bad == 0 && slotWrites == 1);
    reset ({{"pagani-data-file", 2}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "descriptor revoked") && lun.writes.empty ());
    // Android re-publishes after re-verifying (generation 8), XNU uses the file again.
    df_header again = baseHeader (8);
    publish (again, e, 0);
    reset ({{"pagani-data-file", 2}});
    CHECK (dataFileStoreOpen (window, root (), &first, &why) == 1 && gDataFileDiag.generation == 8);
    // pagani-data-file-revoke=0: the canary loss is reported, nothing is written to unixnu.
    lun.put (victim, junk.data ());
    reset ({{"pagani-data-file", 2}, {"pagani-data-file-revoke", 0}, {"pagani-data-file-eager", 1}});
    CHECK (!dataFileStoreOpen (window, root (), &first, &why) && !strcmp (why, "canary gone") && lun.writes.empty ());
  }

  dataFileClose ();
  CHECK (allocated == 0);
  printf ("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
