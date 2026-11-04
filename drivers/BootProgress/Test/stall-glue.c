// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the stall probe's glue, the end of BootProgress.c: the sampler (stall_tick), its targets and references, the App on screen, the
// cross-call, the file mappings, the three limits and the second's records, from before the first frame on, on the actual BootProgress.c and RamoopsLog.c
// over a fake of what phone-stall-probe.patch adds to
// XNU (boot_progress_stall_*, written as its C does: a list that may be busy, pid 0 the kernel task). Test/stall.c tests StallProbe.h; what is
// left to the phone is XNU's side of that interface.
//   cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I../../PaganiDarwinBoot/DarwinBootPkg/Driver stall-glue.c -o stall-glue
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define BOOT_PROGRESS_TEST 1
#define BOOT_PROGRESS_STALL_TEST 1
#ifndef T0SZ_BOOT
#define T0SZ_BOOT 26
#endif
typedef struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct { uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; } Video;
} boot_args;
typedef void *thread_call_t;
enum { NSEC_PER_SEC = 1000000000, THREAD_CALL_PRIORITY_KERNEL = 1, THREAD_CALL_PRIORITY_HIGH = 2, THREAD_CALL_PRIORITY_LOW = 3, THREAD_CALL_OPTIONS_ONCE = 1 };
struct { uint32_t timebase_frequency_hz; } gPEClockFrequencyInfo;
volatile uint32_t armdt_platform_configured, armcpu_registered, armcpu_gic_ready, armcpu_gic_boot[8];
volatile uint64_t armcpu_timer_interrupts;
void early_boot_progress_vectors_done(void) {}
typedef uintptr_t vm_offset_t;
static uintptr_t ml_io_map_wcomb(uintptr_t base, uint64_t size) { (void)size; return base; }
static vm_offset_t ml_vtophys(vm_offset_t va) { return va; }
// The three thread calls this test tells apart: the observer (never run here), the sampler and the file mappings' one.
struct fake_call { void (*fn)(void *, void *); int entered; };
static struct fake_call fake_calls[4];
static uint64_t delayed_deadline;
static void clock_interval_to_deadline(unsigned n, unsigned scale, uint64_t *deadline) { *deadline = (uint64_t)n * scale; }
static void thread_call_enter(thread_call_t call) { ((struct fake_call *)call)->entered = 1; }
static void thread_call_enter_delayed(thread_call_t call, uint64_t deadline) { ((struct fake_call *)call)->entered = 1; delayed_deadline = deadline; }
static thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int priority, int options)
{
    (void)arg; (void)options;
    fake_calls[priority].fn = fn;
    return &fake_calls[priority];
}
#define BOOT_LOGO_WIDTH 4
#define BOOT_LOGO_HEIGHT 4
#define BOOT_LOGO_X 2
#define BOOT_LOGO_Y -1
static const unsigned char boot_logo[] = {0,64,128,255,255,128,64,0,0,64,128,255,255,128,64,0};
static uint64_t test_cpu;
unsigned processor_count, processor_avail_count;
unsigned long gPhysSize;
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
unsigned vm_page_free_count = 5000;
uint32_t memorystatus_available_pages = 300000;
// The records the glue writes go to a table; the rest of the log to the actual writer.
#define ramoops_log_line test_log_line
#include "../BootProgress.c"
#undef ramoops_log_line
void ramoops_log_line(char kind, const char *text);
#include "../RamoopsLog.c"
#include "../GunyahWdog.c"

uint32_t pagani_stall_on;
static uint32_t fake_gpu;
uint32_t adreno830_cpu_hint(void) { return fake_gpu; }
volatile int native_owner_cache_stage;
volatile uint64_t native_ufs_block_reads, native_ufs_block_failures, native_ufs_read_ticks, native_ufs_verify_ticks, native_legacy_copy_stats[4];
volatile uint64_t native_ram_block_resident_bytes;
volatile uint32_t native_adreno_pool_stats[8];
static uint8_t ramoops_region[RAMOOPS_LOG_DEFAULT_CONSOLE + RAMOOPS_LOG_DEFAULT_PMSG];
static uint64_t ramoops_clock;
uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device) { (void)pa; (void)bytes; (void)device; return (uintptr_t)ramoops_region; }
int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    if (strcmp(name, "pagani-ramoops") || bytes != 4) return 0;
    *(uint32_t *)value = 1;
    return 1;
}
const char *ramoops_platform_boot_args(void) { return "-v pagani-ramoops=1 pagani-diag=1"; }
uint64_t ramoops_platform_counter(void) { return ramoops_clock += 1000; }
uint64_t ramoops_platform_frequency(void) { return 19200000; }
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{ (void)hvc; (void)function; (void)a; (void)b; (void)c; return -1; }
int ramoops_platform_psci_method(char method[4]) { (void)method; return 0; }
int ramoops_platform_arm_timer(uint64_t milliseconds) { (void)milliseconds; return -1; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid) { (void)milliseconds; (void)avoid; return -1; }
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = 0x8c0000000ULL; *bytes = 1ULL << 32; }
uint32_t ramoops_platform_cpu_mhz(int *cpu) { *cpu = 5; return 4320; }
void ramoops_platform_smc(uint64_t regs[8]) { regs[0] = 0; }
unsigned ramoops_platform_park_others(uint32_t *parked) { (void)parked; return 0; }
int gunyah_wdog_platform_arg(const char *name, uint32_t *value) { (void)name; (void)value; return 0; }
uint64_t gunyah_wdog_platform_counter(void) { return ramoops_clock; }
uint64_t gunyah_wdog_platform_frequency(void) { return 19200000; }
void gunyah_wdog_platform_call(int hvc, uint32_t function, uint32_t a1, uint32_t a2, uint64_t result[3])
{ (void)hvc; (void)function; (void)a1; (void)a2; result[0] = result[1] = result[2] = 0; }
int gunyah_wdog_platform_start_thread(void) { return 0; }
int gunyah_wdog_platform_start_beat(void) { return -1; }
static int test_boot_arg(const char *name, uint32_t *value)
{
    if (strcmp(name, "pagani-diag")) return 0;
    *value = 1;
    return 1;
}

