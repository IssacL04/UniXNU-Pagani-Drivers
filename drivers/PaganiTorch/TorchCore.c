/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The torch core (TorchCore.h). Register facts and sources in TorchRegs.h ([L] = Linux leds-qcom-flash.c, facts only).
 *
 * Torch on [L] (qcom_flash_led_brightness_set, in its order, with read-backs added): the board channels' CHAN_EN bits cleared (the
 * strobe off), the module off, IRES 5 mA steps on the board channels, ITARGET of each channel, the safety timer off, the module on,
 * the software strobe, then everything read back and only then CHAN_EN set; the status registers are read at once and every
 * poll_ms while on. Torch off: CHAN_EN cleared, the module off, ITARGET back to the lowest code (as [L] does for brightness 0), and
 * CHAN_EN and the module read back.
 * Every write goes through the gate (TorchSpmi.c), which refuses anything else and refuses to switch a channel on before it wrote
 * that channel's current itself. Any failure, refusal, timeout, failed read-back or fault bit: the emergency stop (CHAN_EN = 0,
 * EN_CTL = 0, allowed once even on a closed bus) and the torch stays off until reboot. */
#include "TorchCore.h"
#include "TorchRegs.h"
#include "TorchText.h"

enum { E_IO = -5, E_NODEV = -19, E_INVAL = -22 };
enum {
    LINE = 200,
    POLL_MIN_MS = 10, POLL_MAX_MS = 1000, POLL_DEFAULT_MS = 100,
    EVENT_FIRST = 8,            /* log 1: the first on/off lines, then one per EVENT_GAP_US, at most EVENT_MAX_RATIONED a boot */
    EVENT_MAX_RATIONED = 24, EVENT_MAX_ALL = 64, FAULT_LINES = 3,
};
#define EVENT_GAP_US 300000000ull

static uint64_t now_us(const PgtCore *t) {return t->ops->now_us(t->ops->ctx);}
static void trace(const PgtCore *t, const char *line)
{
    if (t->ops->trace) t->ops->trace(t->ops->ctx, line);
}
static void begin(PgtText *x, char *buf)
{
    pgt_text_start(x, buf, LINE);
    pgt_text_str(x, "[torch] ");
}

int pgt_config_make(PgtConfig *c, unsigned mode, unsigned ee, uint16_t ppid, uint8_t chan_mask, unsigned hard_ma, unsigned max_ma_arg,
                    unsigned log)
{
    if (!c || (mode != PGT_MODE_ON && mode != PGT_MODE_DRY)) return 0;
    pgt_zero(c, sizeof *c);
    if (hard_ma > PGT_TORCH_POLICY_MA) hard_ma = PGT_TORCH_POLICY_MA;
    unsigned cap = max_ma_arg ? max_ma_arg : PGT_DEFAULT_MA;
    if (cap > hard_ma) cap = hard_ma;
    if (cap < PGT_TORCH_MIN_MA) cap = PGT_TORCH_MIN_MA;
    c->mode = mode;
    c->ee = ee;
    c->ppid = ppid;
    c->chan_mask = chan_mask;
    c->hard_ma = (uint16_t)hard_ma;
    c->max_ma = (uint16_t)cap;
    c->auto_off_ms = PGT_AUTO_OFF_MS;
    c->poll_ms = POLL_DEFAULT_MS;
    c->log = (uint8_t)(log > 2 ? 2 : log);
    return 1;
}

uint32_t pgt_test_ms(unsigned arg) {return arg < PGT_TEST_MAX_MS ? arg : PGT_TEST_MAX_MS;}

