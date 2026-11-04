/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 一加 13T（PKX110，SM8750）相机阶段 2 的硬件事实。这里只有数字和它们的来源，不含任何 GPL 或厂商代码。来源标记：
 *   [DT]    本机运行设备树 build-userspace/a830-inputs/dt/running.dts（cam_cci0 第 26937 行，cam-sensor2 第 27180 行，camcc 第 9483 行，
 *           cambistmclkcc 第 9465 行，titan GDSC 第 9694 行，CPAS 第 27643 行，引脚第 5719 / 6083 / 6323 行，轨道第 8131 / 8575 / 8884 / 8898 行，
 *           PPID 第 33331 行起 regulator-ocp-notifier 的 periph-*-supply）
 *   [KO]    原厂 vendor_boot_a.img 里 camcc-sun.ko、cambistmclkcc-sun.ko、gcc-sun.ko、pinctrl-sun.ko 的 .data 和 probe（clk_branch、clk_rcg2、parent_map、
 *           freq_tbl、msm_pingroup 的字段值），只取数字
 *   [U]     主线 Linux 的事实：i2c-qcom-cci.c（CCI 寄存器和队列命令）、gdsc.c（GDSCR 位）、clk-rcg2.c、clk-branch.c、pinctrl-msm.c（TLMM 位、egpio）。
 *           这台机器上没有这些源码，不下载，CCI 的偏移和命令字是按公开驱动的记忆写的，只和 camera.ko 的字符串、下游 gdsc-regulator.ko 的位对过，没有逐行核对：
 *           读到意外值就停，错了停在级 4
 *   [BT]    PaganiBtBoard.h（TLMM 窗口、PPID 规则、VRM 寄存器）
 *   [推断]  写明了依据，上机才算数
 * 没上过机的数字都还是推断。README.md 列了每一条要在哪一级确认。 */
#ifndef PAGANI_CAM_BOARD_H
#define PAGANI_CAM_BOARD_H
#include <stdint.h>

/* 窗口。TLMM 映射到 gpio178 的引脚页，只访问白名单里的四个脚：保留脚 36..39、74 在窗口里但绝不碰。 */
#define PCAM_GCC_BASE 0x00100000ull
#define PCAM_GCC_BYTES 0xa1000u
#define PCAM_TLMM_BASE 0x0f100000ull        /* [BT] 引脚 n 的 CTL 在 + 0x1000 × n，IO 在其后 +4；[KO] ctl_reg = 0x100000 + 0x1000 × n，基址 0xf000000 */
#define PCAM_TLMM_BYTES 0xb3000u
#define PCAM_CAMCC_BASE 0x0ade0000ull       /* [DT] */
#define PCAM_CAMCC_BYTES 0x20000u
#define PCAM_BIST_BASE 0x01760000ull        /* [DT] cambistmclkcc */
#define PCAM_BIST_BYTES 0x6000u
#define PCAM_CCI0_BASE 0x0ac7b000ull        /* [DT] cam_cci0 */
#define PCAM_CCI_BYTES 0x1000u
#define PCAM_RSC_DRV2 0x16520000ull
#define PCAM_RSC_BYTES 0x10000u
#define PCAM_CMD_DB 0x81c60000ull
#define PCAM_CMD_DB_BYTES 0x20000u

enum PcamWin {PcamWinGcc, PcamWinTlmm, PcamWinCamcc, PcamWinBist, PcamWinCci, PcamWinCount};

/* CBCR 和 RCG 的位 [U]。 */
#define PCAM_CBCR_ENABLE 0x1u
#define PCAM_CBCR_HW_CTL 0x2u
#define PCAM_CBCR_OFF 0x80000000u
#define PCAM_RCG_UPDATE 0x1u
#define PCAM_RCG_ROOT_OFF 0x80000000u
#define PCAM_RCG_SRC_MASK 0x700u            /* CFG [10:8] */
#define PCAM_RCG_DIV_MASK 0x1fu             /* CFG [4:0] */
#define PCAM_RCG_MODE_MASK 0x3000u
#define PCAM_RCG_CFG_XO 0x1u                /* 源 0 是 XO，分频字段 1 是不分频，模式 0：[KO] parent_map 的 {XO, 0} 和 ftbl_cam_cc_xo_clk_src 的 pre_div 1 */

/* GCC：[KO] gcc-sun.ko 的 probe 无条件置位的 CBCR。这三个在它的时钟表里没有条目（设备树的 clocks 下标 5、6 落到空项），Android 一直开着它们；XNU 没跑
 * 那段 probe，所以要自己看。名字：0x26004 和 0x26034 由主线和 CPAS 的 gcc_ahb_clk 对上；0xa0004 是这组里第一个，按下游 probe 的注释顺序是
 * cam_bist_mclk_ahb [推断]，无论叫什么，Android 也是无条件写它的 bit 0。 */
enum {
    PcamGccBistAhb = 0xa0004,
    PcamGccCameraAhb = 0x26004,
    PcamGccCameraXo = 0x26034,
};