enum { RECORDS = 4096, HZ = 19200000 };
#define MS(ms) ((uint64_t)(ms) * (HZ / 1000))
static char records[RECORDS][RAMOOPS_LOG_TEXT_MAX];
static char record_kind[RECORDS];
static unsigned recorded;

void test_log_line(char kind, const char *text)
{
    assert(recorded < RECORDS && strlen(text) < RAMOOPS_LOG_TEXT_MAX);
    record_kind[recorded] = kind;
    strcpy(records[recorded++], text);
}

// Records since `from` that hold `text`.
static unsigned count(unsigned from, const char *text)
{
    unsigned found = 0;
    for (unsigned i = from; i < recorded; i++) found += strstr(records[i], text) != NULL;
    return found;
}

static const char *find(unsigned from, const char *text)
{
    for (unsigned i = from; i < recorded; i++) if (strstr(records[i], text)) return records[i];
    return NULL;
}

// ---- the kernel's side --------------------------------------------------------------------------------------------------------------------
enum { TASKS = 8, THREADS = 8, CACHE = 0x180000000ULL, CACHE_END = 0x280000000ULL, STACK = 0x16f000000ULL };
struct fake_task {
    int32_t pid;
    const char *name;
    int alive;
    int refs, maps;
};
struct fake_thread {
    struct fake_task *task;
    uint64_t tid;
    const char *name;
    int active, refs;
    int cpu;                                // -1 off core
    uint32_t state;
    uint64_t pc, fp;
};
static struct fake_task tasks[TASKS] = {
    {0, "kernel_task", 1, 0, 0}, {38, "backboardd", 1, 0, 0}, {27, "SpringBoard", 1, 0, 0}, {440, "kbd", 1, 0, 0}, {448, "MobileNotes", 1, 0, 0},
    [6] = {449, "Preferences", 1, 0, 0},
};
static struct fake_thread threads[THREADS] = {
    {&tasks[1], 0x100, "com.apple.coreanimation.render-server", 1, 0, -1, 1, CACHE + 0x1000, 0},
    {&tasks[2], 0x200, NULL, 1, 0, -1, 1, CACHE + 0x2000, 0},
    {&tasks[3], 0x300, NULL, 1, 0, -1, 1, CACHE + 0x3000, 0},
    {&tasks[4], 0x400, NULL, 1, 0, -1, 1, CACHE + 0x4000, 0},
    [5] = {&tasks[6], 0x600, NULL, 1, 0, -1, 1, CACHE + 0x6000, 0},
};
static int list_busy, task_busy, answer_none;
static void (*late_function)(void *);
static void *late_parameter;
static unsigned xcalls, image_lookups, kernel_task_lookups;
static int32_t focal_pid;                   // the App in the focal role, 0 the home screen
static uint32_t focal_tasks = 1;
static uint64_t page_waits;
static uint64_t fake_stack[64];
static struct boot_progress_stall_cpu fake_cpus[STALL_CPUS];

uint32_t boot_progress_stall_abi(void) { return BOOT_PROGRESS_STALL_ABI; }

int boot_progress_stall_thread_read(void *handle, struct boot_progress_stall_thread *out)
{
    const struct fake_thread *t = handle;
    assert(t->refs > 0);
    memset(out, 0, sizeof(*out));
    out->tid = t->tid;
    out->state = t->state;
    out->active = (uint32_t)t->active;
    out->sched_pri = out->base_pri = 47;
    out->cpu = t->cpu;
    out->last_run = 1;
    out->user.valid = 1;
    out->user.pc = t->pc;
    out->user.lr = t->pc + 4;
    out->user.fp = t->fp;
    return 1;
}

int boot_progress_stall_regs_here(void *handle, struct boot_progress_stall_regs *out)
{
    const struct fake_thread *t = handle;
    if (t->cpu < 0) return 0;
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->pc = t->pc + 0x10;                 // the interrupted state, not the PCB's
    out->lr = t->pc + 0x14;
    return 1;
}

