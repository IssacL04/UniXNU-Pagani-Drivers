/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 结构、日志写法和“先落盘再持住写者”的规则沿用 PaganiBt BtBringup.c。CamBringup.h 写了各级做什么。 */
#include "CamBringup.h"
#include "CamUtil.h"

enum {
    kBranchPolls = 200, kBranchStepUs = 10,
    kGdscPolls = 500, kGdscStepUs = 10,
    kRcgPolls = 1000, kRcgStepUs = 10,
    kCrumbMs = 3500, kQuietMs = 1000,      /* UFS 日志 3 s 一轮；写者 500 ms 看一次保持字，一轮最多几 ms：PanelProbe.h 同样的数 */
    kRailSettleMs = 5, kMclkSettleMs = 1, kResetLowMs = 1,
    kResetSettleMs = 20,                   /* 放开复位到第一次 I²C。真实时序在 sensormodule 里，阶段 0 再核；给足 */
    kCoreAhbDelayUs = 10,
};

typedef struct {uint16_t gpio; uint8_t function; const char *name;} PcamPin;
static const PcamPin kPins[PcamPins] = {
    {PcamPinMclk0, 1, "mclk0"}, {PcamPinSda0, 1, "sda0"}, {PcamPinScl0, 1, "scl0"}, {PcamPinReset, 0, "reset"},
};
enum {kPinMclk, kPinSda, kPinScl, kPinReset};
static const uint32_t kGccBranches[3] = {PcamGccBistAhb, PcamGccCameraAhb, PcamGccCameraXo};
static const uint32_t kAlwaysOn[4] = {PcamCcDrvAhb, PcamCcDrvXo, PcamCcGdscClk, PcamCcSleepClk};    /* 下游 probe 的顺序 */

/* ---- 闸门 ---- */
static int cbcr_on(uint32_t value) {return (value & PCAM_CBCR_ENABLE) != 0;}

static int tlmm_allowed(uint32_t offset, uint32_t value)
{
    const uint32_t pin = offset / PcamPinStride, reg = offset % PcamPinStride;
    for (unsigned i = 0; i < PcamPins; ++i) {
        if (pin != kPins[i].gpio) continue;
        if (reg == 0) {
            const uint32_t function = (value >> PcamCtlFunctionShift) & PcamCtlFunctionMask;
            return function == 0 || function == kPins[i].function;
        }
        return reg == 4 && i == kPinReset && (value == 0 || value == PcamIoOut);
    }
    return 0;
}

static int cci_allowed(uint32_t offset, uint32_t value)
{
    switch (offset) {
    case PcamCciQueueStart: return value == 1u;         /* 只有 master 0 queue 0 */
    case PcamCciHalt: return value == PcamCciHaltM0;
    case PcamCciReset: return value == PcamCciResetAll;
    case PcamCciSclCtl: case PcamCciSdaCtl0: case PcamCciSdaCtl1: case PcamCciSdaCtl2: case PcamCciMiscCtl:
    case PcamCciExecWords: case PcamCciLoadData:
    case PcamCciIrqGlobalClear: case PcamCciIrqMask0: case PcamCciIrqClear0:
        return 1;
    default:
        return 0;
    }
}

