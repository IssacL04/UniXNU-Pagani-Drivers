// SPDX-License-Identifier: MIT
// Host test of the UFS log (UfsLog.c, the actual writer on the actual RamoopsLog.c) over a model of NativeUfs's log interface: off without
// sheng-log, the wait for the window and a refusal, a round's order (slot, header, flush; a flush before the header only for a new export),
// `live` moving only after that, nothing written without new records, changed blocks only, slot advance and the pinned slots and ring, that
// neighbouring slots lose nothing, burst rounds and the last seconds before a timed reset, the Power key, failed rounds, the GPT stop, the statistics, the
// message buffer, the export's two halves and what a power cut at any command leaves of them, the panic path's order, budget and refusals, with
// the panic reset keeping its time, the last snapshot's switch, and what every reset does first. Threads: ufslog-threads.c.
//   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined ufslog.c -o ufslog && ./ufslog
//   ./ufslog --dump DIR    also writes the windows of a few runs for the reader's test (工具/gpu-test-loop/tests/test_ufslog.py)
#define RAMOOPS_LOG_TEST 1
#define UFS_LOG_TEST 1
#define _DARWIN_C_SOURCE 1                  // memmem
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../RamoopsLog.c"
#include "../UfsLog.c"

enum { WINDOW_BLOCKS = 8192, EXPORT_BLOCKS = 65536, SLOTS = 509 };

#include "ufslog-platform.h"

// ---- the export lock ------------------------------------------------------------------------------------------------------
static unsigned locks_held;
static void (*unlock_hook)(void);           // runs after the export lock is let go for the unlock_skip'th time from now
static unsigned unlock_skip;
void *ufs_log_platform_lock_alloc(void) { static int lock; return &lock; }
void ufs_log_platform_lock(void *lock) { (void)lock; assert(!locks_held); locks_held++; }
void ufs_log_platform_unlock(void *lock)
{
    (void)lock;
    assert(locks_held);
    locks_held--;
    if (unlock_hook && !unlock_skip--) { void (*hook)(void) = unlock_hook; unlock_hook = NULL; hook(); }
}
void ufs_log_platform_pause(void) {}

// ---- the model of NativeUfs ---------------------------------------------------------------------------------------------------
struct op { char kind; uint64_t rel; uint32_t blocks; uint32_t timeout; };
static struct op ops[1 << 20];
static unsigned op_count;
static uint8_t *window, *export_area;
static int open_state;                      // 0 opens, 1 "no root layout", 2 refused
static int model_open, model_closed, claim_refused, panic_claimed;
static unsigned fail_writes, fail_syncs, fail_panic_write;
static unsigned gpt_result, gpt_checks;
static uint64_t op_ticks;                   // what a command takes
static uint64_t sync_ticks;                 // and a flush, when it is not the same
static uint64_t model_export_blocks = EXPORT_BLOCKS;
static unsigned claim_refusals;             // the next claims that find the transport busy
static void (*sync_hook)(void);             // runs inside the next flush, as another thread would
static void (*claim_hook)(void);            // runs in the next claim, as the other CPUs would

// With `cached` on the device has a cache: writes wait in `pending` for a flush, in any order for all anyone knows, and check_cut asks at every
// command what a power cut there could leave.
struct pending { int export; uint64_t rel; uint32_t blocks; uint8_t *data; };
static struct pending pendings[512];
static unsigned pending_count, cuts_checked;
static int cached;
static void check_cut(void);

static void store(int export, uint64_t rel, uint32_t blocks, const void *src)
{
    if (!cached) {
        memcpy((export ? export_area : window) + rel * 4096, src, blocks * 4096UL);
        return;
    }
    assert(pending_count < sizeof(pendings) / sizeof(pendings[0]));
    uint8_t *copy = malloc(blocks * 4096UL);
    assert(copy);
    memcpy(copy, src, blocks * 4096UL);
    pendings[pending_count++] = (struct pending){export, rel, blocks, copy};
    check_cut();
}

static void flush_pending(void)
{
    for (unsigned i = 0; i < pending_count; i++) {
        const struct pending *e = &pendings[i];
        memcpy((e->export ? export_area : window) + e->rel * 4096, e->data, e->blocks * 4096UL);
        free(e->data);
    }
    pending_count = 0;
}

static void op(char kind, uint64_t rel, uint32_t blocks, uint32_t timeout)
{
    assert(op_count < sizeof(ops) / sizeof(ops[0]));
    ops[op_count++] = (struct op){kind, rel, blocks, timeout};
}

int native_ufs_log_open(uint64_t *first, uint64_t *blocks, const char **why)
{
    op('o', 0, 0, 0);
    if (open_state) { *why = open_state == 1 ? "no root layout" : "content gate: foreign data at the log window's first block"; return 0; }
    assert(!model_open);
    model_open = 1;
    *first = FIRST_LBA;
    *blocks = WINDOW_BLOCKS;
    return 1;
}

int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src)
{
    assert(model_open && !model_closed && !panic_claimed);
    assert(blocks && blocks <= 32 && rel < WINDOW_BLOCKS - 1 && blocks <= WINDOW_BLOCKS - 1 - rel);      // the last block is never written
    op('w', rel, blocks, 0);
    clock_now += op_ticks;
    if (fail_writes) { fail_writes--; return 0; }
    store(0, rel, blocks, src);
    return 1;
}

uint64_t native_ufs_export_blocks(void) { return model_open ? model_export_blocks : 0; }

int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src)
{
    assert(model_open && !model_closed && !panic_claimed);
    assert(locks_held);                     // the pieces are serialised, and writer_stop waits for this one
    assert(blocks && blocks <= 64 && rel < EXPORT_BLOCKS - 1 && blocks <= EXPORT_BLOCKS - 1 - rel);
    op('e', rel, blocks, 0);
    if (fail_writes) { fail_writes--; return 0; }
    store(1, rel, blocks, src);
    return 1;
}

int native_ufs_log_sync(void)
{
    assert(model_open && !model_closed && !panic_claimed);
    op('s', 0, 0, 0);
    clock_now += sync_ticks ? sync_ticks : op_ticks;
    if (sync_hook) { void (*hook)(void) = sync_hook; sync_hook = NULL; hook(); }
    if (fail_syncs) { fail_syncs--; return 0; }
    flush_pending();
    return 1;
}

void native_ufs_log_close(void) { model_closed = 1; op('c', 0, 0, 0); }
unsigned native_ufs_store_gpt_check(void) { gpt_checks++; op('g', 0, 0, 0); return gpt_result; }

int native_ufs_log_panic(uint32_t waitUs)
{
    assert(host_daif_masked);               // interrupts stay off for all of it
    assert(!model_closed);
    op('C', 0, 0, waitUs);
    if (claim_hook) { void (*hook)(void) = claim_hook; claim_hook = NULL; hook(); }
    if (!model_open) return 0;
    if (claim_refused) { clock_now += (uint64_t)waitUs * HZ / 1000000; return 0; }
    if (claim_refusals) { claim_refusals--; clock_now += (uint64_t)waitUs * HZ / 1000000; return 0; }
    panic_claimed = 1;
    return 1;
}

int native_ufs_log_panic_write(uint64_t rel, uint32_t blocks, const void *src, uint32_t timeoutUs)
{
    assert(panic_claimed && !model_closed && host_daif_masked && rel + blocks <= WINDOW_BLOCKS - 1);
    op('W', rel, blocks, timeoutUs);
    clock_now += op_ticks;
    if (fail_panic_write) { fail_panic_write--; return 0; }
    store(0, rel, blocks, src);
    return 1;
}

int native_ufs_log_panic_sync(uint32_t timeoutUs)
{
    assert(panic_claimed && !model_closed && host_daif_masked);
    op('S', 0, 0, timeoutUs);
    clock_now += op_ticks;
    flush_pending();
    return 1;
}

