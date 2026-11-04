/* SPDX-License-Identifier: MIT */
/* Host models of the SM8750 AOP command DB and of the apps RSC DRV-2 for Sm8750Rpmh:
 * cmd-db layout as in Linux cmd-db.c (magic 0x0c0330db, 8 slave headers from +8, 24-byte
 * entries and aux data relative to +144); the RSC with its v2/v3 register layouts, three
 * ACTIVE TCSes, busy TCSes, lost writes, late, partial or missing completion; the touch path
 * sharing the DRV-2 lock; and the host vote policy of ARCHITECTURE 4.3.3. */
#include "../platform/sm8750/Sm8750Rpmh.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);   \
            abort();                                                                \
        }                                                                           \
    } while (0)

/* The touch side as SOC kits/Sm8750Platform/Rpmh.h is to add it (ARCHITECTURE 4.3.4): its own
 * weak definition of the same word. The link must merge it with the one in Sm8750Rpmh.c. */
__attribute__((weak)) volatile uint32_t sm8750_rpmh_drv2_owner;
enum { TouchTag = 0x54434831 };

static int touch_try_exchange(void)
{
    return __atomic_exchange_n(&sm8750_rpmh_drv2_owner, (uint32_t)TouchTag, __ATOMIC_ACQUIRE) == 0;
}

static int touch_try_cas(void)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&sm8750_rpmh_drv2_owner, &expected, (uint32_t)TouchTag, 0, __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
}

static void touch_release(void)
{
    __atomic_store_n(&sm8750_rpmh_drv2_owner, 0, __ATOMIC_RELEASE);
}

/* ---- command DB image -------------------------------------------------------------------- */

typedef struct {
    const char *id;
    uint32_t addr;
    const uint8_t *aux;
    uint16_t aux_len;
} Entry;

typedef struct {
    uint8_t bytes[Sm8750CmdDbBytes];
    unsigned slaves;
    uint32_t cursor;                        /* next free byte after +144 */
    struct {
        char id[9];
        uint32_t at;                        /* byte offset of the 24-byte entry */
    } where[64];
    unsigned nwhere;
    unsigned reads;
    uint32_t magic_after_first_read;        /* nonzero: the magic word changes once it has been read */
} Image;

typedef struct {
    int gfx;                                /* 0 drops gfx.lvl */
    int gmxc;                               /* 0 absent, 1 table, 2 all zero, 3 only the first level */
    int gbx;                                /* 0 absent, 1 table */
    int duplicate_cx;                       /* a second "cx.lvl" in another slave */
    uint8_t vcd[3];                         /* SH0, MC0, ACV */
    uint32_t sh0_unit;
    int k90_bcm;                            /* SH0/MC0 aux as REDMI K90 (SM8750) logged it: unit 4800000, width 64/16 */
} Spec;

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One slave as the AOP lays it out: its entry table at the cursor, its aux area behind it. */
static void image_slave(Image *m, uint16_t slv_id, const Entry *e, unsigned count)
{
    uint8_t *h = m->bytes + 8 + 16 * m->slaves++;
    uint32_t table = m->cursor, data = table + 24 * count, aux = 0;
    put16(h, slv_id);
    put16(h + 2, table);
    put16(h + 4, data);
    put16(h + 6, count);
    put16(h + 8, 0x0100);                   /* version 1.0 */
    for (unsigned n = 0; n < count; n++) {
        uint8_t *p = m->bytes + Sm8750CmdDbDataOffset + table + 24 * n;
        size_t len = strlen(e[n].id);
        CHECK(len >= 1 && len <= 8);
        memcpy(p, e[n].id, len);            /* zero padded, no terminator at 8 bytes */
        put32(p + 8, 0);
        put32(p + 12, 0);
        put32(p + 16, e[n].addr);
        put16(p + 20, e[n].aux_len);
        put16(p + 22, aux);
        if (e[n].aux_len)
            memcpy(m->bytes + Sm8750CmdDbDataOffset + data + aux, e[n].aux, e[n].aux_len);
        aux += (e[n].aux_len + 3u) & ~3u;
        CHECK(m->nwhere < 64);
        memcpy(m->where[m->nwhere].id, e[n].id, len + 1);
        m->where[m->nwhere++].at = (uint32_t)(p - m->bytes);
    }
    m->cursor = (data + aux + 7u) & ~7u;
}

static uint32_t entry_at(const Image *m, const char *id)
{
    for (unsigned i = 0; i < m->nwhere; i++)
        if (!strcmp(m->where[i].id, id))
            return m->where[i].at;
    CHECK(0);
    return 0;
}

static void arc_aux(uint8_t *out, const uint16_t *levels, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        put16(out + 2 * i, levels[i]);
}

static void bcm_aux(uint8_t out[8], uint32_t unit, uint16_t width, uint8_t vcd)
{
    put32(out, unit);
    put16(out + 4, width);
    out[6] = vcd;
    out[7] = 0;
}

static const uint16_t CxLevels[16] = {0x0, 0x10, 0x30, 0x38, 0x40, 0x50, 0x60, 0x80, 0x90, 0xc0, 0xe0, 0x100,
    0x120, 0x140, 0x180, 0x1a0};
static const uint16_t MxLevels[16] = {0x0, 0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1a0};
static const uint16_t GfxLevels[16] = {0x0, 0x10, 0x30, 0x32, 0x34, 0x38, 0x3c, 0x40, 0x50, 0x80, 0x90, 0xc0,
    0xe0, 0x100};
static const uint16_t GmxcLevels[16] = {0x0, 0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180};
static const uint16_t GbxLevels[16] = {0x0, 0x10, 0x30, 0x40, 0x80};
static const uint16_t OnlyFirst[16] = {0x10};
static const uint16_t Zeros[16] = {0};
static const uint16_t BigLevels[17] = {0x10, 0x30, 0x38, 0x40, 0x50, 0x60, 0x80, 0x90, 0xc0, 0xe0, 0x100, 0x120,
    0x140, 0x180, 0x1a0, 0x1c0, 0x1d0};
static const uint16_t PaddedLevels[20] = {0x0, 0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1a0, 0x1c0, 0x1d0};
/* KGSL stops only at a zero that follows a nonzero level: leading zeros are counted. */
static const uint16_t GapLevels[16] = {0x0, 0x0, 0x30, 0x40, 0x0, 0x80};

static void build(Image *m, const Spec *s)
{
    static uint8_t cx[32], mx[32], gfx[32], gmxc[32], gbx[32], big[34], padded[40], lcx[32], gap[32];
    static uint8_t acv[8], mc0[8], sh0[8], cn0[8], mm0[8], late[8];
    memset(m, 0, sizeof(*m));
    put32(m->bytes, 0x00030001);            /* version word, not checked */
    put32(m->bytes + 4, Sm8750CmdDbMagic);
    arc_aux(cx, CxLevels, 16);
    arc_aux(mx, MxLevels, 16);
    arc_aux(gfx, GfxLevels, 16);
    arc_aux(gmxc, s->gmxc == 1 ? GmxcLevels : s->gmxc == 3 ? OnlyFirst : Zeros, 16);
    arc_aux(gbx, GbxLevels, 16);
    arc_aux(big, BigLevels, 17);
    arc_aux(padded, PaddedLevels, 20);
    arc_aux(lcx, GbxLevels, 16);
    arc_aux(gap, GapLevels, 16);
    bcm_aux(acv, 1, 1, s->vcd[2]);
    bcm_aux(mc0, s->k90_bcm ? 4800000 : 1000000, s->k90_bcm ? 16 : 4, s->vcd[1]);
    bcm_aux(sh0, s->k90_bcm ? 4800000 : s->sh0_unit, s->k90_bcm ? 64 : 32, s->vcd[0]);
    bcm_aux(cn0, 1000, 4, 2);
    bcm_aux(mm0, 1000, 32, 3);

    /* Slave order on purpose: BCM, ARC, an unknown type, VRM, then the terminator. A duplicate
     * "cx.lvl" sits in the BCM slave, ahead of the real one, so a last-match parser would pass. */
    const Entry bcm[] = {{"ACV", 0x50000, acv, 8}, {"MC0", 0x50004, mc0, 8}, {"SH0", 0x50008, sh0, 8},
        {"CN0", 0x5000c, cn0, 8}, {"MM0", 0x50010, mm0, 8}, {"cx.lvl", 0x50014, mm0, 8}};
    image_slave(m, Sm8750CmdDbSlaveBcm, bcm, s->duplicate_cx ? 6 : 5);

    Entry arc[12];
    unsigned n = 0;
    arc[n++] = (Entry){"cx.lvl", 0x30000, cx, 32};
    arc[n++] = (Entry){"mx.lvl", 0x30010, mx, 32};
    if (s->gfx)
        arc[n++] = (Entry){"gfx.lvl", 0x30020, gfx, 32};
    if (s->gmxc)
        arc[n++] = (Entry){"gmxc.lvl", 0x30030, gmxc, 32};
    if (s->gbx)
        arc[n++] = (Entry){"gbx.lvl", 0x30080, gbx, 32};
    arc[n++] = (Entry){"lcx.lvl", 0x30040, lcx, 32};
    arc[n++] = (Entry){"lmx.lvl", 0x30050, lcx, 32};
    arc[n++] = (Entry){"big.lvl", 0x30060, big, 34};
    arc[n++] = (Entry){"pad.lvl", 0x30070, padded, 40};
    arc[n++] = (Entry){"gap.lvl", 0x300a0, gap, 32};
    image_slave(m, Sm8750CmdDbSlaveArc, arc, n);

    const Entry other[] = {{"pdc.seq", 0x60010, NULL, 0}};
    image_slave(m, 6, other, 1);

    const Entry vrm[] = {{"ldob4", 0x40800, NULL, 0}, {"ldob16", 0x41900, NULL, 0}, {"smpb1", 0x40100, NULL, 0}};
    image_slave(m, Sm8750CmdDbSlaveVrm, vrm, 3);

    /* A slave behind the terminating slv_id 0 is never searched (cmd_db_get_header). */
    m->slaves++;
    const Entry after[] = {{"late.lvl", 0x30090, late, 8}};
    image_slave(m, Sm8750CmdDbSlaveArc, after, 1);
}

