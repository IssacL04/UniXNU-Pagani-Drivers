/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 相机上电的硬件模型。规矩写进模型：读写一个没开时钟的块就是挂总线，测试当场失败；第一次碰可能挂住的块时，写者必须已经持住，上一行必须已经有时间落盘。
 * 模型只知道寄存器和时序，不引用 CamBringup.c 的表：板级数字从 PaganiCameraBoard.h 来，闸门的白名单在这里另写一份，两边不一致就失败。 */
#ifndef PAGANI_CAM_TEST_MODEL_H
#define PAGANI_CAM_TEST_MODEL_H
#include <stdarg.h>
#include "check.h"
#include "../CamBringup.h"
#include "../CamUtil.h"

enum {MaxOps = 32768, MaxLines = 64, MaxQueue = 64};
typedef enum {OpRead, OpWrite, OpArc, OpVrm, OpQuiet, OpSleep} OpKind;
typedef struct {OpKind kind; unsigned win; uint32_t off, value; const char *name; unsigned tick;} Op;

typedef struct {
    Op ops[MaxOps];
    unsigned nops;
    char lines[MaxLines][200];
    unsigned nlines;
    uint64_t now_us, quiet_since_us, last_line_us;
    unsigned quiet;
    /* 硬件状态 */
    uint32_t gcc[3];                /* bist, ahb, xo */
    uint32_t always[4];             /* DrvAhb, DrvXo, GdscClk, SleepClk */
    uint32_t gdscr, cfg_gdscr;
    uint32_t slow_cfg, fast_cfg, cci_cfg, slow_cmd, fast_cmd, cci_cmd;
    uint32_t core_ahb, top_ahb, top_fast, cci0;
    uint32_t mclk_cfg, mclk_cmd, mclk_cbcr;
    uint32_t pin_ctl[4], pin_io[4];
    int rail_on[3];
    uint16_t rail_mv[3];
    uint32_t rail_mode[3];
    uint32_t arc_mm, arc_mxc;
    /* CCI */
    uint32_t cci_version, cci_status, cci_mask, cci_queue[MaxQueue], cci_exec, cci_read_word, cci_read_level, cci_timing[5];
    unsigned cci_queued, cci_resets, cci_halts;
    int cci_inited;
    uint16_t sensor_page;
    uint8_t sensor_address;
    uint16_t sensor_id;
    unsigned sensor_writes, sensor_reads, nacks;
    /* 故障注入 */
    int gcc_stuck, gdsc_stuck, gdsc_hw_ctl, rcg_stuck, branch_stuck, mclk_stuck, camcc_ones, arc_fail, vrm_fail_at, pmic_unread, pmic_wrong_mv, pin_busy;
    int cci_reset_stuck, off_stuck, core_stuck;
    unsigned vrm_calls;
    int touched_camcc, touched_bist, touched_cci, touched_rail;
} Model;

static Model *M;
static void hang(const char *what)
{
    fprintf(stderr, "bus hang: %s\n", what);
    exit(1);
}
static void record(OpKind kind, unsigned win, uint32_t off, uint32_t value, const char *name)
{
    CHECK(M->nops < MaxOps);
    M->ops[M->nops++] = (Op){kind, win, off, value, name, 0};
}

static int gcc_running(uint32_t v) {return (v & PCAM_CBCR_ENABLE) && !(v & PCAM_CBCR_OFF);}
static int gdsc_powered(void) {return (M->gdscr & PCAM_GDSC_PWR_ON) != 0;}

/* 第一次碰有风险的块：写者持住了，并且持住之前上一行等够了一轮日志。 */
static void guarded(const char *what)
{
    if (!M->quiet) {
        fprintf(stderr, "%s touched without the writer held\n", what);
        exit(1);
    }
    CHECK(M->quiet_since_us >= M->last_line_us + 3500000);
    CHECK(M->now_us >= M->quiet_since_us + 1000000);
}

static void first_touch(int *done, const char *what)
{
    if (*done) return;
    guarded(what);
    *done = 1;
}

