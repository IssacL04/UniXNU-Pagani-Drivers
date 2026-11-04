// SPDX-License-Identifier: BSD-2-Clause-Patent
// Volatile layer for a big (> 2 GiB) Data disk, for NativeRamBlock (integration/NativeRamBlock.patch): the fallback when the persistent
// store is refused, failed or off, and the read-only verification mode (sheng-data-persist / pagani-data-file = 1 over a resumed
// store). Today a big disk without a read-write store fails every write (NativeRamBlock.cpp: isBig && !direct -> kIOReturnNotReady),
// and mode 1 over a resumed store reaches the Loader, which dereferences sparse.pages, which a big disk does not have.
//
// Pages the running system writes live in RAM (a two-level table: 1024 pages per leaf, leaves allocated on demand), up to a byte limit;
// every other page comes from `Base`: the store when the overlay holds it (read only), else the seed, else zeros. Nothing is ever
// written to the store from here, so the persisted Data stays exactly as the last read-write boot left it.
//   Base:  bool page (uint64_t index, uint8_t *dst4096)            the page as the disk holds it now (outside this layer)
//   Ops:   uint8_t *allocate ()                                     a zeroed 4096-byte page, or null
//          void release (uint8_t *page)                             gives back a page allocate() returned
//          void **table (uint64_t entries)                          a zeroed pointer array, or null
//          void untable (void **table, uint64_t entries)            gives back a table
//   Buf:   uint64_t read (uint64_t at, void *p, uint64_t n)         bytes of the request buffer (the data of a write)
//          uint64_t copy (uint64_t at, const void *p, uint64_t n)   bytes into the request buffer (the data of a read)
// Callers serialize I/O.
#pragma once
#include <stdint.h>
#include <string.h>

struct BigVolatile {
  static constexpr uint64_t kLeaf = 1024;
  uint64_t pages = 0, leaves = 0, resident = 0, limit = 0, refused = 0;
  uint8_t ***dir = nullptr;

  template <class Ops> bool init (uint64_t pageCount, uint64_t byteLimit, Ops &ops)
  {
    pages = pageCount;
    leaves = (pages + kLeaf - 1) / kLeaf;
    limit = byteLimit;
    resident = refused = 0;
    dir = reinterpret_cast<uint8_t ***> (ops.table (leaves ? leaves : 1));
    return dir != nullptr;
  }
  uint8_t *find (uint64_t i) const
  {
    if (i >= pages || !dir) return nullptr;
    uint8_t **leaf = dir[i / kLeaf];
    return leaf ? leaf[i % kLeaf] : nullptr;
  }
  // The RAM page for i, made from the disk's current page on first use; null when the limit or memory is exhausted, or Base fails.
  template <class Base, class Ops> uint8_t *ensure (uint64_t i, Base &base, Ops &ops, bool *noMemory)
  {
    if (uint8_t *p = find (i)) return p;
    if (i >= pages || !dir) return nullptr;
    if (resident + 4096 > limit) { ++refused; *noMemory = true; return nullptr; }
    uint8_t **&leaf = dir[i / kLeaf];
    if (!leaf && !(leaf = reinterpret_cast<uint8_t **> (ops.table (kLeaf)))) { *noMemory = true; return nullptr; }
    uint8_t *p = ops.allocate ();
    if (!p) { *noMemory = true; return nullptr; }
    if (!base.page (i, p)) { ops.release (p); return nullptr; }
    leaf[i % kLeaf] = p;
    resident += 4096;
    return p;
  }
  // Every page and table back to Ops (NativeRamBlock::free).
  template <class Ops> void release (Ops &ops)
  {
    if (!dir) return;
    for (uint64_t l = 0; l < leaves; ++l) {
      uint8_t **leaf = dir[l];
      if (!leaf) continue;
      for (uint64_t k = 0; k < kLeaf; ++k) if (leaf[k]) ops.release (leaf[k]);
      ops.untable (reinterpret_cast<void **> (leaf), kLeaf);
    }
    ops.untable (reinterpret_cast<void **> (dir), leaves ? leaves : 1);
    dir = nullptr;
    resident = 0;
  }
};

// Returns the bytes completed. A failed Base read, an exhausted limit (*noMemory) or a short buffer copy stops the request there.
template <class Base, class Ops, class Buf>
static uint64_t bigVolatileIO (BigVolatile &v, Base &base, Ops &ops, uint8_t *stage, bool write, uint64_t offset, uint64_t length, Buf &buf,
                               bool *noMemory)
{
  *noMemory = false;
  if (!length || offset > v.pages * 4096 || length > v.pages * 4096 - offset) return 0;
  uint64_t done = 0;
  while (done < length) {
    uint64_t at = offset + done, index = at / 4096, skip = at & 4095, n = 4096 - skip;
    if (n > length - done) n = length - done;
    uint64_t moved;
    if (write) {
      uint8_t *p = v.ensure (index, base, ops, noMemory);
      if (!p) break;
      moved = buf.read (done, p + skip, n);
    } else {
      const uint8_t *p = v.find (index);
      if (!p) {
        if (!base.page (index, stage)) break;
        p = stage;
      }
      moved = buf.copy (done, p + skip, n);
    }
    done += moved;
    if (moved != n) break;
  }
  return done;
}
