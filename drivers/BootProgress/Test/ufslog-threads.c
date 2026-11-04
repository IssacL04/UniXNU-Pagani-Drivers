// SPDX-License-Identifier: MIT
// The UFS log with threads: the actual UfsLog.c on the actual RamoopsLog.c over a thread-safe model of NativeUfs, built with ThreadSanitizer (what
// it reports is the test's result: a counter read without the export lock, a stop not serialised with the export) and with ASan and
// UBSan. Checked besides:
// - a blob that ends during a round, with only the export lock to order its counters for that round;
// - ramoops_log_snapshot against a writer on another CPU: what the totals count is in the copy (they are read first, and move after the data);
// - the writer, the render server's export, a stream of records and a root reader together, ended by a reset, two at once, a panic, or a
//   writer that stops: nothing on the wire at the reset, nothing written after the claim or the close, a header on the medium that is whole
//   and names a whole blob.
//   cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread ufslog-threads.c -o ufslog-threads && ./ufslog-threads
#define RAMOOPS_LOG_TEST 1
#define UFS_LOG_TEST 1
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../RamoopsLog.c"
#include "../UfsLog.c"
#include "ufslog-platform.h"

// ramoops_log_snapshot takes no lock, on purpose: a record being written is torn at worst, and its CRC tells (RamoopsLog.h).
const char *__tsan_default_suppressions(void);
const char *__tsan_default_suppressions(void) { return "race:ramoops_log_snapshot\nrace:last_counted_byte\n"; }

enum { WINDOW_BLOCKS = 8192, EXPORT_BLOCKS = 65536, BLOB_MAX = 200000 };

// ---- the export lock ------------------------------------------------------------------------------------------------------
static pthread_mutex_t export_mutex = PTHREAD_MUTEX_INITIALIZER;
void *ufs_log_platform_lock_alloc(void) { return &export_mutex; }
void ufs_log_platform_lock(void *lock) { pthread_mutex_lock(lock); }
void ufs_log_platform_unlock(void *lock) { pthread_mutex_unlock(lock); }
void ufs_log_platform_pause(void) { sched_yield(); }

// ---- the model of NativeUfs ------------------------------------------------------------------------------------------------
// gHost.lock is `transport`: every command runs under it to its end, and a claim keeps it. After the claim the other threads wait
// for ever on the phone; here they give up once the test is over.
static uint8_t *window, *export_area;
static int transport, on_wire, closed, claimed, test_over, fail_rounds;
static unsigned commands, headers_written;
static int hold_round;                      // test_counters: 1 the next write to a slot waits, 2 it is waiting, 3 it goes on

static int take_transport(void)
{
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&transport, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        if (__atomic_load_n(&test_over, __ATOMIC_ACQUIRE)) return 0;
        sched_yield();
    }
}

static void give_transport(void) { __atomic_store_n(&transport, 0, __ATOMIC_RELEASE); }

// A command takes a moment on the wire, and another CPU may be anywhere then.
static void on_the_wire(void)
{
    assert(!__atomic_exchange_n(&on_wire, 1, __ATOMIC_RELAXED));
    for (unsigned i = 0; i < 3; i++) sched_yield();
    __atomic_store_n(&on_wire, 0, __ATOMIC_RELAXED);
    commands++;
}

int native_ufs_log_open(uint64_t *first, uint64_t *blocks, const char **why)
{
    (void)why;
    *first = FIRST_LBA;
    *blocks = WINDOW_BLOCKS;
    return 1;
}

