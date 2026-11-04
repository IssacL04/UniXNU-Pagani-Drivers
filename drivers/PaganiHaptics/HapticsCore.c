/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The haptics driver logic (HapticsCore.h). Register sequences follow the facts in HapticsRegs.h ([Q] the 13T's Linux driver, [U] the
 * upstream HAP530 driver); the order of each sequence is documented where it is written. */
#include "HapticsCore.h"
#include "HapticsRegs.h"
#include "HapticsText.h"

enum { E_PERM = -1, E_IO = -5, E_BUSY = -16, E_NODEV = -19, E_INVAL = -22 };
enum {
    POLL_STREAM_MS = 4, POLL_WAIT_MS = 2, POLL_PLAY_MS = 20, POLL_EFFECT_MS = 5,
    FIFO_INIT_FILL = 320,        /* the first chunk before the play starts ([Q] FIFO_PRGM_INIT_SIZE, [U] FIFO_INIT_FILL) */
    REFILL_WRITES = 256,         /* 4-byte FIFO writes per poll at most (about 2.5 ms of bus time) */
    STREAM_IDLE_MS = 500,        /* a begun stream without data is dropped */
    STREAM_PREBUFFER_MS = 8,     /* a begun stream starts once this much is there: two polls of the FIFO, so the first refill finds the
                                  * FIFO not dry, and all of it counts as latency (the iOS bridge's target is 30 ms from the audio server's
                                  * write to the motor: 8 ms of its lead, this, the 2 ms poll and the start's bus time) */
    UNDERRUN_MS = 100,           /* FIFO and ring empty this long without an end: the producer is gone */
    F0_MIN_PLAY_US = 120000,     /* a DIRECT_PLAY with auto resonance this long gives a closed-loop period */
    MIN_BUDGET_MS = 30,
};

static int playing(int state) {return state == PGH_ST_DIRECT || state == PGH_ST_EFFECT || state == PGH_ST_STREAM;}
static uint64_t now(PghCore *h) {return h->ops.now_us(h->ops.ctx);}
static void wait(PghCore *h, unsigned us) {h->ops.wait_us(h->ops.ctx, us);}
static uint32_t period(const PghCore *h) {return h->cl_t_lra_us ? h->cl_t_lra_us : h->cfg.t_lra_us;}

static void emit(PghCore *h, PghText *t)
{
    h->log_lines++;
    if (h->ops.trace) h->ops.trace(h->ops.ctx, t->buf);
}

/* ---- faults: the play is stopped (SPMI_PLAY = 0, also after the bus closed) and the driver stays off ---- */
static void fault(PghCore *h, const char *what, int rc)
{
    if (h->state == PGH_ST_FAULT) return;
    const int was = h->state;
    const int stop = h->spmi.p[PGH_CFG].writable ? pgh_spmi_emergency_stop(&h->spmi) : E_NODEV;   /* E_NODEV: nothing was writable */
    h->state = PGH_ST_FAULT;
    h->stage = -9;
    h->fault_rc = rc;
    char line[200];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] FAULT ");
    pgh_text_str(&t, what);
    pgh_text_kd(&t, "rc", rc);
    pgh_text_kd(&t, "periph", h->spmi.last_periph);
    pgh_text_kx(&t, "reg", h->spmi.last_reg);
    pgh_text_kx(&t, "status", h->spmi.last_status);
    pgh_text_kd(&t, "state", was);
    pgh_text_kd(&t, "stop", stop);
    pgh_text_str(&t, ": off until reboot");
    emit(h, &t);
}

/* ---- register helpers: every bus error is a fault ---- */
static int rdn(PghCore *h, unsigned p, uint8_t reg, uint8_t *v, unsigned n)
{
    const int rc = pgh_spmi_read(&h->spmi, p, reg, v, n);
    if (rc) fault(h, "read", rc);
    return rc;
}
static int rd(PghCore *h, unsigned p, uint8_t reg, uint8_t *v) {return rdn(h, p, reg, v, 1);}
static int wrn(PghCore *h, unsigned p, uint8_t reg, const uint8_t *v, unsigned n)
{
    const int rc = pgh_spmi_write(&h->spmi, p, reg, v, n);
    if (rc) fault(h, rc == E_PERM ? "write refused" : "write", rc);
    return rc;
}
static int wr(PghCore *h, unsigned p, uint8_t reg, uint8_t v) {return wrn(h, p, reg, &v, 1);}
/* read-modify-write, written only when it changes (as regmap_update_bits) */
static int update(PghCore *h, unsigned p, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old = 0;
    const int rc = rd(h, p, reg, &old);
    if (rc) return rc;
    const uint8_t next = (uint8_t)((old & ~mask) | (value & mask));
    return next == old ? 0 : wr(h, p, reg, next);
}
static int status_data(PghCore *h, uint16_t sel, uint8_t *msb, uint8_t *lsb)
{
    int rc = 0;
    if (h->sel != sel) {
        h->sel = 0xffff;
        rc = wr(h, PGH_CFG, PGH_CFG_MOD_STATUS_XT, (uint8_t)(sel >> 8));
        if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_MOD_STATUS_SEL, (uint8_t)(sel & 0xffu));
        if (rc) return rc;
        h->sel = sel;
    }
    uint8_t d[2] = {0, 0};
    rc = rdn(h, PGH_CFG, PGH_CFG_STATUS_DATA_MSB, d, 2);
    *msb = d[0];
    *lsb = d[1];
    return rc;
}

/* ---- the on-time budget: refills with time, drains while playing ---- */
static void budget_tick(PghCore *h)
{
    const uint64_t t = now(h);
    const uint64_t dt = t > h->t_budget ? t - h->t_budget : 0;
    h->t_budget = t;
    int64_t b = h->budget_us + (int64_t)(dt * h->cfg.refill_permille / 1000u);
    if (playing(h->state)) b -= (int64_t)dt;
    const int64_t full = (int64_t)h->cfg.budget_ms * 1000;
    h->budget_us = b > full ? full : b;
}

