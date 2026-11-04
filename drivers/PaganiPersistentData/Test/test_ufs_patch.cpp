// SPDX-License-Identifier: BSD-2-Clause-Patent
// integration/NativeUfs.patch on the host: NativeUfs's real command path (RootCommands.h with the patch, through the tests'
// RootCommandsBridge.cpp, which replaces only MMIO, DMA sync, clock and locks) plus DataFileUfs.h, over a device model that executes
// READ(10), WRITE(10) and SYNCHRONIZE CACHE(10) against a simulated LUN 0. Run.sh copies NativeUfs and this directory to a temporary
// tree, applies the patch there and builds this file in NativeUfs/tests/ with -DNATIVE_UFS_READ_ONLY=1 -DNATIVE_UFS_DATA_FILE=1
// (and once more with -DNATIVE_UFS_SM8750=1). Checked: the read-only build still refuses every WRITE while no store is armed;
// with an armed store every WRITE(10) carries FUA and lands only on verified data blocks of the file; UNMAP stays refused; a
// poisoned store disarms the command filter again.
#include "RootCommandsBridge.cpp"
#include <array>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../PaganiPersistentData/DfSha256.h"

// ---- what NativeUfs.cpp has around the glue: PartitionTable.h's lookup, boot arguments, allocation, libkern's SHA-256 -----------
struct UfsPartition { uint32_t lun; uint64_t first, blocks; };
enum : int { UfsPartOk = 0, UfsPartNotFound = -5 };
static constexpr uint32_t kUfsPartScratch = 4096 + 128 * 128;
struct SHA256_CTX { df_sha256_ctx c; };
static void SHA256_Init (SHA256_CTX *x) { df_sha256_init (&x->c); }
static void SHA256_Update (SHA256_CTX *x, const void *p, size_t n) { df_sha256_update (&x->c, p, n); }
static void SHA256_Final (uint8_t *out, SHA256_CTX *x) { df_sha256_final (&x->c, out); }
static std::map<std::string, uint32_t> bootArgs;
static bool PE_parse_boot_argn (const char *name, void *value, int size)
{
  auto it = bootArgs.find (name);
  if (it == bootArgs.end () || size != 4) return false;
  memcpy (value, &it->second, 4);
  return true;
}
static void *IOMallocData (size_t n) { void *p = malloc (n); if (p) memset (p, 0xA5, n); return p; }
static void IOFreeData (void *p, size_t) { free (p); }
static const uint64_t kUserFirst = 4020972, kUserLast = 107409151, kUnixnuFirst = 107409152, kUnixnuLast = 124186367;
static const uint64_t kFsMain = 297984, kFsBlocks = 103387919;
static int ufsPartFind (const char *name, UfsPartition *out, uint8_t *scratch)
{
  memset (scratch, 0, kUfsPartScratch);
  if (strcmp (name, "userdata")) return UfsPartNotFound;
  *out = {0, kUserFirst, kUserLast - kUserFirst + 1};
  return UfsPartOk;
}
#include "../../PaganiPersistentData/DataFileBuild.h"
#include "../../PaganiPersistentData/DataFileUfs.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- the device: one request slot, UPIU in, UPIU out, data through the DMA area ---------------------------------------------------
static std::array<uint8_t, kDmaBytes> device;
static std::array<uint32_t, 256> registers;
static uint64_t now;
static bool released;
static std::unordered_map<uint64_t, std::array<uint8_t, 4096>> medium;
struct Command { uint8_t opcode, flags; uint64_t lba; uint32_t blocks; };
static std::vector<Command> commands;
static void stale (uint64_t lba, uint8_t *out) { std::mt19937_64 r (lba + 11); for (unsigned i = 0; i < 4096; i += 8) { uint64_t v = r (); memcpy (out + i, &v, 8); } }
static uint32_t be32 (const uint8_t *p) { return uint32_t (p[0]) << 24 | uint32_t (p[1]) << 16 | uint32_t (p[2]) << 8 | p[3]; }