unsigned pgt_ma_code(unsigned ma)
{
    const uint64_t ua = (uint64_t)ma * 1000u + 1u;
    const uint64_t code = ua > PGT_TORCH_STEP_UA ? ua / PGT_TORCH_STEP_UA - 1u : 0u;
    return code > PGT_ITARGET_MASK ? PGT_ITARGET_MASK : (unsigned)code;
}
unsigned pgt_code_ma(unsigned code) {return (code + 1u) * (PGT_TORCH_STEP_UA / 1000u);}
unsigned pgt_level_ma(unsigned level, unsigned max_ma)
{
    if (!level) return 0;
    if (level > PGT_LEVEL_MAX) level = PGT_LEVEL_MAX;
    const unsigned ma = (unsigned)(((uint64_t)level * max_ma + PGT_LEVEL_MAX / 2) / PGT_LEVEL_MAX);
    return ma < PGT_TORCH_MIN_MA ? PGT_TORCH_MIN_MA : ma;
}

static int rd(PgtCore *t, uint8_t reg, uint8_t *v) {return pgt_spmi_read(&t->spmi, PGT_FLASH, reg, v, 1);}
static int wr(PgtCore *t, uint8_t reg, uint8_t v) {return pgt_spmi_write1(&t->spmi, PGT_FLASH, reg, v);}

/* ---- the log ---- */
static int event_line_allowed(PgtCore *t)
{
    if (!t->cfg.log) {
        t->dropped++;
        return 0;
    }
    const uint64_t n = now_us(t);
    if (t->event_lines >= (t->cfg.log >= 2 ? EVENT_MAX_ALL : EVENT_MAX_RATIONED) ||
        (t->cfg.log == 1 && t->event_lines >= EVENT_FIRST && n - t->event_line_us < EVENT_GAP_US)) {
        t->dropped++;
        return 0;
    }
    t->event_lines++;
    t->event_line_us = n;
    return 1;
}

static void status_fields(PgtText *x, const uint8_t s[3])
{
    pgt_text_str(x, " s1=");
    pgt_text_hex2(x, s[0]);
    pgt_text_str(x, " s2=");
    pgt_text_hex2(x, s[1]);
    pgt_text_str(x, " s3=");
    pgt_text_hex2(x, s[2]);
}

/* ---- faults ---- */
static const char *fault_of(const PgtCore *t, const uint8_t s[3])
{
    const unsigned mask = t->cfg.chan_mask;
    for (unsigned ch = 0; ch < PGT_CHANNELS; ++ch)
        if ((mask >> ch & 1u) && (s[0] >> (2u * ch) & 1u)) return "short-circuit";
    if (s[1] & (PGT_STS2_OTST1 | PGT_STS2_OTST2 | PGT_STS2_BOB_THM_OVLD)) return "over-temperature";
    if (s[1] & (PGT_STS2_BOB_ILIM_S1 | PGT_STS2_BOB_ILIM_S2 | PGT_STS2_BCL_IBAT)) return "over-current";
    if (s[1] & PGT_STS2_VPH_LOW) return "vph-low";
    for (unsigned ch = 0; ch < PGT_CHANNELS; ++ch)
        if ((mask >> ch & 1u) && (s[2] >> (2u * ch) & 1u)) return "safety-timer";
    return 0;
}

/* Stops the LED (the gate's one emergency stop) and keeps the torch off until reboot. */
static int fault(PgtCore *t, const char *why, int rc)
{
    const int stop = t->spmi.p[PGT_FLASH].writable ? pgt_spmi_emergency_stop(&t->spmi) : E_NODEV;
    t->state = PGT_ST_FAULT;
    t->faults++;
    t->level = 0;
    t->ma = 0;
    t->why = why;
    t->last_rc = rc;
    if (t->fault_lines < FAULT_LINES) {
        t->fault_lines++;
        char buf[LINE];
        PgtText x;
        begin(&x, buf);
        pgt_text_str(&x, "FAULT ");
        pgt_text_str(&x, why);
        pgt_text_kd(&x, "rc", rc);
        pgt_text_kd(&x, "stop", stop);
        status_fields(&x, t->sts);
        pgt_text_kx(&x, "reg", t->spmi.last_reg);
        pgt_text_kx(&x, "status", t->spmi.last_status);
        pgt_text_kd(&x, "refused", t->spmi.refused);
        pgt_text_str(&x, ": torch off until reboot");
        trace(t, buf);
    }
    return E_IO;
}

