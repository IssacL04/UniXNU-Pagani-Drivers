/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiBtHost.cpp 对 PaganiBt.cpp 的接口：阶段 4 面向 iOS 的一半（控制套接字、bluetooth 服务、MobileGestalt 的两个键、守卫的收尾）。 */
#ifndef PAGANI_BT_HOST_H
#define PAGANI_BT_HOST_H
#include <stdint.h>
#include "BtH4.h"
#ifdef __cplusplus
extern "C" {
#endif

/* pagani_bt_start 同步调用，launchd 之前做完：注册控制名，写 /chosen、/product 的键，发布 bluetooth 服务。不碰硬件。
 * address 是线上顺序（BtTlv.h），baud 写进 transport-speed，stand_in 非 0 时命令由替身回。0 成功，负数失败且什么都没留下。 */
int pagani_bt_host_publish(const uint8_t address[6], uint32_t baud, int stand_in);
/* 蓝牙线程：下载完成，接管串口，一直跑到守卫 trip，撤销之后返回。 */
void pagani_bt_host_run(const PbH4Rx *rx, const uint8_t *rest, uint32_t rest_bytes);
/* 蓝牙线程：上电或下载失败，撤销。 */
void pagani_bt_host_failed(void);

/* PaganiBt.cpp 提供：下载用的同一个串口。 */
int pagani_bt_uart_write(const uint8_t *data, uint32_t bytes, unsigned timeout_ms);
uint32_t pagani_bt_uart_read(uint8_t *data, uint32_t max);

#ifdef __cplusplus
}
#endif
#endif
