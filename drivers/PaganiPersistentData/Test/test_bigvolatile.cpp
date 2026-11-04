// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of BigVolatile.h: the RAM layer of a big Data disk over (store pages the overlay holds, read only) + seed + zeros.
// The store is never written; the byte limit refuses further pages; reads keep working past the limit.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>
#include "BigVolatile.h"
#include "DirectDisk.h"   // NativeRamBlock: PersistOverlay.h, SeedDisk.h (read-only use)

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Medium {   // the persistent store window
  std::map<uint64_t, std::vector<uint8_t>> blocks;
  unsigned writes = 0, reads = 0;
  bool read (uint64_t b, uint32_t n, void *d) {
    ++reads;
    for (uint32_t i = 0; i < n; ++i) { auto it = blocks.find (b + i); if (it == blocks.end ()) memset ((uint8_t *) d + i * 4096, 0xA5, 4096); else memcpy ((uint8_t *) d + i * 4096, it->second.data (), 4096); }
    return true;
  }
  bool write (uint64_t b, uint32_t n, const void *s) { ++writes; for (uint32_t i = 0; i < n; ++i) blocks[b + i].assign ((const uint8_t *) s + i * 4096, (const uint8_t *) s + (i + 1) * 4096); return true; }
  bool sync () { return true; }
};
struct Ops {
  uint64_t pagesOut = 0, tables = 0;
  uint8_t *allocate () { ++pagesOut; return (uint8_t *) calloc (1, 4096); }
  void release (uint8_t *p) { --pagesOut; free (p); }
  void **table (uint64_t n) { ++tables; return (void **) calloc (n, sizeof (void *)); }
  void untable (void **t, uint64_t) { --tables; free (t); }
};
struct Base {               // NativeRamBlock would pass: overlay (when active) -> store, else seed, else zero
  persist::Overlay *ov; Medium *m; const SeedDisk *seed; uint64_t pages; bool failNext = false;
  bool page (uint64_t i, uint8_t *dst) {
    if (failNext) { failNext = false; return false; }
    if (ov && ov->active && ov->has (i)) return m->read (persist::pageBlock (pages, i), 1, dst);
    const uint8_t *p = seed->find (i);
    if (p) memcpy (dst, p, 4096); else memset (dst, 0, 4096);
    return true;
  }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read (uint64_t at, void *p, uint64_t n) { memcpy (p, &data[at], n); return n; }
  uint64_t copy (uint64_t at, const void *p, uint64_t n) { memcpy (&data[at], p, n); return n; }
};