/* STATUS1, STATUS2, (0x08), STATUS3 in one observer read. */
static int read_status(PgtCore *t, uint8_t s[3])
{
    uint8_t d[4];
    const int rc = pgt_spmi_read(&t->spmi, PGT_FLASH, PGT_FLASH_STATUS1, d, 4);
    if (rc) return rc;
    s[0] = d[0];
    s[1] = (uint8_t)(d[1] & PGT_STS2_MASK);
    s[2] = d[PGT_FLASH_STATUS3 - PGT_FLASH_STATUS1];
    return 0;
}

/* ---- init ---- */
static int stop_at(PgtCore *t, int stage, const char *why, int rc)
{
    t->stage = stage;
    t->state = PGT_ST_REFUSED;
    t->why = why;
    t->last_rc = rc;
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, "refused: ");
    pgt_text_str(&x, why);
    pgt_text_kd(&x, "rc", rc);
    pgt_text_str(&x, " (nothing more is written; torch off)");
    trace(t, buf);
    return rc ? rc : E_NODEV;
}

static const char *problem(const PgtCore *t)
{
    const PgtPeriph *p = &t->spmi.p[PGT_FLASH];
    if (p->type != PGT_FLASH_TYPE) return "not-a-flash-module";
    if (p->subtype != PGT_FLASH_SUBTYPE_4CH) return "not-the-4-channel-module";
    if (p->owner != t->cfg.ee) return "channel-not-owned";
    return 0;
}

/* After the dump: [L] has only bit 7 in EN_CTL and the gate takes nothing else; better refused here than at the first "on". */
static const char *dump_problem(const PgtCore *t)
{
    return t->en_ctl & (uint8_t)~PGT_MODULE_EN ? "en-ctl-reserved-bits" : 0;
}

static void spmi_line(const PgtCore *t, int rc)
{
    const PgtPeriph *p = &t->spmi.p[PGT_FLASH];
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, "spmi");
    pgt_text_kd(&x, "rc", rc);
    pgt_text_kx(&x, "v", t->spmi.version);
    pgt_text_kd(&x, "ch", t->spmi.channels);
    pgt_text_kd(&x, "ee", t->cfg.ee);
    pgt_text_str(&x, " flash=");
    pgt_text_hex(&x, p->ppid);
    pgt_text_char(&x, ':');
    pgt_text_dec(&x, p->apid);
    pgt_text_str(&x, ":ee");
    pgt_text_dec(&x, p->owner);
    pgt_text_str(&x, ":irq");
    pgt_text_dec(&x, p->irq);
    pgt_text_str(&x, ":x");
    pgt_text_dec(&x, p->mappings);
    trace(t, buf);
}

/* REVISION/TYPE/SUBTYPE and the verdict; then the torch registers (both modes): status, timers, targets, enable, resolution,
 * strobes, channel enable. */
static int identity_and_dump(PgtCore *t, const char *verdict)
{
    const PgtPeriph *p = &t->spmi.p[PGT_FLASH];
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, "id flash=");
    pgt_text_hex2(&x, p->type);
    pgt_text_char(&x, '/');
    pgt_text_hex2(&x, p->subtype);
    pgt_text_str(&x, ":r");
    pgt_text_udec(&x, p->rev[1]);
    pgt_text_char(&x, '.');
    pgt_text_udec(&x, p->rev[0]);
    pgt_text_char(&x, '.');
    pgt_text_udec(&x, p->rev[3]);
    pgt_text_char(&x, '.');
    pgt_text_udec(&x, p->rev[2]);
    pgt_text_ks(&x, "verdict", verdict ? verdict : "ok");
    trace(t, buf);
    static const uint8_t from[] = {0x06, 0x3e, 0x42, 0x46, 0x4a};
    uint8_t d[20] = {0}, en = 0;
    int rc = 0;
    for (unsigned i = 0; i < sizeof from && !rc; ++i) rc = pgt_spmi_read(&t->spmi, PGT_FLASH, from[i], d + 4 * i, 4);
    if (!rc) rc = rd(t, PGT_FLASH_CHAN_EN, &en);
    begin(&x, buf);
    pgt_text_str(&x, "reg");
    if (rc) {
        pgt_text_kd(&x, "rc", rc);
    } else {
        for (unsigned i = 0; i < sizeof from; ++i)
            for (unsigned k = 0; k < 4; ++k) {
                pgt_text_char(&x, ' ');
                pgt_text_hex2(&x, from[i] + k);
                pgt_text_char(&x, '=');
                pgt_text_hex2(&x, d[4 * i + k]);
            }
        pgt_text_str(&x, " 4e=");
        pgt_text_hex2(&x, en);
        t->boot_sts[0] = d[0];
        t->boot_sts[1] = (uint8_t)(d[1] & PGT_STS2_MASK);
        t->boot_sts[2] = d[3];
        t->en_ctl = d[12];
        t->ires = d[15];
        t->chan_en = en;
    }
    trace(t, buf);
    return rc;
}

