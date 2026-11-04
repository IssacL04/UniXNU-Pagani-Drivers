/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 上电序列，写成寄存器回调上的可移植代码，顺序和数值由主机模型检查（Test/bringup.c），结构照 PaganiWifi WifiBringup：
 *
 *   101 勘察：GCC 投票、分支、RCG、GPLL0，四个串口脚和 WLAN_EN、SW_CTRL，七路轨道，睡眠时钟，BT_EN。只读。
 *   102 投 0x52010 开 QUP2 外壳和 SE6 的时钟，分支都起来才碰 SE，读 GENI_FW_REVISION，协议必须是 UART，FIFO 必须可用。第一次读 SE 可能挂住
 *       总线：上一行日志先落到 UFS，再持住日志的写者，读的时候线上没有 UFS 写（PanelProbe.h 的规则，UfsLog.h 说了原因）。
 *   201 睡眠时钟引脚、七路 RPMh 轨道、rfclka1。S7I 同时带 UFS，规则照 Wi-Fi 且更严：
 *       读不到不投（任何一路都不投）；电压只升不降；总投高功率模式；绝不 enable=0；投后读回，读不到、掉了、低于所需都停在这里。
 *   202 BT_EN 低 ≥ 54 ms，再高，等 54 ms。任何异常都停在这一步之前。
 *   301 RCG 7.3728 MHz，SE 配 115200 8N1，引脚还是 GPIO 时跑一次内部回环（只记录），TLMM 切功能 1，开硬件流控和接收，读 CTS。
 *   302、303 由 BtDownload.c 完成，判据是收到 0xfc 和 0x34 版本应答。
 *
 * 每步有步号，失败时 b->step 是负的步号，b->error 是错误码，并写一行日志。这里不映射内存，只经 PbHw 回调访问。 */
#ifndef PAGANI_BT_BRINGUP_H
#define PAGANI_BT_BRINGUP_H
#include <stdint.h>
#include "BtDownload.h"
#include "BtGeni.h"
#include "BtPmic.h"
#include "PaganiBtBoard.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 每个窗口映射的字节数，内核据此映射，模型据此检查每次访问。 */
static const uint32_t kPbWinBytes[PbWinCount] = {PB_GCC_BYTES, PB_TLMM_BYTES, PB_SE_BYTES};
static const uint64_t kPbWinBase[PbWinCount] = {PB_GCC_BASE, PB_TLMM_BASE, PB_SE_BASE};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned win, uint32_t offset);
    void (*write)(void *ctx, unsigned win, uint32_t offset, uint32_t value);   /* 带屏障 */
    void (*udelay)(void *ctx, unsigned us);
    void (*msleep)(void *ctx, unsigned ms);
    uint64_t (*now_ms)(void *ctx);
    /* 共享锁字 sm8750_gcc_vote_owner，关中断持有。0 拿到，负数没拿到。 */
    int (*vote_lock)(void *ctx, int take);
    /* 一条 RPMh VRM 命令：cmd-db 资源名、寄存器、值。0 或负数；*address 给出资源地址。 */
    int (*vrm)(void *ctx, const char *resource, uint32_t reg, uint32_t value, uint32_t *address);
    /* SPMI observer 读稳压器，绝不写。 */
    int (*pmic)(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);
    /* 睡眠时钟引脚，write 0 只读。见 pb_pmic_sleep_clock。 */
    int (*sleep_clock)(void *ctx, int write);
    /* BT_EN，level < 0 只读。见 pb_pmic_bt_en。 */
    int (*bt_en)(void *ctx, int level, PbGpioState *state);
    void (*log)(void *ctx, const char *line);
    /* 持住 UFS 日志的写者（1）或放开（0），可以没有。 */
    void (*quiet)(void *ctx, unsigned on);
} PbHw;

typedef struct {uint32_t address; int rc; uint8_t status, enable, mode, read, skipped; uint16_t millivolts, voted;} PbRailResult;

typedef struct {
    const PbHw *hw;
    int stop;                       /* 走完这一步就停（0：不停） */
    int step, error;
    uint32_t vote, gpll0, rcg_cmd, rcg_cfg, rcg_dfs, halts;   /* halts：bit i 对应 kPbBranches[i] 读到 CLK_OFF */
    uint32_t pin_ctl[6], pin_io[6];
    PbRailResult rails[PB_RAIL_COUNT];
    int sleep_clock;
    uint32_t rf_clock;              /* rfclka1 的 RPMh 地址 */
    PbGeni geni;
    PbGeniInfo info;
    uint32_t level;                 /* CLK_SEL */
    uint32_t baud, ios, loopback;   /* loopback：1 通过，0 没通过（只记录） */
    uint8_t held;                   /* 蓝牙这一位持着 UFS 日志的写者 */
} PbBringup;

int pb_survey(PbBringup *b);        /* 步 101 */
int pb_clocks(PbBringup *b);        /* 步 102 */
int pb_power(PbBringup *b);         /* 步 201 */
int pb_bt_enable(PbBringup *b);     /* 步 202 */
int pb_uart(PbBringup *b);          /* 步 301 */
/* 切到 kPbRates 里的波特率：停接收、改 RCG 和分频、开接收。阶段 2 换速时用。 */
int pb_uart_baud(PbBringup *b, uint32_t baud);
/* TLMM 控制字：pull（-1 下拉，0 无，1 上拉）、功能、驱动 mA、输出使能。 */
uint32_t pb_pin_config(int pull, unsigned function, unsigned drive_ma, int output);
/* RCG 的 M/N/D/CFG。 */
void pb_rcg_values(const PbRate *rate, uint32_t *m, uint32_t *n, uint32_t *d, uint32_t *cfg);

#ifdef __cplusplus
}
#endif
#endif
