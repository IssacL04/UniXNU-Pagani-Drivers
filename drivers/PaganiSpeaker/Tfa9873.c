/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* TFA9873 programs and their runner; see Tfa9873.h for the sources of every register and value. New code: the program/runner shape
 * follows UniXNU-sheng package/ShengCs35l43.c (BSD-2-Clause-Patent), the register facts come from NXP's GPL driver and the phone's
 * container file and are written here as tables, not copied. */
#include "Tfa9873.h"

typedef struct {uint16_t field, value;} Field;
typedef struct {Tfa9873Step *p; unsigned n;} P;

static void add(P *p, uint8_t op, uint8_t reg, uint8_t aux, uint16_t mask, uint16_t value)
{
    if (p->n < TFA9873_MAX_STEPS) {
        Tfa9873Step *s = &p->p[p->n++];
        s->op = op; s->reg = reg; s->aux = aux; s->unused = 0; s->mask = mask; s->value = value;
    }
}
static void write(P *p, uint8_t reg, uint16_t value) {add(p, Tfa9873Write, reg, 0, 0, value);}
static void read(P *p, uint8_t reg) {add(p, Tfa9873Read, reg, 0, 0, 0);}
static void field(P *p, uint16_t f, uint16_t value)
{
    add(p, Tfa9873Update, TFA9873_FIELD_REG(f), 0, TFA9873_FIELD_MASK(f), (uint16_t)((value << TFA9873_FIELD_POS(f)) & TFA9873_FIELD_MASK(f)));
}

/* The per-revision register tables of tfa9873_specific (full register writes after the keys). */
typedef struct {uint8_t reg; uint16_t value;} Reg;
static const Reg kRev0a73[] = {
    {0x02, 0x0628}, {0x4c, 0x00e9}, {0x52, 0x17d0}, {0x56, 0x0011}, {0x58, 0x0200}, {0x59, 0x0001}, {0x5f, 0x0180}, {0x61, 0x0183},
    {0x63, 0x055a}, {0x65, 0x0542}, {0x6f, 0x00a3}, {0x70, 0xa3fb}, {0x71, 0x007e}, {0x83, 0x009a}, {0x84, 0x0211}, {0x85, 0x0382},
    {0x8c, 0x0210}, {0xd5, 0x0000},
};
static const Reg kRev0b73[] = {
    {0x02, 0x0628}, {0x61, 0x0183}, {0x63, 0x005a}, {0x6f, 0x0082}, {0x70, 0xa3eb}, {0x71, 0x107e}, {0x73, 0x0187}, {0x83, 0x071c},
    {0x85, 0x0380}, {0xd5, 0x004d},
};

/* The container's device list (tfa98xx.cnt device 0 at 0x34, device 1 at 0x35; item numbers in the decoded container), in its order. */
typedef struct {uint16_t field; uint16_t value[2]; uint8_t item;} DeviceItem;
static const DeviceItem kDevice[] = {
    {Tfa9873Tdmnbck, {2, 2}, 0},        /* 64 BCK per frame */
    {Tfa9873Tdmslln, {31, 31}, 1},      /* 32-bit slots */
    {Tfa9873Tdmssize, {31, 31}, 2},     /* 32-bit samples */
    {Tfa9873Tdmslots, {1, 1}, 3},       /* 2 slots */
    {Tfa9873Tdmfspol, {1, 1}, 4},
    {Tfa9873Tdmclinv, {0, 0}, 5},
    {Tfa9873Tdmdel, {1, 1}, 6},         /* I2S: data one BCK after the frame sync edge */
    {Tfa9873Tdmcse, {1, 1}, 7},
    {Tfa9873Tdmvse, {0, 0}, 8},
    {Tfa9873Tdmdce, {1, 1}, 9},
    {Tfa9873Tdmspks, {0, 1}, 10},       /* the speaker plays slot 0 (0x34) or 1 (0x35) */
    {Tfa9873Tdmdcs, {1, 0}, 11},
    {Tfa9873Tdmcss, {0, 1}, 12},
    {Tfa9873Tdmvss, {1, 0}, 13},
    {Tfa9873Tdmsrcmap, {3, 3}, 14},
    {Tfa9873Tdmsrcas, {1, 1}, 15},
    {Tfa9873Tdmsrcbs, {2, 2}, 16},
    {Tfa9873Audfs, {8, 8}, 17},         /* 48 kHz */
    {Tfa9873Fractdel, {0x31, 0x31}, 18},
    {Tfa9873Encurcomp, {0, 0}, 19},
    {0x8510, {0, 0}, 20},
    {0x54a1, {3, 3}, 21},
    {0x7371, {3, 3}, 22},
    {0xf910, {1, 1}, 23},
    {0x8790, {0, 0}, 24},
    {0x6505, {1, 1}, 25},
    {Tfa9873Tdmspkg, {6, 6}, 26},
    {Tfa9873Dctrip, {10, 10}, 27},
    {Tfa9873Dctript, {16, 16}, 28},
    {Tfa9873Dcvos, {0x2f, 0x2f}, 29},
    {Tfa9873Dcmcc, {9, 9}, 30},
    {0x71c1, {1, 1}, 31},
    {Tfa9873Fssyncen, {1, 1}, 32},
};
enum {kTdmItems = 17};                  /* items 0..16 are the TDM interface, written between TDME = 0 and TDME = 1 */
/* Profile "speaker" (0x34) / "speakerR" (0x35), the items before its default section. */
static const Field kSpeakerProfile[] = {{Tfa9873Dca, 1}, {Tfa9873Lnmode, 1}, {Tfa9873Tdmspkg, 6}};

