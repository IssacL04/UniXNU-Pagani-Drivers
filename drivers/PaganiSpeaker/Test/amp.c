/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Host tests of the GENI I2C core (GeniI2c.c; the engine model and the cases follow UniXNU-sheng package/tests/geni_i2c_host.c, commit
 * 08cde20e) and of the TFA9873 programs and runner (Tfa9873.c) against a register model of the amplifier: the reset, the keys, the
 * revision tables, every container field per amplifier, slot width and feedback setting, the order of the writes, the readback check,
 * power up and down and the status read. With -DCONTAINER_ITEMS="file" (made by check-pagani-speaker-host.py from the decoded
 * tfa98xx.cnt) every device item of the phone's container is compared with what init writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GeniI2c.h"
#include "Tfa9873.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* ---- the serial engine model with one 8-bit register device at 0x11 and one TFA9873-like 16-bit device at 0x34 ---- */
typedef struct {
    uint32_t reg[0x1000 / 4];
    uint32_t status;
    int active, op, hold, length, address;
    uint32_t rx, rx_next, rx_count;
    int present, mute, cancel_ack, abort_ack;
    uint8_t slave[256], pointer;
    uint16_t wide[256]; uint8_t wide_pointer;
    uint8_t tx[8]; int tx_bytes;
    unsigned accesses, commands;
} Engine;

static uint32_t eng_read(void *ctx, uint32_t r)
{
    Engine *e = ctx;
    e->accesses++;
    if (r == GENI_M_IRQ_STATUS) return e->status;
    if (r == GENI_RX_FIFO_STATUS) return e->rx_count;
    if (r == GENI_RX_FIFO) {
        const uint32_t word = e->rx;
        e->rx = e->rx_next;
        if (e->rx_count) e->rx_count--;
        return word;
    }
    CHECK(r < sizeof e->reg && !(r & 3));
    return e->reg[r / 4];
}
static void eng_write(void *ctx, uint32_t r, uint32_t v)
{
    Engine *e = ctx;
    e->accesses++;
    switch (r) {
    case GENI_M_IRQ_CLEAR: e->status &= ~v; return;
    case GENI_M_CMD0:
        e->commands++;
        if (e->active) {e->status |= GENI_M_ILLEGAL_CMD | GENI_M_CMD_DONE; return;}
        e->op = (int)(v >> 27); e->address = (int)((v >> 9) & 0x7f); e->hold = (v & GENI_STOP_STRETCH) != 0; e->active = 1;
        if (e->op == GENI_OP_WRITE) {
            e->length = (int)e->reg[GENI_TX_LEN / 4];
            if (e->reg[GENI_TX_WATERMARK / 4] && !e->mute) e->status |= GENI_M_TX_WATERMARK;
        } else {
            const uint32_t n = e->reg[GENI_RX_LEN / 4];
            CHECK(e->op == GENI_OP_READ && (n == 1 || (e->address == 0x34 && n == 2)));
            if (e->mute) return;
            if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; e->active = 0; return;}
            uint32_t word;
            if (n == 2) {const uint16_t w = e->wide[e->wide_pointer]; word = (uint32_t)(w >> 8) | (uint32_t)(w & 0xff) << 8;}
            else word = e->slave[e->pointer++];
            /* a FIFO: a word still in it comes out first */
            if (e->rx_count) e->rx_next = word;
            else e->rx = word;
            e->rx_count++;
            e->status |= GENI_M_CMD_DONE; e->active = 0;
        }
        return;
    case GENI_TX_WATERMARK:
        e->reg[r / 4] = v;
        if (v && e->active && e->op == GENI_OP_WRITE && !e->mute) e->status |= GENI_M_TX_WATERMARK;
        return;
    case GENI_TX_FIFO:
        CHECK(e->active && e->op == GENI_OP_WRITE);
        for (int i = 0; i < 4 && e->tx_bytes < e->length; ++i) e->tx[e->tx_bytes++] = (uint8_t)(v >> (8 * i));
        if (e->tx_bytes < e->length) return;
        e->tx_bytes = 0;
        e->active = 0;
        if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; return;}
        if (e->address == 0x34) {
            CHECK(e->length == 1 || e->length == 3);
            e->wide_pointer = e->tx[0];
            if (e->length == 3) e->wide[e->tx[0]] = (uint16_t)(e->tx[1] << 8 | e->tx[2]);
            e->status |= GENI_M_CMD_DONE;
            return;
        }
        e->pointer = (uint8_t)v;
        if (e->length == 2) e->slave[e->pointer] = (uint8_t)(v >> 8);
        e->status |= GENI_M_CMD_DONE;
        return;
    case GENI_M_CMD_CTRL:
        if (v & GENI_CMD_CTRL_CANCEL && e->cancel_ack) {e->active = 0; e->status |= GENI_M_CMD_CANCEL;}
        if (v & GENI_CMD_CTRL_ABORT && e->abort_ack) {e->active = 0; e->status |= GENI_M_CMD_ABORT;}
        return;
    default:
        CHECK(r < sizeof e->reg && !(r & 3));
        e->reg[r / 4] = v;
    }
}
static void boot_state(Engine *e)
{
    memset(e, 0, sizeof *e);
    e->reg[GENI_FW_REVISION / 4] = GENI_PROTO_I2C << 8 | 0x47;
    e->reg[GENI_CLK_CFG / 4] = 0x21;
    e->reg[GENI_SCL_COUNTERS / 4] = 5u << 20 | 11u << 10 | 22u;
    e->reg[GENI_DMA_MODE / 4] = 1;
    e->present = 1; e->cancel_ack = 1; e->abort_ack = 1;
}
static int run(GeniI2c *g, unsigned *n)
{
    for (unsigned i = 1; i <= 8; ++i) {
        const int r = pagani_geni_i2c_poll(g);
        if (r != GeniPending) {if (n) *n = i; return r;}
    }
    return GeniPending;
}

