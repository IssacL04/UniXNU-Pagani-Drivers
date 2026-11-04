// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of CrashTrace.h (the X records of the ramoops log): the frame chain walk on simulated user memory (a normal
// chain, the 16-frame limit, unreadable, misaligned, out-of-range, non-climbing and far-jumping frame pointers, Swift async
// and pointer-authentication bits, a zero return address, no frame pointer), the naming of addresses (shared cache unslid,
// main executable, images with and without a Mach-O header, raw; the image and lookup limits), the signal filter and the ABI
// guard, the rate limit (10 s per name, "same" records within 30 s, skipped counts, the name table's eviction, pagani-crash=0
// and =2, a lock that cannot be taken), the record lengths with worst-case values, and the records through the actual
// RamoopsLog.c writer (kind X in the events ring, CRC checked).
//   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined crash.c -o crash && ./crash
//   ./crash --dump DIR   also writes the two zone images of a run with crash reports and expected.tsv (gpu-test-loop's
//                        decoder and symbolize-crashes.py tests)
#define RAMOOPS_LOG_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../RamoopsLog.c"
#include "../CrashTrace.h"

// ---- RamoopsLog platform (as Test/ramoops.c, reduced) ------------------------------------------------------------------
enum { REGION = RAMOOPS_LOG_DEFAULT_CONSOLE + RAMOOPS_LOG_DEFAULT_PMSG, HZ = 19200000 };
static uint8_t *region;
static uint64_t clock_now = 1000000;
uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{
    (void)pa; (void)device;
    assert(bytes <= REGION);
    memset(region, 0xa5, REGION);
    return (uintptr_t)region;
}
int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    if (strcmp(name, "pagani-ramoops")) return 0;
    assert(bytes == 4);
    uint32_t one = 1;
    memcpy(value, &one, 4);
    return 1;
}
const char *ramoops_platform_boot_args(void) { return "-v pagani-ramoops=1"; }
uint64_t ramoops_platform_counter(void) { return clock_now += 19200; }
uint64_t ramoops_platform_frequency(void) { return HZ; }
uint32_t ramoops_platform_cpu_mhz(int *cpu) { *cpu = 0; return 1000; }
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{ (void)hvc; (void)function; (void)a; (void)b; (void)c; return -1; }
int ramoops_platform_psci_method(char method[4]) { (void)method; return 0; }
int ramoops_platform_arm_timer(uint64_t milliseconds) { (void)milliseconds; return 0; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid) { (void)milliseconds; (void)avoid; return -1; }
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = 0x8c0000000ULL; *bytes = 0x100000000ULL; }
// No pagani-reset-seq or pagani-reset-wdog here: an SCM call, a CPU park or a watchdog call fails the test.
void ramoops_platform_smc(uint64_t regs[8]) { (void)regs; assert(0); }
unsigned ramoops_platform_park_others(uint32_t *parked) { (void)parked; assert(0); return 0; }
int ramoops_platform_wdog_bite(const char *reason) { (void)reason; assert(0); return 0; }
int ramoops_platform_wdog_hold(const char *reason) { (void)reason; assert(0); return 0; }
void ramoops_platform_wdog_release(const char *reason) { (void)reason; assert(0); }
void ramoops_platform_wdog_flushed(void) {}

// ---- simulated user memory and mappings ---------------------------------------------------------------------------------
struct block { uint64_t base; uint32_t bytes; uint8_t *data; };
static struct block blocks[8];
static unsigned block_count, reads, bad_reads;
static uint64_t read_limit;                 // no read may touch [read_limit, ...)

static void memory_reset(void)
{
    for (unsigned i = 0; i < block_count; i++) free(blocks[i].data);
    block_count = 0;
    reads = bad_reads = 0;
    read_limit = 0;
}

static uint8_t *memory_add(uint64_t base, uint32_t bytes)
{
    assert(block_count < 8);
    blocks[block_count].base = base;
    blocks[block_count].bytes = bytes;
    blocks[block_count].data = calloc(1, bytes);
    assert(blocks[block_count].data);
    return blocks[block_count++].data;
}

static void put64(uint64_t address, uint64_t value)
{
    for (unsigned i = 0; i < block_count; i++)
        if (address >= blocks[i].base && address + 8 <= blocks[i].base + blocks[i].bytes) {
            memcpy(blocks[i].data + (address - blocks[i].base), &value, 8);
            return;
        }
    assert(!"put64 outside the simulated memory");
}

// A frame record {next fp, return address} at fp.
static void frame(uint64_t fp, uint64_t next, uint64_t ret) { put64(fp, next); put64(fp + 8, ret); }

static int test_read(void *context, uint64_t address, void *buffer, uint32_t bytes)
{
    assert(context == (void *)0x5eed);
    reads++;
    if (read_limit && address + bytes > read_limit) bad_reads++;
    for (unsigned i = 0; i < block_count; i++)
        if (address >= blocks[i].base && address + bytes <= blocks[i].base + blocks[i].bytes) {
            memcpy(buffer, blocks[i].data + (address - blocks[i].base), bytes);
            return 0;
        }
    return -1;                              // EFAULT
}

