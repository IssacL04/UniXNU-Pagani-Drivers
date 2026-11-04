/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host test of WifiRddm.c: a synthetic RAM dump behind a BHIe receive vector table, in a model of the DMA window. Also writes the export
 * blob to the file named on the command line, for check-pagani-wifi-host.py to unpack with the reader of the UFS log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiRddm.h"
#include "../WifiUtil.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

#define BUS 0xc0000000ull
#define WINDOW (8u << 20)
#define BHIE 0x124u
static uint8_t *win;
static uint32_t regs[0x400];
static uint32_t bhie_off = BHIE, upload_after;  /* status reads before the upload is done */
static unsigned sleeps, slept_ms, reads_past_window;
static char lines[1024][112];
static unsigned line_count;

static uint32_t rd(void *c, uint32_t reg)
{
    (void)c;
    if (reg == PW_RDDM_BHIEOFF) return bhie_off;
    if (reg >= 0x1000 || (reg & 3)) {++reads_past_window; return 0xffffffffu;}
    if (reg == BHIE + PW_RDDM_RXVECSTATUS && upload_after) {--upload_after; return 1u << PW_RDDM_STATUS_SHIFT;}
    return regs[reg / 4];
}
static const uint8_t *va(void *c, uint64_t bus, uint64_t bytes)
{
    (void)c;
    if (bus < BUS || bus + bytes < bus || bus + bytes > BUS + WINDOW) return NULL;
    return win + (bus - BUS);
}
static void nap(void *c, unsigned ms) {(void)c; ++sleeps; slept_ms += ms;}
static void note(void *c, const char *line)
{
    (void)c;
    CHECK(strlen(line) < 112);
    if (line_count < 1024) snprintf(lines[line_count++], 112, "%s", line);
}
static int logged(const char *text)
{
    for (unsigned i = 0; i < line_count; ++i) if (strstr(lines[i], text)) return 1;
    return 0;
}
static const PwRddmIo io = {NULL, rd, va, nap, note};

static void put64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void entry(unsigned e, uint64_t bus, uint64_t size) {put64(win + 0x1000 + e * 16, bus); put64(win + 0x1000 + e * 16 + 8, size);}
static void table(uint32_t entries)
{
    memset(regs, 0, sizeof regs);
    regs[(BHIE + PW_RDDM_RXVECADDR_LOW) / 4] = (uint32_t)(BUS + 0x1000);
    regs[(BHIE + PW_RDDM_RXVECADDR_HIGH) / 4] = (uint32_t)((BUS + 0x1000) >> 32);
    regs[(BHIE + PW_RDDM_RXVECSIZE) / 4] = entries * 16;
    regs[(BHIE + PW_RDDM_RXVECSTATUS) / 4] = 2u << PW_RDDM_STATUS_SHIFT;
}
static void start(void) {line_count = 0; sleeps = 0; slept_ms = 0;}

