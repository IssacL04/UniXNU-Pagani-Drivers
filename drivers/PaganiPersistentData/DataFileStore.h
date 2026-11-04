// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU side of the iOS Data file (DataFileFormat.h): the descriptor loader, the window -> UFS LBA map, the canary checks and the write
// gate. Transport independent and freestanding (no allocation, no exceptions, no STL), for NativeUfs's persistent store
// (native_ufs_store_*, see DataFileUfs.h). NativeRamBlock's overlay (PersistOverlay.h) keeps its window layout; only the provider
// behind it changes.
//
// The window is an optional head region (contiguous absolute LBAs that XNU owns: the free tail of the unixnu partition) followed by the
// data blocks of the file's segments in file order. Head blocks map 1:1; file block w maps to segment w / 511, block 1 + w % 511. No
// mapped run crosses a segment, so no I/O ever covers a canary. The store has capacity() blocks; the window NativeRamBlock asks for is
// at most that, and the blocks past it are spare (the write self-test uses them).
//
// Guards, in the order a write meets them:
//   1. open: descriptor hash, geometry, extent table (inside the f2fs main area, segment aligned, no overlap), the live GPT's userdata
//      equal to the descriptor's, the head clear of userdata, every LBA reachable by READ(10)/WRITE(10).
//   2. per segment, once per boot, before its first read or write: its canary block must be exactly df_canary_block (else the store is
//      poisoned: no write ever again in this boot).
//   3. writeAllowed (also called by NativeUfs's hostWrite4k for every WRITE(10)): the blocks lie in the head, or inside one segment's
//      data blocks (never block 0) of a segment verified in this boot, and the store is writable and not poisoned.
//   4. optional read-back (verifyWrites): every written run is read again and compared; a difference poisons the store.
//
// Io (absolute 4096-byte LBAs on LUN 0):
//   bool read4k (uint64_t lba, uint32_t blocks, void *dst);
//   bool write4k (uint64_t lba, uint32_t blocks, const void *src);   // NativeUfs: hostWrite4k with FUA, behind writeAllowed()
//   bool sync ();
// Callers serialize all calls (NativeRamBlock holds its gate for every request; the overlay open runs before the first request).
#pragma once
#include "DataFileFormat.h"

static constexpr uint64_t kDfNoSegment = ~0ULL;
static constexpr uint32_t kDfMaxRun = 32;      // blocks per transport command (NativeUfs kDataMax / 4096)

// Bytes of caller memory a store needs for this descriptor (DfStore::open carves it): extents, cumulative segment starts, the
// by-start index, the verified bitmap, two blocks for canary comparisons and a run buffer for read-back. 64-byte aligned pieces.
static inline uint64_t dfStoreMemoryBytes (uint32_t extents, uint64_t segments)
{
  auto up = [] (uint64_t v) { return (v + 63) & ~63ULL; };
  return up ((uint64_t) extents * sizeof (df_extent)) + up (((uint64_t) extents + 1) * 8) + up ((uint64_t) extents * 4) +
         up ((segments + 7) / 8) + 2ULL * DF_BLOCK + (uint64_t) kDfMaxRun * DF_BLOCK;
}

