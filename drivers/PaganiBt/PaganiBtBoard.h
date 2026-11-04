/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 一加 13T（PKX110，SM8750）蓝牙的硬件事实。这里只有数字和它们的来源，不含任何 GPL 或厂商代码。来源标记：
 *   [DT]    本机运行设备树 build-userspace/a830-inputs/dt/running.dts（bt_peach 第 42477 行，qup_uart@898000 第 25429 行，
 *           qupv3_se14_* 第 2247 行，bt_uwb_en 第 11597 行）
 *   [UEFI]  pagani 的 UEFI ClockDxe.efi（UEFI-Pagani_SM8750/Binaries/pagani）里的时钟表：描述符 CBCR、投票寄存器、掩码，
 *           时钟域的 RCG 地址和 16 项频率表（源、div2x、M、N），基址偏移 0x10000
 *   [HAL]   本机 bt-impl-qti.so 的反编译（PeriPatchDLManager、PatchDLManager、HciUartTransport）
 *   [TRACE] Android hsuart 跟踪（ser_clk_cfg 33/769/65/17，geni_ios 0x1）
 *   [U]     主线 Linux 事实：gcc-sm8750.c、pinctrl-sm8750.c、clk-rpmh.c、qcom_geni_serial.c、pinctrl-spmi-gpio.c
 *   [WIFI]  PaganiWifiTables.h（同一组轨道；Test/tables.cpp 比对两边一致）
 * 没上过机的数字都还是推断，README.md 的轮次清单写了每一轮验证哪些。 */
#ifndef PAGANI_BT_BOARD_H
#define PAGANI_BT_BOARD_H
#include <stdint.h>

/* 窗口。TLMM 只映射到 gpio27 的引脚页：蓝牙只碰 24..27，读 16、18。 */
#define PB_GCC_BASE 0x00100000ull
#define PB_GCC_BYTES 0x80000u
#define PB_TLMM_BASE 0x0f100000ull         /* [WIFI] 引脚 n 的 CTL 在 + 0x1000 * n，IO 在其后 +4 */
#define PB_TLMM_BYTES 0x1c000u
#define PB_SE_BASE 0x00898000ull           /* [DT] QUP2 SE6，hsuart0，/dev/ttyHS0 */
#define PB_SE_BYTES 0x4000u
#define PB_RSC_DRV2 0x16520000ull
#define PB_RSC_BYTES 0x10000u
#define PB_CMD_DB 0x81c60000ull
#define PB_CMD_DB_BYTES 0x20000u

enum PbWin {PbWinGcc, PbWinTlmm, PbWinSe, PbWinCount};

/* GCC：[UEFI] 与 [U] 一致。这个投票寄存器目前没有别的 XNU 驱动在用（0x52008 才是共享的那个）。 */
enum {
    PbGccVote = 0x52010,
    PbVoteCore = 1u << 0, PbVoteSAhb = 1u << 1, PbVoteMAhb = 1u << 2, PbVoteCore2x = 1u << 3, PbVoteSe6 = 1u << 10,
    PbVoteBits = PbVoteCore | PbVoteSAhb | PbVoteMAhb | PbVoteCore2x | PbVoteSe6,
    PbHaltMAhb = 0x23298, PbHaltSAhb = 0x2329c, PbHaltCore = 0x232a0, PbHaltCore2x = 0x232b4, PbHaltSe6 = 0x1e76c,
    PbRcg = 0x1e77c,                        /* CMD +0，CFG +4，M +8，N +0xc，D +0x10 */
    PbRcgCfg = 4, PbRcgM = 8, PbRcgN = 0xc, PbRcgD = 0x10,
    PbRcgDfs = 0x14,                        /* bit 0：动态频率表开着，速率由 SE 的 CLK_SEL 选 */
    PbRcgPerf = 0x1c, PbRcgPerfM = 0x5c, PbRcgPerfN = 0x9c,   /* 每级 4 字节，共 8 级 */
    PbRcgLevels = 8,
    PbRcgUpdate = 1u << 0,
    PbGpll0Mode = 0,                        /* 只读进日志：bit 31 锁定 */
};
#define PB_CBCR_OFF 0x80000000u             /* 分支时钟读到 CLK_OFF */
#define PB_CBCR_HW_CTL 0x2u                 /* 分支在硬件门控下，空闲时可以读到 CLK_OFF（与 sm8750_touch_clock 同一判据） */
#define PB_RCG_ROOT_OFF 0x80000000u
#define PB_GCC_OWNER 0x42544c45u            /* 共享锁字里的标记 'BTLE' */
#define PB_QUIET_BIT 4u                     /* pagani_panel_probe_quiet 里蓝牙的一位：bit 0 是 PaganiPanel，bit 1 是 PaganiBattery 的 USB 探针 */

