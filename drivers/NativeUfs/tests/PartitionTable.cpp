// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The production PartitionTable.h against LUNs of GPTs built here (PKX110-like: modem_a on LUN 4 from block 4870, 96 entries) and,
// given a directory, the phone's own primary GPT dumps sda..sdf_主GPT.bin (LUN 0..5, blocks 0..5). Every read is checked to stay
// inside its LUN; no read ever writes.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = 0xe00002d8, kIOReturnIOError = 0xe00002ca;
constexpr unsigned kDataMax = 131072;
struct UfsLun { uint32_t last = 0, blockBytes = 0; };
static UfsLun gLuns[8];
static struct { bool ready = true, poisoned = false; } gHost;
struct Lun { std::vector<uint8_t> data; bool fail = false; unsigned attention = 0; };
static Lun luns[8];
static unsigned reads, largest;
static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  assert (gHost.ready && lun < 8 && blocks && blocks <= kDataMax / 4096);
  assert (gLuns[lun].blockBytes == 4096 && lba + blocks - 1 <= gLuns[lun].last);   // never outside the LUN
  ++reads;
  if (blocks > largest) largest = blocks;
  Lun &l = luns[lun];
  if (l.fail) return kIOReturnIOError;
  if (l.attention) { --l.attention; return kIOReturnNotReady; }
  uint8_t *out = static_cast<uint8_t *> (dst);
  for (uint32_t b = 0; b < blocks; ++b) {
    const uint64_t at = (lba + b) * 4096;
    for (unsigned i = 0; i < 4096; ++i) out[b * 4096 + i] = at + i < l.data.size () ? l.data[at + i] : uint8_t ((lba + b) * 7 + i);
  }
  return kIOReturnSuccess;
}
#include "../PartitionTable.h"

static void put32 (uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t (v >> (8 * i)); }
static void put64 (uint8_t *p, uint64_t v) { put32 (p, uint32_t (v)); put32 (p + 4, uint32_t (v >> 32)); }
struct Part { std::string name; uint64_t first, last; };

static void gpt (unsigned lun, const std::vector<Part> &parts, uint32_t count = 96, uint32_t lastLba = 634879)
{
  gLuns[lun] = { lastLba, 4096 };
  Lun &l = luns[lun];
  l = Lun ();
  const uint32_t entryBlocks = (count * 128 + 4095) / 4096;
  l.data.assign ((2 + entryBlocks) * 4096, 0);
  uint8_t *h = l.data.data () + 4096, *e = l.data.data () + 8192;
  for (size_t i = 0; i < parts.size (); ++i) {
    uint8_t *entry = e + 128 * i;
    entry[0] = 0xa2; entry[16] = uint8_t (i + 1);
    put64 (entry + 32, parts[i].first);
    put64 (entry + 40, parts[i].last);
    for (size_t c = 0; c < parts[i].name.size () && c < 36; ++c) entry[56 + 2 * c] = uint8_t (parts[i].name[c]);
  }
  memcpy (h, "EFI PART", 8);
  put32 (h + 8, 0x10000); put32 (h + 12, 92);
  put64 (h + 24, 1); put64 (h + 32, lastLba); put64 (h + 40, 2 + entryBlocks); put64 (h + 48, lastLba - 5);
  put64 (h + 72, 2); put32 (h + 80, count); put32 (h + 84, 128);
  put32 (h + 88, ufsPartCrc32 (e, count * 128, false));
  put32 (h + 16, ufsPartCrc32 (h, 92, true));
}
static void refresh (unsigned lun)   // CRCs again after a change
{
  uint8_t *h = luns[lun].data.data () + 4096;
  put32 (h + 88, ufsPartCrc32 (h + 4096, ufsPartLe32 (h + 80) * 128, false));
  put32 (h + 16, ufsPartCrc32 (h, 92, true));
}
static void pkx110 ()
{
  for (auto &l : gLuns) l = UfsLun ();
  gpt (0, { { "ssd", 6, 7 }, { "userdata", 4020972, 124186617 } }, 128, 124186623);
  gpt (1, { { "xbl_a", 6, 901 } }, 32, 8190);
  gpt (2, { { "xbl_b", 6, 901 } }, 32, 8190);
  std::vector<Part> four;
  for (unsigned i = 0; i < 90; ++i) four.push_back ({ "part" + std::to_string (i), 200000 + 10 * i, 200000 + 10 * i + 9 });
  four.push_back ({ "modem_a", 4870, 4870 + 89600 - 1 });
  four.push_back ({ "dsp_a", 97798, 97798 + 16384 - 1 });
  four.push_back ({ "abcdefghijklmnopqrstuvwxyz0123456789", 300000, 300001 });   // 36 units: no terminator in the entry
  gpt (4, four);
  gpt (5, { { "modem_a", 10, 20 } }, 32, 1000);      // a GPT that fails its CRC: never consulted
  luns[5].data[8192 + 56] ^= 1;
  gHost = { true, false };
}

