// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile with -fsanitize=address,undefined; uses the actual renderer and map builder.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define BOOT_PROGRESS_TEST 1
#ifndef T0SZ_BOOT
#define T0SZ_BOOT 26
#endif
typedef struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct { uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; } Video;
} boot_args;
typedef void *thread_call_t;
enum { NSEC_PER_SEC = 1000000000, THREAD_CALL_PRIORITY_KERNEL = 1, THREAD_CALL_OPTIONS_ONCE = 1 };
struct { uint32_t timebase_frequency_hz; } gPEClockFrequencyInfo;
volatile uint32_t armdt_platform_configured, armcpu_registered, armcpu_gic_ready, armcpu_gic_boot[8];
volatile uint64_t armcpu_timer_interrupts;
void early_boot_progress_vectors_done(void) {}
typedef uintptr_t vm_offset_t;
// The write-combined alias of the framebuffer is the framebuffer itself on the host; ml_vtophys maps a second virtual alias to it.
static uintptr_t wc_maps;
static uintptr_t ml_io_map_wcomb(uintptr_t base, uint64_t size) { (void)size; wc_maps++; return base; }
static uintptr_t alias_va, alias_pa;
static vm_offset_t ml_vtophys(vm_offset_t va) { return va == alias_va ? alias_pa : va; }
static void clock_interval_to_deadline(unsigned n, unsigned scale, uint64_t *deadline) { *deadline = (uint64_t)n * scale; }
static unsigned scheduled;
static void thread_call_enter_delayed(thread_call_t call, uint64_t deadline) { (void)call; assert(deadline == NSEC_PER_SEC); scheduled++; }
static void thread_call_enter(thread_call_t call) { assert(call); }
static thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int priority, int options)
{ (void)fn; (void)arg; (void)priority; (void)options; return (void *)1; }
#define BOOT_LOGO_WIDTH 4
#define BOOT_LOGO_HEIGHT 4
#define BOOT_LOGO_X 2
#define BOOT_LOGO_Y -1
static const unsigned char boot_logo[] = {0,64,128,255,255,128,64,0,0,64,128,255,255,128,64,0};
static uint64_t test_cpu;
unsigned processor_count, processor_avail_count;
unsigned long gPhysSize;
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
static int release_on_contention;
static unsigned exchange_for_test(unsigned *lock, unsigned value, int order)
{
    unsigned previous = __atomic_exchange_n(lock, value, order);
    // Let the other CPU finish immediately after this failed acquisition.
    if (previous && release_on_contention) {
        release_on_contention = 0;
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    }
    return previous;
}
#define __atomic_exchange_n exchange_for_test
#include "../BootProgress.c"
#undef __atomic_exchange_n
#include "../RamoopsLog.c"
#include "../GunyahWdog.c"
// The persistent log on a host buffer in place of the Android ramoops region; this test runs it with pagani-ramoops=1 only.
static uint8_t ramoops_region[RAMOOPS_LOG_DEFAULT_CONSOLE + RAMOOPS_LOG_DEFAULT_PMSG];
static uint64_t ramoops_clock;
static uint32_t console_reset_seq;          // pagani-reset-seq, set by the probe's stage line test only
static unsigned console_smcs;
uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{ assert(pa == RAMOOPS_LOG_DEFAULT_PA && bytes == sizeof(ramoops_region) && !device); return (uintptr_t)ramoops_region; }
int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    if (console_reset_seq && !strcmp(name, "pagani-reset-seq") && bytes == 4) { *(uint32_t *)value = console_reset_seq; return 1; }
    if (strcmp(name, "pagani-ramoops") || bytes != 4) return 0;
    *(uint32_t *)value = 1;
    return 1;
}
const char *ramoops_platform_boot_args(void)
{
    assert(!console_reset_seq || console_reset_seq == 0x100);
    return console_reset_seq ? "-v pagani-ramoops=1 pagani-reset-seq=0x100" : "-v pagani-ramoops=1";
}
uint64_t ramoops_platform_counter(void) { return ramoops_clock += 1000; }
uint64_t ramoops_platform_frequency(void) { return 19200000; }
// No PSCI either, but for the probe's stage line test, where PSCI_VERSION finds nothing (no reset is configured there).
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{ (void)hvc; (void)a; (void)b; (void)c; assert(console_reset_seq && function == 0x84000000); return -1; }
int ramoops_platform_psci_method(char method[4]) { (void)method; return 0; }
int ramoops_platform_arm_timer(uint64_t milliseconds) { (void)milliseconds; assert(0); return -1; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid) { (void)milliseconds; (void)avoid; assert(0); return -1; }
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = 0x8c0000000ULL; *bytes = 1ULL << 32; }
uint32_t ramoops_platform_cpu_mhz(int *cpu) { *cpu = 5; return 4320; }    // the heartbeat's CPU clock (CpuClock.h in the kernel)
// pagani-reset-seq only in the probe's stage line test (Test/ramoops.c covers the rest): IS_CALL_AVAIL says yes, the download mode
// reads 0x20. A CPU park fails the test.
void ramoops_platform_smc(uint64_t regs[8])
{
    assert(console_reset_seq);
    console_smcs++;
    regs[1] = regs[0] == 0xc2000501 ? 0x20 : 1;
    regs[0] = 0;
}
unsigned ramoops_platform_park_others(uint32_t *parked) { (void)parked; assert(0); return 0; }
// GunyahWdog stays off here (no pagani-wdog): a hypervisor call, a pet thread or a heartbeat fails the test (Test/gunyah-wdog.c
// covers the watchdog itself).
static unsigned wdog_platform_calls;
int gunyah_wdog_platform_arg(const char *name, uint32_t *value) { (void)name; (void)value; return 0; }
uint64_t gunyah_wdog_platform_counter(void) { return ramoops_clock; }
uint64_t gunyah_wdog_platform_frequency(void) { return 19200000; }
void gunyah_wdog_platform_call(int hvc, uint32_t function, uint32_t a1, uint32_t a2, uint64_t result[3])
{ (void)hvc; (void)function; (void)a1; (void)a2; result[0] = result[1] = result[2] = 0; wdog_platform_calls++; }
int gunyah_wdog_platform_start_thread(void) { wdog_platform_calls++; return 0; }
int gunyah_wdog_platform_start_beat(void) { wdog_platform_calls++; return -1; }
// Both zones hold the text (the zones are small here: no wrap).
static int ramoops_seen(const char *text)
{
    size_t n = strlen(text);
    for (unsigned z = 0; z < 2; z++) {
        const uint8_t *data = ramoops_region + (z ? RAMOOPS_LOG_DEFAULT_CONSOLE : 0) + RAMOOPS_LOG_ZONE_HEADER;
        uint32_t size;
        memcpy(&size, data - 4, 4);
        int found = 0;
        for (uint32_t i = 0; !found && i + n <= size; i++) found = !memcmp(data + i, text, n);
        if (!found) return 0;
    }
    return 1;
}
// How often one area of the log (copy 0) holds the text.
static unsigned ramoops_area_count(unsigned area, const char *text)
{
    const volatile uint8_t *base = copies[0] + areas[area].offset;
    size_t n = strlen(text);
    unsigned found = 0;
    for (uint32_t i = 0; i + n <= areas[area].bytes; i++) {
        size_t k = 0;
        while (k < n && base[i + k] == (uint8_t)text[k]) k++;
        found += k == n;
    }
    return found;
}
// The boot-args BootProgress reads itself (pagani-fps and those its default follows): none until a test sets them.
static struct { const char *name; uint32_t value; } test_args[8];
static unsigned test_arg_count;
static int test_boot_arg(const char *name, uint32_t *value)
{
    for (unsigned i = 0; i < test_arg_count; i++)
        if (!strcmp(test_args[i].name, name)) { *value = test_args[i].value; return 1; }
    return 0;
}
static void test_arg(const char *name, uint32_t value)
{
    assert(test_arg_count < 8);
    test_args[test_arg_count].name = name; test_args[test_arg_count].value = value; test_arg_count++;
}
volatile int native_owner_cache_stage;
volatile uint64_t native_ufs_block_reads, native_ufs_block_failures;
volatile uint64_t native_legacy_copy_stats[4] = {4100, 9000, 1234567, 5000000000ULL};
unsigned vm_page_free_count;
uint32_t memorystatus_available_pages;

