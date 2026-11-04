/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CamBringup.c 对模型：每一级的范围、顺序、闸门，每条失败路径都安全停下。 */
#include "model.h"

static PcamBringup gB;
static Model gM;

static int run_level(unsigned level)
{
    memset(&gB, 0, sizeof gB);
    gB.hw = &kModelHw;
    gB.level = level;
    return pcam_run(&gB);
}

static void levels(void)
{
    CHECK(pcam_level(0) == 0 && pcam_level(5) == 0 && pcam_level(12) == 0);
    for (unsigned i = 1; i <= 4; ++i) CHECK(pcam_level(i) == i);
    memset(&gB, 0, sizeof gB);
    CHECK(pcam_run(&gB) == PcamErrArgument);
    CHECK(pcam_run(NULL) == PcamErrArgument);
    gB.hw = &kModelHw;
    CHECK(pcam_run(&gB) == PcamErrArgument);        /* 级 0 */
    gB.level = 5;
    CHECK(pcam_run(&gB) == PcamErrArgument);
}

/* 级 1：只读，碰不到 CAMCC。 */
static void level_one(void)
{
    model_reset(&gM);
    CHECK(run_level(1) == 0 && gB.step == PcamStepSurvey);
    CHECK(model_count(OpWrite) == 0 && model_count(OpVrm) == 0 && model_count(OpArc) == 0 && model_count(OpQuiet) == 0);
    for (unsigned i = 0; i < gM.nops; ++i) CHECK(gM.ops[i].win == PcamWinGcc || gM.ops[i].win == PcamWinTlmm);
    CHECK(model_log_has("[cam] survey gcc") && model_log_has("[cam] pins") && model_log_has("[cam] rail vio"));
}

/* 级 2：只写 GCC 的三个位，CAMCC 只读，第一次读有保护。 */
static void level_two(void)
{
    model_reset(&gM);
    CHECK(run_level(2) == 0 && gB.step == PcamStepCamcc);
    CHECK(model_writes(PcamWinGcc) == 3);
    CHECK(model_writes(PcamWinCamcc) == 0 && model_writes(PcamWinBist) == 0 && model_writes(PcamWinCci) == 0 && model_writes(PcamWinTlmm) == 0);
    CHECK(model_count(OpArc) == 0 && model_count(OpVrm) == 0);
    CHECK(model_count(OpQuiet) == 2 && !gM.quiet);
    /* 已经开着就不写 */
    model_reset(&gM);
    gM.gcc[0] = gM.gcc[1] = gM.gcc[2] = PCAM_CBCR_ENABLE;
    CHECK(run_level(2) == 0);
    CHECK(model_writes(PcamWinGcc) == 0);
    /* 分支起不来：CAMCC 一次都不读（模型里读了就是挂总线） */
    model_reset(&gM);
    gM.gcc_stuck = 1;
    CHECK(run_level(2) == PcamErrGcc && gB.step == -PcamStepCamcc);
    CHECK(model_count(OpQuiet) == 0);
    for (unsigned i = 0; i < gM.nops; ++i) CHECK(gM.ops[i].win != PcamWinCamcc || gM.ops[i].kind == OpSleep || gM.ops[i].kind == OpQuiet);
    /* 全 1 */
    model_reset(&gM);
    gM.camcc_ones = 1;
    CHECK(run_level(2) == PcamErrCamccValue && !gM.quiet);
    /* 硬件控制位 */
    model_reset(&gM);
    gM.gdscr |= PCAM_GDSC_HW_CONTROL;
    CHECK(run_level(2) == PcamErrGdsc && !gM.quiet);
}

static unsigned op_index(OpKind kind, unsigned win, uint32_t off)
{
    for (unsigned i = 0; i < gM.nops; ++i) if (gM.ops[i].kind == kind && gM.ops[i].win == win && gM.ops[i].off == off) return i;
    return ~0u;
}