int pcam_write_allowed(unsigned win, uint32_t offset, uint32_t value)
{
    switch (win) {
    case PcamWinGcc:
        return (offset == PcamGccBistAhb || offset == PcamGccCameraAhb || offset == PcamGccCameraXo) && cbcr_on(value);
    case PcamWinTlmm:
        return tlmm_allowed(offset, value);
    case PcamWinCamcc:
        switch (offset) {
        case PcamCcGdscr: return !(value & (PCAM_GDSC_SW_COLLAPSE | PCAM_GDSC_HW_CONTROL));     /* 只开，不关 */
        case PcamCcGdscClk: case PcamCcSleepClk: case PcamCcDrvXo: case PcamCcDrvAhb:
        case PcamCcCoreAhb: case PcamCcTopAhb: case PcamCcTopFastAhb: case PcamCcCci0:
            return cbcr_on(value);
        case PcamCcSlowAhbRcg: case PcamCcFastAhbRcg: case PcamCcCciRcg:
            return (value & PCAM_RCG_UPDATE) != 0;
        case PcamCcSlowAhbRcg + PcamRcgCfg: case PcamCcFastAhbRcg + PcamRcgCfg: case PcamCcCciRcg + PcamRcgCfg:
            return value == PCAM_RCG_CFG_XO;
        default:
            return 0;
        }
    case PcamWinBist:
        return offset == PcamBistMclk0Cbcr || (offset == PcamBistMclk0Rcg && (value & PCAM_RCG_UPDATE)) ||
               (offset == PcamBistMclk0Rcg + PcamRcgCfg && value == PCAM_RCG_CFG_XO);
    case PcamWinCci:
        return cci_allowed(offset, value);
    default:
        return 0;
    }
}

/* 写错电压可能烧传感器，所以这里只有一张表：资源名、电压都对才放行。 */
int pcam_vrm_allowed(const char *resource, uint32_t reg, uint32_t value)
{
    for (unsigned i = 0; i < PCAM_RAIL_COUNT; ++i) {
        if (!pcam_streq(resource, kPcamRails[i].resource)) continue;
        switch (reg) {
        case PcamVrmVoltage: return value == kPcamRails[i].millivolts;
        case PcamVrmEnable: return value <= 1;
        case PcamVrmMode: return value == PcamVrmModeHigh;
        default: return 0;
        }
    }
    return 0;
}

/* ---- 访问 ---- */
static uint32_t rd(PcamBringup *b, unsigned win, uint32_t off) {return b->hw->read(b->hw->ctx, win, off);}
static void wr(PcamBringup *b, unsigned win, uint32_t off, uint32_t value)
{
    if (!pcam_write_allowed(win, off, value)) {
        b->refused++;
        return;
    }
    b->hw->write(b->hw->ctx, win, off, value);
}
static void line(PcamBringup *b, const char *text) {if (b->hw->log) b->hw->log(b->hw->ctx, text);}

static int done(PcamBringup *b, int step)
{
    b->step = step;
    return 0;
}
static int failed(PcamBringup *b, int step, int error, const char *what)
{
    char text[160];
    PcamText t;
    b->step = -step;
    b->error = error;
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] FAIL");
    pcam_text_kd(&t, "step", step);
    pcam_text_kd(&t, "err", error);
    pcam_text_char(&t, ' ');
    pcam_text_str(&t, what);
    line(b, text);
    return error;
}

/* 第一次碰可能挂住总线的块之前：看门狗的复位不打招呼，落在 UFS 写上就是掉电，这颗闪存最坏丢整个 LUN 0（UfsLog.h）。上一行先落盘，再持住写者。真挂住了就一直持着。 */
static void guard_on(PcamBringup *b)
{
    b->hw->msleep(b->hw->ctx, kCrumbMs);
    b->hw->quiet(b->hw->ctx, 1);
    b->held = 1;
    b->hw->msleep(b->hw->ctx, kQuietMs);
}
static void guard_off(PcamBringup *b)
{
    if (!b->held) return;
    b->hw->quiet(b->hw->ctx, 0);
    b->held = 0;
}

static uint32_t pin_ctl_off(unsigned i) {return (uint32_t)kPins[i].gpio * PcamPinStride;}

