// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng package/tests/qcom_adsp_host.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). 13T changes: the
// SMEM access guard and table lookups, SMP2P inbound size bounds, GLINK stop and eight channels, an RX_DONE owed while the FIFO is full,
// MDT placement without the file check; the QMP, RPMh and SCM parts moved to Test/platform.c (those are now the Adreno830 platform
// layer's).
// Host tests of the ADSP bring-up cores: firmware image parsing, SMEM partitions and items, SMP2P, GLINK (two endpoints talking over
// shared FIFOs, plus the exact wire bytes). No hardware: every shared memory is a host buffer and every doorbell a counter.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../QcomGlink.h"
#include "../QcomMdt.h"
#include "../QcomSmem.h"
#include "../QcomSmp2p.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16));}

/* ---- firmware images ---- */
typedef struct {const uint8_t *data; size_t size; unsigned reads;} Blob;
static int blob_read(void *context, uint64_t offset, void *to, size_t length)
{
    Blob *b = context;
    b->reads++;
    if (offset > b->size || length > b->size - offset) return -1;
    memcpy(to, b->data + offset, length);
    return 0;
}

/* An ELF32 image: PT_NULL header segment, the hash segment, two relocatable PT_LOAD segments (the second with a zero tail). */
static size_t make_elf32(uint8_t *f, int with_hash, uint32_t flags_extra)
{
    memset(f, 0, 4096);
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 1; f[5] = 1; f[6] = 1;
    put16(f + 16, 2); put16(f + 18, 164);
    put32(f + 28, 52);              /* phoff */
    put16(f + 40, 52); put16(f + 42, 32); put16(f + 44, 4);
    const uint32_t hdr = 52 + 4 * 32;
    struct {uint32_t type, off, paddr, filesz, memsz, flags;} ph[4] = {
        {0, 0, 0, hdr, 0, 7u << 24},
        {0, 0x200, 0x8a000000, 0x40, 0x1000, with_hash ? (2u << 24) : 0},
        {1, 0x400, 0x8a100000, 0x100, 0x100, 0x5 | flags_extra},
        {1, 0x600, 0x8a101000, 0x80, 0x200, 0x6 | flags_extra},
    };
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *p = f + 52 + 32 * i;
        put32(p, ph[i].type); put32(p + 4, ph[i].off); put32(p + 8, ph[i].paddr); put32(p + 12, ph[i].paddr);
        put32(p + 16, ph[i].filesz); put32(p + 20, ph[i].memsz); put32(p + 24, ph[i].flags); put32(p + 28, 0x1000);
    }
    for (unsigned i = 0; i < 0x40; ++i) f[0x200 + i] = (uint8_t)(0xa0 + i);
    for (unsigned i = 0; i < 0x100; ++i) f[0x400 + i] = (uint8_t)i;
    return 0x680;
}

static void test_mdt(void)
{
    static uint8_t f[4096];
    size_t n = make_elf32(f, 1, QCOM_MDT_RELOCATABLE);
    Blob b = {f, n, 0};
    QcomMdtImage image;
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtOk);
    CHECK(!image.elf64 && image.count == 4 && image.hash == 1 && image.relocatable);
    CHECK(image.header_size == 52 + 4 * 32);
    CHECK(image.min_address == 0x8a100000 && image.max_address == 0x8a101200);
    CHECK(qcom_mdt_span(&image) == 0x2000);  /* the end rounded up to 4 KiB */
    CHECK(image.segment[2].load && image.segment[3].load && !image.segment[0].load && !image.segment[1].load);
    uint8_t meta[1024];
    CHECK(qcom_mdt_metadata_size(&image) == image.header_size + 0x40);
    CHECK(qcom_mdt_metadata(&image, blob_read, &b, meta, sizeof meta) == QcomMdtOk);
    CHECK(!memcmp(meta, f, image.header_size) && meta[image.header_size] == 0xa0 && meta[image.header_size + 0x3f] == 0xdf);
    CHECK(qcom_mdt_metadata(&image, blob_read, &b, meta, 10) == QcomMdtTooSmall);
    uint64_t at;
    CHECK(qcom_mdt_place(&image, 2, 0x9ea00000, 0x4680000, &at) == QcomMdtOk && at == 0);
    CHECK(qcom_mdt_place(&image, 3, 0x9ea00000, 0x4680000, &at) == QcomMdtOk && at == 0x1000);
    CHECK(qcom_mdt_place(&image, 3, 0x9ea00000, 0x1100, &at) == QcomMdtOutside);
    CHECK(qcom_mdt_place(&image, 1, 0x9ea00000, 0x4680000, &at) == QcomMdtOutside);
    /* Fixed (not relocatable) images go by address. */
    n = make_elf32(f, 1, 0);
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtOk && !image.relocatable);
    CHECK(qcom_mdt_place(&image, 3, 0x8a100000, 0x2000, &at) == QcomMdtOk && at == 0x1000);
    CHECK(qcom_mdt_place(&image, 2, 0x8a200000, 0x2000, &at) == QcomMdtOutside);
    /* No hash segment, not an ELF, truncated program headers, file shorter than a segment. */
    n = make_elf32(f, 0, QCOM_MDT_RELOCATABLE);
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtNoHash);
    n = make_elf32(f, 1, QCOM_MDT_RELOCATABLE);
    f[1] = 'X';
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtNotElf);
    f[1] = 'E';
    CHECK(qcom_mdt_parse(&image, 100, blob_read, &b) == QcomMdtBadHeaders);
    CHECK(qcom_mdt_parse(&image, 0x500, blob_read, &b) == QcomMdtOk);
    /* 13T: placement checks the carveout only; the segment's bytes are the caller's (.mdt or .bNN) */
    CHECK(qcom_mdt_place(&image, 3, 0, 0x4680000, &at) == QcomMdtOk && at == 0x1000);
    /* The headers alone (a .b00): the hash is neither packed nor inside, so the metadata is the caller's to assemble */
    {
        Blob hb = {f, image.header_size, 0};
        QcomMdtImage h;
        CHECK(qcom_mdt_parse(&h, hb.size, blob_read, &hb) == QcomMdtOk && h.hash == 1);
        CHECK(qcom_mdt_metadata(&h, blob_read, &hb, meta, sizeof meta) == QcomMdtOutside);
    }
    /* p_filesz above p_memsz is refused, as the Linux loader refuses it */
    {
        static uint8_t g[4096];
        memcpy(g, f, sizeof g);
        put32(g + 52 + 3 * 32 + 16, 0x300);   /* segment 3: filesz 0x300 > memsz 0x200 */
        Blob gb = {g, 0x680, 0};
        QcomMdtImage bad;
        CHECK(qcom_mdt_parse(&bad, gb.size, blob_read, &gb) == QcomMdtBadHeaders);
    }
    /* The split form: the hash packed right after the headers, nothing else in the file. */
    static uint8_t split[512];
    memcpy(split, f, image.header_size);
    memcpy(split + image.header_size, f + 0x200, 0x40);
    Blob sb = {split, image.header_size + 0x40, 0};
    QcomMdtImage s;
    CHECK(qcom_mdt_parse(&s, sb.size, blob_read, &sb) == QcomMdtOk);
    CHECK(qcom_mdt_metadata(&s, blob_read, &sb, meta, sizeof meta) == QcomMdtOk && meta[s.header_size] == 0xa0);
    /* ELF64 with one load segment. */
    memset(f, 0, sizeof f);
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 2; f[5] = 1;
    put32(f + 32, 64); put16(f + 54, 56); put16(f + 56, 3);
    const uint32_t hdr = 64 + 3 * 56;
    uint8_t *p = f + 64;
    put32(p, 0); put32(p + 4, 7u << 24); put32(p + 32, hdr);
    p += 56; put32(p, 0); put32(p + 4, 2u << 24); put32(p + 8, 0x300); put32(p + 32, 0x20); put32(p + 40, 0x20);
    p += 56; put32(p, 1); put32(p + 4, 5); put32(p + 8, 0x400); put32(p + 24, 0x1000); put32(p + 32, 0x10); put32(p + 40, 0x10);
    Blob b64 = {f, 0x410, 0};
    CHECK(qcom_mdt_parse(&image, 0x410, blob_read, &b64) == QcomMdtOk && image.elf64 && image.hash == 1);
    CHECK(image.min_address == 0x1000 && image.max_address == 0x1010 && image.segment[2].file_offset == 0x400);
}