struct mapping { uint64_t start, end, offset; const char *name; };
static struct mapping mappings[12];
static unsigned mapping_count, lookups;

static int test_image(void *context, uint64_t address, struct boot_progress_crash_image *image)
{
    assert(context == (void *)0x5eed);
    lookups++;
    for (unsigned i = 0; i < mapping_count; i++)
        if (address >= mappings[i].start && address < mappings[i].end) {
            image->start = mappings[i].start;
            image->end = mappings[i].end;
            image->offset = mappings[i].offset;
            snprintf(image->name, sizeof(image->name), "%s", mappings[i].name);
            return 1;
        }
    return 0;
}

static void mapping(uint64_t start, uint64_t end, uint64_t offset, const char *name, int header)
{
    assert(mapping_count < 12);
    mappings[mapping_count++] = (struct mapping){start, end, offset, name};
    if (header) {
        uint8_t *data = memory_add(start, 64);
        uint32_t magic = CRASH_MH_MAGIC_64;
        memcpy(data, &magic, 4);
    }
}

// ---- records ------------------------------------------------------------------------------------------------------------
static char lines[64][CRASH_LINE + 2];
static unsigned line_count;
static void collect(void *context, const char *line)
{
    (void)context;
    assert(line_count < 64);
    size_t n = strlen(line);
    assert(n > 0 && n <= CRASH_LINE && n < RAMOOPS_LOG_TEXT_MAX);
    for (size_t i = 0; i < n; i++) assert(line[i] >= 0x20 && line[i] <= 0x7e);
    memcpy(lines[line_count++], line, n + 1);
}
static unsigned locks, unlocks;
static int lock_fails;
static uint64_t test_lock(void) { if (lock_fails) return UINT64_MAX; locks++; return 7; }
static void test_unlock(uint64_t flags) { assert(flags == 7); unlocks++; }
static const struct crash_sink sink = {collect, test_lock, test_unlock, NULL};

static struct crash_table table;
static void lines_reset(void) { line_count = 0; }

static const char *find_line(const char *prefix)
{
    for (unsigned i = 0; i < line_count; i++) if (!strncmp(lines[i], prefix, strlen(prefix))) return lines[i];
    return NULL;
}

static void print_lines(const char *title)
{
    printf("  %s\n", title);
    for (unsigned i = 0; i < line_count; i++) printf("    X %s\n", lines[i]);
}

// The layout every test starts from: a 64 GiB user space (2^36), the shared region at 0x180000000 + 3 GiB with slide
// 0x2a7c000, the main executable at 0x100a4c000 (its __TEXT mapping, 0x40000 bytes), the stack at 0x16f000000.
enum { STACK = 0x16f000000ULL, STACK_BYTES = 0x40000 };
static const uint64_t SLIDE = 0x2a7c000, CACHE = 0x180000000ULL, MAIN = 0x100a4c000ULL;

static void crash_init(struct boot_progress_crash *c, int pid, const char *name, int signal, uint64_t time_ms)
{
    memset(c, 0, sizeof(*c));
    c->size = sizeof(*c);
    c->version = BOOT_PROGRESS_CRASH_VERSION;
    c->pid = pid;
    c->signal = signal;
    c->name = name;
    c->time_ms = time_ms;
    c->thread_id = 0x2b5;
    c->user_limit = 0x0000000fc0000000ULL;
    c->cache_start = CACHE;
    c->cache_end = CACHE + 0xc0000000ULL;
    c->cache_slide = SLIDE;
    c->main_header = MAIN;
    c->context = (void *)0x5eed;
    c->read = test_read;
    c->image = test_image;
    c->sp = STACK + 0x100;
}

static void world_reset(void)
{
    memory_reset();
    mapping_count = lookups = 0;
    memory_add(STACK, STACK_BYTES);
    mapping(MAIN, MAIN + 0x40000, 0x8000, "SpringBoard", 1);     // a fat file: its arm64 slice at 0x8000
}

