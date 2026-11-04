// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Read-only partition access of NativeUfs for other kernel drivers (implementation: PartitionTable.h). The ABI of UniXNU-sheng's
// NativeUfs: 1 on success, 0 otherwise. Callers that may be linked without NativeUfs define weak defaults returning 0.
//  - native_ufs_part_find: the partition named `name` (GPT, UTF-16LE, exact) on any LUN: its LUN, first 4 KiB block and block count.
//    0 while the UFS host is not up yet, when no LUN has the name or more than one entry has it.
//  - native_ufs_part_read: `blocks` 4 KiB blocks from `lba` of the LUN into dst (kernel memory), never past the LUN's capacity;
//    serialized with the root volume's reads.
//  - native_ufs_part_status: why the last call failed (a UfsPart* code of PartitionTable.h; 0 after a success), for logs.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
int native_ufs_part_status(void);
#ifdef __cplusplus
}
#endif