int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src)
{
    int expected = 1;
    if (rel >= UFS_LOG_SLOT_FIRST && __atomic_compare_exchange_n(&hold_round, &expected, 2, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        while (__atomic_load_n(&hold_round, __ATOMIC_RELAXED) != 3) sched_yield();
    if (!take_transport()) return 0;
    assert(!__atomic_load_n(&closed, __ATOMIC_ACQUIRE) && !__atomic_load_n(&claimed, __ATOMIC_ACQUIRE));
    assert(blocks && blocks <= 32 && rel + blocks <= WINDOW_BLOCKS - 1);
    on_the_wire();
    const int fail = __atomic_load_n(&fail_rounds, __ATOMIC_ACQUIRE);
    if (!fail) memcpy(window + rel * 4096, src, blocks * 4096UL);
    if (!fail && !rel) __atomic_fetch_add(&headers_written, 1, __ATOMIC_RELAXED);
    give_transport();
    return !fail;
}

uint64_t native_ufs_export_blocks(void) { return EXPORT_BLOCKS; }

int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src)
{
    if (!take_transport()) return 0;
    assert(!__atomic_load_n(&closed, __ATOMIC_ACQUIRE) && !__atomic_load_n(&claimed, __ATOMIC_ACQUIRE));
    assert(blocks && blocks <= 64 && rel + blocks <= EXPORT_BLOCKS - 1);
    on_the_wire();
    memcpy(export_area + rel * 4096, src, blocks * 4096UL);
    give_transport();
    return 1;
}

int native_ufs_log_sync(void)
{
    if (!take_transport()) return 0;
    assert(!__atomic_load_n(&closed, __ATOMIC_ACQUIRE) && !__atomic_load_n(&claimed, __ATOMIC_ACQUIRE));
    on_the_wire();
    give_transport();
    return 1;
}

void native_ufs_log_close(void)
{
    if (!take_transport()) return;
    __atomic_store_n(&closed, 1, __ATOMIC_RELEASE);
    give_transport();
}

unsigned native_ufs_store_gpt_check(void) { return 0; }

int native_ufs_log_panic(uint32_t waitUs)
{
    assert(host_daif_masked);
    if (__atomic_load_n(&claimed, __ATOMIC_ACQUIRE)) return 1;
    const uint64_t deadline = ramoops_platform_counter() + (uint64_t)waitUs * HZ / 1000000;
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&transport, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (ramoops_platform_counter() >= deadline) return 0;
        sched_yield();
    }
    __atomic_store_n(&claimed, 1, __ATOMIC_RELEASE);
    assert(!__atomic_load_n(&on_wire, __ATOMIC_RELAXED));   // with the claim nothing is on the wire
    return 1;
}

int native_ufs_log_panic_write(uint64_t rel, uint32_t blocks, const void *src, uint32_t timeoutUs)
{
    assert(__atomic_load_n(&claimed, __ATOMIC_ACQUIRE) && timeoutUs && rel + blocks <= WINDOW_BLOCKS - 1);
    memcpy(window + rel * 4096, src, blocks * 4096UL);
    return 1;
}

int native_ufs_log_panic_sync(uint32_t timeoutUs)
{
    assert(__atomic_load_n(&claimed, __ATOMIC_ACQUIRE) && timeoutUs);
    return 1;
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------
static void boot(void)
{
    ramoops_log_suspend();
    memset(region, 0, sizeof(region));
    memset(window, 0, (size_t)WINDOW_BLOCKS * 4096);
    memset(export_area, 0, (size_t)EXPORT_BLOCKS * 4096);
    memset(&w, 0, sizeof(w));
    memset(headers, 0, sizeof(headers));
    live = NULL;
    writer_stopped = writer_started = panic_flushed = reset_coming = 0;
    last_state = LAST_IDLE;
    export_lock = NULL;
    export_stage = NULL;
    export_region = export_total = export_filled = export_generation = export_target = 0;
    export_ready = export_ready_bytes = export_ready_first = export_named = 0;
    transport = on_wire = closed = claimed = test_over = fail_rounds = hold_round = 0;
    headers_written = 0;
    psci_resets = 0;
    psci_reset_at = 0;
    table_count = 0;
    pets_stopped = wdog_flushed = 0;
    thread_starts = 0;
    arg("sheng-log", 1);
    arg("pagani-ramoops", 1);
    arg("pagani-panic-reset", 1);
    clock_step = 8;
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ufs_log_start();
}

static void generation_blob(uint8_t *out, uint32_t generation, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) out[i] = (uint8_t)(i * 3 + (i >> 8) + generation * 29);
    memcpy(out, UFS_LOG_EXPORT_MAGIC, 8);
}