int main(int argc, char **argv)
{
    win = calloc(1, WINDOW);
    CHECK(win);
    /* three segments: 0 the crash record, 1 the runtime log, 2 holds an assertion twice and a format string */
    entry(0, BUS + 0x10000, 0x10000);
    entry(1, BUS + 0x100000, 0x80000);
    entry(2, BUS + 0x200000, 0x80000);
    uint8_t *crash = win + 0x10000, *runtime = win + 0x100000, *third = win + 0x200000;
    memcpy(crash + 0x40, "wlan_mac.c:1234", 15);
    memcpy(crash + 0x100, "abcd", 4);                       /* context only: too short for a message */
    memcpy(crash + 0xec10, "fw crash reason", 15);
    for (unsigned i = 0; i < 8; ++i) put64(crash + 8 * i, 0x1122334455667700ull + i);
    memcpy(runtime + 0x78100, "wlan runtime log line", 21);
    memcpy(runtime + 0x78200, "skip this %d format", 19);
    memcpy(runtime + 0x10, "before the log area", 19);
    memcpy(third + 0x100, "ASSERT failed in phy", 20);
    memcpy(third + 0x200, "ASSERT failed in phy", 20);      /* the same text once */
    memcpy(third + 0x300, "printf %d ASSERT", 16);          /* a format string of the image */
    memcpy(third + 0x400, "err_fatal handler", 17);
    table(3);

    /* the whole scan, after a wait for the upload */
    start();
    upload_after = 7;
    PwRddmResult r;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmOk);
    CHECK(r.waited_ms == 70 && r.table == win + 0x1000 && r.table_bytes == 48);
    CHECK(r.scanned == 0x10000u + 0x80000u + 0x80000u);
    CHECK(logged("rddm upload: status=0x80000000 ms=70 table=0xc0001000 bytes=48"));
    CHECK(logged("rddm 0+0x40: wlan_mac.c:1234"));
    CHECK(logged("rddm 2+0x100: ASSERT failed in phy"));
    CHECK(!logged("rddm 2+0x200:"));
    CHECK(!logged("printf %d"));
    CHECK(logged("rddm 2+0x400: err_fatal handler"));
    CHECK(r.messages == 3);
    CHECK(logged("rddm scan: bytes=1114112 messages=3"));
    CHECK(logged("rddm-ctx 0+0x100: abcd"));
    CHECK(logged("rddm-ctx 0+0xec10: fw crash reason"));
    CHECK(logged("rddm-ctx 1+0x78100: wlan runtime log line"));
    CHECK(!logged("skip this"));
    CHECK(!logged("before the log area"));
    CHECK(logged("rddm-rec at=0x0 0x55667700 0x11223344 0x55667701 0x11223344"));
    CHECK(logged("rddm-rec at=0x1e0"));
    CHECK(r.record_lines == 16);
    CHECK(r.lines == line_count);

    /* the export: the size matches what is written, and it comes back as the same bytes */
    const uint64_t bytes = pw_rddm_export_bytes(&io, &r);
    CHECK(bytes == 16 + 8 + 4 + 5 + 4 + 24 + 9 + 48 + 3 * (24 + 9) + 0x10000u + 0x80000u + 0x80000u);
    uint8_t *blob = malloc(bytes);
    CHECK(blob && pw_rddm_export_fill(&io, &r, blob, bytes) == bytes);
    CHECK(pw_rddm_export_fill(&io, &r, blob, bytes - 1) == 0);
    CHECK(!memcmp(blob, "SHEXR001", 8) && !memcmp(blob + 16, "M2VCACH1", 8));
    CHECK(!memcmp(blob + 16 + 8 + 4, "/rddm", 5));
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        CHECK(f && fwrite(blob, 1, bytes, f) == bytes);
        fclose(f);
    }
    free(blob);

    /* a table with a segment outside the window: that one is skipped, the rest read */
    entry(1, BUS + WINDOW, 0x1000);
    start();
    CHECK(pw_rddm_scan(&io, &r) == PwRddmOk && r.scanned == 0x10000u + 0x80000u);
    CHECK(!logged("rddm-ctx 1+"));
    entry(1, 0xfffffffffffff000ull, 0x2000);                /* a wrap */
    start();
    CHECK(pw_rddm_scan(&io, &r) == PwRddmOk && r.scanned == 0x10000u + 0x80000u);
    entry(1, BUS + 0x100000, 0x80000);

    /* a table of one entry: what lies behind it is no segment, also for the context scan, which asks for segment 1 by number */
    table(1);
    start();
    CHECK(pw_rddm_scan(&io, &r) == PwRddmOk && r.scanned == 0x10000u && logged("rddm-ctx 0+0x100: abcd"));
    CHECK(!logged("rddm-ctx 1+"));
    CHECK(pw_rddm_export_bytes(&io, &r) == 16 + 8 + 4 + 5 + 4 + 24 + 9 + 16 + 24 + 9 + 0x10000u);
    table(3);

    /* a link that is gone: all ones everywhere, no wait, no read outside the window */
    start();
    bhie_off = 0xffffffffu;
    const unsigned stray = reads_past_window;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoBhie && logged("rddm: no BHIe block") && reads_past_window == stray && !r.table && !sleeps);
    bhie_off = 0;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoBhie);
    bhie_off = 0x123;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoBhie);
    bhie_off = BHIE;
    /* the status reads all ones: the top bits say "error", no table is touched */
    start();
    regs[(BHIE + PW_RDDM_RXVECSTATUS) / 4] = 0xffffffffu;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNotDone && !r.table && r.messages == 0);
    /* the upload never ends: 4 s, then nothing is read */
    start();
    regs[(BHIE + PW_RDDM_RXVECSTATUS) / 4] = 1u << PW_RDDM_STATUS_SHIFT;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNotDone && slept_ms == 4000 && r.waited_ms == 4000 && !r.table);
    /* the device reports an error */
    table(3);
    regs[(BHIE + PW_RDDM_RXVECSTATUS) / 4] = 3u << PW_RDDM_STATUS_SHIFT;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNotDone && !r.table);
    /* tables that make no sense */
    table(3);
    regs[(BHIE + PW_RDDM_RXVECSIZE) / 4] = 0;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoTable);
    regs[(BHIE + PW_RDDM_RXVECSIZE) / 4] = 47;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoTable);
    regs[(BHIE + PW_RDDM_RXVECSIZE) / 4] = PW_RDDM_TABLE_MAX + 16;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoTable);
    table(3);
    regs[(BHIE + PW_RDDM_RXVECADDR_HIGH) / 4] = 1;
    CHECK(pw_rddm_scan(&io, &r) == PwRddmNoTable && !r.table && pw_rddm_export_bytes(&io, &r) == 0);

    /* many different messages: at most PW_RDDM_MESSAGES, and the pauses for the log's ring come */
    table(3);
    memset(third, 0, 0x80000);
    for (unsigned i = 0; i < 100; ++i) {
        char text[32];
        const int n = snprintf(text, sizeof text, "ASSERT number %03u", i);
        memcpy(third + 0x1000 + i * 64, text, (size_t)n);
    }
    start();
    CHECK(pw_rddm_scan(&io, &r) == PwRddmOk && r.messages == PW_RDDM_MESSAGES);
    CHECK(sleeps == r.lines / PW_RDDM_PACE_LINES && slept_ms == sleeps * PW_RDDM_PACE_MS);
    /* an export larger than its limit is refused: twelve segments of 8 MiB */
    entry(0, BUS, WINDOW);
    entry(1, BUS, WINDOW);
    entry(2, BUS, WINDOW);
    for (uint32_t e = 3; e < 12; ++e) entry(e, BUS, WINDOW);
    PwRddmResult big = r;
    big.table_bytes = 12 * 16;
    CHECK(pw_rddm_export_bytes(&io, &big) == 0);
    free(win);
    printf("PASS RDDM reader: %u checks\n", checks);
    return 0;
}
