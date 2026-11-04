// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
#pragma once
// Read-only partition access for other drivers: find a partition by its GPT name on any LUN that READ CAPACITY found
// (LunDiscovery.h), and read 4 KiB blocks of a LUN inside its capacity. Used by PaganiAdsp to read the ADSP firmware in place from
// modem_a (LUN 4 on the PKX110). Included inside the transport namespace, after RootCommands.h and LunDiscovery.h: every block goes
// through hostRead4k, which takes gHost.lock, so these reads are serialized with the root path's and never share its buffers (the
// caller gives the scratch). Nothing here writes, and no command leaves a LUN's capacity.
//
// GPT checks, per LUN: "EFI PART" revision 1.0, header size 92..4096, header CRC, my LBA 1, 128-byte entries, 1..128 of them
// (the PKX110's LUN 4 has 96), the entry array before the first usable block and its CRC, the usable range inside the LUN, each
// used entry inside the usable range. A LUN whose GPT fails a check is skipped. A name (UTF-16LE, exact, at most 36 units) found
// more than once in all is refused.
#include <stdint.h>

enum : int {
  UfsPartOk = 0,
  UfsPartNotReady = -1,        // the UFS host is not up (yet)
  UfsPartArgument = -2,
  UfsPartIo = -3,              // a read failed (on a LUN with no match elsewhere)
  UfsPartNoTable = -4,         // no LUN has a usable GPT
  UfsPartNotFound = -5,
  UfsPartDuplicate = -6,       // the name is in more than one entry
  UfsPartRange = -7,           // a read outside the LUN, or a LUN READ CAPACITY did not find
};

struct UfsPartition {
  uint32_t lun;
  uint64_t first, blocks;      // 4 KiB blocks
};

static constexpr uint32_t kUfsPartBlock = 4096, kUfsPartMaxEntries = 128, kUfsPartEntryBytes = 128;
static constexpr uint32_t kUfsPartScratch = kUfsPartBlock + kUfsPartMaxEntries * kUfsPartEntryBytes;   // header + entries

static uint32_t ufsPartLe32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}
static uint64_t ufsPartLe64 (const uint8_t *at)
{
  return uint64_t (ufsPartLe32 (at)) | (uint64_t (ufsPartLe32 (at + 4)) << 32);
}
// The reflected IEEE CRC32 of GPT; the header's own CRC field counts as zero.
static uint32_t ufsPartCrc32 (const uint8_t *data, size_t bytes, bool header)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
static bool ufsPartUsed (const uint8_t *entry)
{
  uint8_t any = 0;
  for (unsigned i = 0; i < 16; ++i) any |= entry[i];
  return any != 0;
}
static bool ufsPartNameIs (const uint8_t *entry, const char *name, size_t length)
{
  for (size_t i = 0; i < length; ++i)
    if (entry[56 + 2 * i] != static_cast<uint8_t> (name[i]) || entry[57 + 2 * i]) return false;
  return length == 36 || (!entry[56 + 2 * length] && !entry[57 + 2 * length]);
}