static uint32_t blob_bytes(uint32_t generation) { return 6000 + generation * 7919 % (BLOB_MAX - 6000); }

// What a test leaves: a whole header, and the export it names whole.
static struct ufs_log_header check_medium(void)
{
    static uint8_t want[BLOB_MAX];
    struct ufs_log_header h;
    memcpy(&h, window, sizeof(h));
    assert(!memcmp(h.magic, UFS_LOG_MAGIC, 8) && h.header_bytes == sizeof(h));
    assert(h.crc == ufs_crc((const uint8_t *)&h, offsetof(struct ufs_log_header, crc)));
    assert(h.first_lba == FIRST_LBA);
    if (h.export_bytes) {
        assert(h.export_bytes == blob_bytes(h.export_generation) && (h.export_first == 0 || h.export_first == EXPORT_BLOCKS / 2));
        generation_blob(want, h.export_generation, h.export_bytes);
        assert(!memcmp(export_area + h.export_first * 4096UL, want, h.export_bytes));
    }
    return h;
}

// ---- the threads ---------------------------------------------------------------------------------------------------------------
static void *writer_thread_main(void *unused)
{
    (void)unused;
    const int opened = ufs_log_writer_open();
    assert(opened == 1);
    while (!__atomic_load_n(&test_over, __ATOMIC_ACQUIRE)) {
        // Time passes for this thread's sleep, but not while the way out counts its bounds, which are not what is tested here.
        if (__atomic_load_n(&last_state, __ATOMIC_ACQUIRE) == LAST_IDLE) __atomic_add_fetch(&clock_now, (uint64_t)HZ / 16, __ATOMIC_RELAXED);
        if (!ufs_log_writer_tick()) break;
        usleep(50);
    }
    return NULL;
}

static void *emitter_thread(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&test_over, __ATOMIC_ACQUIRE)) {
        ramoops_log_line('U', "a record from another CPU, about this long");
        ramoops_log_line('C', "and one for the console ring as well");
        sched_yield();
    }
    return NULL;
}

// The render server: blobs in pieces, again a little later when the kernel says not yet.
static void *export_thread(void *unused)
{
    (void)unused;
    static uint8_t blob[BLOB_MAX];
    uint32_t generation = 1;
    while (!__atomic_load_n(&test_over, __ATOMIC_ACQUIRE)) {
        const uint32_t bytes = blob_bytes(generation);
        generation_blob(blob, generation, bytes);
        int status = 0;
        for (uint32_t offset = 0; !status && offset < bytes;) {
            const uint32_t n = bytes - offset < 3968 ? bytes - offset : 3968;
            status = boot_log_export_put(generation, bytes, offset, blob + offset, n);
            offset += n;
        }
        if (!status) generation++;
        else if (status == -4 || status == -6 || status == -9) break;      // stopped, failed, or a reset on its way
        else if (status == -1) sched_yield();       // the window is not open yet
        else assert(status == -8);
        sched_yield();
    }
    return NULL;
}

static void wait_rounds(uint32_t rounds)
{
    while (__atomic_load_n(&headers_written, __ATOMIC_RELAXED) < rounds + 1) sched_yield();
}

static void reset_lands(void)
{
    assert(!__atomic_load_n(&on_wire, __ATOMIC_RELAXED) && __atomic_load_n(&claimed, __ATOMIC_ACQUIRE));
    assert(__atomic_load_n(&last_state, __ATOMIC_ACQUIRE) == LAST_DONE);
}

static void *restart_thread(void *unused)
{
    (void)unused;
    ramoops_log_restart("restart");
    return NULL;
}

enum { END_RESET, END_TWO_RESETS, END_PANIC, END_STOP };