/* 级 3：投票、GDSC、时钟，CCI 只读版本。 */
static void level_three(void)
{
    model_reset(&gM);
    CHECK(run_level(3) == 0 && gB.step == PcamStepDomain);
    CHECK(gB.cci_version == 0x10060000u);
    CHECK(model_writes(PcamWinCci) == 0);
    CHECK(model_count(OpVrm) == 0);
    CHECK(!gM.quiet);
    const unsigned gdsc = op_index(OpWrite, PcamWinCamcc, PcamCcGdscr), version = op_index(OpRead, PcamWinCci, PcamCciVersion);
    CHECK(gdsc != ~0u && version != ~0u && gdsc < version);
    CHECK(op_index(OpArc, 0, 0) < gdsc);
    CHECK(!(gM.gdscr & PCAM_GDSC_SW_COLLAPSE) && (gM.gdscr & 0x00222000u) == 0x00222000u);     /* 等待字段不动 */
    CHECK(gM.cci0 & PCAM_CBCR_ENABLE);
    /* CFG 已经是 XO：RCG 一个字都不写 */
    CHECK(op_index(OpWrite, PcamWinCamcc, PcamCcCciRcg + PcamRcgCfg) == ~0u);
    CHECK(model_log_has("cci_version=0x10060000"));
    /* RCG 留着别的源：切 XO 并更新 */
    model_reset(&gM);
    gM.cci_cfg = 0x21f;
    CHECK(run_level(3) == 0 && gM.cci_cfg == PCAM_RCG_CFG_XO && !(gM.cci_cmd & PCAM_RCG_UPDATE));
    CHECK(op_index(OpWrite, PcamWinCamcc, PcamCcCciRcg) != ~0u);
    /* RCG 更新不完成：CCI 没读 */
    model_reset(&gM);
    gM.cci_cfg = 0x21f;
    gM.rcg_stuck = 1;
    CHECK(run_level(3) == PcamErrRcg && !gM.quiet);
    CHECK(op_index(OpRead, PcamWinCci, PcamCciVersion) == ~0u);
    /* 投票失败：GDSC 不动 */
    model_reset(&gM);
    gM.arc_fail = 1;
    CHECK(run_level(3) == PcamErrVote);
    CHECK(op_index(OpWrite, PcamWinCamcc, PcamCcGdscr) == ~0u && model_count(OpQuiet) == 2);
    /* GDSC 上不了电：CCI 没读，写者放开（没挂住） */
    model_reset(&gM);
    gM.gdsc_stuck = 1;
    CHECK(run_level(3) == PcamErrGdsc && !gM.quiet);
    CHECK(op_index(OpRead, PcamWinCci, PcamCciVersion) == ~0u);
    /* 步 102 之后才变成硬件控制：不写 GDSC */
    model_reset(&gM);
    gM.gdsc_hw_ctl = 1;
    CHECK(run_level(3) == PcamErrGdsc && !gM.quiet);
    CHECK(op_index(OpWrite, PcamWinCamcc, PcamCcGdscr) == ~0u && gB.refused == 0);    /* 自己看出来的，不是撞到闸门 */
    /* 三个轮询的分支各卡一次 */
    static const uint32_t branches[3] = {PcamCcTopAhb, PcamCcTopFastAhb, PcamCcCci0};
    for (unsigned i = 0; i < 3; ++i) {
        model_reset(&gM);
        gM.branch_stuck = (int)branches[i];
        CHECK(run_level(3) == PcamErrBranch && !gM.quiet);
        CHECK(op_index(OpRead, PcamWinCci, PcamCciVersion) == ~0u);
    }
    /* core_ahb 不轮询，写了没生效：碰 CCI 之前的总检查拦住 */
    model_reset(&gM);
    gM.core_stuck = 1;
    CHECK(run_level(3) == PcamErrBranch && !gM.quiet);
    CHECK(op_index(OpRead, PcamWinCci, PcamCciVersion) == ~0u);
    /* 版本读到全 0、全 1 */
    model_reset(&gM);
    gM.cci_version = 0;
    CHECK(run_level(3) == PcamErrCciVersion);
    model_reset(&gM);
    gM.cci_version = 0xffffffffu;
    CHECK(run_level(3) == PcamErrCciVersion);
    /* GDSC 已经开着：不写 */
    model_reset(&gM);
    gM.gdscr = 0x80222000u;
    gM.arc_mm = 1;
    CHECK(run_level(3) == 0);
    CHECK(op_index(OpWrite, PcamWinCamcc, PcamCcGdscr) == ~0u);
}