// ---- tests ----------------------------------------------------------------------------------------------------------------
static void test_walk_normal(void)
{
    world_reset();
    mapping(0x104000000ULL, 0x104200000ULL, 0, "NativeAdrenoMetal", 1);
    mapping(0x105000000ULL, 0x105100000ULL, 0x24000, "libvulkan_freedreno.dylib", 0);    // a mapping without the header
    struct boot_progress_crash c;
    crash_init(&c, 797, "SpringBoard", 11, 1000);
    c.pc = 0x1a9463e64ULL + SLIDE;          // in the cache
    c.lr = MAIN + 0x1234;
    c.fp = STACK + 0x200;
    c.exception = 1; c.code = 2; c.subcode = 0x10;
    c.esr = 0x92000006; c.far = 0x10; c.cpsr = 0x60000000;
    c.x0 = 0; c.x1 = 0x1d2e3f400ULL + SLIDE;
    frame(STACK + 0x200, STACK + 0x280, 0x1b0000010ULL + SLIDE);
    frame(STACK + 0x280, STACK + 0x300, MAIN + 0x5678);
    frame(STACK + 0x300, STACK + 0x380, 0x104001234ULL);
    frame(STACK + 0x380, STACK + 0x400, 0x105000100ULL);
    frame(STACK + 0x400, STACK + 0x480, 0x900001234ULL);      // JIT or garbage: raw
    frame(STACK + 0x480, 0, 0x1b0000020ULL + SLIDE);          // the thread's first frame: next fp 0
    lines_reset();
    struct crash_table fresh = {0};
    table = fresh;
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    print_lines("normal chain");
    assert(line_count == 5);
    assert(!strcmp(lines[0], "crash pid=797 SpringBoard sig=11 exc=1 code=0x2 sub=0x10 esr=0x92000006 far=0x10 tid=0x2b5"));
    assert(!strcmp(lines[1], "regs pid=797 pc=c:1a9463e64 lr=m:1234 fp=0x16f000200 sp=0x16f000100 x0=0x0 x1=c:1d2e3f400 "
                             "cpsr=0x60000000"));
    static const char map_prefix[] = "map pid=797 slide=0x2a7c000 main=0x100a4c000 hash=";
    assert(!strncmp(lines[2], map_prefix, strlen(map_prefix)) && strstr(lines[2], " skipped=0"));
    assert(!strcmp(lines[3], "img pid=797 m=SpringBoard a=NativeAdrenoMetal b=libvulkan_freedreno.dylib"));
    assert(!strcmp(lines[4], "bt pid=797 0 c:1b0000010 m:5678 a:1234 b@24100 ?:900001234 c:1b0000020 end=end"));
    assert(reads == 6 + 3);                 // six records, three header checks
    assert(lookups == 4);                   // main (lr), a, b, and the raw address
}

static void test_walk_limits(void)
{
    struct boot_progress_crash c;
    struct crash_trace t;
    // More than CRASH_FRAMES records: 16 taken, the walk stops at the 17th record's address ("max").
    world_reset();
    crash_init(&c, 1, "Deep", 11, 0);
    c.fp = STACK + 0x1000;
    for (unsigned i = 0; i < 40; i++) frame(STACK + 0x1000 + 0x40 * i, STACK + 0x1000 + 0x40 * (i + 1), 0x1b0000000ULL + SLIDE + 4 * i);
    crash_take(&c, &t);
    assert(t.frames == CRASH_FRAMES && t.stop == CRASH_STOP_MAX && t.stop_at == STACK + 0x1000 + 0x40 * 16);
    assert(reads == CRASH_FRAMES);
    for (unsigned i = 0; i < CRASH_FRAMES; i++) assert(t.frame[i].kind == CRASH_CACHE && t.frame[i].value == 0x1b0000000ULL + 4 * i);
    // An unreadable frame record (the second one points outside the simulated memory): one frame, "read".
    world_reset();
    crash_init(&c, 1, "Read", 11, 0);
    c.fp = STACK + 0x100;
    frame(STACK + 0x100, STACK + STACK_BYTES + 0x100, 0x1b0000000ULL + SLIDE);
    crash_take(&c, &t);
    assert(t.frames == 1 && t.stop == CRASH_STOP_READ && t.stop_at == STACK + STACK_BYTES + 0x100);
    // Misaligned frame pointers, first and later.
    crash_init(&c, 1, "Align", 11, 0);
    c.fp = STACK + 0x104;
    reads = 0;
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_ALIGN && t.stop_at == STACK + 0x104 && reads == 0);
    frame(STACK + 0x100, STACK + 0x13c, 0x1b0000000ULL + SLIDE);
    c.fp = STACK + 0x100;
    crash_take(&c, &t);
    assert(t.frames == 1 && t.stop == CRASH_STOP_ALIGN && t.stop_at == STACK + 0x13c);
    // Outside the user address space, and in page zero: nothing is read.
    reads = 0;
    c.fp = 0x0000000fc0000000ULL;
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_RANGE && reads == 0);
    c.fp = 0x0000000fbffffff8ULL;           // 8 bytes below the limit: the record would cross it
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_RANGE && reads == 0);
    c.fp = 0x10;
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_RANGE && reads == 0);
    // A kernel-looking fp keeps only its user bits; read_limit asserts that no read crossed the user space.
    world_reset();
    read_limit = 0x0000000fc0000000ULL;
    crash_init(&c, 1, "Kernel", 11, 0);
    c.fp = 0xfffffff000000000ULL | (STACK + 0x100);
    frame(STACK + 0x100, 0xfffffff007004000ULL, 0x1b0000000ULL + SLIDE);
    crash_take(&c, &t);
    assert(bad_reads == 0 && t.frames == 1);
    assert(t.stop == CRASH_STOP_ORDER || t.stop == CRASH_STOP_READ || t.stop == CRASH_STOP_RANGE);
    // A chain that does not climb: the same record again (a loop), and one below.
    world_reset();
    crash_init(&c, 1, "Loop", 11, 0);
    c.fp = STACK + 0x200;
    frame(STACK + 0x200, STACK + 0x200, 0x1b0000000ULL + SLIDE);
    crash_take(&c, &t);
    assert(t.frames == 1 && t.stop == CRASH_STOP_ORDER && t.stop_at == STACK + 0x200 && reads == 1);
    frame(STACK + 0x200, STACK + 0x100, 0x1b0000000ULL + SLIDE);
    crash_take(&c, &t);
    assert(t.frames == 1 && t.stop == CRASH_STOP_ORDER && t.stop_at == STACK + 0x100);
    // A jump of more than CRASH_STEP_MAX.
    frame(STACK + 0x200, STACK + 0x200 + CRASH_STEP_MAX + 16, 0x1b0000000ULL + SLIDE);
    crash_take(&c, &t);
    assert(t.frames == 1 && t.stop == CRASH_STOP_GAP);
    // A zero return address ends the chain without a frame; so does fp 0.
    frame(STACK + 0x200, STACK + 0x300, 0);
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_END);
    c.fp = 0;
    reads = 0;
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_END && reads == 0);
    // No reader at all: "read" at the first record.
    c.fp = STACK + 0x200;
    c.read = NULL;
    crash_take(&c, &t);
    assert(t.frames == 0 && t.stop == CRASH_STOP_READ);
    printf("ok  frame chain limits: 16 frames, read, align, range, order, gap, zero return, no fp\n");
}

