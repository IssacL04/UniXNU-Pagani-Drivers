# PaganiPersistentData

Persistent iOS Data on the OnePlus 13T without touching the partition table: a pinned, fully allocated f2fs file in Android's
`/data` (created by the Android helper, `工具/persistent-data/unixnu-datafile.c`) whose UFS blocks XNU reads and writes directly,
never through f2fs or Android's encryption. NativeRamBlock's existing persistent overlay (`PersistOverlay.h`, `DirectDisk.h`) runs
on top unchanged; only the provider behind `native_ufs_store_*` changes.

Current state: design and host prototype. Nothing here is built into the kernel yet; `integration/*.patch` are proposals.

- `DataFileFormat.h`: the descriptor (two A/B slots in the padding of the `unixnu` root partition, blocks 16..255) with the file's
  extent table, the per-segment canary block (block 0 of every 2 MiB f2fs segment), the write self-test block, disk sizing.
- `DataFileBuild.h`: writer side (FIEMAP extents to segment extents, slot image), for the Android helper and the tests.
- `DataFileStore.h`: XNU side: descriptor loader, window to LBA map (head region + data blocks of the segments), canary checks
  before first use, the write gate NativeUfs's `hostWrite4k` asks for every WRITE(10), optional read-back of every write, self-test.
- `DataFileUfs.h`: NativeUfs glue (boot arguments `pagani-data-file*`, userdata lookup, head placement, REVOKED header on canary
  loss), included inside NativeUfs's transport namespace by `integration/NativeUfs.patch`.
- `BigVolatile.h`: RAM layer for a big (> 2 GiB) Data disk without a read-write store (fallback, read-only verification), used by
  `integration/NativeRamBlock.patch`.
- `DfSha256.h`: SHA-256 for the Android helper and the tests (XNU passes libkern's).

Boot arguments (with `-DNATIVE_UFS_DATA_FILE=1`): `pagani-data-file=0|1|2` (off, read-only, read-write), `pagani-data-file-eager`,
`pagani-data-file-verify` (read back every write, default 1), `pagani-data-file-selftest=N`, `pagani-data-file-head=MiB`,
`pagani-data-file-revoke`; NativeRamBlock: `pagani-data-volatile-mib` (default 1024).

Tests: `Test/run.sh` (ASan/UBSan): format, slots, FIEMAP conversion, map and gate against brute force, canary loss, read-back faults,
self-test, three-boot end-to-end run of NativeRamBlock's overlay, the simulated 128 GiB allocation of the PKX110 f2fs snapshot, the
glue over a mock transport, the NativeUfs patch on the real RootCommands.h command path over a device model (plain and SM8750), the
unchanged NativeUfs command test with the patched header, a kernel-style compile, a syntax check of the patched NativeRamBlock.cpp
against declaration stubs (`Test/kstub`), and `git apply --check` of both patches.