/* ---- SMEM ---- */
#define SMEM_BYTES 0x40000u
static uint32_t smem_words[SMEM_BYTES / 4];
static QcomSmem smem = {(volatile uint8_t *)smem_words, SMEM_BYTES, 0, {{0, 0}}, 0};
static void w32(uint32_t at, uint32_t v) {smem_words[at / 4] = v;}

static void make_partition(uint32_t toc_index, uint32_t offset, uint32_t size, uint16_t h0, uint16_t h1, uint32_t cacheline)
{
    const uint32_t e = SMEM_BYTES - 0x1000 + 32 + 48 * toc_index;
    w32(e, offset); w32(e + 4, size); w32(e + 8, 0); w32(e + 12, h0 | (uint32_t)h1 << 16); w32(e + 16, cacheline);
    w32(offset, 0x54525024u); w32(offset + 4, h0 | (uint32_t)h1 << 16); w32(offset + 8, size);
    w32(offset + 12, 32); w32(offset + 16, size);
}

static void test_smem(void)
{
    memset(smem_words, 0, sizeof smem_words);
    QcomSmemPartition p;
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemNoTable);
    const uint32_t toc = SMEM_BYTES - 0x1000;
    w32(toc, 0x434f5424u); w32(toc + 4, 1); w32(toc + 8, 3);
    make_partition(0, 0x1000, 0x8000, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, 128);
    make_partition(1, 0x9000, 0x6000, 1, 0, 64);  /* apps-modem: not ours */
    make_partition(2, 0x10000, 0x10000, 2, 0, 64); /* adsp-apps, hosts listed the other way round */
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemOk && p.offset == 0x10000 && p.size == 0x10000 && p.cacheline == 64);
    CHECK(qcom_smem_partition(&smem, 0, 5, &p) == QcomSmemNoPartition);
    QcomSmemPartition g;
    CHECK(qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &g) == QcomSmemOk && g.offset == 0x1000);
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemOk);
    uint32_t at, size;
    CHECK(qcom_smem_find(&smem, &p, 478, &at, &size) == QcomSmemMissing);
    CHECK(qcom_smem_alloc(&smem, &p, 478, 32) == QcomSmemOk);
    CHECK(qcom_smem_alloc(&smem, &p, 479, 0x4000) == QcomSmemOk);
    CHECK(qcom_smem_alloc(&smem, &p, 429, 341) == QcomSmemOk);  /* rounded to 344, padding 3 */
    CHECK(qcom_smem_alloc(&smem, &p, 478, 32) == QcomSmemExists);
    CHECK(qcom_smem_find(&smem, &p, 478, &at, &size) == QcomSmemOk && at == 0x10000 + 32 + 16 && size == 32);
    CHECK(qcom_smem_find(&smem, &p, 479, &at, &size) == QcomSmemOk && at == 0x10000 + 32 + 48 + 16 && size == 0x4000);
    CHECK(qcom_smem_find(&smem, &p, 429, &at, &size) == QcomSmemOk && size == 341 && (at & 7) == 0);
    CHECK(smem_words[(0x10000 + 12) / 4] == 32 + 48 + 16 + 0x4000 + 16 + 344);
    /* A cached item made by the remote at the top of the partition: entry in the last cache line slot, data below it. */
    const uint32_t top = 0x10000 + 0x10000 - 64;
    w32(top, 0xa5a5u | 480u << 16); w32(top + 4, 0x2000); w32(top + 8, 0);
    w32(0x10000 + 16, 0x10000 - 64 - 0x2000);
    CHECK(qcom_smem_find(&smem, &p, 480, &at, &size) == QcomSmemOk && at == top - 0x2000 && size == 0x2000);
    CHECK(qcom_smem_alloc(&smem, &p, 480, 8) == QcomSmemExists);
    /* No room once the uncached list would run into the cached one. */
    CHECK(qcom_smem_alloc(&smem, &p, 600, 0x10000) == QcomSmemNoSpace);
    /* A broken canary is reported, not walked past. */
    smem_words[(0x10000 + 32) / 4] ^= 1;
    CHECK(qcom_smem_find(&smem, &p, 429, &at, &size) == QcomSmemCorrupt);
    CHECK(qcom_smem_alloc(&smem, &p, 601, 8) == QcomSmemCorrupt);
    smem_words[(0x10000 + 32) / 4] ^= 1;
    /* A partition header that disagrees with the table is refused. */
    w32(0x10000 + 8, 0x1234);
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemBadPartition);
}