static void geni(void)
{
    Engine e;
    GeniI2c g;
    const GeniIo io = {eng_read, eng_write, &e};
    uint32_t config = 0;
    unsigned n = 0;
    boot_state(&e); e.reg[GENI_FW_REVISION / 4] = 2 << 8;
    CHECK(pagani_geni_i2c_prepare(&g, &io, 0x11, &config) == -1 && config >> 24 == 2);
    boot_state(&e); e.reg[GENI_IF_DISABLE / 4] = 1;
    CHECK(pagani_geni_i2c_prepare(&g, &io, 0x11, &config) == -1);
    boot_state(&e); e.reg[GENI_CLK_CFG / 4] = 0x20;
    CHECK(pagani_geni_i2c_prepare(&g, &io, 0x11, &config) == -1);
    boot_state(&e); e.reg[GENI_SCL_COUNTERS / 4] = 0;
    CHECK(pagani_geni_i2c_prepare(&g, &io, 0x11, &config) == -1 && e.reg[GENI_DMA_MODE / 4] == 1);
    boot_state(&e);
    e.reg[GENI_M_IRQ_EN / 4] = 1u << 26; e.status = GENI_M_CMD_DONE;
    CHECK(pagani_geni_i2c_prepare(&g, &io, 0x11, &config) == 0 && config == (3u << 24 | 0x21));
    CHECK(e.reg[GENI_DMA_MODE / 4] == 0 && e.reg[GENI_TX_PACK0 / 4] == 0x7f8fe && e.reg[GENI_RX_PACK1 / 4] == 0xffefe && e.status == 0);
    CHECK(e.reg[GENI_M_IRQ_EN / 4] == GENI_M_IRQ_WANTED && (e.reg[GENI_SE_IRQ_EN / 4] & GENI_SE_M_IRQ_EN));
    e.slave[4] = 0xaa;
    CHECK(pagani_geni_i2c_start_write(&g, 0x04, 0x05) == 0 && pagani_geni_i2c_busy(&g) && pagani_geni_i2c_start_read(&g, 5) == -1);
    CHECK(run(&g, &n) == GeniDone && n == 2 && e.slave[4] == 0x05 && !pagani_geni_i2c_busy(&g));
    e.slave[5] = 0x7d;
    CHECK(pagani_geni_i2c_start_read(&g, 0x05) == 0 && e.hold && run(&g, &n) == GeniDone && n == 3 && g.data == 0x7d);
    CHECK(pagani_geni_i2c_start_write(&g, 0x06, 0x13) == 0);
    e.status = 0; e.active = 0;
    CHECK(pagani_geni_i2c_event(&g, GENI_M_CMD_DONE) == GeniFailed && g.error == GeniErrState);
    e.present = 0;
    CHECK(pagani_geni_i2c_start_write(&g, 4, 1) == 0 && run(&g, NULL) == GeniFailed && g.error == GeniErrBus && (g.error_status & (1u << 10)));
    e.present = 1;
    const uint32_t stray = g.stray;
    CHECK(pagani_geni_i2c_event(&g, GENI_M_CMD_DONE) == GeniPending && g.stray == stray + 1);
    e.mute = 1; e.status = 0;
    CHECK(pagani_geni_i2c_start_write(&g, 0x04, 2) == 0 && run(&g, NULL) == GeniPending);
    CHECK(pagani_geni_i2c_timeout(&g) == GeniPending && g.phase == GeniCancelling && run(&g, NULL) == GeniFailed && g.error == GeniErrTimeout);
    e.cancel_ack = 0;
    CHECK(pagani_geni_i2c_start_read(&g, 0x04) == 0 && pagani_geni_i2c_timeout(&g) == GeniPending && run(&g, NULL) == GeniPending);
    CHECK(pagani_geni_i2c_timeout(&g) == GeniPending && g.phase == GeniAborting && run(&g, NULL) == GeniFailed);
    e.abort_ack = 0; e.active = 0;
    CHECK(pagani_geni_i2c_start_write(&g, 0x04, 3) == 0 && pagani_geni_i2c_timeout(&g) == GeniPending &&
          pagani_geni_i2c_timeout(&g) == GeniPending && pagani_geni_i2c_timeout(&g) == GeniFailed && !pagani_geni_i2c_busy(&g));
    e.mute = 0; e.active = 0; e.status = 0; e.cancel_ack = e.abort_ack = 1;
    for (unsigned v = 0; v < 256; ++v) {
        CHECK(pagani_geni_i2c_start_write(&g, (uint8_t)(v ^ 0x5a), (uint8_t)v) == 0 && run(&g, NULL) == GeniDone);
        CHECK(pagani_geni_i2c_start_read(&g, (uint8_t)(v ^ 0x5a)) == 0 && run(&g, NULL) == GeniDone && g.data == v);
    }
    /* A TFA9873 register: 3-byte write (register, value high, low); 1-byte write holding the stop, then a 2-byte read. */
    GeniI2c w;
    CHECK(pagani_geni_i2c_prepare(&w, &io, 0x34, NULL) == 0);
    const uint8_t put[3] = {0x20, 0x2b, 0x83}, at[1] = {0x20};
    CHECK(pagani_geni_i2c_start_write_bytes(&w, put, 3) == 0 && e.length == 3 && run(&w, NULL) == GeniDone && e.wide[0x20] == 0x2b83);
    CHECK(pagani_geni_i2c_start_read_bytes(&w, at, 1, 2) == 0 && e.hold && run(&w, NULL) == GeniDone && tfa9873_wire_value(w.rx) == 0x2b83);
    /* A word a read cut short left in the FIFO is not taken for the next read's (a read-modify-write of a TFA register would write it). */
    e.rx = 0xdead;
    e.rx_count = 1;
    CHECK(pagani_geni_i2c_start_read_bytes(&w, at, 1, 2) == 0 && run(&w, NULL) == GeniDone && tfa9873_wire_value(w.rx) == 0x2b83 && !e.rx_count);
}