// One LUN's GPT: UfsPartOk (matches counted into *matches, the first kept in *found), UfsPartIo, or UfsPartNoTable.
static int ufsPartScanLun (uint32_t lun, const char *name, size_t length, uint8_t *scratch, UfsPartition *found, unsigned *matches)
{
  const uint64_t last = gLuns[lun].last;
  if (hostRead4k (static_cast<uint8_t> (lun), 1, 1, scratch) != kIOReturnSuccess) return UfsPartIo;
  const uint8_t *h = scratch;
  const uint32_t headerBytes = ufsPartLe32 (h + 12), count = ufsPartLe32 (h + 80);
  if (ufsPartLe32 (h) != 0x20494645U || ufsPartLe32 (h + 4) != 0x54524150U || ufsPartLe32 (h + 8) != 0x00010000U ||
      headerBytes < 92 || headerBytes > kUfsPartBlock || ufsPartCrc32 (h, headerBytes, true) != ufsPartLe32 (h + 16) ||
      ufsPartLe64 (h + 24) != 1 || ufsPartLe32 (h + 84) != kUfsPartEntryBytes || !count || count > kUfsPartMaxEntries)
    return UfsPartNoTable;
  const uint64_t backup = ufsPartLe64 (h + 32), firstUsable = ufsPartLe64 (h + 40), lastUsable = ufsPartLe64 (h + 48);
  const uint64_t entriesLba = ufsPartLe64 (h + 72);
  const uint32_t entriesCrc = ufsPartLe32 (h + 88), entryBlocks = (count * kUfsPartEntryBytes + kUfsPartBlock - 1) / kUfsPartBlock;
  if (entriesLba < 2 || entriesLba + entryBlocks > firstUsable || firstUsable > lastUsable || lastUsable > last || backup > last)
    return UfsPartNoTable;
  uint8_t *entries = scratch + kUfsPartBlock;
  if (hostRead4k (static_cast<uint8_t> (lun), entriesLba, entryBlocks, entries) != kIOReturnSuccess) return UfsPartIo;
  if (ufsPartCrc32 (entries, count * kUfsPartEntryBytes, false) != entriesCrc) return UfsPartNoTable;
  for (uint32_t index = 0; index < count; ++index) {
    const uint8_t *entry = entries + index * kUfsPartEntryBytes;
    if (!ufsPartUsed (entry)) continue;
    const uint64_t first = ufsPartLe64 (entry + 32), end = ufsPartLe64 (entry + 40);
    if (first < firstUsable || end < first || end > lastUsable) return UfsPartNoTable;
    if (!ufsPartNameIs (entry, name, length)) continue;
    if (!(*matches)++) *found = { lun, first, end - first + 1 };
  }
  return UfsPartOk;
}

static int ufsPartFind (const char *name, UfsPartition *out, uint8_t *scratch)
{
  size_t length = 0;
  while (name && length <= 36 && name[length]) ++length;
  if (!name || !length || length > 36 || !out || !scratch) return UfsPartArgument;
  if (!gHost.ready) return UfsPartNotReady;
  unsigned matches = 0, tables = 0, failures = 0;
  UfsPartition found = { 0, 0, 0 };
  for (uint32_t lun = 0; lun < 8; ++lun) {
    if (gLuns[lun].blockBytes != kUfsPartBlock || !gLuns[lun].last) continue;
    const int rc = ufsPartScanLun (lun, name, length, scratch, &found, &matches);
    if (rc == UfsPartOk) ++tables;
    else if (rc == UfsPartIo) ++failures;
  }
  if (matches > 1) return UfsPartDuplicate;
  if (matches == 1) {
    *out = found;
    return UfsPartOk;
  }
  return tables ? (failures ? UfsPartIo : UfsPartNotFound) : failures ? UfsPartIo : UfsPartNoTable;
}

// `blocks` 4 KiB blocks from lba of the LUN into dst, at most kDataMax per command; a unit attention is tried again twice.
static int ufsPartRead (uint32_t lun, uint64_t lba, uint32_t blocks, uint8_t *dst)
{
  if (!dst || !blocks) return UfsPartArgument;
  if (!gHost.ready) return UfsPartNotReady;
  if (lun >= 8 || gLuns[lun].blockBytes != kUfsPartBlock) return UfsPartRange;
  const uint64_t last = gLuns[lun].last;
  if (lba > last || blocks - 1 > last - lba) return UfsPartRange;
  while (blocks) {
    const uint32_t chunk = blocks > kDataMax / kUfsPartBlock ? static_cast<uint32_t> (kDataMax / kUfsPartBlock) : blocks;
    IOReturn status = kIOReturnNotReady;
    for (unsigned attempt = 0; attempt < 3 && status == kIOReturnNotReady && !gHost.poisoned; ++attempt)
      status = hostRead4k (static_cast<uint8_t> (lun), lba, chunk, dst);
    if (status != kIOReturnSuccess) return UfsPartIo;
    dst += static_cast<size_t> (chunk) * kUfsPartBlock;
    lba += chunk;
    blocks -= chunk;
  }
  return UfsPartOk;
}