static uint32_t image_read(void *context, uint32_t offset)
{
    Image *m = context;
    CHECK(!(offset & 3) && offset < Sm8750CmdDbBytes);
    m->reads++;
    uint32_t value = get32(m->bytes + offset);
    if (offset == 4 && m->magic_after_first_read) {
        put32(m->bytes + 4, m->magic_after_first_read);
        m->magic_after_first_read = 0;
    }
    return value;
}

static void image_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    (void)offset;
    (void)value;
    CHECK(0);                               /* cmd-db is never written */
}

static void image_delay(void *context, unsigned microseconds)
{
    (void)context;
    (void)microseconds;
}

static Image image;
static Sm8750CmdDb db;

static void load(const Spec *s)
{
    build(&image, s);
    Sm8750RpmhIo io = {&image, image_read, image_write, image_delay};
    CHECK(Sm8750CmdDbCopy(&io, &db) == Sm8750RpmhOk);
}

static const Spec Default = {1, 1, 0, 0, {1, 0, 0}, 1000000, 0};

/* The DT DDR table (qcom,bus-table-ddr, kBps), ARCHITECTURE 4.1.4. */
static const uint32_t DdrKbps[12] = {0, 781250, 2136718, 5285156, 6074218, 6671875, 8171875, 10687500,
    12449218, 14398437, 16500000, 18597656};

/* 128 KiB snapshot: static storage here as in the driver (ARCHITECTURE 4.1.1). The header refuses
 * to compile if the type ever shrinks to something a 16 KiB kernel stack could hold. */
_Static_assert(sizeof(Sm8750CmdDb) >= Sm8750CmdDbBytes && Sm8750CmdDbBytes > 4 * Sm8750RpmhKernelStackBytes,
    "the cmd-db snapshot is never a stack object");

/* ---- RSC DRV-2 model --------------------------------------------------------------------- */

typedef struct {
    unsigned slot, n;
    uint32_t wait, enable;
    uint32_t msgid[16], addr[16], data[16];
} Sent;

typedef struct {
    uint32_t regs[0x10000 / 4];
    uint32_t wait, control, status, enable, msg, stride;
    uint32_t ignored;                       /* writes to this register are lost */
    uint32_t lost_after_trigger;            /* writes to this register are lost once a TCS was triggered */
    uint32_t lost_value;                    /* ... only writes of this value (~0u: any value) */
    int triggered;
    long complete_ms;                       /* completion this long after the trigger; < 0 never */
    long status_lag_ms;                     /* STATUS turns idle this long after every CMD_STATUS completed */
    uint64_t idle_at;
    int partial;                            /* last command never gets COMPL */
    int garbage;                            /* CMD_STATUS reads 0xffffffff after completion */
    unsigned pending;
    uint64_t now, triggered_at, touch_release_at;
    uint32_t irq_poke;                      /* another IRQ_ENABLE bit set while a TCS runs */
    unsigned reads, writes, nsent, nlog;
    Sent sent[32];
    struct {
        uint32_t reg, value;
    } log[256];
} Rsc;

static uint32_t tcs(unsigned slot)
{
    return Sm8750RpmhTcsBase + slot * Sm8750RpmhTcsStride;
}

static uint32_t rsc_read(void *context, uint32_t reg)
{
    Rsc *m = context;
    CHECK(!(reg & 3) && reg < 0x10000);
    CHECK(Sm8750RpmhDrv2Owner() == Sm8750RpmhLockOwnerGpu);    /* DRV-2 is only touched under the lock */
    m->reads++;
    return m->regs[reg / 4];
}

static void rsc_write(void *context, uint32_t reg, uint32_t value)
{
    Rsc *m = context;
    CHECK(!(reg & 3) && reg < 0x10000);
    CHECK(Sm8750RpmhDrv2Owner() == Sm8750RpmhLockOwnerGpu);
    CHECK(reg < tcs(Sm8750RpmhActiveTcs) || reg >= tcs(8));    /* sleep, wake and fast-path TCSes: never */
    CHECK(reg != Sm8750RpmhTcsBase + 4);                        /* IRQ_STATUS is read-only: cleared via IRQ_CLEAR */
    m->writes++;
    if (m->nlog < 256) {
        m->log[m->nlog].reg = reg;
        m->log[m->nlog++].value = value;
    }
    int lost = m->triggered && reg == m->lost_after_trigger && (m->lost_value == ~0u || value == m->lost_value);
    if (reg == m->ignored || lost)
        return;
    if (reg == Sm8750RpmhTcsBase + 8) {     /* IRQ_CLEAR */
        m->regs[(Sm8750RpmhTcsBase + 4) / 4] &= ~value;
        return;
    }
    m->regs[reg / 4] = value;
    for (unsigned s = 0; s < 8; s++) {
        uint32_t b = tcs(s);
        if (reg != b + m->control || !(value & Sm8750RpmhAmcTrigger))
            continue;
        CHECK(s < Sm8750RpmhActiveTcs && value == (Sm8750RpmhAmcEnable | Sm8750RpmhAmcTrigger));
        CHECK(!(m->regs[Sm8750RpmhTcsBase / 4] & (1u << s)));      /* its IRQ is masked while it runs */
        CHECK(m->nsent < 32);
        m->triggered = 1;
        Sent *t = &m->sent[m->nsent++];
        t->slot = s;
        t->wait = m->regs[(b + m->wait) / 4];
        t->enable = m->regs[(b + m->enable) / 4];
        CHECK(t->enable && !(t->wait & ~t->enable));
        for (unsigned i = 0; i < 16 && (t->enable >> i); i++) {
            uint32_t at = b + m->msg + i * m->stride;
            t->msgid[i] = m->regs[at / 4];
            t->addr[i] = m->regs[(at + 4) / 4];
            t->data[i] = m->regs[(at + 8) / 4];
            m->regs[(at + 12) / 4] = 0;
            t->n = i + 1;
        }
        m->regs[(b + m->status) / 4] = 0;
        m->pending = s;
        m->triggered_at = m->now;
    }
}

static void rsc_delay(void *context, unsigned microseconds)
{
    Rsc *m = context;
    m->now += microseconds;
    if (m->touch_release_at && m->now >= m->touch_release_at) {
        touch_release();
        m->touch_release_at = 0;
    }
    if (m->pending == ~0u)
        return;
    if (m->irq_poke) {
        m->regs[Sm8750RpmhTcsBase / 4] |= m->irq_poke;
        m->irq_poke = 0;
    }
    if (m->complete_ms < 0 || m->now < m->triggered_at + (uint64_t)m->complete_ms * 1000)
        return;
    uint32_t b = tcs(m->pending), enabled = m->regs[(b + m->enable) / 4];
    if (!m->idle_at) {                                      /* the commands complete now, the TCS a bit later */
        for (unsigned i = 0; i < 16; i++) {
            if (!(enabled & (1u << i)))
                continue;
            int last = !(enabled >> (i + 1));
            uint32_t v = m->partial && last ? 0x100 : 0x10100;
            m->regs[(b + m->msg + 12 + i * m->stride) / 4] = m->garbage ? 0xffffffffu : v;
        }
        m->idle_at = m->now + (uint64_t)m->status_lag_ms * 1000;
    }
    if (m->now < m->idle_at)
        return;
    m->regs[(b + m->status) / 4] = 1;
    m->regs[(Sm8750RpmhTcsBase + 4) / 4] |= 1u << m->pending;
    m->pending = ~0u;
    m->idle_at = 0;
}

static Rsc *rsc_new(unsigned major, unsigned ncmd)
{
    Rsc *m = calloc(1, sizeof(*m));
    CHECK(m);
    m->wait = major == 2 ? 0x10 : 0x20;
    m->control = m->wait + 4;
    m->status = m->wait + 8;
    m->enable = m->wait + 12;
    m->msg = major == 2 ? 0x30 : 0x34;
    m->stride = major == 2 ? 20 : 24;
    m->regs[0] = major << 16 | 7u << 8;
    /* DRV_PRNT_CHLD_CONFIG: DRV0 2 TCS, DRV1 3, DRV2 8 (3 active, 2 sleep, 2 wake, 1 fast path) */
    m->regs[0x0c / 4] = 2u | 3u << 6 | 8u << 12 | (uint32_t)ncmd << 27;
    m->regs[Sm8750RpmhTcsBase / 4] = 0x87;
    for (unsigned s = 0; s < 8; s++)
        m->regs[(tcs(s) + m->status) / 4] = 1;
    for (unsigned s = 3; s < 7; s++)                        /* sleep and wake TCSes hold their votes */
        m->regs[(tcs(s) + m->enable) / 4] = 0x3;
    m->complete_ms = 1;
    m->pending = ~0u;
    m->lost_value = ~0u;
    return m;
}

static uint32_t reg_of(const Rsc *m, uint32_t reg)
{
    return m->regs[reg / 4];
}

