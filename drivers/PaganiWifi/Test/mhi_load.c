// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of WifiMhiLoad.c (stage 5: the firmware into the endpoint by PaganiWifi's own MHI boot) against a model of the chip's boot
// ROM and boot loader over a DMA window at a bus address above 4 GiB (as the 13T's). The model reads what the device would read
// through its bus addresses: the boot loader buffer, the BHIe vector table and every segment it names, the MHI context arrays, and
// compares them with the firmware file; it checks the context registers are programmed before the loader is offered and again after
// the loader reset them, the address ranges (0 .. 2^36 - 1), and the event ring acknowledgements. Cases: peach's file (boot loader +
// second ELF at 512 KiB), WCN7850's (one image), an image too large, a window too small, no READY, a crash into RDDM, and the
// endpoint's PCIe core restarting while the loader comes up (BAR0 all ones until the board puts it back).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiMhiLoad.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {BHI = 0x200, BHIE = 0x300, ERDB = 0x700, SPACE = 0x1000};
enum {kCcabap = 0x58, kEcabap = 0x60, kCrcbap = 0x68, kCtrlBase = 0x80, kCtrlLimit = 0x88, kDataBase = 0x98, kDataLimit = 0xa0};
#define BUS 0x8c1200000ull           /* bus = physical on the 13T (XNU's DRAM starts at 0x8c0000000) */

static PwDma dma;
static uint8_t *window;
static uint32_t window_bytes;
static const uint8_t *file;
static uint32_t file_bytes;

typedef struct {
    uint32_t reg[SPACE / 4];
    uint32_t ee, state, ready, reset, syserr;
    int action, countdown, never_ready, rddm;
    int bar_lost;                    /* BAR0 reads all ones and takes no write until link_check puts it back */
    uint32_t link_calls, link_put_back;
    int contexts_seen_before_bhi, contexts_cleared, contexts_again;
    uint32_t sbl_size, bhie_entries;
    uint64_t now;
    int acks_checked;
} Dev;
static Dev d;

static uint8_t *at_bus(uint64_t bus, uint64_t n)
{
    if (bus < BUS || bus + n > BUS + window_bytes) return NULL;
    return window + (bus - BUS);
}
static uint64_t reg64(uint32_t low) {return (uint64_t)d.reg[low / 4] | (uint64_t)d.reg[low / 4 + 1] << 32;}
static int contexts_ok(void)
{
    const uint64_t cc = reg64(kCcabap), ec = reg64(kEcabap), cr = reg64(kCrcbap);
    return at_bus(cc, 128 * 44) && at_bus(ec, 2 * 44) && at_bus(cr, 44) && reg64(kCtrlBase) == 0 && reg64(kDataBase) == 0 &&
           reg64(kCtrlLimit) == PW_MHI_DMA_LIMIT && reg64(kDataLimit) == PW_MHI_DMA_LIMIT && ((d.reg[0x10 / 4] >> 16) & 0xff) == 2;
}