static uint64_t io (unsigned op, uint64_t address, uint64_t value, void *bytes)
{
  if (op == 0) return registers.at (address / 4);
  if (op == 1) {
    if (address == 0x20) registers[address / 4] &= ~uint32_t (value);
    else registers.at (address / 4) = uint32_t (value);
    if (address == 0x58) {
      assert (released);
      released = false;
      registers[0x58 / 4] = 0;
      uint8_t *cmd = device.data () + kUcdOffset, *rsp = cmd + kRspOffset, *data = device.data () + kDataOffset;
      if (cmd[0] == 1) {   // COMMAND UPIU: execute the CDB on LUN 0
        const uint8_t *cdb = cmd + 16;
        Command c = {cdb[0], cdb[1], be32 (cdb + 2), uint32_t (cdb[7]) << 8 | cdb[8]};
        assert (cmd[2] == 0);
        commands.push_back (c);
        if (c.opcode == 0x28)
          for (uint32_t i = 0; i < c.blocks; ++i) {
            auto it = medium.find (c.lba + i);
            if (it == medium.end ()) stale (c.lba + i, data + i * 4096ULL); else memcpy (data + i * 4096ULL, it->second.data (), 4096);
          }
        if (c.opcode == 0x2a)
          for (uint32_t i = 0; i < c.blocks; ++i) memcpy (medium[c.lba + i].data (), data + i * 4096ULL, 4096);
      }
      std::fill (rsp, rsp + 512, 0);
      *reinterpret_cast<uint32_t *> (device.data () + 8) = 0;
      memcpy (rsp, cmd, 32);
      rsp[0] = cmd[0] + 0x20;
      rsp[1] = 0;
      if (cmd[0] == 1) putBe32 (rsp + 12, 0);
    }
    return 0;
  }
  if (op == 2 || op == 3) {
    assert (address >= gHost.phys && address - gHost.phys + value <= device.size ());
    auto at = device.data () + (address - gHost.phys);
    if (op == 2) { memcpy (at, bytes, value); released = true; }
    else memcpy (bytes, at, value);
  }
  if (op == 4) return now++;
  if (op == 5) now += value;
  return 0;
}
static void resetHost ()
{
  device.fill (0);
  registers.fill (0);
  registers[0x34 / 4] = 1;
  registers[0x30 / 4] = 15;
  registers[0x60 / 4] = 1;
  ufs_test_reset (io, 0x8cb5e9000);
  commands.clear ();
}
static uint64_t seg (uint64_t k) { return kFsMain + k * DF_SEG_BLOCKS; }

