/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 上电规则沿用 PaganiWifi WifiBringup.cpp（powerOn，来自 UniXNU-sheng package/WifiProbe.cpp，commit 08cde20e）；BtBringup.h 写了和它的差别。 */
#include "BtBringup.h"
#include "BtUtil.h"

typedef struct {uint32_t halt; uint32_t vote; const char *name;} PbBranch;
static const PbBranch kBranches[5] = {
    {PbHaltMAhb, PbVoteMAhb, "m_ahb"}, {PbHaltSAhb, PbVoteSAhb, "s_ahb"}, {PbHaltCore, PbVoteCore, "core"},
    {PbHaltCore2x, PbVoteCore2x, "core_2x"}, {PbHaltSe6, PbVoteSe6, "se6"},
};
static const uint16_t kLogPins[6] = {PbPinCts, PbPinRts, PbPinTx, PbPinRx, PbPinWlanEn, PbPinSwCtrl};
enum {kBranchPolls = 200, kBranchStepUs = 10, kRcgPolls = 1000, kRcgStepUs = 10, kRailSettleMs = 5, kLoopbackWaitMs = 10};
enum {kCrumbMs = 3500, kQuietMs = 1000};   /* UFS 日志 3 s 一轮；写者 500 ms 看一次保持字，一轮最多几 ms：PanelProbe.h 同样的数 */

static uint32_t rd(PbBringup *b, unsigned win, uint32_t off) {return b->hw->read(b->hw->ctx, win, off);}
static void wr(PbBringup *b, unsigned win, uint32_t off, uint32_t value) {b->hw->write(b->hw->ctx, win, off, value);}
static void line(PbBringup *b, const char *text) {if (b->hw->log) b->hw->log(b->hw->ctx, text);}

static int done(PbBringup *b, int step)
{
    b->step = step;
    return b->stop == step ? PbErrStopped : 0;
}
/* 线上没有 UFS 写的一段（UfsLog.h）：上一行先落盘，再持住写者，等手上那一轮写完。挂住了就一直持着，定时复位和 panic 的最后快照照样写得出去。失败和走完都放开。 */
static void hold_writer(PbBringup *b)
{
    const PbHw *hw = b->hw;
    if (!hw->quiet) return;
    hw->msleep(hw->ctx, kCrumbMs);
    b->held = 1;
    hw->quiet(hw->ctx, 1);
    hw->msleep(hw->ctx, kQuietMs);
}
static void let_writer_go(PbBringup *b)
{
    if (!b->held) return;
    b->held = 0;
    b->hw->quiet(b->hw->ctx, 0);
}
static int failed(PbBringup *b, int step, int error, const char *what)
{
    char text[160];
    PbText t;
    b->step = -step;
    b->error = error;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] FAIL");
    pb_text_kd(&t, "step", step);
    pb_text_kd(&t, "err", error);
    pb_text_char(&t, ' ');
    pb_text_str(&t, what);
    line(b, text);
    let_writer_go(b);
    return error;
}

uint32_t pb_pin_config(int pull, unsigned function, unsigned drive_ma, int output)
{
    const uint32_t pull_bits = pull < 0 ? 1u : pull > 0 ? 3u : 0u;
    const uint32_t drive = drive_ma >= 2 && drive_ma <= 16 ? drive_ma / 2 - 1 : 0u;
    return pull_bits | (function & 0xfu) << 2 | (drive & 7u) << 6 | (output ? 1u << 9 : 0u);
}

void pb_rcg_values(const PbRate *r, uint32_t *m, uint32_t *n, uint32_t *d, uint32_t *cfg)
{
    /* 双沿 M/N（m != n 时 CFG 模式 2），N 寄存器存 ~(n - m)，D 寄存器存 ~n（50% 占空比），分频字段 2×div - 1 */
    *m = r->m;
    *n = ~(uint32_t)(r->n - r->m) & 0xffffu;
    *d = ~(uint32_t)r->n & 0xffffu;
    *cfg = (r->m != r->n ? 2u << 12 : 0u) | (uint32_t)r->source << 8 | (uint32_t)(r->div2x - 1u);
}

static uint32_t read_halts(PbBringup *b)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t v = rd(b, PbWinGcc, kBranches[i].halt);
        if ((v & PB_CBCR_OFF) && !(v & PB_CBCR_HW_CTL)) off |= 1u << i;
    }
    return off;
}