/* 级 4：这一段里有日志行，模型只看写者是不是还持着。 */
static void writer_held(const char *what)
{
    if (M->quiet) return;
    fprintf(stderr, "%s without the writer held\n", what);
    exit(1);
}

static uint32_t cbcr_value(uint32_t v, int stuck)
{
    v &= ~PCAM_CBCR_OFF;
    return (v & PCAM_CBCR_ENABLE) && !stuck ? v : v | PCAM_CBCR_OFF;
}

static uint32_t *camcc_cell(uint32_t off)
{
    switch (off) {
    case PcamCcDrvAhb: return &M->always[0];
    case PcamCcDrvXo: return &M->always[1];
    case PcamCcGdscClk: return &M->always[2];
    case PcamCcSleepClk: return &M->always[3];
    case PcamCcGdscr: return &M->gdscr;
    case PcamCcCfgGdscr: return &M->cfg_gdscr;
    case PcamCcSlowAhbRcg + PcamRcgCfg: return &M->slow_cfg;
    case PcamCcFastAhbRcg + PcamRcgCfg: return &M->fast_cfg;
    case PcamCcCciRcg + PcamRcgCfg: return &M->cci_cfg;
    case PcamCcSlowAhbRcg: return &M->slow_cmd;
    case PcamCcFastAhbRcg: return &M->fast_cmd;
    case PcamCcCciRcg: return &M->cci_cmd;
    case PcamCcCoreAhb: return &M->core_ahb;
    case PcamCcTopAhb: return &M->top_ahb;
    case PcamCcTopFastAhb: return &M->top_fast;
    case PcamCcCci0: return &M->cci0;
    default: return NULL;
    }
}

static void cci_requires(void)
{
    if (!gdsc_powered()) hang("CCI with the GDSC off");
    if (!(M->core_ahb & PCAM_CBCR_ENABLE)) hang("CCI without core AHB");
    if (!gcc_running(cbcr_value(M->top_ahb, 0))) hang("CCI without top AHB");
    if (!gcc_running(cbcr_value(M->top_fast, 0))) hang("CCI without top fast AHB");
    if (!gcc_running(cbcr_value(M->cci0, 0))) hang("CCI without its clock");
}

static int pin_index(uint32_t off)
{
    const uint32_t pin = off / PcamPinStride;
    return pin == PcamPinMclk0 ? 0 : pin == PcamPinSda0 ? 1 : pin == PcamPinScl0 ? 2 : pin == PcamPinReset ? 3 : -1;
}

/* 传感器能应答：三路轨道都在白名单电压上、MCLK 在跑且脚是 cam_mclk、CCI 脚对、复位放开。 */
static int sensor_alive(void)
{
    for (unsigned i = 0; i < 3; ++i) if (!M->rail_on[i] || M->rail_mv[i] != kPcamRails[i].millivolts) return 0;
    if (!gcc_running(cbcr_value(M->mclk_cbcr, 0))) return 0;
    if ((M->pin_ctl[0] & PcamCtlMask) != PcamCtlMclk || (M->pin_ctl[1] & PcamCtlMask) != PcamCtlCci || (M->pin_ctl[2] & PcamCtlMask) != PcamCtlCci) return 0;
    return (M->pin_ctl[3] & (1u << 9)) && (M->pin_io[3] & PcamIoOut);
}

static void cci_finish(uint32_t bits)
{
    M->cci_status |= bits;
}