int boot_progress_stall_xcall(unsigned cpu, void (*function)(void *), void *parameter)
{
    (void)cpu;
    xcalls++;
    if (answer_none) {                      // a CPU with interrupts masked: the handler runs when the test says so
        late_function = function;
        late_parameter = parameter;
        return 0;
    }
    function(parameter);
    return 0;
}

void *boot_progress_stall_task_find(const char *name, int32_t pid, struct boot_progress_stall_task *out)
{
    out->pid = -1;
    if (list_busy) return NULL;
    out->pid = 0;
    for (unsigned i = 0; i < TASKS; i++) {
        struct fake_task *t = &tasks[i];
        if (!t->name || !t->alive) continue;
        if (name ? strcmp(t->name, name) : t->pid != pid) continue;
        if (!t->pid) kernel_task_lookups++;
        t->refs++;
        out->pid = t->pid;
        snprintf(out->name, sizeof(out->name), "%s", t->name);
        out->cache_start = t->pid ? CACHE : 0;
        out->cache_end = t->pid ? CACHE_END : 0;
        out->cache_slide = 0;
        out->main_header = 0x100000000ULL;
        out->user_limit = 0x400000000ULL;
        return t;
    }
    return NULL;
}

void boot_progress_stall_task_release(void *handle) { struct fake_task *t = handle; assert(t->refs > 0); t->refs--; }

int32_t boot_progress_stall_foreground(const char *not_a, const char *not_b, uint32_t *count)
{
    assert(!strcmp(not_a, "backboardd") && !strcmp(not_b, "SpringBoard"));
    *count = 0;
    if (list_busy) return -1;
    *count = focal_pid ? focal_tasks : 0;
    return focal_pid;
}

void *boot_progress_stall_thread_find(void *handle, const char *name, uint64_t tid)
{
    const struct fake_task *task = handle;
    if (task_busy) return NULL;
    for (unsigned i = 0; i < THREADS; i++) {
        struct fake_thread *t = &threads[i];
        if (t->task != task || !t->active) continue;
        if (tid ? t->tid != tid : name && (!t->name || !strstr(t->name, name))) continue;
        t->refs++;
        return t;
    }
    return NULL;
}

void boot_progress_stall_thread_release(void *handle) { struct fake_thread *t = handle; assert(t->refs > 0); t->refs--; }

unsigned boot_progress_stall_thread_times(void *handle, uint64_t *tids, uint64_t *times, unsigned max)
{
    const struct fake_task *task = handle;
    unsigned n = 0;
    if (task_busy) return max + 1;
    for (unsigned i = 0; i < THREADS && n < max; i++) {
        if (threads[i].task != task || !threads[i].active) continue;
        tids[n] = threads[i].tid;
        times[n++] = 1000;
    }
    return n;
}

void *boot_progress_stall_map(void *handle) { struct fake_task *t = handle; t->maps++; return t; }
void boot_progress_stall_map_release(void *handle) { struct fake_task *t = handle; assert(t->maps > 0); t->maps--; }

int boot_progress_stall_map_read(void *handle, uint64_t address, void *buffer, uint32_t bytes)
{
    (void)handle;
    if (address < STACK || address + bytes > STACK + sizeof(fake_stack)) return -1;
    memcpy(buffer, (const uint8_t *)fake_stack + (address - STACK), bytes);
    return 0;
}

int boot_progress_stall_image(void *handle, uint64_t address, struct boot_progress_crash_image *image)
{
    const struct fake_task *t = handle;
    assert(t->refs > 0 && t->pid);          // never the kernel
    image_lookups++;
    image->start = address & ~0xfffffULL;
    image->end = image->start + 0x200000;
    image->offset = 0;
    strcpy(image->name, "NativeAdrenoMetal");
    return 1;
}

void boot_progress_stall_cpu_read(unsigned cpu, struct boot_progress_stall_cpu *out) { *out = fake_cpus[cpu]; }
void boot_progress_stall_vm_read(struct boot_progress_stall_vm *out)
{
    memset(out, 0, sizeof(*out));
    out->page_waits = page_waits;
}

void boot_progress_stall_mem_read(struct boot_progress_stall_mem *out)
{
    out->wired = 240000;
    out->internal = 360000;
    out->external = 250000;
    out->compressor = 11;
    out->compressed = 22;
    out->throttled = 90000;
    out->purgeable = 1200;
    out->zone = 110000;
}

// ---- driving it ------------------------------------------------------------------------------------------------------------------------------
static struct fake_call *const sampler = &fake_calls[THREAD_CALL_PRIORITY_HIGH];
static struct fake_call *const mapper = &fake_calls[THREAD_CALL_PRIORITY_LOW];

// One thread-call round at `at`: the sampler if it is entered, then the mappings' lookup if that is.
static void tick(uint64_t at)
{
    test_clock = at;
    if (sampler->entered) {
        sampler->entered = 0;
        sampler->fn(NULL, NULL);
    }
    if (mapper->entered) {
        mapper->entered = 0;
        mapper->fn(NULL, NULL);
    }
}