static void powered_down(void)
{
    for (unsigned i = 0; i < 3; ++i) CHECK(!gM.rail_on[i]);
    CHECK(!(gM.pin_io[3] & PcamIoOut));
    CHECK(!gM.quiet);
}

/* 级 4：判据那一行，下电后的状态。 */
static void level_four(void)
{
    model_reset(&gM);
    CHECK(run_level(4) == 0 && gB.step == PcamStepOff);
    CHECK(model_log_has("[cam] cci0 m0 id 0x38e5"));
    CHECK(gB.sensor_id == PcamTeleId && gB.sensor_address == kPcamTeleAddresses[0]);
    CHECK(gM.sensor_writes == 1 && gM.sensor_reads == 1 && gM.nacks == 0);
    powered_down();
    CHECK(!(gM.mclk_cbcr & PCAM_CBCR_ENABLE));
    CHECK(gM.pin_ctl[0] == 0x1u && gM.pin_ctl[1] == 0x1u && gM.pin_ctl[2] == 0x1u && gM.pin_ctl[3] == (0x1u | PcamCtlEgpioPresent));
    /* 轨道：电压、顺序、模式；复位放开在三路都起来之后 */
    CHECK(gM.rail_mv[0] == 1800 && gM.rail_mv[1] == 2200 && gM.rail_mv[2] == 1000);
    CHECK(gM.rail_mode[0] == 7 && gM.rail_mode[1] == 7 && gM.rail_mode[2] == 7);
    const char *up[3] = {"ldom5", "ldom4", "ldoi1"};
    unsigned at = 0, last = 0;
    for (unsigned i = 0; i < gM.nops; ++i) {
        const Op *o = &gM.ops[i];
        if (o->kind == OpVrm && o->win == PcamVrmEnable && o->value == 1) {
            CHECK(at < 3 && !strcmp(o->name, up[at]));
            ++at;
            last = i;
        }
    }
    CHECK(at == 3);
    unsigned released = ~0u, mclk = ~0u;
    for (unsigned i = 0; i < gM.nops; ++i) {
        const Op *o = &gM.ops[i];
        if (o->kind == OpWrite && o->win == PcamWinTlmm && o->off == (uint32_t)PcamPinReset * PcamPinStride + 4 && o->value == PcamIoOut) released = i;
        if (o->kind == OpWrite && o->win == PcamWinBist && o->off == PcamBistMclk0Cbcr && (o->value & 1) && mclk == ~0u) mclk = i;
    }
    CHECK(released != ~0u && mclk != ~0u && last < mclk && mclk < released);
    /* 复位放开之后 20 ms 才有第一次 I²C（模型按 msleep 记时） */
    CHECK(gM.ops[released + 1].kind == OpSleep && gM.ops[released + 1].off >= 20);
    /* 下电顺序：反过来 */
    const char *down[3] = {"ldoi1", "ldom4", "ldom5"};
    at = 0;
    for (unsigned i = 0; i < gM.nops; ++i) {
        const Op *o = &gM.ops[i];
        if (o->kind == OpVrm && o->win == PcamVrmEnable && o->value == 0) {
            CHECK(at < 3 && !strcmp(o->name, down[at]));
            ++at;
        }
    }
    CHECK(at == 3);
    /* 时序寄存器是设备树 400 kHz 那一组 */
    CHECK(gM.cci_timing[0] == (0x26u << 16 | 0x38u) && gM.cci_timing[1] == (0x28u << 16 | 0x28u) && gM.cci_timing[2] == (0x16u << 16 | 0x23u));
    CHECK(gM.cci_timing[3] == 0x3e && gM.cci_timing[4] == (6u << 4 | 3u));
    /* 第二个地址才回应：第一个的 NACK 要恢复，I²C 还能用 */
    model_reset(&gM);
    gM.sensor_address = kPcamTeleAddresses[1];
    CHECK(run_level(4) == 0);
    CHECK(gB.sensor_address == kPcamTeleAddresses[1] && gM.nacks == 1 && gM.cci_halts == 1 && gM.cci_resets == 2);
    powered_down();
    /* ID 不对：下电，失败 */
    model_reset(&gM);
    gM.sensor_id = 0x1234;
    CHECK(run_level(4) == PcamErrIdentity && gB.step == -PcamStepId);
    powered_down();
    CHECK(model_log_has("id 0x1234") && !model_log_has("0x38e5 addr"));
    /* 谁都不应答 */
    model_reset(&gM);
    gM.sensor_address = 0x77;
    CHECK(run_level(4) == PcamErrIdentity);
    CHECK(gM.nacks == 2);
    powered_down();
    /* 读回读不到（PM8010 在另一条总线）：继续 */
    model_reset(&gM);
    gM.pmic_unread = 1;
    CHECK(run_level(4) == 0);
    powered_down();
    CHECK(model_log_has("unread"));
    /* 读回的电压不对：MCLK 和复位都不动，下电 */
    for (int rail = 1; rail <= 3; ++rail) {
        model_reset(&gM);
        gM.pmic_wrong_mv = rail;
        CHECK(run_level(4) == PcamErrRail && gB.step == -PcamStepRails);
        powered_down();
        CHECK(gM.mclk_cbcr == 0);
    }
    /* 投票失败在每一条命令上 */
    for (int at_call = 1; at_call <= 9; ++at_call) {
        model_reset(&gM);
        gM.vrm_fail_at = at_call;
        CHECK(run_level(4) == PcamErrRail);
        powered_down();
    }
    /* 读回读不到时，投票失败也要停：不能指望读回兜底 */
    for (int at_call = 1; at_call <= 9; ++at_call) {
        model_reset(&gM);
        gM.vrm_fail_at = at_call;
        gM.pmic_unread = 1;
        CHECK(run_level(4) == PcamErrRail && gB.step == -PcamStepRails);
        powered_down();
    }
    /* 关轨道的命令失败：读回读不到也要报 */
    for (int at_call = 10; at_call <= 12; ++at_call) {
        model_reset(&gM);
        gM.vrm_fail_at = at_call;
        gM.pmic_unread = 1;
        CHECK(run_level(4) == PcamErrRail && gB.step == -PcamStepOff);
    }
    /* 复位脚的输出锁存里留着高：先写低再开输出 */
    model_reset(&gM);
    gM.pin_io[3] = PcamIoOut;
    CHECK(run_level(4) == 0);
    powered_down();
    /* 轨道已经开在别的电压上：不碰它 */
    model_reset(&gM);
    gM.rail_on[1] = 1;
    gM.rail_mv[1] = 2800;
    CHECK(run_level(4) == PcamErrRail);
    CHECK(gM.vrm_calls <= 7);
    /* 脚被别的功能占着：什么都不写 */
    model_reset(&gM);
    gM.pin_ctl[1] = (7u << PcamCtlFunctionShift) | 1u;
    CHECK(run_level(4) == PcamErrPin);
    CHECK(model_writes(PcamWinTlmm) == 0 && model_count(OpVrm) == 0);
    /* 复位脚没有 egpio：不置 enable */
    model_reset(&gM);
    gM.pin_ctl[3] = 1u;
    CHECK(run_level(4) == 0);
    unsigned reset_ctl = 0;
    for (unsigned i = 0; i < gM.nops; ++i)
        if (gM.ops[i].kind == OpWrite && gM.ops[i].win == PcamWinTlmm && gM.ops[i].off == (uint32_t)PcamPinReset * PcamPinStride && !reset_ctl) reset_ctl = gM.ops[i].value;
    CHECK(reset_ctl == PcamCtlResetOut);
    /* 有 egpio：归 apps */
    model_reset(&gM);
    CHECK(run_level(4) == 0);
    reset_ctl = 0;
    for (unsigned i = 0; i < gM.nops; ++i)
        if (gM.ops[i].kind == OpWrite && gM.ops[i].win == PcamWinTlmm && gM.ops[i].off == (uint32_t)PcamPinReset * PcamPinStride && !reset_ctl) reset_ctl = gM.ops[i].value;
    CHECK(reset_ctl == (PcamCtlResetOut | PcamCtlEgpioPresent | PcamCtlEgpioEnable));
    /* MCLK 分支起不来 */
    model_reset(&gM);
    gM.mclk_stuck = 1;
    CHECK(run_level(4) == PcamErrBranch && gB.step == -PcamStepSensor);
    powered_down();
    /* CCI 复位不完成 */
    model_reset(&gM);
    gM.cci_reset_stuck = 1;
    CHECK(run_level(4) == PcamErrCci);
    powered_down();
    /* 下电时一路没关：报告，且不盖掉前面的结果 */
    model_reset(&gM);
    gM.off_stuck = 1;
    CHECK(run_level(4) == PcamErrRail && gB.step == -PcamStepOff);
    model_reset(&gM);
    gM.off_stuck = 1;
    gM.sensor_id = 0x1234;
    CHECK(run_level(4) == PcamErrIdentity && gB.step == -PcamStepId && gB.error == PcamErrIdentity);
}