static void panic_for_test(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    boot_progress_panic(format, &args, 0xfffffff007123456, 9, 0x140);
    va_end(args);
}

int main(void)
{
    enum { W = 126, H = 840, PITCH = 132, GUARD = 32 };
    uint32_t *allocation = malloc((PITCH * H + 2 * GUARD) * sizeof(uint32_t));
    assert(allocation);
    for (unsigned i = 0; i < PITCH * H + 2 * GUARD; i++) allocation[i] = 0x12345678;
    boot_args args = { .virtBase = 0xfffffff007004000ULL, .physBase = 0xc0004000,
                      .memSize = 0x10000000, .Video = { (uintptr_t)(allocation + GUARD), 1, PITCH * 4, W, H, 32 } };
    early_boot_progress_init(&args);
    assert(boot_progress_active() && logo_visible);
    early_boot_progress_vm_ready();
    assert(wc_maps == 1 && framebuffer_wc == framebuffer && framebuffer_wc_bytes == (uint64_t)PITCH * 4 * H);
    {
        // XNU's console rebinds to its own Device mapping of the same framebuffer: the write-combined alias stays in use.
        uintptr_t wc = framebuffer;
        uint32_t *device_alias = malloc((PITCH * H + 2 * GUARD) * sizeof(uint32_t));
        assert(device_alias);
        alias_va = (uintptr_t)(device_alias + GUARD); alias_pa = framebuffer_pa;
        boot_progress_bind(alias_va, W, H, PITCH * 4, 32);
        assert(framebuffer == wc && width == W && height == H && stride == PITCH);
        boot_progress_bind(alias_va, W, H + 1, PITCH * 4, 32);   // larger than the alias: the console's mapping is used
        assert(framebuffer == alias_va && height == H + 1);
        alias_pa = framebuffer_pa + 4096;                       // another physical framebuffer: rebinds
        boot_progress_bind(alias_va, W, H, PITCH * 4, 32);
        assert(framebuffer == alias_va);
        alias_pa = framebuffer_pa;
        boot_progress_bind(alias_va, W, H, PITCH * 4, 32);
        assert(framebuffer == wc);
        alias_va = alias_pa = 0;
        free(device_alias);
    }
    unsigned previous = stage_count;
    uint64_t held = irq_save();
    assert(held != UINT64_MAX);
    test_cpu = 0x10000;
    boot_progress_stage("contended secondary");
    assert(stage_count == previous);
    release_on_contention = 1;
    assert(boot_progress_display_handoff());
    assert(!release_on_contention && !console_lock && display_owned);
    boot_progress_display_failed();
    test_cpu = 0;
    irq_restore(held);
    boot_progress_stage("uncontended boot CPU");
    assert(stage_count == previous + 1);

    unsigned logo_at = GUARD + ((H - BOOT_LOGO_HEIGHT) / 2 + BOOT_LOGO_Y) * PITCH +
                       (W - BOOT_LOGO_WIDTH) / 2 + BOOT_LOGO_X;
    assert(allocation[logo_at] == 0xff000000 && allocation[logo_at + 1] == 0xff404040 &&
           allocation[logo_at + 2] == 0xff808080 && allocation[logo_at + 3] == 0xffffffff);
    for (unsigned i = 0; i < 5000; i++) boot_progress_putc((i % 41) ? 'X' : '\n');
    assert(allocation[logo_at + 3] == 0xffffffff);
    boot_progress_stage("wrap, CR\rLF\n and invalid bytes \1\377");
    for (unsigned i = 0; i < GUARD; i++) {
        assert(allocation[i] == 0x12345678);
        assert(allocation[GUARD + PITCH * H + i] == 0x12345678);
    }
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
    assert(!geometry(UINTPTR_MAX - 3, W, H, PITCH * 4, 32));
    assert(!geometry(4, W, H, W * 4 - 4, 32));
    assert(!geometry(4, W, H, PITCH * 4, 16));
    assert(!geometry(4, W, 100000, PITCH * 4, 32));
    assert(!geometry(5, W, H, PITCH * 4, 32));
    // The persistent log mirrored the stages (the early ones replayed at VM ready, the contended one the screen skipped),
    // the console lines and the status line into both zones.
    assert(ramoops_seen(" S S1: XNU entry, MMU off\n") && ramoops_seen(" S S6: kernel VM and exception vectors ready\n"));
    assert(ramoops_seen(" S ramoops on 0x880000000 copies 36 x 0xfffc\n"));
    assert(ramoops_seen(" S contended secondary\n") && ramoops_seen(" S uncontended boot CPU\n"));
    assert(ramoops_seen(" C XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n") && ramoops_seen(" S wrap, CR?LF\n"));
    args.Video.v_width += 1ULL << 32;
    early_boot_progress_init(&args);
    // Every early init (the fatal path too) switches the persistent log off.
    assert(!boot_progress_active() && framebuffer_pa == 0 && !boot_progress_trace_enabled());
    args.Video.v_width = W;
    early_boot_progress_init(&args);

    uint64_t *tables = aligned_alloc(4096, 4 * 4096);
    assert(tables);
    memset(tables, 0, 4 * 4096);
    framebuffer_pa = 0x3ffff000;
    assert(early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    assert(tables[0] == ((uintptr_t)(tables + 512) | 3));
    assert(tables[1] == ((uintptr_t)(tables + 1024) | 3));
    assert(tables[512 + 511] == (0x3fe00000ULL | (1ULL << 54) | (1ULL << 53) | 0x605));
    assert(tables[1024] == (0x40000000ULL | (1ULL << 54) | (1ULL << 53) | 0x605));
    for (unsigned i = 1536; i < 2048; i++) assert(tables[i] == 0);
    // A repeated map uses the existing tables without consuming another page.
    assert(early_boot_progress_map(tables, (uintptr_t)(tables + 1536), (uintptr_t)(tables + 1536)));
    tables[1024] ^= 4;
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 1536), (uintptr_t)(tables + 1536)));
    memset(tables, 0, 4 * 4096);
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 512)));
    assert(tables[0] == 0);
    framebuffer_pa = (1ULL << (64 - T0SZ_BOOT)) - 4096;
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    framebuffer_pa = 1ULL << (64 - T0SZ_BOOT);
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    assert(tables[0] == 0);
    armcpu_timer_interrupts = 7;
    boot_progress_start(); poll(NULL, NULL);
    assert(!gunyah_wdog_status() && !wdog_platform_calls);   // pagani-wdog absent: the watchdog stays off
    assert(poll_count == 1 && armcpu_timer_interrupts == 7);
    boot_progress_root("root device wait begin", "IOMedia Content=Apple_HFS", 60);
    assert(root_sequence == 1 && root_value == 60);
    assert(!strcmp(root_stage, "root device wait begin"));
    assert(!strcmp(root_detail, "IOMedia Content=Apple_HFS"));
    native_owner_cache_stage = 3;
    vm_page_free_count = 1234; memorystatus_available_pages = 2345;
    native_ufs_block_reads = 5678; native_ufs_block_failures = 9;
    uint32_t root_line[LINE * PITCH];
    memcpy(root_line, (void *)(framebuffer + 3 * LINE * stride * 4), sizeof(root_line));
    unsigned saved_row = row, saved_column = column;
    poll(NULL, NULL);
    assert(!strcmp(storage_status, "free=1234 avail=2345 UFS=5678 err=9"));
    assert(!memcmp(root_line, (void *)(framebuffer + 3 * LINE * stride * 4), sizeof(root_line)));
    assert(row == saved_row && column == saved_column && native_ufs_block_reads == 5678);
    // pagani-overlay=0: the text primitives leave the framebuffer alone outside a panic (the panic below still paints).
    {
        static uint32_t before[PITCH * H];
        memcpy(before, (void *)framebuffer, sizeof(before));
        __atomic_store_n(&overlay_hidden, 1, __ATOMIC_RELAXED);
        status_paint(9, "hidden row");
        clear_line(2 * LINE);
        glyph_pixels('X', 0, 0, 0xffffffff);
        line_raw("hidden line");
        assert(!memcmp(before, (void *)framebuffer, sizeof(before)));
        unsigned y0 = 1, y1 = 1;
        assert(!boot_progress_gpu_band(&y0, &y1) && y0 == 1 && y1 == 1);   // no band: the presenter copies whole frames
        __atomic_store_n(&overlay_hidden, 0, __ATOMIC_RELAXED);
    }
    boot_progress_kernel_exception(0x96000010, 0xfffffff007345678, 0x12345000);
    assert(fault_valid && fault_esr == 0x96000010 && fault_far == 0x12345000);
    console_smp = 1; console_lock = 1; // A stopped console owner cannot block panic.
    panic_for_test("Failed to mount %s: %u %llx %%", "root device", 60U, 0x123456789abcdef0ULL);
    assert(panic_state == 1 && busy);
    assert(!boot_progress_display_handoff());
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    test_cpu = 0x10000;
    panic_for_test("second CPU must preserve first panic");
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    test_cpu = 0;
    assert(allocation[GUARD] == 0xffff4040);
    assert(allocation[GUARD + W / 3] == 0xff40ff40);
    assert(allocation[GUARD + 2 * W / 3] == 0xff4080ff);
    unsigned before_scheduled = scheduled, before_stages = stage_count;
    boot_progress_putc('X'); boot_progress_stage("must not overwrite panic");
    boot_progress_root("must not replace root state", NULL, 1); poll(NULL, NULL);
    assert(scheduled == before_scheduled && stage_count == before_stages && root_sequence == 1);
    assert(poll_count == 2 && armcpu_timer_interrupts == 7);
    uintptr_t before_framebuffer = framebuffer;
    boot_progress_bind(4, W, H, PITCH * 4, 32);
    assert(framebuffer == before_framebuffer);
    panic_for_test("nested failure %d", 123);
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    boot_progress_kernel_exception(0x96000004, 0xabcd, 0xdead);
    assert(fault_esr == 0x96000010); // The first fault remains available.
    early_boot_progress_init(&args);
    char long_message[3000];
    memset(long_message, 'P', sizeof(long_message) - 1); long_message[sizeof(long_message) - 1] = 0;
    for (unsigned i = 0; i < HISTORY + 3; i++) boot_progress_stage("history wraps within its fixed storage");
    panic_for_test("%s", long_message);
    assert(strlen(boot_progress_panic_text) == sizeof(boot_progress_panic_text) - 1 && panic_clipped);
    assert(allocation[GUARD] == 0xffff4040); // Long text never wraps over the panic banner.
    for (unsigned i = 0; i < GUARD; i++) {
        assert(allocation[i] == 0x12345678);
        assert(allocation[GUARD + PITCH * H + i] == 0x12345678);
    }
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
    early_boot_progress_init(&args);
    boot_progress_process_stage(42, "launchd", "load_machfile");
    assert(strstr(stages[(stage_count - 1) % HISTORY], "pid=42 launchd load_machfile"));
    boot_progress_exit(43, "backboardd", 1, 2, 3);
    assert(strstr(stages[(stage_count - 1) % HISTORY], "exit pid=43 backboardd"));
    boot_progress_display_status("GOP display started", 1);
    for (unsigned i = 0; i < HISTORY + 1; i++) boot_progress_process_stage(44, "launchd", "exec");
    assert(strstr(exit_stage, "exit pid=43 backboardd"));
    assert(!strcmp(display_stage, "GOP display started") && display_value == 1);
    for (unsigned i = 0; i < 29; i++) poll(NULL, NULL);
    assert(logo_visible && allocation[logo_at + 3] == 0xffffffff);
    poll(NULL, NULL);
    assert(!logo_visible && !display_owned && !panic_state && poll_count == 30);
    assert(!strcmp(display_status, "DISPLAY GOP display started 1"));
    early_boot_progress_init(&args);
    assert(logo_visible && poll_count == 0);
    assert(boot_progress_display_handoff());
    boot_progress_display_failed();
    assert(!display_owned);
    for (unsigned i = 0; i < 30; i++) poll(NULL, NULL);
    assert(!logo_visible);
    early_boot_progress_init(&args);
    busy = 1;
    assert(!boot_progress_display_handoff());
    busy = 0;
    assert(boot_progress_display_handoff());
    uint32_t *snapshot = malloc(PITCH * H * sizeof(uint32_t));
    assert(snapshot);
    memcpy(snapshot, allocation + GUARD, PITCH * H * sizeof(uint32_t));
    boot_progress_stage("must preserve system frame");
    boot_progress_root("frame ownership", "preserve pixels", 1);
    for (unsigned i = 0; i < 31; i++) poll(NULL, NULL);
    assert(!memcmp(snapshot, allocation + GUARD, PITCH * H * sizeof(uint32_t)));
    boot_progress_peripheral(PAGANI_TOUCH, "TouchComm start", -71, 0x1000010, 0);
    boot_progress_peripheral(PAGANI_KEYS, "HID report", 0, 0xc001, 3);
    boot_progress_peripheral(PAGANI_PERIPHERALS, "out of bounds", 0, 0, 0);
    boot_progress_peripheral(PAGANI_SPI, "reserved for SPI snapshot", 0, 0, 0);
    assert(!peripheral_status[PAGANI_SPI][0]);
    boot_progress_touch_spi(0x101,1,1,0x40000000,1,0x80000001);
    assert(strstr(peripheral_status[PAGANI_TOUCH], "r=ffffffb9 v=1000010 n=0"));
    assert(strstr(peripheral_status[PAGANI_KEYS], "n=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SPI fw=101 if=1 st=1 irq=40000000"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI FIFO tx=1 rx=80000001"));
    boot_progress_touch_smmu(7,81,0x800000a3,0);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SMMU slot=7 bank=51"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SMMU smr=800000a3 s2cr=0"));
    boot_progress_touch_spi(0x102,1,0x41,0,0,0);
    boot_progress_touch_dma(4,0,2,1);
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI DMA tx=4 rx=0 ti=2 ri=1"));
    boot_progress_touch_power(0x40800,0x30000,1,0,0x10100,0x100,0);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "PWR a=40800 hw=30000 t=1"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "PWR st=0 c=10100/100/0"));
    boot_progress_touch_read(4,0,13,1,0x40,4,4,0xc800);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SPI RX n=4 h=00000000 a=1/0 rst=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI dma=1 ti=4 ri=4 st=40 dly=c800"));
    boot_progress_touch_hid(31,0xd0004,430,420,3,0x40007fff,0x800000010000ULL,0x802);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "HID b=1f use=d/4 ev=430/420 e=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "XY in=4000/7fff out=8000/10000 mask=802"));
    saved_row=row;saved_column=column;
    boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));
    boot_progress_peripheral_enable(1);
    boot_progress_peripheral_frame();
    assert(display_owned && row==saved_row && column==saved_column);
    assert(!memcmp(snapshot,allocation+GUARD,4*LINE*PITCH*sizeof(uint32_t)));
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,5*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot + 9 * LINE * PITCH, allocation + GUARD + 9 * LINE * PITCH,
                   (H - 9 * LINE) * PITCH * sizeof(uint32_t)));
    for (unsigned y=4*LINE;y<9*LINE;++y)
        for (unsigned x=W;x<PITCH;++x) assert(allocation[GUARD+y*PITCH+x]==0x12345678);
    busy=1;memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));busy=0;
    boot_progress_peripheral(PAGANI_KEYS,"PMIC sample",0,0x8001,5);
    poll(NULL,NULL);
    assert(!memcmp(snapshot,allocation+GUARD,4*LINE*PITCH*sizeof(uint32_t)));
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,5*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+9*LINE*PITCH,allocation+GUARD+9*LINE*PITCH,(H-9*LINE)*PITCH*sizeof(uint32_t)));
    boot_progress_peripheral_enable(0);
    boot_progress_platform_enable(1);
    processor_count = processor_avail_count = 8; gPhysSize = 4UL << 30;
    for (unsigned cpu=0; cpu<8; cpu++) armcpu_core_timer[cpu] = armcpu_core_sgi[cpu] = 1;
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_peripheral_frame();
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,2*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+6*LINE*PITCH,allocation+GUARD+6*LINE*PITCH,(H-6*LINE)*PITCH*sizeof(uint32_t)));
    {
        // The platform row carries the copy statistics: C<last>/<mean>/<largest>us F<copies>.
        char platform[80];
        platform_line(platform, sizeof(platform));
        assert(!strcmp(platform, "CPU reg=8 online=8 T=ff I=ff C4100/4050/9000us F1234567"));
        native_legacy_copy_stats[0] = 3900; native_legacy_copy_stats[2] += 1; native_legacy_copy_stats[3] += 3900;
        platform_line(platform, sizeof(platform));
        assert(!strcmp(platform, "CPU reg=8 online=8 T=ff I=ff C3900/4050/9000us F1234568"));
        assert(strlen(platform) <= 66);
    }
    // Status rows: GPU rows stored by boot_progress_gpu_rows and display probe rows by boot_progress_dpu_rows, drawn by poll() on rows
    // 10-13 and 14-18 once per change, also after the display hand-off, and reported as one band the presentation paths leave alone.
    boot_progress_platform_enable(0);
    static const char gpu_rows[PAGANI_GPU_ROWS][PAGANI_GPU_ROW_BYTES] = {
        "GPU S0 INPUTS STOP ok 0ms", "SOC - FC - BIN - FW ok", "ARC gfx - mx - cx - gmxc -", "BCM SH0 - MC0 - ACV - CN0 -"};
    static const char dpu_rows[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES] = {
        "TE 120.00Hz AR 80000001 FC 120/s LOCK RL 0", "CTL0 PIPE 00010000 FLUSH 00000000 INTF 00000002 LM 00000001",
        "SSPP VIG0 SRC0 fc800000 STR 00001300 FMT 000237ff MR 0 +1", "MDP SID 800 S2C 00010000 SMR 80000800 BYPASS",
        "FLIP 3 LAT 5.4/4.1ms MISS 0 UR 0 TO 0"};
    unsigned y0 = 1, y1 = 1;
    const unsigned band = PAGANI_GPU_TOP_ROW * LINE, dband = PAGANI_DPU_TOP_ROW * LINE;
    assert(!boot_progress_gpu_band(&y0, &y1) && y0 == 1 && y1 == 1);
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_gpu_rows(gpu_rows, 1);
    assert(gpu_seq == 1 && gpu_count == 1 && !strcmp(gpu_status[0], gpu_rows[0]) && !gpu_status[1][0]);
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));   // stored, not drawn
    assert(boot_progress_gpu_band(&y0, &y1) && y0 == band && y1 == band + LINE);
    poll(NULL, NULL);
    assert(display_owned && gpu_drawn_seq == 1 && status_owned == 1);
    assert(!memcmp(snapshot,allocation+GUARD,band*PITCH*sizeof(uint32_t)));
    assert(memcmp(snapshot+band*PITCH,allocation+GUARD+band*PITCH,LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+(band+LINE)*PITCH,allocation+GUARD+(band+LINE)*PITCH,(H-band-LINE)*PITCH*sizeof(uint32_t)));
    for (unsigned y=band;y<band+LINE;++y)
        for (unsigned x=W;x<PITCH;++x) assert(allocation[GUARD+y*PITCH+x]==0x12345678);
    allocation[GUARD + band * PITCH] = 0x11111111;   // no change, no redraw
    poll(NULL, NULL);
    assert(allocation[GUARD + band * PITCH] == 0x11111111);
    boot_progress_gpu_rows(gpu_rows, 1);             // the same text again: a new sequence, but the row is not repainted
    poll(NULL, NULL);
    assert(allocation[GUARD + band * PITCH] == 0x11111111 && gpu_drawn_seq == gpu_seq);
    boot_progress_gpu_rows(gpu_rows, 4);
    assert(boot_progress_gpu_band(&y0, &y1) && y1 == band + 4 * LINE);
    poll(NULL, NULL);
    assert(allocation[GUARD + band * PITCH] == 0x11111111 && status_owned == 0xf);   // row 10 unchanged, rows 11-13 painted
    assert(memcmp(snapshot+(band+3*LINE)*PITCH,allocation+GUARD+(band+3*LINE)*PITCH,LINE*PITCH*sizeof(uint32_t)));
    boot_progress_gpu_rows(gpu_rows, 1);   // shrinking clears the rows no longer shown
    poll(NULL, NULL);
    for (unsigned y=band+LINE;y<band+4*LINE;++y)
        for (unsigned x=0;x<W;++x) assert(allocation[GUARD+y*PITCH+x]==0xff000000);
    assert(status_owned == 1);
    saved_row = row; saved_column = column;
    row = PAGANI_GPU_TOP_ROW - 1; column = 0;
    putc_raw('\n');                       // the log steps over the GPU row
    assert(row == PAGANI_GPU_TOP_ROW + 1);
    row = saved_row; column = saved_column;
    // Display probe rows join: the band runs from row 10 to the last probe row; rows 11-13 between the sets are black and owned.
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_dpu_rows(dpu_rows, 4);
    assert(dpu_seq == 1 && dpu_count == 4 && boot_progress_gpu_band(&y0, &y1) && y0 == band && y1 == dband + 4 * LINE);
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));   // stored, not drawn
    poll(NULL, NULL);
    assert(dpu_drawn_seq == 1 && status_owned == 0xff);
    for (unsigned y=band+LINE;y<dband;++y) for (unsigned x=0;x<W;++x) assert(allocation[GUARD+y*PITCH+x]==0xff000000);
    assert(memcmp(snapshot+dband*PITCH,allocation+GUARD+dband*PITCH,4*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+(dband+4*LINE)*PITCH,allocation+GUARD+(dband+4*LINE)*PITCH,(H-dband-4*LINE)*PITCH*sizeof(uint32_t)));
    saved_row = row; saved_column = column;
    row = PAGANI_GPU_TOP_ROW - 1; column = 0;
    putc_raw('\n');                       // the log steps over the whole band
    assert(row == PAGANI_DPU_TOP_ROW + 4);
    row = saved_row; column = saved_column;
    // Only a changed probe row is repainted; a fifth row (flips on) extends the band.
    // The last pixel of a row is always glyph background (column 5 of the last cell), so a repaint makes it black.
    allocation[GUARD + dband * PITCH + W - 1] = 0x22222222; allocation[GUARD + (dband + LINE) * PITCH + W - 1] = 0x33333333;
    char changed[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES];
    memcpy(changed, dpu_rows, sizeof(changed));
    memcpy(changed[1], "CTL1", 4);
    boot_progress_dpu_rows((const char (*)[PAGANI_GPU_ROW_BYTES])changed, 5);
    assert(boot_progress_gpu_band(&y0, &y1) && y1 == dband + 5 * LINE);
    poll(NULL, NULL);
    assert(allocation[GUARD + dband * PITCH + W - 1] == 0x22222222 && allocation[GUARD + (dband + LINE) * PITCH + W - 1] == 0xff000000);
    assert(status_owned == 0x1ff);
    // The GPU rows go away: the band starts at row 14; rows 10-13 are cleared once and handed back.
    boot_progress_gpu_rows(NULL, 0);
    assert(boot_progress_gpu_band(&y0, &y1) && y0 == dband && y1 == dband + 5 * LINE);
    allocation[GUARD + band * PITCH] = 0x44444444;
    poll(NULL, NULL);
    assert(allocation[GUARD + band * PITCH] == 0xff000000 && status_owned == 0x1f0);
    allocation[GUARD + band * PITCH] = 0x55555555;   // no longer ours: never touched again
    poll(NULL, NULL);
    assert(allocation[GUARD + band * PITCH] == 0x55555555);
    boot_progress_dpu_rows(dpu_rows, 9);   // clamped to five rows
    assert(dpu_count == PAGANI_DPU_ROWS);
    boot_progress_dpu_rows(NULL, 0);
    boot_progress_gpu_rows(gpu_rows, 9);   // clamped to four rows
    assert(gpu_count == 4);
    boot_progress_gpu_rows(NULL, 2);       // no rows: nothing shown, no band
    assert(gpu_count == 0 && dpu_count == 0 && !boot_progress_gpu_band(&y0, &y1));
    poll(NULL, NULL);
    assert(status_owned == 0);
    for (unsigned y=band;y<dband+5*LINE;++y) assert(allocation[GUARD+y*PITCH]==0xff000000 || y == band);
    boot_progress_gpu_rows(gpu_rows, 4);
    boot_progress_display_frames(4242);
    assert(display_frames == 4242);
    panic_for_test("panic after system display handoff");
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    unsigned gpu_before = gpu_seq, dpu_before = dpu_seq;
    boot_progress_gpu_rows(gpu_rows, 1); boot_progress_dpu_rows(dpu_rows, 2); poll(NULL, NULL);   // a panic owns the screen
    assert(gpu_seq == gpu_before && gpu_count == 4 && dpu_seq == dpu_before && dpu_count == 0);
    assert(display_frames == 4242);     // printed by the panic screen above
    boot_progress_peripheral(PAGANI_KEYS,"must preserve panic",0,0,0);
    boot_progress_touch_spi(0,0,0,0,0,0);boot_progress_touch_smmu(0,0,0,0);
    boot_progress_touch_dma(0,0,0,0);boot_progress_touch_read(0,0,0,0,0,0,0,0);boot_progress_touch_power(0,0,0,0,0,0,0);
    boot_progress_touch_hid(0,0,0,0,0,0,0,0);boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));
    assert(strstr(peripheral_status[PAGANI_KEYS],"PMIC sample"));
    assert(strstr(peripheral_status[PAGANI_SPI_FIFO],"in=4000/7fff out=8000/10000 mask=802"));
    free(snapshot);
    boot_progress_display_failed();
    assert(allocation[GUARD] == 0xffff4040 && !boot_progress_display_handoff());
    early_boot_progress_suspend();
    assert(!boot_progress_active());
    // FPS and SLOW rows and the frame-rate log (pagani-fps, FrameRate.h): frames of both paths through boot_progress_frame and
    // boot_progress_frame_timed, the boot-arg defaults, the rows on text rows 8 (SLOW) and 9 (FPS) as part of the band (drawn only when
    // their text changed), the band with the other rows, the F line in the console ring (only while frames come or when it changed, with
    // the SLOW row while it matters) and the J records in the events ring with their rate limit and the late frame's breakdown, the
    // display events of each second, the stage the display path waits in, and the peripheral overlay leaving row 8 to the SLOW row.
    {
        enum { HZ = 19200000 };
        early_boot_progress_init(&args);
        early_boot_progress_vm_ready();                     // the persistent log again (pagani-ramoops=1 in this test)
        assert(boot_progress_trace_enabled() && fps_mode == 0 && !fps_row && presented.count == 0);
        unsigned y0 = 0, y1 = 0;
        fps_configure();
        assert(fps_mode == 0 && !fps_row && !boot_progress_gpu_band(&y0, &y1));
        test_arg("pagani-gpu", 1);                          // the GPU status rows (pagani-gpu-status is 1 unless given)
        fps_configure();
        assert(fps_mode == 1 && fps_row);
        test_arg("pagani-gpu-status", 0);
        fps_configure();
        assert(fps_mode == 0 && !fps_row);
        test_arg("pagani-platform-status", 1);
        fps_configure();
        assert(fps_mode == 1 && fps_row);
        test_arg("pagani-fps", 0);
        fps_configure();
        assert(fps_mode == 0 && !fps_row);
        test_arg_count = 0; test_arg("pagani-fps", 2);
        fps_configure();
        assert(fps_mode == 2 && !fps_row && !boot_progress_gpu_band(&y0, &y1));
        test_arg_count = 0; test_arg("pagani-fps", 7);
        fps_configure();
        assert(fps_mode == 1 && fps_row);
        assert(ramoops_seen(" S fps: rows 8-9 and ramoops F/J records (pagani-fps=1)\n"));
        // The band is rows 8-9 alone (what the CPU copy and the GPU client leave out); the log steps over both.
        const unsigned slow_top = PAGANI_SLOW_ROW * LINE, fps_top = PAGANI_FPS_ROW * LINE;
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == slow_top && y1 == fps_top + LINE && slow_top + LINE == fps_top);
        row = PAGANI_SLOW_ROW - 1; column = 0;
        putc_raw('\n');
        assert(row == PAGANI_FPS_ROW + 1);
        assert(boot_progress_display_handoff());
        uint32_t *before = malloc(PITCH * H * sizeof(uint32_t));
        assert(before);
        // No frame yet: rows 8 and 9 are painted, nothing else changes; the log gets the line once.
        test_clock = 100ULL * HZ;
        memcpy(before, allocation + GUARD, PITCH * H * sizeof(uint32_t));
        poll(NULL, NULL);
        assert(!strcmp(fps_status, "FPS 0 (0-0) | ft - | gpu 0 cpu 0") && fps_owned && fps_drawn_seq == fps_seq);
        assert(!strcmp(slow_status, "SLOW - fb0 to0") && slow_owned);
        assert(!memcmp(before, allocation + GUARD, slow_top * PITCH * sizeof(uint32_t)));
        assert(memcmp(before + slow_top * PITCH, allocation + GUARD + slow_top * PITCH, LINE * PITCH * sizeof(uint32_t)));
        assert(memcmp(before + fps_top * PITCH, allocation + GUARD + fps_top * PITCH, LINE * PITCH * sizeof(uint32_t)));
        assert(!memcmp(before + (fps_top + LINE) * PITCH, allocation + GUARD + (fps_top + LINE) * PITCH,
                       (H - fps_top - LINE) * PITCH * sizeof(uint32_t)));
        for (unsigned y = slow_top; y < fps_top + LINE; y++)
            for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 0 (0-0) | ft - | gpu 0 cpu 0 | jit 0\n") == 1);
        assert(ramoops_area_count(AREA_EVENTS, " H up ") == 1 && ramoops_area_count(AREA_EVENTS, " cpu 4320MHz on 5\n") == 1);
        // The same texts a second later: neither repainted nor logged again.
        // (The last pixel of a row is always glyph background: a repaint makes it black.)
        allocation[GUARD + fps_top * PITCH + W - 1] = 0x11111111;
        allocation[GUARD + slow_top * PITCH + W - 1] = 0x11111111;
        test_clock += HZ;
        poll(NULL, NULL);
        assert(allocation[GUARD + fps_top * PITCH + W - 1] == 0x11111111 && allocation[GUARD + slow_top * PITCH + W - 1] == 0x11111111);
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 0 (0-0)") == 1);
        // Sixty GPU frames at 60 Hz, all within the last second (without a breakdown: the SLOW row has the interval alone).
        const uint64_t t0 = test_clock + HZ / 10;
        for (unsigned k = 0; k < 60; k++) boot_progress_frame(PAGANI_FRAME_GPU, t0 + k * (HZ / 60));
        assert(presented.count == 60 && presented.paths[PAGANI_FRAME_GPU] == 60 && !presented.paths[PAGANI_FRAME_CPU]);
        test_clock = t0 + HZ - 1;
        poll(NULL, NULL);
        assert(!strcmp(fps_status, "FPS 60 (0-60) | ft 16.7 p99 16.7 max 16.7ms | gpu 60 cpu 0"));
        assert(!strcmp(slow_status, "SLOW 16.7ms fb0 to0"));
        assert(allocation[GUARD + fps_top * PITCH + W - 1] == 0xff000000);    // the changed texts were painted
        assert(allocation[GUARD + slow_top * PITCH + W - 1] == 0xff000000);
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 60 (0-60) | ft 16.7 p99 16.7 max 16.7ms | gpu 60 cpu 0 | jit 0\n") == 1);
        // A CPU frame 88 ms after the last one: the rows show it, the F line counts it and carries the SLOW row (above 50 ms), a J record
        // in the events ring dates it in the log's own milliseconds (10 ms before the round, the log having run 100 s).
        const uint64_t late = t0 + 59 * (HZ / 60) + HZ / 1000 * 88;
        boot_progress_frame(PAGANI_FRAME_CPU, late);
        test_clock = late + HZ / 100;
        ramoops_clock += 100ULL * HZ;
        poll(NULL, NULL);
        assert(!strcmp(fps_status, "FPS 56 (0-56) | ft 17.9 p99 48.8 max 88.0ms | gpu 55 cpu 1"));
        assert(!strcmp(slow_status, "SLOW 88.0ms fb0 to0"));
        assert(presented.count == 61 && presented.paths[PAGANI_FRAME_CPU] == 1);
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 56 (0-56) | ft 17.9 p99 48.8 max 88.0ms | gpu 55 cpu 1 | jit 1 | SLOW 88.0ms "
                                                "fb0 to0\n") == 1);
        assert(ramoops_area_count(AREA_EVENTS, " J jitter 88.0ms cpu at ") == 1 && !ramoops_area_count(AREA_CONSOLE, " J jitter"));
        assert(!ramoops_area_count(AREA_EVENTS, " F FPS"));
        {
            const volatile uint8_t *base = copies[0] + areas[AREA_EVENTS].offset;
            const char *key = " J jitter 88.0ms cpu at ";
            const size_t n = strlen(key);
            uint64_t at = 0;
            for (uint32_t i = 0; i + n < areas[AREA_EVENTS].bytes; i++) {
                size_t k = 0;
                while (k < n && base[i + k] == (uint8_t)key[k]) k++;
                if (k < n) continue;
                uint32_t j = i + (uint32_t)n;
                for (; base[j] >= '0' && base[j] <= '9'; j++) at = at * 10 + (base[j] - '0');
                // No breakdown for this frame (boot_progress_frame): this second's fallbacks and empty client waits only.
                const char *tail = "ms fb0 to0\n";
                for (unsigned c = 0; tail[c]; c++) assert(base[j + c] == (uint8_t)tail[c]);
                break;
            }
            const uint64_t elapsed = elapsed_ms();
            assert(at >= 99000 && at + 10 <= elapsed && at + 10 + 100 >= elapsed);
        }
        // Ten jitters in one round (GPU frames 60 ms apart): the limit (four at once, then one per 5 s; one is spent) logs three.
        uint64_t t = late;
        for (unsigned k = 0; k < 10; k++) { t += HZ / 1000 * 60; boot_progress_frame(PAGANI_FRAME_GPU, t); }
        test_clock = t + 1;
        poll(NULL, NULL);
        assert(ramoops_area_count(AREA_EVENTS, " J jitter 60.0ms gpu at ") == 3 && jitter_limit.skipped == 7);
        assert(ramoops_area_count(AREA_CONSOLE, " | jit 10 | SLOW 88.0ms fb0 to0\n") == 1);     // the 88 ms frame is in the second
        // Five seconds on the next one goes out with the number left out.
        t += HZ / 1000 * 70;
        boot_progress_frame(PAGANI_FRAME_GPU, t);
        test_clock = t + 5ULL * HZ;
        poll(NULL, NULL);
        assert(ramoops_area_count(AREA_EVENTS, " J jitter 70.0ms gpu at ") == 1 && ramoops_area_count(AREA_EVENTS, "ms fb0 to0 skipped 7\n") == 1);
        assert(!jitter_limit.skipped);
        // A gap of 3 s is idle, not a jitter, and no frame time either.
        t = test_clock + 3ULL * HZ;
        boot_progress_frame(PAGANI_FRAME_GPU, t);
        boot_progress_frame(PAGANI_FRAME_GPU, t + HZ / 60);
        test_clock = t + HZ / 30;
        unsigned jitter_records = ramoops_area_count(AREA_EVENTS, " J jitter");
        poll(NULL, NULL);
        assert(ramoops_area_count(AREA_EVENTS, " J jitter") == jitter_records);
        assert(strstr(fps_status, "FPS 2 (") && strstr(fps_status, "| ft 16.7 p99 16.7 max 16.7ms | gpu 2 cpu 0"));
        assert(!strcmp(slow_status, "SLOW 16.7ms fb0 to0"));
        // Idle: once the ten windows are empty the lines stay, and the log does not repeat them.
        for (unsigned i = 0; i < 12; i++) { test_clock += HZ; poll(NULL, NULL); }
        assert(!strcmp(fps_status, "FPS 0 (0-0) | ft - | gpu 0 cpu 0") && !strcmp(slow_status, "SLOW - fb0 to0"));
        unsigned idle_lines = ramoops_area_count(AREA_CONSOLE, " F FPS 0 (0-0)");
        for (unsigned i = 0; i < 3; i++) { test_clock += HZ; poll(NULL, NULL); }
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 0 (0-0)") == idle_lines);
        // With the GPU rows the band is rows 8-13; with the display probe rows alone 8-18, rows 10-13 black between them.
        boot_progress_gpu_rows(gpu_rows, 4);
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == slow_top && y1 == PAGANI_DPU_TOP_ROW * LINE);
        poll(NULL, NULL);
        assert(status_owned == 0xf && fps_owned && slow_owned);
        row = PAGANI_SLOW_ROW - 1; column = 0;
        putc_raw('\n');
        assert(row == PAGANI_DPU_TOP_ROW);
        boot_progress_gpu_rows(NULL, 0);
        boot_progress_dpu_rows(dpu_rows, 5);
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == slow_top && y1 == (PAGANI_DPU_TOP_ROW + 5) * LINE);
        poll(NULL, NULL);
        assert(status_owned == 0x1ff && fps_owned && slow_owned);
        for (unsigned y = PAGANI_GPU_TOP_ROW * LINE; y < PAGANI_DPU_TOP_ROW * LINE; y++)
            for (unsigned x = 0; x < W; x++) assert(allocation[GUARD + y * PITCH + x] == 0xff000000);
        boot_progress_dpu_rows(NULL, 0);
        // Timed frames (boot_progress_frame_timed, as NativeLegacyDisplay reports them through Stage7Probe) and display events: the
        // second's slowest interval with its frame's breakdown on row 8, fb (refused offers and failed GPU copies) and to (the GPU
        // client's empty waits) of this second; the F line and the J record carry the same.
        {
            const uint64_t ms = HZ / 1000;
            uint64_t a = test_clock + HZ / 10;
            struct pagani_frame_timing timing;
            uint64_t presented_at = 0;
            for (unsigned k = 0; k < 31; k++) {
                // 30 frames at 60 Hz (gap 16.7, off 0.2, take 0.3, copy 3.0, done 0.1), then one the compositor sent 70.1 ms after the
                // one before, whose copy took 17.0 ms (queued behind rendering): presented 84.1 ms after its predecessor.
                const int slow_one = k == 30;
                if (k) a += slow_one ? ms * 701 / 10 : HZ / 60;
                timing.arrival = a; timing.gap = k ? (slow_one ? ms * 701 / 10 : HZ / 60) : 0;
                timing.offer = a + ms / 5; timing.taken = timing.offer + ms * 3 / 10;
                timing.copied = timing.taken + (slow_one ? 17 * ms : 3 * ms);
                timing.flags = slow_one ? PAGANI_TIMING_DEFERRED : 0; timing.reserved = 0;
                presented_at = timing.copied + ms / 10;
                boot_progress_frame_timed(PAGANI_FRAME_GPU, presented_at, &timing);
            }
            boot_progress_display_event(PAGANI_DISPLAY_REFUSED);
            boot_progress_display_event(PAGANI_DISPLAY_FAILED);
            for (unsigned i = 0; i < 3; i++) boot_progress_display_event(PAGANI_DISPLAY_TIMEOUT);
            boot_progress_display_event(PAGANI_DISPLAY_EVENTS);         // out of range: ignored
            boot_progress_frame_stage(PAGANI_STAGE_IDLE, presented_at);
            boot_progress_frame_stage(PAGANI_STAGES, presented_at);    // out of range: ignored
            test_clock = presented_at + 2 * ms;
            allocation[GUARD + slow_top * PITCH + W - 1] = 0x44444444;
            poll(NULL, NULL);
            assert(!strcmp(slow_status, "SLOW 84.1ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb2 to3"));
            assert(strlen(slow_status) <= 65 && allocation[GUARD + slow_top * PITCH + W - 1] == 0xff000000);
            assert(ramoops_area_count(AREA_CONSOLE, " | SLOW 84.1ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb2 to3 | ref 1 fail 1 "
                                                    "early 0\n") == 1);
            assert(ramoops_area_count(AREA_EVENTS, "ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 deferred fb2 to3\n") == 1);
            assert(ramoops_area_count(AREA_EVENTS, " J jitter 84.1ms gpu at ") == 1);
            // The next second without frames: the counts start over; the path waits for the compositor ("idle", since the last frame).
            test_clock = presented_at + HZ + HZ / 2;
            poll(NULL, NULL);
            assert(!strcmp(slow_status, "SLOW - idle 1.5s fb0 to0"));
            // A copy that never ends (the GPU client took the frame and went quiet): the row says where the frame is stuck and the F
            // line logs it every second while it grows.
            boot_progress_frame_stage(PAGANI_STAGE_TAKEN, test_clock);
            boot_progress_display_event(PAGANI_DISPLAY_EARLY_FAILED);
            test_clock += 2 * HZ + HZ / 3;
            poll(NULL, NULL);
            assert(!strcmp(slow_status, "SLOW - copy 2.3s fb0 to0"));
            assert(ramoops_area_count(AREA_CONSOLE, " | jit 0 | SLOW - copy 2.3s fb0 to0 | ref 0 fail 0 early 1\n") == 1);
            test_clock += HZ;
            poll(NULL, NULL);
            assert(!strcmp(slow_status, "SLOW - copy 3.3s fb0 to0"));
            assert(ramoops_area_count(AREA_CONSOLE, " | jit 0 | SLOW - copy 3.3s fb0 to0\n") == 1);
            boot_progress_frame_stage(PAGANI_STAGE_IDLE, test_clock);
            test_clock += HZ;
            poll(NULL, NULL);
            assert(!strcmp(slow_status, "SLOW - idle 1.0s fb0 to0") && !ramoops_area_count(AREA_CONSOLE, "SLOW - idle"));
        }
        // The peripheral overlay (rows 4-8) gives row 8 to the SLOW row: rows 4-7 only while it is shown.
        for (unsigned i = 0; i < PAGANI_PERIPHERALS; i++) copy_line(peripheral_status[i], sizeof(peripheral_status[i]), "PERIPHERAL ROW");
        boot_progress_peripheral_enable(1);
        allocation[GUARD + 7 * LINE * PITCH] = 0x55555555;
        allocation[GUARD + slow_top * PITCH] = 0x55555555;
        boot_progress_peripheral_frame();
        assert(allocation[GUARD + 7 * LINE * PITCH] != 0x55555555 && allocation[GUARD + slow_top * PITCH] == 0x55555555);
        boot_progress_peripheral_enable(0);
        // pagani-fps=2: both rows go (cleared once, no band), the log keeps its line.
        test_arg_count = 0; test_arg("pagani-fps", 2);
        fps_configure();
        allocation[GUARD + fps_top * PITCH + W - 1] = 0x22222222;
        allocation[GUARD + slow_top * PITCH + W - 1] = 0x22222222;
        poll(NULL, NULL);
        assert(fps_mode == 2 && !fps_row && !fps_owned && !slow_owned && !status_owned && !boot_progress_gpu_band(&y0, &y1));
        assert(allocation[GUARD + fps_top * PITCH + W - 1] == 0xff000000 && allocation[GUARD + slow_top * PITCH + W - 1] == 0xff000000);
        allocation[GUARD + fps_top * PITCH + W - 1] = 0x33333333;
        allocation[GUARD + slow_top * PITCH + W - 1] = 0x33333333;
        // Without the row the peripheral overlay has row 8 again.
        boot_progress_peripheral_enable(1);
        boot_progress_peripheral_frame();
        assert(allocation[GUARD + slow_top * PITCH] != 0x55555555);
        boot_progress_peripheral_enable(0);
        allocation[GUARD + slow_top * PITCH + W - 1] = 0x33333333;
        boot_progress_frame(PAGANI_FRAME_CPU, 0);           // 0: stamped now
        test_clock += HZ / 2;
        poll(NULL, NULL);
        assert(allocation[GUARD + fps_top * PITCH + W - 1] == 0x33333333 && allocation[GUARD + slow_top * PITCH + W - 1] == 0x33333333);
        // (The ten windows still hold the 31 timed frames above.)
        assert(!strcmp(fps_next, "FPS 1 (0-31) | ft - | gpu 0 cpu 1"));
        assert(ramoops_area_count(AREA_CONSOLE, " F FPS 1 (0-31) | ft - | gpu 0 cpu 1 | jit 0\n") == 1);
        test_arg_count = 0;
        free(before);
    }
    // The CPU row (PaganiArmCpu pagani-cpufreq, boot_progress_cpu_row): stored, then drawn by poll() on row 14 only when its text changed,
    // cut to 65 characters, part of the band (alone: rows 14-15 of pixels [14, 15) * LINE), moved to row 19 below the display probe rows
    // while those are shown and back, the log stepping over it, removed with NULL (cleared once, handed back), ignored once a panic owns
    // the screen.
    {
        early_boot_progress_init(&args);
        early_boot_progress_vm_ready();
        assert(cpu_row == 0 && cpu_seq == 0 && cpu_status[0] == 0);
        assert(boot_progress_display_handoff());
        unsigned y0 = 0, y1 = 0;
        const unsigned cpu_top = PAGANI_CPU_ROW * LINE, low_top = PAGANI_CPU_ROW_LOW * LINE, gpu_top = PAGANI_GPU_TOP_ROW * LINE;
        const unsigned bit14 = 1U << (PAGANI_CPU_ROW - PAGANI_GPU_TOP_ROW), bit19 = 1U << (PAGANI_CPU_ROW_LOW - PAGANI_GPU_TOP_ROW);
        assert(PAGANI_CPU_ROW == 14 && PAGANI_CPU_ROW_LOW == 19 && STATUS_ROWS == 10 && (PAGANI_CPU_ROW_LOW + 1) * LINE == H);
        assert(!boot_progress_gpu_band(&y0, &y1));
        uint32_t *before = malloc(PITCH * H * sizeof(uint32_t));
        assert(before);
        memcpy(before, allocation + GUARD, PITCH * H * sizeof(uint32_t));
        boot_progress_cpu_row("CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 0");
        assert(cpu_row == 1 && cpu_seq == 1 && !strcmp(cpu_status, "CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 0"));
        assert(!memcmp(before, allocation + GUARD, PITCH * H * sizeof(uint32_t)));          // stored, not drawn
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == cpu_top && y1 == cpu_top + LINE);
        poll(NULL, NULL);
        assert(cpu_drawn_seq == 1 && status_owned == bit14);
        assert(!memcmp(before, allocation + GUARD, cpu_top * PITCH * sizeof(uint32_t)));
        assert(memcmp(before + cpu_top * PITCH, allocation + GUARD + cpu_top * PITCH, LINE * PITCH * sizeof(uint32_t)));
        assert(!memcmp(before + (cpu_top + LINE) * PITCH, allocation + GUARD + (cpu_top + LINE) * PITCH,
                       (H - cpu_top - LINE) * PITCH * sizeof(uint32_t)));
        for (unsigned y = cpu_top; y < cpu_top + LINE; y++)
            for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
        // The same text again: no new sequence, no repaint. (The last pixel of a row is glyph background: a repaint makes it black.)
        allocation[GUARD + cpu_top * PITCH + W - 1] = 0x11111111;
        boot_progress_cpu_row("CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 0");
        poll(NULL, NULL);
        assert(cpu_seq == 1 && allocation[GUARD + cpu_top * PITCH + W - 1] == 0x11111111);
        // A changed text is painted; longer than 65 characters it is cut, unprintable bytes become spaces.
        char longer[100];
        memset(longer, 'L', sizeof(longer) - 1); longer[sizeof(longer) - 1] = 0; longer[3] = '\1';
        boot_progress_cpu_row(longer);
        assert(cpu_seq == 2 && strlen(cpu_status) == PAGANI_CPU_ROW_WIDTH && cpu_status[3] == ' ' && cpu_status[64] == 'L');
        poll(NULL, NULL);
        assert(allocation[GUARD + cpu_top * PITCH + W - 1] == 0xff000000 && cpu_drawn_seq == 2);
        boot_progress_cpu_row("CPU 1996/2438MHz lv8/6 ld 34/12% boost 3 thr 0 err 0");
        assert(cpu_seq == 3);
        // The log steps over the row.
        unsigned saved_row = row, saved_column = column;
        row = PAGANI_CPU_ROW - 1; column = 0;
        putc_raw('\n');
        assert(row == PAGANI_CPU_ROW + 1);
        row = saved_row; column = saved_column;
        // With the GPU rows the band is rows 10-14; with the display probe rows the CPU row goes to row 19 (band 10-19) and back.
        static const char gpu_rows[PAGANI_GPU_ROWS][PAGANI_GPU_ROW_BYTES] = {"GPU A", "GPU B", "GPU C", "GPU D"};
        static const char dpu_rows[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES] = {"TE A", "CTL B", "SSPP C", "MDP D", "FLIP E"};
        boot_progress_gpu_rows(gpu_rows, 4);
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == gpu_top && y1 == cpu_top + LINE);
        poll(NULL, NULL);
        assert(status_owned == (0xfU | bit14));
        boot_progress_dpu_rows(dpu_rows, 5);
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == gpu_top && y1 == low_top + LINE);
        allocation[GUARD + low_top * PITCH + W - 1] = 0x22222222;
        poll(NULL, NULL);
        assert(status_owned == 0x3ffU && !strcmp(status_shown[PAGANI_CPU_ROW - PAGANI_GPU_TOP_ROW], "TE A"));
        assert(!strcmp(status_shown[PAGANI_CPU_ROW_LOW - PAGANI_GPU_TOP_ROW], cpu_status));
        assert(allocation[GUARD + low_top * PITCH + W - 1] == 0xff000000);
        row = PAGANI_GPU_TOP_ROW - 1; column = 0;
        putc_raw('\n');                           // the log steps over the whole band, rows 10-19 (then wraps to the top of the log)
        assert(row == LOG_TOP);
        row = saved_row; column = saved_column;
        boot_progress_dpu_rows(NULL, 0);
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == gpu_top && y1 == cpu_top + LINE);
        allocation[GUARD + low_top * PITCH] = 0x33333333;
        poll(NULL, NULL);
        assert(status_owned == (0xfU | bit14) && !strcmp(status_shown[PAGANI_CPU_ROW - PAGANI_GPU_TOP_ROW], cpu_status));
        assert(allocation[GUARD + low_top * PITCH] == 0xff000000);    // row 19 cleared once and handed back
        allocation[GUARD + low_top * PITCH] = 0x44444444;
        poll(NULL, NULL);
        assert(allocation[GUARD + low_top * PITCH] == 0x44444444 && !(status_owned & bit19));
        boot_progress_gpu_rows(NULL, 0);
        // With the FPS rows (pagani-fps=1) and the CPU row alone the band is rows 8-14, rows 10-13 black between them.
        test_arg("pagani-fps", 1);
        fps_configure();
        assert(boot_progress_gpu_band(&y0, &y1) && y0 == PAGANI_SLOW_ROW * LINE && y1 == cpu_top + LINE);
        poll(NULL, NULL);
        assert(status_owned == (0xfU | bit14) && fps_owned && slow_owned);
        for (unsigned y = gpu_top; y < cpu_top; y++)
            for (unsigned x = 0; x < W; x++) assert(allocation[GUARD + y * PITCH + x] == 0xff000000);
        test_arg_count = 0;
        fps_configure();
        // NULL removes the row: no band, row 14 cleared once and handed back.
        boot_progress_cpu_row(NULL);
        assert(cpu_row == 0 && cpu_seq == 4 && !boot_progress_gpu_band(&y0, &y1));
        allocation[GUARD + cpu_top * PITCH] = 0x55555555;
        poll(NULL, NULL);
        assert(status_owned == 0 && allocation[GUARD + cpu_top * PITCH] == 0xff000000);
        allocation[GUARD + cpu_top * PITCH] = 0x66666666;
        poll(NULL, NULL);
        assert(allocation[GUARD + cpu_top * PITCH] == 0x66666666);
        boot_progress_cpu_row("");                 // empty: still no row, no new sequence
        assert(cpu_row == 0 && cpu_seq == 4);
        // A panic owns the screen: the row is neither stored nor drawn.
        boot_progress_cpu_row("CPU scmi off: timeout lvl get");
        assert(cpu_row == 1 && cpu_seq == 5);
        panic_for_test("panic with the CPU row");
        memcpy(before, allocation + GUARD, PITCH * H * sizeof(uint32_t));
        boot_progress_cpu_row("CPU 4320/4320MHz lv15/15 ld 99/99% boost 1 thr 0 err 0");
        poll(NULL, NULL);
        assert(cpu_seq == 5 && !strcmp(cpu_status, "CPU scmi off: timeout lvl get"));
        assert(!memcmp(before, allocation + GUARD, PITCH * H * sizeof(uint32_t)));
        early_boot_progress_init(&args);
        assert(cpu_row == 0 && cpu_seq == 0 && cpu_drawn_seq == 0 && cpu_status[0] == 0);
        free(before);
    }
    // pagani-reset-seq=0x100: when the reset paths start, the probe's answers become a stage line (the screen's history and an S
    // record in the persistent log).
    {
        early_boot_progress_init(&args);
        console_reset_seq = 0x100;
        early_boot_progress_vm_ready();
        start_reset_paths();
        const char *line = "reset seq 0x100 avail sdi 1 dl 1 rd 1 wr 1 spmi 1 oem 1 dload 0x20";
        assert(console_smcs == 7 && !strcmp(stages[(stage_count - 1) % HISTORY], line));
        assert(ramoops_seen(" S reset seq 0x100 avail sdi 1 dl 1 rd 1 wr 1 spmi 1 oem 1 dload 0x20\n"));
        assert(ramoops_seen(" U reset seq probe: dload read SCM 0xc2000501(0x1fd9000) = 0 x1 0x20\n"));
        console_reset_seq = 0;
        early_boot_progress_init(&args);
    }
    free(tables); free(allocation);
    assert(!wdog_platform_calls);   // the panics, exceptions and observer rounds above left the watchdog alone
    puts("PASS: framebuffer/map bounds, write-combined alias kept on rebind, root state, panic formatting, truncation, first-fault "
         "retention, frozen observer, copy statistics, display frames, GPU and display probe status band, FPS and SLOW rows (row 8: the "
         "slowest frame's breakdown, fallbacks, empty client waits, the stuck stage), frame-rate log with F/J breakdowns, heartbeat CPU "
         "clock, CPU row (row 14 or 19, cut to 65, redrawn on change, band, removal, panic), the pagani-reset-seq probe's stage line");
    return 0;
}
