/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CCI（相机控制接口）的 I²C 主机，轮询，只用 master 0、queue 0。协议是公开的，寄存器和命令字见 PaganiCameraBoard.h，这里的代码是自己写的：
 * 把命令字一个个写进 LOAD_DATA，把 CUR_WORD_CNT 抄进 EXEC_WORD_CNT，QUEUE_START 启动，等 IRQ_STATUS。
 *   写：SET_PARAM、WRITE（字节连续打包）、REPORT，等 REPORT 位。
 *   读：SET_PARAM、LOCK、WRITE_DISABLE_P（寄存器地址，不发 STOP）、READ、UNLOCK，等 RD_DONE，READ_DATA 第一个字节不是数据。
 * 出错（NACK、超时）时先 HALT，再全局复位，重新配置，调用者可以接着试下一个地址。 */
#ifndef PAGANI_CAM_CCI_H
#define PAGANI_CAM_CCI_H
#include <stdint.h>
#include "PaganiCameraBoard.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PcamCciIo;

enum {PcamCciOk = 0, PcamCciArgument = -22, PcamCciTimeout = -110, PcamCciNack = -6 /* ENXIO：包括别的错误位 */, PcamCciBusy = -16, PcamCciData = -71};
enum {PcamCciPolls = 2000, PcamCciPollUs = 10};     /* 20 ms：两字节读在 204 kHz 下不到 1 ms */

/* 复位，写中断掩码和 master 0 的时序。 */
int pcam_cci_init(const PcamCciIo *io, const PcamCciTiming *timing);
/* 16 位寄存器、16 位数据，大端。addr 是 7 位从地址。出错时已经恢复到可以再用。 */
int pcam_cci_write16(const PcamCciIo *io, const PcamCciTiming *timing, uint8_t addr, uint16_t reg, uint16_t value);
int pcam_cci_read16(const PcamCciIo *io, const PcamCciTiming *timing, uint8_t addr, uint16_t reg, uint16_t *value);

#ifdef __cplusplus
}
#endif
#endif