uint32_t pgh_core_vmax(PghCore *h, uint32_t mv)
{
    uint32_t cap = h->cfg.cap_mv;
    if (cap > h->cfg.hard_mv) cap = h->cfg.hard_mv;
    if (cap > h->hw_max_mv) cap = h->hw_max_mv;
    if (cap > PGH_VMAX_PLUS_HDRM_MAX - PGH_HDRM_DEFAULT_MV) cap = PGH_VMAX_PLUS_HDRM_MAX - PGH_HDRM_DEFAULT_MV;
    if (h->clamp_5v && cap > PGH_CLAMPED_VMAX_MV) cap = PGH_CLAMPED_VMAX_MV;
    if (h->ops.battery && h->cfg.vbat_low_mv && cap > h->cfg.vbat_low_mv) {
        int percent = -1, deci_c = 250;
        h->ops.battery(h->ops.ctx, &percent, &deci_c);
        const int low = deci_c >= h->cfg.vbat_cold_dc ? h->cfg.vbat_low_soc : h->cfg.vbat_cold_soc;
        if (percent >= 0 && percent < low) cap = h->cfg.vbat_low_mv;
    }
    if (mv > cap) {
        mv = cap;
        h->clips++;
    }
    return mv - mv % PGH_VMAX_STEP_MV;
}

/* ---- HAP530 building blocks [Q] ---- */
static int toggle_enable(PghCore *h)
{
    wait(h, 100);
    int rc = wr(h, PGH_CFG, PGH_CFG_EN_CTL, 0);
    if (!rc) {
        wait(h, PGH_EN_DELAY_US);
        rc = wr(h, PGH_CFG, PGH_CFG_EN_CTL, PGH_HAPTICS_EN);
    }
    h->en_on = rc ? 0 : 1;
    return rc;
}

static int ensure_enabled(PghCore *h)
{
    if (h->en_on) return 0;
    uint8_t v = 0;
    int rc = rd(h, PGH_CFG, PGH_CFG_EN_CTL, &v);
    if (!rc && !(v & PGH_HAPTICS_EN)) {
        wait(h, PGH_EN_DELAY_US);   /* HAP530: wait before enabling (VNDRV overshoot) */
        rc = wr(h, PGH_CFG, PGH_CFG_EN_CTL, PGH_HAPTICS_EN);
    }
    if (!rc) h->en_on = 1;
    return rc;
}

/* After a VMAX write HPWR must show disabled or ready within 10 ms; then the VNDRV LDO discharges for 500 us. Otherwise the module is
 * toggled (as [Q]) and the play does not start (-16, not a fault). */
static int hpwr_settle(PghCore *h)
{
    for (unsigned i = 0; i < PGH_HPWR_POLLS; ++i) {
        uint8_t v = 0;
        const int rc = rd(h, PGH_CFG, PGH_CFG_HPWR_INTF, &v);
        if (rc) return rc;
        v &= PGH_HPWR_MASK;
        if (v == PGH_HPWR_DISABLED || v == PGH_HPWR_READY) {
            wait(h, PGH_DISCHARGE_US);
            return 0;
        }
        wait(h, PGH_HPWR_POLL_US);
    }
    h->hpwr_timeouts++;
    const int rc = toggle_enable(h);
    return rc ? rc : E_BUSY;
}

/* VMAX on HAP530 [Q]: zero-cross timeout fault powered down and the closed-loop VDRIVE off around the write, faults cleared first. */
static int set_vmax(PghCore *h, uint32_t mv)
{
    const uint8_t code = (uint8_t)(mv / PGH_VMAX_STEP_MV);
    if (code == h->vmax_code) return 0;
    uint8_t vdrive = 0;
    int rc = update(h, PGH_CFG, PGH_CFG_ZX_SYNC_CFG, PGH_EN_PDN_ZX_TO_FAULT, PGH_EN_PDN_ZX_TO_FAULT);
    if (!rc) rc = rd(h, PGH_PTN, PGH_PTN_CL_VDRIVE_CTL, &vdrive);
    if (!rc && (vdrive & PGH_EN_CL_VDRIVE)) rc = wr(h, PGH_PTN, PGH_PTN_CL_VDRIVE_CTL, (uint8_t)(vdrive & ~PGH_EN_CL_VDRIVE));
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_FAULT_CLR, PGH_FAULT_CLR_ALL);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_VMAX, code);
    if (rc) return rc;
    h->vmax_code = code;
    const int settle = hpwr_settle(h);
    if (h->state == PGH_ST_FAULT) return settle;
    rc = update(h, PGH_CFG, PGH_CFG_ZX_SYNC_CFG, PGH_EN_PDN_ZX_TO_FAULT, 0);
    if (!rc && (vdrive & PGH_EN_CL_VDRIVE)) rc = wr(h, PGH_PTN, PGH_PTN_CL_VDRIVE_CTL, vdrive);
    return rc ? rc : settle;
}

/* HBOOST read only: busy when enabled in open loop by someone else (not following the haptics module); its 5 V clamp. */
static int boost_free(PghCore *h)
{
    uint8_t vreg = 0, follow = 0, clamp = 0;
    int rc = rd(h, PGH_HBST, PGH_HBST_VREG_EN, &vreg);
    if (!rc) rc = rd(h, PGH_HBST, PGH_HBST_HW_CTRL_FOLLOW, &follow);
    if (!rc) rc = rd(h, PGH_HBST, PGH_HBST_CLAMP, &clamp);
    if (rc) return rc;
    h->clamp_5v = (uint8_t)(clamp & PGH_HBST_CLAMP_5V);
    if ((vreg & PGH_HBST_VREG_ON) && !(follow & PGH_FOLLOW_HW_EN)) {
        h->busy++;
        return E_BUSY;
    }
    return 0;
}

/* Brake [Q]: after PLAY_EN is cleared the hardware plays the brake samples (half an LRA period each); HPWR goes to disabled when
 * done. Polled every half period up to the brake length plus one period; then 500 us for the VNDRV discharge. A timeout toggles the
 * module. */
static int brake_wait(PghCore *h)
{
    const uint32_t t = period(h), step = t / 2 ? t / 2 : 1;
    const uint32_t brake_us = h->brake_on ? t * h->cfg.brake_len / 2 : 0;
    unsigned polls = (brake_us + t) / step + 1;
    while (polls--) {
        wait(h, step);
        uint8_t v = 0;
        const int rc = rd(h, PGH_CFG, PGH_CFG_HPWR_INTF, &v);
        if (rc) return rc;
        if ((v & PGH_HPWR_MASK) == PGH_HPWR_DISABLED) {
            wait(h, PGH_DISCHARGE_US);
            return 0;
        }
    }
    h->hpwr_timeouts++;
    return toggle_enable(h);
}

