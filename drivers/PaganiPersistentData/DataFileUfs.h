// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeUfs's persistent Data store backed by the iOS Data file (a pinned file in Android's /data, DataFileFormat.h), for
// NativeRamBlock's overlay through native_ufs_store_*. Draft of the integration: NativeUfs.cpp includes it inside its transport
// namespace after RootCommands.h and PartitionTable.h when built with NATIVE_UFS_DATA_FILE=1 (integration/NativeUfs.patch); the host
// test (Test/test_ufs_glue.cpp) includes it the same way over a mock transport.
//
// Needs, declared before the include (NativeUfs has them):
//   IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst);
//   IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua);   // gated by hostInWindow
//   IOReturn hostSyncCache (void);
//   int ufsPartFind (const char *name, UfsPartition *out, uint8_t *scratch);   kUfsPartScratch, UfsPartOk
//   kDataMax, kIOReturnSuccess, PE_parse_boot_argn, IOMallocData/IOFreeData, SHA256_CTX/SHA256_Init/SHA256_Update/SHA256_Final,
//   mach_absolute_time
// and RootCommands.h calls dataFileWriteAllowed() from hostInWindow (every WRITE(10) of the driver passes it) and lets WRITE(10)
// and SYNCHRONIZE CACHE(10) through its read-only command filter only while dataFileWritesArmed().
//
// Boot arguments (read once):
//   pagani-data-file=0|1|2        off (default) | read only: verify and read, never write | read-write
//   pagani-data-file-eager=0|1    check every canary when the store opens (default 1 in mode 1, 0 in mode 2: each segment is then
//                                 checked before its first use in the boot)
//   pagani-data-file-verify=0|1   read every written run back and compare (default 1; the first rounds on a new device)
//   pagani-data-file-selftest=N   1..32: before the store is handed over, write N blocks at three places of the spare blocks past the
//                                 window, read them back, compare (also in mode 1: the store is downgraded to read-only afterwards)
//   pagani-data-file-head=MiB     put this much of the unixnu tail (below the log reserve, clear of the root image) before the file
//                                 in the window (default 0)
//   pagani-data-file-revoke=0|1   in mode 2 (or with a self-test), when a canary is gone, write a REVOKED header into the free
//                                 descriptor slot (one block of unixnu) so the next boot refuses the file at once (default 1)
#pragma once
#include "DataFileStore.h"

struct DataFileRoot {            // the root (unixnu) partition, as NativeUfs found it
  uint64_t first, last;          // absolute 4096-byte LBAs on LUN 0
  uint64_t imageBytes;           // the flashed root image (manifest image_bytes)
  uint64_t minWindowOffset;      // the lowest root window offset, bytes (the descriptor slots must end below it)
  uint64_t reserveBlocks;        // the tail NativeUfs keeps for its log and export regions
};

struct DataFileConfig {
  uint32_t mode, eager, verify, selftest, headMiB, revoke;
};

struct DataFileDiag {            // for the boot page (NativeUfs exports a copy)
  uint32_t mode, state, error, slot;   // state: 0 off, 1 open read-only, 2 open read-write, 3 refused, 4 poisoned
  uint64_t generation, extents, segments, capacity, window, checked, bad, firstBad, reads, writes, gateRefusals, mismatches,
           selftested, scanTicks, revokedGeneration;
};

struct DataFileUfsIo {
  bool read4k (uint64_t lba, uint32_t n, void *d)
  {
    uint8_t *p = static_cast<uint8_t *> (d);
    while (n) {
      const uint32_t c = n > kDataMax / DF_BLOCK ? static_cast<uint32_t> (kDataMax / DF_BLOCK) : n;
      if (hostRead4k (0, lba, c, p) != kIOReturnSuccess) return false;
      p += static_cast<uint64_t> (c) * DF_BLOCK;
      lba += c;
      n -= c;
    }
    return true;
  }
  bool write4k (uint64_t lba, uint32_t n, const void *s)
  {
    const uint8_t *p = static_cast<const uint8_t *> (s);
    while (n) {
      const uint32_t c = n > kDataMax / DF_BLOCK ? static_cast<uint32_t> (kDataMax / DF_BLOCK) : n;
      // Forced unit access: on the medium when the command completes (the overlay relies on it, NativeRamBlock's doSynchronizeCache too).
      if (hostWrite4k (0, lba, c, p, true) != kIOReturnSuccess) return false;
      p += static_cast<uint64_t> (c) * DF_BLOCK;
      lba += c;
      n -= c;
    }
    return true;
  }
  bool sync () { return hostSyncCache () == kIOReturnSuccess; }
};

