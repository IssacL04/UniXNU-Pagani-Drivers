// SPDX-License-Identifier: BSD-2-Clause-Patent
// The diagnostic log window of NativeUfs for the log writer (BootProgress UfsLog.c; the ABI of UniXNU-sheng's NativeUfs and BootLog.c, plus
// the close and the panic path). Blocks are 4096 bytes, `rel` counts from the start of the log window or of the export region. Every
// function but the panic path's is for thread context and returns 1 on success, 0 otherwise. Callers that may be linked without NativeUfs
// declare these weak.
//  - native_ufs_log_open: opens the window once (sheng-log=1): its first absolute LBA and its size. On a refusal *why is the reason, a
//    static string; "no root layout" means not yet.
//  - native_ufs_log_write, native_ufs_export_write: write inside the log window or the export region; native_ufs_log_sync flushes.
//  - native_ufs_export_blocks: the export region's size once the log is open, else 0.
//  - native_ufs_log_close: nothing writes after this, and the window never opens again.
//  - native_ufs_store_gpt_check: 0 while the GPT is what the root start read; anything else: it changed, or could not be read.
//  - native_ufs_log_panic: from a panic or a reset, interrupts masked, nothing may sleep: claims the transport for good, trying for at most
//    waitUs; with the claim nothing is on the wire. After a 1 the same CPU may use native_ufs_log_panic_write and native_ufs_log_panic_sync,
//    each bounded by its own timeout; a command that outlives it ends the transport (quarantine) and returns 0. The claim is never released:
//    the reset comes next.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why);
int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src);
uint64_t native_ufs_export_blocks(void);
int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src);
int native_ufs_log_sync(void);
void native_ufs_log_close(void);
unsigned native_ufs_store_gpt_check(void);
int native_ufs_log_panic(uint32_t waitUs);
int native_ufs_log_panic_write(uint64_t rel, uint32_t blocks, const void *src, uint32_t timeoutUs);
int native_ufs_log_panic_sync(uint32_t timeoutUs);
#ifdef __cplusplus
}
#endif