/* Dry run: the RW channel page answers (one read of its STATUS word; nothing is written). */
static void probe_channel(PgtCore *t)
{
    const PgtSpmiIo *io = t->spmi.io;
    const PgtPeriph *p = &t->spmi.p[PGT_FLASH];
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, "chan");
    pgt_text_kd(&x, "apid", p->apid);
    if (!io->map_channel || !io->chan_read) {
        pgt_text_str(&x, " (no channel access)");
    } else {
        const int rc = io->map_channel(io->ctx, PGT_FLASH, (unsigned)p->apid);
        pgt_text_kd(&x, "map", rc);
        if (!rc) pgt_text_kx(&x, "status", io->chan_read(io->ctx, PGT_FLASH, PGT_ARB_STATUS));
        pgt_text_str(&x, " (read only)");
    }
    trace(t, buf);
}

int pgt_core_init(PgtCore *t, const PgtConfig *c, const PgtOps *ops, const PgtSpmiIo *io)
{
    if (!t || !c || !ops || !ops->now_us || !io) return E_INVAL;
    pgt_zero(t, sizeof *t);
    t->cfg = *c;
    t->ops = ops;
    t->stage = 1;
    PgtConfig *k = &t->cfg;
    if (k->hard_ma > PGT_TORCH_POLICY_MA) k->hard_ma = PGT_TORCH_POLICY_MA;
    if ((k->mode != PGT_MODE_ON && k->mode != PGT_MODE_DRY) || k->hard_ma < PGT_TORCH_MIN_MA || !k->chan_mask ||
        k->chan_mask > (1u << PGT_CHANNELS) - 1u)
        return stop_at(t, -1, "config", E_INVAL);
    if (k->max_ma > k->hard_ma) k->max_ma = k->hard_ma;
    if (k->max_ma < PGT_TORCH_MIN_MA) k->max_ma = PGT_TORCH_MIN_MA;
    if (!k->auto_off_ms || k->auto_off_ms > PGT_AUTO_OFF_MS) k->auto_off_ms = PGT_AUTO_OFF_MS;
    if (!k->poll_ms) k->poll_ms = POLL_DEFAULT_MS;
    if (k->poll_ms < POLL_MIN_MS) k->poll_ms = POLL_MIN_MS;
    if (k->poll_ms > POLL_MAX_MS) k->poll_ms = POLL_MAX_MS;
    t->hard_code = (uint8_t)pgt_ma_code(k->hard_ma);
    t->code_max = (uint8_t)pgt_ma_code(k->max_ma);
    if (t->code_max > t->hard_code) t->code_max = t->hard_code;

    const uint16_t ppid[PGT_PERIPHS] = {k->ppid};
    int rc = pgt_spmi_discover(&t->spmi, io, k->ee, ppid, k->chan_mask, t->hard_code);
    spmi_line(t, rc);
    if (rc) return stop_at(t, -2, "arbiter", rc);
    t->stage = 2;
    if (t->spmi.p[PGT_FLASH].apid < 0) return stop_at(t, -2, "no-channel", E_NODEV);
    rc = pgt_spmi_identify(&t->spmi, PGT_FLASH);
    if (rc) return stop_at(t, -3, "identity-read", rc);
    t->stage = 3;
    const char *verdict = problem(t);
    rc = identity_and_dump(t, verdict);
    if (rc) return stop_at(t, -3, "dump-read", rc);
    if (!verdict) verdict = dump_problem(t);
    if (k->mode == PGT_MODE_DRY) {
        if (!verdict) probe_channel(t);
        t->state = PGT_ST_DRY;
        t->stage = 5;
        char buf[LINE];
        PgtText x;
        begin(&x, buf);
        pgt_text_str(&x, "dry run done: nothing written");
        pgt_text_ks(&x, "verdict", verdict ? verdict : "ok");
        pgt_text_kd(&x, "max_ma", k->max_ma);
        pgt_text_kd(&x, "code", t->code_max);
        pgt_text_kd(&x, "reads", t->spmi.reads);
        trace(t, buf);
        return 1;
    }
    if (verdict) return stop_at(t, -3, verdict, E_NODEV);
    rc = pgt_spmi_open(&t->spmi, PGT_FLASH);
    if (rc) return stop_at(t, -4, "open", rc);
    /* every channel off (the safe state, and the first write proves the RW path); the module off when it was on */
    rc = wr(t, PGT_FLASH_CHAN_EN, 0);
    if (!rc && (t->en_ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(t->en_ctl & ~PGT_MODULE_EN));
    if (rc) {
        t->stage = -4;
        return fault(t, "init-write", rc);
    }
    t->state = PGT_ST_OFF;
    t->stage = 4;
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, "ready");
    pgt_text_kd(&x, "max_ma", k->max_ma);
    pgt_text_kd(&x, "code", t->code_max);
    pgt_text_kd(&x, "hard_ma", k->hard_ma);
    pgt_text_kd(&x, "hard_code", t->hard_code);
    pgt_text_kx(&x, "chans", k->chan_mask);
    pgt_text_kd(&x, "off_s", k->auto_off_ms / 1000u);
    pgt_text_kd(&x, "writes", t->spmi.writes);
    trace(t, buf);
    return 0;
}