/* CAMCC：[KO]。相对 0xade0000。 */
enum {
    PcamCcGdscr = 0x1134c,                  /* [DT] 0xadf134c，cam_cc_titan_top_gdsc */
    PcamCcCfgGdscr = 0x11350,               /* GDSCR + 4，[DT] qcom,support-cfg-gdscr */
    PcamCcGdscClk = 0x1137c,                /* cam_cc 的 probe 无条件置位的四个，按主线是 GDSC、SLEEP、DRV_XO、DRV_AHB */
    PcamCcSleepClk = 0x11398,
    PcamCcDrvXo = 0x113c0,
    PcamCcDrvAhb = 0x113c4,
    PcamCcSlowAhbRcg = 0x10100,             /* cam_cc_slow_ahb_clk_src：core_ahb、cam_top_ahb 的父 */
    PcamCcFastAhbRcg = 0x100dc,             /* cam_cc_fast_ahb_clk_src：cam_top_fast_ahb 的父 */
    PcamCcCciRcg = 0x1126c,                 /* cam_cc_cci_0_clk_src，安卓是 PLL0 分到 37.5 MHz；这一阶段不开 PLL，用 XO */
    PcamCcCoreAhb = 0x11360,                /* halt_check 不是轮询型 [KO]：开了等一下，不看 CLK_OFF */
    PcamCcTopAhb = 0x113ac,
    PcamCcTopFastAhb = 0x1139c,
    PcamCcCci0 = 0x11284,
    PcamRcgCfg = 4,
};

/* GDSC 的位 [U]；qcom,support-cfg-gdscr 时状态也在 CFG_GDSCR。 */
#define PCAM_GDSC_SW_COLLAPSE 0x1u
#define PCAM_GDSC_HW_CONTROL 0x2u
#define PCAM_GDSC_PWR_ON 0x80000000u
#define PCAM_GDSC_CFG_UP_DONE 0x10000u

/* MCLK：[KO] cambistmclkcc-sun.ko。相对 0x1760000。设备树要 19.2 MHz（clock-rates 0x124f800），频率表第一项就是 XO 直出，不开 PLL。 */
enum {
    PcamBistMclk0Rcg = 0x4000,
    PcamBistMclk0Cbcr = 0x4018,
};

/* 引脚 [DT] [KO]。功能号是 msm_pingroup 里的序号。TLMM 位 [U]：[1:0] 上下拉（0 无、1 下拉、3 上拉），[5:2] 功能，[8:6] 驱动（0 是 2 mA），[9] 输出使能；
 * [KO] egpio_present = 11，egpio_enable = 12：gpio178 是 egpio 脚，归 apps 时 enable 置 1（设备树 qcom,apps）。IO 寄存器 bit 0 输入、bit 1 输出。 */
enum {
    PcamPinMclk0 = 89, PcamPinSda0 = 113, PcamPinScl0 = 114, PcamPinReset = 178,
    PcamPinStride = 0x1000,
    PcamCtlMclk = 1u << 2,                  /* 功能 1 cam_mclk，无上下拉，2 mA */
    PcamCtlCci = (1u << 2) | 3u,            /* 功能 1 cci_i2c，上拉，2 mA */
    PcamCtlResetOut = 1u << 9,              /* 功能 0 gpio，无上下拉，2 mA，输出 */
    PcamCtlEgpioPresent = 1u << 11, PcamCtlEgpioEnable = 1u << 12,
    PcamCtlMask = 0x3ffu,                   /* 上面这些位 */
    PcamCtlFunctionShift = 2, PcamCtlFunctionMask = 0xfu,
    PcamIoOut = 2,
};

/* 轨道 [DT]：cam-sensor2 的 rgltr-min-voltage 和 rgltr-max-voltage 相等，白名单就是这个值。闸门在 CamBringup.c 的 pcam_vrm_allowed：只放这三个资源的这三个电压。
 * 不放 cam_vaf（L14B）：读 ID 用不到音圈，而且设备树里它还挂着一个触摸节点，不是相机独占。order 是上电顺序：RESET 全程拉低，顺序不是判据，阶段 0 的真实时序回来再核。
 * PPID 用来经 observer 读回，[DT] periph-c44 / c43 / 8c1。 */
typedef struct {const char *resource; uint16_t ppid, millivolts; uint8_t order; const char *name;} PcamRail;
static const PcamRail kPcamRails[3] = {
    {"ldom5", 0xc44, 1800, 1, "vio"},
    {"ldom4", 0xc43, 2200, 2, "vana"},
    {"ldoi1", 0x8c1, 1000, 3, "vdig"},
};
#define PCAM_RAIL_COUNT 3u
enum {PcamVrmVoltage = 0, PcamVrmEnable = 4, PcamVrmMode = 8, PcamVrmModeHigh = 7};     /* [BT] */