// ---- helpers --------------------------------------------------------------------------------------------------------------
static void boot(void)
{
    ramoops_log_suspend();
    memset(region, 0, sizeof(region));
    memset(window, 0, (size_t)WINDOW_BLOCKS * 4096);
    memset(&w, 0, sizeof(w));
    memset(headers, 0, sizeof(headers));
    memset(msg_ring, 0, sizeof(msg_ring));
    live = NULL;
    writer_stopped = reset_coming = 0;
    writer_started = panic_flushed = 0;
    export_lock = NULL;
    export_stage = NULL;
    export_region = export_total = export_filled = export_generation = export_target = 0;
    export_ready = export_ready_bytes = export_ready_first = export_named = 0;
    last_state = LAST_IDLE;
    open_state = model_open = model_closed = claim_refused = panic_claimed = 0;
    claim_refusals = 0;
    fail_writes = fail_syncs = fail_panic_write = gpt_result = gpt_checks = 0;
    op_count = 0;
    op_ticks = sync_ticks = 0;
    sync_hook = claim_hook = unlock_hook = NULL;
    psci_reset_hook = wdog_bite_hook = wdog_flushed_hook = NULL;
    model_export_blocks = EXPORT_BLOCKS;
    allocations = alloc_fail = 0;
    thread_starts = thread_start_fails = 0;
    psci_resets = 0;
    psci_reset_at = 0;
    wdog_armed = 1;
    pets_stopped = wdog_flushed = 0;
    power_held = 0;
    table_count = 0;
    clock_step = 1;
    test_msgbuf.msg_bufx = 0;
    test_msgbuf.msg_size = sizeof(msg_ring);
    test_msgbuf.msg_bufc = msg_ring;
    host_daif_masked = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
}

static void start_log(void)
{
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
}

static void open_window(void)
{
    start_log();
    ufs_log_start();
    assert(thread_starts == 1);
    assert(ufs_log_writer_open() == 1);
}

static void advance_ms(uint64_t ms) { clock_now += ms * HZ / 1000; }
// What a pass answers: the first UFS_LOG_EARLY_MS after the writer started look at the rings more often, also in the last seconds before a reset.
static unsigned early(void) { return clock_now - w.start < (uint64_t)HZ * UFS_LOG_EARLY_MS / 1000; }
static unsigned normal_tick(void) { return early() ? UFS_LOG_EARLY_TICK_MS : UFS_LOG_TICK_MS; }
static unsigned final_tick(void) { return early() ? UFS_LOG_EARLY_TICK_MS : UFS_LOG_FINAL_TICK_MS; }

// Emits `bytes` of records into a ring.
static void emit_bytes(char kind, unsigned bytes)
{
    static char text[RAMOOPS_LOG_TEXT_MAX + 1];
    while (bytes) {
        const unsigned total = bytes < 64 ? 64 : bytes > 200 ? 200 : bytes, length = total - 34;
        memset(text, 'a' + (char)(sequence % 26), length);
        text[length] = 0;
        ramoops_log_line(kind, text);
        bytes -= bytes < total ? bytes : total;
    }
}

static struct ufs_log_header medium(void)
{
    struct ufs_log_header h;
    memcpy(&h, window, sizeof(h));
    if (h.magic[0]) {
        assert(!memcmp(h.magic, UFS_LOG_MAGIC, 8) && h.header_bytes == sizeof(h));
        assert(h.crc == ufs_crc((const uint8_t *)&h, offsetof(struct ufs_log_header, crc)));
    }
    return h;
}

static uint64_t slot_block(uint32_t logical)
{
    assert(w.slot_count == SLOTS);
    return UFS_LOG_SLOT_FIRST + (uint64_t)slot_position(logical) * UFS_LOG_SLOT_BLOCKS;
}

static unsigned count_ops(char kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < op_count; i++) n += ops[i].kind == kind;
    return n;
}

// Marks every intact record of a slot's copy.
static unsigned scan_slot(const uint8_t *slot, uint8_t *seen, uint32_t limit)
{
    static uint8_t ring[RAMOOPS_LOG_COPY_LIMIT + 300];
    unsigned found = 0;
    for (unsigned a = 0; a < AREAS; a++) {
        const uint32_t offset = areas[a].offset, bytes = areas[a].bytes;
        memcpy(ring, slot + offset, bytes);
        memcpy(ring + bytes, slot + offset, 240 < bytes ? 240 : bytes);     // a record that wraps the ring's end
        for (uint32_t at = 0; at < bytes;) {
            unsigned length = 0;
            const uint32_t seq = record_at(ring + at, &length);
            if (seq && seq < limit) { seen[seq] = 1; found++; }
            at += seq ? length : 1;
        }
    }
    return found;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------
static void test_off(void)
{
    boot();
    table_count = 0;
    arg("pagani-ramoops", 1);
    start_log();
    ufs_log_start();
    assert(!thread_starts && !op_count && !allocations);
    assert(boot_log_export_put(1, 16, 0, "0123456789abcdef", 16) == -1);
    ufs_log_panic(0);
    assert(!ufs_log_panic_pending());
    ufs_log_quiesce("off");
    ramoops_log_restart("off");
    assert(psci_resets && !op_count && !allocations && !pets_stopped);       // a reset without the log: nothing new
    // sheng-log=0, or anything but 1, is off too.
    boot();
    arg("sheng-log", 0);
    start_log();
    ufs_log_start();
    assert(!thread_starts);
    boot();
    arg("sheng-log", 2);
    start_log();
    ufs_log_start();
    assert(!thread_starts && !op_count);
}

static void test_open(void)
{
    // Started once; waits while the root layout is missing, without writing or allocating anything.
    boot();
    start_log();
    ufs_log_start();
    ufs_log_start();
    assert(thread_starts == 1);
    open_state = 1;
    assert(ufs_log_writer_open() == 0 && ufs_log_writer_open() == 0);
    assert(!allocations && count_ops('o') == 2 && count_ops('w') == 0);
    open_state = 0;
    assert(ufs_log_writer_open() == 1);
    // The first header is on the medium before any round: a new run replaces the old header at once.
    struct ufs_log_header h = medium();
    assert(h.version == UFS_LOG_VERSION && h.first_lba == FIRST_LBA && h.window_blocks == WINDOW_BLOCKS);
    assert(h.msg_first == 1 && h.msg_blocks == 32 && h.slot_first == 33 && h.slot_blocks == 16 && h.slot_count == SLOTS);
    assert(h.slot_pinned == 32 && !h.slots_written && !h.rounds && h.state == UFS_LOG_STATE_WRITING);
    assert(h.ramoops_run == run_id && h.boot_id == w.start && h.period_ms == 3000);
    assert(h.export_blocks == EXPORT_BLOCKS && h.export_below == EXPORT_BLOCKS && !h.export_bytes);
    assert(gpt_checks == 1 && !h.gpt_open && !h.gpt_last);
    // A refusal stops the writer, says why in the log, and writes and allocates nothing.
    boot();
    start_log();
    ufs_log_start();
    open_state = 2;
    const uint32_t before = sequence;
    assert(ufs_log_writer_open() == -1);
    assert(!allocations && !count_ops('w') && sequence == before + 1);
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    struct ramoops_snapshot s;
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    assert(memmem(copy, sizeof(copy), "ufslog refused: content gate", 28));
    // A failed allocation closes the window it opened; a thread that does not start says so.
    boot();
    start_log();
    alloc_fail = 1;
    assert(ufs_log_writer_open() == -1 && model_closed);
    boot();
    start_log();
    thread_start_fails = 1;
    ufs_log_start();
    assert(thread_starts == 1);
    // The GPT read at the start must match the root start; nothing is written otherwise.
    boot();
    start_log();
    gpt_result = 5;
    assert(ufs_log_writer_open() == -1 && model_closed && !count_ops('w'));
    // The period: default, boot argument, floor.
    boot();
    arg("pagani-ufslog-ms", 100);
    open_window();
    assert(w.period_ms == 1000);
    boot();
    arg("pagani-ufslog-ms", 5000);
    open_window();
    assert(w.period_ms == 5000 && medium().period_ms == 5000);
    boot();
    arg("pagani-ufslog-ms", 0);
    open_window();
    assert(w.period_ms == 3000);
}

static const struct ufs_log_header *live_before;
static void live_stands(void) { assert(__atomic_load_n(&live, __ATOMIC_ACQUIRE) == live_before); }

static void test_rounds(void)
{
    boot();
    open_window();
    const unsigned at_open = op_count;
    // The first tick is due at once; order: slot, message buffer, header, flush.
    msg_ring[0] = 'k';
    test_msgbuf.msg_bufx = 1;
    assert(ufs_log_writer_tick() == normal_tick());
    assert(op_count == at_open + 4);
    assert(ops[at_open].kind == 'w' && ops[at_open].rel == slot_block(0) && ops[at_open].blocks == 16);
    assert(ops[at_open + 1].kind == 'w' && ops[at_open + 1].rel == 1 && ops[at_open + 1].blocks == 32);
    assert(ops[at_open + 2].kind == 'w' && ops[at_open + 2].rel == 0 && ops[at_open + 2].blocks == 1);
    assert(ops[at_open + 3].kind == 's');
    struct ufs_log_header h = medium();
    assert(h.rounds == 1 && h.slots_written == 1 && h.msg_bytes == 1 && window[4096] == 'k' && !window[4097]);
    // The slot is copy 0, byte for byte, its copy number included; the rest of the slot is zero.
    assert(!memcmp(window + slot_block(0) * 4096, (const uint8_t *)(uintptr_t)copies[0], 0x10));
    uint32_t copy_number;
    memcpy(&copy_number, window + slot_block(0) * 4096 + 12, 4);
    assert(!copy_number);
    for (unsigned i = 0xfffc; i < UFS_LOG_SLOT_BYTES; i++) assert(!window[slot_block(0) * 4096 + i]);
    // Nothing new: nothing written and nothing flushed, however long it stays that way.
    unsigned mark = op_count;
    for (unsigned i = 0; i < 40; i++) {
        advance_ms(500);
        assert(ufs_log_writer_tick() == normal_tick());
    }
    assert(op_count == mark);
    // A new record waits for the period, then takes one changed block; the message buffer did not move, so it is not written.
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() == normal_tick() && op_count == mark + 3 && ops[mark].blocks < 16);
    assert(ops[mark].rel >= slot_block(0) && ops[mark].rel < slot_block(0) + 16);
    assert(ops[mark + 1].rel == 0 && ops[mark + 2].kind == 's' && medium().rounds == 2 && medium().slots_written == 1);
    // `live` moves only once its header is flushed: the way out reads it, and exports go by it.
    emit_bytes('U', 64);
    advance_ms(3001);
    live_before = live;
    sync_hook = live_stands;
    assert(ufs_log_writer_tick() == normal_tick() && !sync_hook);
    assert(live != live_before && live->rounds == medium().rounds);
    mark = op_count;
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() == normal_tick() && op_count == mark);               // the period has not passed
    advance_ms(2999);
    assert(ufs_log_writer_tick() == normal_tick() && op_count == mark);
    advance_ms(2);
    assert(ufs_log_writer_tick() == normal_tick() && op_count > mark);
    // The medium equals the snapshot, block by block (the unchanged blocks were left alone but are the same).
    static uint8_t copy[UFS_LOG_SLOT_BYTES];
    struct ramoops_snapshot s;
    memset(copy, 0, sizeof(copy));
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    assert(!memcmp(copy, window + slot_block(0) * 4096, sizeof(copy)));
    // The message buffer alone is worth a round too, oldest byte first across its wrap.
    mark = op_count;
    memset(msg_ring, 'x', sizeof(msg_ring));
    msg_ring[99] = 'n';                     // newest
    msg_ring[100] = 'o';                    // oldest
    test_msgbuf.msg_bufx = 100;
    advance_ms(3001);
    assert(ufs_log_writer_tick() == normal_tick());
    assert(window[4096] == 'o' && window[4096 + sizeof(msg_ring) - 1] == 'n');
    assert(medium().msg_bytes == sizeof(msg_ring));
    // A buffer larger than its blocks: the newest bytes.
    static char big[MSG_BYTES + 5000];
    memset(big, 'y', sizeof(big));
    big[0] = 'a';
    big[sizeof(big) - 1] = 'z';
    test_msgbuf.msg_bufc = big;
    test_msgbuf.msg_size = sizeof(big);
    test_msgbuf.msg_bufx = 0;
    advance_ms(3001);
    assert(ufs_log_writer_tick() == normal_tick());
    assert(window[4096 + MSG_BYTES - 1] == 'z' && window[4096] == 'y' && medium().msg_bytes == MSG_BYTES);
}