static DataFileConfig gDataFileConfig;
static bool gDataFileConfigLoaded;
static DfStore<DataFileUfsIo> gDataFile;
static DataFileUfsIo gDataFileIo;
static uint8_t *gDataFileMemory;
static uint64_t gDataFileMemoryBytes, gDataFileUnixnuFirst, gDataFileNewest, gDataFileRevokeLba;
static int gDataFileSlot;
static bool gDataFileRevokeTried;
static DataFileDiag gDataFileDiag;

static inline void dataFileSha (const void *data, size_t bytes, uint8_t out[32])
{
  SHA256_CTX context;
  SHA256_Init (&context);
  SHA256_Update (&context, data, bytes);
  SHA256_Final (out, &context);
}

static inline uint64_t dataFileClock () { return mach_absolute_time (); }

static inline uint32_t dataFileArg (const char *name, uint32_t fallback, uint32_t max)
{
  uint32_t value = 0;
  return PE_parse_boot_argn (name, &value, sizeof (value)) && value <= max ? value : fallback;
}

// (Re)reads the boot arguments; the driver calls it once, the host test after changing them.
static inline const DataFileConfig &dataFileConfigLoad ()
{
  DataFileConfig c;
  c.mode = dataFileArg ("pagani-data-file", 0, 2);
  c.eager = dataFileArg ("pagani-data-file-eager", c.mode == 1 ? 1 : 0, 1);
  c.verify = dataFileArg ("pagani-data-file-verify", 1, 1);
  c.selftest = dataFileArg ("pagani-data-file-selftest", 0, kDfMaxRun);
  c.headMiB = dataFileArg ("pagani-data-file-head", 0, 65536);
  c.revoke = dataFileArg ("pagani-data-file-revoke", 1, 1);
  gDataFileConfig = c;
  gDataFileConfigLoaded = true;
  return gDataFileConfig;
}
static inline const DataFileConfig &dataFileConfig () { return gDataFileConfigLoaded ? gDataFileConfig : dataFileConfigLoad (); }
static inline unsigned dataFileMode () { return dataFileConfig ().mode; }

// hostInWindow: the file's verified data blocks (and the head) while the store is writable, and the one REVOKED header block while XNU
// writes it. Nothing else of userdata or unixnu is ever writable through this path.
static bool dataFileWriteAllowed (uint64_t lba, uint32_t blocks)
{
  if (gDataFileRevokeLba && lba == gDataFileRevokeLba && blocks == 1) return true;
  return gDataFile.writeAllowed (lba, blocks);
}
// The read-only command filter lets WRITE(10)/SYNCHRONIZE CACHE(10) through only while this is true.
static bool dataFileWritesArmed () { return gDataFileRevokeLba || (gDataFile.opened && gDataFile.writable && !gDataFile.poisoned); }

static inline void dataFileDiagUpdate ()
{
  DataFileDiag &d = gDataFileDiag;
  d.mode = dataFileConfig ().mode;
  d.error = static_cast<uint32_t> (gDataFile.error);
  d.state = !d.mode ? 0 : gDataFile.poisoned ? 4 : !gDataFile.opened ? 3 : gDataFile.writable ? 2 : 1;
  d.checked = gDataFile.canaryChecked;
  d.bad = gDataFile.canaryBad;
  d.firstBad = gDataFile.firstBadSegment;
  d.reads = gDataFile.reads;
  d.writes = gDataFile.writes;
  d.gateRefusals = gDataFile.gateRefusals;
  d.mismatches = gDataFile.verifyMismatches;
  d.selftested = gDataFile.selfTested;
  d.scanTicks = gDataFile.scanTicks;
}