static Sm8750RpmhIo rsc_io(Rsc *m)
{
    Sm8750RpmhIo io = {m, rsc_read, rsc_write, rsc_delay};
    return io;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_cmd_db_copy(void)
{
    build(&image, &Default);
    Sm8750RpmhIo io = {&image, image_read, image_write, image_delay};
    CHECK(Sm8750CmdDbCopy(&io, &db) == Sm8750RpmhOk);
    CHECK(db.bytes == Sm8750CmdDbBytes && image.reads == 1 + Sm8750CmdDbBytes / 4);
    CHECK(!memcmp(db.snapshot, image.bytes, Sm8750CmdDbBytes));

    image.reads = 0;
    put32(image.bytes + 4, 0x0c0330dc);
    CHECK(Sm8750CmdDbCopy(&io, &db) == Sm8750RpmhNotFound && image.reads == 1 && db.bytes == 0);
    CHECK(Sm8750CmdDbCopy(NULL, &db) == Sm8750RpmhArgument && Sm8750CmdDbCopy(&io, NULL) == Sm8750RpmhArgument);

    /* The magic checked first is not the one copied: the snapshot is refused and stays unusable. */
    build(&image, &Default);
    image.magic_after_first_read = 0x0c0330dc;
    CHECK(Sm8750CmdDbCopy(&io, &db) == Sm8750RpmhCorrupt && db.bytes == 0);
    Sm8750RpmhResources res;
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhArgument);
}

static void test_cmd_db_lookup(void)
{
    uint32_t addr, unit;
    uint16_t vlvl[16], width;
    unsigned count;
    uint8_t vcd;
    load(&Default);

    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", &addr, vlvl, &count) == Sm8750RpmhOk);
    CHECK(addr == 0x30000 && count == 16 && !memcmp(vlvl, CxLevels, sizeof(vlvl)));
    CHECK(Sm8750CmdDbArc(&db, "mx.lvl", &addr, vlvl, &count) == Sm8750RpmhOk);
    CHECK(addr == 0x30010 && count == 10 && vlvl[9] == 0x1a0 && vlvl[10] == 0);
    CHECK(Sm8750CmdDbArc(&db, "gfx.lvl", &addr, vlvl, &count) == Sm8750RpmhOk && addr == 0x30020 && count == 14);
    CHECK(Sm8750CmdDbArc(&db, "gmxc.lvl", &addr, vlvl, &count) == Sm8750RpmhOk && addr == 0x30030 && count == 9);
    CHECK(Sm8750CmdDbArc(&db, "gbx.lvl", &addr, vlvl, &count) == Sm8750RpmhNotFound);
    CHECK(Sm8750CmdDbArc(&db, "pad.lvl", &addr, vlvl, &count) == Sm8750RpmhOk && count == 12);   /* 20 entries */
    CHECK(Sm8750CmdDbArc(&db, "gap.lvl", &addr, vlvl, &count) == Sm8750RpmhOk && count == 4 && vlvl[5] == 0x80);
    CHECK(Sm8750CmdDbArc(&db, "big.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);             /* 17 levels */
    CHECK(Sm8750CmdDbArc(&db, "late.lvl", &addr, vlvl, &count) == Sm8750RpmhNotFound);

    CHECK(Sm8750CmdDbBcm(&db, "SH0", &addr, &unit, &width, &vcd) == Sm8750RpmhOk);
    CHECK(addr == 0x50008 && unit == 1000000 && width == 32 && vcd == 1);
    CHECK(Sm8750CmdDbBcm(&db, "MC0", &addr, &unit, &width, &vcd) == Sm8750RpmhOk);
    CHECK(addr == 0x50004 && unit == 1000000 && width == 4 && vcd == 0);
    CHECK(Sm8750CmdDbBcm(&db, "ACV", &addr, &unit, &width, &vcd) == Sm8750RpmhOk && addr == 0x50000);
    CHECK(Sm8750CmdDbBcm(&db, "CN0", &addr, &unit, &width, &vcd) == Sm8750RpmhOk && addr == 0x5000c && vcd == 2);

    CHECK(Sm8750CmdDbVrm(&db, "ldob4", &addr) == Sm8750RpmhOk && addr == 0x40800);
    CHECK(Sm8750CmdDbVrm(&db, "ldob16", &addr) == Sm8750RpmhOk && addr == 0x41900);

    /* Type is checked against the slave and the address; names are 1..8 bytes. */
    CHECK(Sm8750CmdDbArc(&db, "SH0", &addr, vlvl, &count) == Sm8750RpmhCorrupt);
    CHECK(Sm8750CmdDbBcm(&db, "cx.lvl", &addr, &unit, &width, &vcd) == Sm8750RpmhCorrupt);
    CHECK(Sm8750CmdDbVrm(&db, "MC0", &addr) == Sm8750RpmhCorrupt);
    CHECK(Sm8750CmdDbArc(&db, "cx.lv", &addr, vlvl, &count) == Sm8750RpmhNotFound);
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl.x9", &addr, vlvl, &count) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbArc(&db, "", &addr, vlvl, &count) == Sm8750RpmhArgument);

    Sm8750CmdDbEntry e;
    CHECK(Sm8750CmdDbFind(&db, "pdc.seq", 6, &e) == Sm8750RpmhOk && e.addr == 0x60010 && e.slave == 6);
    CHECK(Sm8750CmdDbFind(&db, "cx.lvl", 0, &e) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbFind(&db, "cx.lvl", 8, &e) == Sm8750RpmhArgument);
    uint32_t arc_data = (uint32_t)image.bytes[8 + 16 + 4] | (uint32_t)image.bytes[8 + 16 + 5] << 8;
    CHECK(Sm8750CmdDbFind(&db, "cx.lvl", 3, &e) == Sm8750RpmhOk && e.aux_len == 32 && e.slave == 3);
    CHECK(e.aux_offset == Sm8750CmdDbDataOffset + arc_data);    /* first aux of the ARC slave */
    CHECK(db.snapshot[e.aux_offset + 2 * 15] == 0xa0 && db.snapshot[e.aux_offset + 2 * 15 + 1] == 0x01);

    /* Dummy gmxc: all zero, or nothing after the first level (KGSL treats both as no gMxC). */
    for (int kind = 2; kind <= 3; kind++) {
        Spec s = Default;
        s.gmxc = kind;
        load(&s);
        CHECK(Sm8750CmdDbArc(&db, "gmxc.lvl", &addr, vlvl, &count) == Sm8750RpmhNoData && count == 0);
        CHECK(addr == 0x30030 && vlvl[0] == (kind == 3 ? 0x10 : 0));
    }
}

static void test_cmd_db_corrupt(void)
{
    uint32_t addr;
    uint16_t vlvl[16];
    unsigned count;
    Sm8750CmdDbEntry e;

    Spec dup = Default;
    dup.duplicate_cx = 1;
    load(&dup);
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);
    CHECK(Sm8750CmdDbArc(&db, "mx.lvl", &addr, vlvl, &count) == Sm8750RpmhOk);

    load(&Default);
    uint32_t mx = entry_at(&image, "mx.lvl");
    put32(db.snapshot + mx + 16, 0x50010);                  /* slave bits disagree with the ARC slave */
    CHECK(Sm8750CmdDbArc(&db, "mx.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);
    put32(db.snapshot + mx + 16, 0x30012);                  /* not 4-byte aligned */
    CHECK(Sm8750CmdDbArc(&db, "mx.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);
    put32(db.snapshot + mx + 16, 0x130010);                 /* bits above the slave field */
    CHECK(Sm8750CmdDbArc(&db, "mx.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);

    load(&Default);
    put32(db.snapshot + entry_at(&image, "ldob4") + 16, 0x40804);
    CHECK(Sm8750CmdDbVrm(&db, "ldob4", &addr) == Sm8750RpmhCorrupt);

    load(&Default);                                         /* aux beyond the region */
    put16(db.snapshot + 8 + 16 * 1 + 4, 0xffff);            /* ARC slave data_offset */
    put16(db.snapshot + entry_at(&image, "cx.lvl") + 22, 0xffff);
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", &addr, vlvl, &count) == Sm8750RpmhCorrupt);

    load(&Default);                                         /* entry table beyond the region */
    put16(db.snapshot + 8 + 16 * 2 + 6, 0xffff);
    CHECK(Sm8750CmdDbVrm(&db, "ldob4", &addr) == Sm8750RpmhCorrupt);

    /* BCM aux shorter than 8 bytes: Corrupt for the lookup and for SH0/MC0/ACV, whose unit, width and
     * vcd the votes use. CN0 needs only its address (constant votes); the author's PKX110-qualified
     * parser accepted a CN0 without aux, so its aux is optional. */
    uint32_t unit;
    uint16_t width;
    uint8_t vcd;
    Sm8750RpmhResources res;
    for (uint16_t len = 0; len < 8; len += 4) {
        load(&Default);
        put16(db.snapshot + entry_at(&image, "CN0") + 20, len);
        CHECK(Sm8750CmdDbBcm(&db, "CN0", &addr, &unit, &width, &vcd) == Sm8750RpmhCorrupt);
        CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhOk && res.failed == Sm8750RpmhResNone);
        CHECK(res.cn0.present && res.cn0.addr == 0x5000c && res.cn0.aux_len == len);
        CHECK(!res.cn0.unit && !res.cn0.width && !res.cn0.vcd && res.acv.aux_len == 8);
        Sm8750RpmhRequest r;
        CHECK(Sm8750RpmhHostCn0Request(&res, &r) == Sm8750RpmhOk);
        CHECK(r.cmd[0].addr == 0x5000c && r.cmd[0].data == 0x60000001);
    }
    static const struct {
        const char *name;
        uint32_t failed;
    } short_aux[] = {{"SH0", Sm8750RpmhResSh0}, {"MC0", Sm8750RpmhResMc0}, {"ACV", Sm8750RpmhResAcv}};
    for (unsigned i = 0; i < 3; i++) {
        load(&Default);
        put16(db.snapshot + entry_at(&image, short_aux[i].name) + 20, 7);
        CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhCorrupt && res.failed == short_aux[i].failed);
    }

    /* SH0/MC0 scale by the width and divide by the unit: neither may be 0. */
    for (unsigned field = 0; field < 2; field++) {
        load(&Default);
        CHECK(Sm8750CmdDbFind(&db, "MC0", Sm8750CmdDbSlaveBcm, &e) == Sm8750RpmhOk);
        if (field)
            put16(db.snapshot + e.aux_offset + 4, 0);
        else
            put32(db.snapshot + e.aux_offset, 0);
        CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhCorrupt && res.failed == Sm8750RpmhResMc0);
        CHECK(res.sh0.present && res.mc0.addr == 0x50004);  /* what was parsed is kept for the status page */
    }

    /* Two names, one address: malformed. The later name is reported. */
    static const struct {
        const char *name, *other;
        uint32_t failed;
    } same[] = {{"cx.lvl", "mx.lvl", Sm8750RpmhResCx}, {"gmxc.lvl", "gfx.lvl", Sm8750RpmhResGmxc},
        {"MC0", "SH0", Sm8750RpmhResMc0}, {"CN0", "ACV", Sm8750RpmhResCn0}};
    for (unsigned i = 0; i < 4; i++) {
        load(&Default);
        uint32_t other = get32(db.snapshot + entry_at(&image, same[i].other) + 16);
        put32(db.snapshot + entry_at(&image, same[i].name) + 16, other);
        CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhCorrupt && res.failed == same[i].failed);
    }
    Spec dummy = Default;                                   /* a dummy gmxc is absent: its address does not count */
    dummy.gmxc = 2;
    dummy.gbx = 1;
    load(&dummy);
    put32(db.snapshot + entry_at(&image, "gmxc.lvl") + 16, 0x30080);   /* = gbx.lvl */
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhOk && !res.gmxc.present && res.gbx.present);

    load(&Default);
    put32(db.snapshot + 4, 0);
    CHECK(Sm8750CmdDbFind(&db, "cx.lvl", 3, &e) == Sm8750RpmhNotFound);
    db.bytes = 0;                                           /* never copied */
    CHECK(Sm8750CmdDbFind(&db, "cx.lvl", 3, &e) == Sm8750RpmhArgument);
}

static void test_resources(void)
{
    Sm8750RpmhResources res;
    load(&Default);
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhOk && res.failed == Sm8750RpmhResNone);
    CHECK(res.version == 0x00030001);
    CHECK(res.gfx.present && res.gfx.count == 14 && res.gfx.addr == 0x30020 && res.gfx.aux_len == 32);
    CHECK(res.mx.present && res.mx.count == 10 && res.cx.present && res.cx.count == 16);
    CHECK(res.gmxc.present && res.gmxc.count == 9 && !res.gbx.present && !res.gbx.count);
    CHECK(res.sh0.present && res.sh0.unit == 1000000 && res.sh0.width == 32 && res.sh0.vcd == 1);
    CHECK(res.mc0.present && res.acv.present && res.cn0.present && res.cn0.addr == 0x5000c);

    Spec s = Default;
    s.gmxc = 2;
    s.gbx = 1;
    load(&s);
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhOk);
    CHECK(!res.gmxc.present && !res.gmxc.count && res.gmxc.addr == 0x30030 && res.gmxc.aux_len == 32);
    CHECK(res.gbx.present && res.gbx.count == 5);

    s = Default;
    s.gmxc = 0;
    load(&s);
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhOk && !res.gmxc.present && !res.gmxc.addr);

    s = Default;
    s.gfx = 0;
    load(&s);
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhNotFound && res.failed == Sm8750RpmhResGfx);

    s = Default;
    s.sh0_unit = 0;
    load(&s);
    CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhCorrupt && res.failed == Sm8750RpmhResSh0);
    CHECK(res.cx.present && res.mx.present);                /* what was parsed is kept for the status page */

    static const struct {
        const char *name;
        uint32_t failed;
    } required[] = {{"mx.lvl", Sm8750RpmhResMx}, {"cx.lvl", Sm8750RpmhResCx}, {"SH0", Sm8750RpmhResSh0},
        {"MC0", Sm8750RpmhResMc0}, {"ACV", Sm8750RpmhResAcv}, {"CN0", Sm8750RpmhResCn0}};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        load(&Default);
        db.snapshot[entry_at(&image, required[i].name)] ^= 0x20;    /* renamed: the resource is missing */
        CHECK(Sm8750RpmhResourcesParse(&db, &res) == Sm8750RpmhNotFound && res.failed == required[i].failed);
    }
}

