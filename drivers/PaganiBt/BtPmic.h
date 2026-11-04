/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 轨道读回和睡眠时钟引脚取自 PaganiWifi WifiPmic.c（UniXNU-sheng package/WifiProbe.cpp pmicScan/pmicPrepare，commit 08cde20e），名字改成
 * pb_ 前缀，两个驱动在同一个归档里。BT_EN 是新写的；PMIC GPIO 的寄存器事实来自主线 pinctrl-spmi-gpio.c，只取事实。 */
/* 蓝牙上电的 PMIC 一侧，走 SPMI 仲裁器 v7（core 0xc400000，owner 表 0xc42d000，observer 0xc440000，通道 0xc500000 + APID x 0x1000）：
 *  - 稳压器状态，只经 apps observer 通道读：STATUS 0x08、EN_CTL 0x46、MODE_CTL 0x45、设定值 mV 0x40/0x41；
 *  - PMK8550 gpio3 的 32 kHz 睡眠时钟（PPID 0x0ba），和 Wi-Fi 同一条规则；
 *  - BT_EN：PM8550VE-F gpio3（PPID 0x58a），普通功能、推挽输出、无上下拉、power-source 1。写之前查 owner 是 apps（EE 0）、类型是 GPIO
 *    （0x10）、子类型是 LV/MV（0x10..0x13）。输出电平在 DIG_OUT_SOURCE_CTL 0x44 的 bit 7，源选 0；写后读回。这个文件不写别的 PPID。 */
#ifndef PAGANI_BT_PMIC_H
#define PAGANI_BT_PMIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PB_SPMI_CORE 0xc400000ull
#define PB_SPMI_CORE_BYTES 0x3000u
#define PB_SPMI_CNFG 0xc42d000ull
#define PB_SPMI_CNFG_BYTES 0x4000u
#define PB_SPMI_OBS 0xc440000ull
#define PB_SPMI_OBS_BYTES 0x8000u          /* 只用 EE 0：APID x 0x20 */
#define PB_SPMI_CHAN 0xc500000ull
#define PB_SPMI_CHAN_STRIDE 0x1000u
#define PB_SLEEP_CLOCK_PPID 0x0bau

enum {PbSpmiOk = 0, PbSpmiIoError = -5, PbSpmiNotFound = -19, PbSpmiTimeout = -110, PbSpmiNotOwner = -1, PbSpmiLayout = -95,
      PbSpmiArgument = -22};

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);
    uint32_t (*cnfg)(void *ctx, uint32_t offset);
    uint32_t (*obs_read)(void *ctx, uint32_t offset);
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    /* 一个 APID 的通道页（空指针：没有写路径） */
    uint32_t (*chan_read)(void *ctx, unsigned apid, uint32_t offset);
    void (*chan_write)(void *ctx, unsigned apid, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PbSpmiIo;

/* PPID 的 APID，优先 apps（EE 0）的通道；*owner 给出 owner。没有时返回负数。 */
int pb_spmi_find(const PbSpmiIo *io, unsigned ppid, int *owner);
int pb_spmi_read8(const PbSpmiIo *io, int apid, uint8_t reg, uint8_t *value);
/* 稳压器的 STATUS、EN_CTL、MODE_CTL（读失败时 0xee，只用于记录）和设定值。 */
int pb_pmic_rail(const PbSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);

/* 一个 GPIO 引脚：寄存器 04 05 40 41 42 44 45 46 */
enum {PbGpioRegs = 8};
typedef struct {
    int apid, owner;
    uint8_t regs[PbGpioRegs];   /* 操作前 */
    uint8_t after[PbGpioRegs];  /* 操作后 */
    int written;                /* 写了几次 */
} PbGpioState;

/* 1：已经是 func1 使能输出；2：刚打开（write 置位时）；0：不是（write 清零时）；负数：读不了或开不了。Wi-Fi 规则，另一个 EE 占着就不写。 */
int pb_pmic_sleep_clock(const PbSpmiIo *io, int write, PbGpioState *out);
/* level 0/1：把 BT_EN 配成输出并拉到这个电平，读回确认。level < 0：只读。返回 0 或负数。写之前检查 owner 和子类型，
 * 不是 apps 的通道（PbSpmiNotOwner）或不是 LV/MV GPIO（PbSpmiLayout）一个字节都不写。 */
int pb_pmic_bt_en(const PbSpmiIo *io, int level, PbGpioState *out);

#ifdef __cplusplus
}
#endif
#endif