static uint32_t rd(void *ctx, uint32_t r)
{
    (void)ctx;
    CHECK(r < SPACE && !(r & 3));
    if (d.bar_lost) return 0xffffffffu;
    if (r == MHI_STATUS) return d.state << 8 | (d.syserr ? MHI_STATUS_SYSERR : 0) | (d.ready ? MHI_STATUS_READY : 0);
    if (r == MHI_CTRL) return d.reset ? MHI_CTRL_RESET : 0;
    if (r == BHI + MHI_BHI_EXECENV) return d.ee;
    return d.reg[r / 4];
}
static void wr(void *ctx, uint32_t r, uint32_t v)
{
    (void)ctx;
    CHECK(r < SPACE && !(r & 3));
    if (d.bar_lost) return;
    d.reg[r / 4] = v;
    if (r == MHI_CTRL) {
        if (((v >> 8) & 0xff) == MhiStateM0) {
            CHECK(d.ready);
            CHECK(contexts_ok());            /* given again after the loader cleared them */
            if (d.contexts_cleared) d.contexts_again = 1;
            d.state = MhiStateM0;
            d.action = 4;
            d.countdown = 2;
        }
    } else if (r == BHI + MHI_BHI_IMGTXDB) {
        CHECK(d.ee == MhiEePbl);
        d.contexts_seen_before_bhi = contexts_ok();
        const uint64_t a = (uint64_t)d.reg[(BHI + MHI_BHI_IMGADDR_HIGH) / 4] << 32 | d.reg[(BHI + MHI_BHI_IMGADDR_LOW) / 4];
        d.sbl_size = d.reg[(BHI + MHI_BHI_IMGSIZE) / 4];
        const uint8_t *sbl = at_bus(a, d.sbl_size);
        CHECK(sbl && !(a & (0x80000 - 1)));                        /* aligned to its size */
        CHECK(!memcmp(sbl, file, d.sbl_size));
        d.action = 2;
        d.countdown = 2;
    } else if (r == BHIE + MHI_BHIE_TXVECDB) {
        CHECK(d.ee == MhiEeSbl);
        const uint64_t t = (uint64_t)d.reg[(BHIE + MHI_BHIE_TXVECADDR_HIGH) / 4] << 32 | d.reg[(BHIE + MHI_BHIE_TXVECADDR_LOW) / 4];
        const uint32_t bytes = d.reg[(BHIE + MHI_BHIE_TXVECSIZE) / 4];
        const MhiBhieEntry *e = (const MhiBhieEntry *)at_bus(t, bytes);
        CHECK(e && bytes % sizeof(MhiBhieEntry) == 0);
        d.bhie_entries = bytes / sizeof(MhiBhieEntry);
        /* the vector table describes exactly the BHIe image: the second ELF when there is one, else the whole file */
        uint32_t start = 0;
        if (file_bytes > 0x80004 && !memcmp(file + 0x80000, "\177ELF", 4)) start = 0x80000;
        uint64_t done = 0;
        for (uint32_t i = 0; i < d.bhie_entries; ++i) {
            const uint8_t *seg = at_bus(e[i].address, e[i].size);
            CHECK(seg && e[i].size && !(e[i].address & (0x80000 - 1)));
            CHECK(!memcmp(seg, file + start + done, e[i].size));
            done += e[i].size;
        }
        CHECK(done == file_bytes - start);
        d.reg[(BHIE + MHI_BHIE_TXVECSTATUS) / 4] = 0;
        d.action = 5;
        d.countdown = 3;
    }
}
/* the device posts an event on ring 0: its read pointer moves one element on */
static void post_event(void)
{
    const uint64_t ec = reg64(kEcabap);
    uint8_t *c = at_bus(ec, 44);
    uint64_t base, len, rp;
    memcpy(&base, c + 12, 8); memcpy(&len, c + 20, 8); memcpy(&rp, c + 28, 8);
    rp = rp + 16 >= base + len ? base : rp + 16;
    memcpy(c + 28, &rp, 8);
}
static void tick(void)
{
    d.now++;
    if (!d.action || --d.countdown > 0) return;
    const int action = d.action;
    d.action = 0;
    switch (action) {
    case 2:
        d.reg[(BHI + MHI_BHI_STATUS) / 4] = 2u << 30;
        /* the loader starts: it resets the MHI registers the contexts were given in */
        d.reg[kCcabap / 4] = d.reg[kCcabap / 4 + 1] = d.reg[kEcabap / 4] = d.reg[kEcabap / 4 + 1] = 0;
        d.contexts_cleared = 1;
        if (!d.never_ready) {d.action = 3; d.countdown = 2;}
        break;
    case 3:
        d.ready = 1; d.state = MhiStateReady;
        if (d.link_put_back == 1) d.bar_lost = 1;     /* case: the core restarts as the loader starts */
        break;
    case 4: d.ee = MhiEeSbl; post_event(); break;
    case 5:
        if (d.rddm) {d.ee = MhiEeRddm; break;}
        d.reg[(BHIE + MHI_BHIE_TXVECSTATUS) / 4] = (d.reg[(BHIE + MHI_BHIE_TXVECDB) / 4] & MHI_SEQ_MASK) | 2u << 30;
        d.action = 6; d.countdown = 2;
        break;
    case 6: d.ee = MhiEeAmss; break;
    }
}
static void msleep(void *ctx, unsigned ms) {(void)ctx; for (unsigned i = 0; i < ms; ++i) tick();}
static uint64_t now(void *ctx) {(void)ctx; return d.now;}
static unsigned cleaned;
static void clean(void *ctx, const void *va, size_t n) {(void)ctx; (void)va; (void)n; cleaned++;}
static void say(void *ctx, const char *line) {(void)ctx; if (getenv("PW_VERBOSE")) printf("%s\n", line);}
/* The board's link check: after two calls configuration space answers again and BAR0 is put back. */
static unsigned link_check(void *ctx)
{
    (void)ctx;
    d.link_calls++;
    if (d.link_calls < 2) return 0;
    d.bar_lost = 0;
    d.link_put_back = 2;
    return 1;
}
static const PwMhiIo io = {NULL, rd, wr, msleep, now, clean, say, link_check};
static const PwMhiIo io_no_check = {NULL, rd, wr, msleep, now, clean, say, NULL};