static void log_rail(PcamBringup *b, const char *when, unsigned i)
{
    const PcamRailResult *r = &b->rails[i];
    char text[160];
    PcamText t;
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] rail ");
    pcam_text_str(&t, kPcamRails[i].name);
    pcam_text_char(&t, ' ');
    pcam_text_str(&t, kPcamRails[i].resource);
    pcam_text_char(&t, ' ');
    pcam_text_str(&t, when);
    if (r->read) {
        pcam_text_kx(&t, "status", r->status);
        pcam_text_kx(&t, "en", r->enable);
        pcam_text_kx(&t, "mode", r->mode);
        pcam_text_kd(&t, "mv", r->millivolts);
    } else {
        pcam_text_str(&t, " unread");
    }
    if (r->address) pcam_text_kx(&t, "addr", r->address);
    if (r->rc) pcam_text_kd(&t, "rc", r->rc);
    line(b, text);
}

static void read_rail(PcamBringup *b, unsigned i)
{
    PcamRailResult *r = &b->rails[i];
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    r->read = 0;
    if (b->hw->pmic(b->hw->ctx, kPcamRails[i].ppid, &status, &enable, &mode, &mv)) return;
    r->read = 1;
    r->status = status;
    r->enable = enable;
    r->mode = mode;
    r->millivolts = mv;
}

/* ---- 步 101 ---- */
static int survey(PcamBringup *b)
{
    char text[190];
    PcamText t;
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] survey gcc");
    for (unsigned i = 0; i < 3; ++i) {
        b->gcc[i] = rd(b, PcamWinGcc, kGccBranches[i]);
        pcam_text_kx(&t, i == 0 ? "bist" : i == 1 ? "ahb" : "xo", b->gcc[i]);
    }
    line(b, text);
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] pins");
    for (unsigned i = 0; i < PcamPins; ++i) {
        b->pin_ctl[i] = rd(b, PcamWinTlmm, pin_ctl_off(i));
        b->pin_io[i] = rd(b, PcamWinTlmm, pin_ctl_off(i) + 4);
        pcam_text_char(&t, ' ');
        pcam_text_str(&t, kPins[i].name);
        pcam_text_char(&t, '=');
        pcam_text_hex(&t, b->pin_ctl[i] & 0x3fffu);
        pcam_text_char(&t, '/');
        pcam_text_udec(&t, b->pin_io[i] & 3u);
    }
    line(b, text);
    for (unsigned i = 0; i < PCAM_RAIL_COUNT; ++i) {
        read_rail(b, i);
        log_rail(b, "now", i);
    }
    return done(b, PcamStepSurvey);
}

/* ---- 步 102 ---- */
static uint32_t branch_off(uint32_t value) {return (value & PCAM_CBCR_OFF) && !(value & PCAM_CBCR_HW_CTL);}

static int camcc_read(PcamBringup *b)
{
    char text[190];
    PcamText t;
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t now = rd(b, PcamWinGcc, kGccBranches[i]);
        if (!(now & PCAM_CBCR_ENABLE)) wr(b, PcamWinGcc, kGccBranches[i], now | PCAM_CBCR_ENABLE);
    }
    uint32_t off = 7;
    for (unsigned n = 0; n < kBranchPolls && off; ++n) {
        off = 0;
        for (unsigned i = 0; i < 3; ++i) {
            b->gcc[i] = rd(b, PcamWinGcc, kGccBranches[i]);
            if (branch_off(b->gcc[i]) || !(b->gcc[i] & PCAM_CBCR_ENABLE)) off |= 1u << i;
        }
        if (off) b->hw->udelay(b->hw->ctx, kBranchStepUs);
    }
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] gcc");
    pcam_text_kx(&t, "bist", b->gcc[0]);
    pcam_text_kx(&t, "ahb", b->gcc[1]);
    pcam_text_kx(&t, "xo", b->gcc[2]);
    pcam_text_kx(&t, "off", off);
    line(b, text);
    /* 分支没起来时绝不读 CAMCC：读没时钟的块会把总线挂住 */
    if (off) return failed(b, PcamStepCamcc, PcamErrGcc, "a GCC branch did not start: CAMCC was not read");
    guard_on(b);
    b->gdscr = rd(b, PcamWinCamcc, PcamCcGdscr);
    b->cfg_gdscr = rd(b, PcamWinCamcc, PcamCcCfgGdscr);
    b->cci_cbcr = rd(b, PcamWinCamcc, PcamCcCci0);
    b->cci_rcg = rd(b, PcamWinCamcc, PcamCcCciRcg + PcamRcgCfg);
    b->mclk_cbcr = rd(b, PcamWinBist, PcamBistMclk0Cbcr);
    b->mclk_rcg = rd(b, PcamWinBist, PcamBistMclk0Rcg + PcamRcgCfg);
    guard_off(b);
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] camcc");
    pcam_text_kx(&t, "gdscr", b->gdscr);
    pcam_text_kx(&t, "cfg", b->cfg_gdscr);
    pcam_text_kx(&t, "cci", b->cci_cbcr);
    pcam_text_kx(&t, "ccircg", b->cci_rcg);
    pcam_text_kx(&t, "mclk", b->mclk_cbcr);
    pcam_text_kx(&t, "mclkrcg", b->mclk_rcg);
    line(b, text);
    if (b->gdscr == 0xffffffffu || b->cci_cbcr == 0xffffffffu || b->mclk_cbcr == 0xffffffffu) return failed(b, PcamStepCamcc, PcamErrCamccValue, "CAMCC reads all ones");
    if (b->gdscr & PCAM_GDSC_HW_CONTROL) return failed(b, PcamStepCamcc, PcamErrGdsc, "the GDSC is under hardware control");
    return done(b, PcamStepCamcc);
}

