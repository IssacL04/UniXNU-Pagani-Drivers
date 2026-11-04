// SPDX-License-Identifier: BSD-2-Clause-Patent
// The checks that stand between the diagnostic log (sheng-log=1) and the medium, without the kernel, so tests/LogWindow.cpp runs the
// same code: where the window goes, what must already be there, and the panic path's bounded claim of the transport.
//
// The log window is the last kLogReserveBlocks of the root partition and the export region the kExportBlocks below them. Authorized on
// the OnePlus 13T for the `unixnu` partition only (2026-10-31); everything else stays read-only.
#pragma once
constexpr uint64_t kLogReserveBlocks = 8192;      // 32 MiB
constexpr uint64_t kExportBlocks = 65536;         // 256 MiB
static constexpr char kLogPartition[] = "unixnu";

struct UfsLogPlan {
  const char *refusal;    // nullptr: the window fits
  uint64_t logFirst;      // absolute LBA of the log window
  uint64_t exportFirst;   // absolute LBA of the export region
};

// A window inside the partition, clear of the flashed root image plus a 4 MiB guard. `name` is the partition the root layout came from.
static UfsLogPlan ufsLogPlan (const char *name, uint64_t first, uint64_t last, uint64_t imageBytes, uint64_t reserveBlocks)
{
  UfsLogPlan plan = { nullptr, 0, 0 };
  unsigned index = 0;
  while (name && kLogPartition[index] && name[index] == kLogPartition[index]) ++index;
  if (!name || kLogPartition[index] || name[index]) {
    plan.refusal = "root partition is not unixnu";
    return plan;
  }
  if (last < first || last == UINT64_MAX) {
    plan.refusal = "partition bounds";
    return plan;
  }
  const uint64_t partition = last - first + 1, imageBlocks = (imageBytes / 4096) + ((imageBytes & 4095) ? 1 : 0);
  if (reserveBlocks < kLogReserveBlocks + kExportBlocks) plan.refusal = "reserve too small for the log";
  else if (reserveBlocks >= partition || partition - reserveBlocks < imageBlocks + 1024) plan.refusal = "overlaps the root image";
  else {
    plan.logFirst = last + 1 - kLogReserveBlocks;
    plan.exportFirst = plan.logFirst - kExportBlocks;
  }
  return plan;
}

// The magics the window's own writer leaves at the start of a block (BootProgress/UfsLog.c, AdrenoMetal CacheExport.h).
static bool ufsLogBlockIs (const uint8_t *block, const char *magic)
{
  for (unsigned index = 0; index < 8; ++index) if (block[index] != static_cast<uint8_t> (magic[index])) return false;
  return true;
}

static bool ufsLogBlockZero (const uint8_t *block)
{
  uint8_t any = 0;
  for (unsigned index = 0; index < 4096; ++index) any |= block[index];
  return !any;
}

// Before anything is written: the first block of the log window and of the export region must be blank or ours, the last block of each
// blank. A window at the wrong place (wrong geometry, a table that moved) meets someone else's data here, and nothing is opened. `read (lba,
// block)` reads one 4096-byte block through the read-only path and returns true when it did. The last block of each is never written
// (UfsLog.c stops one block short), so a magic there is an old window at another place, not ours. Returns the reason for a refusal, nullptr
// to go on.
template <class Read>
static const char *ufsLogContentGate (uint64_t logFirst, uint64_t exportFirst, uint8_t *block, Read &&read)
{
  if (!read (logFirst, block)) return "content gate: log window first block unreadable";
  if (!ufsLogBlockZero (block) && !ufsLogBlockIs (block, "PGLOG001")) return "content gate: foreign data at the log window's first block";
  if (!read (logFirst + kLogReserveBlocks - 1, block)) return "content gate: log window last block unreadable";
  if (!ufsLogBlockZero (block)) return "content gate: foreign data at the log window's last block";
  if (!read (exportFirst, block)) return "content gate: export region first block unreadable";
  if (!ufsLogBlockZero (block) && !ufsLogBlockIs (block, "SHEXR001")) return "content gate: foreign data at the export region's first block";
  if (!read (exportFirst + kExportBlocks - 1, block)) return "content gate: export region last block unreadable";
  if (!ufsLogBlockZero (block)) return "content gate: foreign data at the export region's last block";
  return nullptr;
}

// The whole of opening: the plan, the content gate, then `open (first, blocks)` (hostLogOpen) for one window over the export region and the
// log. Returns the refusal, nullptr with the log window's first LBA in *logFirst.
template <class Read, class Open>
static const char *ufsLogOpenWindow (const char *name, uint64_t first, uint64_t last, uint64_t imageBytes, uint64_t reserveBlocks,
                                     uint8_t *block, Read &&read, Open &&open, uint64_t *logFirst)
{
  const UfsLogPlan plan = ufsLogPlan (name, first, last, imageBytes, reserveBlocks);
  if (plan.refusal) return plan.refusal;
  if (const char *foreign = ufsLogContentGate (plan.logFirst, plan.exportFirst, block, read)) return foreign;
  if (!open (plan.exportFirst, kExportBlocks + kLogReserveBlocks)) return "window refused";
  *logFirst = plan.logFirst;
  return nullptr;
}

// The panic path claims the transport: a try-lock in a loop that ends at the deadline, whatever the lock's owner does. `tryLock` is a
// single compare-and-swap (lck_mtx_try_lock takes no interlock, sleeps never and has no interrupt check), so this runs with interrupts
// masked; a holder that never lets go costs the time and nothing else.
template <class TryLock, class Expired, class Pause>
static bool ufsLogClaim (TryLock &&tryLock, Expired &&expired, Pause &&pause)
{
  for (;;) {
    if (tryLock ()) return true;
    if (expired ()) return false;
    pause ();
  }
}