/* ---- the TFA9873 register model ---- */
typedef struct {
    uint16_t reg[256], por[256];
    unsigned writes, reads, waits, transfers;
    uint8_t order[512]; uint16_t order_value[512]; unsigned logged;   /* every register written, in order */
    int keys_open, key1_ok, key2_ok;
    int ignore;                    /* writes to this register are dropped (a readback mismatch); -1 none */
    int clks_after;                /* CLKS reads 1 from this many STATUS reads after PWDN = 0 and MANSCONF = 1; -1 never */
    int status_reads;
} Tfa;

static void tfa_por(Tfa *m, uint16_t rev)
{
    memset(m, 0, sizeof *m);
    for (unsigned i = 0; i < 256; ++i) m->por[i] = (uint16_t)(i * 0x0101u ^ 0x5a00u);   /* arbitrary reset values */
    m->por[0x00] = 0x0001; m->por[0x01] = 0x0000; m->por[0x02] = 0x0008; m->por[0x03] = rev; m->por[0x04] = 0x0100;   /* FSSYNCEN on at reset */
    m->por[0x10] = 0; m->por[0x11] = 0; m->por[0x14] = 0; m->por[0xfb] = 0x1234; m->por[0x40] = 0x00c0;
    m->por[0x20] = 0x0001;                                    /* TDME on at reset (as a running interface would be) */
    memcpy(m->reg, m->por, sizeof m->reg);
    m->ignore = -1;
    m->clks_after = 2;
}
static void tfa_write(Tfa *m, uint8_t r, uint16_t v)
{
    m->writes++;
    if (m->logged < 512) {m->order[m->logged] = r; m->order_value[m->logged++] = v;}
    if (r == m->ignore) return;
    if (r == 0x00 && (v & 2)) {memcpy(m->reg, m->por, sizeof m->reg); m->keys_open = 0; return;}   /* I2CR */
    if (r == 0x0f) m->keys_open = v == 0x5a6b;
    if (r == 0xa0) m->key1_ok = m->keys_open && v == (m->reg[0xfb] ^ 0x5a);
    if (r == 0xa1) m->key2_ok = m->keys_open && v == 0x5a;
    if (r == 0x44) {m->reg[0x40] &= (uint16_t)~v; return;}   /* write one to clear */
    m->reg[r] = v;
    if (r == 0x00 || r == 0x01) m->status_reads = 0;
}
static uint16_t tfa_read(Tfa *m, uint8_t r)
{
    m->reads++;
    if (r == 0x10) {
        const int running = !(m->reg[0x00] & 1) && (m->reg[0x01] & 4);
        m->status_reads++;
        return (uint16_t)(running && m->clks_after >= 0 && m->status_reads > m->clks_after ? 0x40 : 0);
    }
    if (r == 0x14) return (uint16_t)((m->reg[0x00] & 8) ? 9u << 3 : 0);
    return m->reg[r];
}
static unsigned play(Tfa *m, Tfa9873Run *r)
{
    Tfa9873Action a;
    unsigned transfers = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        const int k = tfa9873_run_next(r, &a);
        if (k == Tfa9873Finished) return transfers;
        if (k == Tfa9873Wait) {m->waits += a.delay_ms; continue;}
        CHECK(k == Tfa9873Transfer);
        ++transfers;
        if (a.read_length) {
            CHECK(a.length == 1 && a.read_length == 2);
            tfa9873_run_result(r, tfa_read(m, a.bytes[0]));
        } else {
            CHECK(a.length == 3);
            tfa_write(m, a.bytes[0], (uint16_t)(a.bytes[1] << 8 | a.bytes[2]));
            tfa9873_run_result(r, 0);
        }
    }
    CHECK(0);
    return 0;
}
static unsigned field_value(const Tfa *m, uint16_t f)
{
    return (m->reg[TFA9873_FIELD_REG(f)] & TFA9873_FIELD_MASK(f)) >> TFA9873_FIELD_POS(f);
}
static int first_write(const Tfa *m, uint8_t reg, int from)
{
    for (unsigned i = (unsigned)from; i < m->logged; ++i) if (m->order[i] == reg) return (int)i;
    return -1;
}
static int last_write(const Tfa *m, uint8_t reg)
{
    for (unsigned i = m->logged; i-- > 0;) if (m->order[i] == reg) return (int)i;
    return -1;
}