static void test_levels(void)
{
    Sm8750RpmhResources res;
    uint32_t h;
    load(&Default);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    CHECK(Sm8750RpmhArcLevel(&res.cx, Sm8750RpmhLowSvs, &h) == Sm8750RpmhOk && h == 4);
    CHECK(Sm8750RpmhArcLevel(&res.mx, Sm8750RpmhLowSvs, &h) == Sm8750RpmhOk && h == 3);
    CHECK(Sm8750RpmhArcLevel(&res.cx, 0x41, &h) == Sm8750RpmhOk && h == 5);    /* first vlvl >= request */
    CHECK(Sm8750RpmhArcLevel(&res.cx, 1, &h) == Sm8750RpmhOk && h == 1);       /* lowest nonzero */
    CHECK(Sm8750RpmhArcLevel(&res.cx, 0, &h) == Sm8750RpmhOk && h == 0);
    CHECK(Sm8750RpmhArcLevel(&res.mx, 0x1a1, &h) == Sm8750RpmhRange);
    CHECK(Sm8750RpmhArcLevel(&res.gfx, 0x100, &h) == Sm8750RpmhOk && h == 13);
    CHECK(Sm8750RpmhArcTop(&res.cx, &h) == Sm8750RpmhOk && h == 15);
    CHECK(Sm8750RpmhArcTop(&res.mx, &h) == Sm8750RpmhOk && h == 9);
    CHECK(Sm8750RpmhArcLevel(&res.gbx, 0x40, &h) == Sm8750RpmhArgument);      /* absent */

    Sm8750RpmhArc dummy = res.mx;
    dummy.present = 0;
    CHECK(Sm8750RpmhArcTop(&dummy, &h) == Sm8750RpmhArgument);
    Sm8750RpmhArc zero = res.mx;                            /* usable count but no nonzero level: nothing to vote */
    memset(zero.vlvl, 0, sizeof(zero.vlvl));
    CHECK(Sm8750RpmhArcTop(&zero, &h) == Sm8750RpmhRange && Sm8750RpmhArcLevel(&zero, 1, &h) == Sm8750RpmhRange);
    zero.count = Sm8750RpmhArcMaxLevels + 1;
    CHECK(Sm8750RpmhArcLevel(&zero, 0, &h) == Sm8750RpmhArgument && Sm8750RpmhArcTop(NULL, &h) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhArcLevel(&res.cx, 0x40, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhArcTop(&res.cx, NULL) == Sm8750RpmhArgument);

    /* Lookups refuse missing outputs before reading anything. */
    uint32_t addr, unit;
    uint16_t vlvl[16], width;
    unsigned count;
    uint8_t vcd;
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", NULL, vlvl, &count) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", &addr, NULL, &count) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbArc(&db, "cx.lvl", &addr, vlvl, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbArc(NULL, "cx.lvl", &addr, vlvl, &count) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbArc(&db, NULL, &addr, vlvl, &count) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbBcm(&db, "SH0", NULL, &unit, &width, &vcd) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbBcm(&db, "SH0", &addr, &unit, &width, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750CmdDbVrm(&db, "ldob4", NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhResourcesParse(NULL, &res) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhResourcesParse(&db, NULL) == Sm8750RpmhArgument);
}

static void test_encoding(void)
{
    Sm8750RpmhResources res;
    Sm8750RpmhRequest r;
    CHECK(Sm8750BcmCommand(1, 1, 0, 1) == (uint32_t)Sm8750RpmhCn0On);
    CHECK(Sm8750BcmCommand(1, 0, 0, 0) == (uint32_t)Sm8750RpmhCn0Off);
    CHECK(Sm8750BcmCommand(0, 1, 0x7fff, 0x4001) == (0x20000000u | 0x3fffu << 14 | 1));
    CHECK(Sm8750BcmCommand(5, 7, 2, 3) == (0x60000000u | 2u << 14 | 3));

    load(&Default);                                         /* vcd SH0 1, MC0 0, ACV 0 */
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    /* GMU bandwidth table rows (Linux a8xx): valid 0 at 0 kBps, ACV BIT(2) from 10687500 kBps. */
    static const struct {
        uint32_t kbps, sh0, mc0, acv;
    } gmu[] = {
        {0, 0x40000000, 0x00000000, 0x40000000},            /* level 0: valid 0 */
        {1, 0x60004001, 0x20004001, 0x60000000},            /* rounds down to 0, clamped to 1 */
        {781250, 0x6186861a, 0x20c3430d, 0x60000000},
        {2136718, 0x642c50b1, 0x22160858, 0x60000000},      /* level 2 */
        {5285156, 0x6a52a94a, 0x252954a5, 0x60000000},      /* level 3 */
        {8171875, 0x6ff5ffd7, 0x27fadfeb, 0x60000000},      /* level 6 */
        {10687500, 0x6fffffff, 0x2a6fe9bf, 0x60000004},     /* level 7: GPU perfmode from here */
        {18597656, 0x6fffffff, 0x2fffffff, 0x60000004},     /* level 11, both clamped */
    };
    /* Host votes (Linux HLOS): SH0/MC0 keepalive (x = y >= 1, never valid 0), ACV BIT(0). */
    static const struct {
        uint32_t kbps, sh0, mc0, acv;
    } host[] = {
        {0, 0x60004001, 0x20004001, 0x40000000},            /* floor 0: keepalive minimum, ACV off */
        {1, 0x60004001, 0x20004001, 0x60000001},
        {2136718, 0x642c50b1, 0x22160858, 0x60000001},      /* pagani-gpu-ddr=2 */
        {5285156, 0x6a52a94a, 0x252954a5, 0x60000001},      /* pagani-gpu-ddr-floor=3 */
        {10687500, 0x6fffffff, 0x2a6fe9bf, 0x60000001},     /* never BIT(2) */
        {18597656, 0x6fffffff, 0x2fffffff, 0x60000001},     /* default: level 11, the author's bandwidth */
    };
    for (unsigned i = 0; i < sizeof(gmu) / sizeof(gmu[0]); i++) {
        CHECK(Sm8750RpmhGmuBwRequest(&res, gmu[i].kbps, &r) == Sm8750RpmhOk);
        CHECK(r.count == 3 && r.wait_mask == 5);
        CHECK(r.cmd[0].addr == 0x50008 && r.cmd[1].addr == 0x50004 && r.cmd[2].addr == 0x50000);
        CHECK(r.cmd[0].data == gmu[i].sh0 && r.cmd[1].data == gmu[i].mc0 && r.cmd[2].data == gmu[i].acv);
    }
    for (unsigned i = 0; i < sizeof(host) / sizeof(host[0]); i++) {
        CHECK(Sm8750RpmhHostDdrRequest(&res, host[i].kbps, &r) == Sm8750RpmhOk);
        CHECK(r.count == 3 && r.wait_mask == 5);
        CHECK(r.cmd[0].addr == 0x50008 && r.cmd[1].addr == 0x50004 && r.cmd[2].addr == 0x50000);
        CHECK(r.cmd[0].data == host[i].sh0 && r.cmd[1].data == host[i].mc0 && r.cmd[2].data == host[i].acv);
    }
    /* Over the whole DT table and around the ACV threshold, the two encodings keep their rules. */
    for (uint32_t i = 0; i < 12 + 4; i++) {
        uint32_t kbps = i < 12 ? DdrKbps[i] : (uint32_t)Sm8750RpmhAcvPerfmodeKbps - 2 + (i - 12);
        Sm8750RpmhRequest g;
        CHECK(!Sm8750RpmhGmuBwRequest(&res, kbps, &g) && !Sm8750RpmhHostDdrRequest(&res, kbps, &r));
        for (unsigned b = 0; b < 2; b++) {
            CHECK((r.cmd[b].data & 1u << 29) && ((r.cmd[b].data >> 14) & 0x3fff) && (r.cmd[b].data & 0x3fff));
            CHECK(kbps ? g.cmd[b].data == r.cmd[b].data : !(g.cmd[b].data & 1u << 29));
        }
        CHECK(r.cmd[2].data == (kbps ? 0x60000001u : 0x40000000u));
        CHECK(g.cmd[2].data == (!kbps ? 0x40000000u : kbps >= 10687500 ? 0x60000004u : 0x60000000u));
    }

    /* SH0/MC0 aux as REDMI K90 (SM8750) logged it: unit 4800000, width 64 and 16. No DT level clamps
     * there, so each row checks the arithmetic itself (words computed outside this code). */
    Spec k90 = Default;
    k90.k90_bcm = 1;
    load(&k90);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res) && res.sh0.unit == 4800000 && res.mc0.width == 16);
    static const uint32_t k90_rows[12][2] = {
        {0, 0}, {0x60a2c28b, 0x20a2c28b}, {0x61bd06f4, 0x21bd06f4}, {0x644d1134, 0x244d1134},
        {0x64f153c5, 0x24f153c5}, {0x656dd5b7, 0x256dd5b7}, {0x66a65a99, 0x26a65a99}, {0x68b2a2ca, 0x28b2a2ca},
        {0x6a21a886, 0x2a21a886}, {0x6bb7aede, 0x2bb7aede}, {0x6d6db5b6, 0x2d6db5b6}, {0x6f22bc8a, 0x2f22bc8a},
    };
    for (unsigned level = 0; level < 12; level++) {
        Sm8750RpmhRequest g;
        CHECK(!Sm8750RpmhGmuBwRequest(&res, DdrKbps[level], &g) && !Sm8750RpmhHostDdrRequest(&res, DdrKbps[level], &r));
        if (!level) {
            CHECK(g.cmd[0].data == 0x40000000 && g.cmd[1].data == 0 && g.cmd[2].data == 0x40000000);
            CHECK(r.cmd[0].data == 0x60004001 && r.cmd[1].data == 0x20004001 && r.cmd[2].data == 0x40000000);
            continue;
        }
        CHECK(g.cmd[0].data == k90_rows[level][0] && g.cmd[1].data == k90_rows[level][1]);
        CHECK(r.cmd[0].data == k90_rows[level][0] && r.cmd[1].data == k90_rows[level][1]);
        CHECK(g.cmd[2].data == (level >= 7 ? 0x60000004u : 0x60000000u) && r.cmd[2].data == 0x60000001);
    }

    Spec s = Default;                                       /* one VCD: only the last BCM commits */
    s.vcd[0] = 0;
    load(&s);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    CHECK(!Sm8750RpmhGmuBwRequest(&res, 2136718, &r) && r.wait_mask == 4 && r.cmd[0].data == 0x242c50b1);
    CHECK(!Sm8750RpmhHostDdrRequest(&res, 0, &r) && r.wait_mask == 4 && r.cmd[0].data == 0x20004001);
    s.vcd[1] = 1;                                           /* all different: everything commits */
    s.vcd[2] = 2;
    load(&s);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    CHECK(!Sm8750RpmhGmuBwRequest(&res, 2136718, &r) && r.wait_mask == 7 && r.cmd[1].data == 0x62160858);
    CHECK(!Sm8750RpmhHostDdrRequest(&res, 2136718, &r) && r.wait_mask == 7 && r.cmd[1].data == 0x62160858);

    load(&Default);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    CHECK(Sm8750RpmhRailRequest(&res, 3, 4, 1, &r) == Sm8750RpmhOk && r.count == 2 && r.wait_mask == 3);
    CHECK(r.cmd[0].addr == 0x30010 && r.cmd[0].data == 3 && r.cmd[1].addr == 0x30000 && r.cmd[1].data == 4);
    CHECK(Sm8750RpmhRailRequest(&res, 3, 4, 0, &r) == Sm8750RpmhOk);
    CHECK(r.cmd[0].addr == 0x30000 && r.cmd[0].data == 4 && r.cmd[1].addr == 0x30010 && r.cmd[1].data == 3);
    CHECK(Sm8750RpmhRailRequest(&res, 10, 4, 1, &r) == Sm8750RpmhArgument);   /* mx has 10 levels */
    CHECK(Sm8750RpmhHostCn0Request(&res, &r) == Sm8750RpmhOk && r.count == 1 && r.wait_mask == 1);
    CHECK(r.cmd[0].addr == 0x5000c && r.cmd[0].data == 0x60000001);
    Sm8750RpmhResources broken = res;
    broken.mc0.unit = 0;
    CHECK(Sm8750RpmhGmuBwRequest(&broken, 2136718, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostDdrRequest(&broken, 2136718, &r) == Sm8750RpmhArgument);
    broken = res;
    broken.cn0.present = 0;
    CHECK(Sm8750RpmhHostCn0Request(&broken, &r) == Sm8750RpmhArgument);
    broken = res;
    broken.acv.present = 0;
    CHECK(Sm8750RpmhGmuBwRequest(&broken, 2136718, &r) == Sm8750RpmhArgument);
    broken = res;
    broken.cx.present = 0;
    CHECK(Sm8750RpmhRailRequest(&broken, 3, 4, 1, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhRailRequest(&res, 3, 16, 1, &r) == Sm8750RpmhArgument);    /* cx has 16 levels: 0..15 */
    CHECK(Sm8750RpmhRailRequest(NULL, 3, 4, 1, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhRailRequest(&res, 3, 4, 1, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhGmuBwRequest(NULL, 0, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostDdrRequest(&res, 0, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostCn0Request(NULL, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostCn0Request(&res, NULL) == Sm8750RpmhArgument);
}

/* The PKX110-tested sequence of SOC Rpmh.h, for 2 commands on TCS 0. */
static void test_tcs_sequence(unsigned major)
{
    Rsc *m = rsc_new(major, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cmds[2] = {{0x30010, 3}, {0x30000, 4}};
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk);
    CHECK(r.tcs == 0 && r.status == 1 && r.command[0] == 0x10100 && r.command[1] == 0x10100 && r.busy == 0);
    CHECK(r.polls == 1 && r.step == Sm8750RpmhStepDone && r.irq_enable == 0x87 && (r.irq_status & 1));
    CHECK(r.hardware >> 16 == major && ((r.config >> 12) & 63) == 8 && r.lock_waits == 0);
    const uint32_t b = tcs(0), c0 = b + m->msg, c1 = c0 + m->stride;
    const struct {
        uint32_t reg, value;
    } want[] = {
        {0xd00, 0x86},
        {c0, 0x10108}, {c0 + 4, 0x30010}, {c0 + 8, 3},
        {c1, 0x10108}, {c1 + 4, 0x30000}, {c1 + 8, 4},
        {b + m->wait, 3}, {b + m->enable, 3},
        {b + m->control, 0x10000}, {b + m->control, 0x01010000},
        {b + m->control, 0x10000}, {b + m->control, 0}, {b + m->enable, 0}, {b + m->wait, 0},
        {0xd08, 1}, {0xd00, 0x87},
    };
    CHECK(m->nlog == sizeof(want) / sizeof(want[0]));
    for (unsigned i = 0; i < m->nlog; i++)
        CHECK(m->log[i].reg == want[i].reg && m->log[i].value == want[i].value);
    CHECK(m->nsent == 1 && m->sent[0].n == 2 && m->sent[0].msgid[1] == 0x10108);
    CHECK(!(reg_of(m, 0xd04) & 1) && reg_of(m, 0xd00) == 0x87 && Sm8750RpmhDrv2Owner() == 0);
    CHECK(m->now == 1000);
    free(m);
}

static void test_tcs_busy(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cn0 = {0x5000c, 0x60000001};
    m->regs[(tcs(0) + m->control) / 4] = 0x10000;           /* TCS 0 in use */
    m->regs[0xd04 / 4] = 0x2;                               /* TCS 1 completion not yet acknowledged */
    m->regs[(tcs(2) + m->status) / 4] = 0;                  /* TCS 2 not idle */
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhBusy);
    CHECK(r.busy == 7 && r.tcs == 0xffffffffu && r.step == Sm8750RpmhStepSelect && m->writes == 0);
    CHECK(Sm8750RpmhDrv2Owner() == 0);

    m->regs[(tcs(2) + m->status) / 4] = 1;
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.tcs == 2 && r.busy == 3);
    for (unsigned i = 0; i < m->nlog; i++)
        CHECK(m->log[i].reg < Sm8750RpmhTcsBase + 0x10 ||
              (m->log[i].reg >= tcs(2) && m->log[i].reg < tcs(2) + Sm8750RpmhTcsStride));
    CHECK(reg_of(m, 0xd04) == 0x2 && reg_of(m, 0xd00) == 0x87);    /* other TCS's IRQ state untouched */
    CHECK(reg_of(m, tcs(0) + m->control) == 0x10000);

    m->regs[(tcs(0) + m->enable) / 4] = 1;                  /* CMD_ENABLE or WAIT left set also mean busy */
    m->regs[(tcs(0) + m->control) / 4] = 0;
    m->regs[0xd04 / 4] = 0;
    m->regs[(tcs(1) + m->wait) / 4] = 1;
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.tcs == 2 && r.busy == 3);
    free(m);
}

static void test_tcs_refusals(void)
{
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd four[4] = {{0x30000, 1}, {0x30010, 1}, {0x40800, 1800}, {0x5000c, 0x60000001}};

    Rsc *m = rsc_new(4, 16);                                /* unknown layout */
    Sm8750RpmhIo io = rsc_io(m);
    CHECK(Sm8750RpmhWriteActive(&io, four, 4, 0xf, &r) == Sm8750RpmhUnsupported && m->writes == 0);
    CHECK(r.step == Sm8750RpmhStepHardware && Sm8750RpmhDrv2Owner() == 0);
    free(m);

    m = rsc_new(3, 3);                                      /* 3 commands per TCS */
    io = rsc_io(m);
    CHECK(Sm8750RpmhWriteActive(&io, four, 4, 0xf, &r) == Sm8750RpmhUnsupported && m->writes == 0);
    CHECK(Sm8750RpmhWriteActive(&io, four, 3, 0x7, &r) == Sm8750RpmhOk);
    m->regs[0x0c / 4] = 2u | 3u << 6 | 2u << 12 | 16u << 27;    /* DRV2 with 2 TCSes */
    m->writes = 0;
    CHECK(Sm8750RpmhWriteActive(&io, four, 1, 1, &r) == Sm8750RpmhUnsupported && m->writes == 0);
    m->regs[0x0c / 4] = 2u | 3u << 6 | 8u << 12 | 17u << 27;
    CHECK(Sm8750RpmhWriteActive(&io, four, 1, 1, &r) == Sm8750RpmhUnsupported && m->writes == 0);
    free(m);

    /* Argument checks happen before the lock or any register access. */
    m = rsc_new(3, 16);
    io = rsc_io(m);
    Sm8750RpmhCmd bad = {0x30000, 1};
    CHECK(Sm8750RpmhWriteActive(&io, four, 0, 1, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhWriteActive(&io, four, 17, 1, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhWriteActive(&io, four, 4, 0, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhWriteActive(&io, four, 2, 4, &r) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhWriteActive(&io, four, 4, 0xf, NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhWriteActive(NULL, four, 4, 0xf, &r) == Sm8750RpmhArgument && r.tcs == 0xffffffffu);
    CHECK(Sm8750RpmhSend(&io, NULL, &r) == Sm8750RpmhArgument);
    const Sm8750RpmhCmd refused[] = {
        {0x60000, 1},                                       /* slave 6 */
        {0x30002, 1},                                       /* unaligned */
        {0, 0},
        {0x30000, 0x100},                                   /* ARC level above 0xff */
        {0x40800, 0x2000},                                  /* VRM value above 8191 */
        {0x5000c, 0x80000000u},                             /* BCM bit 31 */
        {0x5000c, 0x10000000},                              /* BCM bit 28 */
        {0x130000, 1},                                      /* bits above the slave field */
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        bad = refused[i];
        CHECK(Sm8750RpmhWriteActive(&io, &bad, 1, 1, &r) == Sm8750RpmhArgument);
    }
    CHECK(m->reads == 0 && m->writes == 0 && Sm8750RpmhDrv2Owner() == 0);
    free(m);
}

static void test_tcs_lost_write(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cmds[2] = {{0x30010, 3}, {0x30000, 4}};
    m->ignored = tcs(0) + m->msg + m->stride + 8;           /* command 1 DATA never sticks */
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError);
    CHECK(r.step == Sm8750RpmhStepProgram && r.reg == m->ignored && r.expect == 4 && r.value == 0 && r.tcs == 0);
    CHECK(m->nsent == 0 && m->now == 100);                  /* never triggered; 100 x 1 us read-back */
    CHECK(!reg_of(m, tcs(0) + m->control) && !reg_of(m, tcs(0) + m->enable) && !reg_of(m, tcs(0) + m->wait));
    CHECK(reg_of(m, 0xd00) == 0x87 && Sm8750RpmhDrv2Owner() == 0);
    m->ignored = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 0);  /* cleaned TCS is reused */

    m->ignored = 0xd00;                                     /* IRQ_ENABLE mask does not stick: nothing else is written */
    m->nlog = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && r.step == Sm8750RpmhStepIrqMask);
    CHECK(m->nlog == 1 && m->nsent == 1);
    free(m);
}

/* Writes that stop sticking once the TCS ran: the vote completed, but the TCS cannot be given back.
 * The failure is reported with the register, and a TCS left busy is never picked again. */
static void test_tcs_cleanup(void)
{
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cmds[2] = {{0x30010, 3}, {0x30000, 4}};

    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    m->lost_after_trigger = tcs(0) + m->control;            /* CONTROL keeps 0x01010000 */
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && r.tcs == 0 && m->nsent == 1);
    CHECK(r.step == Sm8750RpmhStepCleanup && r.status == 1 && r.command[1] == 0x10100);
    CHECK(r.reg == tcs(0) + m->control && r.value == 0x01010000 && r.expect == 0x10000);
    CHECK(reg_of(m, 0xd00) == 0x86 && (reg_of(m, 0xd04) & 1) && Sm8750RpmhDrv2Owner() == 0);
    m->lost_after_trigger = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 1 && r.busy == 1);
    free(m);

    m = rsc_new(3, 16);
    io = rsc_io(m);
    m->lost_after_trigger = tcs(0) + m->control;            /* AMC mode stays enabled: CONTROL 0x10000, busy */
    m->lost_value = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && r.step == Sm8750RpmhStepCleanup);
    CHECK(r.reg == tcs(0) + m->control && r.value == 0x10000 && r.expect == 0 && reg_of(m, tcs(0) + m->enable) == 3);
    m->lost_after_trigger = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 1 && r.busy == 1);
    free(m);

    m = rsc_new(3, 16);
    io = rsc_io(m);
    m->lost_after_trigger = tcs(0) + m->enable;             /* CONTROL cleared, CMD_ENABLE stays: busy */
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && r.step == Sm8750RpmhStepCleanup);
    CHECK(r.reg == tcs(0) + m->enable && r.value == 3 && r.expect == 0);
    CHECK(!reg_of(m, tcs(0) + m->control) && reg_of(m, tcs(0) + m->wait) == 3);
    m->lost_after_trigger = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 1 && r.busy == 1);
    free(m);

    m = rsc_new(3, 16);
    io = rsc_io(m);
    m->lost_after_trigger = 0xd00;                          /* the IRQ_ENABLE bit cannot be put back */
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && r.step == Sm8750RpmhStepCleanup);
    CHECK(r.reg == 0xd00 && r.value == 0x86 && r.expect == 0x87 && r.tcs == 0);
    CHECK(!reg_of(m, tcs(0) + m->control) && !reg_of(m, tcs(0) + m->enable) && !(reg_of(m, 0xd04) & 1));
    m->lost_after_trigger = 0;                              /* the TCS itself is idle again; its IRQ stays off */
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 0 && r.irq_enable == 0x86);
    CHECK(reg_of(m, 0xd00) == 0x86);
    free(m);

    /* Never triggered, and the cleanup fails too: the cleanup failure is the one reported, and the
     * half-programmed TCS (CMD_ENABLE, WAIT set) is not picked again. */
    m = rsc_new(3, 16);
    io = rsc_io(m);
    m->ignored = tcs(0) + m->control;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhIoError && m->nsent == 0);
    CHECK(r.step == Sm8750RpmhStepCleanup && r.reg == tcs(0) + m->control && r.value == 0 && r.expect == 0x10000);
    CHECK(reg_of(m, tcs(0) + m->enable) == 3 && reg_of(m, tcs(0) + m->wait) == 3 && Sm8750RpmhDrv2Owner() == 0);
    m->ignored = 0;
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 2, 3, &r) == Sm8750RpmhOk && r.tcs == 1 && r.busy == 1);
    free(m);
}