static void log_rail(PbBringup *b, const char *when, unsigned i)
{
    const PbRailResult *r = &b->rails[i];
    char text[160];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] rail ");
    pb_text_str(&t, kPbRails[i].resource);
    pb_text_char(&t, ' ');
    pb_text_str(&t, when);
    if (r->read) {
        pb_text_kx(&t, "status", r->status);
        pb_text_kx(&t, "en", r->enable);
        pb_text_kx(&t, "mode", r->mode);
        pb_text_kd(&t, "mv", r->millivolts);
    } else {
        pb_text_str(&t, " unread");
    }
    if (r->voted) pb_text_kd(&t, "voted", r->voted);
    if (r->skipped) pb_text_str(&t, " kept");
    if (r->address) pb_text_kx(&t, "addr", r->address);
    if (r->rc) pb_text_kd(&t, "rc", r->rc);
    line(b, text);
}
static void read_rail(PbBringup *b, unsigned i)
{
    PbRailResult *r = &b->rails[i];
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    r->read = 0;
    if (b->hw->pmic(b->hw->ctx, kPbRails[i].ppid, &status, &enable, &mode, &mv)) return;
    r->read = 1;
    r->status = status;
    r->enable = enable;
    r->mode = mode;
    r->millivolts = mv;
}
static unsigned rail_s7i(void)
{
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) if (pb_streq(kPbRails[i].resource, PB_RAIL_S7I)) return i;
    return 0;
}
static void log_bt_en(PbBringup *b, const char *when, const PbGpioState *s, int rc)
{
    char text[160];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] bt_en ");
    pb_text_str(&t, when);
    pb_text_kd(&t, "apid", s->apid);
    pb_text_kd(&t, "owner", s->owner);
    pb_text_kx(&t, "sub", s->regs[1]);
    pb_text_kx(&t, "mode", s->after[2]);
    pb_text_kx(&t, "src", s->after[5]);
    pb_text_kx(&t, "en", s->after[7]);
    pb_text_kd(&t, "written", s->written);
    pb_text_kd(&t, "rc", rc);
    line(b, text);
}

int pb_survey(PbBringup *b)
{
    const PbHw *hw = b->hw;
    if (!hw || !hw->read || !hw->write || !hw->udelay || !hw->msleep || !hw->now_ms || !hw->pmic || !hw->bt_en) return PbErrArgument;
    char text[190];
    PbText t;
    b->vote = rd(b, PbWinGcc, PbGccVote);
    b->gpll0 = rd(b, PbWinGcc, PbGpll0Mode);
    b->rcg_cmd = rd(b, PbWinGcc, PbRcg);
    b->rcg_cfg = rd(b, PbWinGcc, PbRcg + PbRcgCfg);
    b->rcg_dfs = rd(b, PbWinGcc, PbRcg + PbRcgDfs);
    b->halts = read_halts(b);
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] survey");
    pb_text_kx(&t, "v52010", b->vote);
    pb_text_kx(&t, "gpll0", b->gpll0);
    pb_text_kx(&t, "rcg", b->rcg_cmd);
    pb_text_kx(&t, "cfg", b->rcg_cfg);
    pb_text_kx(&t, "dfs", b->rcg_dfs);
    pb_text_kx(&t, "halts", b->halts);
    line(b, text);
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] pins");
    for (unsigned i = 0; i < 6; ++i) {
        b->pin_ctl[i] = rd(b, PbWinTlmm, (uint32_t)kLogPins[i] * PbPinStride);
        b->pin_io[i] = rd(b, PbWinTlmm, (uint32_t)kLogPins[i] * PbPinStride + 4);
        pb_text_char(&t, ' ');
        pb_text_udec(&t, kLogPins[i]);
        pb_text_char(&t, '=');
        pb_text_hex(&t, b->pin_ctl[i] & 0x3fffu);
        pb_text_char(&t, '/');
        pb_text_udec(&t, b->pin_io[i] & 3u);
    }
    line(b, text);
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        read_rail(b, i);
        log_rail(b, "now", i);
    }
    b->sleep_clock = hw->sleep_clock ? hw->sleep_clock(hw->ctx, 0) : -1;
    PbGpioState en;
    const int rc = hw->bt_en(hw->ctx, -1, &en);
    log_bt_en(b, "now", &en, rc);
    return done(b, PbStepSurvey);
}