static int write_tlra(PghCore *h, uint32_t us)
{
    const uint32_t code = us / PGH_TLRA_STEP_US;
    const uint8_t v[2] = {(uint8_t)((code >> 8) & 0x0fu), (uint8_t)(code & 0xffu)};
    return wrn(h, PGH_CFG, PGH_CFG_TLRA_OL_HIGH, v, 2);
}

/* The closed-loop period after a DIRECT_PLAY with auto resonance ([Q] haptics_get_closeloop_lra_period, the two "done" cases). */
static void measure_f0(PghCore *h)
{
    uint8_t msb = 0, lsb = 0, cal_en = 0;
    if (status_data(h, PGH_STS_CAL_TLRA_CL, &msb, &lsb) || !(msb & PGH_AUTO_RES_CAL_DONE)) return;
    const uint64_t cal = (uint64_t)(msb & 0x1fu) << 8 | lsb;
    if (rd(h, PGH_CFG, PGH_CFG_CAL_EN, &cal_en) || !cal) return;
    uint64_t us = 0;
    if ((cal_en & PGH_CAL_RC_CLK_MASK) == PGH_CAL_RC_CLK_AUTO) {
        uint8_t gm = 0, gl = 0;
        if (status_data(h, PGH_STS_LAST_GOOD_TLRA_CL, &gm, &gl)) return;
        const uint64_t last = (uint64_t)(gm & 0x1fu) << 8 | gl;
        us = last * last * 833333u / cal / 1000000u;            /* 0.833333 us steps scaled by the RC clock calibration */
    } else if ((cal_en & PGH_CAL_RC_CLK_MASK) == PGH_CAL_RC_CLK_DISABLED) {
        us = cal * 3333000u / 1000000u;                         /* 3.333 us steps */
    } else {
        return;
    }
    char line[120];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] closed-loop period");
    pgh_text_kd(&t, "us", (int64_t)us);
    pgh_text_kx(&t, "cal", cal);
    pgh_text_kx(&t, "cal_en", cal_en);
    const uint64_t lo = 10000000u / (h->cfg.f0_max_dhz ? h->cfg.f0_max_dhz : 1), hi = 10000000u / (h->cfg.f0_min_dhz ? h->cfg.f0_min_dhz : 1);
    if (us >= lo && us <= hi) {
        h->tlra_pending = (uint8_t)(h->cl_t_lra_us != (uint32_t)us);
        h->cl_t_lra_us = (uint32_t)us;
        h->f0_dhz = (uint32_t)(10000000u / us);
        pgh_text_kd(&t, "f0_dhz", h->f0_dhz);
    } else {
        pgh_text_str(&t, " outside the accepted range: ignored");
    }
    emit(h, &t);
}

static void play_line(PghCore *h, const char *what, uint32_t a, uint32_t b)
{
    if (!h->cfg.log || (h->cfg.log == 1 && h->plays > 8 && h->plays % 100)) return;
    char line[160];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] ");
    pgh_text_str(&t, what);
    pgh_text_kd(&t, "a", a);
    pgh_text_kd(&t, "b", b);
    pgh_text_kd(&t, "mv", (int64_t)h->vmax_code * PGH_VMAX_STEP_MV);
    pgh_text_kd(&t, "n", h->plays);
    pgh_text_kd(&t, "budget_ms", h->budget_us / 1000);
    emit(h, &t);
}

/* ---- stop [Q]: PLAY_EN cleared first (brake enabled when the play had it), the brake waited for, then the restore steps ---- */
static int stop_play(PghCore *h, const char *why)
{
    if (h->state == PGH_ST_STREAM_WAIT) {
        h->state = PGH_ST_IDLE;
        return 0;
    }
    if (!playing(h->state)) return 0;
    budget_tick(h);
    if (h->state == PGH_ST_DIRECT && h->cfg.autores && h->autores_ok && now(h) - h->t_start >= F0_MIN_PLAY_US) {
        measure_f0(h);
        if (h->state == PGH_ST_FAULT) return h->fault_rc;
    }
    const int fifo = h->src == PGH_SRC_FIFO;
    int rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, (uint8_t)(h->src | (h->brake_on ? PGH_BRAKE_EN : 0)));
    if (rc) return rc;
    h->state = PGH_ST_IDLE;
    h->stops++;
    rc = brake_wait(h);
    if (!rc && fifo) rc = update(h, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, PGH_FIFO_RATE_MASK, PGH_RATE_T_LRA);
    if (!rc && fifo) rc = update(h, PGH_CFG, PGH_CFG_CAL_EN, PGH_CAL_RC_CLK_MASK, PGH_CAL_RC_CLK_AUTO);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_VMAX_HDRM, (uint8_t)(PGH_HDRM_DEFAULT_MV / PGH_VMAX_STEP_MV));   /* HAP530: back to 1.5 V */
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_TRIG_PRIORITY, PGH_SWR_IGNORE, 0);
    if (!rc && h->tlra_pending) {   /* the measured resonance becomes the open-loop period (FIFO effects at T_LRA) */
        rc = write_tlra(h, h->cl_t_lra_us);
        h->tlra_pending = 0;
    }
    if (h->cfg.log >= 2 || (h->cfg.log == 1 && fifo && h->rate >= PGH_RATE_F_8KHZ)) {
        char line[160];
        PghText t;
        pgh_text_start(&t, line, sizeof line);
        pgh_text_str(&t, "[haptics] stop ");
        pgh_text_str(&t, why);
        pgh_text_kd(&t, "rc", rc);
        pgh_text_kd(&t, "ms", (int64_t)((now(h) - h->t_start) / 1000u));
        pgh_text_kd(&t, "samples", (int64_t)h->samples);
        pgh_text_kd(&t, "underruns", h->underruns);
        emit(h, &t);
    }
    return rc;
}

/* The common start of every play: the boost free (and its 5 V clamp read before the voltage is chosen), SWR plays ignored during the
 * SPMI play (HAP530), the drive voltage with every cap applied. */
static int prologue(PghCore *h, uint32_t request_mv, uint32_t *chosen)
{
    int rc = boost_free(h);
    if (rc) return rc;
    const uint32_t mv = pgh_core_vmax(h, request_mv);
    if (mv < PGH_VMAX_STEP_MV) return E_INVAL;
    if (chosen) *chosen = mv;
    rc = update(h, PGH_CFG, PGH_CFG_TRIG_PRIORITY, PGH_SWR_IGNORE, PGH_SWR_IGNORE);
    if (!rc) rc = set_vmax(h, mv);
    if (rc == E_BUSY && h->state != PGH_ST_FAULT) update(h, PGH_CFG, PGH_CFG_TRIG_PRIORITY, PGH_SWR_IGNORE, 0);
    return rc;
}