static void test_walk_bits(void)
{
    struct boot_progress_crash c;
    struct crash_trace t;
    // Swift async frames (bit 60 of the saved fp) and signed return addresses (bits above the user space) are stripped.
    world_reset();
    crash_init(&c, 1, "Swift", 11, 0);
    c.fp = STACK + 0x100;
    frame(STACK + 0x100, CRASH_SWIFT_ASYNC_BIT | (STACK + 0x180), 0x2a00000000000000ULL | (0x1b0000040ULL + SLIDE));
    frame(STACK + 0x180, 0, 0x1b0000080ULL + SLIDE);
    crash_take(&c, &t);
    assert(t.frames == 2 && t.async == 1 && t.stop == CRASH_STOP_END);
    assert(t.frame[0].kind == CRASH_CACHE && t.frame[0].value == 0x1b0000040ULL);
    assert(t.frame[1].kind == CRASH_CACHE && t.frame[1].value == 0x1b0000080ULL);
    // The mask follows user_limit: 2^47 when none is given.
    c.user_limit = 0;
    assert(crash_user_mask(&c) == (1ULL << 47) - 1);
    c.user_limit = 0x0000000fc0000000ULL;
    assert(crash_user_mask(&c) == 0xfffffffffULL);
    c.user_limit = 0x0000800000000000ULL;
    assert(crash_user_mask(&c) == (1ULL << 47) - 1);
    printf("ok  frame chain bits: Swift async frames, pointer authentication, the user mask\n");
}

