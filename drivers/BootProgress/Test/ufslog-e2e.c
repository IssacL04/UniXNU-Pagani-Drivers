// SPDX-License-Identifier: MIT
// The writer side of NativeUfs/tests/LogInterface.cpp: the actual UfsLog.c on the actual RamoopsLog.c, with the platform of the host tests and
// NativeUfs's log interface left to be linked in, so a run goes from a record down to the blocks of a model LUN through the real functions.
#define RAMOOPS_LOG_TEST 1
#define UFS_LOG_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../RamoopsLog.c"
#include "../UfsLog.c"
#include "ufslog-platform.h"

void *ufs_log_platform_lock_alloc(void) { static int lock; return &lock; }
void ufs_log_platform_lock(void *lock) { (void)lock; }
void ufs_log_platform_unlock(void *lock) { (void)lock; }
void ufs_log_platform_pause(void) {}

void e2e_boot(int reset_after);
int e2e_open(void);
unsigned e2e_tick(void);
void e2e_emit(unsigned records);
int e2e_export(uint32_t generation, uint32_t total, const void *data);
void e2e_restart(void);
void e2e_panic(void);
int64_t e2e_resets(void);

void e2e_boot(int reset_after)
{
    ramoops_log_suspend();
    memset(region, 0, sizeof(region));
    memset(&w, 0, sizeof(w));
    memset(headers, 0, sizeof(headers));
    live = NULL;
    writer_stopped = writer_started = panic_flushed = reset_coming = 0;
    last_state = LAST_IDLE;
    export_lock = NULL;
    export_stage = NULL;
    export_region = export_total = export_filled = export_generation = export_target = 0;
    export_ready = export_ready_bytes = export_ready_first = export_named = 0;
    thread_starts = 0;
    psci_resets = 0;
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    arg("pagani-panic-reset", 1);
    if (reset_after) arg("pagani-reset-after", (uint64_t)reset_after);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ufs_log_start();
    assert(thread_starts == 1);
}

int e2e_open(void) { return ufs_log_writer_open(); }

unsigned e2e_tick(void)
{
    clock_now += 3 * (uint64_t)HZ + HZ / 100;
    return ufs_log_writer_tick();
}

void e2e_emit(unsigned records)
{
    while (records--) ramoops_log_line('U', "a record on its way to the model LUN");
}

int e2e_export(uint32_t generation, uint32_t total, const void *data)
{
    int status = 0;
    for (uint32_t offset = 0; !status && offset < total;) {
        const uint32_t n = total - offset < 3968 ? total - offset : 3968;
        status = boot_log_export_put(generation, total, offset, (const uint8_t *)data + offset, n);
        offset += n;
    }
    return status;
}

void e2e_restart(void) { ramoops_log_restart("restart"); }

void e2e_panic(void)
{
    ramoops_log_panic_reset();
}

int64_t e2e_resets(void) { return psci_resets; }