int tfa9873_known(uint16_t rev) {return (rev & 0xffu) == TFA9873_FAMILY;}

static const Reg *table(uint16_t rev, unsigned *count)
{
    if (rev == 0x0a73) {*count = sizeof kRev0a73 / sizeof *kRev0a73; return kRev0a73;}
    if (rev == 0x0b73) {*count = sizeof kRev0b73 / sizeof *kRev0b73; return kRev0b73;}
    *count = 0;
    return 0;
}

unsigned tfa9873_table_size(uint16_t rev)
{
    unsigned n = 0;
    table(rev, &n);
    return n;
}

static int config_ok(const Tfa9873Config *c) {return c && c->amp <= 1 && (c->bits == 32 || c->bits == 16) && c->feedback <= 1;}

/* Every field write of init after the register table, in order. */
static unsigned init_fields(const Tfa9873Config *c, Field *out, unsigned capacity)
{
    unsigned n = 0;
#define PUT(f, v) do {if (n < capacity) {out[n].field = (uint16_t)(f); out[n].value = (uint16_t)(v);} ++n;} while (0)
    PUT(Tfa9873Fssyncen, 0);                                 /* tfa9873_specific: FS synchronisation off */
    PUT(Tfa9873Tdme, 0);                                     /* the TDM format changes with the interface off */
    for (unsigned i = 0; i < sizeof kDevice / sizeof *kDevice; ++i) {
        uint16_t v = kDevice[i].value[c->amp];
        if (c->bits == 16) {
            if (kDevice[i].field == Tfa9873Tdmnbck) v = 0;
            if (kDevice[i].field == Tfa9873Tdmslln || kDevice[i].field == Tfa9873Tdmssize) v = 15;
        }
        PUT(kDevice[i].field, v);
        if (i + 1 == kTdmItems) PUT(Tfa9873Tdme, 1);
    }
    for (unsigned i = 0; i < sizeof kSpeakerProfile / sizeof *kSpeakerProfile; ++i) PUT(kSpeakerProfile[i].field, kSpeakerProfile[i].value);
    if (!c->feedback) {                                      /* nothing driven onto the data line back to the SoC */
        PUT(Tfa9873Tdmcse, 0);
        PUT(Tfa9873Tdmvse, 0);
    }
    PUT(Tfa9873Pwdn, 1);                                     /* powered down and unconfigured until the clocks run */
    PUT(Tfa9873Ampe, 0);
    PUT(Tfa9873Mansconf, 0);
#undef PUT
    return n;
}

unsigned tfa9873_identify_program(Tfa9873Step *out)
{
    P p = {out, 0};
    read(&p, TFA9873_REG_REV);
    return p.n;
}