static void test_naming(void)
{
    struct boot_progress_crash c;
    struct crash_trace t;
    // Five images: the first four get a-d (the main executable among them as m), the fifth stays raw; lookups stop at 8.
    world_reset();
    for (unsigned i = 0; i < 6; i++) {
        static char names[6][16];
        snprintf(names[i], sizeof(names[i]), "lib%u.dylib", i);
        mapping(0x110000000ULL + 0x1000000ULL * i, 0x110100000ULL + 0x1000000ULL * i, 0, names[i], 1);
    }
    crash_init(&c, 5, "Images", 6, 0);
    c.pc = 0x110000010ULL;
    c.lr = 0x111000020ULL;
    c.fp = STACK + 0x100;
    frame(STACK + 0x100, STACK + 0x140, 0x112000030ULL);
    frame(STACK + 0x140, STACK + 0x180, MAIN + 0x40);           // main: the fourth image slot
    frame(STACK + 0x180, STACK + 0x1c0, 0x113000050ULL);        // a fifth image: no slot left
    frame(STACK + 0x1c0, STACK + 0x200, 0x110000060ULL);        // back in the first: no lookup
    frame(STACK + 0x200, 0, 0x900000000ULL);                    // nowhere: no slot left either
    lookups = 0;
    crash_take(&c, &t);
    assert(t.images.count == CRASH_IMAGES && lookups == 4);
    assert(t.pc.kind == CRASH_IMAGE && t.pc.image == 0 && t.pc.value == 0x10);
    assert(t.lr.kind == CRASH_IMAGE && t.lr.image == 1 && t.lr.value == 0x20);
    assert(t.frame[0].kind == CRASH_IMAGE && t.frame[0].image == 2 && t.frame[0].value == 0x30);
    assert(t.frame[1].kind == CRASH_MAIN && t.frame[1].value == 0x40 && t.images.main[3]);
    assert(t.pc.letter == 'a' && t.lr.letter == 'b' && t.frame[0].letter == 'c' && t.images.letter[3] == 'm');
    assert(t.frame[2].kind == CRASH_RAW && t.frame[2].value == 0x113000050ULL);
    assert(t.frame[3].kind == CRASH_IMAGE && t.frame[3].image == 0 && t.frame[3].value == 0x60);
    assert(t.frame[4].kind == CRASH_RAW);
    // Addresses outside every mapping use up the lookups (8), then stay raw without asking.
    world_reset();
    crash_init(&c, 6, "Lookups", 11, 0);
    c.fp = STACK + 0x100;
    for (unsigned i = 0; i < 12; i++) frame(STACK + 0x100 + 0x40 * i, i == 11 ? 0 : STACK + 0x140 + 0x40 * i, 0x900000000ULL + 0x100 * i);
    lookups = 0;
    crash_take(&c, &t);
    assert(t.frames == 12 && lookups == CRASH_LOOKUPS && t.images.count == 0);
    for (unsigned i = 0; i < 12; i++) assert(t.frame[i].kind == CRASH_RAW);
    // No image callback, no shared region, no main executable: everything raw, nothing asked.
    world_reset();
    crash_init(&c, 7, "Bare", 11, 0);
    c.image = NULL;
    c.cache_end = 0;
    c.main_header = 0;
    c.pc = 0x1a9463e64ULL + SLIDE;
    c.x1 = 0x1d2e3f400ULL + SLIDE;
    crash_take(&c, &t);
    assert(t.pc.kind == CRASH_RAW && t.pc.value == 0x1a9463e64ULL + SLIDE && t.x1.kind == CRASH_RAW);
    // The hash ignores where raw addresses landed (ASLR) but not what the cache addresses are.
    world_reset();
    crash_init(&c, 8, "Hash", 11, 0);
    c.pc = 0x1a9463e64ULL + SLIDE;
    c.lr = 0x900000100ULL;
    crash_take(&c, &t);
    const uint32_t first = t.hash;
    c.lr = 0x910000200ULL;
    c.pc = 0x1a9463e64ULL + SLIDE + 0x4000;     // another slide, same unslid pc
    c.cache_slide = SLIDE + 0x4000;
    crash_take(&c, &t);
    assert(t.hash == first);
    c.pc += 4;
    crash_take(&c, &t);
    assert(t.hash != first);
    printf("ok  naming: cache, main, images with and without headers, the image and lookup limits, the hash\n");
}

static void test_filter_and_guard(void)
{
    struct boot_progress_crash c;
    world_reset();
    struct crash_table fresh = {0};
    table = fresh;
    const int quiet[] = {1, 2, 3, 9, 13, 14, 15, 24, 25, 30, 31, 0, -1};
    for (unsigned i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        crash_init(&c, 10, "Quiet", quiet[i], 0);
        lines_reset();
        crash_report(&table, &c, CRASH_MODE_ON, &sink);
        assert(line_count == 0);
    }
    const int loud[] = {4, 5, 6, 7, 8, 10, 11, 12};
    for (unsigned i = 0; i < sizeof(loud) / sizeof(loud[0]); i++) {
        char name[16];
        snprintf(name, sizeof(name), "Sig%d", loud[i]);
        crash_init(&c, 20 + loud[i], name, loud[i], 0);
        lines_reset();
        crash_report(&table, &c, CRASH_MODE_ON, &sink);
        assert(line_count >= 4 && find_line("crash ") && find_line("bt "));
    }
    crash_init(&c, 30, "Guard", 11, 0);
    c.size = sizeof(c) - 8;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 0);
    crash_init(&c, 30, "Guard", 11, 0);
    c.version = 2;
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 0);
    crash_init(&c, 30, "Guard", 11, 0);
    crash_report(&table, &c, CRASH_MODE_OFF, &sink);
    crash_report(&table, NULL, CRASH_MODE_ON, &sink);
    assert(line_count == 0);
    printf("ok  signal filter (ILL TRAP ABRT EMT FPE BUS SEGV SYS only), the ABI guard, pagani-crash=0\n");
}