// From `at` on, a tick each 10 ms for `ms`.
static void run_for(uint64_t at, unsigned ms)
{
    for (unsigned t = 0; t <= ms; t += 10) tick(at + MS(t));
}

// Frames of a running animation, one every 8 ms, each with its submission. Returns when the newest came.
static uint64_t animate(uint64_t start, unsigned frames)
{
    uint64_t at = start;
    for (unsigned i = 0; i < frames; i++, at += MS(8)) {
        fake_gpu++;
        test_clock = at;
        boot_progress_frame(1, at);
        if (!(i % 2)) tick(at + MS(1));
    }
    return at - MS(8);
}

static void stall_state_clean(void)
{
    memset(&stall_gaps, 0, sizeof(stall_gaps));
    memset(&stall_limits, 0, sizeof(stall_limits));
    memset(&stall_kbt_limits, 0, sizeof(stall_kbt_limits));
    memset(&stall_tail_limits, 0, sizeof(stall_tail_limits));
    memset(&stall_open, 0, sizeof(stall_open));
    memset(&stall_known_maps, 0, sizeof(stall_known_maps));
}

// Before the first frame the sampler sleeps and nothing was shown or touched: every second has its records all the same, M mem every fifth.
static void test_before_first_frame(void)
{
    assert(!__atomic_load_n(&presented.count, __ATOMIC_ACQUIRE) && !__atomic_load_n(&stall_armed, __ATOMIC_ACQUIRE) && !stall_m_until);
    const unsigned first = recorded;
    for (unsigned s = 0; s < 20; s++) {
        test_clock = MS(3000 + 1000 * s);
        stall_second(0);
    }
    assert(count(first, "lat") == 20 && count(first, "cpu idle ") == 20 && count(first, "ipi max ") == 20 && count(first, "io rd ") == 20);
    assert(count(first, "mem wire ") == 4 && recorded - first == 4 * 20 + 4);
    assert(!sampler->entered && !__atomic_load_n(&stall_armed, __ATOMIC_ACQUIRE));
    assert(!strcmp(records[first + 4], "mem wire 240000 int 360000 ext 250000 cmp 11/22 thr 90000 pur 1200 zone 110000 ram 0 gpu 0/0"));
    printf("ok  before the first frame: the records of every second with nothing awake, M mem every fifth\n");
}

static void test_animation_end(void)
{
    stall_mem_next = UINT64_MAX;            // the seconds of these tests have their four records
    const unsigned first = recorded;
    const uint64_t last = animate(MS(25000), 40);
    run_for(last + MS(10), 3000);
    assert(count(first, " why=gap") == 1 && find(first, " why=gap110 ") && find(first, " ask=0"));
    assert(recorded - first - count(first, "map pid=") == STALL_SAMPLE_LINES(2) && find(first, " fg=0"));
    // On the limit of its own: of the others only the map records.
    assert(stall_tail_limits.used == STALL_SAMPLE_LINES(2) && stall_limits.used == count(first, "map pid="));
    assert(find(first, " th rs pid=38 backboardd tid=0x100 st=W ") && find(first, " th sb pid=27 SpringBoard tid=0x200 "));
    assert(find(first, " bt rs pid=38 pc=c:180001000 lr=c:180001004 "));
    // Asleep again: the ending of an animation keeps nothing awake.
    assert(!sampler->entered && !__atomic_load_n(&stall_armed, __ATOMIC_ACQUIRE));
    printf("ok  the end of an animation: one sample, ask=0, on its own limit, then asleep\n");
}

static void test_idle_tap(void)
{
    const unsigned first = recorded;
    // A tap on a screen idle for a minute, seen by the 1 Hz observer, the sampler asleep. No frame answers for 9 s.
    const uint64_t tap = MS(90000);
    test_clock = tap;
    pagani_input_touch_events += 2;
    assert(stall_touched() && sampler->entered);
    run_for(tap, 9000);
    assert(count(first, " why=gap") == STALL_GAP_LEVELS);
    assert(find(first, " why=gap110 ") && find(first, " why=gap8000 ") && count(first, " ask=2") == STALL_GAP_LEVELS);
    // The age is the stall's, since is the idle time before it. The sampler stayed up to the last threshold, then slept.
    const char *last = find(first, " why=gap8000 ");
    assert(strstr(last, " age=8000 ") || strstr(last, " age=8010 "));
    uint32_t since = 0;
    assert(sscanf(strstr(last, " since="), " since=%u", &since) == 1 && since > 60000);
    assert(!sampler->entered && !__atomic_load_n(&stall_armed, __ATOMIC_ACQUIRE));
    printf("ok  a tap on a screen idle for a minute: every threshold from the tap, up to 8 s, past the sampler's 2 s\n");
}