/* GENI 层经 SE 窗口访问寄存器。 */
static uint32_t se_read(void *ctx, uint32_t reg) {return rd((PbBringup *)ctx, PbWinSe, reg);}
static void se_write(void *ctx, uint32_t reg, uint32_t value) {wr((PbBringup *)ctx, PbWinSe, reg, value);}
static void se_delay(void *ctx, unsigned us) {PbBringup *b = (PbBringup *)ctx; b->hw->udelay(b->hw->ctx, us);}
static void se_sleep(void *ctx, unsigned ms) {PbBringup *b = (PbBringup *)ctx; b->hw->msleep(b->hw->ctx, ms);}
static uint64_t se_now(void *ctx) {PbBringup *b = (PbBringup *)ctx; return b->hw->now_ms(b->hw->ctx);}

int pb_clocks(PbBringup *b)
{
    const PbHw *hw = b->hw;
    /* 目前只有这里用 0x52010，仍然走锁字和关中断的读改写：将来的用户不会被这里写丢位 */
    if (hw->vote_lock && hw->vote_lock(hw->ctx, 1)) return failed(b, PbStepClocks, PbErrLock, "GCC vote lock busy");
    const uint32_t now = rd(b, PbWinGcc, PbGccVote);
    if ((now & PbVoteBits) != PbVoteBits) wr(b, PbWinGcc, PbGccVote, now | PbVoteBits);
    if (hw->vote_lock) hw->vote_lock(hw->ctx, 0);
    b->vote = rd(b, PbWinGcc, PbGccVote);
    if ((b->vote & PbVoteBits) != PbVoteBits) return failed(b, PbStepClocks, PbErrBranch, "a branch vote did not stick");
    for (unsigned n = 0; n < kBranchPolls && (b->halts = read_halts(b)); ++n) hw->udelay(hw->ctx, kBranchStepUs);
    char text[190];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] clocks");
    pb_text_kx(&t, "v52010", b->vote);
    pb_text_kx(&t, "halts", b->halts);
    pb_text_kx(&t, "rcg", rd(b, PbWinGcc, PbRcg));
    line(b, text);
    /* 分支没起来时绝不读 SE：读没时钟的块会把总线挂住 */
    if (b->halts) return failed(b, PbStepClocks, PbErrBranch, "a branch did not start: the SE was not read");
    /* 这次是 XNU 第一次读这个块，QUP2 的总线投票这版没做：挂住时看门狗的复位不打招呼，落在 UFS 写上就是掉电（UfsLog.h）。 */
    hold_writer(b);
    const PbGeniIo io = {b, se_read, se_write, se_delay, se_sleep, se_now};
    const int rc = pb_geni_probe(&b->geni, &io, &b->info);
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] geni");
    pb_text_kx(&t, "fw", b->info.fw);
    pb_text_kx(&t, "proto", b->info.proto);
    pb_text_kx(&t, "if_disable", b->info.if_disable);
    pb_text_kx(&t, "status", b->info.status);
    pb_text_kx(&t, "ios", b->info.ios);
    pb_text_kd(&t, "tx_words", b->info.tx_depth);
    pb_text_kd(&t, "rx_words", b->info.rx_depth);
    line(b, text);
    let_writer_go(b);
    if (rc) return failed(b, PbStepClocks, PbErrGeni, "SE firmware is not a usable UART");
    return done(b, PbStepClocks);
}