static void scenario(int ending, uint32_t rounds)
{
    boot();
    psci_reset_hook = ending == END_STOP ? NULL : reset_lands;
    pthread_t threads[3], resetters[2];
    assert(!pthread_create(&threads[0], NULL, writer_thread_main, NULL));
    assert(!pthread_create(&threads[1], NULL, emitter_thread, NULL));
    assert(!pthread_create(&threads[2], NULL, export_thread, NULL));
    wait_rounds(rounds);
    switch (ending) {
    case END_RESET:
        assert(!pthread_create(&resetters[0], NULL, restart_thread, NULL));
        assert(!pthread_join(resetters[0], NULL));
        break;
    case END_TWO_RESETS:
        for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&resetters[i], NULL, restart_thread, NULL));
        for (unsigned i = 0; i < 2; i++) assert(!pthread_join(resetters[i], NULL));
        break;
    case END_PANIC:
        ramoops_log_panic_reset();
        break;
    default:
        __atomic_store_n(&fail_rounds, 1, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < 1u << 28 && !__atomic_load_n(&closed, __ATOMIC_ACQUIRE); i++) sched_yield();
        assert(__atomic_load_n(&closed, __ATOMIC_ACQUIRE));
        break;
    }
    if (ending != END_STOP) assert(__atomic_load_n(&psci_resets, __ATOMIC_RELAXED));
    __atomic_store_n(&test_over, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 3; i++) assert(!pthread_join(threads[i], NULL));
    const struct ufs_log_header h = check_medium();
    if (!(ending == END_STOP ? h.state == UFS_LOG_STATE_WRITING && writer_stopped :
          ending == END_PANIC ? h.state == UFS_LOG_STATE_PANIC : h.state == UFS_LOG_STATE_RESET)) {
        printf("ending %d: state %u\n", ending, h.state);
        assert(0);
    }
    assert(h.rounds >= rounds);
}

// ---- a blob that ends during a round -----------------------------------------------------------------------------------------
// In `scenario` the transport orders the threads nearly always, and hides a counter read without the export lock unless a blob happens to end
// inside the few commands before that read. Here it always does: the round waits in its slot write for the blob, on a word of relaxed
// operations, which order nothing for ThreadSanitizer; the blob's last command gave the transport back before its counters were set. Only the
// export lock orders them for the round now.
static void test_counters(void)
{
    static uint8_t blob[BLOB_MAX];
    boot();
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, writer_thread_main, NULL));
    while (!__atomic_load_n(&live, __ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&hold_round, 1, __ATOMIC_RELAXED);
    ramoops_log_line('U', "a record for the round to take");
    while (__atomic_load_n(&hold_round, __ATOMIC_RELAXED) != 2) sched_yield();
    const uint32_t bytes = blob_bytes(1);
    generation_blob(blob, 1, bytes);
    assert(!boot_log_export_put(1, bytes, 0, blob, bytes));
    const unsigned headers = __atomic_load_n(&headers_written, __ATOMIC_RELAXED);
    __atomic_store_n(&hold_round, 3, __ATOMIC_RELAXED);
    while (__atomic_load_n(&headers_written, __ATOMIC_RELAXED) == headers) sched_yield();
    __atomic_store_n(&test_over, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(writer, NULL));
    const struct ufs_log_header h = check_medium();
    assert(h.export_generation == 1 && h.export_bytes == bytes);        // the round that waited named it
    puts("ok  a blob that ends during a round: named by that round, its counters ordered by the export lock alone");
}

// ---- ramoops_log_snapshot against a writer on another CPU -------------------------------------------------------------------
// The last byte the events total counts, in copy 0, read as the snapshot reads it: a record's last byte is its newline.
__attribute__((noinline)) static uint8_t last_counted_byte(uint64_t total)
{
    const uint32_t ring = areas[AREA_EVENTS].bytes, offset = areas[AREA_EVENTS].offset;
    return copies[0][offset + (total - 1) % ring];
}

static int emitting;
static const char fixed_text[] = "records of one size, so a total says which record ends there";