// The boot's stretch: the rings are looked at every 100 ms, then every 500 ms; a burst goes out at the first look that sees it.
static void test_early_ticks(void)
{
    boot();
    open_window();
    assert(ufs_log_writer_tick() == UFS_LOG_EARLY_TICK_MS);
    advance_ms(UFS_LOG_EARLY_MS - 2000);
    assert(ufs_log_writer_tick() == UFS_LOG_EARLY_TICK_MS);
    advance_ms(2000);
    assert(ufs_log_writer_tick() == UFS_LOG_TICK_MS);
    assert(ufs_log_writer_tick() == UFS_LOG_TICK_MS);
    printf("ok  ticks: %u ms for the first %u s, then %u ms\n", UFS_LOG_EARLY_TICK_MS, UFS_LOG_EARLY_MS / 1000, UFS_LOG_TICK_MS);
}

static void test_advance(void)
{
    boot();
    open_window();
    assert(w.advance == 24 * 1024 && w.burst == 12 * 1024);
    assert(ufs_log_writer_tick());
    assert(w.current == 0);
    // Under the threshold the slot stays; past it the snapshot opens the next slot and the old one stays as it was.
    for (unsigned i = 0; i < 7; i++) { emit_bytes('C', 3000); advance_ms(3001); assert(ufs_log_writer_tick()); }
    assert(w.current == 0 && medium().slots_written == 1);
    static uint8_t before[UFS_LOG_SLOT_BYTES];
    memcpy(before, window + slot_block(0) * 4096, sizeof(before));
    unsigned mark = op_count;
    emit_bytes('C', 4000);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(w.current == 1 && medium().slots_written == 2 && ops[mark].rel == slot_block(1) && ops[mark].blocks == 16);
    assert(!memcmp(before, window + slot_block(0) * 4096, sizeof(before)));
    // The events ring advances it too.
    for (unsigned i = 0; i < 9; i++) { emit_bytes('U', 3000); advance_ms(3001); assert(ufs_log_writer_tick()); }
    assert(w.current == 2);
    // The ring: positions after the pinned ones wrap, and the pinned ones are never written again.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    static uint8_t pinned[UFS_LOG_PINNED][64];
    uint32_t last = 0;
    while (last < 2 * SLOTS) {
        emit_bytes('C', 25000);
        advance_ms(3001);
        const unsigned at = op_count;
        assert(ufs_log_writer_tick());
        assert(w.current == last + 1 || w.current == last);
        last = w.current;
        for (unsigned i = at; i < op_count; i++) {
            if (ops[i].kind != 'w' || ops[i].rel < UFS_LOG_SLOT_FIRST) continue;
            if (w.current >= UFS_LOG_PINNED) assert(ops[i].rel >= UFS_LOG_SLOT_FIRST + UFS_LOG_PINNED * UFS_LOG_SLOT_BLOCKS);
            assert(ops[i].rel + ops[i].blocks <= UFS_LOG_SLOT_FIRST + SLOTS * UFS_LOG_SLOT_BLOCKS);
        }
        if (w.current == UFS_LOG_PINNED - 1)
            for (unsigned i = 0; i < UFS_LOG_PINNED; i++) memcpy(pinned[i], window + slot_block(i) * 4096 + 4096, 64);
    }
    for (unsigned i = 0; i < UFS_LOG_PINNED; i++) assert(!memcmp(pinned[i], window + slot_block(i) * 4096 + 4096, 64));
    assert(medium().slots_written == last + 1);
    assert(slot_position(UFS_LOG_PINNED) == UFS_LOG_PINNED && slot_position(SLOTS - 1) == SLOTS - 1);
    assert(slot_position(SLOTS) == UFS_LOG_PINNED && slot_position(SLOTS + 1) == UFS_LOG_PINNED + 1);
    assert(slot_block(SLOTS - 1) + 16 <= WINDOW_BLOCKS - 1);
    // A smaller copy: 7/8 of its smaller ring.
    boot();
    arg("pagani-ramoops-copy", 0x8000);
    open_window();
    assert(w.advance == areas[AREA_EVENTS].bytes - areas[AREA_EVENTS].bytes / 8 && w.advance < 24 * 1024);
    assert(ufs_log_writer_tick());
    static uint8_t tail[4];
    assert(!memcmp(window + slot_block(0) * 4096 + copy_bytes, tail, 4));
}