int pb_power(PbBringup *b)
{
    const PbHw *hw = b->hw;
    char text[160];
    PbText t;
    /* 从睡眠时钟脚的第一次写到投票走完，线上不能有 UFS 写：S7I 带着 UFS（PaganiWifi 的 rails 段同样的理由）。 */
    line(b, "[bt] guard rails");
    hold_writer(b);
    /* 32 kHz 睡眠时钟：另一个 EE 占着时只记录，引导可能已经开了，握手这一步见分晓 */
    if (hw->sleep_clock) {
        b->sleep_clock = hw->sleep_clock(hw->ctx, 1);
        pb_text_start(&t, text, sizeof text);
        pb_text_str(&t, b->sleep_clock == 2 ? "[bt] sleep clock pin switched on" : b->sleep_clock == 1 ? "[bt] sleep clock pin already on" :
                                                                                  "[bt] WARN sleep clock pin not switched on");
        pb_text_kd(&t, "rc", b->sleep_clock);
        line(b, text);
    }
    if (!hw->vrm) return failed(b, PbStepPower, PbErrRail, "no RPMh");
    const unsigned ufs = rail_s7i();
    read_rail(b, ufs);
    const PbRailResult before = b->rails[ufs];
    /* 读不到 S7I 就什么都不投：不能让芯片的负载落到一条没看过的轨道上，它还带着 UFS */
    if (!before.read) {
        log_rail(b, "unread", ufs);
        return failed(b, PbStepPower, PbErrRailRead, "S7I not read: no rail voted");
    }
    for (unsigned order = 1; order <= PB_RAIL_COUNT; ++order) {
        unsigned i = 0;
        while (i < PB_RAIL_COUNT && kPbRails[i].order != order) ++i;
        if (i == PB_RAIL_COUNT) continue;
        const PbRail *rail = &kPbRails[i];
        PbRailResult *r = &b->rails[i];
        read_rail(b, i);
        uint32_t mv = rail->chosenMv;
        int set_voltage = 1, set_enable = 1;
        if (r->read && r->millivolts > mv) {
            if (r->millivolts <= rail->maxMv) mv = r->millivolts;   /* 已经比要的高：沿用，绝不往下投 */
            else set_voltage = 0;                                   /* 超过范围：不是我们该动的值 */
        }
        if (i == ufs) {
            if (r->read && r->millivolts >= rail->chosenMv) set_voltage = 0;
            if (r->read && (r->enable & 0x80)) set_enable = 0;
            if (!r->read) {                                          /* 投到一半读不到了：停，已投的不撤 */
                log_rail(b, "unread", i);
                return failed(b, PbStepPower, PbErrRailRead, "S7I not read: not voted");
            }
        }
        uint32_t address = 0;
        /* 模式总要投：设备树让 S7I 停在低模式，等 200 mA 的负载投票才进 HPM，这个内核不投负载；模式只升不降 */
        int rc = hw->vrm(hw->ctx, rail->resource, PbVrmMode, PbVrmModeHigh, &address);
        if (!rc && set_voltage) rc = hw->vrm(hw->ctx, rail->resource, PbVrmVoltage, mv, &address);
        if (!rc && set_enable) rc = hw->vrm(hw->ctx, rail->resource, PbVrmEnable, 1, &address);
        r->address = address;
        r->rc = rc;
        r->voted = set_voltage ? (uint16_t)mv : 0;
        r->skipped = (uint8_t)!set_voltage;
        if (rc) {
            log_rail(b, "vote failed", i);
            return failed(b, PbStepPower, PbErrRail, rail->resource);
        }
    }
    hw->msleep(hw->ctx, kRailSettleMs);
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        read_rail(b, i);
        log_rail(b, "after", i);
    }
    /* 投后读回。S7I 读不到、比原来低、掉了使能、不够芯片要的电压，都停在这里：BT_EN 不拉，芯片就不从它取电 */
    const PbRailResult *after = &b->rails[ufs];
    if (!after->read) return failed(b, PbStepPower, PbErrRailRead, "S7I not read back");
    if (after->millivolts < before.millivolts || ((before.enable & 0x80) && !(after->enable & 0x80)))
        return failed(b, PbStepPower, PbErrRail, "S7I dropped");
    if (after->millivolts < kPbRails[ufs].chosenMv || !(after->enable & 0x80)) return failed(b, PbStepPower, PbErrRail, "S7I below what the chip needs");
    /* RF 参考时钟只投开，绝不投关 */
    int rc = hw->vrm(hw->ctx, PB_RF_CLOCK, PbRfClockEnable, 1, &b->rf_clock);
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] " PB_RF_CLOCK);
    pb_text_kx(&t, "addr", b->rf_clock);
    pb_text_kd(&t, "rc", rc);
    line(b, text);
    if (rc) return failed(b, PbStepPower, PbErrRail, PB_RF_CLOCK);
    let_writer_go(b);
    return done(b, PbStepPower);
}

