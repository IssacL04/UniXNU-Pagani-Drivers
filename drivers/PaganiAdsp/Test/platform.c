// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the PAS / AOSS QMP / RPMh adapters (AdspPlatform.c) over the Adreno830 SM8750 platform layer itself (Sm8750Scm.c,
// Sm8750AossQmp.c, Sm8750Rpmh.c, built from that checkout): the exact SMC function ids and arguments of the ADSP sequence, the
// INTERRUPTED and EBUSY handling, the shared SCM and QMP lock words, the AOP handshake and message, the cmd-db lookup of lcx/lmx and
// the TCS commands of the proxy votes. Time is simulated: every sleep and delay advances one clock.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AdspPlatform.h"
#include "../AdspCore.h"

/* The core's name for a secure call out of its budget is the code the platform layer gives it. */
_Static_assert(PaganiAdspErrScmParked == PaganiAdspPlatScm - (int)Sm8750ScmTimeout, "a parked secure call");

extern volatile uint32_t sm8750_rpmh_drv2_owner;   /* Sm8750Rpmh.c */

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static void put16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, v); put16(p + 2, v >> 16);}

/* ---- the clock, and the A830 driver holding a lock word for a while ---- */
static uint64_t now;
static volatile uint32_t *held_word;
static uint64_t release_at;
static uint64_t now_us(void *c) {(void)c; return now;}
static void advance(uint64_t us)
{
    now += us;
    if (held_word && release_at && now >= release_at) {*held_word = 0; held_word = 0;}
}
static void sleep_us(void *c, uint32_t us) {(void)c; advance(us);}

/* ---- the secure world ---- */
typedef struct {uint64_t in[8];} Call;
static Call calls[64];
static unsigned ncalls, interrupt_next, busy_next;
static int64_t result_x0;
static uint64_t result_x1;
static int fake_smc(void *c, uint64_t regs[8])
{
    (void)c;
    CHECK(sm8750_scm_owner == PaganiAdspLockOwner);    /* never without the SCM word */
    CHECK(ncalls < 64);
    memcpy(calls[ncalls++].in, regs, sizeof calls[0].in);
    advance(50);
    if (regs[0] == Sm8750ScmX0Interrupted) CHECK(regs[6] == 0x77);    /* a resume carries the session back */
    if (interrupt_next) {interrupt_next--; regs[0] = Sm8750ScmX0Interrupted; regs[6] = 0x77; return 0;}
    if (busy_next) {busy_next--; regs[0] = (uint64_t)(int64_t)Sm8750ScmX0Busy; return 0;}
    regs[0] = (uint64_t)result_x0;
    regs[1] = result_x1;
    return 0;
}
static uint8_t meta_copy[8192];
static uint64_t meta_bytes;
static unsigned meta_allocs, meta_frees;
static int meta_alloc(void *c, const uint8_t *data, uint64_t bytes, uint64_t *phys, void **handle)
{
    (void)c;
    CHECK(bytes <= sizeof meta_copy);
    memcpy(meta_copy, data, (size_t)bytes);
    meta_bytes = bytes;
    meta_allocs++;
    *phys = 0x8c1234000ull + 0x10000ull * meta_allocs;
    *handle = (void *)(uintptr_t)meta_allocs;
    return 0;
}
static void meta_free(void *c, void *handle) {(void)c; (void)handle; meta_frees++;}

/* ---- the AOSS message RAM and the AOP ---- */
static uint32_t ram[0x400 / 4];
static int aop_dead, aop_taken;
static char aop_text[80];
static uint32_t ram_read(void *c, uint32_t offset) {(void)c; CHECK(offset < 0x400 && !(offset & 3)); return ram[offset / 4];}
static void ram_write(void *c, uint32_t offset, uint32_t value) {(void)c; CHECK(offset < 0x400 && !(offset & 3)); ram[offset / 4] = value;}
static void ram_delay(void *c, unsigned us) {(void)c; advance(us);}
static unsigned kicks;
static void ipcc_write(void *c, uint32_t offset, uint32_t value)
{
    (void)c;
    CHECK(offset == 0x0c && value == 0);   /* SEND_ID: client 0 (AOP), signal 0 */
    CHECK(sm8750_aoss_qmp_owner == PaganiAdspLockOwner);
    kicks++;
    if (aop_dead) return;
    if (ram[0x24 / 4] == 0xffff) ram[0x28 / 4] = 0xffff;               /* our link acked */
    if (ram[0x2c / 4] == 0xffff) ram[0x14 / 4] = 0xffff;               /* the AOP's channel up */
    if (ram[0x18 / 4] == 0xffff) ram[0x30 / 4] = 0xffff;               /* our channel acked */
    const uint32_t box = ram[0x38 / 4];
    if (ram[box / 4] == 64) {                                          /* a message: take it */
        memcpy(aop_text, &ram[box / 4 + 1], 64);
        aop_text[64] = 0;
        ram[box / 4] = 0;
        aop_taken++;
    }
}
static uint32_t ipcc_read(void *c, uint32_t offset) {(void)c; (void)offset; return 0;}
static const Sm8750AossQmpIo ram_io = {0, ram_read, ram_write, ram_delay}, ipcc_io = {0, ipcc_read, ipcc_write, ram_delay};