static void init_case(uint16_t rev, uint8_t amp, uint8_t bits, uint8_t feedback)
{
    static Tfa m;
    Tfa9873Step steps[TFA9873_MAX_STEPS];
    Tfa9873Run r;
    const Tfa9873Config k = {rev, amp, bits, feedback};
    tfa_por(&m, rev);
    unsigned n = tfa9873_init_program(steps, &k);
    CHECK(n > 40 && n < TFA9873_MAX_STEPS);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    /* The reset first, the keys before anything hidden, the revision table next. */
    CHECK(m.logged && m.order[0] == 0x00 && (m.order_value[0] & 2) && m.waits >= 1);
    CHECK(m.key1_ok && m.key2_ok && m.keys_open);
    const unsigned rows = tfa9873_table_size(rev);
    CHECK(rows == (rev == 0x0a73 ? 18u : rev == 0x0b73 ? 10u : 0u));
    CHECK(m.order[1] == 0x0f && m.order[2] == 0xa0 && m.order[3] == 0x0f && m.order[4] == 0xa1);
    if (rows) CHECK(m.order[5] == 0x02 && m.order_value[5] == 0x0628);
    CHECK(m.reg[0x48] == 0x001d && m.reg[0x0f] == 0x5a6b && m.reg[0xa1] == 0x5a);
    /* Every container field, as the amplifier's own device list gives it, with the 13T overrides. */
    CHECK(field_value(&m, Tfa9873Tdmnbck) == (bits == 32 ? 2u : 0u));
    CHECK(field_value(&m, Tfa9873Tdmslln) == (bits == 32 ? 31u : 15u) && field_value(&m, Tfa9873Tdmssize) == (bits == 32 ? 31u : 15u));
    CHECK(field_value(&m, Tfa9873Tdmslots) == 1 && field_value(&m, Tfa9873Tdmfspol) == 1 && field_value(&m, Tfa9873Tdmclinv) == 0);
    CHECK(field_value(&m, Tfa9873Tdmdel) == 1 && field_value(&m, Tfa9873Tdmdce) == 1 && field_value(&m, Tfa9873Tdmvse) == 0);
    CHECK(field_value(&m, Tfa9873Tdmcse) == feedback);
    CHECK(field_value(&m, Tfa9873Tdmspks) == amp && field_value(&m, Tfa9873Tdmdcs) == (amp ? 0u : 1u));
    CHECK(field_value(&m, Tfa9873Tdmcss) == amp && field_value(&m, Tfa9873Tdmvss) == (amp ? 0u : 1u));
    CHECK(field_value(&m, Tfa9873Tdmsrcmap) == 3 && field_value(&m, Tfa9873Tdmsrcas) == 1 && field_value(&m, Tfa9873Tdmsrcbs) == 2);
    CHECK(field_value(&m, Tfa9873Audfs) == 8 && field_value(&m, Tfa9873Fractdel) == 0x31 && field_value(&m, Tfa9873Encurcomp) == 0);
    CHECK(field_value(&m, 0x8510) == 0 && field_value(&m, 0x54a1) == 3 && field_value(&m, 0x7371) == 3 && field_value(&m, 0xf910) == 1);
    CHECK(field_value(&m, 0x8790) == 0 && field_value(&m, 0x6505) == 1 && field_value(&m, 0x71c1) == 1);
    CHECK(field_value(&m, Tfa9873Tdmspkg) == 6 && field_value(&m, Tfa9873Dctrip) == 10 && field_value(&m, Tfa9873Dctript) == 16);
    CHECK(field_value(&m, Tfa9873Dcvos) == 0x2f && field_value(&m, Tfa9873Dcmcc) == 9 && field_value(&m, Tfa9873Fssyncen) == 1);
    CHECK(field_value(&m, Tfa9873Dca) == 1 && field_value(&m, Tfa9873Lnmode) == 1);   /* profile "speaker" */
    CHECK(field_value(&m, Tfa9873Tdme) == 1);
    /* Powered down and unconfigured at the end. */
    CHECK(field_value(&m, Tfa9873Pwdn) == 1 && field_value(&m, Tfa9873Ampe) == 0 && field_value(&m, Tfa9873Mansconf) == 0);
    /* TDME went off before the first TDM format write and back on after the last. */
    const int tdme_off = first_write(&m, 0x20, 0), tdm_last = last_write(&m, 0x27);
    CHECK(tdme_off > 0 && !(m.order_value[tdme_off] & 1) && tdm_last > tdme_off);
    int tdme_on = -1;
    for (int i = tdm_last; i < (int)m.logged; ++i) if (m.order[i] == 0x20 && (m.order_value[i] & 1)) {tdme_on = i; break;}
    CHECK(tdme_on > tdm_last);
    /* FSSYNCEN: off (tfa9873_specific) before the container turns it on. */
    const int fs_off = first_write(&m, 0x04, 0);
    CHECK(fs_off > 0 && !(m.order_value[fs_off] & 0x100) && (m.reg[0x04] & 0x100));
    /* The readback check agrees; one register that ignores writes is named. */
    n = tfa9873_verify_program(steps);
    CHECK(n == 16);
    tfa9873_run_start(&r, steps, n);
    CHECK(play(&m, &r) == 16 && r.read_count == 16);
    uint16_t got = 0, want = 0;
    CHECK(tfa9873_verify(&k, r.reads, r.read_count, &got, &want) == 0);
    for (unsigned f = 0; f < 4; ++f) {
        const uint16_t fields[4] = {Tfa9873Tdmspkg, Tfa9873Lnmode, Tfa9873Dca, Tfa9873Tdmcse};
        CHECK(tfa9873_expected_field(&k, fields[f]) >= 0);
    }
    CHECK(tfa9873_expected_field(&k, Tfa9873Tdmcse) == feedback && tfa9873_expected_field(&k, 0x1234) == -1);
    tfa_por(&m, rev);
    m.ignore = 0x26;
    n = tfa9873_init_program(steps, &k);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    n = tfa9873_verify_program(steps);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(tfa9873_verify(&k, r.reads, r.read_count, &got, &want) == 0x26 && got == m.por[0x26] && (want & 0xf) == amp);
    CHECK(tfa9873_verify(&k, r.reads, 15, &got, &want) == -1);
}