/* ---- SMP2P ---- */
static void test_smp2p(void)
{
    static uint32_t ours[QCOM_SMP2P_ITEM_BYTES / 4], theirs[QCOM_SMP2P_ITEM_BYTES / 4];
    QcomSmp2p s, remote;
    qcom_smp2p_init_out(&s, ours, 0, 2);
    CHECK(ours[0] == QCOM_SMP2P_MAGIC && ours[1] == (1u | 1u << 8) && ours[2] == (0u | 2u << 16) && ours[3] == 16);
    const int stop = qcom_smp2p_add_entry(&s, "master-kernel");
    CHECK(stop == 0 && ours[3] == (16u | 1u << 16) && !memcmp(&ours[5], "master-kernel", 13));
    CHECK(qcom_smp2p_update(&s, stop, 1, 1) == 1 && qcom_smp2p_out_value(&s, stop) == 1);
    CHECK(qcom_smp2p_update(&s, stop, 1, 1) == 0);
    CHECK(qcom_smp2p_update(&s, stop, 1, 0) == 1 && qcom_smp2p_out_value(&s, stop) == 0);
    /* The DSP's item, as the DSP writes it (pid 2 local, 0 remote), with its slave-kernel entry. */
    qcom_smp2p_init_out(&remote, theirs, 2, 0);
    const int slave = qcom_smp2p_add_entry(&remote, "slave-kernel");
    CHECK(qcom_smp2p_check_in(theirs, 0, 2) == 0);
    CHECK(qcom_smp2p_check_in(theirs, 0, 3) == -3);
    uint32_t value, changed;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 0);  /* no inbound item yet */
    s.in = theirs;
    CHECK(qcom_smp2p_find_in(&s, "slave-kernel") == 0 && qcom_smp2p_find_in(&s, "sleepstate") == -1);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(s.negotiated && s.ssr_ack_enabled && value == 0 && changed == 0);
    /* Ready and handover (bits 1 and 2) arrive, then fatal (bit 0). */
    qcom_smp2p_update(&remote, slave, 6, 6);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 6 && changed == 6);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 6 && changed == 0);
    qcom_smp2p_update(&remote, slave, 1, 1);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 7 && changed == 1);
    /* A restart: the DSP sets restart done, we answer with restart ack (and ring), then both clear. */
    theirs[4] |= QCOM_SMP2P_FLAG_RESTART_DONE;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 1 && (ours[4] & QCOM_SMP2P_FLAG_RESTART_ACK));
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 0);
    theirs[4] &= ~QCOM_SMP2P_FLAG_RESTART_DONE;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 1 && !(ours[4] & QCOM_SMP2P_FLAG_RESTART_ACK));
    /* A remote without the SSR ack feature: our features drop to the common set. */
    QcomSmp2p t;
    static uint32_t ours2[QCOM_SMP2P_ITEM_BYTES / 4];
    qcom_smp2p_init_out(&t, ours2, 0, 2);
    theirs[1] = 1;
    t.in = theirs;
    CHECK(qcom_smp2p_service(&t, "slave-kernel", &value, &changed) == 1 && ours2[1] == 1 && !t.ssr_ack_enabled);
    for (int i = 0; i < 16; ++i) CHECK(qcom_smp2p_add_entry(&t, "x") == i);
    CHECK(qcom_smp2p_add_entry(&t, "x") == -1);
}

/* ---- GLINK ---- */
#define FIFO 1024u
static uint32_t desc[4];
static uint8_t fifo_a[FIFO], fifo_b[FIFO];  /* a: A's transmit FIFO (item 479), b: B's (item 480) */
static unsigned kicks_a, kicks_b;
static void kick_a(void *c) {(void)c; kicks_a++;}
static void kick_b(void *c) {(void)c; kicks_b++;}

typedef struct {unsigned ups, downs, messages; uint8_t last[256]; uint32_t last_length;} Seen;
static void on_state(void *context, QcomGlinkChannel *c, int up) {(void)c; Seen *s = context; if (up) s->ups++; else s->downs++;}
static void on_receive(void *context, QcomGlinkChannel *c, const uint8_t *data, uint32_t length)
{
    (void)c;
    Seen *s = context;
    s->messages++;
    s->last_length = length;
    memcpy(s->last, data, length < sizeof s->last ? length : sizeof s->last);
}

static void settle(QcomGlink *a, QcomGlink *b)
{
    for (int i = 0; i < 20; ++i) {
        int n = qcom_glink_poll(a);
        CHECK(n >= 0);
        int m = qcom_glink_poll(b);
        CHECK(m >= 0);
        if (!n && !m && i > 2) break;
    }
}

