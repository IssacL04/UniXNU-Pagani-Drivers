# NativeUfs

Native UFS storage driver. Owns the host controller, SMMU, queues and the root read path.

Current state: the native UFS root works on sheng.

Linked into XNU by `unixnu build kernel`.

Solid state: `declareSolidState` (here and in NativeRamBlock, before `registerService`) sets `Medium Type` = `Solid State` in the device's
`Device Characteristics`. `IOBlockStorageDriver::handleStart` reads it from its provider and `DKIOCISSOLIDSTATE` from the parents of the
media, an APFS volume's too; the answer sets `MNTK_SSD` and the device's throttle table. All it changes on iOS, as read in XNU and not
measured: the I/O throttle periods (5/15/25 ms instead of 40/85/200); in `vfs_cluster.c` the I/O size limit of a throttled window doubles,
its asynchronous count goes from 0 to `MIN(mnt_ioscale, 4)`, an asynchronous write's scale from 6 to 2 (at the largest cluster 1 I/O in
flight instead of 5; UFS refuses writes, so RAM Data only), and a speculative read-ahead stops at 512 KiB per request; the phantom cache's
thrashing threshold goes from 500 to 1000; a swap file keeps the SSD compressor thresholds. No difference on iOS: `PREFETCH`,
`WRITE_BEHIND`, `vm_object_cluster_size` (macOS branch), the UPL write throttle (`vm_pageout.c`, always SSD). Left aside: `rootvp_is_ssd`
(only a pivot image) and `kern_symfile.c` (polled files). APFS reads `Device Characteristics` in two places, both with null checks, for its
statistics strings and for Target Disk Mode. It also asks `DKIOCISSOLIDSTATE` itself: `dev_init` keeps the answer in its device handle and logs
`solidstate`, and the one reader, `tx_leave`, takes a transaction threshold of 1024 for a solid state device against 256 (read from the 21H461
kernelcache, APFS 2236.140.6): a writable container, so Data, makes larger transactions; the read-only ones do not care. That thread of APFS sends
no TRIM here: its only `DKIOCUNMAP` is behind `DK_FEATURE_UNMAP`, which this device never reports (`features 0` on the phone), and `vnode_isonssd`
has no importer among the kexts. Test: `check_solid_state` in `test-stage6.py` of the device repository.

Read-only partition access for other drivers (`NativeUfsPart.h`, `PartitionTable.h`): `native_ufs_part_find` finds a partition by
its GPT name on any LUN READ CAPACITY found, `native_ufs_part_read` reads 4 KiB blocks inside a LUN's capacity through the same
`hostRead4k` (and `gHost.lock`) as the root path, `native_ufs_part_status` keeps the reason of the last refusal. Nothing is written.
PaganiAdsp reads the ADSP firmware from modem_a (LUN 4, block 4870) this way. Test: `tests/PartitionTable.cpp` (in
`check_root_path_host.py`; `--gpt-dir` adds the PKX110 GPT dumps).

Diagnostic log window (`NativeUfsLog.h`, `LogInterface.h`, `LogWindow.h`, boot argument `sheng-log=1`): the kernel is built with
`NATIVE_UFS_READ_ONLY=1` and `NATIVE_UFS_LOG_WRITES=1`, so the root path stays read-only and the Data store's window never opens, while the log window
may: the last 32 MiB of the `unixnu` partition and the 256 MiB below them. It opens only clear of the root image plus a 4 MiB guard, and only when
the first block of both is blank or the log's own and the last block of both is blank (the content gate, `ufsLogContentGate`). `hostScsiDir` then lets
exactly a plain WRITE(10) inside the window, read back from the CDB, and SYNCHRONIZE CACHE(10) through on LUN 0; every other write command is refused.
`native_ufs_log_close` ends it. While the window is open a read gets a write's timeout, as one that outlives a second behind the device's cache
would end the root volume. A panic and every reset (`native_ufs_log_panic`) take `gHost.lock` with a bounded try-lock and keep it: `hostSubmit` never
sleeps, only that lock could block, and with it nothing of the log is on the wire when the reset lands. Tests: `tests/LogWindow.cpp`,
`tests/RootCommands.cpp` and `tests/LogInterface.cpp` (the interface, and `BootProgress/UfsLog.c` over it), in `check_root_path_host.py` with the kernel's
flags.