// Whatever the two rings take between rounds, every record stays in some slot as long as no ring turned over between two rounds.
static void test_continuity(void)
{
    static uint8_t seen[1 << 20];
    for (unsigned seed = 1; seed <= 20; seed++) {
        boot();
        open_window();
        uint32_t random = seed * 2654435761u;
        for (unsigned round = 0; round < 120; round++) {
            random = random * 1664525 + 1013904223;
            const unsigned events = (random >> 8) % 14000, console = (random >> 18) % 14000;
            emit_bytes('U', events);
            emit_bytes('C', console);
            advance_ms(500);
            assert(ufs_log_writer_tick());
            if ((random & 7) == 0) { advance_ms(3000); assert(ufs_log_writer_tick()); }
        }
        advance_ms(3001);
        assert(ufs_log_writer_tick());
        struct ufs_log_header h = medium();
        assert(h.slots_written < SLOTS);
        memset(seen, 0, sizeof(seen));
        const uint32_t limit = sequence + 1;
        assert(limit < sizeof(seen));
        for (uint32_t n = 0; n < h.slots_written; n++) assert(scan_slot(window + slot_block(n) * 4096, seen, limit));
        uint32_t missing = 0;
        for (uint32_t s = 1; s < limit; s++) missing += !seen[s];
        if (missing) printf("seed %u: %u of %u records missing\n", seed, missing, limit - 1);
        assert(!missing);
    }
}

static void test_burst_and_deadline(void)
{
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    unsigned mark = op_count;
    // Half the advance in either ring brings the round forward; less waits.
    emit_bytes('C', 11000);
    advance_ms(500);
    assert(ufs_log_writer_tick() && op_count == mark);
    emit_bytes('C', 2000);
    advance_ms(500);
    assert(ufs_log_writer_tick() && op_count > mark);
    mark = op_count;
    emit_bytes('U', 13000);
    advance_ms(500);
    assert(ufs_log_writer_tick() && op_count > mark);
    // The last 2 s before a timed reset: a round on every pass with new records, and passes every 250 ms.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    assert(ramoops_log_reset_in_ms() > 59000 && ramoops_log_reset_in_ms() <= 60000);
    advance_ms(57000);
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() == normal_tick());
    mark = op_count;
    emit_bytes('U', 64);
    advance_ms(100);
    assert(ufs_log_writer_tick() == normal_tick() && op_count == mark);       // still 2.9 s away, and a round just went
    advance_ms(1000);
    assert(ramoops_log_reset_in_ms() <= 2000);
    assert(ufs_log_writer_tick() == final_tick() && op_count > mark);  // the period has not passed, the deadline is near
    mark = op_count;
    assert(ufs_log_writer_tick() == final_tick() && op_count == mark); // and still only with new records
    emit_bytes('U', 64);
    advance_ms(250);
    assert(ufs_log_writer_tick() == final_tick() && op_count > mark);
    advance_ms(5000);
    assert(ramoops_log_reset_in_ms() == 1);                                     // past it: stays non-zero
    assert(ufs_log_writer_tick() == final_tick());
}

static uint8_t blob[300000];
static void put_blob(uint32_t generation, uint32_t total, uint32_t piece);

static void test_power_key(void)
{
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    unsigned mark = op_count;
    // A press takes what led to it at once, though the period has not passed.
    emit_bytes('U', 64);
    advance_ms(100);
    power_held = 1;
    assert(ufs_log_writer_tick() == normal_tick() && op_count > mark);
    assert(medium().rounds == 2);
    // Then nothing while it is down: no due round, no burst.
    mark = op_count;
    for (unsigned i = 0; i < 6; i++) {
        emit_bytes('C', 13000);
        advance_ms(3001);
        assert(ufs_log_writer_tick() == normal_tick() && op_count == mark);
    }
    // No export either: nothing reaches the medium, and it starts over.
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -9);
    assert(op_count == mark && !export_ready && !export_generation);
    // It comes up: the next pass catches up, and what follows runs as ever.
    power_held = 0;
    assert(ufs_log_writer_tick() && op_count > mark);
    assert(medium().rounds == 3);
    assert(!boot_log_export_put(2, 16, 0, blob, 16) && export_ready == 2);
    mark = op_count;
    advance_ms(3001);
    assert(ufs_log_writer_tick() && op_count > mark && medium().export_generation == 2);
    advance_ms(3001);
    assert(ufs_log_writer_tick());                                              // that round's own record
    // A press with nothing new writes nothing, and a press that went up again leaves the next round to its period.
    mark = op_count;
    power_held = 1;
    assert(ufs_log_writer_tick() && op_count == mark);
    power_held = 0;
    emit_bytes('U', 64);
    advance_ms(100);
    assert(ufs_log_writer_tick() && op_count == mark);
    advance_ms(3000);
    assert(ufs_log_writer_tick() && op_count > mark);
    // A press while the key is already down is the same press; down again after a pass with it up is a new one.
    mark = op_count;
    power_held = 1;
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() && op_count > mark);
    mark = op_count;
    power_held = 0;
    assert(ufs_log_writer_tick() && op_count == mark);
    emit_bytes('U', 64);
    power_held = 1;
    assert(ufs_log_writer_tick() && op_count > mark);
    // A reset does not wait for the key: the last snapshot goes out with it down.
    mark = op_count;
    emit_bytes('U', 64);
    host_daif_masked = 1;
    ufs_log_quiesce("key");
    assert(op_count > mark && medium().state == UFS_LOG_STATE_RESET);
    // The last seconds before a timed reset are no exception.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    power_held = 1;
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() == normal_tick() && w.presses == 1);
    mark = op_count;
    advance_ms(59000);
    assert(ramoops_log_reset_in_ms() <= 2000);
    emit_bytes('U', 64);
    assert(ufs_log_writer_tick() == final_tick() && op_count == mark);
    // A piece of a blob that is well under way stops too, and the whole blob starts over, in order, after the key.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    enum { BLOB = 300000 };
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    assert(!boot_log_export_put(1, BLOB, 0, blob, 270000) && export_filled == 270000);
    mark = op_count;
    assert(count_ops('e') == 1);
    power_held = 1;
    assert(boot_log_export_put(1, BLOB, 270000, blob + 270000, 30000) == -9 && op_count == mark && !export_generation);
    power_held = 0;
    assert(boot_log_export_put(1, BLOB, 270000, blob + 270000, 30000) == -3);    // not from the middle
    assert(!boot_log_export_put(1, BLOB, 0, blob, BLOB) && export_ready == 1);
}

static void test_failures(void)
{
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    // A failed round leaves the header alone and the next round writes the slot in whole.
    emit_bytes('U', 64);
    advance_ms(3001);
    fail_writes = 1;
    assert(ufs_log_writer_tick());
    assert(medium().rounds == 1 && w.errors == 1);
    advance_ms(3001);
    unsigned mark = op_count;
    assert(ufs_log_writer_tick());
    assert(ops[mark].blocks == 16 && medium().rounds == 2 && w.errors == 0 && medium().write_errors == 1);
    // A failed flush counts the same: `live` stays where it was, and the next round does the slot in whole.
    emit_bytes('U', 64);
    advance_ms(3001);
    fail_syncs = 1;
    assert(ufs_log_writer_tick());
    assert(w.errors == 1 && live->rounds == 2);
    advance_ms(3001);
    mark = op_count;
    assert(ufs_log_writer_tick());
    assert(ops[mark].blocks == 16 && live->rounds == 3 && medium().rounds == 3 && !w.errors);
    // The flush before a new export's header, failed: that header does not go out.
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    put_blob(1, 8000, 3968);
    fail_syncs = 1;
    advance_ms(3001);
    mark = op_count;
    assert(ufs_log_writer_tick());
    for (unsigned i = mark; i < op_count; i++) assert(!(ops[i].kind == 'w' && ops[i].rel == 0));
    assert(medium().rounds == 3 && !medium().export_bytes && export_named == 0);
    // A failed slot advance retries the same slot.
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(medium().export_generation == 1 && export_named == 1);
    emit_bytes('C', 25000);
    advance_ms(3001);
    fail_writes = 1;
    assert(ufs_log_writer_tick());
    assert(w.current == 0 && medium().slots_written == 1);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(w.current == 1 && medium().slots_written == 2);
    // Two failures and a success keep it going; three in a row stop it for good and close the window.
    for (unsigned i = 0; i < 2; i++) {
        emit_bytes('U', 64);
        advance_ms(3001);
        fail_writes = 1;
        assert(ufs_log_writer_tick());
    }
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    const uint32_t kept = medium().rounds;
    for (unsigned i = 0; i < 2; i++) {
        emit_bytes('U', 64);
        advance_ms(3001);
        fail_writes = 1;
        assert(ufs_log_writer_tick());
    }
    emit_bytes('U', 64);
    advance_ms(3001);
    fail_writes = 1;
    assert(ufs_log_writer_tick() == 0);
    assert(model_closed && writer_stopped && medium().rounds == kept);
    assert(boot_log_export_put(1, 16, 0, "0123456789abcdef", 16) == -4);
    const unsigned at = op_count;
    ufs_log_panic(0);
    assert(op_count == at);                  // a stopped writer stays stopped, in a panic too
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    struct ramoops_snapshot s;
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    assert(memmem(copy, sizeof(copy), "ufslog stopped for good (state 3)", 33));
}