static void *fixed_emitter(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&emitting, __ATOMIC_ACQUIRE)) ramoops_log_line('U', fixed_text);
    return NULL;
}

// The record that ends at `total` in a copy of the events ring: its sequence number, 0 when no whole record ends there.
static uint32_t record_ending_at(const uint8_t *copy, uint64_t total, unsigned size)
{
    static uint8_t bytes[512];
    const uint32_t ring = areas[AREA_EVENTS].bytes, offset = areas[AREA_EVENTS].offset;
    for (unsigned i = 0; i < size; i++) bytes[i] = copy[offset + (total - size + i) % ring];
    unsigned length = 0;
    const uint32_t seq = record_at(bytes, &length);
    return length == size ? seq : 0;
}

static void test_snapshot(void)
{
    static uint8_t copy[RAMOOPS_LOG_COPY_LIMIT];
    boot();
    emitting = 1;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, fixed_emitter, NULL));
    struct ramoops_snapshot s;
    // Past one turn of the ring, so a record that is not there yet shows as an older one rather than as nothing.
    do { ramoops_log_snapshot(NULL, 0, &s); sched_yield(); } while (s.events_total < 3ull * s.events_bytes);
    const unsigned size = RAMOOPS_LOG_HEADER_BYTES + (unsigned)strlen(fixed_text) + 1;
    uint64_t base_total = 0;
    uint32_t base_seq = 0;
    unsigned checked = 0, torn = 0;
    for (unsigned i = 0; i < 4000; i++) {
        ramoops_log_snapshot(copy, sizeof(copy), &s);
        const uint32_t seq = record_ending_at(copy, s.events_total, size);
        // The ring's size is no multiple of a record's, so what lay there a turn ago seldom lines up: a total ahead of the copy shows here more
        // often than as an older record. In order, only a turn of the ring during the copy does this, which takes a descheduled reader.
        if (!seq) { torn++; continue; }
        if (!base_seq) {
            base_seq = seq;
            base_total = s.events_total;
            continue;
        }
        // Newer is fine (the ring went on while the copy ran); older means the total counted a record that was not in the copy yet.
        assert(seq - base_seq >= (s.events_total - base_total) / size);
        checked++;
    }
    // The total moves after the data: whatever moment it is read at, the byte it counts last is there. (Unless a whole turn of the ring went by
    // meanwhile, which takes a descheduled reader: the second reading tells.)
    const uint64_t began = __atomic_load_n(&areas[AREA_EVENTS].total, __ATOMIC_ACQUIRE);
    unsigned bytes_checked = 0;
    do {
        const uint64_t total = __atomic_load_n(&areas[AREA_EVENTS].total, __ATOMIC_ACQUIRE);
        const uint8_t byte = last_counted_byte(total);
        if (__atomic_load_n(&areas[AREA_EVENTS].total, __ATOMIC_ACQUIRE) - total > areas[AREA_EVENTS].bytes / 2) continue;
        assert(byte == '\n');
        bytes_checked++;
    } while (__atomic_load_n(&areas[AREA_EVENTS].total, __ATOMIC_ACQUIRE) - began < 2000 * size);
    __atomic_store_n(&emitting, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(checked > 1000 && torn < 40 && bytes_checked > 2000);
    printf("ok  snapshot: %u checked, %u copies without a whole record at the total\n", checked, torn);
}

int main(void)
{
    window = calloc(WINDOW_BLOCKS, 4096);
    export_area = calloc(EXPORT_BLOCKS, 4096);
    assert(window && export_area);
    test_snapshot();
    test_counters();
    for (unsigned turn = 0; turn < 6; turn++) {
        for (int ending = END_RESET; ending <= END_STOP; ending++) scenario(ending, 6 + turn * 7);
        printf("ok  turn %u: reset, two resets, panic, stop\n", turn);
    }
    puts("PASS: UFS log with threads: snapshot order, writer, export, records and resets together");
    return 0;
}