/* ---- 步 201 ---- */
static int rcg_to_xo(PcamBringup *b, unsigned win, uint32_t rcg)
{
    const uint32_t cfg = rd(b, win, rcg + PcamRcgCfg);
    /* 复位值就是 XO：分频字段 0 和 1 都是不分频，什么都不用写 */
    if (!(cfg & (PCAM_RCG_SRC_MASK | PCAM_RCG_MODE_MASK)) && (cfg & PCAM_RCG_DIV_MASK) <= 1) return 0;
    wr(b, win, rcg + PcamRcgCfg, PCAM_RCG_CFG_XO);
    wr(b, win, rcg, rd(b, win, rcg) | PCAM_RCG_UPDATE);
    for (unsigned n = 0; n < kRcgPolls; ++n) {
        if (!(rd(b, win, rcg) & PCAM_RCG_UPDATE)) return 0;
        b->hw->udelay(b->hw->ctx, kRcgStepUs);
    }
    return -1;
}

static int branch_on(PcamBringup *b, unsigned win, uint32_t cbcr)
{
    const uint32_t now = rd(b, win, cbcr);
    if (!(now & PCAM_CBCR_ENABLE)) wr(b, win, cbcr, now | PCAM_CBCR_ENABLE);
    for (unsigned n = 0; n < kBranchPolls; ++n) {
        const uint32_t v = rd(b, win, cbcr);
        if ((v & PCAM_CBCR_ENABLE) && !branch_off(v)) return 0;
        b->hw->udelay(b->hw->ctx, kBranchStepUs);
    }
    return -1;
}

static int gdsc_on(uint32_t gdscr, uint32_t cfg) {return (gdscr & PCAM_GDSC_PWR_ON) || (cfg & PCAM_GDSC_CFG_UP_DONE);}

/* 碰 CCI 之前逐个看：GDSC 上电，AHB 和 CCI 分支都在跑。这是这个驱动最要紧的一条：未上电访问会挂总线。 */
static int cci_ready(PcamBringup *b)
{
    if (!gdsc_on(rd(b, PcamWinCamcc, PcamCcGdscr), rd(b, PcamWinCamcc, PcamCcCfgGdscr))) return 0;
    if (!(rd(b, PcamWinCamcc, PcamCcCoreAhb) & PCAM_CBCR_ENABLE)) return 0;
    static const uint32_t polled[3] = {PcamCcTopAhb, PcamCcTopFastAhb, PcamCcCci0};
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t v = rd(b, PcamWinCamcc, polled[i]);
        if (!(v & PCAM_CBCR_ENABLE) || branch_off(v)) return 0;
    }
    return 1;
}