static void cci_run(void)
{
    /* 解释队列：第一个字是 SET_PARAM。 */
    CHECK(M->cci_exec == M->cci_queued && M->cci_queued >= 2);
    CHECK(M->cci_inited);
    uint32_t w0 = M->cci_queue[0];
    CHECK((w0 & 0xf) == PcamCciCmdSetParam);
    const uint8_t addr = (uint8_t)((w0 >> 4) & 0x7f);
    CHECK(((w0 >> PcamCciRetryShift) & 3u) == PcamCciRetries);
    uint32_t w1 = M->cci_queue[1];
    M->cci_queued = 0;
    M->cci_exec = 0;
    if (addr != M->sensor_address || !sensor_alive()) {
        M->nacks++;
        cci_finish(0x08000000u); /* M0 Q0 的 NACK，错误掩码里的一位 */
        return;
    }
    if ((w1 & 0xf) == PcamCciCmdWrite) {
        CHECK(((w1 >> 4) & 0xf) == 4);
        const uint16_t reg = (uint16_t)(((w1 >> 8) & 0xff) << 8 | ((w1 >> 16) & 0xff));
        const uint16_t value = (uint16_t)((w1 >> 24) << 8 | (M->cci_queue[2] & 0xff));
        CHECK((M->cci_queue[3] & 0xf) == PcamCciCmdReport && (M->cci_queue[3] & PcamCciReportIrq));
        if (reg == PcamTelePageReg) M->sensor_page = value;
        M->sensor_writes++;
        cci_finish(PCAM_CCI_IRQ_REPORT);
        return;
    }
    CHECK((w1 & 0xf) == PcamCciCmdLock);
    uint32_t w2 = M->cci_queue[2], w3 = M->cci_queue[3], w4 = M->cci_queue[4];
    CHECK((w2 & 0xf) == PcamCciCmdWriteNoStop && ((w2 >> 4) & 0xf) == 2);
    CHECK((w3 & 0xf) == PcamCciCmdRead && ((w3 >> 4) & 0xf) == 2);
    CHECK((w4 & 0xf) == PcamCciCmdUnlock);
    const uint16_t reg = (uint16_t)(((w2 >> 8) & 0xff) << 8 | ((w2 >> 16) & 0xff));
    uint16_t value = 0;
    if (reg == PcamTeleIdReg && M->sensor_page == PcamTelePageValue) value = M->sensor_id;
    M->cci_read_word = 0x5au | (uint32_t)(value >> 8) << 8 | (uint32_t)(value & 0xff) << 16;
    M->cci_read_level = 1;
    M->sensor_reads++;
    cci_finish(PCAM_CCI_IRQ_RD_DONE);
}

static uint32_t m_read(void *ctx, unsigned win, uint32_t off)
{
    (void)ctx;
    CHECK(win < PcamWinCount);
    CHECK(!(off & 3) && off + 4 <= kPcamWinBytes[win]);
    record(OpRead, win, off, 0, NULL);
    switch (win) {
    case PcamWinGcc:
        if (off == PcamGccBistAhb) return cbcr_value(M->gcc[0], M->gcc_stuck);
        if (off == PcamGccCameraAhb) return cbcr_value(M->gcc[1], M->gcc_stuck);
        if (off == PcamGccCameraXo) return cbcr_value(M->gcc[2], M->gcc_stuck);
        CHECK(0 && "GCC read outside the list");
        return 0;
    case PcamWinTlmm: {
        const int i = pin_index(off);
        CHECK(i >= 0);
        CHECK(off % PcamPinStride == 0 || off % PcamPinStride == 4);
        return off % PcamPinStride ? M->pin_io[i] : M->pin_ctl[i];
    }
    case PcamWinCamcc: {
        if (!gcc_running(cbcr_value(M->gcc[1], 0))) hang("CAMCC without the GCC AHB branch");
        first_touch(&M->touched_camcc, "CAMCC");
        if (M->camcc_ones) return 0xffffffffu;
        uint32_t *cell = camcc_cell(off);
        CHECK(cell != NULL);
        uint32_t v = *cell;
        if (off == PcamCcCoreAhb) return v;
        if (off == PcamCcTopAhb || off == PcamCcTopFastAhb || off == PcamCcCci0) return cbcr_value(v, M->branch_stuck == (int)off);
        if ((off == PcamCcSlowAhbRcg || off == PcamCcFastAhbRcg || off == PcamCcCciRcg) && (v & PCAM_RCG_UPDATE) && !M->rcg_stuck) *cell = v & ~PCAM_RCG_UPDATE;
        return v;
    }
    case PcamWinBist:
        if (!gcc_running(cbcr_value(M->gcc[0], 0))) hang("BIST CC without its GCC branch");
        first_touch(&M->touched_bist, "BIST CC");
        if (off == PcamBistMclk0Cbcr) return cbcr_value(M->mclk_cbcr, M->mclk_stuck);
        if (off == PcamBistMclk0Rcg + PcamRcgCfg) return M->mclk_cfg;
        if (off == PcamBistMclk0Rcg) {
            const uint32_t v = M->mclk_cmd;
            if ((v & PCAM_RCG_UPDATE) && !M->rcg_stuck) M->mclk_cmd = v & ~PCAM_RCG_UPDATE;
            return v;
        }
        CHECK(0 && "BIST read outside the list");
        return 0;
    case PcamWinCci:
        cci_requires();
        first_touch(&M->touched_cci, "CCI");
        switch (off) {
        case PcamCciVersion: return M->cci_version;
        case PcamCciCurWords: return M->cci_queued;
        case PcamCciIrqStatus0: return M->cci_status;
        case PcamCciReadLevel: return M->cci_read_level;
        case PcamCciReadData: M->cci_read_level = 0; return M->cci_read_word;
        default:
            CHECK(0 && "CCI read outside the list");
            return 0;
        }
    default:
        return 0;
    }
}