// Read both slots of the unixnu partition (absolute first LBA unixnuFirst) and return the descriptor in force: *h decoded and the
// selected slot's blocks in slotBuf (DF_SLOT_BLOCKS * 4096 bytes; the table starts at slotBuf + 4096). Nothing is written. *slotOut:
// 1 (A) or 2 (B); *newest: the highest generation of any valid slot (a REVOKED one included), for a writer of the next generation.
template <class Io> static int dfLoadDescriptor (Io &io, uint64_t unixnuFirst, uint8_t *slotBuf, df_header *h, df_sha256_fn sha,
                                                 int *slotOut = nullptr, uint64_t *newest = nullptr)
{
  if (!slotBuf || !h || !sha) return DF_ERR_ARG;
  uint8_t *a = slotBuf, *b = slotBuf + DF_BLOCK;   // the two header blocks first; the table of the winner is read afterwards
  if (!io.read4k (unixnuFirst + DF_SLOT_A, 1, a) || !io.read4k (unixnuFirst + DF_SLOT_B, 1, b)) return DF_ERR_IO;
  df_header ha, hb;
  int rcA = df_header_decode (a, &ha, sha), rcB = df_header_decode (b, &hb, sha), err = DF_OK;
  if (newest) {
    *newest = 0;
    if (rcA == DF_OK && ha.generation > *newest) *newest = ha.generation;
    if (rcB == DF_OK && hb.generation > *newest) *newest = hb.generation;
  }
  int pick = df_pick_slot (&ha, rcA, a, &hb, rcB, b, &err);
  if (slotOut) *slotOut = pick;
  if (!pick) return err;
  *h = pick == 1 ? ha : hb;
  if (pick == 2) memcpy (a, b, DF_BLOCK);
  const uint64_t first = unixnuFirst + (pick == 1 ? DF_SLOT_A : DF_SLOT_B);
  const uint32_t tableBlocks = (uint32_t) (((uint64_t) h->extent_count * DF_EXTENT_BYTES + DF_BLOCK - 1) / DF_BLOCK);
  for (uint32_t done = 0; done < tableBlocks;) {
    uint32_t n = tableBlocks - done > kDfMaxRun ? kDfMaxRun : tableBlocks - done;
    if (!io.read4k (first + 1 + done, n, slotBuf + (uint64_t) (1 + done) * DF_BLOCK)) return DF_ERR_IO;
    done += n;
  }
  return DF_OK;
}