static int domain_fail(PcamBringup *b, int error, const char *what)
{
    guard_off(b);
    return failed(b, PcamStepDomain, error, what);
}

static int domain_on(PcamBringup *b)
{
    const PcamHw *hw = b->hw;
    char text[190];
    PcamText t;
    int rc = hw->arc(hw->ctx, PCAM_ARC_MM, PcamArcLevel, &b->mm_hlvl);
    if (!rc) rc = hw->arc(hw->ctx, PCAM_ARC_MXC, PcamArcLevel, &b->mxc_hlvl);
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] arc");
    pcam_text_kd(&t, "mm", b->mm_hlvl);
    pcam_text_kd(&t, "mxc", b->mxc_hlvl);
    pcam_text_kd(&t, "rc", rc);
    line(b, text);
    if (rc) return failed(b, PcamStepDomain, PcamErrVote, "ARC vote");
    guard_on(b);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t now = rd(b, PcamWinCamcc, kAlwaysOn[i]);
        if (!(now & PCAM_CBCR_ENABLE)) wr(b, PcamWinCamcc, kAlwaysOn[i], now | PCAM_CBCR_ENABLE);
    }
    uint32_t gdscr = rd(b, PcamWinCamcc, PcamCcGdscr);
    if (gdscr & PCAM_GDSC_HW_CONTROL) return domain_fail(b, PcamErrGdsc, "the GDSC is under hardware control");
    if (!gdsc_on(gdscr, rd(b, PcamWinCamcc, PcamCcCfgGdscr))) {
        wr(b, PcamWinCamcc, PcamCcGdscr, gdscr & ~PCAM_GDSC_SW_COLLAPSE);
        hw->udelay(hw->ctx, 1);                 /* 8 个 XO 周期之后再看状态 */
        unsigned n = 0;
        for (; n < kGdscPolls; ++n) {
            if (gdsc_on(rd(b, PcamWinCamcc, PcamCcGdscr), rd(b, PcamWinCamcc, PcamCcCfgGdscr))) break;
            hw->udelay(hw->ctx, kGdscStepUs);
        }
        if (n == kGdscPolls) return domain_fail(b, PcamErrGdsc, "the GDSC did not power up: CCI was not read");
        hw->udelay(hw->ctx, 1);
    }
    if (rcg_to_xo(b, PcamWinCamcc, PcamCcSlowAhbRcg) || rcg_to_xo(b, PcamWinCamcc, PcamCcFastAhbRcg) || rcg_to_xo(b, PcamWinCamcc, PcamCcCciRcg))
        return domain_fail(b, PcamErrRcg, "an RCG did not update: CCI was not read");
    const uint32_t core = rd(b, PcamWinCamcc, PcamCcCoreAhb);
    if (!(core & PCAM_CBCR_ENABLE)) wr(b, PcamWinCamcc, PcamCcCoreAhb, core | PCAM_CBCR_ENABLE);
    hw->udelay(hw->ctx, kCoreAhbDelayUs);
    if (branch_on(b, PcamWinCamcc, PcamCcTopAhb) || branch_on(b, PcamWinCamcc, PcamCcTopFastAhb) || branch_on(b, PcamWinCamcc, PcamCcCci0))
        return domain_fail(b, PcamErrBranch, "a branch did not start: CCI was not read");
    if (!cci_ready(b)) return domain_fail(b, PcamErrBranch, "state check failed: CCI was not read");
    b->cci_version = rd(b, PcamWinCci, PcamCciVersion);
    guard_off(b);
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] domain on");
    pcam_text_kx(&t, "gdscr", rd(b, PcamWinCamcc, PcamCcGdscr));
    pcam_text_kx(&t, "cci", rd(b, PcamWinCamcc, PcamCcCci0));
    pcam_text_kx(&t, "cci_version", b->cci_version);
    line(b, text);
    if (!b->cci_version || b->cci_version == 0xffffffffu) return failed(b, PcamStepDomain, PcamErrCciVersion, "CCI version is not plausible");
    return done(b, PcamStepDomain);
}