static void test_gpt(void)
{
    boot();
    open_window();
    for (unsigned i = 1; i < 90; i++) {
        emit_bytes('U', 64);
        advance_ms(3001);
        assert(ufs_log_writer_tick());
        assert(gpt_checks == 1 + (medium().rounds + 0) / 30);
    }
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    struct ramoops_snapshot s;
    // Any change, or a failure to read it, ends the log before that round writes anything.
    gpt_result = 2;
    emit_bytes('U', 64);
    advance_ms(3001);
    const unsigned mark = op_count;
    assert(medium().rounds == 89 && !ufs_log_writer_tick() && writer_stopped);
    assert(model_closed && ops[mark].kind == 'g' && ops[mark + 1].kind == 'c' && op_count == mark + 2);
    assert(medium().rounds == 89 && medium().gpt_last == 0);
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    assert(memmem(copy, sizeof(copy), "ufslog stopped for good (state 4)", 33));
}

// What the statistics say, against what the model's commands took: a round from its first command to its last flush, the snapshot left out;
// a command, a write as well as a flush.
static void test_stats(void)
{
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    op_ticks = HZ / 50;                     // 20 ms a write
    sync_ticks = HZ / 100;                  // 10 ms a flush
    enum { ROUND = 50000, WRITE = 20000 };
    for (unsigned i = 0; i < 29; i++) {
        emit_bytes('U', 64);
        advance_ms(3001);
        const uint64_t began = clock_now;
        assert(ufs_log_writer_tick());
        const uint32_t round_time = (uint32_t)((clock_now - began) * 1000000 / HZ);
        // The slot, the header, the flush; the header's value is of the rounds before it.
        assert(round_time >= ROUND && round_time < ROUND + 1000);
        if (i) assert(medium().max_round_us >= ROUND && medium().max_round_us <= round_time + 200);
        else assert(!medium().max_round_us);
    }
    assert(w.max_round_us >= ROUND && w.max_round_us <= ROUND + 200 && w.max_command_us >= WRITE && w.max_command_us <= WRITE + 100);
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    struct ramoops_snapshot s;
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    const char *line = memmem(copy, sizeof(copy), "ufslog rounds 30 slots", 22);
    assert(line);
    unsigned rounds, slots, errors, lately, ever, command, command_ever;
    unsigned presses;
    assert(sscanf(line, "ufslog rounds %u slots %u errors %u longest round %u us (of the last 30), ever %u us; longest command %u us, ever %u us; key presses %u",
                  &rounds, &slots, &errors, &lately, &ever, &command, &command_ever, &presses) == 8 && !presses);
    assert(rounds == 30 && slots == 1 && !errors);
    assert(lately >= ROUND && lately <= ROUND + 200 && ever == lately && command >= WRITE && command <= WRITE + 100 && command_ever == command);
    // Lately starts over, ever does not.
    assert(!w.stat_round_us && !w.stat_command_us && w.max_round_us == ever);
    // A flush that takes longer than any write is the longest command.
    sync_ticks = HZ / 25;
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(w.max_command_us >= 40000 && w.max_command_us <= 40100);
    sync_ticks = 0;
    op_ticks = HZ / 100;
    // A round that fails counts too, and a round that names an export is a flush longer.
    emit_bytes('U', 64);
    advance_ms(3001);
    fail_writes = 1;
    assert(ufs_log_writer_tick());
    assert(w.stat_round_us >= 80000 && w.stat_round_us < 81000);    // the longest since the last record: the slow flush above
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    put_blob(1, 8000, 3968);
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(ops[op_count - 3].kind == 's' && ops[op_count - 1].kind == 's');
    assert(w.max_round_us >= 80000 && w.max_round_us < 81000);
}

static void finish_export(void) { put_blob(9, 8000, 3968); }

static void put_blob(uint32_t generation, uint32_t total, uint32_t piece)
{
    for (uint32_t offset = 0; offset < total;) {
        const uint32_t n = total - offset < piece ? total - offset : piece;
        assert(!boot_log_export_put(generation, total, offset, blob + offset, n));
        offset += n;
    }
}

static void test_export(void)
{
    boot();
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -1);                      // before the window opens
    open_window();
    assert(ufs_log_writer_tick());
    const unsigned base_allocations = allocations;
    assert(allocations == 1 && !export_stage);                                  // the staging buffer comes with the first piece
    assert(boot_log_export_put(0, 16, 0, blob, 16) == -1 && boot_log_export_put(1, 0, 0, blob, 16) == -1);
    assert(boot_log_export_put(1, 16, 0, NULL, 16) == -1 && boot_log_export_put(1, 16, 17, blob, 1) == -1);
    assert(boot_log_export_put(1, 16, 8, blob, 9) == -1 && allocations == base_allocations);
    for (unsigned i = 0; i < sizeof(blob); i++) blob[i] = (uint8_t)(i * 31 + (i >> 8));
    // A first piece that is not a blob's would close the content gate for the next boot; nothing of it is taken.
    const unsigned no_write = op_count;
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -7);
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 7);
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -7);
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    assert(boot_log_export_put(1, 16, 0, blob, 7) == -7);
    assert(op_count == no_write && allocations == base_allocations);
    // Generation 1: in pieces like the driver's, across the staging buffer; the header names it only when it is all there.
    assert(!boot_log_export_put(1, 300000, 0, blob, 3968));
    assert(allocations == base_allocations + 1 && export_stage);
    assert(boot_log_export_put(1, 300000, 8000, blob + 8000, 3968) == -3);      // only in order
    assert(boot_log_export_put(2, 300000, 3968, blob + 3968, 3968) == -3);
    uint32_t sent = 3968;
    for (; sent + 3968 < 300000; sent += 3968) assert(!boot_log_export_put(1, 300000, sent, blob + sent, 3968));
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(!medium().export_bytes);                                             // not finished
    assert(!boot_log_export_put(1, 300000, sent, blob + sent, 300000 - sent));
    assert(allocations == base_allocations + 1);
    // Its tail block is padded with zeros, and the last 'e' comes before the flush that comes before the header that names it.
    const unsigned mark = op_count;
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    struct ufs_log_header h = medium();
    assert(h.export_generation == 1 && h.export_bytes == 300000 && h.export_first == 0);
    assert(!memcmp(export_area, blob, 300000));
    for (unsigned i = 300000; i < 73 * 4096; i++) assert(!export_area[i]);
    unsigned sync_at = 0, header_at = 0;
    for (unsigned i = mark; i < op_count; i++) {
        if (ops[i].kind == 's' && !sync_at) sync_at = i;
        if (ops[i].kind == 'w' && !ops[i].rel) header_at = i;
    }
    assert(sync_at && header_at > sync_at);
    assert(ops[op_count - 1].kind == 's' && ops[op_count - 2].kind == 'w' && !ops[op_count - 2].rel && ops[op_count - 3].kind == 's');
    // Only that round: the next one flushes once.
    emit_bytes('U', 64);
    advance_ms(3001);
    const unsigned plain = op_count;
    assert(ufs_log_writer_tick());
    assert(op_count == plain + 3 && ops[plain + 1].kind == 'w' && !ops[plain + 1].rel && ops[plain + 2].kind == 's');
    // Generation 2 goes to the other half while the header still names generation 1, whole.
    memset(blob + 8, 0x77, 50000);
    assert(!boot_log_export_put(2, 200000, 0, blob, 3968));
    assert(!memcmp(export_area, "SHEXR001", 8) && export_area[8] != 0x77);
    for (uint32_t offset = 3968; offset < 200000; offset += 3968) {
        const uint32_t n = 200000 - offset < 3968 ? 200000 - offset : 3968;
        assert(!boot_log_export_put(2, 200000, offset, blob + offset, n));
    }
    assert(!memcmp(export_area + 32768 * 4096UL, blob, 200000));
    // Generation 3 waits for a round: its half is the one the medium's header still names (generation 1). Nothing of it goes out.
    assert(medium().export_generation == 1 && export_ready == 2);
    const unsigned refused = op_count;
    assert(boot_log_export_put(3, 100000, 0, blob, 3968) == -8);
    assert(op_count == refused);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    h = medium();
    assert(h.export_generation == 2 && h.export_bytes == 200000 && h.export_first == 32768);
    // Generation 3 is back in the first half; a failed write loses the blob, not the one before it.
    put_blob(3, 100000, 3968);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(medium().export_first == 0 && medium().export_generation == 3);
    fail_writes = 1;
    assert(boot_log_export_put(4, 300000, 0, blob, 262144) == -6);
    assert(boot_log_export_put(4, 300000, 262144, blob + 262144, 3968) == -3);
    advance_ms(3001);
    assert(medium().export_generation == 3 && export_ready == 3);
    // Up to a half less a block: its last block stays blank for the content gate.
    const uint32_t most = (32768 - 1) * 4096;
    assert(boot_log_export_put(5, most + 1, 0, blob, 16) == -5);
    assert(!boot_log_export_put(5, most, 0, blob, 16));
    // No export region: refused, and nothing allocated.
    boot();
    model_export_blocks = 0;
    open_window();
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -4 && !export_stage);
    // No memory for the staging buffer: refused, and the next piece may try again.
    boot();
    open_window();
    alloc_fail = 1;
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -2 && !export_stage);
    alloc_fail = 0;
    assert(!boot_log_export_put(1, 16, 0, blob, 16) && export_stage);
    // A blob that finishes while a round flushes is not named by that round's header: the flush may have missed it.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    emit_bytes('U', 64);
    advance_ms(3001);
    sync_hook = finish_export;
    assert(ufs_log_writer_tick() && !sync_hook);
    assert(!medium().export_bytes);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(medium().export_generation == 9 && medium().export_bytes == 8000);
    // Nor one that finishes right after the round looked at the counters, with no flush to come before its header.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    emit_bytes('U', 64);
    advance_ms(3001);
    unlock_hook = finish_export;
    unlock_skip = 1;                        // the tick's look comes first
    assert(ufs_log_writer_tick() && !unlock_hook);
    assert(!medium().export_bytes && export_ready == 9);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    assert(medium().export_generation == 9 && medium().export_bytes == 8000);
}