static int find (const char *name, UfsPartition *p)
{
  static uint8_t scratch[kUfsPartScratch];
  return ufsPartFind (name, p, scratch);
}

int main (int argc, char **argv)
{
  UfsPartition p;
  pkx110 ();
  assert (find ("modem_a", &p) == UfsPartOk && p.lun == 4 && p.first == 4870 && p.blocks == 89600);
  assert (find ("dsp_a", &p) == UfsPartOk && p.lun == 4 && p.first == 97798 && p.blocks == 16384);
  assert (find ("userdata", &p) == UfsPartOk && p.lun == 0 && p.first == 4020972);
  assert (find ("abcdefghijklmnopqrstuvwxyz0123456789", &p) == UfsPartOk && p.first == 300000 && p.blocks == 2);
  assert (find ("modem", &p) == UfsPartNotFound && find ("modem_ab", &p) == UfsPartNotFound && find ("MODEM_A", &p) == UfsPartNotFound);
  assert (find ("", &p) == UfsPartArgument && find (nullptr, &p) == UfsPartArgument);
  assert (find ("abcdefghijklmnopqrstuvwxyz0123456789x", &p) == UfsPartArgument);
  // a name on two LUNs is refused
  gpt (1, { { "xbl_a", 6, 901 }, { "modem_a", 1000, 1999 } }, 32, 8190);
  assert (find ("modem_a", &p) == UfsPartDuplicate);
  pkx110 ();
  // a LUN that cannot be read: the match elsewhere stands; with no match, an I/O error
  luns[2].fail = true;
  assert (find ("modem_a", &p) == UfsPartOk && p.lun == 4);
  assert (find ("nosuch", &p) == UfsPartIo);
  pkx110 ();
  // the host not up, no LUN found, no usable GPT anywhere
  gHost.ready = false;
  assert (find ("modem_a", &p) == UfsPartNotReady);
  gHost.ready = true;
  for (unsigned lun : { 0u, 1u, 2u, 4u }) luns[lun].data[4096] ^= 1;
  assert (find ("modem_a", &p) == UfsPartNoTable);
  pkx110 ();
  // per-LUN refusals: entries outside the usable range, another entry size, the array after the first usable block, a
  // usable range beyond the LUN, too many entries
  {
    uint8_t *h = luns[4].data.data () + 4096;
    put64 (luns[4].data.data () + 8192 + 128 * 90 + 40, 634875); refresh (4);
    assert (find ("modem_a", &p) == UfsPartNotFound);
    pkx110 (); h = luns[4].data.data () + 4096;
    put32 (h + 84, 256); refresh (4);
    assert (find ("modem_a", &p) == UfsPartNotFound);
    pkx110 (); h = luns[4].data.data () + 4096;
    put64 (h + 40, 4); refresh (4);
    assert (find ("modem_a", &p) == UfsPartNotFound);
    pkx110 (); h = luns[4].data.data () + 4096;
    gLuns[4].last = 634000;
    assert (find ("modem_a", &p) == UfsPartNotFound);
    pkx110 (); h = luns[4].data.data () + 4096;
    put32 (h + 80, 129); put32 (h + 16, ufsPartCrc32 (h, 92, true));   // header CRC only: the array would be longer than the LUN's
    assert (find ("modem_a", &p) == UfsPartNotFound);
    pkx110 ();
  }
  // reads: inside the LUN only, in 32-block commands, a unit attention tried again
  std::vector<uint8_t> buf (100 * 4096);
  reads = largest = 0;
  assert (ufsPartRead (4, 4870, 100, buf.data ()) == UfsPartOk && reads == 4 && largest == 32);
  for (unsigned i = 0; i < buf.size (); i += 4093) assert (buf[i] == uint8_t ((4870 + i / 4096) * 7 + i % 4096));
  luns[4].attention = 2;
  assert (ufsPartRead (4, 4870, 1, buf.data ()) == UfsPartOk);
  luns[4].attention = 3;
  assert (ufsPartRead (4, 4870, 1, buf.data ()) == UfsPartIo);
  gHost.poisoned = true;
  luns[4].attention = 1;
  reads = 0;
  assert (ufsPartRead (4, 4870, 1, buf.data ()) == UfsPartIo && reads == 0);
  gHost.poisoned = false;
  luns[4].attention = 0;
  assert (ufsPartRead (4, 634879, 1, buf.data ()) == UfsPartOk && ufsPartRead (4, 634879, 2, buf.data ()) == UfsPartRange);
  assert (ufsPartRead (4, 634880, 1, buf.data ()) == UfsPartRange && ufsPartRead (6, 0, 1, buf.data ()) == UfsPartRange);
  assert (ufsPartRead (9, 0, 1, buf.data ()) == UfsPartRange && ufsPartRead (4, 0, 0, buf.data ()) == UfsPartArgument);
  assert (ufsPartRead (4, UINT64_MAX, 2, buf.data ()) == UfsPartRange && ufsPartRead (4, 0, 1, nullptr) == UfsPartArgument);
  gHost.ready = false;
  assert (ufsPartRead (4, 4870, 1, buf.data ()) == UfsPartNotReady);
  gHost.ready = true;
  unsigned dumps = 0;
  if (argc > 1) {
    // The PKX110's own GPTs: LUN n from sd('a' + n)_主GPT.bin (protective MBR, header, entries), capacity at the backup header.
    for (auto &l : gLuns) l = UfsLun ();
    for (unsigned lun = 0; lun < 6; ++lun) {
      const std::string path = std::string (argv[1]) + "/sd" + char ('a' + lun) + "_主GPT.bin";
      std::ifstream file (path, std::ios::binary);
      if (!file) continue;
      luns[lun] = Lun ();
      luns[lun].data.assign (std::istreambuf_iterator<char> (file), {});
      assert (luns[lun].data.size () >= 3 * 4096);
      gLuns[lun] = { uint32_t (ufsPartLe64 (luns[lun].data.data () + 4096 + 32)), 4096 };
      ++dumps;
    }
    assert (dumps == 6);
    assert (find ("modem_a", &p) == UfsPartOk && p.lun == 4 && p.first == 4870 && p.blocks == 89600);
    assert (find ("modem_b", &p) == UfsPartOk && p.lun == 4 && p.first == 2400544 / 8 && p.blocks == 89600);
    assert (find ("dsp_a", &p) == UfsPartOk && p.lun == 4 && p.first == 782384 / 8 && p.blocks == 131072 / 8);
    assert (find ("userdata", &p) == UfsPartOk && p.lun == 0 && p.first == 32167776 / 8 && p.blocks == 961325176 / 8);
    assert (find ("xbl_a", &p) == UfsPartOk && p.lun == 1 && p.first == 48 / 8 && p.blocks == 7168 / 8);
    assert (find ("last_parti", &p) == UfsPartDuplicate);   // on LUN 1 and LUN 2
    assert (find ("modemst1", &p) == UfsPartOk && p.lun == 5);
  }
  printf ("PASS: GPT lookup on every LUN (PKX110 layout%s), duplicate and damaged tables, bounded reads%s\n",
          dumps ? " and the phone's GPT dumps" : "", "");
  return 0;
}