/* ---- 步 301 ---- */
static int vrm(PcamBringup *b, const PcamRail *rail, uint32_t reg, uint32_t value, uint32_t *address)
{
    if (!pcam_vrm_allowed(rail->resource, reg, value)) {
        b->refused++;
        return -1;
    }
    return b->hw->vrm(b->hw->ctx, rail->resource, reg, value, address);
}

static int rails_on(PcamBringup *b)
{
    const PcamHw *hw = b->hw;
    for (unsigned i = 0; i < PcamPins; ++i) {
        b->pin_ctl[i] = rd(b, PcamWinTlmm, pin_ctl_off(i));
        b->pin_io[i] = rd(b, PcamWinTlmm, pin_ctl_off(i) + 4);
        const uint32_t function = (b->pin_ctl[i] >> PcamCtlFunctionShift) & PcamCtlFunctionMask;
        /* 功能 0 和目标功能以外就是别人在用；这一步还什么都没写 */
        if (function != 0 && function != kPins[i].function) return failed(b, PcamStepRails, PcamErrPin, kPins[i].name);
    }
    /* 复位脚先定在低：轨道起来的全程传感器都在复位里。先写输出电平再开输出，不留毛刺。 */
    b->pins_touched = 1;
    const uint32_t reset = pin_ctl_off(kPinReset), now = b->pin_ctl[kPinReset];
    const uint32_t ctl = (now & ~(uint32_t)PcamCtlMask) | PcamCtlResetOut | (now & PcamCtlEgpioPresent ? PcamCtlEgpioEnable : 0);
    wr(b, PcamWinTlmm, reset + 4, 0);
    wr(b, PcamWinTlmm, reset, ctl);
    if ((rd(b, PcamWinTlmm, reset) & PcamCtlMask) != PcamCtlResetOut || (rd(b, PcamWinTlmm, reset + 4) & PcamIoOut))
        return failed(b, PcamStepRails, PcamErrPin, "reset pin not low");
    for (unsigned order = 1; order <= PCAM_RAIL_COUNT; ++order) {
        unsigned i = 0;
        while (i < PCAM_RAIL_COUNT && kPcamRails[i].order != order) ++i;
        if (i == PCAM_RAIL_COUNT) continue;
        const PcamRail *rail = &kPcamRails[i];
        PcamRailResult *r = &b->rails[i];
        read_rail(b, i);
        /* 已经开着、又不是白名单的电压：那是别的东西在用，不是这个驱动该动的 */
        if (r->read && (r->enable & 0x80) && r->millivolts != rail->millivolts) {
            log_rail(b, "unexpected", i);
            return failed(b, PcamStepRails, PcamErrRail, rail->resource);
        }
        uint32_t address = 0;
        b->rails_touched |= (uint8_t)(1u << i);
        int rc = vrm(b, rail, PcamVrmMode, PcamVrmModeHigh, &address);
        if (!rc) rc = vrm(b, rail, PcamVrmVoltage, rail->millivolts, &address);
        if (!rc) rc = vrm(b, rail, PcamVrmEnable, 1, &address);
        r->address = address;
        r->rc = rc;
        r->voted = 1;
        if (rc) {
            log_rail(b, "vote failed", i);
            return failed(b, PcamStepRails, PcamErrRail, rail->resource);
        }
        hw->msleep(hw->ctx, kRailSettleMs);
    }
    for (unsigned i = 0; i < PCAM_RAIL_COUNT; ++i) {
        read_rail(b, i);
        log_rail(b, "after", i);
        /* 读不到不算数（PM8010 可能在第二条 SPMI 总线上），读到了就必须对 */
        if (b->rails[i].read && (b->rails[i].millivolts != kPcamRails[i].millivolts || !(b->rails[i].enable & 0x80)))
            return failed(b, PcamStepRails, PcamErrRail, kPcamRails[i].resource);
    }
    return done(b, PcamStepRails);
}