static void test_keyboard(void)
{
    // All of the other limit is gone; a pair left open has nothing of it to fear.
    stall_limits.used = STALL_LINES_MAX;
    threads[3].cpu = 2;
    threads[3].state = 4;
    const unsigned first = recorded;
    const uint64_t begin = MS(110000);
    test_clock = begin;
    boot_progress_stall_kbt(448, STALL_KBT_IN, 0);
    assert(sampler->entered);
    run_for(begin, 900);
    assert(count(first, " why=in ") == 2 && find(first, " pid=448 MobileNotes open=1"));
    // On core: its CPU answered with the interrupted state.
    assert(find(first, " th app pid=448 MobileNotes tid=0x400 st=C ") && find(first, " x=ok ") && find(first, " bt app pid=448 pc=c:180004010 lr=c:180004014 "));
    assert(xcalls >= 2 && find(first, " th kbd pid=440 kbd tid=0x300 "));
    assert(stall_kbt_limits.used && stall_limits.used == STALL_LINES_MAX);
    // A CPU that does not answer: the sampler goes on after its patience, and the question stays open until the answer lands in its own slot.
    answer_none = 1;
    test_clock = begin + MS(1300);
    sampler->entered = 1;
    pthread_t clock_thread;
    extern void *clock_runner(void *);
    assert(!pthread_create(&clock_thread, NULL, clock_runner, NULL));
    sampler->fn(NULL, NULL);
    __atomic_store_n(&answer_none, 2, __ATOMIC_RELEASE);
    assert(!pthread_join(clock_thread, NULL));
    answer_none = 0;
    assert(find(first, " x=to "));
    sampler->entered = 1;
    test_clock += MS(600);
    sampler->fn(NULL, NULL);                // the slot is still asked: "to" without a new call
    assert(late_function);
    late_function(late_parameter);
    assert(stall_slot_answered(&stall_slots[2]));
    threads[3].cpu = -1;
    threads[3].state = 1;
    test_clock += MS(10);
    boot_progress_stall_kbt(448, STALL_KBT_IN, 1);
    assert(!stall_kbt_any(&stall_open));
    stall_limits.used = 0;
    printf("ok  a key: pair sampled at its own limit, the running thread's interrupted state, a CPU that does not answer\n");
}

void *clock_runner(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&answer_none, __ATOMIC_ACQUIRE) == 1) {
        __atomic_fetch_add(&test_clock, (uint64_t)HZ / 1000, __ATOMIC_RELAXED);
        usleep(100);
    }
    return NULL;
}

// A frame outside the shared cache sends its mapping to the low-priority lookup. Its list busy, exactly then, must change nobody's identity; the
// lookup goes through at the next sample, for that process and never for the kernel.
static void test_identity(void)
{
    const unsigned first = recorded;
    const uint64_t begin = MS(120000);
    threads[3].pc = 0x104001000ULL;
    list_busy = 0;
    test_clock = begin;
    boot_progress_stall_kbt(448, STALL_KBT_DN, 0);
    tick(begin);
    run_for(begin + MS(10), 280);           // up to the sample's tick: the targets are found, then the list goes busy
    assert(stall_app.task && stall_app.info.pid == 448);
    list_busy = 1;
    tick(begin + MS(300));
    tick(begin + MS(310));
    tick(begin + MS(320));
    assert(count(first, " why=dn ") == 1 && !image_lookups && stall_image.state == STALL_IMAGE_FREE);
    assert(stall_app.info.pid == 448 && !strcmp(stall_app.info.name, "MobileNotes") && stall_app.info.cache_end == CACHE_END);
    run_for(begin + MS(330), 480);          // the second sample, the list still busy
    assert(count(first, " why=dn ") == 2 && !image_lookups);
    assert(stall_app.info.pid == 448 && !strcmp(stall_app.info.name, "MobileNotes") && stall_app.info.cache_end == CACHE_END);
    list_busy = 0;
    run_for(begin + MS(820), 500);          // the third: nothing has changed, and the lookup goes through
    assert(count(first, " why=dn ") == 3 && image_lookups == 1 && !kernel_task_lookups);
    assert(count(first, " pid=448 MobileNotes ") == 3 + 3 + (unsigned)(find(first, "map pid=448") != NULL) && !find(first, "kernel_task"));
    assert(stall_image.state == STALL_IMAGE_FREE || stall_image.state == STALL_IMAGE_DONE);
    test_clock = begin + MS(1400);
    run_for(begin + MS(1400), 30);
    assert(find(first, "img pid=448 ") && strstr(find(first, "img pid=448 "), "NativeAdrenoMetal"));
    threads[3].pc = CACHE + 0x4000;
    boot_progress_stall_kbt(448, STALL_KBT_DN, 1);
    assert(!stall_kbt_any(&stall_open));
    printf("ok  a busy task list while a mapping is asked for: the target keeps its identity, the kernel task is never looked at\n");
}

// A target that exits: dropped with its references, a "no" record, found again when it comes back.
static void test_exit(void)
{
    const unsigned first = recorded;
    const uint64_t begin = MS(130000);
    test_clock = begin;
    boot_progress_stall_kbt(448, STALL_KBT_IN, 0);
    run_for(begin, 310);
    threads[3].active = 0;
    tasks[4].alive = 0;
    run_for(begin + MS(320), 500);
    assert(find(first, " no app") && !stall_app.task && !stall_app.thread);
    // Off the list: its pair is dropped too, at the next lookup.
    run_for(begin + MS(830), 2600);
    assert(!stall_kbt_any(&stall_open));
    threads[3].active = 1;
    tasks[4].alive = 1;
    printf("ok  a target that exits: dropped, its pair forgotten\n");
}