/* 模型自己的白名单：闸门放过去的写，这里还要再对一遍。 */
static void m_write(void *ctx, unsigned win, uint32_t off, uint32_t value)
{
    (void)ctx;
    CHECK(win < PcamWinCount);
    CHECK(!(off & 3) && off + 4 <= kPcamWinBytes[win]);
    record(OpWrite, win, off, value, NULL);
    switch (win) {
    case PcamWinGcc:
        CHECK(value & PCAM_CBCR_ENABLE);
        if (off == PcamGccBistAhb) M->gcc[0] = value;
        else if (off == PcamGccCameraAhb) M->gcc[1] = value;
        else if (off == PcamGccCameraXo) M->gcc[2] = value;
        else CHECK(0 && "GCC write outside the list");
        return;
    case PcamWinTlmm: {
        const int i = pin_index(off);
        CHECK(i >= 0);
        if (off % PcamPinStride) {
            CHECK(i == 3 && off % PcamPinStride == 4);      /* 只有复位脚写电平 */
            M->pin_io[i] = value;
            return;
        }
        const uint32_t function = (value >> PcamCtlFunctionShift) & PcamCtlFunctionMask;
        CHECK(function == 0 || (i < 3 && function == 1));
        /* 脚从 GPIO 切 cam_mclk 之前，时钟要在跑；CCI 脚同理不要求。 */
        if (i == 0 && function == 1) CHECK(gcc_running(cbcr_value(M->mclk_cbcr, 0)));
        M->pin_ctl[i] = value;
        return;
    }
    case PcamWinCamcc: {
        if (!gcc_running(cbcr_value(M->gcc[1], 0))) hang("CAMCC without the GCC AHB branch");
        CHECK(M->touched_camcc);
        uint32_t *cell = camcc_cell(off);
        CHECK(cell != NULL && off != PcamCcCfgGdscr);
        if (off == PcamCcGdscr) {
            guarded("the GDSC");
            CHECK(M->always[2] & PCAM_CBCR_ENABLE);         /* GDSC 的时钟先开 */
            CHECK(M->arc_mm && M->arc_mxc);                 /* 轨道先投 */
            CHECK(!(value & PCAM_GDSC_HW_CONTROL));
            *cell = value;
            if (!(value & PCAM_GDSC_SW_COLLAPSE) && !M->gdsc_stuck) {
                M->gdscr |= PCAM_GDSC_PWR_ON;
                M->cfg_gdscr |= PCAM_GDSC_CFG_UP_DONE;
            }
            return;
        }
        /* 分支和 RCG 都在 GDSC 里面：先上电 */
        if (off != PcamCcDrvAhb && off != PcamCcDrvXo && off != PcamCcGdscClk && off != PcamCcSleepClk) CHECK(gdsc_powered());
        if (off == PcamCcSlowAhbRcg + PcamRcgCfg || off == PcamCcFastAhbRcg + PcamRcgCfg || off == PcamCcCciRcg + PcamRcgCfg) CHECK(value == PCAM_RCG_CFG_XO);
        if (off == PcamCcCoreAhb && M->core_stuck) return;
        *cell = value;
        return;
    }
    case PcamWinBist:
        if (!gcc_running(cbcr_value(M->gcc[0], 0))) hang("BIST CC without its GCC branch");
        CHECK(M->touched_bist);
        writer_held("a BIST CC write");
        if (off == PcamBistMclk0Cbcr) M->mclk_cbcr = value;
        else if (off == PcamBistMclk0Rcg + PcamRcgCfg) {CHECK(value == PCAM_RCG_CFG_XO); M->mclk_cfg = value;}
        else if (off == PcamBistMclk0Rcg) M->mclk_cmd = value;
        else CHECK(0 && "BIST write outside the list");
        return;
    case PcamWinCci:
        cci_requires();
        CHECK(M->touched_cci);
        switch (off) {
        case PcamCciIrqMask0: M->cci_mask = value; return;
        case PcamCciIrqClear0: M->cci_status &= ~value; return;
        case PcamCciIrqGlobalClear: return;
        case PcamCciReset:
            CHECK(value == PcamCciResetAll);
            M->cci_resets++;
            M->cci_queued = 0;
            M->cci_inited = 0;
            memset(M->cci_timing, 0, sizeof M->cci_timing);
            if (!M->cci_reset_stuck) cci_finish(PCAM_CCI_IRQ_RESET_DONE);
            return;
        case PcamCciHalt:
            CHECK(value == PcamCciHaltM0);
            M->cci_halts++;
            cci_finish(PCAM_CCI_IRQ_HALT_ACK);
            return;
        case PcamCciSclCtl: M->cci_timing[0] = value; return;
        case PcamCciSdaCtl0: M->cci_timing[1] = value; return;
        case PcamCciSdaCtl1: M->cci_timing[2] = value; return;
        case PcamCciSdaCtl2: M->cci_timing[3] = value; return;
        case PcamCciMiscCtl: M->cci_timing[4] = value; M->cci_inited = M->cci_mask != 0; return;
        case PcamCciLoadData:
            CHECK(M->cci_queued < MaxQueue);
            M->cci_queue[M->cci_queued++] = value;
            return;
        case PcamCciExecWords: M->cci_exec = value; return;
        case PcamCciQueueStart: CHECK(value == 1u); cci_run(); return;
        default: CHECK(0 && "CCI write outside the list"); return;
        }
    default:
        return;
    }
}