// ---- a power cut at any command --------------------------------------------------------------------------------------------
// One blob per generation, its bytes a function of the two.
static void generation_blob(uint8_t *out, uint32_t generation, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) out[i] = (uint8_t)(i * 7 + (i >> 9) + generation * 13);
    memcpy(out, UFS_LOG_EXPORT_MAGIC, 8);
}

// The export `h` names must be whole on what the cut leaves: what was flushed and, with `landed`, all that is pending.
static void check_named(const struct ufs_log_header *h, int landed)
{
    static uint8_t on_medium[300000 + 4096], want[300000];
    if (!h->export_bytes) return;
    assert(h->export_bytes <= sizeof(want));
    const uint64_t first = h->export_first * 4096UL;
    memcpy(on_medium, export_area + first, h->export_bytes);
    for (unsigned i = 0; landed && i < pending_count; i++) {
        const struct pending *e = &pendings[i];
        const uint64_t at = e->rel * 4096, bytes = e->blocks * 4096UL;
        if (!e->export || at + bytes <= first || at >= first + h->export_bytes) continue;
        for (uint64_t b = 0; b < bytes; b++)
            if (at + b >= first && at + b < first + h->export_bytes) on_medium[at + b - first] = e->data[b];
    }
    generation_blob(want, h->export_generation, h->export_bytes);
    assert(!memcmp(on_medium, want, h->export_bytes));
    cuts_checked++;
}

// Pending writes land in any order, or not at all. The two worst cuts: the newest header alone, and all but the header.
static void check_cut(void)
{
    struct ufs_log_header h = medium();
    for (unsigned i = 0; i < pending_count; i++) if (!pendings[i].export && !pendings[i].rel) memcpy(&h, pendings[i].data, sizeof(h));
    check_named(&h, 0);
    h = medium();
    check_named(&h, 1);
}

static void test_power_cut(void)
{
    static uint8_t data[300000];
    boot();
    open_window();
    cached = 1;
    cuts_checked = 0;
    assert(ufs_log_writer_tick());
    uint32_t generation = 0, refused = 0, random = 12345;
    for (unsigned turn = 0; turn < 60; turn++) {
        random = random * 1664525 + 1013904223;
        const uint32_t next = generation + 1, bytes = 20000 + (random >> 16) % 250000;
        generation_blob(data, next, bytes);
        int status = 0;
        for (uint32_t offset = 0; !status && offset < bytes;) {
            const uint32_t n = bytes - offset < 3968 ? bytes - offset : 3968;
            status = boot_log_export_put(next, bytes, offset, data + offset, n);
            offset += n;
            // A round in the middle of a blob, now and then.
            if (!(offset % (3968 * 40)) && (random & 8)) { emit_bytes('U', 64); advance_ms(3001); assert(ufs_log_writer_tick()); }
        }
        assert(!status || status == -8);
        generation += !status;
        refused += !!status;
        if (random & 3) { emit_bytes('U', 64); advance_ms(3001); assert(ufs_log_writer_tick()); }
    }
    assert(generation > 10 && refused > 5 && cuts_checked > 200);
    // A panic's flush over it: its header adds nothing to what is on the medium.
    emit_bytes('U', 64);
    host_daif_masked = 1;
    ufs_log_panic(1000);
    host_daif_masked = 0;
    assert(medium().state == UFS_LOG_STATE_PANIC && !pending_count);
    cached = 0;
}

// The window, three rounds in, with the panic in its records.
static void panic_setup(void)
{
    arg("pagani-panic-reset", 5);
    open_window();
    assert(ufs_log_writer_tick());
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    const char *text = "panic(cpu 0): the test\n";
    while (*text) ramoops_log_panic_putc(*text++);
}

static void log_during_claim(void) { ramoops_log_line('T', "a record while the panic waits"); }

static void round_during_claim(void)
{
    struct ufs_log_header *next = live == &headers[0] ? &headers[1] : &headers[0];
    memcpy(next, live, sizeof(*next));
    next->rounds += 5;
    __atomic_store_n(&live, next, __ATOMIC_RELEASE);
    // The writer goes on to its next pass with something new: no round.
    const unsigned at = op_count;
    emit_bytes('U', 13000);                 // a burst: due at once
    assert(ufs_log_writer_tick() && op_count == at);
}
static unsigned flushed_at;
static void flushed_after_commands(void) { flushed_at = op_count; }