/* ---- on / off ---- */
static int torch_off(PgtCore *t)
{
    const uint8_t mask = t->cfg.chan_mask;
    uint8_t en = 0, ctl = 0;
    int rc = rd(t, PGT_FLASH_CHAN_EN, &en);
    if (!rc) rc = rd(t, PGT_FLASH_EN_CTL, &ctl);
    if (!rc && (en & mask)) rc = wr(t, PGT_FLASH_CHAN_EN, (uint8_t)(en & ~mask));
    if (!rc && (ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(ctl & ~PGT_MODULE_EN));
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch) {
        if (!(mask >> ch & 1u)) continue;
        uint8_t it = 0;
        rc = rd(t, (uint8_t)PGT_FLASH_ITARGET(ch), &it);
        if (!rc && (it & PGT_ITARGET_MASK)) rc = wr(t, (uint8_t)PGT_FLASH_ITARGET(ch), (uint8_t)(it & ~PGT_ITARGET_MASK));
    }
    if (rc) return fault(t, "off-write", rc);
    /* really off: it may not stay on behind a write that was acknowledged */
    rc = rd(t, PGT_FLASH_CHAN_EN, &en);
    if (!rc) rc = rd(t, PGT_FLASH_EN_CTL, &ctl);
    if (rc) return fault(t, "off-read", rc);
    if ((en & mask) || (ctl & PGT_MODULE_EN)) return fault(t, "off-verify", E_IO);
    t->state = PGT_ST_OFF;
    t->level = 0;
    t->ma = 0;
    t->code = 0;
    return 0;
}