int main () {
  using namespace persist;
  const uint64_t pages = 3ULL << 20;                       // a 12 GiB disk (big: > 2 GiB)
  // Seed: a few nonzero pages.
  std::vector<uint64_t> index = {0, 5, 4096, pages - 1};
  std::vector<std::vector<uint8_t>> data;
  std::vector<uint8_t *> ptr;
  for (size_t i = 0; i < index.size (); ++i) data.emplace_back (4096, (uint8_t) (0x10 + i));
  for (auto &d : data) ptr.push_back (d.data ());
  SeedDisk seed; seed.bytes = pages * 4096; seed.count = seed.capacity = index.size (); seed.index = index.data (); seed.page = ptr.data ();
  // An earlier read-write boot left pages in the store.
  Medium m;
  std::vector<uint8_t> bits (bitmapBlocks (pages) * kBlock), stage (32 * kBlock);
  uint8_t digest[32] = {1};
  Overlay rw;
  CHECK (rw.open (m, pages, digest, false, bits.data ()) == Open::Fresh);
  std::map<uint64_t, std::vector<uint8_t>> oracle;        // page -> content where it differs from the seed/zero
  std::mt19937_64 r (17);
  for (uint64_t i : {1ULL, 5ULL, 777ULL, 778ULL, pages / 2, pages - 1}) {
    std::vector<uint8_t> pg (4096); for (auto &b : pg) b = (uint8_t) r ();
    CHECK (rw.writeRun (m, i, 1, pg.data ()));
    oracle[i] = pg;
  }
  // This boot: read-only verification (mode 1). The overlay is opened read only; all writes stay in RAM.
  const unsigned writesBefore = m.writes;
  std::vector<uint8_t> bits2 (bits.size ());
  Overlay ro;
  CHECK (ro.open (m, pages, digest, true, bits2.data ()) == Open::Resumed && ro.present == 6);
  Ops ops;
  BigVolatile v;
  CHECK (v.init (pages, 64 * 4096, ops));
  Base base = {&ro, &m, &seed, pages};
  auto expect = [&] (uint64_t i, uint8_t *out) {
    auto it = oracle.find (i);
    if (it != oracle.end ()) { memcpy (out, it->second.data (), 4096); return; }
    const uint8_t *p = seed.find (i); if (p) memcpy (out, p, 4096); else memset (out, 0, 4096);
  };
  std::vector<uint8_t> io (40 * 4096), want (40 * 4096);
  bool noMemory = false;
  for (int op = 0; op < 400; ++op) {
    static const uint64_t hot[7] = {0, 1, 5, 777, 4096, 0, 0};
    uint64_t pick = r () % 7, hotPage = pick == 5 ? pages / 2 : pick == 6 ? pages - 3 : hot[pick];
    uint64_t length = 1 + r () % (8 * 4096), page = (op % 4 == 0) ? hotPage : r () % pages;
    uint64_t off = page * 4096 + r () % 4096;
    if (off + length > pages * 4096) length = pages * 4096 - off;
    Buffer b {io};
    if (r () % 2 && v.resident + 9 * 4096 <= v.limit) {   // writes while the limit allows the whole request
      for (uint64_t k = 0; k < length; ++k) io[k] = (uint8_t) r ();
      CHECK (bigVolatileIO (v, base, ops, stage.data (), true, off, length, b, &noMemory) == length && !noMemory);
      for (uint64_t at = off; at < off + length;) {
        uint64_t i = at / 4096, skip = at & 4095, n = 4096 - skip; if (n > off + length - at) n = off + length - at;
        std::vector<uint8_t> pg (4096); expect (i, pg.data ()); memcpy (&pg[skip], &io[at - off], n); oracle[i] = pg; at += n;
      }
    } else {
      CHECK (bigVolatileIO (v, base, ops, stage.data (), false, off, length, b, &noMemory) == length);
      for (uint64_t at = off; at < off + length;) {
        uint64_t i = at / 4096, skip = at & 4095, n = 4096 - skip; if (n > off + length - at) n = off + length - at;
        uint8_t pg[4096]; expect (i, pg);
        if (memcmp (&io[at - off], pg + skip, n)) { CHECK (false); break; }
        at += n;
      }
    }
  }
  CHECK (m.writes == writesBefore);                        // the store was never written
  CHECK (v.resident == ops.pagesOut * 4096 && v.resident <= v.limit);
  // The limit: a write needing more pages is refused with noMemory; what was written before stays; reads still work.
  {
    std::vector<uint8_t> big (80 * 4096, 0x77); Buffer b {big};
    uint64_t freshStart = pages / 3;
    uint64_t got = bigVolatileIO (v, base, ops, stage.data (), true, freshStart * 4096, big.size (), b, &noMemory);
    CHECK (noMemory && got < big.size () && got % 4096 == 0 && v.resident == v.limit && v.refused > 0);
    for (uint64_t at = 0; at < got; at += 4096) { std::vector<uint8_t> pg (4096, 0x77); oracle[freshStart + at / 4096] = pg; }
    Buffer rb {io};
    CHECK (bigVolatileIO (v, base, ops, stage.data (), false, freshStart * 4096, 20 * 4096, rb, &noMemory) == 20 * 4096);
    for (uint64_t k = 0; k < 20; ++k) { uint8_t pg[4096]; expect (freshStart + k, pg); CHECK (!memcmp (&io[k * 4096], pg, 4096)); }
  }
  // A failing base read stops the request without keeping a page.
  {
    BigVolatile w; Ops o2; CHECK (w.init (pages, 1 << 20, o2));
    Base fb = {&ro, &m, &seed, pages}; fb.failNext = true;
    std::vector<uint8_t> one (4096, 1); Buffer b {one};
    CHECK (bigVolatileIO (w, fb, o2, stage.data (), true, 777 * 4096, 4096, b, &noMemory) == 0 && !noMemory && o2.pagesOut == 0 && !w.find (777));
    w.release (o2);
    CHECK (o2.tables == 0);
  }
  // Store refused (no overlay): the seed and zeros only, writes in RAM.
  {
    BigVolatile w; Ops o2; CHECK (w.init (pages, 1 << 20, o2));
    Base nb = {nullptr, &m, &seed, pages};
    std::vector<uint8_t> one (4096); Buffer b {one};
    CHECK (bigVolatileIO (w, nb, o2, stage.data (), false, 5 * 4096, 4096, b, &noMemory) == 4096 && one[0] == 0x11 && one[4095] == 0x11);
    CHECK (bigVolatileIO (w, nb, o2, stage.data (), false, 777 * 4096, 4096, b, &noMemory) == 4096 && one[0] == 0 && one[100] == 0);
    w.release (o2);
    CHECK (o2.tables == 0 && o2.pagesOut == 0);
  }
  const unsigned long long ramPages = v.resident / 4096, tables = ops.tables;
  // NativeRamBlock::free gives everything back.
  v.release (ops);
  CHECK (ops.pagesOut == 0 && ops.tables == 0 && !v.find (1) && v.resident == 0);
  printf ("%s: %d checks, %d failures (RAM pages %llu, tables %llu)\n", failures ? "FAIL" : "PASS", checks, failures, ramPages, tables);
  return failures ? 1 : 0;
}