/* Both completion conditions count: every CMD_STATUS completed and the TCS idle again. */
static void test_tcs_status_lag(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cn0 = {0x5000c, 0x60000001};
    m->status_lag_ms = 4;                                   /* commands done at 1 ms, TCS idle at 5 ms */
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.polls == 5 && r.status == 1);
    CHECK(r.step == Sm8750RpmhStepDone && !reg_of(m, tcs(0) + m->control));
    free(m);
}

static void test_tcs_timeout(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd ddr[3] = {{0x50008, 0x642c50b1}, {0x50004, 0x22160858}, {0x50000, 0x60000000}};
    m->complete_ms = -1;
    CHECK(Sm8750RpmhWriteActive(&io, ddr, 3, 5, &r) == Sm8750RpmhTimeout);
    CHECK(r.tcs == 0 && r.polls == 2000 && r.step == Sm8750RpmhStepPoll && m->now == 2000000);
    CHECK(r.status == 0 && r.command[0] == 0 && Sm8750RpmhDrv2Owner() == 0);
    /* Retained as evidence and never reused: still triggered, commands enabled, IRQ masked. */
    CHECK(reg_of(m, tcs(0) + m->control) == 0x01010000 && reg_of(m, tcs(0) + m->enable) == 7);
    CHECK(reg_of(m, tcs(0) + m->wait) == 5 && reg_of(m, 0xd00) == 0x86);
    m->complete_ms = 3;
    CHECK(Sm8750RpmhWriteActive(&io, ddr, 3, 5, &r) == Sm8750RpmhOk && r.tcs == 1 && r.busy == 1 && r.polls == 3);
    CHECK(reg_of(m, 0xd00) == 0x86 && reg_of(m, tcs(0) + m->control) == 0x01010000);

    m->partial = 1;                                         /* STATUS idle but one command not completed */
    CHECK(Sm8750RpmhWriteActive(&io, ddr, 3, 5, &r) == Sm8750RpmhTimeout && r.tcs == 1);
    CHECK(r.status == 1 && r.command[0] == 0x10100 && r.command[2] == 0x100);
    m->partial = 0;
    m->garbage = 1;                                         /* all-ones reads are not a completion */
    CHECK(Sm8750RpmhWriteActive(&io, ddr, 3, 5, &r) == Sm8750RpmhTimeout && r.tcs == 2);
    CHECK(r.command[0] == 0xffffffffu);
    m->garbage = 0;
    CHECK(Sm8750RpmhWriteActive(&io, ddr, 3, 5, &r) == Sm8750RpmhBusy && r.busy == 7);
    free(m);
}