static int torch_on(PgtCore *t, uint8_t code)
{
    const uint8_t mask = t->cfg.chan_mask;
    uint8_t en = 0, ctl = 0, ires = 0, it[PGT_CHANNELS] = {0}, st[PGT_CHANNELS] = {0};
    int rc = rd(t, PGT_FLASH_CHAN_EN, &en);
    if (!rc) rc = rd(t, PGT_FLASH_EN_CTL, &ctl);
    if (!rc) rc = rd(t, PGT_FLASH_IRES, &ires);
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch) {
        if (!(mask >> ch & 1u)) continue;
        rc = rd(t, (uint8_t)PGT_FLASH_ITARGET(ch), &it[ch]);
        if (!rc) rc = rd(t, (uint8_t)PGT_FLASH_CHAN_STROBE(ch), &st[ch]);
    }
    if (rc) return fault(t, "on-read", rc);
    /* [L]: strobe off, module off, current, timer off, module on, software strobe */
    if (en & mask) rc = wr(t, PGT_FLASH_CHAN_EN, (uint8_t)(en & ~mask));
    if (!rc && (ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(ctl & ~PGT_MODULE_EN));
    if (!rc) rc = wr(t, PGT_FLASH_IRES, (uint8_t)(ires | mask));
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch)
        if (mask >> ch & 1u) rc = wr(t, (uint8_t)PGT_FLASH_ITARGET(ch), (uint8_t)((it[ch] & ~PGT_ITARGET_MASK) | code));
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch)
        if (mask >> ch & 1u) rc = wr(t, (uint8_t)PGT_FLASH_CHAN_TIMER(ch), 0);
    if (!rc) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)((ctl & ~PGT_MODULE_EN) | PGT_MODULE_EN));
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch)
        if (mask >> ch & 1u)
            rc = wr(t, (uint8_t)PGT_FLASH_CHAN_STROBE(ch), (uint8_t)((st[ch] & ~PGT_STROBE_FIELD) | PGT_STROBE_SW_TORCH));
    if (rc) return fault(t, "on-write", rc);
    /* read back before the channels go on */
    uint8_t v = 0;
    rc = rd(t, PGT_FLASH_IRES, &v);
    int bad = !rc && (v & mask) != mask;
    if (!rc && !bad) {
        rc = rd(t, PGT_FLASH_EN_CTL, &v);
        bad = !rc && !(v & PGT_MODULE_EN);
    }
    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc && !bad; ++ch) {
        if (!(mask >> ch & 1u)) continue;
        rc = rd(t, (uint8_t)PGT_FLASH_ITARGET(ch), &v);
        bad = !rc && ((v & PGT_ITARGET_MASK) != code || code > t->code_max);
        if (!rc && !bad) {
            rc = rd(t, (uint8_t)PGT_FLASH_CHAN_TIMER(ch), &v);
            bad = !rc && v != 0;
        }
        if (!rc && !bad) {
            rc = rd(t, (uint8_t)PGT_FLASH_CHAN_STROBE(ch), &v);
            bad = !rc && (v & PGT_STROBE_FIELD) != PGT_STROBE_SW_TORCH;
        }
    }
    if (rc || bad) return fault(t, rc ? "verify-read" : "verify", rc ? rc : E_IO);
    rc = wr(t, PGT_FLASH_CHAN_EN, (uint8_t)((en & ~mask) | mask));
    if (rc) return fault(t, "enable-write", rc);
    t->state = PGT_ST_ON;
    t->code = code;
    t->ma = pgt_code_ma(code);
    rc = read_status(t, t->sts);
    if (rc) return fault(t, "status-read", rc);
    const char *f = fault_of(t, t->sts);
    return f ? fault(t, f, E_IO) : 0;
}