/* ---- cmd-db and the DRV-2 TCSes (RSC v3 layout) ---- */
static Sm8750CmdDb db;
static uint8_t cmd_db[Sm8750CmdDbBytes];
static uint32_t db_read(void *c, uint32_t offset) {(void)c; uint32_t v; memcpy(&v, cmd_db + offset, 4); return v;}
static void db_write(void *c, uint32_t offset, uint32_t value) {(void)c; (void)offset; (void)value; CHECK(0);}
static uint16_t lcx_levels[9] = {0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1a0};
static void make_cmd_db(int with_lmx)
{
    memset(cmd_db, 0, sizeof cmd_db);
    put32(cmd_db, 1);
    put32(cmd_db + 4, Sm8750CmdDbMagic);
    /* slave 3 (ARC): entries at data offset 0, aux at 0x200 */
    const char *names[3] = {"cx.lvl", "lcx.lvl", "lmx.lvl"};
    const unsigned count = with_lmx ? 3 : 2;
    put16(cmd_db + 8, Sm8750CmdDbSlaveArc); put16(cmd_db + 10, 0); put16(cmd_db + 12, 0x200); put16(cmd_db + 14, count);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *e = cmd_db + Sm8750CmdDbDataOffset + Sm8750CmdDbEntryBytes * i;
        memcpy(e, names[i], strlen(names[i]));
        put32(e + 16, 0x30000 + 0x10 * i);
        put16(e + 20, 32);
        put16(e + 22, (uint16_t)(32 * i));
        for (unsigned l = 0; l < 9; ++l) put16(cmd_db + Sm8750CmdDbDataOffset + 0x200 + 32 * i + 2 * l, lcx_levels[l]);
    }
}
static const Sm8750RpmhIo db_io = {0, db_read, db_write, ram_delay};

enum {kWait = 0x20, kControl = 0x24, kStatus = 0x28, kEnable = 0x2c, kMsg = 0x34, kStride = 24};
static uint32_t rsc[0x10000 / 4];
static struct {uint32_t addr[4], data[4], n;} sent[16];
static unsigned nsent;
static uint32_t tcs(unsigned slot) {return Sm8750RpmhTcsBase + slot * Sm8750RpmhTcsStride;}
static uint32_t rsc_read(void *c, uint32_t reg) {(void)c; CHECK(reg < 0x10000 && !(reg & 3)); return rsc[reg / 4];}
static void rsc_write(void *c, uint32_t reg, uint32_t value)
{
    (void)c;
    CHECK(reg < 0x10000 && !(reg & 3) && Sm8750RpmhDrv2Owner() == Sm8750RpmhLockOwnerGpu);
    if (reg == Sm8750RpmhTcsBase + 8) {rsc[(Sm8750RpmhTcsBase + 4) / 4] &= ~value; return;}
    rsc[reg / 4] = value;
    for (unsigned s = 0; s < 3; ++s) {
        const uint32_t b = tcs(s);
        if (reg != b + kControl || !(value & Sm8750RpmhAmcTrigger)) continue;
        CHECK(nsent < 16);
        const uint32_t enable = rsc[(b + kEnable) / 4];
        unsigned n = 0;
        for (; n < 4 && (enable >> n) & 1; ++n) {
            sent[nsent].addr[n] = rsc[(b + kMsg + 4 + n * kStride) / 4];
            sent[nsent].data[n] = rsc[(b + kMsg + 8 + n * kStride) / 4];
            rsc[(b + kMsg + 12 + n * kStride) / 4] = 0x10100;            /* issued and completed */
        }
        sent[nsent++].n = n;
        rsc[(b + kStatus) / 4] = 1;
        rsc[(Sm8750RpmhTcsBase + 4) / 4] |= 1u << s;
    }
}
static const Sm8750RpmhIo rsc_io = {0, rsc_read, rsc_write, ram_delay};
static void make_rsc(void)
{
    memset(rsc, 0, sizeof rsc);
    rsc[0] = 3u << 16;
    rsc[0x0c / 4] = 2u | 3u << 6 | 8u << 12 | 16u << 27;
    for (unsigned s = 0; s < 8; ++s) rsc[(tcs(s) + kStatus) / 4] = 1;
}