/* ---- 步 302 ---- */
static int sensor_on(PcamBringup *b)
{
    const PcamHw *hw = b->hw;
    if (rcg_to_xo(b, PcamWinBist, PcamBistMclk0Rcg)) return failed(b, PcamStepSensor, PcamErrRcg, "MCLK RCG did not update");
    b->mclk_on = 1;
    if (branch_on(b, PcamWinBist, PcamBistMclk0Cbcr)) return failed(b, PcamStepSensor, PcamErrBranch, "MCLK branch did not start");
    const uint32_t ctl = b->pin_ctl[kPinMclk];
    wr(b, PcamWinTlmm, pin_ctl_off(kPinMclk), (ctl & ~(uint32_t)PcamCtlMask) | PcamCtlMclk);
    hw->msleep(hw->ctx, kMclkSettleMs);
    for (unsigned i = kPinSda; i <= kPinScl; ++i) wr(b, PcamWinTlmm, pin_ctl_off(i), (b->pin_ctl[i] & ~(uint32_t)PcamCtlMask) | PcamCtlCci);
    wr(b, PcamWinTlmm, pin_ctl_off(kPinReset) + 4, PcamIoOut);
    hw->msleep(hw->ctx, kResetSettleMs);
    char text[160];
    PcamText t;
    pcam_text_start(&t, text, sizeof text);
    pcam_text_str(&t, "[cam] sensor on");
    pcam_text_kx(&t, "mclk", rd(b, PcamWinBist, PcamBistMclk0Cbcr));
    pcam_text_kx(&t, "reset", rd(b, PcamWinTlmm, pin_ctl_off(kPinReset) + 4) & 3u);
    pcam_text_kx(&t, "sda", rd(b, PcamWinTlmm, pin_ctl_off(kPinSda)) & 0x3ffu);
    pcam_text_kx(&t, "scl", rd(b, PcamWinTlmm, pin_ctl_off(kPinScl)) & 0x3ffu);
    line(b, text);
    return done(b, PcamStepSensor);
}

/* ---- 步 303 ---- */
static uint32_t cci_read(void *ctx, uint32_t off) {PcamBringup *b = (PcamBringup *)ctx; return rd(b, PcamWinCci, off);}
static void cci_write(void *ctx, uint32_t off, uint32_t value) {wr((PcamBringup *)ctx, PcamWinCci, off, value);}
static void cci_delay(void *ctx, unsigned us) {PcamBringup *b = (PcamBringup *)ctx; b->hw->udelay(b->hw->ctx, us);}

static int sensor_id(PcamBringup *b)
{
    char text[160];
    PcamText t;
    const PcamCciIo io = {b, cci_read, cci_write, cci_delay};
    b->cci = io;
    const int rc = pcam_cci_init(&b->cci, &kPcamCciFast);
    if (rc) {
        pcam_text_start(&t, text, sizeof text);
        pcam_text_str(&t, "[cam] cci init");
        pcam_text_kd(&t, "rc", rc);
        line(b, text);
        return failed(b, PcamStepId, PcamErrCci, "CCI did not reset");
    }
    for (unsigned i = 0; i < PCAM_TELE_ADDRESS_COUNT; ++i) {
        const uint8_t addr = kPcamTeleAddresses[i];
        uint16_t id = 0;
        int e = pcam_cci_write16(&b->cci, &kPcamCciFast, addr, PcamTelePageReg, PcamTelePageValue);
        if (!e) e = pcam_cci_read16(&b->cci, &kPcamCciFast, addr, PcamTeleIdReg, &id);
        pcam_text_start(&t, text, sizeof text);
        pcam_text_str(&t, "[cam] cci0 m0");
        if (e) {
            pcam_text_kx(&t, "addr", addr);
            pcam_text_kd(&t, "rc", e);
            line(b, text);
            continue;
        }
        b->sensor_id = id;
        pcam_text_str(&t, " id ");
        pcam_text_hex(&t, id);
        pcam_text_kx(&t, "addr", addr);
        line(b, text);
        if (id == PcamTeleId) {
            b->sensor_address = addr;
            return done(b, PcamStepId);
        }
    }
    return failed(b, PcamStepId, PcamErrIdentity, "no address answered 0x38e5");
}