int pb_bt_enable(PbBringup *b)
{
    const PbHw *hw = b->hw;
    PbGpioState s;
    /* 芯片的负载随 BT_EN 落到 S7I 上（PaganiWifi 的 wlan_en 段同样的理由）。 */
    line(b, "[bt] guard bt_en");
    hold_writer(b);
    /* WLAN_EN 只记录：HAL 要求两者动作隔 100 ms，线程之间没有共享的时间戳，轮次清单规定两者不同时首测 */
    const uint32_t wlan = rd(b, PbWinTlmm, (uint32_t)PbPinWlanEn * PbPinStride + 4) & 3u;
    int rc = hw->bt_en(hw->ctx, 0, &s);
    log_bt_en(b, "low", &s, rc);
    if (rc) return failed(b, PbStepBtEn, PbErrBtEn, "BT_EN not driven low");
    const uint64_t low_at = hw->now_ms(hw->ctx);
    hw->msleep(hw->ctx, PbBtEnLowMs);
    /* 一次睡眠可能提前醒，低电平时间按时钟算够 */
    while (hw->now_ms(hw->ctx) - low_at < PbBtEnLowMs) hw->msleep(hw->ctx, 1);
    rc = hw->bt_en(hw->ctx, 1, &s);
    log_bt_en(b, "high", &s, rc);
    if (rc) return failed(b, PbStepBtEn, PbErrBtEn, "BT_EN not driven high");
    hw->msleep(hw->ctx, PbBtEnHighMs);
    char text[160];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] after bt_en");
    pb_text_kx(&t, "wlan_en", wlan);
    pb_text_kx(&t, "sw_ctrl", rd(b, PbWinTlmm, (uint32_t)PbPinSwCtrl * PbPinStride + 4) & 3u);
    pb_text_kd(&t, "low_ms", (int64_t)(hw->now_ms(hw->ctx) - low_at) - PbBtEnHighMs);
    line(b, text);
    let_writer_go(b);
    return done(b, PbStepBtEn);
}

static const PbRate *rate_of(uint32_t baud)
{
    for (unsigned i = 0; i < PB_RATE_COUNT; ++i) if (kPbRates[i].baud == baud) return &kPbRates[i];
    return NULL;
}

/* 动态频率表开着时不能写 CFG，速率由 CLK_SEL 选：在 8 级里找一样的（源、分频、M、N）。找不到返回负数。 */
static int dfs_level(PbBringup *b, const PbRate *r)
{
    uint32_t m, n, d, cfg;
    pb_rcg_values(r, &m, &n, &d, &cfg);
    for (unsigned i = 0; i < PbRcgLevels; ++i) {
        const uint32_t level = rd(b, PbWinGcc, PbRcg + PbRcgPerf + 4 * i);
        if ((level & 0x371fu) != cfg) continue;
        if (!(cfg >> 12)) return (int)i;
        if ((rd(b, PbWinGcc, PbRcg + PbRcgPerfM + 4 * i) & 0xffffu) == m && (rd(b, PbWinGcc, PbRcg + PbRcgPerfN + 4 * i) & 0xffffu) == n) return (int)i;
    }
    return -1;
}

static int set_rate(PbBringup *b, const PbRate *r)
{
    const PbHw *hw = b->hw;
    b->rcg_dfs = rd(b, PbWinGcc, PbRcg + PbRcgDfs);
    if (b->rcg_dfs & 1u) {
        const int level = dfs_level(b, r);
        if (level < 0) return PbErrRcg;
        b->level = (uint32_t)level;
        return 0;
    }
    uint32_t m, n, d, cfg;
    pb_rcg_values(r, &m, &n, &d, &cfg);
    wr(b, PbWinGcc, PbRcg + PbRcgM, m);
    wr(b, PbWinGcc, PbRcg + PbRcgN, n);
    wr(b, PbWinGcc, PbRcg + PbRcgD, d);
    wr(b, PbWinGcc, PbRcg + PbRcgCfg, cfg);
    wr(b, PbWinGcc, PbRcg, rd(b, PbWinGcc, PbRcg) | PbRcgUpdate);
    for (unsigned i = 0; i < kRcgPolls; ++i) {
        if (!(rd(b, PbWinGcc, PbRcg) & PbRcgUpdate)) {
            b->level = 0;
            return 0;
        }
        hw->udelay(hw->ctx, kRcgStepUs);
    }
    return PbErrRcg;
}