static void test_panic(void)
{
    // In order: the claim, the snapshot into the next slot, header in state 5, flush. The current slot is not touched. What the other CPUs
    // log until the transport is ours is in it, and a watchdog held through this goes off after it.
    boot();
    panic_setup();
    static uint8_t kept[UFS_LOG_SLOT_BYTES];
    memcpy(kept, window + slot_block(0) * 4096, sizeof(kept));
    const uint32_t rounds = medium().rounds;
    const uint64_t began = clock_now;
    unsigned mark = op_count;
    op_ticks = HZ / 200;                    // 5 ms a command
    claim_hook = log_during_claim;
    assert(ufs_log_panic_pending());
    wdog_flushed_hook = flushed_after_commands;
    ramoops_log_panic_reset();
    assert(wdog_flushed == 1 && flushed_at == op_count);
    assert(ops[mark].kind == 'C' && ops[mark].timeout == UFS_LOG_PANIC_CLAIM_US);
    assert(ops[mark + 1].kind == 'W' && ops[mark + 1].rel == slot_block(1) && ops[mark + 1].blocks == 16);
    assert(ops[mark + 2].kind == 'W' && ops[mark + 2].rel == 0 && ops[mark + 2].blocks == 1);
    assert(ops[mark + 3].kind == 'S' && op_count == mark + 4);
    struct ufs_log_header h = medium();
    assert(h.state == UFS_LOG_STATE_PANIC && h.slots_written == 2 && h.rounds == rounds + 1);
    assert(!memcmp(kept, window + slot_block(0) * 4096, sizeof(kept)));
    assert(memmem(window + slot_block(1) * 4096, UFS_LOG_SLOT_BYTES, "panic(cpu 0): the test", 22));
    assert(memmem(window + slot_block(1) * 4096, UFS_LOG_SLOT_BYTES, "panic: reset in 5000 ms", 23));
    assert(memmem(window + slot_block(1) * 4096, UFS_LOG_SLOT_BYTES, "a record while the panic waits", 30));
    // The reset comes on time: the flush lies inside the wait.
    assert(psci_resets && psci_reset_at - began >= 5ull * HZ && psci_reset_at - began < 5ull * HZ + HZ / 10);
    // Once: a nested panic does not flush again.
    assert(!ufs_log_panic_pending());
    mark = op_count;
    ramoops_log_panic_reset();
    assert(op_count == mark);
    // The header is read with the transport in hand: a round that ended meanwhile counts, and none starts after that.
    boot();
    panic_setup();
    const uint32_t before_claim = medium().rounds;
    claim_hook = round_during_claim;
    ramoops_log_panic_reset();
    assert(medium().state == UFS_LOG_STATE_PANIC && medium().rounds == before_claim + 5 + 1);
    // A claim that failed gives the transport back to the rounds.
    boot();
    panic_setup();
    claim_refusals = 1;
    host_daif_masked = 1;
    ufs_log_panic(0);
    host_daif_masked = 0;
    emit_bytes('U', 64);
    advance_ms(3001);
    mark = op_count;
    assert(ufs_log_writer_tick() && op_count > mark);
    // A headless panic comes here with interrupts on: they are off for the flush and back after it (the model checks the first).
    boot();
    panic_setup();
    assert(!host_daif_masked);
    ramoops_log_panic_headless("headless %d", NULL, 0, 0, 0);
    assert(medium().state == UFS_LOG_STATE_PANIC && !host_daif_masked);
    // The transport does not come free: the bounded wait, then nothing is written. The reset tries once more and comes within its own bound.
    boot();
    panic_setup();
    claim_refused = 1;
    mark = op_count;
    const uint64_t at = clock_now;
    ramoops_log_panic_reset();
    assert(op_count == mark + 2 && ops[mark].kind == 'C' && ops[mark + 1].kind == 'C' && ops[mark + 1].timeout == UFS_LOG_QUIESCE_CLAIM_US);
    assert(medium().state == UFS_LOG_STATE_WRITING);
    assert(psci_resets && psci_reset_at - at < 6ull * HZ + HZ / 10);
    // Free by then: the last snapshot goes out from the reset, in the panic's state.
    boot();
    panic_setup();
    claim_refusals = 1;
    mark = op_count;
    ramoops_log_panic_reset();
    assert(op_count == mark + 5 && ops[mark].kind == 'C' && ops[mark + 1].kind == 'C' && ops[mark + 2].kind == 'W');
    assert(medium().state == UFS_LOG_STATE_PANIC);
    // A short reset leaves a smaller budget: 4/5 of the wait, and every step stops at it. Commands of 300 ms: the slot, the header, then
    // 200 ms are left for the flush.
    boot();
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    arg("pagani-panic-reset", 1);
    open_window();
    assert(ufs_log_writer_tick());
    mark = op_count;
    op_ticks = HZ / 1000 * 300;
    const uint64_t short_began = clock_now;
    ramoops_log_panic_reset();
    assert(ops[mark].kind == 'C' && ops[mark + 1].kind == 'W' && ops[mark + 1].timeout <= 200000);
    assert(ops[mark + 2].kind == 'W' && ops[mark + 2].timeout <= 200000);
    assert(ops[mark + 3].kind == 'S' && ops[mark + 3].timeout <= 200000 && ops[mark + 3].timeout > 100000);
    assert(op_count == mark + 4);
    assert(psci_resets && psci_reset_at - short_began < HZ + HZ / 20);      // 900 ms of model commands, and the reset at 1 s
    // Commands of 450 ms: the slot, and nothing is left for the header. No flush either: it would go past the budget.
    boot();
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    arg("pagani-panic-reset", 1);
    open_window();
    assert(ufs_log_writer_tick());
    mark = op_count;
    op_ticks = HZ / 1000 * 900;
    ramoops_log_panic_reset();
    assert(ops[mark + 1].kind == 'W' && op_count == mark + 2);
    // Without a reset to keep, the whole budget, and it gives up at the first failure.
    boot();
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    open_window();
    assert(ufs_log_writer_tick());
    fail_panic_write = 1;
    mark = op_count;
    ramoops_log_panic_reset();
    assert(op_count == mark + 2 && medium().state == UFS_LOG_STATE_WRITING && !psci_resets);
    // The budget runs out while a command takes long: no later step goes out.
    boot();
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    open_window();
    mark = op_count;
    op_ticks = HZ * 2;
    ramoops_log_panic_reset();
    assert(op_count == mark + 2);
    // Not on: pagani-ufslog-panic=0, a panic before the window opened, a log that never started. The transport is still claimed before the
    // reset, without a command.
    boot();
    arg("pagani-ufslog-panic", 0);
    panic_setup();
    mark = op_count;
    assert(!ufs_log_panic_pending());
    ramoops_log_panic_reset();
    assert(op_count == mark + 1 && ops[mark].kind == 'C' && psci_resets);
    boot();
    arg("pagani-panic-reset", 5);
    start_log();
    ufs_log_start();
    ramoops_log_panic_reset();
    assert(op_count == 1 && ops[0].kind == 'C' && psci_resets);
    boot();
    table_count = 0;
    arg("pagani-ramoops", 1);
    arg("pagani-panic-reset", 5);
    start_log();
    ramoops_log_panic_reset();
    assert(!op_count && psci_resets);
    // In the ring: the next logical slot's position.
    boot();
    panic_setup();
    while (w.current < SLOTS + 3) {
        emit_bytes('C', 25000);
        advance_ms(3001);
        assert(ufs_log_writer_tick());
    }
    mark = op_count;
    ramoops_log_panic_reset();
    assert(ops[mark + 1].rel == slot_block(w.current + 1) && medium().slots_written == w.current + 2);
}

// ---- what every reset does first ------------------------------------------------------------------------------------------------
static unsigned ops_at_reset, ops_at_bite;
static void reset_lands(void) { ops_at_reset = op_count; assert(!pending_count); }
static void bite_set(void) { ops_at_bite = op_count; }

static void second_resetter(void)
{
    const uint64_t began = clock_now;
    const unsigned at = op_count;
    clock_step = 100;
    ufs_log_quiesce("backup timer");
    clock_step = 1;
    // It does not overtake the first, nor touch the transport, however long the first takes; and it gives up in time.
    assert(op_count == at && psci_resets == 0 && clock_now - began >= 2ull * HZ && clock_now - began < 2ull * HZ + HZ / 100);
    clock_now = began;                      // on the first resetter's clock this took no time
}