// A canary is gone: Android reused, discarded or moved the file. Write a REVOKED header (newest generation + 1) into the slot that does
// not hold the descriptor in force, so the next boot refuses the file before it reads anything from it. Once per boot; mode 2 or a
// self-test only (mode 1 never writes). Returns true when the header is on the medium and read back.
static inline bool dataFileRevoke ()
{
  const DataFileConfig &c = dataFileConfig ();
  if (gDataFileRevokeTried || !c.revoke || (c.mode != 2 && !c.selftest) || !gDataFileUnixnuFirst || !gDataFileSlot) return false;
  gDataFileRevokeTried = true;
  uint8_t *block = static_cast<uint8_t *> (IOMallocData (2 * DF_BLOCK));
  if (!block) return false;
  const uint64_t generation = gDataFileNewest + 1;
  df_build_revoked (&gDataFile.h, generation, block, dataFileSha);
  gDataFileRevokeLba = gDataFileUnixnuFirst + (gDataFileSlot == 1 ? DF_SLOT_B : DF_SLOT_A);
  bool ok = gDataFileIo.write4k (gDataFileRevokeLba, 1, block) && gDataFileIo.read4k (gDataFileRevokeLba, 1, block + DF_BLOCK) &&
            !memcmp (block, block + DF_BLOCK, DF_BLOCK);
  gDataFileRevokeLba = 0;
  IOFreeData (block, 2 * DF_BLOCK);
  if (ok) {
    gDataFileNewest = generation;
    gDataFileDiag.revokedGeneration = generation;
  }
  return ok;
}

// native_ufs_store_open: the window of `blocks` store blocks NativeRamBlock asks for. Returns 1 and the first absolute LBA of the
// window (for the log only), 0 with a reason.
static inline int dataFileStoreOpen (uint64_t blocks, const DataFileRoot &root, uint64_t *firstLba, const char **why)
{
  const DataFileConfig &c = dataFileConfig ();
  const char *reason = nullptr;
  uint8_t *slot = nullptr, *scratch = nullptr;
  df_header h;
  UfsPartition part = { 0, 0, 0 };
  uint64_t head = 0, headBlocks = 0, gptFirst = 0, gptLast = 0, need = 0;
  int rc = DF_OK;
  if (gDataFile.opened || gDataFileMemory) {   // the store lives for the whole boot; a second open changes nothing
    if (why) *why = "already open";
    return 0;
  }
  gDataFileDiag.mode = c.mode;
  if (!c.mode) reason = "off";
  else if (!blocks || root.last < root.first) reason = "argument";
  else if (root.minWindowOffset < static_cast<uint64_t> (DF_SLOTS_END) * DF_BLOCK) { rc = DF_ERR_LAYOUT; reason = "descriptor slots overlap the root windows"; }
  if (reason) goto done;
  // userdata, as the live GPT has it now: on LUN 0 and nowhere else.
  scratch = static_cast<uint8_t *> (IOMallocData (kUfsPartScratch));
  if (!scratch) { reason = "no memory"; goto done; }
  rc = ufsPartFind ("userdata", &part, scratch);
  IOFreeData (scratch, kUfsPartScratch);
  if (rc != UfsPartOk || part.lun != 0 || !part.blocks) { rc = DF_ERR_PARTITION; reason = "no userdata on LUN 0"; goto done; }
  gptFirst = part.first;
  gptLast = part.first + part.blocks - 1;
  if (root.first <= gptLast && gptFirst <= root.last) { rc = DF_ERR_LAYOUT; reason = "userdata overlaps the root partition"; goto done; }
  // The head: the unixnu tail below the reserve, clear of the root image plus a 4 MiB guard.
  if (c.headMiB) {
    const uint64_t partition = root.last - root.first + 1, imageBlocks = (root.imageBytes + DF_BLOCK - 1) / DF_BLOCK;
    headBlocks = static_cast<uint64_t> (c.headMiB) * 256;
    if (root.reserveBlocks >= partition || headBlocks > partition - root.reserveBlocks ||
        partition - root.reserveBlocks - headBlocks < imageBlocks + 1024) {
      rc = DF_ERR_LAYOUT;
      reason = "head overlaps the root image";
      goto done;
    }
    head = root.last + 1 - root.reserveBlocks - headBlocks;
  }
  slot = static_cast<uint8_t *> (IOMallocData (static_cast<size_t> (DF_SLOT_BLOCKS) * DF_BLOCK));
  if (!slot) { reason = "no memory"; goto done; }
  rc = dfLoadDescriptor (gDataFileIo, root.first, slot, &h, dataFileSha, &gDataFileSlot, &gDataFileNewest);
  gDataFileUnixnuFirst = root.first;
  if (rc) { reason = rc == DF_ERR_MAGIC ? "no descriptor" : rc == DF_ERR_REVOKED ? "descriptor revoked" : "descriptor"; goto done; }
  gDataFileDiag.slot = static_cast<uint32_t> (gDataFileSlot);
  gDataFileDiag.generation = h.generation;
  gDataFileDiag.extents = h.extent_count;
  gDataFileDiag.segments = h.segment_count;
  need = dfStoreMemoryBytes (h.extent_count, h.segment_count);
  gDataFileMemory = static_cast<uint8_t *> (IOMallocData (static_cast<size_t> (need)));
  if (!gDataFileMemory) { reason = "no memory"; goto done; }
  gDataFileMemoryBytes = need;
  {
    // A self-test needs write access: it opens read-write (an ACTIVE descriptor only) and downgrades to the mode afterwards.
    const bool test = c.selftest && h.state == DF_STATE_ACTIVE;
    rc = gDataFile.open (gDataFileIo, h, slot + DF_BLOCK, gDataFileMemory, need, gptFirst, gptLast, head, headBlocks, blocks,
                         test ? 2 : c.mode, c.eager != 0, dataFileSha, dataFileClock, c.verify != 0);
    if (!rc && test) {
      rc = gDataFile.selfTest (c.selftest);
      if (rc == DF_ERR_CAPACITY) rc = DF_OK;   // the window fills the store: no spare block to test, nothing written
      if (!rc && c.mode != 2) gDataFile.downgrade ();
    }
  }
  gDataFileDiag.capacity = gDataFile.opened ? gDataFile.capacity () : 0;
  gDataFileDiag.window = blocks;
  if (rc) {
    reason = rc == DF_ERR_CANARY ? "canary gone" : rc == DF_ERR_PARTITION ? "userdata moved" : rc == DF_ERR_CAPACITY ? "window too large"
           : rc == DF_ERR_READONLY ? "descriptor read-only" : rc == DF_ERR_VERIFY ? "self-test read-back differs" : "store refused";
    if (rc == DF_ERR_CANARY) dataFileRevoke ();
    gDataFile.opened = false;
  }
done:
  if (slot) IOFreeData (slot, static_cast<size_t> (DF_SLOT_BLOCKS) * DF_BLOCK);
  if (reason && gDataFileMemory) {
    IOFreeData (gDataFileMemory, static_cast<size_t> (gDataFileMemoryBytes));
    gDataFileMemory = nullptr;
    gDataFileMemoryBytes = 0;
  }
  dataFileDiagUpdate ();
  if (reason) {
    gDataFileDiag.state = c.mode ? 3 : 0;
    gDataFileDiag.error = static_cast<uint32_t> (rc ? rc : DF_ERR_ARG);
  }
  if (why) *why = reason ? reason : "";
  if (!reason && firstLba) {
    uint64_t lba = 0, segment = 0;
    uint32_t run = 0;
    *firstLba = gDataFile.map (0, &lba, &run, &segment) ? lba : 0;
  }
  return reason ? 0 : 1;
}