static void test_rate_limit(void)
{
    struct boot_progress_crash c;
    world_reset();
    struct crash_table fresh = {0};
    table = fresh;
    // SpringBoard every 5.4 s with the same trace, PosterBoard twice every 10 s.
    unsigned full = 0, same = 0, quiet = 0, posters = 0;
    char last_same[CRASH_LINE + 2] = "";
    for (unsigned k = 0; k < 40; k++) {
        crash_init(&c, 800 + (int)k, "SpringBoard", 11, 5400ULL * k);
        c.pc = 0x1a9463e64ULL + SLIDE;
        lines_reset();
        crash_report(&table, &c, CRASH_MODE_ON, &sink);
        if (!line_count) quiet++;
        else if (find_line("same ")) { same++; snprintf(last_same, sizeof(last_same), "%s", lines[0]); assert(line_count == 1); }
        else { full++; assert(find_line("crash pid=") && find_line("bt pid=")); }
        if (k % 2 == 0) {
            for (unsigned twice = 0; twice < 2; twice++) {
                crash_init(&c, 2000 + (int)(2 * k + twice), "PosterBoard", 6, 5400ULL * k + 100 * twice);
                c.pc = 0x1a0000000ULL + SLIDE;
                lines_reset();
                crash_report(&table, &c, CRASH_MODE_ON, &sink);
                posters += line_count != 0;
            }
        }
    }
    // 40 crashes over 210.6 s: one report per 10.8 s (every second crash); a full one when the name's last full report is at
    // least CRASH_REPEAT_MS (30 s) old, so every third report (0, 32.4, 64.8 ... 194.4 s), else "same".
    printf("  SpringBoard: %u full, %u same, %u left out; PosterBoard reports %u of 40; last: %s\n", full, same, quiet, posters,
           last_same);
    assert(quiet == 20 && full + same == 20 && full == 7 && same == 13);
    assert(strstr(last_same, " skipped=1") && strstr(last_same, "same pid=838 SpringBoard sig=11 hash=") &&
           strstr(last_same, " as=836 "));
    assert(posters == 20);
    // A different trace is a full report; the first after the quiet time says how many it missed.
    crash_init(&c, 900, "SpringBoard", 11, 300000);
    c.pc = 0x1a9463e68ULL + SLIDE;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=900") && strstr(find_line("map pid=900"), " skipped=1"));     // pid 839, 5.4 s after 838
    for (unsigned k = 1; k <= 3; k++) {
        crash_init(&c, 900 + (int)k, "SpringBoard", 11, 300000 + 1000 * k);
        lines_reset();
        crash_report(&table, &c, CRASH_MODE_ON, &sink);
        assert(line_count == 0);
    }
    crash_init(&c, 910, "SpringBoard", 11, 311000);
    c.pc = 0x1a9463e6cULL + SLIDE;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(strstr(find_line("map pid=910"), " skipped=3"));     // a different trace 11 s after pid 900's full report: full
    // The same trace again within CRASH_REPEAT_MS of its full report: one "same" record pointing at it; once that full report
    // is CRASH_REPEAT_MS old, a full report again (the events ring keeps only about a minute: the newest full report must
    // still be in it).
    crash_init(&c, 911, "SpringBoard", 11, 311000 + CRASH_QUIET_MS);
    c.pc = 0x1a9463e6cULL + SLIDE;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 1 && strstr(lines[0], "same pid=911 SpringBoard sig=11 hash=") && strstr(lines[0], " as=910 skipped=0"));
    crash_init(&c, 912, "SpringBoard", 11, 311000 + CRASH_REPEAT_MS - 1);
    c.pc = 0x1a9463e6cULL + SLIDE;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 1 && strstr(lines[0], "same pid=912 ") && strstr(lines[0], " as=910 "));
    crash_init(&c, 913, "SpringBoard", 11, 311000 + CRASH_REPEAT_MS - 1 + CRASH_QUIET_MS);
    c.pc = 0x1a9463e6cULL + SLIDE;
    lines_reset();
    crash_report(&table, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=913") && find_line("bt pid=913"));
    // pagani-crash=2: every crash, always in full.
    for (unsigned k = 0; k < 3; k++) {
        crash_init(&c, 920 + (int)k, "SpringBoard", 11, 311000 + k);
        c.pc = 0x1a9463e6cULL + SLIDE;
        lines_reset();
        crash_report(&table, &c, CRASH_MODE_ON | CRASH_MODE_ALL, &sink);
        assert(find_line("crash pid=") && strstr(find_line("map pid="), " all"));
    }
    // As many names as the table holds crashing together (the phone identity's dozen poster extensions fit) all stay
    // rate-limited; one name more pushes out the one reported longest ago (it reports again at once).
    _Static_assert(CRASH_NAMES >= 16, "the poster extensions must fit the rate limit's table");
    struct crash_table names = {0};
    for (unsigned k = 0; k < CRASH_NAMES; k++) {
        char name[16];
        snprintf(name, sizeof(name), "Proc%u", k);
        crash_init(&c, 100 + (int)k, name, 11, 1000 + k);
        lines_reset();
        crash_report(&names, &c, CRASH_MODE_ON, &sink);
        assert(line_count);
    }
    for (unsigned k = 0; k < CRASH_NAMES; k++) {
        char name[16];
        snprintf(name, sizeof(name), "Proc%u", k);
        crash_init(&c, 150 + (int)k, name, 11, 1500 + k);
        lines_reset();
        crash_report(&names, &c, CRASH_MODE_ON, &sink);
        assert(line_count == 0);
    }
    char extra[16];
    snprintf(extra, sizeof(extra), "Proc%u", (unsigned)CRASH_NAMES);
    crash_init(&c, 199, extra, 11, 1900);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=199"));
    crash_init(&c, 200, "Proc0", 11, 2000);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=200"));
    crash_init(&c, 201, extra, 11, 2001);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 0);
    // Names compare on 16 characters (what the S exit record prints).
    crash_init(&c, 300, "PridePosterExtension", 11, 50000);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=300 PridePosterExten sig=11"));
    crash_init(&c, 301, "PridePosterExtensionB", 11, 50001);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(line_count == 0);
    // A lock that cannot be taken: the report still comes, in full, and the table is left alone.
    lock_fails = 1;
    unsigned before = locks;
    crash_init(&c, 400, "PridePosterExtension", 11, 50002);
    lines_reset();
    crash_report(&names, &c, CRASH_MODE_ON, &sink);
    assert(find_line("crash pid=400") && locks == before);
    lock_fails = 0;
    assert(locks == unlocks);
    printf("ok  rate limit: 10 s per name, same within 30 s, skipped counts, pagani-crash=2, eviction, 16-character names, "
           "lock failure\n");
}