static int budget_limit(PghCore *h, uint32_t *ms, uint32_t max_ms)
{
    budget_tick(h);
    const int64_t left = h->budget_us / 1000;
    if (left < MIN_BUDGET_MS) {
        h->refused++;
        return E_BUSY;
    }
    uint32_t limit = max_ms;
    if ((int64_t)limit > left) limit = (uint32_t)left;
    if (*ms > limit) {
        *ms = limit;
        h->clips++;
    }
    return 0;
}

/* The final steps of every start: faults cleared, the module enabled, then SPMI_PLAY with PLAY_EN last. */
static int trigger(PghCore *h, uint8_t src, int brake, int next_state)
{
    int rc = wr(h, PGH_CFG, PGH_CFG_FAULT_CLR, PGH_FAULT_CLR_ALL);
    if (!rc) rc = ensure_enabled(h);
    h->src = src;
    h->brake_on = (uint8_t)(brake ? 1 : 0);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, (uint8_t)(PGH_PLAY_EN | (brake ? PGH_BRAKE_EN : 0) | src));
    if (rc) return rc;
    h->state = next_state;
    h->t_start = now(h);
    h->plays++;
    return 0;
}

/* ---- DIRECT_PLAY [Q][U]: amplitude, auto resonance per policy, brake from the board ---- */
static int play_constant(PghCore *h, uint32_t permille, uint32_t ms, uint32_t mv_request)
{
    if (!permille || !ms) return E_INVAL;
    if (permille > 1000) permille = 1000;
    int rc = stop_play(h, "replaced");
    if (rc) return rc;
    rc = budget_limit(h, &ms, h->cfg.max_play_ms);
    if (rc) return rc;
    rc = prologue(h, mv_request ? mv_request : h->cfg.vmax_mv, 0);
    if (!rc) rc = wr(h, PGH_PTN, PGH_PTN_DIRECT_PLAY, (uint8_t)((255u * permille + 999u) / 1000u));
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_AUTORES_CFG, PGH_AUTORES_EN, h->cfg.autores && h->autores_ok ? PGH_AUTORES_EN : 0);
    if (!rc) rc = trigger(h, PGH_SRC_DIRECT, h->cfg.brake_len > 0, PGH_ST_DIRECT);
    if (rc) return rc;
    h->deadline = h->t_start + (uint64_t)ms * 1000u;
    play_line(h, "constant", permille, ms);
    return 0;
}

/* FIFO data: 4-byte writes to FIFO_DIN_0..3, the last 1..3 samples through FIFO_DIN_1B ([Q]: only at the very end, so the hardware
 * never pads in the middle). Samples come from a buffer or the ring (wrapping). */
static int fifo_put(PghCore *h, const int8_t *base, uint32_t from, uint32_t mask, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint8_t d[4];
        for (unsigned k = 0; k < 4; ++k) d[k] = (uint8_t)base[(from + i + k) & mask];
        const int rc = wrn(h, PGH_PTN, PGH_PTN_FIFO_DIN_0, d, 4);
        if (rc) return rc;
    }
    for (; i < n; ++i) {
        const int rc = wr(h, PGH_PTN, PGH_PTN_FIFO_DIN_1B, (uint8_t)base[(from + i) & mask]);
        if (rc) return rc;
    }
    h->samples += n;
    return 0;
}

/* The common start of a FIFO play [Q][U]: auto resonance off, RC clock calibration off for kHz streams (as OPLUS streams), the rate,
 * the FIFO flushed, the fill status selected. */
static int fifo_prologue(PghCore *h, uint8_t rate, uint32_t request_mv, uint32_t *chosen)
{
    int rc = prologue(h, request_mv, chosen);
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_AUTORES_CFG, PGH_AUTORES_EN, 0);
    if (!rc && rate >= PGH_RATE_F_8KHZ) rc = update(h, PGH_CFG, PGH_CFG_CAL_EN, PGH_CAL_RC_CLK_MASK, PGH_CAL_RC_CLK_DISABLED);
    if (!rc) rc = update(h, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, PGH_FIFO_RATE_MASK, rate);
    if (!rc) rc = wr(h, PGH_PTN, PGH_PTN_MEM_OP_ACCESS, PGH_MEM_FLUSH_RELOAD);
    if (!rc) rc = wr(h, PGH_PTN, PGH_PTN_MEM_OP_ACCESS, 0);
    uint8_t msb = 0, lsb = 0;
    if (!rc) rc = status_data(h, PGH_STS_FIFO_REAL_TIME, &msb, &lsb);
    h->rate = rate;
    return rc;
}

/* ---- effects: the board's FIFO effects at the LRA period (one 0x7f sample per period), no brake (wf-brake-disable) ---- */
static int play_effect(PghCore *h, uint32_t id, uint32_t permille)
{
    if (id >= h->cfg.effects || !permille) return E_INVAL;
    if (permille > 1000) permille = 1000;
    const uint32_t cycles = h->cfg.effect_cycles[id];
    int rc = stop_play(h, "replaced");
    if (rc || !cycles) return rc;
    uint32_t ms = (cycles + 2u) * period(h) / 1000u + 20u;
    rc = budget_limit(h, &ms, h->cfg.max_play_ms);
    if (rc) return rc;
    int8_t samples[16];
    for (unsigned i = 0; i < sizeof samples; ++i) samples[i] = 0x7f;
    rc = fifo_prologue(h, PGH_RATE_T_LRA, h->cfg.effect_mv * permille / 1000u, 0);
    if (!rc) rc = fifo_put(h, samples, 0, 15u, cycles > 16 ? 16 : cycles);
    if (!rc) rc = trigger(h, PGH_SRC_FIFO, 0, PGH_ST_EFFECT);
    if (rc) return rc;
    h->deadline = h->t_start + (uint64_t)ms * 1000u;
    play_line(h, "effect", id, permille);
    return 0;
}

/* ---- streams ---- */
static int rate_code(uint32_t hz)
{
    switch (hz) {
    case 8000: return PGH_RATE_F_8KHZ;
    case 16000: return PGH_RATE_F_16KHZ;
    case 24000: return PGH_RATE_F_24KHZ;
    case 32000: return PGH_RATE_F_32KHZ;
    case 44100: return PGH_RATE_F_44P1KHZ;
    case 48000: return PGH_RATE_F_48KHZ;
    default: return -1;
    }
}