static void test_tcs_wide(void)
{
    Rsc *m = rsc_new(2, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    Sm8750RpmhCmd cmds[16];
    for (unsigned i = 0; i < 16; i++) {
        static const uint32_t base[3] = {0x30000, 0x40800, 0x50000};
        cmds[i].addr = base[i % 3] + 4 * (i / 3) * (i % 3 == 1 ? 4 : 1);
        cmds[i].data = i % 3 == 1 ? 1200 + i : i;
    }
    CHECK(Sm8750RpmhWriteActive(&io, cmds, 16, 0x8001, &r) == Sm8750RpmhOk);
    CHECK(m->nsent == 1 && m->sent[0].n == 16 && m->sent[0].enable == 0xffff && m->sent[0].wait == 0x8001);
    for (unsigned i = 0; i < 16; i++)
        CHECK(m->sent[0].addr[i] == cmds[i].addr && m->sent[0].data[i] == cmds[i].data && r.command[i] == 0x10100);
    CHECK(m->log[1 + 3 * 15].reg == tcs(0) + 0x30 + 15 * 20);   /* v2: MSGID 0x30, 20-byte stride */
    free(m);
}

static void test_tcs_irq_bits(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cn0 = {0x5000c, 0x60000001};
    m->irq_poke = 0x20;                                     /* some other bit enabled meanwhile */
    m->regs[0xd04 / 4] = 0x80;                              /* a non-ACTIVE TCS has a pending IRQ */
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.tcs == 0);
    CHECK(reg_of(m, 0xd00) == 0xa7 && reg_of(m, 0xd04) == 0x80);

    m->regs[0xd00 / 4] = 0x06;                              /* TCS 0 IRQ was off: it stays off */
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.tcs == 0 && r.irq_enable == 0x06);
    CHECK(reg_of(m, 0xd00) == 0x06);
    free(m);
}