/* TLMM：[DT] 活动态，四个脚都是功能 1（[U] pinctrl-sm8750.c），2 mA。 */
enum {
    PbPinCts = 24, PbPinRts = 25, PbPinTx = 26, PbPinRx = 27,
    PbPinWlanEn = 16, PbPinSwCtrl = 18,     /* 只读进日志 */
    PbPinStride = 0x1000,
    PbPinFunction = 1, PbPinDriveMa = 2,
};
typedef struct {uint16_t gpio; int8_t pull; const char *name;} PbUartPin;   /* pull：-1 下拉，0 无，1 上拉 */
static const PbUartPin kPbUartPins[4] = {
    {PbPinCts, 0, "cts"}, {PbPinRts, -1, "rts"}, {PbPinTx, 1, "tx"}, {PbPinRx, 0, "rx"},
};

/* RPMh VRM 轨道：[WIFI] 七路，蓝牙用的是它的子集；L8B 是天线共享轨，蓝牙节点没有列，蓝牙是否必须未知，按报告一起投。order 是
 * cnss2 的上电顺序，chosenMv 是 cnss 的最低值，蓝牙设备树的范围都能容下。VRM 寄存器 +0 电压 mV，+4 使能，+8 模式（7 高功率）。 */
typedef struct {const char *resource; uint16_t ppid, chosenMv, maxMv; uint8_t order;} PbRail;
static const PbRail kPbRails[7] = {
    {"ldof3", 0x5c3, 1800, 1800, 1},
    {"ldof2", 0x5c2, 1200, 1200, 2},
    {"ldob8", 0x1c8, 2800, 2800, 3},
    {"smpd4", 0x3a4, 876, 1036, 4},
    {"smpf5", 0x5a7, 876, 1000, 5},
    {"smpg3", 0x6a1, 1860, 2000, 6},
    {"smpi7", 0x8ad, 1312, 1340, 7},      /* S7I：同时是 UFS vccq 的上级 */
};
#define PB_RAIL_COUNT 7u
#define PB_RAIL_S7I "smpi7"
enum {PbVrmVoltage = 0, PbVrmEnable = 4, PbVrmMode = 8, PbVrmModeHigh = 7};
#define PB_RF_CLOCK "rfclka1"               /* [DT] bt_rf_clk1 = RPMH_RF_CLK1；[U] clk-rpmh.c：VRM 资源，+4 写 1 */
enum {PbRfClockEnable = 4};

/* PMIC：[DT] qcom,bt-reset-gpio = PM8550VE-F gpio3（PPID 0x58a），normal、推挽输出、无上下拉、power-source 1。
 * 上电时序 [TRACE/dmesg]：低 54 ms，高，再等 54 ms。 */
#define PB_BT_EN_PPID 0x58au
enum {PbBtEnLowMs = 54, PbBtEnHighMs = 54, PbBtEnVin = 1};

/* 芯片身份 [HAL/TRACE]。 */
#define PB_SOC_VERSION 0x40210200u
#define PB_PRODUCT_ID 0x21u
#define PB_BUILD_LABEL "BTFW.BRAHMA"

/* 串口速率表。rcgHz = 波特率 × 16（过采样，[U] IP ≥ 2.5）× seDiv；[UEFI] 频率表里的源、M、N；[TRACE] ser_clk_cfg 给出 seDiv；
 * code 是发给控制器和写进 NVM Tag 17 的波特率码，[HAL] HciUartTransport::SetBaudRate。 */
typedef struct {uint32_t baud, rcgHz; uint8_t source, div2x; uint16_t m, n; uint8_t seDiv, code;} PbRate;
static const PbRate kPbRates[4] = {
    {115200, 7372800, 6, 2, 384, 15625, 4, 0x00},    /* 7.3728 MHz ÷ 4 ÷ 16 */
    {3000000, 48000000, 6, 2, 4, 25, 1, 0x0e},
    {3200000, 51200000, 6, 2, 64, 375, 1, 0x11},
    {8000000, 128000000, 1, 2, 16, 75, 1, 0x15},     /* 128 MHz ÷ 1 ÷ 16 */
};
#define PB_RATE_COUNT 4u

/* 固件文件 [实测：跟踪字节比对]。分区用现成的 FAT 读取器原地读，不进镜像。 */
#define PB_FW_DIR "image/"
#define PB_FW_PERI "brhperifw20.mbn"
#define PB_FW_PERI_NVM "brhperinv20.bin"
#define PB_FW_TMEL "tmel_peach_20.elf"
#define PB_FW_BT "brhbtfw20.mbn"
#define PB_FW_BT_NVM "brhbtnv20.bin"

/* 阶段 4 面向 iOS 的名字 [bluetoothd]：控制名是 sub_1000DDAA8 写死的默认值，沙盒配置 BTServer 放行的也是它；服务名和属性名见 start、sub_10015FD98、
 * sub_10015FC40；两个键见 MobileGestalt（inventory/ios.md 第 3 节，/product/bluetooth-le 有属性就算真）。transport-encoding 1 是 H4 走 POSIX 驱动。 */
#define PB_CONTROL_NAME "com.apple.uart.sk.bluetooth"
#define PB_SERVICE_NAME "bluetooth"
#define PB_KEY_ADDRESS "mac-address-bluetooth0"
#define PB_KEY_LE "bluetooth-le"
enum {PbTransportH4 = 1, PbControlSendBytes = 8192, PbControlRecvBytes = 65536};

#endif