static uint32_t ring_tail(const PghCore *h) {return h->ring.tail;}
static void ring_advance(PghCore *h, uint32_t n) {__atomic_store_n(&h->ring.tail, h->ring.tail + n, __ATOMIC_RELEASE);}
static void ring_discard(PghCore *h) {__atomic_store_n(&h->ring.tail, pgh_ring_head(&h->ring), __ATOMIC_RELEASE);}

unsigned pgh_ring_push(PghRing *r, const int8_t *samples, unsigned n)
{
    if (!r || !samples) return 0;
    const uint32_t head = r->head;   /* only the producer writes it */
    const uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    const uint32_t space = PGH_RING - (head - tail);
    if (n > space) n = space;
    for (unsigned i = 0; i < n; ++i) r->data[(head + i) & (PGH_RING - 1)] = samples[i];
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

static int stream_begin(PghCore *h, uint32_t hz, uint32_t mv, uint32_t first)
{
    const int code = rate_code(hz);
    if (code < 0) return E_INVAL;
    if (!h->fifo_len) return E_NODEV;
    int rc = stop_play(h, "replaced");
    if (rc) return rc;
    const uint32_t head = pgh_ring_head(&h->ring), tail = ring_tail(h);
    /* samples written before this stream began are dropped */
    if (first - tail <= head - tail) ring_advance(h, first - tail);
    else ring_discard(h);
    h->state = PGH_ST_STREAM_WAIT;
    h->rate = (uint8_t)code;
    h->stream_rate_hz = hz;
    h->stream_end = 0;
    h->ending = 0;
    h->t_start = now(h);
    h->t_empty = 0;
    h->stream_mv = mv;
    h->streams++;
    return 0;
}

static int stream_start(PghCore *h, uint32_t avail)
{
    uint32_t ms = h->cfg.max_stream_ms;
    int rc = budget_limit(h, &ms, h->cfg.max_stream_ms);
    if (rc) {
        h->state = PGH_ST_IDLE;
        ring_discard(h);
        return rc;
    }
    uint32_t mv = 0;
    rc = fifo_prologue(h, h->rate, h->stream_mv ? h->stream_mv : h->cfg.cap_mv, &mv);
    uint32_t n = avail < FIFO_INIT_FILL ? avail : FIFO_INIT_FILL;
    if (!(h->ending && n == avail)) n &= ~3u;
    if (!rc) rc = fifo_put(h, h->ring.data, ring_tail(h), PGH_RING - 1, n);
    if (!rc) ring_advance(h, n);
    if (!rc) rc = trigger(h, PGH_SRC_FIFO, 0, PGH_ST_STREAM);
    if (rc) {
        if (h->state != PGH_ST_FAULT) h->state = PGH_ST_IDLE;
        ring_discard(h);
        return rc;
    }
    h->deadline = h->t_start + (uint64_t)ms * 1000u;
    h->t_empty = 0;
    play_line(h, "stream", h->stream_rate_hz, mv);
    return 0;
}

static int stream_end(PghCore *h, uint32_t drain, uint32_t end)
{
    if (h->state != PGH_ST_STREAM && h->state != PGH_ST_STREAM_WAIT) return 0;
    if (!drain) {
        const int rc = stop_play(h, "aborted");
        ring_discard(h);
        return rc;
    }
    const uint32_t head = pgh_ring_head(&h->ring), tail = ring_tail(h);
    h->stream_end = end - tail <= head - tail ? end : head;
    h->ending = 1;
    return 0;
}

/* ---- faults while playing: a short circuit is fatal; an HPWR or auto-resonance fault stops the play ---- */
static int check_faults(PghCore *h)
{
    uint8_t v = 0;
    int rc = rd(h, PGH_CFG, PGH_CFG_FAULT_STATUS, &v);
    if (rc) return rc;
    if (!(v & (PGH_FAULT_SC | PGH_FAULT_HPWR_RDY | PGH_FAULT_AUTO_RES))) return 0;
    h->faults_seen |= v;
    if (v & PGH_FAULT_SC) {
        fault(h, "short circuit", E_IO);
        return E_IO;
    }
    if (v & PGH_FAULT_AUTO_RES) h->autores_ok = 0;   /* open loop from now on */
    rc = stop_play(h, (v & PGH_FAULT_HPWR_RDY) ? "hpwr fault" : "auto-resonance fault");
    return rc ? rc : E_BUSY;
}

static uint32_t poll_stream(PghCore *h)
{
    const uint64_t t = now(h);
    budget_tick(h);
    if (h->budget_us <= 0 || t >= h->deadline) {
        h->clips++;
        stop_play(h, h->budget_us <= 0 ? "budget" : "max length");
        ring_discard(h);
        return 0;
    }
    if (check_faults(h)) return 0;
    uint8_t msb = 0, lsb = 0;
    if (status_data(h, PGH_STS_FIFO_REAL_TIME, &msb, &lsb)) return 0;
    uint32_t fill = (uint32_t)(msb & PGH_FIFO_FILL_MSB_MASK) << 8 | lsb;
    if (fill > h->fifo_len) fill = h->fifo_len;
    const uint32_t head = pgh_ring_head(&h->ring), tail = ring_tail(h);
    uint32_t avail = head - tail;
    if (h->ending && h->stream_end - tail < avail) avail = h->stream_end - tail;
    uint32_t n = h->fifo_len - fill;
    if (n > avail) n = avail;
    if (n > REFILL_WRITES * 4u) n = REFILL_WRITES * 4u;
    if (!(h->ending && n == avail)) n &= ~3u;
    if (n) {
        if (fifo_put(h, h->ring.data, tail, PGH_RING - 1, n)) return 0;
        ring_advance(h, n);
    }
    const int dry = fill == 0;   /* the fill count decides; the empty flag's exact meaning is not relied on */
    if (h->ending && !avail && dry) {   /* everything up to the end was written and has played */
        stop_play(h, "drained");
        return 0;
    }
    if (dry && !h->ending) {   /* the FIFO ran dry: late data (counted), or no producer for UNDERRUN_MS (stopped) */
        if (!h->t_empty) {
            h->t_empty = t;
            h->underruns++;
        } else if (!n && t - h->t_empty >= (uint64_t)UNDERRUN_MS * 1000u) {
            stop_play(h, "underrun");
            ring_discard(h);
            return 0;
        }
        if (n) h->t_empty = 0;
    } else {
        h->t_empty = 0;
    }
    return POLL_STREAM_MS;
}

uint32_t pgh_core_poll(PghCore *h)
{
    if (!h) return 0;
    switch (h->state) {
    case PGH_ST_DIRECT: {
        budget_tick(h);
        const uint64_t t = now(h);
        if (t >= h->deadline || h->budget_us <= 0) {
            if (h->budget_us <= 0) h->clips++;
            stop_play(h, h->budget_us <= 0 ? "budget" : "done");
            return 0;
        }
        if (check_faults(h)) return 0;
        const uint64_t left = (h->deadline - t + 999u) / 1000u;
        return left < POLL_PLAY_MS ? (uint32_t)left : POLL_PLAY_MS;
    }
    case PGH_ST_EFFECT: {
        uint8_t msb = 0, lsb = 0;
        if (check_faults(h) || status_data(h, PGH_STS_FIFO_REAL_TIME, &msb, &lsb)) return 0;
        const uint32_t fill = (uint32_t)(msb & PGH_FIFO_FILL_MSB_MASK) << 8 | lsb;
        if (fill == 0 || now(h) >= h->deadline) {
            stop_play(h, "done");
            return 0;
        }
        return POLL_EFFECT_MS;
    }
    case PGH_ST_STREAM_WAIT: {
        const uint32_t avail = pgh_ring_head(&h->ring) - ring_tail(h);
        uint32_t need = h->stream_rate_hz * STREAM_PREBUFFER_MS / 1000u;
        if (need < 64) need = 64;
        if (need > 1024) need = 1024;
        const uint32_t until_end = h->ending ? h->stream_end - ring_tail(h) : avail;
        if (h->ending && !until_end) {
            h->state = PGH_ST_IDLE;
            return 0;
        }
        if (avail >= need || (h->ending && avail >= until_end)) {
            if (stream_start(h, avail < until_end ? avail : until_end)) return 0;
            return poll_stream(h);
        }
        if (now(h) - h->t_start >= (uint64_t)STREAM_IDLE_MS * 1000u) {
            h->state = PGH_ST_IDLE;
            ring_discard(h);
            return 0;
        }
        return POLL_WAIT_MS;
    }
    case PGH_ST_STREAM:
        return poll_stream(h);
    default:
        return 0;
    }
}

int pgh_core_command(PghCore *h, const PghCmd *cmd)
{
    if (!h || !cmd) return E_INVAL;
    if (h->state == PGH_ST_FAULT) return h->fault_rc ? h->fault_rc : E_IO;
    if (h->state == PGH_ST_OFF || h->state == PGH_ST_DRY) return E_NODEV;
    switch (cmd->op) {
    case PGH_CMD_STOP: {
        const int stream = h->state == PGH_ST_STREAM || h->state == PGH_ST_STREAM_WAIT;
        const int rc = stop_play(h, "stop");
        if (stream) ring_discard(h);
        return rc;
    }
    case PGH_CMD_CONSTANT: return play_constant(h, cmd->a, cmd->b, cmd->c);
    case PGH_CMD_EFFECT: return play_effect(h, cmd->a, cmd->b);
    case PGH_CMD_STREAM_BEGIN: return stream_begin(h, cmd->a, cmd->b, cmd->c);
    case PGH_CMD_STREAM_END: return stream_end(h, cmd->a, cmd->b);
    default: return E_INVAL;
    }
}

/* ---- init ---- */
static const uint8_t kDumpCfg[] = {0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x10, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x50,
                                   0x5c, 0x5d, 0x60, 0x62, 0x63, 0x67, 0x68, 0x6f, 0x70, 0x71, 0x72, 0x86};
static const uint8_t kDumpPtn[] = {0x08, 0x09, 0x24, 0x26, 0x2a, 0x2b, 0x2d, 0x46, 0x47, 0x4a, 0x70, 0x71, 0x72, 0x73, 0xa0, 0xa1,
                                   0xa2};
static const uint8_t kDumpHbst[] = {0x09, 0x0b, 0x41, 0x46, 0x54, 0x70};
static const uint8_t kDumpSdam[] = {0x46, 0x47, 0x75, 0x76, 0x7e};
static const char *const kName[PGH_PERIPHS] = {"cfg", "ptn", "hbst", "sdam"};

static void dump(PghCore *h, unsigned p, const uint8_t *regs, unsigned count)
{
    if (h->spmi.p[p].apid < 0) return;
    char line[224];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] reg ");
    pgh_text_str(&t, kName[p]);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t v = 0;
        const int rc = pgh_spmi_read(&h->spmi, p, regs[i], &v, 1);
        pgh_text_char(&t, ' ');
        pgh_text_hex2(&t, regs[i]);
        pgh_text_char(&t, '=');
        if (rc) pgh_text_str(&t, "??");
        else pgh_text_hex2(&t, v);
    }
    emit(h, &t);
}