static void test_lengths(void)
{
    // Worst case values: every record stays within CRASH_LINE (collect asserts it) and the bt records keep all frames.
    world_reset();
    for (unsigned i = 0; i < 4; i++) {
        static char names[4][64];
        snprintf(names[i], sizeof(names[i]), "AVeryLongImageNameThatKeepsGoing%u.dylib", i);
        mapping(0x200000000ULL + 0x10000000ULL * i, 0x200100000ULL + 0x10000000ULL * i, 0x123456789ULL, names[i], 0);
    }
    struct boot_progress_crash c;
    crash_init(&c, -2147483647 - 1, "A name\twith spaces and\x01junk", 11, 0);
    c.user_limit = 0;                       // a 2^47 mask: 12-digit raw tokens
    c.cache_end = 0;
    c.main_header = 0;
    c.exception = -2147483647 - 1;
    c.code = c.subcode = c.far = c.thread_id = c.x0 = c.x1 = UINT64_MAX;
    c.esr = c.cpsr = UINT32_MAX;
    c.pc = 0x200000010ULL;
    c.lr = 0x210000010ULL;
    c.fp = STACK + 0x100;
    for (unsigned i = 0; i < 20; i++)
        frame(STACK + 0x100 + 0x40 * i, STACK + 0x140 + 0x40 * i, i < 2 ? 0x220000000ULL + 0x10000000ULL * i + 0x40 : 0x7fffffff0000ULL + i);
    lines_reset();
    struct crash_table fresh = {0};
    crash_report(&fresh, &c, CRASH_MODE_ON | CRASH_MODE_ALL, &sink);
    print_lines("worst case");
    unsigned frames = 0;
    for (unsigned i = 0; i < line_count; i++) {
        if (strncmp(lines[i], "bt ", 3)) continue;
        char copy[CRASH_LINE + 2];
        snprintf(copy, sizeof(copy), "%s", lines[i]);
        unsigned field = 0;
        for (char *token = strtok(copy, " "); token; token = strtok(NULL, " "), field++)
            if (field >= 3 && strncmp(token, "end=", 4)) frames++;
    }
    assert(frames == CRASH_FRAMES && strstr(lines[line_count - 1], " end=max@0x"));
    assert(find_line("crash pid=-2147483648 A_name?with_spac sig=11"));
    assert(find_line("img pid=-2147483648 a=AVeryLongImageNameThatKeepsGoing0.dylib"));
    printf("ok  record lengths with worst-case values (all within %u characters)\n", CRASH_LINE);
}

// ---- through RamoopsLog.c ------------------------------------------------------------------------------------------------
static void ramoops_emit(void *context, const char *line) { (void)context; collect(NULL, line); ramoops_log_line('X', line); }
static const struct crash_sink ramoops_sink = {ramoops_emit, test_lock, test_unlock, NULL};