int main ()
{
  // The Android helper's work, raw: canaries in the file's segments, the descriptor in unixnu slot A.
  std::vector<df_extent> e = {{seg (3000), 2, 0}, {seg (100), 1, 0}, {seg (64000), 3, 0}};
  df_header h;
  memset (&h, 0, sizeof h);
  h.state = DF_STATE_ACTIVE;
  h.generation = 1;
  for (int i = 0; i < 16; ++i) h.file_id[i] = (uint8_t) (i * 9 + 1);
  for (int i = 0; i < 32; ++i) h.nonce[i] = (uint8_t) (i * 13 + 5);
  h.part_first = kUserFirst;
  h.part_last = kUserLast;
  h.fs_main = kFsMain;
  h.fs_blocks = kFsBlocks;
  std::vector<uint8_t> slot ((size_t) DF_SLOT_BLOCKS * DF_BLOCK);
  const uint32_t slotBlocks = df_build_slot (&h, e.data (), (uint32_t) e.size (), slot.data (), df_sha256);
  for (uint32_t b = 0; b < slotBlocks; ++b) memcpy (medium[kUnixnuFirst + DF_SLOT_A + b].data (), &slot[(size_t) b * DF_BLOCK], DF_BLOCK);
  uint64_t index = 0;
  for (auto &x : e)
    for (uint32_t s = 0; s < x.segments; ++s, ++index)
      df_canary_block (&h, index, x.first_rel + s * 512ULL, medium[kUserFirst + x.first_rel + s * 512ULL].data (), df_sha256);
  const uint64_t dataLba = kUserFirst + seg (3000) + 1, canaryLba = kUserFirst + seg (3000);
  std::vector<uint8_t> buf (32 * 4096, 0x5c);
  uint32_t scsi = 0;
  uint8_t cdb[10] = {};

  // Nothing armed: the read-only build refuses WRITE(10) and SYNCHRONIZE CACHE(10), hostWrite4k refuses before any command.
  resetHost ();
  CHECK (hostWrite4k (0, dataLba, 1, buf.data (), true) == kIOReturnBadArgument);
  cdb[0] = 0x2a;
  CHECK (hostScsiDir (0, cdb, 10, 1, 4096, 200000, &scsi) == kIOReturnBadArgument);
  CHECK (hostSyncCache () == kIOReturnNotReady && commands.empty ());

  // Mode 2: the glue reads the descriptor and canaries with real READ(10)s, then writes through real WRITE(10)s.
  bootArgs = {{"pagani-data-file", 2}};
  dataFileConfigLoad ();
  const DataFileRoot root = {kUnixnuFirst, kUnixnuLast, 9384755200ULL, 0x100000, 262144};
  const char *why = nullptr;
  uint64_t first = 0;
  const uint64_t window = 1 + 1 + 3000;                          // a 3000-page disk (PersistOverlay windowBlocks) in 6 * 511 blocks
  CHECK (dataFileStoreOpen (window, root, &first, &why) == 1 && first == dataLba && gDataFileDiag.state == 2);
  for (auto &c : commands) CHECK (c.opcode == 0x28);
  commands.clear ();
  CHECK (dataFileStoreWrite (0, 32, buf.data ()) && dataFileStoreWrite (500, 20, buf.data ()) && dataFileStoreSync ());
  std::vector<uint8_t> back (32 * 4096);
  CHECK (dataFileStoreRead (0, 32, back.data ()) && !memcmp (back.data (), buf.data (), back.size ()));
  unsigned writes = 0, syncs = 0, bad = 0;
  for (auto &c : commands) {
    if (c.opcode == 0x2a) {
      ++writes;
      bool ok = (c.flags & 0x08) != 0;                           // FUA
      for (uint32_t i = 0; i < c.blocks && ok; ++i) {
        const uint64_t lba = c.lba + i;
        bool inFile = false;
        for (auto &x : e)
          for (uint32_t s = 0; s < x.segments; ++s) {
            const uint64_t a = kUserFirst + x.first_rel + s * 512ULL;
            inFile |= lba > a && lba < a + 512;
          }
        ok = inFile;
      }
      bad += !ok;
    }
    syncs += c.opcode == 0x35;
    bad += c.opcode != 0x2a && c.opcode != 0x28 && c.opcode != 0x35;
  }
  CHECK (writes == 3 && syncs == 1 && bad == 0);                 // 32 blocks, then 20 split at a segment end (11 + 9)
  // The gate below the glue: a canary block, a block outside the file, and UNMAP are refused even while the store is armed.
  commands.clear ();
  CHECK (hostWrite4k (0, canaryLba, 1, buf.data (), true) == kIOReturnBadArgument);
  CHECK (hostWrite4k (0, kUserFirst + seg (5000) + 1, 1, buf.data (), true) == kIOReturnBadArgument);
  CHECK (hostWrite4k (0, kUnixnuFirst + DF_SLOT_B, 1, buf.data (), true) == kIOReturnBadArgument);
  cdb[0] = 0x42;
  CHECK (hostScsiDir (0, cdb, 10, 0, 0, 200000, &scsi) == kIOReturnBadArgument && commands.empty ());
  // A canary gone: the store is poisoned on first touch, XNU writes the REVOKED header into slot B (one WRITE(10)), and the filter
  // refuses WRITE(10) again.
  medium[kUserFirst + seg (64002)].fill (0);
  CHECK (!dataFileStoreWrite (5 * 511 + 3, 1, buf.data ()) && gDataFileDiag.state == 4 && gDataFileDiag.revokedGeneration == 2);
  unsigned slotWrites = 0, others = 0;
  for (auto &c : commands) {
    if (c.opcode != 0x2a) continue;
    if (c.lba == kUnixnuFirst + DF_SLOT_B && c.blocks == 1 && (c.flags & 0x08)) ++slotWrites; else ++others;
  }
  CHECK (slotWrites == 1 && others == 0 && !dataFileWritesArmed ());
  cdb[0] = 0x2a;
  CHECK (hostScsiDir (0, cdb, 10, 1, 4096, 200000, &scsi) == kIOReturnBadArgument && hostSyncCache () == kIOReturnNotReady);
  df_header revoked;
  CHECK (df_header_decode (medium[kUnixnuFirst + DF_SLOT_B].data (), &revoked, df_sha256) == DF_OK && revoked.state == DF_STATE_REVOKED &&
         revoked.generation == 2 && !memcmp (revoked.file_id, h.file_id, 16));
  dataFileClose ();
  printf ("%s: %d checks, %d failures (patched RootCommands.h%s)\n", failures ? "FAIL" : "PASS", checks, failures,
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
          ", SM8750"
#else
          ""
#endif
  );
  return failures ? 1 : 0;
}