template <class Io> struct DfStore {
  // Diagnostics (NativeUfs copies them out for the boot page).
  int error = DF_ERR_ARG;
  uint64_t canaryChecked = 0, canaryBad = 0, firstBadSegment = kDfNoSegment, reads = 0, writes = 0, gateRefusals = 0;
  uint64_t verifyMismatches = 0, selfTested = 0, scanTicks = 0;

  Io *io = nullptr;
  df_sha256_fn sha = nullptr;
  df_header h = {};
  df_extent *ext = nullptr;      // file order
  uint64_t *cum = nullptr;       // cum[k]: index of extent k's first segment; cum[n] = segment_count
  uint32_t *byStart = nullptr;   // extent indexes sorted by first_rel
  uint8_t *verified = nullptr;   // one bit per segment: its canary was checked in this boot
  uint8_t *expected = nullptr, *actual = nullptr, *check = nullptr;
  uint64_t headFirst = 0, headBlocks = 0, windowBlocks = 0;
  bool opened = false, writable = false, poisoned = false, verifyWrites = false;

  uint64_t capacity () const { return headBlocks + h.data_blocks; }

  // mode 1: read-only (probe), mode 2: read-write (refused unless the descriptor is ACTIVE). eager: check every canary now (all of
  // them, counting the bad ones), else each segment is checked the first time it is read or written in this boot. verify: read
  // every written run back. The descriptor must have been decoded (df_header_decode) and picked (df_pick_slot) by the caller.
  int open (Io &transport, const df_header &header, const uint8_t *table, void *memory, uint64_t memoryBytes, uint64_t gptFirst,
            uint64_t gptLast, uint64_t head, uint64_t headCount, uint64_t window, unsigned mode, bool eager, df_sha256_fn hash,
            uint64_t (*clock) () = nullptr, bool verify = false)
  {
    opened = writable = poisoned = false;
    canaryChecked = canaryBad = reads = writes = gateRefusals = verifyMismatches = selfTested = scanTicks = 0;
    firstBadSegment = kDfNoSegment;
    io = &transport;
    sha = hash;
    h = header;
    verifyWrites = verify;
    if (!table || !memory || !hash || (mode != 1 && mode != 2)) return error = DF_ERR_ARG;
    if (h.state == DF_STATE_REVOKED) return error = DF_ERR_REVOKED;
    if (h.state != DF_STATE_ACTIVE && h.state != DF_STATE_READONLY) return error = DF_ERR_STATE;
    if (mode == 2 && h.state != DF_STATE_ACTIVE) return error = DF_ERR_READONLY;
    if (int rc = df_check_partition (&h, gptFirst, gptLast)) return error = rc;
    if (memoryBytes < dfStoreMemoryBytes (h.extent_count, h.segment_count)) return error = DF_ERR_ARG;
    uint8_t *p = static_cast<uint8_t *> (memory);
    auto take = [&p] (uint64_t bytes) { uint8_t *q = p; p += (bytes + 63) & ~63ULL; return q; };
    ext = reinterpret_cast<df_extent *> (take ((uint64_t) h.extent_count * sizeof (df_extent)));
    cum = reinterpret_cast<uint64_t *> (take (((uint64_t) h.extent_count + 1) * 8));
    byStart = reinterpret_cast<uint32_t *> (take ((uint64_t) h.extent_count * 4));
    verified = take ((h.segment_count + 7) / 8);
    expected = take (DF_BLOCK);
    actual = take (DF_BLOCK);
    check = take ((uint64_t) kDfMaxRun * DF_BLOCK);
    memset (verified, 0, (size_t) ((h.segment_count + 7) / 8));
    if (int rc = df_table_parse (&h, table, ext, byStart, sha)) return error = rc;
    cum[0] = 0;
    for (uint32_t k = 0; k < h.extent_count; ++k) cum[k + 1] = cum[k] + ext[k].segments;
    // Every block of userdata and of the head must be addressable by READ(10)/WRITE(10) (32-bit LBA); the head (it lies in unixnu)
    // must be clear of userdata.
    if (h.part_last > 0xffffffffULL) return error = DF_ERR_GEOMETRY;
    if (headCount && (head > 0xffffffffULL || headCount > 0xffffffffULL - head + 1 ||
                      (head <= h.part_last && h.part_first < head + headCount)))
      return error = DF_ERR_GEOMETRY;
    headFirst = headCount ? head : 0;
    headBlocks = headCount;
    if (!window || window > capacity ()) return error = DF_ERR_CAPACITY;
    windowBlocks = window;
    opened = true;
    error = DF_OK;
    if (eager) {
      uint64_t t0 = clock ? clock () : 0;
      for (uint32_t i = 0; i < h.extent_count; ++i) {
        const uint32_t k = byStart[i];
        for (uint64_t s = cum[k]; s < cum[k + 1]; ++s) {
          int rc = verifySegment (s);
          if (rc == DF_ERR_IO) { opened = false; return error = rc; }
        }
      }
      scanTicks = clock ? clock () - t0 : 0;
      if (canaryBad) { opened = false; return error = DF_ERR_CANARY; }
    }
    writable = mode == 2;
    return DF_OK;
  }

  // From now on nothing is written (after a self-test in a read-only boot, or when the caller wants to stop writing).
  void downgrade () { writable = false; }

  // Partition-relative block of segment `s` (file order).
  uint64_t segmentRel (uint64_t s) const
  {
    uint32_t lo = 0, hi = h.extent_count;   // last k with cum[k] <= s
    while (hi - lo > 1) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (cum[mid] <= s) lo = mid; else hi = mid;
    }
    return ext[lo].first_rel + (s - cum[lo]) * DF_SEG_BLOCKS;
  }

  // Store block -> absolute LBA, the blocks that follow it contiguously (to the end of the head or of the segment, never past `limit`),
  // and the segment (kDfNoSegment in the head). `limit` is windowBlocks for the window, capacity() for the self-test.
  bool mapIn (uint64_t rel, uint64_t limit, uint64_t *lba, uint32_t *run, uint64_t *segment) const
  {
    if (!opened || limit > capacity () || rel >= limit) return false;
    uint64_t left = limit - rel;
    if (rel < headBlocks) {
      uint64_t n = headBlocks - rel;
      if (n > left) n = left;
      *lba = headFirst + rel;
      *run = (uint32_t) (n < 0xffffffffULL ? n : 0xffffffffULL);
      *segment = kDfNoSegment;
      return true;
    }
    uint64_t w = rel - headBlocks, s = w / DF_SEG_DATA, off = w % DF_SEG_DATA;
    if (s >= h.segment_count) return false;
    *lba = h.part_first + segmentRel (s) + 1 + off;
    uint64_t n = DF_SEG_DATA - off;
    *run = (uint32_t) (n < left ? n : left);
    *segment = s;
    return true;
  }
  bool map (uint64_t rel, uint64_t *lba, uint32_t *run, uint64_t *segment) const { return mapIn (rel, windowBlocks, lba, run, segment); }

  bool isVerified (uint64_t s) const { return verified[s >> 3] >> (s & 7) & 1; }

  // DF_OK, DF_ERR_CANARY (the segment is someone else's now: the store is poisoned, no further writes) or DF_ERR_IO.
  int verifySegment (uint64_t s)
  {
    if (s >= h.segment_count) return DF_ERR_ARG;
    if (isVerified (s)) return DF_OK;
    const uint64_t rel = segmentRel (s);
    if (!io->read4k (h.part_first + rel, 1, actual)) return DF_ERR_IO;
    df_canary_block (&h, s, rel, expected, sha);
    ++canaryChecked;
    if (memcmp (expected, actual, DF_BLOCK)) {
      ++canaryBad;
      if (firstBadSegment == kDfNoSegment) firstBadSegment = s;
      poison (DF_ERR_CANARY);
      return DF_ERR_CANARY;
    }
    verified[s >> 3] |= (uint8_t) (1u << (s & 7));
    return DF_OK;
  }

  void poison (int why) { poisoned = true; writable = false; error = why; }

  // The lowest gate (NativeUfs hostWrite4k asks it for every WRITE): the blocks lie in the head, or inside one segment's data blocks
  // (never block 0, the canary) of a segment whose canary was checked in this boot, and the store is writable and not poisoned.
  bool writeAllowed (uint64_t lba, uint32_t blocks) const
  {
    if (!opened || !writable || poisoned || !blocks) return false;
    if (headBlocks && lba >= headFirst && lba - headFirst < headBlocks && blocks <= headBlocks - (lba - headFirst)) return true;
    if (lba < h.part_first || lba > h.part_last) return false;
    const uint64_t rel = lba - h.part_first;
    uint32_t lo = 0, hi = h.extent_count;   // last extent (by start) with first_rel <= rel
    if (ext[byStart[0]].first_rel > rel) return false;
    while (hi - lo > 1) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (ext[byStart[mid]].first_rel <= rel) lo = mid; else hi = mid;
    }
    const uint32_t k = byStart[lo];
    const uint64_t into = rel - ext[k].first_rel;
    if (into >= (uint64_t) ext[k].segments * DF_SEG_BLOCKS) return false;
    const uint64_t off = into % DF_SEG_BLOCKS;
    if (!off || off + blocks > DF_SEG_BLOCKS) return false;
    return isVerified (cum[k] + into / DF_SEG_BLOCKS);
  }

  // Window I/O for native_ufs_store_read/write: whole blocks, split at segment and command boundaries.
  bool read (uint64_t rel, uint32_t blocks, void *dst)
  {
    if (!opened || !dst || !blocks || rel >= windowBlocks || blocks > windowBlocks - rel) return false;
    uint8_t *out = static_cast<uint8_t *> (dst);
    while (blocks) {
      uint64_t lba, segment;
      uint32_t run;
      if (!map (rel, &lba, &run, &segment)) return false;
      if (segment != kDfNoSegment && verifySegment (segment) != DF_OK) return false;
      uint32_t n = run < blocks ? run : blocks;
      if (n > kDfMaxRun) n = kDfMaxRun;
      if (!io->read4k (lba, n, out)) return false;
      ++reads;
      out += (uint64_t) n * DF_BLOCK;
      rel += n;
      blocks -= n;
    }
    return true;
  }

  // One gated, optionally read-back-checked write of n <= kDfMaxRun blocks at lba.
  bool writeRun (uint64_t lba, uint32_t n, const uint8_t *in)
  {
    if (!writeAllowed (lba, n)) { ++gateRefusals; poison (DF_ERR_GATE); return false; }
    if (!io->write4k (lba, n, in)) return false;
    ++writes;
    if (verifyWrites) {
      if (!io->read4k (lba, n, check)) return false;
      if (memcmp (check, in, (size_t) n * DF_BLOCK)) { ++verifyMismatches; poison (DF_ERR_VERIFY); return false; }
    }
    return true;
  }

  bool write (uint64_t rel, uint32_t blocks, const void *src)
  {
    if (!opened || !writable || poisoned || !src || !blocks || rel >= windowBlocks || blocks > windowBlocks - rel) return false;
    const uint8_t *in = static_cast<const uint8_t *> (src);
    while (blocks) {
      uint64_t lba, segment;
      uint32_t run;
      if (!map (rel, &lba, &run, &segment)) return false;
      if (segment != kDfNoSegment && verifySegment (segment) != DF_OK) return false;
      uint32_t n = run < blocks ? run : blocks;
      if (n > kDfMaxRun) n = kDfMaxRun;
      if (!writeRun (lba, n, in)) return false;
      in += (uint64_t) n * DF_BLOCK;
      rel += n;
      blocks -= n;
    }
    return true;
  }

  bool sync () { return opened && writable && !poisoned && io->sync (); }

  // Write self-test in the spare blocks past the window: up to three runs of `per` (1..kDfMaxRun) blocks — the first spare blocks, the
  // middle of the spare region and its last blocks (the end of the file) — each with df_selftest_block content, through the gate,
  // then read back block by block and compared. The pattern stays on the medium: spare blocks hold nothing the overlay reads, and
  // the Android helper's selftest-check finds the last run raw. Identity of a block: its file data index (head blocks: index | 2^63).
  // Returns DF_OK, DF_ERR_CAPACITY (no spare block), DF_ERR_READONLY, DF_ERR_VERIFY, DF_ERR_GATE, DF_ERR_CANARY or DF_ERR_IO.
  int selfTest (uint32_t per)
  {
    if (!opened || !writable || poisoned) return DF_ERR_READONLY;
    if (!per || per > kDfMaxRun) return DF_ERR_ARG;
    const uint64_t cap = capacity (), spare = cap - windowBlocks;
    if (!spare) return DF_ERR_CAPACITY;
    const uint64_t k = per < spare ? per : spare;
    const uint64_t starts[3] = {windowBlocks, windowBlocks + (spare - k) / 2, cap - k};
    for (unsigned r = 0; r < 3; ++r) {
      uint64_t rel = starts[r], left = k;
      while (left) {
        uint64_t lba, segment;
        uint32_t run;
        if (!mapIn (rel, cap, &lba, &run, &segment)) return DF_ERR_ARG;
        if (segment != kDfNoSegment) {
          int rc = verifySegment (segment);
          if (rc) return rc;
        }
        uint32_t n = run < left ? run : (uint32_t) left;
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t id = rel + i < headBlocks ? (rel + i) | (1ULL << 63) : rel + i - headBlocks;
          df_selftest_block (&h, id, segment == kDfNoSegment ? lba + i : lba + i - h.part_first, check + (uint64_t) i * DF_BLOCK, sha);
        }
        if (!writeAllowed (lba, n)) { ++gateRefusals; poison (DF_ERR_GATE); return DF_ERR_GATE; }
        if (!io->write4k (lba, n, check)) return DF_ERR_IO;
        ++writes;
        for (uint32_t i = 0; i < n; ++i) {
          if (!io->read4k (lba + i, 1, actual)) return DF_ERR_IO;
          if (memcmp (actual, check + (uint64_t) i * DF_BLOCK, DF_BLOCK)) { ++verifyMismatches; poison (DF_ERR_VERIFY); return DF_ERR_VERIFY; }
        }
        selfTested += n;
        rel += n;
        left -= n;
      }
    }
    return io->sync () ? DF_OK : DF_ERR_IO;
  }
};