static void spmi_lines(PghCore *h, int rc)
{
    char line[224];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] spmi");
    pgh_text_kd(&t, "rc", rc);
    pgh_text_kx(&t, "v", h->spmi.version);
    pgh_text_kd(&t, "ch", h->spmi.channels);
    pgh_text_kd(&t, "ee", h->cfg.ee);
    for (unsigned k = 0; k < PGH_PERIPHS; ++k) {
        const PghPeriph *p = &h->spmi.p[k];
        pgh_text_char(&t, ' ');
        pgh_text_str(&t, kName[k]);
        pgh_text_char(&t, '=');
        pgh_text_hex(&t, p->ppid);
        if (p->apid < 0) {
            pgh_text_str(&t, ":none");
            continue;
        }
        pgh_text_char(&t, ':');
        pgh_text_udec(&t, (uint64_t)p->apid);
        pgh_text_str(&t, ":ee");
        pgh_text_udec(&t, p->owner);
        pgh_text_str(&t, ":irq");
        pgh_text_udec(&t, p->irq);
        pgh_text_str(&t, ":x");
        pgh_text_udec(&t, p->mappings);
    }
    emit(h, &t);
}

static void id_line(PghCore *h, const char *verdict)
{
    char line[224];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] id");
    for (unsigned k = 0; k < PGH_PERIPHS; ++k) {
        const PghPeriph *p = &h->spmi.p[k];
        pgh_text_char(&t, ' ');
        pgh_text_str(&t, kName[k]);
        pgh_text_char(&t, '=');
        if (!p->identified) {
            pgh_text_str(&t, "?");
            continue;
        }
        pgh_text_hex2(&t, p->type);
        pgh_text_char(&t, '/');
        pgh_text_hex2(&t, p->subtype);
        pgh_text_str(&t, ":r");
        pgh_text_udec(&t, p->rev[1]);
        pgh_text_char(&t, '.');
        pgh_text_udec(&t, p->rev[0]);
        pgh_text_char(&t, '.');
        pgh_text_udec(&t, p->rev[3]);
        pgh_text_char(&t, '.');
        pgh_text_udec(&t, p->rev[2]);
    }
    pgh_text_ks(&t, "verdict", verdict ? verdict : "ok");
    emit(h, &t);
}