/* ---- 步 304 ---- */
static int power_off(PcamBringup *b)
{
    const PcamHw *hw = b->hw;
    int error = 0;
    if (b->pins_touched) {
        wr(b, PcamWinTlmm, pin_ctl_off(kPinReset) + 4, 0);
        hw->msleep(hw->ctx, kResetLowMs);
    }
    if (b->mclk_on) {
        const uint32_t v = rd(b, PcamWinBist, PcamBistMclk0Cbcr);
        wr(b, PcamWinBist, PcamBistMclk0Cbcr, v & ~PCAM_CBCR_ENABLE);
    }
    if (b->pins_touched) {
        /* 脚还原成动之前的样子；复位脚的输出电平留在低 */
        for (unsigned i = 0; i < PcamPins; ++i) wr(b, PcamWinTlmm, pin_ctl_off(i), b->pin_ctl[i]);
    }
    for (unsigned order = PCAM_RAIL_COUNT; order >= 1; --order) {
        unsigned i = 0;
        while (i < PCAM_RAIL_COUNT && kPcamRails[i].order != order) ++i;
        if (i == PCAM_RAIL_COUNT || !(b->rails_touched & (1u << i))) continue;
        uint32_t address = 0;
        const int rc = vrm(b, &kPcamRails[i], PcamVrmEnable, 0, &address);
        if (rc) error = PcamErrRail;
        b->rails[i].rc = rc;
    }
    if (b->rails_touched) {
        hw->msleep(hw->ctx, kRailSettleMs);
        for (unsigned i = 0; i < PCAM_RAIL_COUNT; ++i) {
            if (!(b->rails_touched & (1u << i))) continue;
            read_rail(b, i);
            log_rail(b, "off", i);
            if (b->rails[i].read && (b->rails[i].enable & 0x80)) error = PcamErrRail;
        }
    }
    b->rails_touched = 0;
    if (error) {
        const int step = b->step, already = b->error;
        const int rc = failed(b, PcamStepOff, error, "a rail did not go off");
        if (already) {                          /* 原来的失败更要紧：步号和错误码留原来的 */
            b->step = step;
            b->error = already;
        }
        return rc;
    }
    if (b->step > 0) b->step = PcamStepOff;
    return 0;
}

unsigned pcam_level(unsigned value) {return value >= 1 && value <= PcamLevelMax ? value : 0;}

int pcam_run(PcamBringup *b)
{
    if (!b || !b->hw) return PcamErrArgument;
    const PcamHw *hw = b->hw;
    if (!hw->read || !hw->write || !hw->udelay || !hw->msleep || !hw->arc || !hw->vrm || !hw->pmic || !hw->quiet || !pcam_level(b->level)) return PcamErrArgument;
    int rc = survey(b);
    if (rc || b->level < 2) return rc;
    if ((rc = camcc_read(b)) || b->level < 3) return rc;
    if ((rc = domain_on(b)) || b->level < 4) return rc;
    /* 级 4 从第一次给传感器上电到下电走完都持住写者：轨道第一次带上负载（过流是 PMIC 复位，不打招呼）、BIST 的第一次写、CCI 总线的第一笔事务，都是这次开机没做过的事。 */
    guard_on(b);
    rc = rails_on(b);
    if (!rc) rc = sensor_on(b);
    if (!rc) rc = sensor_id(b);
    const int off = power_off(b);
    guard_off(b);
    return rc ? rc : off;
}