/* 闸门：整张白名单之外什么都不放。 */
static void gate(void)
{
    for (unsigned i = 0; i < PCAM_RAIL_COUNT; ++i) {
        CHECK(pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmVoltage, kPcamRails[i].millivolts));
        CHECK(pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmEnable, 0) && pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmEnable, 1));
        CHECK(!pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmEnable, 2));
        CHECK(pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmMode, PcamVrmModeHigh));
        CHECK(!pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmMode, 4));
        CHECK(!pcam_vrm_allowed(kPcamRails[i].resource, 12, 1));
        for (uint32_t mv = 0; mv < 8192; ++mv) if (mv != kPcamRails[i].millivolts) CHECK(!pcam_vrm_allowed(kPcamRails[i].resource, PcamVrmVoltage, mv));
    }
    static const char *const others[] = {"ldob14", "ldom3", "ldom1", "ldom2", "ldom7", "ldof1", "ldog3", "smpi7", "ldoi", "", "ldom55"};
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; ++i) {
        CHECK(!pcam_vrm_allowed(others[i], PcamVrmEnable, 1));
        CHECK(!pcam_vrm_allowed(others[i], PcamVrmVoltage, 1800));
    }
    /* 设备树的数：cam-sensor2 的 rgltr 表，L4M 2.2 V、L1I 1.0 V、L5M 1.8 V */
    CHECK(!strcmp(kPcamRails[0].resource, "ldom5") && kPcamRails[0].millivolts == 1800 && kPcamRails[0].ppid == 0xc44);
    CHECK(!strcmp(kPcamRails[1].resource, "ldom4") && kPcamRails[1].millivolts == 2200 && kPcamRails[1].ppid == 0xc43);
    CHECK(!strcmp(kPcamRails[2].resource, "ldoi1") && kPcamRails[2].millivolts == 1000 && kPcamRails[2].ppid == 0x8c1);
    /* GCC */
    CHECK(pcam_write_allowed(PcamWinGcc, PcamGccCameraAhb, 1) && !pcam_write_allowed(PcamWinGcc, PcamGccCameraAhb, 0));
    CHECK(!pcam_write_allowed(PcamWinGcc, 0x26014, 1));     /* AXI 这一阶段不开 */
    CHECK(!pcam_write_allowed(PcamWinGcc, 0x27004, 1));
    for (uint32_t off = 0; off < PCAM_GCC_BYTES; off += 4) {
        if (off == PcamGccBistAhb || off == PcamGccCameraAhb || off == PcamGccCameraXo) continue;
        CHECK(!pcam_write_allowed(PcamWinGcc, off, 0xffffffffu));
    }
    /* GDSC 只开不关 */
    CHECK(pcam_write_allowed(PcamWinCamcc, PcamCcGdscr, 0x222000));
    CHECK(!pcam_write_allowed(PcamWinCamcc, PcamCcGdscr, 0x222001));
    CHECK(!pcam_write_allowed(PcamWinCamcc, PcamCcGdscr, 0x222002));
    /* CAMCC 其他一概不放，包括 TFE 和 PLL */
    unsigned allowed = 0;
    for (uint32_t off = 0; off < PCAM_CAMCC_BYTES; off += 4) if (pcam_write_allowed(PcamWinCamcc, off, 0xffffffffu)) ++allowed;
    CHECK(allowed == 11);       /* 4 个常开、4 个分支、3 个 RCG 的 CMD；GDSCR 和 RCG 的 CFG 不收全 1 */
    /* RCG 只许 XO */
    CHECK(pcam_write_allowed(PcamWinCamcc, PcamCcCciRcg + PcamRcgCfg, PCAM_RCG_CFG_XO));
    CHECK(!pcam_write_allowed(PcamWinCamcc, PcamCcCciRcg + PcamRcgCfg, 0x21f));
    CHECK(!pcam_write_allowed(PcamWinBist, PcamBistMclk0Rcg + PcamRcgCfg, 0x301));
    /* 只有 MCLK0 */
    CHECK(!pcam_write_allowed(PcamWinBist, 0x4034, 1));
    CHECK(!pcam_write_allowed(PcamWinBist, 0x401c, 1));
    /* TLMM：只有四个脚，功能只有目标或 GPIO，电平只有复位脚 */
    for (uint32_t pin = 0; pin < PCAM_TLMM_BYTES / PcamPinStride; ++pin) {
        const int ours = pin == PcamPinMclk0 || pin == PcamPinSda0 || pin == PcamPinScl0 || pin == PcamPinReset;
        CHECK(pcam_write_allowed(PcamWinTlmm, pin * PcamPinStride, 0) == ours);
        if (pin != PcamPinReset) CHECK(!pcam_write_allowed(PcamWinTlmm, pin * PcamPinStride + 4, PcamIoOut));
    }
    CHECK(pcam_write_allowed(PcamWinTlmm, PcamPinReset * PcamPinStride + 4, PcamIoOut));
    CHECK(!pcam_write_allowed(PcamWinTlmm, PcamPinReset * PcamPinStride + 4, 1));
    CHECK(!pcam_write_allowed(PcamWinTlmm, PcamPinReset * PcamPinStride, PcamCtlMclk));
    CHECK(!pcam_write_allowed(PcamWinTlmm, PcamPinSda0 * PcamPinStride, 2u << PcamCtlFunctionShift));
    /* CCI：只有 master 0 queue 0 */
    CHECK(pcam_write_allowed(PcamWinCci, PcamCciQueueStart, 1) && !pcam_write_allowed(PcamWinCci, PcamCciQueueStart, 2) && !pcam_write_allowed(PcamWinCci, PcamCciQueueStart, 4));
    CHECK(!pcam_write_allowed(PcamWinCci, 0x200, 1));       /* master 1 的时序 */
    CHECK(!pcam_write_allowed(PcamWinCci, 0x410, 1));       /* queue 1 的 LOAD_DATA 在 0x410 */
    CHECK(!pcam_write_allowed(PcamWinCci, 0x510, 1));       /* master 1 */
    CHECK(!pcam_write_allowed(PcamWinCount, 0, 1));
}

int main(void)
{
    levels();
    level_one();
    level_two();
    level_three();
    level_four();
    gate();
    printf("PASS bringup: %u checks\n", checks);
    return 0;
}