static void reset_dev(void)
{
    memset(&d, 0, sizeof d);
    d.reg[MHI_REGLEN / 4] = SPACE;
    d.reg[MHI_BHIOFF / 4] = BHI;
    d.reg[MHI_BHIEOFF / 4] = BHIE;
    d.reg[0x20 / 4] = ERDB;
    d.ee = MhiEePbl;
    d.state = MhiStateReset;
}
static uint8_t *make_file(uint32_t bytes, int split, unsigned seed)
{
    uint8_t *f = malloc(bytes);
    CHECK(f);
    srand(seed);
    for (uint32_t i = 0; i < bytes; ++i) f[i] = (uint8_t)rand();
    memcpy(f, "\177ELF", 4);
    if (split) memcpy(f + 0x80000, "\177ELF", 4);
    else if (bytes > 0x80004) f[0x80000] = 0;
    return f;
}
static int run(uint32_t bytes, int split, uint32_t window_mib, PwMhiLoad *l, uint32_t phase_ms)
{
    reset_dev();
    window_bytes = window_mib << 20;
    CHECK(pw_dma_init(&dma, window, BUS, window_bytes) == 0);
    uint8_t *f = make_file(bytes, split, bytes);
    file = f;
    file_bytes = bytes;
    PwMhiConfig c = {f, bytes, SPACE, 0, phase_ms, 5, 0x1234};
    const int rc = pw_mhi_load(l, &dma, &io, &c);
    free(f);
    return rc;
}