static PaganiAdspPlatform p;
static PaganiAdspPlatformConfig config;
static void setup(uint32_t rail_level)
{
    config = (PaganiAdspPlatformConfig){0, now_us, sleep_us, fake_smc, meta_alloc, meta_free, &ram_io, &ipcc_io, &rsc_io, &db_io, &db,
                                        rail_level};
    pagani_adsp_platform_init(&p, &config);
    ncalls = 0;
}

static void test_pas(void)
{
    setup(PaganiAdspRailLevel);
    CHECK(p.scm.budget_us == PaganiAdspScmBudgetUs);
    uint8_t meta[300];
    for (unsigned i = 0; i < sizeof meta; ++i) meta[i] = (uint8_t)(i * 7);
    /* init_image: SMC64 SiP PIL 1, args (val pas, rw phys), x0 0 and x1 0 */
    result_x0 = 0; result_x1 = 0;
    CHECK(pagani_adsp_platform_pas_init(&p, 0x24, meta, sizeof meta) == 0);
    CHECK(ncalls == 1 && calls[0].in[0] == 0x42000201u && calls[0].in[1] == 0x82u && calls[0].in[2] == 0x24 &&
          calls[0].in[3] == 0x8c1244000ull);
    CHECK(meta_bytes == sizeof meta && !memcmp(meta_copy, meta, sizeof meta) && sm8750_scm_owner == 0);
    /* mem_setup: PIL 2, three values */
    CHECK(pagani_adsp_platform_pas_mem_setup(&p, 0x24, 0x9ed80000, 0x8000) == 0);
    CHECK(calls[1].in[0] == 0x42000202u && calls[1].in[1] == 3 && calls[1].in[2] == 0x24 && calls[1].in[3] == 0x9ed80000 &&
          calls[1].in[4] == 0x8000);
    /* auth_and_reset, interrupted twice and resumed with the session in x6; then EBUSY once (a 30 ms pause, then again) */
    interrupt_next = 2;
    CHECK(pagani_adsp_platform_pas_auth(&p, 1) == 0);
    CHECK(ncalls == 5 && calls[2].in[0] == 0x42000205u && calls[2].in[1] == 1 && calls[2].in[2] == 1);
    CHECK(calls[3].in[0] == 1 && calls[4].in[0] == 1 && p.last.interrupted == 2);
    busy_next = 1;
    const uint64_t before = now;
    CHECK(pagani_adsp_platform_pas_auth(&p, 1) == 0 && p.last.busy == 1 && now - before >= 30000);
    /* A result word that is not 0 fails a PAS call (Linux "ret ? : res.result[0]") */
    result_x1 = 5;
    CHECK(pagani_adsp_platform_pas_auth(&p, 1) == PaganiAdspPlatScm - Sm8750ScmResultError && p.last.x1 == 5);
    result_x1 = 0;
    result_x0 = -4;
    CHECK(pagani_adsp_platform_pas_mem_setup(&p, 1, 0x9ee00000, 0x10000) == PaganiAdspPlatScm - Sm8750ScmError && p.last.x0 == -4);
    /* the platform layer refuses an unaligned carveout without a call */
    const unsigned issued = ncalls;
    CHECK(pagani_adsp_platform_pas_mem_setup(&p, 1, 0x9ee00800, 0x10000) == PaganiAdspPlatScm - Sm8750ScmArgument && ncalls == issued);
    result_x0 = 0;
    /* shutdown: PIL 6, one value; x1 checked as well */
    CHECK(pagani_adsp_platform_pas_shutdown(&p, 1) == 0 && calls[ncalls - 1].in[0] == 0x42000206u && calls[ncalls - 1].in[1] == 1 &&
          calls[ncalls - 1].in[2] == 1);
    result_x1 = 1;
    CHECK(pagani_adsp_platform_pas_shutdown(&p, 0x24) == PaganiAdspPlatScm - Sm8750ScmResultError);
    result_x1 = 0;
    /* metadata memory: kept after a failed image, returned after an authenticated one */
    CHECK(pagani_adsp_platform_pas_init(&p, 1, meta, 100) == 0 && meta_allocs == 2);
    CHECK(pagani_adsp_platform_pas_init(&p, 5, meta, 100) == PaganiAdspPlatArgument);   /* two slots */
    pagani_adsp_platform_pas_release(&p, 1, 0);
    CHECK(meta_frees == 0);
    pagani_adsp_platform_pas_release(&p, 0x24, 1);
    pagani_adsp_platform_pas_release(&p, 1, 1);
    CHECK(meta_frees == 2 && !p.meta[0].handle && !p.meta[1].handle);
    /* The GPU driver holds the SCM word: we wait (sleeping), and call once it is free */
    sm8750_scm_owner = 0x41383330;   /* 'A830' */
    held_word = &sm8750_scm_owner;
    release_at = now + 5000;
    const unsigned calls_before = ncalls;
    CHECK(pagani_adsp_platform_pas_auth(&p, 1) == 0 && ncalls == calls_before + 1 && p.scm_waits > 0 && sm8750_scm_owner == 0);
    /* ... and never call when it stays held past the deadline */
    sm8750_scm_owner = 0x41383330;
    held_word = 0;
    CHECK(pagani_adsp_platform_pas_auth(&p, 1) == PaganiAdspPlatScm - Sm8750ScmIoError && ncalls == calls_before + 1);
    CHECK(p.scm_lock_failures >= 1 && sm8750_scm_owner == 0x41383330);
    sm8750_scm_owner = 0;
}