static void m_udelay(void *ctx, unsigned us) {(void)ctx; M->now_us += us;}
static void m_msleep(void *ctx, unsigned ms) {(void)ctx; M->now_us += (uint64_t)ms * 1000; record(OpSleep, 0, ms, 0, NULL);}
static int m_arc(void *ctx, const char *resource, uint32_t vlvl, uint32_t *hlvl)
{
    (void)ctx;
    record(OpArc, 0, 0, vlvl, resource);
    CHECK(vlvl == 0x180);
    /* 绝不投 mx.lvl：GPU 的票在表顶 */
    CHECK(!strcmp(resource, "mmcx.lvl") || !strcmp(resource, "mxc.lvl"));
    if (M->arc_fail) return -110;
    if (M->gdsc_hw_ctl) M->gdscr |= PCAM_GDSC_HW_CONTROL;  /* 步 102 看的时候还没有 */
    if (!strcmp(resource, "mmcx.lvl")) M->arc_mm = vlvl;
    else M->arc_mxc = vlvl;
    *hlvl = 6;
    return 0;
}
static int rail_of(const char *resource)
{
    for (unsigned i = 0; i < 3; ++i) if (!strcmp(resource, kPcamRails[i].resource)) return (int)i;
    return -1;
}
static int m_vrm(void *ctx, const char *resource, uint32_t reg, uint32_t value, uint32_t *address)
{
    (void)ctx;
    record(OpVrm, reg, 0, value, resource);
    const int i = rail_of(resource);
    CHECK(i >= 0);
    M->vrm_calls++;
    if (M->vrm_fail_at && M->vrm_calls == (unsigned)M->vrm_fail_at) return -110;
    if (address) *address = 0x40000u + 0x100u * (unsigned)i;
    /* 复位在低才能上轨道；第一次上轨道之前上一行落盘，写者持住 */
    if (reg == PcamVrmEnable && value == 1) CHECK((M->pin_ctl[3] & (1u << 9)) && !(M->pin_io[3] & PcamIoOut));
    if (reg == PcamVrmEnable && value == 1) first_touch(&M->touched_rail, "a camera rail");
    writer_held("a VRM vote");
    if (reg == PcamVrmVoltage) {CHECK(value == kPcamRails[i].millivolts); M->rail_mv[i] = (uint16_t)value;}
    else if (reg == PcamVrmEnable) {if (!(M->off_stuck && !value)) M->rail_on[i] = (int)value;}
    else if (reg == PcamVrmMode) {CHECK(value == PcamVrmModeHigh); M->rail_mode[i] = value;}
    else CHECK(0 && "VRM register");
    return 0;
}
static int m_pmic(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    (void)ctx;
    int i = -1;
    for (unsigned k = 0; k < 3; ++k) if (kPcamRails[k].ppid == ppid) i = (int)k;
    CHECK(i >= 0);
    if (M->pmic_unread) return -19;
    *status = M->rail_on[i] ? 0x80 : 0;
    *enable = M->rail_on[i] ? 0x80 : 0;
    *mode = 7;
    *mv = (uint16_t)(M->rail_mv[i] + (M->pmic_wrong_mv == i + 1 && M->rail_on[i] ? 50 : 0));
    return 0;
}
static void m_log(void *ctx, const char *line)
{
    (void)ctx;
    CHECK(M->nlines < MaxLines);
    snprintf(M->lines[M->nlines++], sizeof M->lines[0], "%s", line);
    M->last_line_us = M->now_us;
}
static void m_quiet(void *ctx, unsigned on)
{
    (void)ctx;
    record(OpQuiet, 0, 0, on, NULL);
    M->quiet = on;
    if (on) M->quiet_since_us = M->now_us;
}