static void test_lock(void)
{
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhResult r;
    const Sm8750RpmhCmd cn0 = {0x5000c, 0x60000001};
    uint32_t waits = 0;

    /* The two weak definitions are one word. */
    CHECK(Sm8750RpmhDrv2Acquire(&io, 0x1234, &waits) == Sm8750RpmhOk && waits == 0);
    CHECK(sm8750_rpmh_drv2_owner == 0x1234 && Sm8750RpmhDrv2Owner() == 0x1234);
    Sm8750RpmhDrv2Release();
    CHECK(sm8750_rpmh_drv2_owner == 0);
    CHECK(Sm8750RpmhDrv2Acquire(&io, 0, &waits) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhDrv2Acquire(NULL, 1, &waits) == Sm8750RpmhArgument);

    /* Touch holds the lock for 5 ms: the GPU vote waits, then runs. The model fails any DRV-2
     * access made while the lock is not the GPU's. */
    CHECK(touch_try_exchange());
    m->touch_release_at = 5000;
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhOk && r.lock_waits == 500 && m->nsent == 1);
    CHECK(Sm8750RpmhDrv2Owner() == 0);

    /* Touch never lets go: give up after 2 s without touching DRV-2 or the owner word. */
    CHECK(touch_try_cas());
    m->now = 0;
    m->reads = m->writes = 0;
    CHECK(Sm8750RpmhWriteActive(&io, &cn0, 1, 1, &r) == Sm8750RpmhBusy);
    CHECK(r.step == Sm8750RpmhStepLock && r.lock_waits == Sm8750RpmhLockSteps && m->now == 2000000);
    CHECK(m->reads == 0 && m->writes == 0 && Sm8750RpmhDrv2Owner() == TouchTag);
    touch_release();
    free(m);
}

/* Real threads: GPU users take the lock through Sm8750RpmhDrv2Acquire (compare-and-swap); touch
 * users with a plain exchange or a compare-and-swap. The critical sections must never overlap. */
static unsigned in_critical, counter;

static void yield_delay(void *context, unsigned microseconds)
{
    (void)context;
    (void)microseconds;
    sched_yield();
}

static void critical(void)
{
    CHECK(__atomic_fetch_add(&in_critical, 1, __ATOMIC_RELAXED) == 0);
    unsigned c = counter;
    sched_yield();
    counter = c + 1;
    CHECK(__atomic_fetch_sub(&in_critical, 1, __ATOMIC_RELAXED) == 1);
}

static void *gpu_user(void *arg)
{
    (void)arg;
    Sm8750RpmhIo io = {NULL, NULL, NULL, yield_delay};
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(Sm8750RpmhDrv2Acquire(&io, Sm8750RpmhLockOwnerGpu, NULL) == Sm8750RpmhOk);
        critical();
        Sm8750RpmhDrv2Release();
    }
    return NULL;
}

static void *touch_user(void *arg)
{
    int exchange = arg != NULL;
    for (unsigned i = 0; i < 1000; i++) {
        while (!(exchange ? touch_try_exchange() : touch_try_cas()))
            sched_yield();
        critical();
        touch_release();
    }
    return NULL;
}

static void test_lock_threads(void)
{
    pthread_t t[4];
    static int exchange = 1;
    CHECK(!pthread_create(&t[0], NULL, gpu_user, NULL));
    CHECK(!pthread_create(&t[1], NULL, gpu_user, NULL));
    CHECK(!pthread_create(&t[2], NULL, touch_user, &exchange));
    CHECK(!pthread_create(&t[3], NULL, touch_user, NULL));
    for (unsigned i = 0; i < 4; i++)
        CHECK(!pthread_join(t[i], NULL));
    CHECK(counter == 4000 && in_critical == 0 && Sm8750RpmhDrv2Owner() == 0);
}

static void check_sent(const Sent *t, unsigned n, uint32_t wait, const uint32_t *addr, const uint32_t *data)
{
    CHECK(t->n == n && t->enable == (1u << n) - 1 && t->wait == wait);
    for (unsigned i = 0; i < n; i++)
        CHECK(t->msgid[i] == 0x10108 && t->addr[i] == addr[i] && t->data[i] == data[i]);
}

/* HLOS rules on everything the host put on apps DRV-2 (ARCHITECTURE 4.3.2): SH0/MC0 valid with
 * x, y >= 1; CN0 exactly 0x60000001; ACV only BIT(0) or off; ARC levels only. */
static void check_host_rules(const Rsc *m, const Sm8750RpmhResources *res)
{
    for (unsigned s = 0; s < m->nsent; s++) {
        const Sent *t = &m->sent[s];
        for (unsigned i = 0; i < t->n; i++) {
            uint32_t a = t->addr[i], d = t->data[i];
            if (a == res->sh0.addr || a == res->mc0.addr)
                CHECK((d & 1u << 29) && ((d >> 14) & 0x3fff) >= 1 && (d & 0x3fff) >= 1);
            else if (a == res->cn0.addr)
                CHECK(d == 0x60000001);
            else if (a == res->acv.addr)
                CHECK((d & 0x9fffffffu) == 1 || (d & 0x9fffffffu) == 0);
            else
                CHECK(a == res->mx.addr || a == res->cx.addr);
        }
    }
}