/* ARC [DT]：qcom,init-voltage-level 0x180 是 Android 在 rpmh-regulator probe 时投的值，CAMCC 的 vdd_mm、vdd_mxc 都是这两条。这个值是 TURBO 档：同一份设备树 CPAS 的
 * vdd-corners 和 vdd-corner-ahb-mapping 把 0x100 标成 nominal、0x180 标成 turbo。不改成 NOM：DRV-2 每个资源只有一票，这一票会顶掉 UEFI 留下的那票，显示也在 MMCX 上，
 * 选 Android 自己开机投的值才不至于把显示的票拉低。代价是这次开机这两条轨一直在 TURBO，这是测试级。绝不投 mx.lvl：GPU 驱动把它投在表顶，这里一投就把它拉低。 */
#define PCAM_ARC_MM "mmcx.lvl"
#define PCAM_ARC_MXC "mxc.lvl"
enum {PcamArcLevel = 0x180};

/* CCI [U]（i2c-qcom-cci.c 的寄存器，只取事实）。这个驱动只用 master 0、queue 0。 */
enum {
    PcamCciVersion = 0x000,
    PcamCciReset = 0x004, PcamCciResetAll = 0x0f73f3f7,
    PcamCciQueueStart = 0x008,
    PcamCciHalt = 0x034, PcamCciHaltM0 = 1,
    PcamCciSclCtl = 0x100, PcamCciSdaCtl0 = 0x104, PcamCciSdaCtl1 = 0x108, PcamCciSdaCtl2 = 0x10c, PcamCciMiscCtl = 0x110,
    PcamCciReadData = 0x118, PcamCciReadLevel = 0x11c,
    PcamCciExecWords = 0x300, PcamCciCurWords = 0x304, PcamCciLoadData = 0x310,
    PcamCciIrqGlobalClear = 0xc00, PcamCciIrqMask0 = 0xc04, PcamCciIrqClear0 = 0xc08, PcamCciIrqStatus0 = 0xc0c,
};
#define PCAM_CCI_IRQ_RD_DONE 0x1u
#define PCAM_CCI_IRQ_REPORT 0x10u
#define PCAM_CCI_IRQ_RESET_DONE 0x1000000u
#define PCAM_CCI_IRQ_HALT_ACK 0x2000000u
#define PCAM_CCI_IRQ_ERROR 0x18000ee6u      /* master 0 的 NACK、错误命令、溢出 */
/* 只写掩码让状态位锁存，中断线（SPI 0x1aa）从不开：这个驱动轮询。掩码关着时状态位锁不锁存待真机确认，主线和下游都是先写掩码。 */
#define PCAM_CCI_IRQ_ENABLE (PCAM_CCI_IRQ_RD_DONE | PCAM_CCI_IRQ_REPORT | PCAM_CCI_IRQ_RESET_DONE | PCAM_CCI_IRQ_HALT_ACK | PCAM_CCI_IRQ_ERROR)
enum {PcamCciCmdSetParam = 1, PcamCciCmdLock = 6, PcamCciCmdUnlock = 7, PcamCciCmdReport = 8, PcamCciCmdWrite = 9, PcamCciCmdRead = 10, PcamCciCmdWriteNoStop = 11,
      PcamCciReportIrq = 1 << 8, PcamCciRetries = 3, PcamCciRetryShift = 16};

/* [DT] i2c_freq_400Khz_cci0（第 26978 行）的节拍数，按 37.5 MHz 算的。这里 CCI 跑 XO 19.2 MHz，节拍数不变，总线约 204 kHz：所有最小时间都只变长，不违反 I²C。 */
typedef struct {uint16_t thigh, tlow, tsu_sto, tsu_sta, thd_dat, thd_sta, tbuf; uint8_t scl_stretch, trdhld, tsp;} PcamCciTiming;
static const PcamCciTiming kPcamCciFast = {0x26, 0x38, 0x28, 0x28, 0x16, 0x23, 0x3e, 0, 6, 3};

/* 长焦 S5KJN5 [DT]：cam-sensor2 的 ftm_setting_compatible = <W 0xfcfc 2 2 0x4000  R 0x0000 2 2 0x38e5>，cci-master 0，CSIPHY 5，MCLK0，复位 gpio178。
 * 从地址设备树里没有，在 sensormodule 里：这里是常见的两个 7 位地址 [推断]，按顺序试，只认 ID 对的那个。阶段 0 拿到真值后改成一项。 */
enum {PcamTelePageReg = 0xfcfc, PcamTelePageValue = 0x4000, PcamTeleIdReg = 0x0000, PcamTeleId = 0x38e5};
static const uint8_t kPcamTeleAddresses[2] = {0x2d, 0x10};
#define PCAM_TELE_ADDRESS_COUNT 2u

#define PCAM_QUIET_BIT 16u                  /* pagani_panel_probe_quiet 里相机的一位：bit 0 面板，bit 1 USB，bit 2 蓝牙，bit 3 留给 Wi-Fi，这里是 bit 4 */

#endif
