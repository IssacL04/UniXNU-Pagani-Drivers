/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 稳压器读回取自 PaganiBt BtPmic.c（它取自 PaganiWifi WifiPmic.c，UniXNU-sheng package/WifiProbe.cpp pmicScan，commit 08cde20e），名字改成 pcam_ 前缀，
 * 只留只读的部分：这个文件没有写路径。 */
/* 经 SPMI 仲裁器 v7 的 apps observer 通道读稳压器：STATUS 0x08、EN_CTL 0x46、MODE_CTL 0x45、设定值 mV 0x40/0x41。observer 是只读通道，读不到不算错：
 * PM8010（PPID 0xcxx）可能挂在第二条 SPMI 总线上（设备树 spmi1_bus），那时这里找不到 PPID，调用者记成 unread。 */
#ifndef PAGANI_CAM_PMIC_H
#define PAGANI_CAM_PMIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PCAM_SPMI_CORE 0xc400000ull
#define PCAM_SPMI_CORE_BYTES 0x3000u
#define PCAM_SPMI_CNFG 0xc42d000ull
#define PCAM_SPMI_CNFG_BYTES 0x4000u
#define PCAM_SPMI_OBS 0xc440000ull
#define PCAM_SPMI_OBS_BYTES 0x8000u         /* 只用 EE 0：APID × 0x20 */

enum {PcamSpmiOk = 0, PcamSpmiIoError = -5, PcamSpmiNotFound = -19, PcamSpmiTimeout = -110, PcamSpmiLayout = -95, PcamSpmiArgument = -22};

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);
    uint32_t (*cnfg)(void *ctx, uint32_t offset);
    uint32_t (*obs_read)(void *ctx, uint32_t offset);
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PcamSpmiIo;

int pcam_pmic_rail(const PcamSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);

#ifdef __cplusplus
}
#endif
#endif