// A system target whose thread was not found (its task's lock was busy) when its process exits: no sample gets to let go of it, the lookup by name does.
static void test_exit_without_thread(void)
{
    struct fake_task *old = &tasks[1], *fresh = &tasks[5];
    struct fake_thread *thread = &threads[4];
    const uint64_t begin = MS(135000), wait = MS(STALL_LOOKUP_MS);
    stall_target_drop(&stall_rs);
    stall_rs.looked = 0;
    task_busy = 1;
    stall_look_system(begin, HZ);
    task_busy = 0;
    assert(stall_rs.task == old && !stall_rs.thread && old->refs == 1);
    // Not found yet, the same process: it stays, and the next look has its thread.
    stall_look_system(begin + wait - 1, HZ);
    assert(stall_rs.task == old && stall_rs.thread == &threads[0]);
    stall_target_drop(&stall_rs);
    stall_rs.looked = 0;
    task_busy = 1;
    stall_look_system(begin + wait, HZ);
    task_busy = 0;
    assert(stall_rs.task == old && !stall_rs.thread);
    old->alive = threads[0].active = 0;
    *fresh = (struct fake_task){39, "backboardd", 1, 0, 0};
    *thread = (struct fake_thread){fresh, 0x500, "com.apple.coreanimation.render-server", 1, 0, -1, 1, CACHE + 0x5000, 0};
    stall_look_system(begin + 2 * wait, HZ);     // the old task has no thread to give: back to the lookup
    assert(!stall_rs.task && !old->refs);
    stall_look_system(begin + 2 * wait + 1, HZ);
    assert(stall_rs.task == fresh && stall_rs.thread == thread && stall_rs.info.pid == 39);
    stall_target_drop(&stall_rs);
    stall_rs.looked = 0;
    old->alive = threads[0].active = 1;
    fresh->alive = thread->active = 0;
    assert(!fresh->refs && !thread->refs);
    stall_look_system(begin + 3 * wait, HZ);     // the target as the next test expects it: its map record is not on that test's limit
    assert(stall_rs.task == old && stall_rs.thread == &threads[0]);
    printf("ok  a target without a thread whose process exits: looked up again by name, the old reference given back\n");
}

// kbd's task lock busy: not a kbd without threads. The target, its thread and the base of the next deltas stay.
static void test_busy_keyboard(void)
{
    const uint64_t begin = MS(137000);
    stall_look_keyboard(448, begin, HZ);
    void *task = stall_kbd.task, *thread = stall_kbd.thread;
    const unsigned known = stall_kbd_busy.count;
    assert(task == &tasks[3] && thread == &threads[2] && known);
    task_busy = 1;
    stall_look_keyboard(448, begin + MS(500), HZ);
    task_busy = 0;
    assert(stall_kbd.task == task && stall_kbd.thread == thread && stall_kbd_busy.count == known);
    // No thread left is the one reason to let go.
    threads[2].active = 0;
    stall_look_keyboard(448, begin + MS(1000), HZ);
    assert(!stall_kbd.task && !stall_kbd.thread && !tasks[3].refs);
    threads[2].active = 1;
    printf("ok  kbd's lock busy: the target and its base stay; no thread left lets it go\n");
}

static void test_watch(void)
{
    const unsigned first = recorded;
    // An idle CPU over a non-empty queue for five ticks; then an IPI that does not land.
    const uint64_t last = animate(MS(140000), 4);
    fake_cpus[3].kind = STALL_CPU_IDLE;
    fake_cpus[3].on_idle = 1;
    fake_cpus[3].runq = 2;
    run_for(last + MS(10), 60);
    assert(find(first, " why=idleq ") && find(first, " cpu=3"));
    fake_cpus[3].runq = 0;
    stall_note_ipi_send(&stall, 5, last + MS(100));
    run_for(last + MS(110), 40);
    assert(find(first, " why=ipi ") && find(first, " cpu=5"));
    stall_note_ipi_recv(&stall, 5, last + MS(160));
    run_for(last + MS(170), 2400);
    printf("ok  the scheduler's watch: idle over a queue, an IPI that does not land\n");
}