/* 内部回环：引脚还是 GPIO，字节到不了芯片。验证时钟、打包、FIFO 和命令序列，结果只记录，寄存器语义没上过机，不拿它拦路。 */
static void loopback_test(PbBringup *b)
{
    static const uint8_t probe[8] = {0x55, 0xaa, 0x31, 0x00, 0xf0, 0xff, 0x02, 0x05};
    uint8_t got[32];
    uint32_t have = 0;
    pb_geni_loopback(&b->geni, 1);
    pb_geni_flow(&b->geni, 0, 1);
    int rc = pb_geni_rx_start(&b->geni);
    if (!rc) rc = pb_geni_write(&b->geni, probe, sizeof probe, 100);
    if (!rc) {
        const uint64_t end = b->hw->now_ms(b->hw->ctx) + kLoopbackWaitMs;
        while (have < sizeof probe && b->hw->now_ms(b->hw->ctx) < end) {
            have += pb_geni_read(&b->geni, got + have, (uint32_t)sizeof got - have);
            b->hw->msleep(b->hw->ctx, 1);
        }
    }
    b->loopback = have == sizeof probe;
    for (uint32_t i = 0; b->loopback && i < sizeof probe; ++i) if (got[i] != probe[i]) b->loopback = 0;
    (void)pb_geni_rx_stop(&b->geni);
    pb_geni_loopback(&b->geni, 0);
    pb_geni_flow(&b->geni, 0, 0);
    char text[160];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] loopback");
    pb_text_kd(&t, "rc", rc);
    pb_text_kd(&t, "bytes", have);
    pb_text_kd(&t, "ok", b->loopback);
    line(b, text);
}

int pb_uart(PbBringup *b)
{
    const PbRate *r = rate_of(115200);
    int rc = set_rate(b, r);
    char text[160];
    PbText t;
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] rcg");
    pb_text_kx(&t, "cmd", rd(b, PbWinGcc, PbRcg));
    pb_text_kx(&t, "cfg", rd(b, PbWinGcc, PbRcg + PbRcgCfg));
    pb_text_kx(&t, "dfs", b->rcg_dfs);
    pb_text_kd(&t, "level", b->level);
    pb_text_kd(&t, "rc", rc);
    line(b, text);
    if (rc) return failed(b, PbStepUart, PbErrRcg, "RCG did not take 7.3728 MHz");
    if (pb_geni_config(&b->geni, r->seDiv, b->level)) return failed(b, PbStepUart, PbErrGeni, "SE not configured");
    b->baud = r->baud;
    loopback_test(b);
    /* 切功能前保持 RFR 未就绪，切完才放开，防止对端看到毛刺 */
    for (unsigned i = 0; i < 4; ++i) {
        const PbUartPin *p = &kPbUartPins[i];
        const uint32_t off = (uint32_t)p->gpio * PbPinStride;
        wr(b, PbWinTlmm, off, (rd(b, PbWinTlmm, off) & ~0x3ffu) | pb_pin_config(p->pull, PbPinFunction, PbPinDriveMa, 0));
    }
    if (pb_geni_rx_start(&b->geni)) return failed(b, PbStepUart, PbErrGeni, "RX not started");
    pb_geni_flow(&b->geni, 1, 1);
    /* 清掉切引脚时进来的杂字节 */
    uint8_t junk[64];
    (void)pb_geni_read(&b->geni, junk, sizeof junk);
    b->ios = pb_geni_ios(&b->geni);
    pb_text_start(&t, text, sizeof text);
    pb_text_str(&t, "[bt] uart");
    pb_text_kd(&t, "baud", b->baud);
    pb_text_kx(&t, "ios", b->ios);
    pb_text_kd(&t, "cts", !(b->ios & PB_IOS_CTS));
    pb_text_kx(&t, "clk", rd(b, PbWinSe, PB_SE_M_CLK_CFG));
    line(b, text);
    return done(b, PbStepUart);
}

int pb_uart_baud(PbBringup *b, uint32_t baud)
{
    const PbRate *r = rate_of(baud);
    if (!r) return PbErrArgument;
    if (pb_geni_rx_stop(&b->geni)) return PbErrGeni;
    int rc = set_rate(b, r);
    if (!rc) rc = pb_geni_set_clock(&b->geni, r->seDiv, b->level);
    if (!rc) rc = pb_geni_rx_start(&b->geni);
    if (!rc) b->baud = baud;
    return rc;
}