#ifdef CONTAINER_ITEMS
typedef struct {uint16_t field, value[2];} ContainerItem;
static const ContainerItem kContainer[] = {
#include CONTAINER_ITEMS
};
#endif

static void programs(void)
{
    static Tfa m;
    Tfa9873Step steps[TFA9873_MAX_STEPS];
    Tfa9873Run r;
    CHECK(tfa9873_wire_value(0x8320u) == 0x2083u);
    CHECK(TFA9873_FIELD_REG(Tfa9873Tdmspks) == 0x26 && TFA9873_FIELD_POS(Tfa9873Tdmspks) == 0 && TFA9873_FIELD_WIDTH(Tfa9873Tdmspks) == 4);
    CHECK(TFA9873_FIELD_MASK(Tfa9873Dcvos) == 0x7e00 && TFA9873_FIELD_MASK(Tfa9873Mansconf) == 0x0004 && TFA9873_FIELD_MASK(0x0255) == 0x07e0);
    CHECK(tfa9873_known(0x0a73) && tfa9873_known(0x0b73) && tfa9873_known(0x1a73) && !tfa9873_known(0x0b74) && !tfa9873_known(0));

    /* Identify. */
    tfa_por(&m, 0x0b73);
    unsigned n = tfa9873_identify_program(steps);
    tfa9873_run_start(&r, steps, n);
    CHECK(play(&m, &r) == 1 && r.read_count == 1 && r.reads[0] == 0x0b73);

    for (unsigned rev = 0; rev < 4; ++rev)
        for (uint8_t amp = 0; amp < 2; ++amp)
            for (uint8_t feedback = 0; feedback < 2; ++feedback) {
                const uint16_t revs[4] = {0x0a73, 0x0b73, 0x1a73, 0x2a73};
                init_case(revs[rev], amp, 32, feedback);
                init_case(revs[rev], amp, 16, feedback);
            }
    const Tfa9873Config bad[3] = {{0x0b73, 2, 32, 0}, {0x0b73, 0, 24, 0}, {0x0b73, 0, 32, 2}};
    for (int i = 0; i < 3; ++i) CHECK(tfa9873_init_program(steps, &bad[i]) == 0 && tfa9873_expected_field(&bad[i], Tfa9873Dca) == -1);
    CHECK(tfa9873_init_program(NULL, &bad[0]) == 0 && tfa9873_verify(&bad[0], r.reads, 16, NULL, NULL) == -1);

#ifdef CONTAINER_ITEMS
    /* The phone's container, item by item (with its own TDMCSE = 1: feedback on, 32-bit slots). */
    for (uint8_t amp = 0; amp < 2; ++amp) {
        const Tfa9873Config k = {0x0b73, amp, 32, 1};
        unsigned items = 0;
        for (unsigned i = 0; i < sizeof kContainer / sizeof *kContainer; ++i) {
            CHECK(tfa9873_expected_field(&k, kContainer[i].field) == kContainer[i].value[amp]);
            ++items;
        }
        CHECK(items == 33);
    }
#endif

    /* An update whose bits already hold costs one read. */
    tfa_por(&m, 0x0b73);
    const Tfa9873Step one[1] = {{Tfa9873Update, 0x02, 0, 0, 0x000f, 0x0008}};
    tfa9873_run_start(&r, one, 1);
    const unsigned writes = m.writes;
    CHECK(play(&m, &r) == 1 && m.writes == writes);

    /* Power up: MANSCONF before PWDN, CLKS waited for (1 ms apart), then AMPE. */
    tfa_por(&m, 0x0b73);
    m.clks_after = 3;
    n = tfa9873_power_up_program(steps);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(r.polls_failed == 0 && m.waits == 3 && (m.reg[0x01] & 4) && !(m.reg[0x00] & 1) && (m.reg[0x00] & 8));
    CHECK(first_write(&m, 0x01, 0) < first_write(&m, 0x00, 0));
    /* No bit clock: 50 reads, then the amplifier is enabled anyway (it waits for its clock itself, as the vendor's sequence). */
    tfa_por(&m, 0x0b73);
    m.clks_after = -1;
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(r.polls_failed == 1 && m.waits == 49 && (m.reg[0x00] & 8));
    /* Power down: PWDN first, then AMPE. */
    const unsigned logged = m.logged;
    n = tfa9873_power_down_program(steps);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(m.logged == logged + 2 && (m.order_value[logged] & 9) == 9 && (m.order_value[logged + 1] & 9) == 1);
    /* Status: seven reads, then the latched interrupt bits cleared. */
    m.reg[0x40] = 0x00c4;
    n = tfa9873_status_program(steps);
    tfa9873_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(r.read_count == Tfa9873StCount && r.reads[Tfa9873StIrq] == 0x00c4 && m.reg[0x40] == 0);
    /* The runner after its end, and an empty program. */
    tfa9873_run_result(&r, 7);
    Tfa9873Action a;
    CHECK(tfa9873_run_next(&r, &a) == Tfa9873Finished);
    tfa9873_run_start(&r, steps, 0);
    CHECK(tfa9873_run_next(&r, &a) == Tfa9873Finished);
    const Tfa9873Step junk[1] = {{99, 0, 0, 0, 0, 0}};
    tfa9873_run_start(&r, junk, 1);
    CHECK(tfa9873_run_next(&r, &a) == Tfa9873Finished);
}

int main(void)
{
    geni();
    programs();
    printf("PASS amp: %u checks\n", checks);
    return 0;
}