/* Dry run only: the CFG channel's RW page mapped and its STATUS register read, nothing written. Under Gunyah the HLOS VM must have
 * the arbiter's chnls window in its stage 2; this read shows it (a missing mapping faults here, before any write could). */
static void probe_channel(PghCore *h)
{
    const PghSpmiIo *io = h->spmi.io;
    const PghPeriph *p = &h->spmi.p[PGH_CFG];
    if (!io || !io->map_channel || !io->chan_read || p->apid < 0 || p->owner != h->cfg.ee) return;
    char line[120];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] chan cfg");
    pgh_text_kd(&t, "apid", p->apid);
    const int rc = io->map_channel(io->ctx, PGH_CFG, (unsigned)p->apid);
    pgh_text_kd(&t, "map", rc);
    if (!rc) pgh_text_kx(&t, "status", io->chan_read(io->ctx, PGH_CFG, PGH_ARB_STATUS));
    pgh_text_str(&t, " (read only)");
    emit(h, &t);
}

static const char *identity_problem(const PghCore *h)
{
    const PghPeriph *cfg = &h->spmi.p[PGH_CFG], *ptn = &h->spmi.p[PGH_PTN], *hb = &h->spmi.p[PGH_HBST];
    if (cfg->apid < 0 || ptn->apid < 0 || hb->apid < 0) return "no-channel";
    if (!cfg->identified || !ptn->identified || !hb->identified) return "unreadable";
    if (cfg->type != PGH_HAPTICS_TYPE || cfg->subtype != PGH_CFG_SUBTYPE_HAP530) return "cfg-type";
    if (cfg->rev[1] != PGH_HAP530_MAJOR || cfg->rev[0] > 1) return "cfg-revision";
    if (ptn->rev[1] != PGH_HAP530_MAJOR) return "ptn-revision";
    if (!ptn->type || ptn->type == 0xff) return "ptn-absent";
    if (h->cfg.ptn_type && (ptn->type != h->cfg.ptn_type || ptn->subtype != h->cfg.ptn_subtype)) return "ptn-type";
    if (!hb->type || hb->type == 0xff) return "hbst-absent";
    if (h->cfg.hbst_type && (hb->type != h->cfg.hbst_type || hb->subtype != h->cfg.hbst_subtype)) return "hbst-type";
    if (cfg->owner != h->cfg.ee) return "cfg-owner";
    if (ptn->owner != h->cfg.ee) return "ptn-owner";
    return 0;
}

/* The initialization [Q] (haptics_hw_init for HAP530 and the OPLUS probe values) in this order: a play left running stopped, faults
 * cleared, VMAX headroom 1.5 V, drive shape and two's complement FIFO data, brake mode and shape, RC clock calibration auto, the
 * open-loop LRA period, drive duty, auto-resonance configuration (off), no waveform hold, SWR not ignored at idle, no FIFO-empty
 * threshold (the FIFO is polled), FIFO rate T_LRA, the whole 8 KiB as FIFO [U], the brake samples, the module's VMAX. */
static int hw_init(PghCore *h)
{
    uint8_t v = 0;
    int rc = rd(h, PGH_CFG, PGH_CFG_HW_CONFIG, &v);
    if (rc) return rc;
    h->hw_max_mv = (v & PGH_HW_HV_DRIVER) ? 10000u : 6000u;
    rc = rd(h, PGH_CFG, PGH_CFG_SPMI_PLAY, &v);
    if (!rc && (v & PGH_PLAY_EN)) rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, 0);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_FAULT_CLR, PGH_FAULT_CLR_ALL);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_VMAX_HDRM, (uint8_t)(PGH_HDRM_DEFAULT_MV / PGH_VMAX_STEP_MV));
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_DRV_WF_SEL, PGH_DRV_WF_SEL_MASK | PGH_DRV_WF_FMT, (uint8_t)(h->cfg.drv_shape & 3u));
    if (!rc)
        rc = update(h, PGH_CFG, PGH_CFG_BRAKE_MODE_CFG, PGH_BRAKE_CFG_MASK,
                    (uint8_t)((h->cfg.brake_mode & 3u) << 6 | (h->cfg.brake_gain & 3u) << 2 | (h->cfg.brake_shape & 3u)));
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_CAL_EN, PGH_CAL_RC_CLK_MASK, PGH_CAL_RC_CLK_AUTO);
    if (!rc) rc = write_tlra(h, h->cfg.t_lra_us);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_DRV_DUTY_CFG, h->cfg.drv_duty);
    if (!rc) rc = wr(h, PGH_CFG, PGH_CFG_AUTORES_CFG, (uint8_t)(h->cfg.autores_cfg & ~PGH_AUTORES_EN));
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_AUTO_SHUTDOWN, PGH_EN_WFHOLD_FIFO, 0);
    if (!rc) rc = update(h, PGH_CFG, PGH_CFG_TRIG_PRIORITY, PGH_SWR_IGNORE, 0);
    if (!rc) rc = wr(h, PGH_PTN, PGH_PTN_FIFO_EMPTY_CFG, 0);
    if (!rc) rc = update(h, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, PGH_FIFO_RATE_MASK, PGH_RATE_T_LRA);
    if (!rc) rc = wr(h, PGH_PTN, PGH_PTN_MMAP_FIFO, (uint8_t)(PGH_MMAP_FIFO_EXIST | (PGH_MEM_BYTES / PGH_MMAP_UNIT - 1)));
    if (!rc) rc = rd(h, PGH_PTN, PGH_PTN_MMAP_FIFO, &v);
    if (rc) return rc;
    h->fifo_len = (v & PGH_MMAP_FIFO_EXIST) ? ((uint32_t)(v & PGH_MMAP_FIFO_LEN_MASK) + 1u) * PGH_MMAP_UNIT : 0;
    uint8_t brake[PGH_BRAKE_SAMPLES];
    for (unsigned i = 0; i < PGH_BRAKE_SAMPLES; ++i) brake[i] = i < h->cfg.brake_len ? h->cfg.brake[i] : 0;
    rc = wrn(h, PGH_PTN, PGH_PTN_BRAKE_AMP, brake, PGH_BRAKE_SAMPLES);
    if (!rc) rc = rd(h, PGH_CFG, PGH_CFG_EN_CTL, &v);
    if (rc) return rc;
    h->en_on = (uint8_t)((v & PGH_HAPTICS_EN) ? 1 : 0);
    rc = set_vmax(h, pgh_core_vmax(h, h->cfg.vmax_mv));
    return rc == E_BUSY ? 0 : rc;   /* a VMAX that did not settle is retried by the first play */
}