static void test_qmp(void)
{
    setup(PaganiAdspRailLevel);
    memset(ram, 0, sizeof ram);
    ram[0] = Sm8750AossQmpMagic; ram[1] = 1; ram[0x0c / 4] = 0xffff; ram[0x34 / 4] = 0x100; ram[0x38 / 4] = 0x100;
    const char on[] = "{class: image, res: load_state, name: adsp, val: on}";
    CHECK(pagani_adsp_platform_qmp(&p, on) == 0 && aop_taken == 1 && !strcmp(aop_text, on) && sm8750_aoss_qmp_owner == 0);
    CHECK(p.qmp.open && !p.qmp.reused && ram[0x10 / 4] == 0xffff);
    /* Opened already (by the GPU driver): reused without a handshake */
    setup(PaganiAdspRailLevel);
    const unsigned k = kicks;
    CHECK(pagani_adsp_platform_qmp(&p, "{class: image, res: load_state, name: adsp, val: off}") == 0 && p.qmp.reused && kicks == k + 1);
    /* An AOP that does not take the message: timeout after 1 s, the message withdrawn */
    aop_dead = 1;
    const uint64_t before = now;
    CHECK(pagani_adsp_platform_qmp(&p, on) == Sm8750AossQmpTimeout && ram[0x100 / 4] == 0 && now - before >= 1000000);
    aop_dead = 0;
    /* The QMP word held past its deadline: nothing written */
    sm8750_aoss_qmp_owner = 0x41383330;
    const unsigned k2 = kicks;
    CHECK(pagani_adsp_platform_qmp(&p, on) == PaganiAdspPlatLock && kicks == k2 && p.qmp_waits > 0);
    sm8750_aoss_qmp_owner = 0;
    /* A bad descriptor is refused before anything is written */
    ram[0] = 0;
    setup(PaganiAdspRailLevel);
    CHECK(pagani_adsp_platform_qmp(&p, on) == Sm8750AossQmpRefused);
}

static void test_rails(void)
{
    make_cmd_db(1);
    make_rsc();
    setup(PaganiAdspRailLevel);
    nsent = 0;
    /* vlvl 384 is index 7 of the table: lmx first when raising */
    CHECK(pagani_adsp_platform_rails(&p, 1) == 0 && p.lcx_hlvl == 7 && p.lmx_hlvl == 7);
    CHECK(nsent == 1 && sent[0].n == 2 && sent[0].addr[0] == 0x30020 && sent[0].data[0] == 7 && sent[0].addr[1] == 0x30010 &&
          sent[0].data[1] == 7);
    /* dropped: lcx first, level 0 */
    CHECK(pagani_adsp_platform_rails(&p, 0) == 0 && nsent == 2 && sent[1].addr[0] == 0x30010 && sent[1].data[0] == 0 &&
          sent[1].addr[1] == 0x30020 && sent[1].data[1] == 0);
    CHECK(Sm8750RpmhDrv2Owner() == 0);
    /* rail level 0: the top of the table (index 8); a level above every entry also gives the top */
    setup(0);
    CHECK(pagani_adsp_platform_rails(&p, 1) == 0 && p.lcx_hlvl == 8);
    setup(0x1ff);
    CHECK(pagani_adsp_platform_rails(&p, 1) == 0 && p.lmx_hlvl == 8);
    /* no lmx.lvl in cmd-db: refused before any TCS write */
    make_cmd_db(0);
    setup(PaganiAdspRailLevel);
    const unsigned n = nsent;
    CHECK(pagani_adsp_platform_rails(&p, 1) == PaganiAdspPlatRails + Sm8750RpmhNotFound && nsent == n);
    /* the DRV-2 word held by the touch driver past 2 s: busy, nothing sent */
    make_cmd_db(1);
    setup(PaganiAdspRailLevel);
    sm8750_rpmh_drv2_owner = 0x54554348;
    CHECK(pagani_adsp_platform_rails(&p, 1) == Sm8750RpmhBusy && nsent == n);
    sm8750_rpmh_drv2_owner = 0;
}

int main(void)
{
    test_pas();
    test_qmp();
    test_rails();
    printf("PASS platform adapters over the Adreno830 SM8750 layer (PAS, QMP, RPMh): %u checks\n", checks);
    return 0;
}