unsigned tfa9873_init_program(Tfa9873Step *out, const Tfa9873Config *c)
{
    if (!out || !config_ok(c)) return 0;
    P p = {out, 0};
    write(&p, 0x00, 1u << 1);                                /* I2CR: every register to its reset value (auto clear) */
    add(&p, Tfa9873DelayMs, 0, 0, 0, 1);
    write(&p, 0x0f, 0x5a6b);                                 /* unhide the keys */
    add(&p, Tfa9873Key, 0xfb, 0xa0, 0, 0x005a);              /* key 1: 0xA0 = 0xFB ^ 0x5A */
    write(&p, 0x0f, 0x5a6b);                                 /* key 2 (MTPK); the keys stay unhidden (advanced key handling) */
    write(&p, 0xa1, 0x005a);
    unsigned rows = 0;
    const Reg *t = table(c->rev, &rows);
    for (unsigned i = 0; i < rows; ++i) write(&p, t[i].reg, t[i].value);
    write(&p, TFA9873_REG_IRQ_ENABLE, 0);                    /* interrupts: off, clear, then POR | OTDS | OCPR | UVDS */
    write(&p, TFA9873_REG_IRQ_CLEAR, 0xffff);
    write(&p, TFA9873_REG_IRQ_ENABLE, 0x001d);
    Field f[64];
    const unsigned n = init_fields(c, f, sizeof f / sizeof *f);
    if (n > sizeof f / sizeof *f) return 0;
    for (unsigned i = 0; i < n; ++i) field(&p, f[i].field, f[i].value);
    return p.n < TFA9873_MAX_STEPS ? p.n : 0;
}

/* The registers verify reads, in this order. */
static const uint8_t kVerify[] = {0x00, 0x02, 0x04, 0x20, 0x21, 0x22, 0x23, 0x26, 0x27, 0x5f, 0x62, 0x68, 0x70, 0x74, 0x75, 0x76};

unsigned tfa9873_verify_program(Tfa9873Step *out)
{
    if (!out) return 0;
    P p = {out, 0};
    for (unsigned i = 0; i < sizeof kVerify; ++i) read(&p, kVerify[i]);
    return p.n;
}

int tfa9873_expected_field(const Tfa9873Config *c, uint16_t f)
{
    if (!config_ok(c)) return -1;
    Field list[64];
    const unsigned n = init_fields(c, list, sizeof list / sizeof *list);
    int value = -1;
    for (unsigned i = 0; i < n && i < sizeof list / sizeof *list; ++i) if (list[i].field == f) value = list[i].value;
    return value;
}

int tfa9873_verify(const Tfa9873Config *c, const uint16_t *reads, unsigned count, uint16_t *got, uint16_t *want)
{
    if (!config_ok(c) || !reads || count < sizeof kVerify) return -1;
    Field list[64];
    const unsigned n = init_fields(c, list, sizeof list / sizeof *list);
    for (unsigned r = 0; r < sizeof kVerify; ++r) {
        uint16_t mask = 0, value = 0;
        for (unsigned i = 0; i < n && i < sizeof list / sizeof *list; ++i) {
            if (TFA9873_FIELD_REG(list[i].field) != kVerify[r]) continue;
            const uint16_t m = TFA9873_FIELD_MASK(list[i].field);
            mask |= m;
            value = (uint16_t)((value & ~m) | ((list[i].value << TFA9873_FIELD_POS(list[i].field)) & m));
        }
        if ((reads[r] & mask) != value) {
            if (got) *got = reads[r];
            if (want) *want = value;
            return kVerify[r];
        }
    }
    return 0;
}

unsigned tfa9873_power_up_program(Tfa9873Step *out)
{
    if (!out) return 0;
    P p = {out, 0};
    field(&p, Tfa9873Mansconf, 1);                           /* INIT_CF: leave the wait for the I2C settings, */
    field(&p, Tfa9873Pwdn, 0);                               /* then out of power down; the PLL locks to the bit clock */
    add(&p, Tfa9873Poll, TFA9873_REG_STATUS, 50, TFA9873_CLKS, TFA9873_CLKS);
    field(&p, Tfa9873Ampe, 1);                               /* OPERATING: the amplifier on (no SBSL on a probus device) */
    return p.n;
}

unsigned tfa9873_power_down_program(Tfa9873Step *out)
{
    if (!out) return 0;
    P p = {out, 0};
    field(&p, Tfa9873Pwdn, 1);
    field(&p, Tfa9873Ampe, 0);                               /* after PWDN, as the vendor's power down does */
    return p.n;
}