static void event_line(PgtCore *t, const char *what, const char *why)
{
    if (!event_line_allowed(t)) return;
    char buf[LINE];
    PgtText x;
    begin(&x, buf);
    pgt_text_str(&x, what);
    pgt_text_kd(&x, "level", t->level);
    pgt_text_kd(&x, "ma", t->ma);
    pgt_text_kd(&x, "code", t->code);
    pgt_text_ks(&x, "by", why ? why : "-");
    const uint64_t n = now_us(t);
    if (t->state == PGT_ST_ON) {
        pgt_text_kd(&x, "off_in_ms", t->deadline_us > n ? (int64_t)((t->deadline_us - n) / 1000u) : 0);
    } else {
        pgt_text_kd(&x, "on_ms", (int64_t)((n - t->on_us) / 1000u));
        pgt_text_kd(&x, "ons", t->ons);
    }
    if (t->dropped) pgt_text_kd(&x, "dropped", t->dropped);
    trace(t, buf);
}

int pgt_core_set(PgtCore *t, unsigned level, uint32_t hold_ms, const char *why)
{
    if (!t || !t->ops) return E_INVAL;
    if (t->state == PGT_ST_FAULT) return E_IO;
    if (t->state != PGT_ST_OFF && t->state != PGT_ST_ON) return E_NODEV;
    if (level > PGT_LEVEL_MAX) return E_INVAL;
    if (!level) {
        if (t->state != PGT_ST_ON) return 0;
        const int rc = torch_off(t);
        if (rc) return rc;
        t->offs++;
        event_line(t, "off", why);
        return 0;
    }
    unsigned code = pgt_ma_code(pgt_level_ma(level, t->cfg.max_ma));
    if (code > t->code_max) code = t->code_max;
    const int was_on = t->state == PGT_ST_ON;
    if (!was_on || code != t->code) {
        const int rc = torch_on(t, (uint8_t)code);
        if (rc) return rc;
    }
    t->level = level;
    const uint64_t n = now_us(t);
    if (!was_on) {
        t->ons++;
        t->on_us = n;
        uint64_t ms = t->cfg.auto_off_ms;
        if (hold_ms && hold_ms < ms) ms = hold_ms;
        t->deadline_us = n + ms * 1000u;
    } else if (hold_ms && n + (uint64_t)hold_ms * 1000u < t->deadline_us) {
        t->deadline_us = n + (uint64_t)hold_ms * 1000u;
    }
    event_line(t, was_on ? "level" : "on", why);
    return 0;
}

uint32_t pgt_core_poll(PgtCore *t)
{
    if (!t || !t->ops || t->state != PGT_ST_ON) return 0;
    int rc = read_status(t, t->sts);
    if (rc) {
        fault(t, "status-read", rc);
        return 0;
    }
    const char *f = fault_of(t, t->sts);
    if (f) {
        fault(t, f, E_IO);
        return 0;
    }
    const uint64_t n = now_us(t);
    if (n >= t->deadline_us) {
        rc = torch_off(t);
        if (rc) return 0;
        t->offs++;
        t->auto_offs++;
        event_line(t, "off", "auto-off");
        return 0;
    }
    const uint64_t left_ms = (t->deadline_us - n + 999u) / 1000u;
    return left_ms < t->cfg.poll_ms ? (uint32_t)left_ms : t->cfg.poll_ms;
}

void pgt_core_export(const PgtCore *t, int32_t w[16])
{
    for (unsigned i = 0; i < 16; ++i) w[i] = 0;
    if (!t) return;
    const PgtPeriph *p = &t->spmi.p[PGT_FLASH];
    w[0] = t->stage;
    w[1] = t->state;
    w[2] = (int32_t)t->level;
    w[3] = (int32_t)t->ma;
    w[4] = t->code;
    w[5] = (int32_t)t->ons;
    w[6] = (int32_t)t->offs;
    w[7] = (int32_t)t->faults;
    w[8] = (int32_t)((uint32_t)t->sts[0] << 16 | (uint32_t)t->sts[1] << 8 | t->sts[2]);
    w[9] = (int32_t)t->spmi.writes;
    w[10] = (int32_t)t->spmi.refused;
    w[11] = p->apid;
    w[12] = p->owner;
    w[13] = (int32_t)((uint32_t)p->type << 8 | p->subtype);
    w[14] = (int32_t)t->auto_offs;
    w[15] = t->last_rc;
}