static void test_reset(void)
{
    // From a window in use: pets off, the claim, the last snapshot in state 6 with the reason in it, a flush, and only then the bite's clock
    // and PSCI. Interrupts are off for it and back after it (the model checks the first).
    boot();
    arg("pagani-reset-after", 60);
    arg("pagani-reset-wdog", 1);
    open_window();
    assert(ufs_log_writer_tick());
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick());
    static uint8_t kept[UFS_LOG_SLOT_BYTES];
    memcpy(kept, window + slot_block(0) * 4096, sizeof(kept));
    const uint32_t rounds = medium().rounds;
    unsigned mark = op_count;
    psci_reset_hook = reset_lands;
    wdog_bite_hook = bite_set;
    cached = 1;
    op_ticks = HZ / 200;
    uint64_t at = clock_now;
    assert(!host_daif_masked);
    ramoops_log_restart("restart");
    assert(!host_daif_masked && pets_stopped == 1);
    assert(ops[mark].kind == 'C' && ops[mark].timeout == UFS_LOG_QUIESCE_CLAIM_US);
    assert(ops[mark + 1].kind == 'W' && ops[mark + 1].rel == slot_block(1) && ops[mark + 1].blocks == 16);
    assert(ops[mark + 2].kind == 'W' && ops[mark + 2].rel == 0 && ops[mark + 3].kind == 'S' && op_count == mark + 4);
    assert(ops_at_reset == op_count && ops_at_bite == op_count && psci_reset_at - at < HZ / 10);
    cached = 0;
    struct ufs_log_header h = medium();
    assert(h.state == UFS_LOG_STATE_RESET && h.slots_written == 2 && h.rounds == rounds + 1);
    assert(!memcmp(kept, window + slot_block(0) * 4096, sizeof(kept)));
    assert(memmem(window + slot_block(1) * 4096, UFS_LOG_SLOT_BYTES, "ufslog last snapshot before the reset: restart", 46));
    // Claimed for good: a second reset, or a panic on the way, goes straight on.
    mark = op_count;
    ufs_log_quiesce("again");
    ufs_log_panic(0);
    assert(op_count == mark);
    // The timed reset and a power off take the same way.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    ramoops_log_timer_fired();
    assert(medium().state == UFS_LOG_STATE_RESET && count_ops('C') == 1 && psci_resets);
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    ramoops_log_power_off("power off");
    assert(medium().state == UFS_LOG_STATE_RESET && count_ops('C') == 1);
    // A transport that does not come free: the wait is bounded, nothing is written, the reset comes.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    claim_refused = 1;
    mark = op_count;
    at = clock_now;
    ramoops_log_restart("restart");
    assert(op_count == mark + 1 && ops[mark].kind == 'C' && medium().state == UFS_LOG_STATE_WRITING);
    assert(psci_resets && psci_reset_at - at >= HZ && psci_reset_at - at < HZ + HZ / 10);
    // Nor does anything start after the failed claim, though the window stays open: a bite or the PSCI call is on its way.
    mark = op_count;
    emit_bytes('U', 64);
    advance_ms(3001);
    assert(ufs_log_writer_tick() && op_count == mark);
    memcpy(blob, UFS_LOG_EXPORT_MAGIC, 8);
    assert(boot_log_export_put(1, 16, 0, blob, 16) == -9 && op_count == mark);
    // A second resetter waits for the first, bounded.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    claim_hook = second_resetter;
    ramoops_log_restart("restart");
    assert(medium().state == UFS_LOG_STATE_RESET && count_ops('C') == 1);
    // A flush that takes the budget stops at it: the whole of it stays within UFS_LOG_QUIESCE_BUDGET_US.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    op_ticks = HZ * 3;
    mark = op_count;
    at = clock_now;
    ramoops_log_restart("restart");
    assert(ops[mark + 1].kind == 'W' && op_count == mark + 2 && psci_reset_at - at < 3ull * HZ + HZ / 10);
    // The last snapshot off: claimed all the same, nothing goes to the bus, the pets go on until the reset's own handling.
    boot();
    arg("pagani-ufslog-panic", 0);
    open_window();
    assert(ufs_log_writer_tick());
    mark = op_count;
    ramoops_log_restart("restart");
    assert(op_count == mark + 1 && ops[mark].kind == 'C' && !pets_stopped && psci_resets);
    // Before the window opened: a claim that comes back at once, no snapshot. A window opened with no round yet: the snapshot is slot 0.
    boot();
    start_log();
    ufs_log_start();
    open_state = 1;
    assert(!ufs_log_writer_open());
    mark = op_count;
    ramoops_log_restart("restart");
    assert(op_count == mark + 1 && ops[mark].kind == 'C' && psci_resets);
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    mark = op_count;
    ramoops_log_restart("restart");
    assert(op_count == mark + 4 && ops[mark + 1].rel == slot_block(0) && medium().slots_written == 1);
    // The ramoops log off: nothing to add, and the flush still goes out: the claim may have come in the middle of a round.
    boot();
    table_count = 0;
    arg("sheng-log", 1);
    arg("pagani-ufslog-panic", 1);
    ufs_log_start();
    assert(ufs_log_writer_open() == 1);
    mark = op_count;
    ramoops_log_restart("restart");
    assert(op_count == mark + 2 && ops[mark].kind == 'C' && ops[mark + 1].kind == 'S');
    // A window closed for good has nothing on the wire: not even a claim.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    gpt_result = 2;
    for (unsigned i = 0; i < UFS_LOG_CHECK_ROUNDS; i++) {
        emit_bytes('U', 64);
        advance_ms(3001);
        if (!ufs_log_writer_tick()) break;
    }
    assert(writer_stopped && model_closed);
    mark = op_count;
    ramoops_log_restart("restart");
    assert(op_count == mark && psci_resets);
}

// The last snapshot runs on a CPU nothing interrupts: by default only with something left that resets the phone.
static void test_backstop(void)
{
    boot();
    wdog_armed = 0;
    open_window();
    assert(!w.panic_on && !ufs_log_panic_pending());
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    struct ramoops_snapshot s;
    ramoops_log_snapshot(copy, sizeof(copy), &s);
    assert(memmem(copy, sizeof(copy), " ms, no last snapshot", 21));
    boot();
    open_window();
    assert(w.panic_on && ufs_log_panic_pending());
    boot();
    wdog_armed = 0;
    arg("pagani-reset-after", 60);
    open_window();
    assert(w.panic_on);
    boot();
    wdog_armed = 0;
    arg("pagani-ufslog-panic", 1);
    open_window();
    assert(w.panic_on);
    boot();
    arg("pagani-ufslog-panic", 0);
    open_window();
    assert(!w.panic_on);
}

static void test_ramoops_interface(void)
{
    boot();
    struct ramoops_snapshot s;
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    assert(!ramoops_log_snapshot(copy, sizeof(copy), &s) && !s.events_total && !s.sequence);   // off
    assert(!ramoops_log_reset_in_ms());
    start_log();
    assert(ramoops_log_snapshot(NULL, 0, &s) == 0xfffc);
    assert(s.events_bytes == 0x8000 && s.console_bytes == 0x6ffc && s.run == run_id && s.sequence == sequence);
    uint64_t events = s.events_total, console = s.console_total;
    ramoops_log_line('U', "x");
    assert(ramoops_log_snapshot(NULL, 0, &s) && s.events_total == events + 35 && s.console_total == console);
    ramoops_log_line('C', "xyz");
    assert(ramoops_log_snapshot(NULL, 0, &s) && s.console_total == console + 37);
    // Monotonic across the ring's turn; the copy is the region's copy 0.
    for (unsigned i = 0; i < 3000; i++) ramoops_log_line('C', "turning the console ring over and over");
    assert(ramoops_log_snapshot(copy, sizeof(copy), &s) == 0xfffc && s.console_total > 3000 * 70);
    assert(!memcmp(copy, region + RAMOOPS_LOG_ZONE_HEADER, 0xfffc));
    // cap: rounded down to whole words, nothing past it.
    static uint8_t small[100];
    memset(small, 0x55, sizeof(small));
    assert(ramoops_log_snapshot(small, 10, &s) == 0xfffc && small[7] != 0x55 && small[8] == 0x55);
}

// ---- windows for the reader's test ----------------------------------------------------------------------------------------------
static void dump_run(const char *directory, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s/window.bin", directory, name);
    FILE *out = fopen(path, "wb");
    assert(out && fwrite(window, 4096, WINDOW_BLOCKS, out) == WINDOW_BLOCKS && !fclose(out));
    snprintf(path, sizeof(path), "%s/%s/info.txt", directory, name);
    out = fopen(path, "w");
    assert(out);
    fprintf(out, "sequence=%u\nrun=%u\n", sequence, run_id);
    assert(!fclose(out));
}

static void dump(const char *directory)
{
    mkdir(directory, 0777);
    // normal: a boot's worth of both rings, a few slots, a message buffer.
    boot();
    open_window();
    memcpy(msg_ring, "kernel message one\nkernel message two\n", 38);
    test_msgbuf.msg_bufx = 38;
    assert(ufs_log_writer_tick());
    for (unsigned i = 0; i < 40; i++) {
        emit_bytes('U', 1500);
        emit_bytes('C', 4000);
        advance_ms(3001);
        assert(ufs_log_writer_tick());
    }
    dump_run(directory, "normal");
    // wrap: past the ring of slots.
    boot();
    open_window();
    assert(ufs_log_writer_tick());
    while (w.current < SLOTS + 40) {
        emit_bytes('C', 25000);
        advance_ms(3001);
        assert(ufs_log_writer_tick());
    }
    dump_run(directory, "wrap");
    // panic: the last snapshot, state 5.
    boot();
    panic_setup();
    ramoops_log_panic_reset();
    dump_run(directory, "panic");
    // reset: the last snapshot, state 6.
    boot();
    arg("pagani-reset-after", 60);
    open_window();
    assert(ufs_log_writer_tick());
    emit_bytes('U', 64);
    ramoops_log_restart("restart");
    dump_run(directory, "reset");
}

int main(int argc, char **argv)
{
    window = calloc(WINDOW_BLOCKS, 4096);
    export_area = calloc(EXPORT_BLOCKS, 4096);
    assert(window && export_area);
    test_off();
    test_open();
    test_rounds();
    test_early_ticks();
    test_advance();
    test_continuity();
    test_burst_and_deadline();
    test_power_key();
    test_failures();
    test_gpt();
    test_stats();
    test_export();
    test_power_cut();
    test_panic();
    test_reset();
    test_backstop();
    test_ramoops_interface();
    if (argc == 3 && !strcmp(argv[1], "--dump")) dump(argv[2]);
    puts("PASS: UFS log writer: off, open, rounds, slots, continuity, deadlines, failures, GPT, statistics, export, power cuts, panic, resets");
    return 0;
}