static inline int dataFileStoreRead (uint64_t rel, uint32_t blocks, void *dst)
{
  const bool ok = gDataFile.read (rel, blocks, dst);
  if (!ok && gDataFile.error == DF_ERR_CANARY) dataFileRevoke ();
  dataFileDiagUpdate ();
  return ok;
}

static inline int dataFileStoreWrite (uint64_t rel, uint32_t blocks, const void *src)
{
  const bool ok = gDataFile.write (rel, blocks, src);
  if (!ok && gDataFile.error == DF_ERR_CANARY) dataFileRevoke ();
  dataFileDiagUpdate ();
  return ok;
}

static inline int dataFileStoreSync () { return gDataFile.sync (); }

// Host tests: forget the store (the driver keeps it for the whole boot).
static inline void dataFileClose ()
{
  if (gDataFileMemory) IOFreeData (gDataFileMemory, static_cast<size_t> (gDataFileMemoryBytes));
  gDataFileMemory = nullptr;
  gDataFileMemoryBytes = gDataFileUnixnuFirst = gDataFileNewest = gDataFileRevokeLba = 0;
  gDataFileSlot = 0;
  gDataFileRevokeTried = false;
  gDataFile.opened = gDataFile.writable = gDataFile.poisoned = false;
  gDataFile.error = DF_ERR_ARG;
  gDataFileDiag = DataFileDiag ();
}