static void test_second(void)
{
    const unsigned first = recorded;
    test_clock = MS(150000);
    struct boot_progress_stall_run run = {MS(149000), MS(60), 0x400, "MobileNotes", 448, 47, 2, 1};
    const struct boot_progress_stall_run kernel = {MS(149001), MS(30), 0x77, NULL, 0, 91, 3, 0}, idle = {MS(149002), 0, 0x78, NULL, -1, 0, 4, 0};
    boot_progress_stall_run(&run);
    assert(!stall_touched());
    stall_second(0);                        // nothing shown, the sampler asleep: the totals move, no record
    assert(recorded == first);
    // Nothing shown with the sampler up, the middle of a hitch: that second's totals are taken here or never.
    run.latency = MS(65);
    boot_progress_stall_run(&run);
    __atomic_store_n(&stall_armed, 1, __ATOMIC_RELEASE);
    stall_second(0);
    __atomic_store_n(&stall_armed, 0, __ATOMIC_RELEASE);
    assert(recorded == first + 4 && record_kind[first] == 'M' && strstr(records[first], "lat fg 1/65.0/1/1/0 MobileNotes[448]/400/47"));
    const unsigned next = recorded;
    run.latency = MS(70);
    boot_progress_stall_run(&run);
    boot_progress_stall_run(&kernel);
    boot_progress_stall_run(&idle);
    page_waits += 3;
    stall_second(30);
    assert(record_kind[next] == 'M' && strstr(records[next], "lat fg 1/70.0/1/1/0 MobileNotes[448]/400/47 k 1/30.0/1/0/0 77/91"));
    assert(strstr(records[next + 1], "cpu idle ") && strstr(records[next + 2], "ipi max ") && strstr(records[next + 3], "io rd "));
    assert(strstr(records[next + 3], " pw 3 ") && strstr(records[next + 3], " q ") && strstr(records[next + 3], " kq ") && strstr(records[next + 3], " tq "));
    uint32_t used = 0;
    assert(sscanf(strstr(records[next + 3], " kq "), " kq %u/", &used) == 1 && used == stall_kbt_limits.used && used);
    assert(sscanf(strstr(records[next + 3], " tq "), " tq %u/", &used) == 1 && used == stall_tail_limits.used && used);
    // Nothing shown past that frame, the sampler asleep, an app opened and left alone: the records go on for STALL_M_TAIL_S, then stop.
    uint64_t now = test_clock;
    for (unsigned s = 1; s <= STALL_M_TAIL_S + 2; s++) {
        const unsigned before = recorded;
        test_clock = now + MS(1000) * s;
        stall_second(0);
        assert(recorded == before + (s < STALL_M_TAIL_S ? 4u : 0u));
        assert(s >= STALL_M_TAIL_S || strstr(records[before + 3], " pw 0 "));    // a count of its second
    }
    // A touch alone starts it over: the sampler stays up to the last threshold for that one, and the records STALL_M_TAIL_S from the touch.
    pagani_input_touch_events++;
    test_clock += MS(1000);
    now = test_clock;
    assert(stall_touched());
    unsigned before = recorded;
    stall_second(0);
    assert(recorded == before + 4);
    run_for(now, 8100);
    assert(!__atomic_load_n(&stall_armed, __ATOMIC_ACQUIRE));
    before = recorded;
    test_clock = now + MS(1000) * (STALL_M_TAIL_S - 1);
    assert(!stall_touched());
    stall_second(0);
    assert(recorded == before + 4);
    before = recorded;
    test_clock = now + MS(1000) * STALL_M_TAIL_S;
    stall_second(0);
    assert(recorded == before);
    const char *extra = stall_fps_extra("base");
    assert(!strncmp(extra, "base | tch ", 11) && strstr(extra, " vl "));
    printf("ok  the second's records, with all three limits, kernel threads in their band\n");
}

static void test_halt(void)
{
    const unsigned first = recorded;
    assert(pagani_stall_on);
    const uint64_t last = animate(MS(190000), 4);
    stall_halt();
    assert(!pagani_stall_on);
    run_for(last + MS(10), 500);
    assert(recorded == first && !sampler->entered);
    // A halted sampler is armed for good: no frame starts it again.
    test_clock = last + MS(1000);
    boot_progress_frame(1, last + MS(1000));
    assert(!sampler->entered);
    // What it holds stays held, nothing is released on a panic's path; the rest balances.
    printf("ok  halted: no sample, no tick, no new arming\n");
}

// A touch nothing answers, after the sampler saw the frame `last`: up to the sample at the first threshold.
static void touch_after(uint64_t last)
{
    tick(last + MS(2));
    pagani_input_touch_events++;
    run_for(last + MS(10), 150);
}