static void test_host_policy(void)
{
    Sm8750RpmhResources res;
    load(&Default);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res));
    Rsc *m = rsc_new(3, 16);
    Sm8750RpmhIo io = rsc_io(m);
    Sm8750RpmhHost host;
    static const uint32_t rails_up[2] = {0x30010, 0x30000}, rails_down[2] = {0x30000, 0x30010};
    static const uint32_t cn0[1] = {0x5000c}, ddr[3] = {0x50008, 0x50004, 0x50000};

    /* Defaults = the author's measured votes (4.3.3): S1 rails at the table top; S2 CN0 once and
     * DDR level 11 with ACV BIT(0); S5 floor = boot level, so no TCS; nothing is ever revoked. */
    Sm8750RpmhHostInit(&host, &io, &res);
    CHECK(Sm8750RpmhHostRails(&host, 1) == Sm8750RpmhOk && host.held == Sm8750RpmhHeldRails);
    check_sent(&m->sent[0], 2, 3, rails_up, (const uint32_t[]){9, 15});
    CHECK(host.mx_hlvl == 9 && host.cx_hlvl == 15 && host.last.tcs == 0 && host.last.step == Sm8750RpmhStepDone);
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhOk);
    check_sent(&m->sent[1], 1, 1, cn0, (const uint32_t[]){0x60000001});
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[Sm8750RpmhDdrDefaultLevel]) == Sm8750RpmhOk);
    check_sent(&m->sent[2], 3, 5, ddr, (const uint32_t[]){0x6fffffff, 0x2fffffff, 0x60000001});
    CHECK(host.held == (Sm8750RpmhHeldRails | Sm8750RpmhHeldCn0 | Sm8750RpmhHeldDdr) && host.ddr_kbps == 18597656);
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[Sm8750RpmhDdrDefaultLevel]) == Sm8750RpmhOk);    /* S5 floor */
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhOk && Sm8750RpmhHostRails(&host, 1) == Sm8750RpmhOk);
    CHECK(m->nsent == 3 && host.sent == 3);                 /* none of the repeats reached the hardware */
    for (unsigned i = 0; i < m->nsent; i++)
        CHECK(m->sent[i].slot == 0);                        /* every TCS came back to the pool */
    CHECK(reg_of(m, 0xd00) == 0x87 && reg_of(m, 0xd04) == 0);
    check_host_rules(m, &res);

    /* M11 single-variable experiments: rails=0 (LOW_SVS), ddr=2, floor 3, floor 0 (keepalive
     * minimum: SH0/MC0 x = y = 1, ACV off, CN0 untouched), then back to the top (raise, mx first). */
    m->nsent = 0;
    Sm8750RpmhHostInit(&host, &io, &res);
    CHECK(Sm8750RpmhHostRails(&host, 0) == Sm8750RpmhOk && host.mx_hlvl == 3 && host.cx_hlvl == 4);
    check_sent(&m->sent[0], 2, 3, rails_up, (const uint32_t[]){3, 4});
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhOk);
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[2]) == Sm8750RpmhOk);
    check_sent(&m->sent[2], 3, 5, ddr, (const uint32_t[]){0x642c50b1, 0x22160858, 0x60000001});
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[3]) == Sm8750RpmhOk);
    check_sent(&m->sent[3], 3, 5, ddr, (const uint32_t[]){0x6a52a94a, 0x252954a5, 0x60000001});
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[0]) == Sm8750RpmhOk);
    check_sent(&m->sent[4], 3, 5, ddr, (const uint32_t[]){0x60004001, 0x20004001, 0x40000000});
    CHECK((host.held & Sm8750RpmhHeldDdr) && host.ddr_kbps == 0);
    CHECK(Sm8750RpmhHostDdr(&host, 0) == Sm8750RpmhOk && m->nsent == 5);
    CHECK(Sm8750RpmhHostRails(&host, 1) == Sm8750RpmhOk);
    check_sent(&m->sent[5], 2, 3, rails_up, (const uint32_t[]){9, 15});
    CHECK(Sm8750RpmhHostRails(&host, 0) == Sm8750RpmhOk);    /* lowering: cx first */
    check_sent(&m->sent[6], 2, 3, rails_down, (const uint32_t[]){4, 3});
    CHECK(m->nsent == 7 && host.sent == 7);
    check_host_rules(m, &res);

    /* A failed vote leaves the held state as it was, and the next call retries it. */
    m->nsent = 0;
    Sm8750RpmhHostInit(&host, &io, &res);
    CHECK(!Sm8750RpmhHostRails(&host, 1) && !Sm8750RpmhHostDdr(&host, DdrKbps[11]));
    m->complete_ms = -1;
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhTimeout && host.last.tcs == 0);
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[3]) == Sm8750RpmhTimeout && host.last.tcs == 1);
    CHECK(host.held == (Sm8750RpmhHeldRails | Sm8750RpmhHeldDdr) && host.ddr_kbps == 18597656);
    m->complete_ms = 1;                                     /* TCS 0 and 1 stay retained */
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhOk && host.last.tcs == 2 && (host.held & Sm8750RpmhHeldCn0));
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[3]) == Sm8750RpmhOk && host.last.tcs == 2 && host.ddr_kbps == 5285156);
    CHECK(host.sent == 6 && m->nsent == 6);
    check_host_rules(m, &res);
    m->complete_ms = -1;                                    /* the last idle TCS retained too: Busy, no write */
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[11]) == Sm8750RpmhTimeout);
    m->writes = 0;
    CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[11]) == Sm8750RpmhBusy && m->writes == 0 && host.last.busy == 7);
    CHECK(host.ddr_kbps == 5285156);
    free(m);

    /* M11 floor adjustment: every DT level as the floor, down and back up. Each change is one
     * TCS, and no floor ever puts valid 0 on SH0/MC0/CN0 or BIT(2) on ACV. */
    m = rsc_new(3, 16);
    io = rsc_io(m);
    Sm8750RpmhHostInit(&host, &io, &res);
    CHECK(!Sm8750RpmhHostRails(&host, 1) && !Sm8750RpmhHostCn0(&host) && !Sm8750RpmhHostDdr(&host, DdrKbps[11]));
    for (int level = 10; level >= 0; level--)
        CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[level]) == Sm8750RpmhOk && host.ddr_kbps == DdrKbps[level]);
    for (int level = 1; level <= 11; level++)
        CHECK(Sm8750RpmhHostDdr(&host, DdrKbps[level]) == Sm8750RpmhOk);
    CHECK(m->nsent == 3 + 11 + 11 && host.sent == m->nsent);
    check_host_rules(m, &res);
    CHECK(m->sent[13].data[2] == 0x40000000 && m->sent[12].data[2] == 0x60000001);  /* ACV off only at 0 */
    free(m);

    /* No LOW_SVS level in a table: refused before anything is sent. */
    m = rsc_new(3, 16);
    io = rsc_io(m);
    Sm8750RpmhResources low = res;
    static const uint16_t lowcx[16] = {0x0, 0x10, 0x30, 0x38};
    memcpy(low.cx.vlvl, lowcx, sizeof(lowcx));
    low.cx.count = 4;
    Sm8750RpmhHostInit(&host, &io, &low);
    CHECK(Sm8750RpmhHostRails(&host, 0) == Sm8750RpmhRange && host.held == 0 && host.sent == 0);
    CHECK(m->reads == 0 && m->writes == 0);
    CHECK(Sm8750RpmhHostRails(&host, 1) == Sm8750RpmhOk);
    check_sent(&m->sent[0], 2, 3, rails_up, (const uint32_t[]){9, 3});
    CHECK(Sm8750RpmhHostRails(NULL, 1) == Sm8750RpmhArgument && Sm8750RpmhHostCn0(NULL) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostDdr(NULL, 0) == Sm8750RpmhArgument);
    Sm8750RpmhHostInit(NULL, &io, &res);                    /* tolerated, does nothing */
    Sm8750RpmhHostInit(&host, &io, NULL);
    CHECK(Sm8750RpmhHostRails(&host, 1) == Sm8750RpmhArgument && Sm8750RpmhHostDdr(&host, 0) == Sm8750RpmhArgument);
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhArgument && host.sent == 0 && host.last.tcs == 0xffffffffu);
    free(m);

    /* A cmd-db whose CN0 has no aux still gives the host CN0 vote, to CN0's address. */
    load(&Default);
    put16(db.snapshot + entry_at(&image, "CN0") + 20, 0);
    CHECK(!Sm8750RpmhResourcesParse(&db, &res) && res.cn0.present && !res.cn0.aux_len);
    m = rsc_new(3, 16);
    io = rsc_io(m);
    Sm8750RpmhHostInit(&host, &io, &res);
    CHECK(Sm8750RpmhHostCn0(&host) == Sm8750RpmhOk);
    check_sent(&m->sent[0], 1, 1, cn0, (const uint32_t[]){0x60000001});
    free(m);
}

int main(void)
{
    test_cmd_db_copy();
    test_cmd_db_lookup();
    test_cmd_db_corrupt();
    test_resources();
    test_levels();
    test_encoding();
    test_tcs_sequence(2);
    test_tcs_sequence(3);
    test_tcs_busy();
    test_tcs_refusals();
    test_tcs_lost_write();
    test_tcs_cleanup();
    test_tcs_status_lag();
    test_tcs_timeout();
    test_tcs_wide();
    test_tcs_irq_bits();
    test_lock();
    test_lock_threads();
    test_host_policy();
    puts("PASS: SM8750 RPMh cmd-db lookups, DRV-2 ACTIVE TCS protocol, DRV-2 lock and host vote policy");
    return 0;
}