int main(void)
{
    window = aligned_alloc(1 << 21, 64u << 20);
    CHECK(window);
    static PwMhiLoad l;

    /* peach: amss20.bin = boot loader (512 KiB) + a second ELF (here 1.3 MiB): BHI takes the first, BHIe only the second. */
    CHECK(run(0x80000 + 1300 * 1024, 1, 48, &l, 20000) == PwMhiOk);
    CHECK(l.boot.phase == MhiDone && l.layout.split && l.layout.image_offset == 0x80000);
    CHECK(d.sbl_size == 0x80000 && d.bhie_entries == 3 && l.table_entries == 3 && l.segments == 3);
    CHECK(d.contexts_seen_before_bhi && d.contexts_cleared && d.contexts_again && l.context_writes == 2);
    CHECK(l.event_acks >= 1);
    {   /* the acknowledgement: the write pointer one element behind the device's read pointer, rung at ERDB + 0 */
        uint64_t rp, wp;
        uint8_t *c = at_bus(reg64(kEcabap), 44);
        memcpy(&rp, c + 28, 8); memcpy(&wp, c + 36, 8);
        CHECK(wp + 16 == rp && reg64(ERDB) == wp);
    }
    CHECK(cleaned >= 5);

    /* WCN7850: one image, BHIe carries the whole file (1.2 MiB: 3 segments, the boot loader again among them). */
    cleaned = 0;
    CHECK(run(1200 * 1024, 0, 48, &l, 20000) == PwMhiOk);
    CHECK(!l.layout.split && l.layout.image_offset == 0 && d.bhie_entries == 3 && d.sbl_size == 0x80000);

    /* An image of more than 64 segments is refused before anything is touched. */
    CHECK(run(0x80000 + 65 * 0x80000, 1, 48, &l, 20000) == PwMhiTooLarge && d.sbl_size == 0);

    /* A window too small for the buffers: nothing is offered to the device. */
    CHECK(run(0x80000 + 4 * 0x80000, 1, 2, &l, 20000) == PwMhiNoDma && d.sbl_size == 0);

    /* The loader never reports READY: the phase deadline ends it. */
    reset_dev();
    window_bytes = 48u << 20;
    CHECK(pw_dma_init(&dma, window, BUS, window_bytes) == 0);
    {
        uint8_t *f = make_file(0x80000 + 0x80000, 1, 7);
        file = f; file_bytes = 0x100000;
        PwMhiConfig c = {f, 0x100000, SPACE, 0, 300, 5, 1};
        d.never_ready = 1;
        CHECK(pw_mhi_load(&l, &dma, &io, &c) == PwMhiTimeout && l.boot.error == MhiErrTimeout && l.boot.failed_phase == MhiWaitReady);
        free(f);
    }

    /* The device falls into its RAM dump state during the image transfer. */
    reset_dev();
    CHECK(pw_dma_init(&dma, window, BUS, window_bytes) == 0);
    {
        uint8_t *f = make_file(0x80000 + 0x80000, 1, 9);
        file = f; file_bytes = 0x100000;
        PwMhiConfig c = {f, 0x100000, SPACE, 0, 20000, 5, 1};
        d.rddm = 1;
        CHECK(pw_mhi_load(&l, &dma, &io, &c) == PwMhiFailed && l.boot.error == MhiErrRddm);
        free(f);
    }

    /* The endpoint's PCIe core restarts as the loader comes up: BAR0 reads all ones, the board puts it back, the boot goes on. */
    reset_dev();
    CHECK(pw_dma_init(&dma, window, BUS, window_bytes) == 0);
    {
        uint8_t *f = make_file(0x80000 + 0x80000, 1, 11);
        file = f; file_bytes = 0x100000;
        PwMhiConfig c = {f, 0x100000, SPACE, 0, 20000, 5, 1};
        d.link_put_back = 1;
        CHECK(pw_mhi_load(&l, &dma, &io, &c) == PwMhiOk && l.link_restores == 1 && d.link_calls == 2 && l.boot.phase == MhiDone);
        /* without a board that can put it back the phase deadline ends it, and nothing is touched meanwhile */
        reset_dev();
        CHECK(pw_dma_init(&dma, window, BUS, window_bytes) == 0);
        PwMhiConfig c2 = {f, 0x100000, SPACE, 0, 300, 5, 1};
        d.link_put_back = 1;
        CHECK(pw_mhi_load(&l, &dma, &io_no_check, &c2) == PwMhiTimeout && l.link_restores == 0);
        free(f);
    }

    /* No image. */
    PwMhiConfig none = {NULL, 0, SPACE, 0, 1, 1, 1};
    CHECK(pw_mhi_load(&l, &dma, &io, &none) == PwMhiNoImage);
    free(window);
    printf("PASS MHI load: %u checks\n", checks);
    return 0;
}