unsigned tfa9873_status_program(Tfa9873Step *out)
{
    if (!out) return 0;
    P p = {out, 0};
    read(&p, TFA9873_REG_STATUS);
    read(&p, TFA9873_REG_STATUS2);
    read(&p, TFA9873_REG_STATUS3);
    read(&p, TFA9873_REG_MANAGER);
    read(&p, TFA9873_REG_BATTERY);
    read(&p, TFA9873_REG_TEMPERATURE);
    read(&p, TFA9873_REG_IRQ_STATUS);
    write(&p, TFA9873_REG_IRQ_CLEAR, 0xffff);
    return p.n;
}

/* ---- the runner -------------------------------------------------------------------------------------------------------------- */

void tfa9873_run_start(Tfa9873Run *r, const Tfa9873Step *steps, unsigned count)
{
    r->steps = steps; r->count = count; r->index = 0; r->phase = 0; r->pending = 0; r->tries = 0; r->read_count = 0; r->last = 0;
    r->transfers = 0; r->polls_failed = 0;
    for (unsigned i = 0; i < TFA9873_MAX_READS; ++i) r->reads[i] = 0;
}

static int transfer_write(Tfa9873Run *r, Tfa9873Action *a, uint8_t reg, uint16_t value)
{
    a->bytes[0] = reg; a->bytes[1] = (uint8_t)(value >> 8); a->bytes[2] = (uint8_t)value; a->length = 3;
    r->transfers++;
    return Tfa9873Transfer;
}
static int transfer_read(Tfa9873Run *r, Tfa9873Action *a, uint8_t reg)
{
    a->bytes[0] = reg; a->length = 1; a->read_length = 2;
    r->transfers++;
    return Tfa9873Transfer;
}

int tfa9873_run_next(Tfa9873Run *r, Tfa9873Action *a)
{
    a->length = a->read_length = a->delay_ms = 0;
    a->bytes[0] = a->bytes[1] = a->bytes[2] = 0;
    if (r->index >= r->count) return Tfa9873Finished;
    const Tfa9873Step *s = &r->steps[r->index];
    switch (r->phase) {
    case 4:                                                   /* the write half of an update */
        r->phase = 2;
        return transfer_write(r, a, s->reg, r->pending);
    case 6:                                                   /* the write half of a key */
        r->phase = 2;
        return transfer_write(r, a, s->aux, r->pending);
    case 8:                                                   /* a poll read did not match: wait 1 ms, */
        r->phase = 9;
        a->delay_ms = 1;
        return Tfa9873Wait;
    case 9:                                                   /* then read again */
        r->phase = 7;
        return transfer_read(r, a, s->reg);
    default:
        break;
    }
    switch (s->op) {
    case Tfa9873DelayMs:
        a->delay_ms = s->value;
        r->index++;
        return Tfa9873Wait;
    case Tfa9873Write:
        r->phase = 2;
        return transfer_write(r, a, s->reg, s->value);
    case Tfa9873Update:
        r->phase = 1;
        return transfer_read(r, a, s->reg);
    case Tfa9873Read:
        r->phase = 3;
        return transfer_read(r, a, s->reg);
    case Tfa9873Key:
        r->phase = 5;
        return transfer_read(r, a, s->reg);
    case Tfa9873Poll:
        r->phase = 7;
        r->tries = 0;
        return transfer_read(r, a, s->reg);
    default:
        r->index = r->count;
        return Tfa9873Finished;
    }
}

void tfa9873_run_result(Tfa9873Run *r, uint16_t value)
{
    if (r->index >= r->count) return;
    const Tfa9873Step *s = &r->steps[r->index];
    switch (r->phase) {
    case 1:                                                   /* the read of an update: write only what changes */
        r->last = value;
        r->pending = (uint16_t)((value & ~s->mask) | (s->value & s->mask));
        if (r->pending == value) {r->phase = 0; r->index++;}
        else r->phase = 4;
        return;
    case 3:
        r->last = value;
        if (r->read_count < TFA9873_MAX_READS) r->reads[r->read_count++] = value;
        break;
    case 5:
        r->last = value;
        r->pending = (uint16_t)(value ^ s->value);
        r->phase = 6;
        return;
    case 7:
        r->last = value;
        if ((value & s->mask) == s->value) break;
        if (++r->tries < s->aux) {r->phase = 8; return;}
        r->polls_failed++;
        break;
    default:
        break;
    }
    r->phase = 0;
    r->index++;
}