static void test_glink(void)
{
    static QcomGlink a, b;
    static uint8_t store_a[4 * 256], store_b[4 * 256];
    memset(desc, 0xee, sizeof desc);  /* garbage before init */
    QcomGlinkFifo a_tx = {&desc[1], &desc[0], fifo_a, FIFO}, a_rx = {&desc[3], &desc[2], 0, 0};
    QcomGlinkFifo b_tx = {&desc[3], &desc[2], fifo_b, FIFO}, b_rx = {&desc[1], &desc[0], fifo_a, FIFO};
    qcom_glink_init(&a, a_tx, a_rx, kick_a, 0);
    desc[3] = 0; desc[0] = 0;  /* the remote's own indices */
    qcom_glink_init(&b, b_tx, b_rx, kick_b, 0);
    const QcomGlinkClient client = {on_state, on_receive};
    Seen sa = {0}, sb = {0};
    QcomGlinkChannel *ca = qcom_glink_channel(&a, "PMIC_RTR_ADSP_APPS", 256, 4, store_a, &client, &sa, 0);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "PMIC_RTR_ADSP_APPS", 256, 4, store_b, &client, &sb, 1);
    CHECK(ca && cb && ca->lcid == 1);
    /* A's first message is VERSION 1 with the intent reuse feature, exactly these bytes. */
    CHECK(qcom_glink_poll(&a) == 0 && kicks_a == 1);
    static const uint8_t version[8] = {0, 0, 1, 0, 1, 0, 0, 0};
    CHECK(!memcmp(fifo_a, version, 8) && desc[1] == 8);
    /* The remote's FIFO is not there yet: A only gets it once B exists. */
    qcom_glink_set_rx(&a, fifo_b, FIFO);
    settle(&a, &b);
    CHECK(a.version_agreed && b.version_agreed && a.features == 1);
    CHECK(ca->up && cb->up && sa.ups == 1 && sb.ups == 1 && ca->rcid == cb->lcid && cb->rcid == ca->lcid);
    /* Requests and answers, many times over so both FIFOs wrap. */
    for (unsigned i = 0; i < 200; ++i) {
        uint8_t m[40];
        for (unsigned k = 0; k < sizeof m; ++k) m[k] = (uint8_t)(i + k);
        CHECK(qcom_glink_send(&b, cb, m, 12 + i % 29) == QcomGlinkOk);
        settle(&a, &b);
        CHECK(sa.messages == i + 1 && sa.last_length == 12 + i % 29 && !memcmp(sa.last, m, sa.last_length));
        CHECK(qcom_glink_send(&a, ca, m, 3 + i % 7) == QcomGlinkOk);
        settle(&a, &b);
        CHECK(sb.messages == i + 1 && sb.last_length == 3 + i % 7);
    }
    CHECK(a.dropped == 0 && b.dropped == 0 && a.rx_data == 200 && b.rx_data == 200);
    /* Four sends with no answer in between use up B's four intents of A; the fifth waits, asks once, and goes after A is done. */
    for (unsigned i = 0; i < 4; ++i) CHECK(qcom_glink_send(&b, cb, "abcd", 4) == QcomGlinkOk);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkAgain && cb->intent_asked);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkAgain);
    settle(&a, &b);
    CHECK(sa.messages == 204);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkOk);
    settle(&a, &b);
    CHECK(sa.messages == 205 && !memcmp(sa.last, "efgh", 4));
    /* Bigger than every intent: stays pending. */
    uint8_t big[300] = {0};
    CHECK(qcom_glink_send(&b, cb, big, sizeof big) == QcomGlinkAgain);
    settle(&a, &b);
    /* A full intent table (the audio DSP offers many small intents first): the larger intent a send asked for replaces the smallest
     * idle one instead of being dropped. */
    {
        for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k) {cb->remote[k].valid = 1; cb->remote[k].busy = 0; cb->remote[k].size = 128; cb->remote[k].id = 1000 + k;}
        cb->remote[3].size = 64;
        const uint32_t dropped = b.dropped;
        uint8_t m[16] = {QCOM_GLINK_INTENT, 0, (uint8_t)ca->lcid, 0, 1, 0, 0, 0, 0x00, 0x02, 0, 0, 0x77, 0, 0, 0};
        uint32_t head = desc[1];
        for (unsigned i = 0; i < 16; ++i) {fifo_a[head] = m[i]; head = (head + 1) % FIFO;}
        desc[1] = head;
        qcom_glink_poll(&b);
        CHECK(b.dropped == dropped && cb->remote[3].valid && cb->remote[3].size == 512 && cb->remote[3].id == 0x77);
        for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k) cb->remote[k].valid = 0;
    }
    /* The remote closes: both sides go down, A answers with CLOSE_ACK and its own CLOSE, then waits for a new OPEN. */
    QcomGlinkChannel saved = *cb;
    (void)saved;
    {
        uint8_t m[8] = {QCOM_GLINK_CLOSE, 0, (uint8_t)cb->lcid, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
        cb->up = 0;
    }
    qcom_glink_poll(&a);
    CHECK(sa.downs == 1 && !ca->up && !ca->remote_open && ca->open_sent == 2);
    CHECK(qcom_glink_send(&a, ca, "x", 1) == QcomGlinkNotOpen);
    /* An unknown channel name opened by the remote is counted and left alone. */
    {
        uint8_t m[8 + 8] = {QCOM_GLINK_OPEN, 0, 9, 0, 6, 0, 0, 0, 'q', 'r', 't', 'r', 'x', 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    CHECK(qcom_glink_poll(&a) == 1 && a.unknown_opens == 1 && !strcmp(a.last_unknown, "qrtrx"));
    /* Every distinct unknown name is listed once. */
    {
        const char *names[3] = {"apr", "qrtrx", "IPCRTR"};
        for (unsigned k = 0; k < 3; ++k) {
            uint8_t m[8 + 8] = {QCOM_GLINK_OPEN, 0, (uint8_t)(10 + k), 0, (uint8_t)(strlen(names[k]) + 1), 0, 0, 0};
            memcpy(m + 8, names[k], strlen(names[k]));
            uint32_t head = desc[3];
            for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
            desc[3] = head;
        }
    }
    CHECK(qcom_glink_poll(&a) == 3 && a.unknown_opens == 4 && !strcmp(a.unknown_names, "qrtrx apr IPCRTR"));
    /* A channel registered after the remote opened it: the OPEN was kept (the repeat of "qrtrx" with its new id replaced the first), the
     * OPEN_ACK and our OPEN go out at the next poll, a CLOSE before anyone came forgets it. */
    {
        static uint8_t store[64];
        static Seen late;
        const uint32_t before = desc[1];
        QcomGlinkChannel *cl = qcom_glink_channel(&a, "apr", 64, 1, store, &client, &late, 0);
        CHECK(cl && cl->remote_open && cl->ack_owed && cl->rcid == 10 && cl->open_sent == 2);
        CHECK(qcom_glink_poll(&a) == 0 && !cl->ack_owed && cl->open_sent == 1);
        uint8_t ack[8], open[8 + 8];
        for (unsigned i = 0; i < 8; ++i) ack[i] = fifo_a[(before + i) % FIFO];
        for (unsigned i = 0; i < sizeof open; ++i) open[i] = fifo_a[(before + 8 + i) % FIFO];
        static const uint8_t want_ack[8] = {QCOM_GLINK_OPEN_ACK, 0, 10, 0, 0, 0, 0, 0};
        CHECK(!memcmp(ack, want_ack, 8));
        CHECK(open[0] == QCOM_GLINK_OPEN && open[2] == cl->lcid && open[4] == 4 && !memcmp(open + 8, "apr", 4));
        CHECK(a.waiting[0].used && a.waiting[0].rcid == 11 && !strcmp(a.waiting[0].name, "qrtrx"));
        uint8_t m[8] = {QCOM_GLINK_CLOSE, 0, 11, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
        CHECK(qcom_glink_poll(&a) == 1 && !a.waiting[0].used);
        static uint8_t store2[64];
        QcomGlinkChannel *gone = qcom_glink_channel(&a, "qrtrx", 64, 1, store2, &client, &late, 0);
        CHECK(gone && !gone->remote_open && !gone->ack_owed && gone->open_sent == 2);
    }
    /* The remote opens our channel again: OPEN_ACK, then our OPEN with the name, padded. */
    {
        const uint32_t before = desc[1];
        uint8_t m[8 + 24] = {QCOM_GLINK_OPEN, 0, 7, 0, 19, 0, 0, 0};
        memcpy(m + 8, "PMIC_RTR_ADSP_APPS", 19);
        uint32_t head = desc[3];
        for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
        CHECK(qcom_glink_poll(&a) == 1 && ca->remote_open && ca->rcid == 7 && ca->open_sent == 1);
        uint8_t ack[8], open[8 + 24];
        for (unsigned i = 0; i < 8; ++i) ack[i] = fifo_a[(before + i) % FIFO];
        for (unsigned i = 0; i < sizeof open; ++i) open[i] = fifo_a[(before + 8 + i) % FIFO];
        static const uint8_t want_ack[8] = {QCOM_GLINK_OPEN_ACK, 0, 7, 0, 0, 0, 0, 0};
        CHECK(!memcmp(ack, want_ack, 8));
        CHECK(open[0] == QCOM_GLINK_OPEN && open[2] == ca->lcid && open[4] == 19 && !memcmp(open + 8, "PMIC_RTR_ADSP_APPS", 19));
        CHECK(desc[1] == (before + 8 + 32) % FIFO);
    }
    /* A command that is not GLINK stops the link for good. */
    {
        uint8_t m[8] = {0x77, 0, 0, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    CHECK(qcom_glink_poll(&a) == QcomGlinkCorrupt && a.broken);
    CHECK(qcom_glink_poll(&a) == QcomGlinkCorrupt);
    /* FIFO accounting: 8 bytes are never used and 16 more stay for a read notification. */
    uint32_t h = 0, t = 0;
    QcomGlinkFifo f = {&h, &t, fifo_a, FIFO};
    CHECK(qcom_glink_tx_avail(&f) == FIFO - 24 && qcom_glink_rx_avail(&f) == 0);
    h = 100; t = 40;
    CHECK(qcom_glink_tx_avail(&f) == FIFO - 60 - 24 && qcom_glink_rx_avail(&f) == 60);
    h = 40; t = 100;
    CHECK(qcom_glink_tx_avail(&f) == 60 - 24 && qcom_glink_rx_avail(&f) == FIFO - 60);
    h = FIFO; /* out of range indices are never trusted */
    CHECK(qcom_glink_tx_avail(&f) == 0 && qcom_glink_rx_avail(&f) == 0);
}

/* ---- 13T: the SMEM access guard, table-only lookups, SMP2P size bounds, GLINK stop ---- */
static void test_guard(void)
{
    /* The table, the global partition, a partition between two remotes (never readable on the 13T) and the apps-adsp one. */
    memset(smem_words, 0, sizeof smem_words);
    const uint32_t toc = SMEM_BYTES - 0x1000;
    w32(toc, 0x434f5424u); w32(toc + 4, 1); w32(toc + 8, 4);
    make_partition(0, 0x1000, 0x8000, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, 128);
    make_partition(1, 0x9000, 0x6000, 1, 2, 64);       /* modem-adsp */
    make_partition(2, 0x10000, 0x10000, 0, 2, 64);     /* apps-adsp */
    uint32_t refused = 0;
    QcomSmem g = {(volatile uint8_t *)smem_words, SMEM_BYTES, 0, {{0, 0}}, &refused};
    CHECK(qcom_smem_allow(&g, toc, 0x1000) == QcomSmemOk && qcom_smem_allow(&g, toc, 0x1000) == QcomSmemOk && g.ranges == 1);
    QcomSmemPartition p;
    CHECK(qcom_smem_toc_count(&g) == 4);
    CHECK(qcom_smem_toc_entry(&g, 1, &p) == QcomSmemOk && p.offset == 0x9000 && p.host0 == 1 && p.host1 == 2 && p.cacheline == 64);
    CHECK(qcom_smem_toc_entry(&g, 3, &p) == QcomSmemMissing);   /* an unused slot */
    CHECK(qcom_smem_toc_entry(&g, 4, &p) == QcomSmemMissing);
    /* The table alone locates the partition: nothing else is read. */
    CHECK(qcom_smem_toc_find(&g, 0, 2, &p) == QcomSmemOk && p.offset == 0x10000 && p.size == 0x10000 && refused == 0);
    CHECK(qcom_smem_toc_find(&g, 0, 5, &p) == QcomSmemNoPartition && refused == 0);
    /* Its header lies outside the readable ranges: refused, not read. */
    CHECK(qcom_smem_partition(&g, 0, 2, &p) == QcomSmemBadPartition && refused == 1);
    CHECK(qcom_smem_allow(&g, 0x10000, 0x10000) == QcomSmemOk);
    refused = 0;
    CHECK(qcom_smem_partition(&g, 0, 2, &p) == QcomSmemOk && refused == 0);
    CHECK(qcom_smem_alloc(&g, &p, 429, 340) == QcomSmemOk && refused == 0);
    uint32_t at = 0, size = 0;
    CHECK(qcom_smem_find(&g, &p, 429, &at, &size) == QcomSmemOk && at >= 0x10000 && at + size <= 0x20000 && refused == 0);
    /* Writes and reads outside every range are dropped and counted, as are unaligned ones. */
    const uint32_t before = smem_words[0x9000 / 4];
    qcom_smem_write32(&g, 0x9000, 0xdead);
    CHECK(smem_words[0x9000 / 4] == before && refused == 1);
    CHECK(qcom_smem_read32(&g, 0x9000) == 0 && refused == 2);
    CHECK(qcom_smem_read32(&g, 0x10002) == 0 && refused == 3);
    CHECK(qcom_smem_read32(&g, SMEM_BYTES) == 0 && refused == 4);
    CHECK(qcom_smem_allowed(&g, 0x1fffc, 4) && !qcom_smem_allowed(&g, 0x1fffc, 8));
    /* Ranges must lie inside the region; four at most. */
    CHECK(qcom_smem_allow(&g, SMEM_BYTES - 8, 16) == QcomSmemGuard && qcom_smem_allow(&g, 0, 0) == QcomSmemGuard);
    CHECK(qcom_smem_allow(&g, 0x1000, 0x8000) == QcomSmemOk && qcom_smem_allow(&g, 0x9000, 0x10) == QcomSmemOk);
    CHECK(qcom_smem_allow(&g, 0x20000, 0x10) == QcomSmemGuard && g.ranges == 4);
    /* A table entry overlapping the table, unaligned, too small, or a host pair listed twice is refused from the table alone. */
    refused = 0;
    w32(toc + 32 + 48 * 3, 0x3f000); w32(toc + 32 + 48 * 3 + 4, 0x2000); w32(toc + 32 + 48 * 3 + 12, 0 | 3u << 16);
    CHECK(qcom_smem_toc_find(&g, 0, 3, &p) == QcomSmemBadPartition);
    w32(toc + 32 + 48 * 3, 0x21004); w32(toc + 32 + 48 * 3 + 4, 0x1000);
    CHECK(qcom_smem_toc_find(&g, 0, 3, &p) == QcomSmemBadPartition);
    w32(toc + 32 + 48 * 3, 0x21000); w32(toc + 32 + 48 * 3 + 4, 0x20);
    CHECK(qcom_smem_toc_find(&g, 0, 3, &p) == QcomSmemBadPartition);
    w32(toc + 32 + 48 * 3, 0x21000); w32(toc + 32 + 48 * 3 + 4, 0x1000); w32(toc + 32 + 48 * 3 + 12, 2u | 0u << 16);
    CHECK(qcom_smem_toc_find(&g, 0, 2, &p) == QcomSmemBadPartition && refused == 0);
    /* A free-cached mark beyond the partition is corrupt, not walked. */
    w32(toc + 32 + 48 * 3 + 12, 0 | 3u << 16);
    refused = 0;
    QcomSmem plain = {(volatile uint8_t *)smem_words, SMEM_BYTES, 0, {{0, 0}}, &refused};
    CHECK(qcom_smem_partition(&plain, 0, 2, &p) == QcomSmemOk);
    w32(0x10000 + 16, 0x20000);
    CHECK(qcom_smem_partition(&plain, 0, 2, &p) == QcomSmemBadPartition);
    w32(0x10000 + 16, 0x10000);
    /* No table: the region is too small or the magic is wrong. */
    QcomSmem tiny = {(volatile uint8_t *)smem_words, 0x1000, 0, {{0, 0}}, 0};
    CHECK(qcom_smem_toc_count(&tiny) == QcomSmemNoTable);
    w32(toc, 0);
    CHECK(qcom_smem_toc_count(&plain) == QcomSmemNoTable && qcom_smem_toc_find(&plain, 0, 2, &p) == QcomSmemNoTable);
}

static void test_smp2p_bounds(void)
{
    static uint32_t ours[QCOM_SMP2P_ITEM_BYTES / 4], theirs[QCOM_SMP2P_ITEM_BYTES / 4];
    QcomSmp2p s, remote;
    qcom_smp2p_init_out(&s, ours, 0, 2);
    qcom_smp2p_init_out(&remote, theirs, 2, 0);
    for (int i = 0; i < 3; ++i) CHECK(qcom_smp2p_add_entry(&remote, i == 2 ? "slave-kernel" : "other") == i);
    /* An item sized for two entries: the third (slave-kernel) is never read. */
    CHECK(qcom_smp2p_set_in(&s, theirs, 20 + 2 * 20) == 0 && s.in_entries == 2);
    CHECK(qcom_smp2p_find_in(&s, "slave-kernel") == -1 && qcom_smp2p_find_in(&s, "other") == 0);
    CHECK(qcom_smp2p_set_in(&s, theirs, QCOM_SMP2P_ITEM_BYTES) == 0 && s.in_entries == 16 && qcom_smp2p_find_in(&s, "slave-kernel") == 2);
    CHECK(qcom_smp2p_set_in(&s, theirs, 19) == -1 && qcom_smp2p_set_in(&s, 0, 400) == -1);
    /* Two outbound entries as the 13T adds them: master-kernel, then sleepstate (awake). */
    CHECK(qcom_smp2p_add_entry(&s, "master-kernel") == 0 && qcom_smp2p_add_entry(&s, "sleepstate") == 1);
    CHECK(qcom_smp2p_update(&s, 1, 1, 1) == 1 && ours[3] == (16u | 2u << 16) && ours[5 + 5 + 4] == 1);
    CHECK(!memcmp(&ours[5 + 5], "sleepstate", 10));
}

static void test_glink_stop(void)
{
    static QcomGlink a, b;
    static uint8_t store_a[2 * 128], store_b[2 * 128];
    memset(desc, 0, sizeof desc);
    QcomGlinkFifo a_tx = {&desc[1], &desc[0], fifo_a, FIFO}, a_rx = {&desc[3], &desc[2], fifo_b, FIFO};
    QcomGlinkFifo b_tx = {&desc[3], &desc[2], fifo_b, FIFO}, b_rx = {&desc[1], &desc[0], fifo_a, FIFO};
    qcom_glink_init(&a, a_tx, a_rx, kick_a, 0);
    qcom_glink_init(&b, b_tx, b_rx, kick_b, 0);
    const QcomGlinkClient client = {on_state, on_receive};
    Seen sa = {0}, sb = {0};
    QcomGlinkChannel *ca = qcom_glink_channel(&a, "adsp_apps", 128, 2, store_a, &client, &sa, 0);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "adsp_apps", 128, 2, store_b, &client, &sb, 1);
    CHECK(ca && cb);
    /* Eight channel slots in all. */
    for (unsigned i = 1; i < QCOM_GLINK_MAX_CHANNELS; ++i) CHECK(qcom_glink_channel(&a, "spare", 0, 0, 0, &client, &sa, 0));
    CHECK(!qcom_glink_channel(&a, "one too many", 0, 0, 0, &client, &sa, 0));
    settle(&a, &b);
    CHECK(ca->up && sa.ups == 1);
    /* Stopped (the remote crashed): the open channel goes down once, nothing more is read or sent. */
    qcom_glink_stop(&a);
    CHECK(a.broken && !ca->up && sa.downs == 1);
    qcom_glink_stop(&a);
    CHECK(sa.downs == 1);
    CHECK(qcom_glink_poll(&a) == QcomGlinkCorrupt && qcom_glink_send(&a, ca, "x", 1) == QcomGlinkNotOpen);
}

/* A message that arrives while our FIFO is full: its RX_DONE cannot go out at once. It is owed (never dropped, or the remote keeps that
 * intent busy for good and, a few times over, has none left), a read notification asks the remote to ring us once it has read, and the
 * RX_DONE goes out on the next poll. Six rounds against four intents: without the owed RX_DONE the channel would stall. */
static unsigned busy_remote(const QcomGlinkChannel *c)
{
    unsigned n = 0;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k) n += c->remote[k].valid && c->remote[k].busy;
    return n;
}
static unsigned owed_local(const QcomGlinkChannel *c)
{
    unsigned n = 0;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_LOCAL_INTENTS; ++k) n += c->local[k].owed;
    return n;
}
static void test_glink_owed(void)
{
    static QcomGlink a, b;
    static uint8_t store_a[4 * 256], store_b[4 * 256];
    memset(desc, 0, sizeof desc);
    QcomGlinkFifo a_tx = {&desc[1], &desc[0], fifo_a, FIFO}, a_rx = {&desc[3], &desc[2], fifo_b, FIFO};
    QcomGlinkFifo b_tx = {&desc[3], &desc[2], fifo_b, FIFO}, b_rx = {&desc[1], &desc[0], fifo_a, FIFO};
    qcom_glink_init(&a, a_tx, a_rx, kick_a, 0);
    qcom_glink_init(&b, b_tx, b_rx, kick_b, 0);
    const QcomGlinkClient client = {on_state, on_receive};
    Seen sa = {0}, sb = {0};
    QcomGlinkChannel *ca = qcom_glink_channel(&a, "PMIC_RTR_ADSP_APPS", 256, 4, store_a, &client, &sa, 0);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "PMIC_RTR_ADSP_APPS", 256, 4, store_b, &client, &sb, 1);
    CHECK(ca && cb);
    settle(&a, &b);
    CHECK(ca->up && cb->up && a.features == QCOM_GLINK_FEATURE_INTENT_REUSE);
    uint8_t m[240];
    memset(m, 0x5a, sizeof m);
    for (unsigned round = 0; round < 6; ++round) {
        /* A fills its FIFO with four messages B does not read yet: not even an 8-byte command fits after them. */
        CHECK(qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk && qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk);
        CHECK(qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk && qcom_glink_send(&a, ca, m, 216) == QcomGlinkOk);
        CHECK(qcom_glink_tx_avail(&a.tx) < 8);
        /* B (its FIFO has room) sends to A; A takes the message, but the RX_DONE is owed and a read notification asked for. */
        CHECK(qcom_glink_send(&b, cb, "pong", 4) == QcomGlinkOk);
        const unsigned owed_before = a.done_owed, kicks_before = kicks_a, kicks_b_before = kicks_b;
        CHECK(qcom_glink_poll(&a) == 1 && sa.messages == round + 1 && !memcmp(sa.last, "pong", 4));
        CHECK(owed_local(ca) == 1 && a.done_owed == owed_before + 1 && kicks_a == kicks_before + 1 && busy_remote(cb) == 1);
        /* B reads the four messages and the notification and rings A; A then pays what it owed. */
        CHECK(qcom_glink_poll(&b) == 5 && sb.messages == 4 * (round + 1) && kicks_b > kicks_b_before);
        qcom_glink_poll(&a);
        CHECK(owed_local(ca) == 0);
        settle(&a, &b);
        /* Every intent is free again on both sides. */
        CHECK(busy_remote(cb) == 0 && busy_remote(ca) == 0);
    }
    CHECK(a.dropped == 0 && b.dropped == 0 && sa.messages == 6 && sb.messages == 24);
    /* A reset (the remote closed the channel) forgets what was owed with the intents. */
    ca->local[0].owed = 1;
    {
        uint8_t close[8] = {QCOM_GLINK_CLOSE, 0, (uint8_t)cb->lcid, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = close[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    qcom_glink_poll(&a);
    CHECK(!ca->up && owed_local(ca) == 0);
}

/* The commands A wrote into its FIFO between two head positions, counted by command. */
static void count_commands(uint32_t from, uint32_t to, unsigned counts[16])
{
    for (unsigned guard = 0; from != to && guard < FIFO / 8; ++guard) {
        uint8_t m[16];
        for (unsigned i = 0; i < 16; ++i) m[i] = fifo_a[(from + i) % FIFO];
        const unsigned cmd = (unsigned)(m[0] | m[1] << 8);
        uint32_t length = 8;
        if (cmd == QCOM_GLINK_TX_DATA || cmd == QCOM_GLINK_TX_DATA_CONT) length = (16u + (m[8] | (uint32_t)m[9] << 8) + 7u) & ~7u;
        else if (cmd == QCOM_GLINK_INTENT) length = 8u + 8u * m[4];
        else if (cmd == QCOM_GLINK_OPEN) length = (8u + m[4] + 7u) & ~7u;
        if (cmd < 16) counts[cmd]++;
        from = (from + length) % FIFO;
    }
    CHECK(from == to);
}

/* The remote closes while our FIFO has room for one 8-byte command but not two: the CLOSE waits (with a read notification) and is then
 * answered exactly once, CLOSE_ACK and our own CLOSE together; handled twice, it would be acknowledged twice. */
static void test_glink_close_full(void)
{
    static QcomGlink a, b;
    static uint8_t store_a[4 * 256], store_b[4 * 256];
    memset(desc, 0, sizeof desc);
    QcomGlinkFifo a_tx = {&desc[1], &desc[0], fifo_a, FIFO}, a_rx = {&desc[3], &desc[2], fifo_b, FIFO};
    QcomGlinkFifo b_tx = {&desc[3], &desc[2], fifo_b, FIFO}, b_rx = {&desc[1], &desc[0], fifo_a, FIFO};
    qcom_glink_init(&a, a_tx, a_rx, kick_a, 0);
    qcom_glink_init(&b, b_tx, b_rx, kick_b, 0);
    const QcomGlinkClient client = {on_state, on_receive};
    Seen sa = {0}, sb = {0};
    QcomGlinkChannel *ca = qcom_glink_channel(&a, "adsp_apps", 256, 4, store_a, &client, &sa, 1);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "adsp_apps", 256, 4, store_b, &client, &sb, 0);
    CHECK(ca && cb);
    settle(&a, &b);
    CHECK(ca->up && cb->up && ca->open_sent == 1);
    uint8_t m[240];
    memset(m, 0x3c, sizeof m);
    CHECK(qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk && qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk);
    CHECK(qcom_glink_send(&a, ca, m, 240) == QcomGlinkOk && qcom_glink_send(&a, ca, m, 208) == QcomGlinkOk);
    CHECK(qcom_glink_tx_avail(&a.tx) == 8);
    {
        uint8_t close[8] = {QCOM_GLINK_CLOSE, 0, (uint8_t)cb->lcid, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = close[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    unsigned counts[16] = {0};
    uint32_t mark = desc[1];
    CHECK(qcom_glink_poll(&a) == 0 && ca->up && sa.downs == 0);    /* nothing answered yet */
    count_commands(mark, desc[1], counts);
    CHECK(counts[QCOM_GLINK_CLOSE_ACK] == 0 && counts[QCOM_GLINK_READ_NOTIF] == 1);
    qcom_glink_poll(&b);                                           /* B reads, rings, and gives its intents back */
    mark = desc[1];
    qcom_glink_poll(&a);
    count_commands(mark, desc[1], counts);
    CHECK(!ca->up && sa.downs == 1 && counts[QCOM_GLINK_CLOSE_ACK] == 1 && counts[QCOM_GLINK_CLOSE] == 1);
    /* A opened this channel, so it opens it again; B closes its side, then answers the new OPEN: up again on both sides. */
    settle(&a, &b);
    CHECK(ca->up && cb->up && sa.downs == 1 && sa.ups == 2);
}

int main(void)
{
    test_mdt();
    test_smem();
    test_smp2p();
    test_glink();
    test_guard();
    test_smp2p_bounds();
    test_glink_stop();
    test_glink_owed();
    test_glink_close_full();
    printf("PASS adsp cores (MDT, SMEM and guard, SMP2P, GLINK, owed RX_DONE, CLOSE on a full FIFO): %u checks\n", checks);
    return 0;
}
