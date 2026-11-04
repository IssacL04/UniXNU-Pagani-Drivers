// SPDX-License-Identifier: MIT
// What UfsLog.c and RamoopsLog.c stand on in the host tests of the UFS log (ufslog.c, ufslog-threads.c, ufslog-e2e.c): boot-args, the counter,
// PSCI and the ramoops region. Each test adds the export lock and NativeUfs. The clock is atomic for the test with threads; it moves by
// clock_step on every read, and by what a test adds to it.
enum { HZ = 19200000, REGION = 0x240000 };
static uint8_t region[REGION];
static uint64_t clock_now = 1000000, clock_step = 1;
static struct { const char *name; uint64_t value; } table[16];
static unsigned table_count;
static const char *boot_args_text = "sheng-log=1 pagani-ramoops=1";
static int thread_starts, thread_start_fails;
static unsigned allocations, alloc_fail;
static int64_t psci_resets;
static uint64_t psci_reset_at;
static int wdog_armed = 1;                  // the watchdog's answer to UfsLog.c
static unsigned pets_stopped, wdog_flushed;
static void (*psci_reset_hook)(void);       // runs at the reset, as the phone would stop there
static void (*wdog_bite_hook)(void), (*wdog_flushed_hook)(void);

static void arg(const char *name, uint64_t value)
{
    assert(table_count < 16);
    table[table_count].name = name;
    table[table_count++].value = value;
}

static int find_arg(const char *name, uint64_t *value)
{
    for (unsigned i = table_count; i-- > 0;) if (!strcmp(table[i].name, name)) { *value = table[i].value; return 1; }
    return 0;
}

uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{
    assert(pa == RAMOOPS_LOG_DEFAULT_PA && bytes == REGION && !device);
    return (uintptr_t)region;
}

int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    uint64_t found;
    if (!find_arg(name, &found)) return 0;
    memcpy(value, &found, bytes);
    return 1;
}

const char *ramoops_platform_boot_args(void) { return boot_args_text; }
uint64_t ramoops_platform_counter(void) { return __atomic_add_fetch(&clock_now, __atomic_load_n(&clock_step, __ATOMIC_RELAXED), __ATOMIC_RELAXED); }
uint64_t ramoops_platform_frequency(void) { return HZ; }
uint32_t ramoops_platform_cpu_mhz(int *cpu) { *cpu = 0; return 1000; }
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
    (void)hvc; (void)a; (void)b; (void)c;
    if (function == PSCI_VERSION) return 0x10001;
    if (function == PSCI_FEATURES) return -1;
    if (function == PSCI_SYSTEM_RESET && !__atomic_fetch_add(&psci_resets, 1, __ATOMIC_RELAXED)) {
        psci_reset_at = __atomic_load_n(&clock_now, __ATOMIC_RELAXED);
        if (psci_reset_hook) psci_reset_hook();
    }
    return 0;
}
int ramoops_platform_psci_method(char method[4]) { (void)method; return 0; }
int ramoops_platform_arm_timer(uint64_t milliseconds) { (void)milliseconds; return 0; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid) { (void)milliseconds; (void)avoid; return 1; }
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = 0x80000000; *bytes = 0x100000; }
void ramoops_platform_smc(uint64_t regs[8]) { regs[0] = 0; }
unsigned ramoops_platform_park_others(uint32_t *parked) { (void)parked; return 0; }
int ramoops_platform_wdog_bite(const char *reason) { (void)reason; if (wdog_bite_hook) wdog_bite_hook(); return 0; }
int ramoops_platform_wdog_hold(const char *reason) { (void)reason; return 0; }
void ramoops_platform_wdog_release(const char *reason) { (void)reason; }
void ramoops_platform_wdog_flushed(void) { wdog_flushed++; if (wdog_flushed_hook) wdog_flushed_hook(); }

int ufs_log_platform_arg(const char *name, uint32_t *value) { return ramoops_platform_arg(name, value, sizeof(*value)); }
uint64_t ufs_log_platform_counter(void) { return ramoops_platform_counter(); }
uint64_t ufs_log_platform_frequency(void) { return HZ; }
void *ufs_log_platform_alloc(uint32_t bytes)
{
    __atomic_fetch_add(&allocations, 1, __ATOMIC_RELAXED);
    if (alloc_fail) return NULL;
    void *p = NULL;
    assert(!posix_memalign(&p, 4096, bytes));
    memset(p, 0xee, bytes);               // the writer must not rely on zeroed memory
    return p;
}
int ufs_log_platform_start(void) { thread_starts++; return !thread_start_fails; }
int ufs_log_platform_wdog_armed(void) { return wdog_armed; }
void ufs_log_platform_stop_pets(void) { __atomic_fetch_add(&pets_stopped, 1, __ATOMIC_RELAXED); }
static int power_held;                      // the Power key, as PaganiInput publishes it
int ufs_log_platform_power_held(void) { return __atomic_load_n(&power_held, __ATOMIC_RELAXED); }

static char msg_ring[0x8000];
static struct ufs_log_msgbuf test_msgbuf = {0x63061, sizeof(msg_ring), 0, 0, msg_ring};
struct ufs_log_msgbuf *msgbufp = &test_msgbuf;

// The window's address on the phone: the unixnu partition of the 512 GB PKX110 (board.toml [userdata], UniXNU/out/dual_sys/applied2/plan.json:
// 107409152..124186367), its last 8192 blocks.
enum { FIRST_LBA = 124178176 };

// A record at `at` of a buffer with its CRC checked: its sequence number, 0 when there is none.
__attribute__((unused)) static uint32_t record_at(const uint8_t *at, unsigned *length)
{
    uint32_t seq = 0, crc = 0;
    unsigned n = 0;
    if (at[0] != '#' || at[9] != ' ' || at[18] != ' ' || at[21] != ' ' || at[30] != ' ' || at[32] != ' ') return 0;
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t c = at[1 + i], d = at[22 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) || !((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f'))) return 0;
        seq = seq << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
        crc = crc << 4 | (uint32_t)(d <= '9' ? d - '0' : d - 'a' + 10);
    }
    for (unsigned i = 19; i < 21; i++) {
        const uint8_t c = at[i];
        n = n << 4 | (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    if (n > RAMOOPS_LOG_TEXT_MAX) return 0;
    uint32_t computed = crc_update(0xffffffffu, at, 22);
    computed = ~crc_update(computed, at + 30, 3 + n);
    if (computed != crc) return 0;
    *length = RAMOOPS_LOG_HEADER_BYTES + n + 1;
    return seq;
}