int pgh_core_init(PghCore *h, const PghConfig *cfg, const PghOps *ops, const PghSpmiIo *io)
{
    if (!h || !cfg || !ops || !ops->now_us || !ops->wait_us) return E_INVAL;
    pgh_zero(h, sizeof *h);
    h->cfg = *cfg;
    h->ops = *ops;
    h->state = PGH_ST_OFF;
    h->sel = 0xffff;
    h->vmax_code = 0xff;
    h->autores_ok = 1;
    h->hw_max_mv = 6000;
    h->stage = 1;
    PghConfig *c = &h->cfg;
    if ((c->mode != PGH_MODE_ON && c->mode != PGH_MODE_DRY) || c->t_lra_us < 1000 || c->t_lra_us > PGH_TLRA_MAX_US ||
        c->brake_len > PGH_BRAKE_SAMPLES || c->effects > PGH_EFFECTS_MAX || c->ee > 7) {
        h->stage = -1;
        return E_INVAL;
    }
    if (c->hard_mv > c->fifo_mv) c->hard_mv = c->fifo_mv;
    if (c->hard_mv > 10000u) c->hard_mv = 10000u;
    if (c->cap_mv > c->hard_mv) c->cap_mv = c->hard_mv;
    if (c->cap_mv < PGH_VMAX_STEP_MV) c->cap_mv = PGH_VMAX_STEP_MV;
    if (c->hard_mv < c->cap_mv) c->hard_mv = c->cap_mv;
    if (c->refill_permille > 1000) c->refill_permille = 1000;
    int rc = pgh_spmi_discover(&h->spmi, io, c->ee, c->ppid, c->hard_mv / PGH_VMAX_STEP_MV);
    h->spmi.retry_dropped = c->spmi_retry ? 1 : 0;
    spmi_lines(h, rc);
    if (rc) {
        h->stage = -2;
        return rc;
    }
    for (unsigned k = 0; k < PGH_PERIPHS; ++k)
        if (h->spmi.p[k].apid >= 0) pgh_spmi_identify(&h->spmi, k);
    const char *problem = identity_problem(h);
    id_line(h, problem);
    dump(h, PGH_CFG, kDumpCfg, sizeof kDumpCfg);
    dump(h, PGH_PTN, kDumpPtn, sizeof kDumpPtn);
    dump(h, PGH_HBST, kDumpHbst, sizeof kDumpHbst);
    dump(h, PGH_SDAM, kDumpSdam, sizeof kDumpSdam);
    if (c->mode == PGH_MODE_DRY) {
        if (!problem) probe_channel(h);
        h->state = PGH_ST_DRY;
        h->stage = 2;
        return 1;
    }
    char line[160];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    if (problem) {
        h->stage = -3;
        pgh_text_str(&t, "[haptics] refused: ");
        pgh_text_str(&t, problem);
        pgh_text_str(&t, " (nothing written)");
        emit(h, &t);
        return E_PERM;
    }
    rc = pgh_spmi_open(&h->spmi, PGH_CFG);
    if (!rc) rc = pgh_spmi_open(&h->spmi, PGH_PTN);
    if (rc) {
        h->stage = -4;
        pgh_text_str(&t, "[haptics] refused: write channel");
        pgh_text_kd(&t, "rc", rc);
        emit(h, &t);
        return rc;
    }
    h->state = PGH_ST_IDLE;   /* hw_init's helpers need a live state to report faults */
    rc = hw_init(h);
    if (rc) {
        if (h->state != PGH_ST_FAULT) fault(h, "init", rc);
        h->stage = -5;
        return rc;
    }
    h->budget_us = (int64_t)c->budget_ms * 1000;
    h->t_budget = now(h);
    h->stage = 3;
    pgh_text_str(&t, "[haptics] ready");
    pgh_text_kd(&t, "fifo", h->fifo_len);
    pgh_text_kd(&t, "hw_max_mv", h->hw_max_mv);
    pgh_text_kd(&t, "cap_mv", c->cap_mv);
    pgh_text_kd(&t, "hard_mv", c->hard_mv);
    pgh_text_kd(&t, "vmax", (int64_t)h->vmax_code * PGH_VMAX_STEP_MV);
    pgh_text_kd(&t, "en", h->en_on);
    pgh_text_kd(&t, "writes", h->spmi.writes);
    if (c->spmi_retry) pgh_text_kd(&t, "dropped", h->spmi.dropped);
    emit(h, &t);
    return 0;
}

void pgh_core_export(const PghCore *h, int32_t w[16])
{
    for (unsigned i = 0; i < 16; ++i) w[i] = 0;
    if (!h) return;
    w[0] = h->stage;
    w[1] = h->state;
    w[2] = (int32_t)h->plays;
    w[3] = (int32_t)h->streams;
    w[4] = (int32_t)h->spmi.writes;
    w[5] = (int32_t)h->spmi.reads;
    w[6] = (int32_t)((h->spmi.refused & 0xffu) | (h->spmi.failures & 0xffu) << 8 | (h->spmi.timeouts & 0xffu) << 16);
    w[7] = (int32_t)h->f0_dhz;
    w[8] = h->vmax_code == 0xff ? -1 : h->vmax_code;
    w[9] = (int32_t)h->faults_seen;
    w[10] = (int32_t)h->underruns;
    w[11] = (int32_t)(h->budget_us / 1000);
    w[12] = (int32_t)h->cl_t_lra_us;
    w[13] = h->fault_rc ? h->fault_rc : h->spmi.last_rc;
    w[14] = (int32_t)h->clips;
    w[15] = (int32_t)(uint32_t)h->samples;
}