static const PcamHw kModelHw = {NULL, m_read, m_write, m_udelay, m_msleep, m_arc, m_vrm, m_pmic, m_log, m_quiet};

static void model_reset(Model *m)
{
    memset(m, 0, sizeof *m);
    M = m;
    m->gcc[0] = 0x80000000u;                /* 关着 */
    m->gcc[1] = 0x80000000u;
    m->gcc[2] = 0x80000000u;
    m->gdscr = 0x00222001u;                 /* 复位值：折叠 */
    m->slow_cfg = 0;
    m->fast_cfg = 0;
    m->cci_cfg = 0;
    m->mclk_cfg = 0;
    m->pin_ctl[0] = 0x1u;                   /* 输入、下拉 */
    m->pin_ctl[1] = 0x1u;
    m->pin_ctl[2] = 0x1u;
    m->pin_ctl[3] = 0x1u | PcamCtlEgpioPresent;
    m->cci_version = 0x10060000u;
    m->sensor_address = kPcamTeleAddresses[0];
    m->sensor_id = PcamTeleId;
}

static int model_log_has(const char *text)
{
    for (unsigned i = 0; i < M->nlines; ++i) if (strstr(M->lines[i], text)) return 1;
    return 0;
}
static unsigned model_writes(unsigned win)
{
    unsigned n = 0;
    for (unsigned i = 0; i < M->nops; ++i) if (M->ops[i].kind == OpWrite && M->ops[i].win == win) ++n;
    return n;
}
static unsigned model_count(OpKind kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < M->nops; ++i) if (M->ops[i].kind == kind) ++n;
    return n;
}
#endif