struct rec { uint32_t seq; char kind; char text[RAMOOPS_LOG_TEXT_MAX + 1]; };
static unsigned hexval(const uint8_t *p, unsigned n, uint32_t *out)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t ch = p[i];
        if (ch >= '0' && ch <= '9') v = v * 16 + (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v = v * 16 + (uint32_t)(ch - 'a' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

// The CRC-checked records of one ring of copy 0.
static unsigned ring_records(unsigned which, struct rec *out, unsigned room)
{
    static uint8_t data[0x10000 + 512];
    const uint8_t *area = (const uint8_t *)copies[0] + areas[which].offset;
    uint32_t bytes = areas[which].bytes, length = bytes + 512, n = 0;
    for (uint32_t i = 0; i < length; i++) data[i] = area[i % bytes];
    for (uint32_t at = 0; at < bytes; at++) {
        const uint8_t *r = data + at;
        uint32_t seq, ms, len, crc;
        if (r[0] != '#' || r[9] != ' ' || r[18] != ' ' || r[21] != ' ' || r[30] != ' ' || r[32] != ' ') continue;
        if (!hexval(r + 1, 8, &seq) || !hexval(r + 10, 8, &ms) || !hexval(r + 19, 2, &len) || !hexval(r + 22, 8, &crc)) continue;
        if (len > RAMOOPS_LOG_TEXT_MAX || r[RAMOOPS_LOG_HEADER_BYTES + len] != '\n') continue;
        if (~crc_update(crc_update(0xffffffffu, r, 22), r + 30, 3 + len) != crc) continue;
        assert(n < room);
        out[n].seq = seq;
        out[n].kind = (char)r[31];
        memcpy(out[n].text, r + RAMOOPS_LOG_HEADER_BYTES, len);
        out[n].text[len] = 0;
        n++;
        at += RAMOOPS_LOG_HEADER_BYTES + len;
    }
    return n;
}

// A run like phone-pack-1: SpringBoard SIGSEGV every 5.4 s, PosterBoard SIGABRT twice every 10 s, extensions SIGTERM.
static void storm(void)
{
    world_reset();
    mapping(0x104000000ULL, 0x104200000ULL, 0, "NativeAdrenoMetal", 1);
    struct crash_table fresh = {0};
    table = fresh;
    assert(ramoops_log_vm_ready());
    ramoops_log_line('S', "S6: kernel VM and exception vectors ready");
    lines_reset();
    for (unsigned k = 0; k < 12; k++) {
        struct boot_progress_crash c;
        crash_init(&c, 797 + (int)k, "SpringBoard", 11, 290000 + 5400ULL * k);
        c.pc = 0x1a9463e64ULL + SLIDE;
        c.lr = 0x104001200ULL;
        c.fp = STACK + 0x200;
        c.exception = 1; c.code = 1; c.subcode = 0x28; c.esr = 0x92000006; c.far = 0x28;
        frame(STACK + 0x200, STACK + 0x280, 0x104001234ULL);
        frame(STACK + 0x280, STACK + 0x300, 0x1b0000010ULL + SLIDE);
        frame(STACK + 0x300, 0, MAIN + 0x5678);
        crash_report(&table, &c, CRASH_MODE_ON, &ramoops_sink);
        char exit_line[96];
        snprintf(exit_line, sizeof(exit_line), "exit pid=%u SpringBoard status=b ns=2 code=b", 797 + k);
        ramoops_log_line('S', exit_line);
        if (k % 2 == 0) {
            crash_init(&c, 1800 + (int)k, "PosterBoard", 6, 290000 + 5400ULL * k + 300);
            c.pc = 0x1d0000008ULL + SLIDE;
            c.lr = 0x1d0001000ULL + SLIDE;
            c.fp = STACK + 0x400;
            c.esr = 0x56000080;
            frame(STACK + 0x400, STACK + 0x440, 0x1d0002000ULL + SLIDE);
            frame(STACK + 0x440, 0, 0x1d0003000ULL + SLIDE);
            crash_report(&table, &c, CRASH_MODE_ON, &ramoops_sink);
            snprintf(exit_line, sizeof(exit_line), "exit pid=%u PosterBoard status=6 ns=2 code=6", 1800 + k);
            ramoops_log_line('S', exit_line);
            snprintf(exit_line, sizeof(exit_line), "exit pid=%u PridePosterExten status=f ns=2 code=f", 1900 + k);
            ramoops_log_line('S', exit_line);
        }
    }
}

static void test_ramoops(void)
{
    storm();
    static struct rec recs[2000];
    unsigned n = ring_records(AREA_EVENTS, recs, 2000), x = 0;
    for (unsigned i = 0; i < n; i++) {
        if (recs[i].kind != 'X') continue;
        int found = 0;
        for (unsigned j = 0; j < line_count && !found; j++) found = !strcmp(lines[j], recs[i].text);
        assert(found);
        x++;
    }
    assert(x == line_count && x > 0);
    // No X record went to the console ring (that one carries the S exit records).
    unsigned m = ring_records(AREA_CONSOLE, recs, 2000);
    for (unsigned i = 0; i < m; i++) assert(recs[i].kind != 'X');
    printf("ok  %u X records through RamoopsLog.c: events ring, CRC checked\n", x);
}

static void dump(const char *directory)
{
    storm();
    char path[1024];
    for (unsigned z = 0; z < 2; z++) {
        snprintf(path, sizeof(path), "%s/%s.zone", directory, z ? "pmsg" : "console");
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(region + (z ? zone_size[0] : 0), 1, zone_size[z], f) == zone_size[z] && !fclose(f));
    }
    snprintf(path, sizeof(path), "%s/expected.tsv", directory);
    FILE *f = fopen(path, "w");
    assert(f);
    for (unsigned i = 0; i < line_count; i++) fprintf(f, "X\t%s\n", lines[i]);
    assert(!fclose(f));
    printf("dumped %u X records\n", line_count);
}

int main(int argc, char **argv)
{
    region = malloc(REGION);
    assert(region);
    if (argc == 3 && !strcmp(argv[1], "--dump")) { dump(argv[2]); return 0; }
    test_walk_normal();
    printf("ok  normal chain: cache, main, image, file mapping, raw; reads and lookups counted\n");
    test_walk_limits();
    test_walk_bits();
    test_naming();
    test_filter_and_guard();
    test_rate_limit();
    test_lengths();
    test_ramoops();
    memory_reset();
    free(region);
    printf("crash trace: all tests passed\n");
    return 0;
}
