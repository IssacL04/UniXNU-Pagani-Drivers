/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 相机阶段 2：长焦 S5KJN5 上电，经 CCI 读 ID。写成寄存器回调上的可移植代码，结构照 PaganiBt BtBringup，顺序和数值由主机模型检查（Test/bringup.c）。
 * 模型的第一条规矩是这个驱动的第一条规矩：没开时钟的块一读就挂总线。pagani-cam=N 决定走到哪一级：
 *
 *   级 1  步 101 勘察：GCC 相机分支、四个脚、三路轨道。只读，只碰 GCC、TLMM、SPMI observer，一个寄存器都不写。
 *   级 2  步 102 开 GCC 的相机 AHB 和 XO 分支（Android 的 probe 无条件开的那三个），分支都起来才碰 CAMCC 和 cambistmclkcc，只读：第一次读可能挂住总线，
 *         上一行先落到 UFS，再持住写者。读到全 1 或硬件控制位就停。
 *   级 3  步 201 投 mmcx、mxc 到 0x180（TURBO，Android 初始化投的值，绝不投 mx），持住写者：CAMCC 常开的四个分支、titan GDSC、AHB 和 CCI 的 RCG 切 XO、分支；
 *         逐个确认 GDSC 和时钟状态之后，才读 CCI_HW_VERSION。
 *   级 4  先等上一行落盘、持住写者，到步 304 走完才放开。步 301 复位脚拉低，三路轨道走闸门 pcam_vrm_allowed（只放设备树的那三个电压），投后读回，读到别的值就下电。
 *         步 302 MCLK0 19.2 MHz（XO 直出）、CCI 脚、复位放开。
 *         步 303 CCI 复位配置，W 0xfcfc=0x4000，R 0x0000，等于 0x38e5 才算：一行 "[cam] cci0 m0 id 0x38e5"。
 *         步 304 不论成败都走：复位拉低、关 MCLK、脚恢复原样、三路轨道关。GDSC 和 AHB 分支留着：这是测试级，不值得为省电去赌第一次 collapse。
 *
 * 不做：传感器初始化序列、CSIPHY、CSID、IFE、SMMU、CAMNOC QoS、BCM 投票、PLL。CCI 跑 XO，总线约 204 kHz。
 * 每步有步号，失败时 b->step 是负的步号，b->error 是错误码，并写一行日志。这里不映射内存，只经 PcamHw 回调访问；所有写都过 pcam_write_allowed。 */
#ifndef PAGANI_CAM_BRINGUP_H
#define PAGANI_CAM_BRINGUP_H
#include <stdint.h>
#include "CamCci.h"
#include "PaganiCameraBoard.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 每个窗口映射的字节数，内核据此映射，模型据此检查每次访问。 */
static const uint32_t kPcamWinBytes[PcamWinCount] = {PCAM_GCC_BYTES, PCAM_TLMM_BYTES, PCAM_CAMCC_BYTES, PCAM_BIST_BYTES, PCAM_CCI_BYTES};
static const uint64_t kPcamWinBase[PcamWinCount] = {PCAM_GCC_BASE, PCAM_TLMM_BASE, PCAM_CAMCC_BASE, PCAM_BIST_BASE, PCAM_CCI0_BASE};

enum PcamStep {PcamStepSurvey = 101, PcamStepCamcc = 102, PcamStepDomain = 201, PcamStepRails = 301, PcamStepSensor = 302, PcamStepId = 303, PcamStepOff = 304};
enum {PcamLevelMax = 4};
enum PcamError {
    PcamErrNone = 0, PcamErrArgument = -22,
    PcamErrGcc = 1,             /* GCC 分支没起来：CAMCC 没读 */
    PcamErrCamccValue,          /* CAMCC 读到不可能的值 */
    PcamErrVote,                /* ARC 投票失败 */
    PcamErrGdsc,                /* GDSC 不是预期状态或没上电 */
    PcamErrRcg,                 /* RCG 更新没完成 */
    PcamErrBranch,              /* 分支没起来：CCI 没读 */
    PcamErrCciVersion,          /* 版本读到全 0 或全 1 */
    PcamErrPin,                 /* 脚不是预期功能 */
    PcamErrRail,                /* 轨道投票失败或读回不对 */
    PcamErrCci,                 /* CCI 初始化失败 */
    PcamErrIdentity,            /* 没有地址回 0x38e5 */
};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned win, uint32_t offset);
    void (*write)(void *ctx, unsigned win, uint32_t offset, uint32_t value);   /* 带屏障 */
    void (*udelay)(void *ctx, unsigned us);
    void (*msleep)(void *ctx, unsigned ms);
    /* ARC：cmd-db 资源名、要的 vlvl，返回实际投的 hlvl。0 或负数。 */
    int (*arc)(void *ctx, const char *resource, uint32_t vlvl, uint32_t *hlvl);
    /* 一条 RPMh VRM 命令。0 或负数；*address 给出资源地址。 */
    int (*vrm)(void *ctx, const char *resource, uint32_t reg, uint32_t value, uint32_t *address);
    /* SPMI observer 读稳压器，绝不写。负数：读不到。 */
    int (*pmic)(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);
    void (*log)(void *ctx, const char *line);
    /* 持住 UFS 日志的写者（1）或放开（0）。 */
    void (*quiet)(void *ctx, unsigned on);
} PcamHw;

typedef struct {uint32_t address; int rc; uint8_t status, enable, mode, read, voted; uint16_t millivolts;} PcamRailResult;

enum {PcamPins = 4};
typedef struct {
    const PcamHw *hw;
    unsigned level;
    int step, error;
    uint32_t gcc[3];
    uint32_t pin_ctl[PcamPins], pin_io[PcamPins];       /* 动之前的值，步 304 写回 */
    uint32_t gdscr, cfg_gdscr, cci_cbcr, cci_rcg, mclk_cbcr, mclk_rcg;
    uint32_t cci_version;
    uint32_t refused;           /* 闸门拒绝的写 */
    uint32_t mm_hlvl, mxc_hlvl;
    PcamRailResult rails[PCAM_RAIL_COUNT];
    uint16_t sensor_id;
    uint8_t sensor_address;
    uint8_t rails_touched, pins_touched, mclk_on, held;
    PcamCciIo cci;
} PcamBringup;

/* pagani-cam 的值：1..4 是级，其他是 0（关）。 */
unsigned pcam_level(unsigned value);
/* 跑到 b->level 这一级，失败时停下并下电。返回 0 或 PcamError。hw 在 b 里。 */
int pcam_run(PcamBringup *b);
/* 闸门。 */
int pcam_write_allowed(unsigned win, uint32_t offset, uint32_t value);
int pcam_vrm_allowed(const char *resource, uint32_t reg, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