// A hitch in an App: backboardd and SpringBoard wait, the sample shows what its main thread does. Asked at every gap sample: an App switch shows at the
// next one, the home screen has no App and costs no records for one, a busy list costs this sample its App and the target nothing.
static void test_foreground(void)
{
    const uint64_t begin = MS(176000);
    unsigned first = recorded;
    // One gap in `focal_pid`: frames, a touch nothing answers. The sample is at its first threshold.
    struct { int32_t pid; uint32_t tasks; const char *field, *th, *bt; unsigned threads; } steps[] = {
        {448, 1, " fg=1", " th fg pid=448 MobileNotes tid=0x400 st=W ", " bt fg pid=448 pc=c:180004000 ", 3},
        {449, 2, " fg=2", " th fg pid=449 Preferences tid=0x600 st=W ", " bt fg pid=449 pc=c:180006000 ", 3},     // switched: the old target is let go
        {0, 1, " fg=0", NULL, NULL, 2},                                                                           // the home screen
    };
    for (unsigned i = 0; i < 3; i++) {
        focal_pid = steps[i].pid;
        focal_tasks = steps[i].tasks;
        const uint64_t last = animate(begin + MS(1000) * i, 4);
        first = recorded;
        const uint32_t used = stall_limits.used, tails = stall_tail_limits.used;
        touch_after(last);
        assert(count(first, " why=gap110 ") == 1 && find(first, " ask=2") && find(first, steps[i].field) && stall_tail_limits.used == tails);
        const unsigned maps = count(first, "map pid=");
        assert(stall_limits.used == used + maps + STALL_SAMPLE_LINES(steps[i].threads));
        assert(recorded - first == maps + STALL_SAMPLE_LINES(steps[i].threads));
        if (steps[i].th) {
            assert(find(first, steps[i].th) && find(first, steps[i].bt) && maps == 1 && find(first, "map pid=44"));
            assert(stall_fg.task && stall_fg.thread && stall_fg.info.pid == steps[i].pid);
        } else {
            assert(!find(first, " fg pid=") && !find(first, " no fg") && !maps && !stall_fg.task && !stall_fg.thread);
        }
    }
    assert(!tasks[6].refs && !threads[5].refs && !tasks[6].maps);
    // A busy list: this sample goes without, the target stays for the next.
    focal_pid = 449;
    focal_tasks = 1;
    uint64_t last = animate(begin + MS(4000), 4);
    touch_after(last);
    assert(stall_fg.task == &tasks[6] && tasks[6].refs == 1 && threads[5].refs == 1);
    last = animate(begin + MS(5000), 4);
    first = recorded;
    list_busy = 1;
    touch_after(last);
    list_busy = 0;
    assert(find(first, " fg=-1") && !find(first, " fg pid=") && !find(first, " no fg") && stall_fg.task == &tasks[6] && tasks[6].refs == 1);
    // Its main thread not found (its task lock was busy): the record says so, and the next look finds it.
    stall_target_drop(&stall_fg);
    stall_fg.looked = 0;
    last = animate(begin + MS(6000), 4);
    first = recorded;
    task_busy = 1;
    touch_after(last);
    task_busy = 0;
    assert(find(first, " no fg") && stall_fg.task == &tasks[6] && !stall_fg.thread);
    last = animate(begin + MS(7500), 4);
    first = recorded;
    touch_after(last);
    assert(find(first, " th fg pid=449 Preferences ") && stall_fg.thread == &threads[5]);
    // Keyboard samples and the scheduler's watch have no App of their own to look for here.
    focal_pid = 0;
    run_for(last + MS(170), 2400);
    printf("ok  the App on screen: its main thread in a gap sample, a switch, the home screen, a busy list, a thread not found\n");
}

// M mem at its turn, with what the other drivers keep.
static void test_memory(void)
{
    native_ram_block_resident_bytes = 1234567890ULL;
    native_adreno_pool_stats[3] = 400;
    native_adreno_pool_stats[4] = 921;
    stall_mem_next = 0;
    const unsigned first = recorded;
    for (unsigned s = 0; s < 11; s++) {
        test_clock = MS(186000 + 1000 * s);
        stall_second(30);
    }
    assert(count(first, "mem wire ") == 3 && count(first, "lat") == 11);
    assert(find(first, "mem wire 240000 int 360000 ext 250000 cmp 11/22 thr 90000 pur 1200 zone 110000 ram 1234567890 gpu 400/921"));
    printf("ok  M mem: every fifth second, the Data disk and the GPU pool\n");
}

// Every reference is accounted for: what the targets hold, one each, and nothing else.
static void test_references(void)
{
    struct stall_target *held[5] = {&stall_rs, &stall_sb, &stall_fg, &stall_app, &stall_kbd};
    for (unsigned i = 0; i < 5; i++) stall_target_drop(held[i]);
    if (stall_image.state != STALL_IMAGE_FREE) boot_progress_stall_task_release(stall_image.task);
    for (unsigned i = 0; i < TASKS; i++) assert(!tasks[i].refs && !tasks[i].maps);
    for (unsigned i = 0; i < THREADS; i++) assert(!threads[i].refs);
    printf("ok  references: every task, thread and map one was given back\n");
}

int main(void)
{
    assert(ramoops_log_vm_ready());
    __atomic_store_n(&fps_mode, 1, __ATOMIC_RELEASE);
    stall_configure();
    assert(stall_mode && pagani_stall_on && sampler->fn && mapper->fn);
    for (unsigned c = 0; c < STALL_CPUS; c++) fake_cpus[c].kind = STALL_CPU_RUNNING;
    stall_state_clean();
    test_before_first_frame();
    test_animation_end();
    test_idle_tap();
    test_keyboard();
    test_identity();
    test_exit();
    test_exit_without_thread();
    test_busy_keyboard();
    test_watch();
    test_second();
    test_foreground();
    test_memory();
    test_halt();
    test_references();
    printf("stall probe's glue: all tests passed\n");
    return 0;
}
