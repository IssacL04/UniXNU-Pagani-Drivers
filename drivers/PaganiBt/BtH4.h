/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* H4 成帧和过滤层。UART 上除了标准 H4 包，还混着 IBS 单字节、0x34 外设事件、控制器日志，这些都不能交给协议栈。
 *
 * 接收：字节流切成帧（只有帧边界上的 0xfc/0xfd/0xfe 才是 IBS，帧里的同样数值是数据），pb_h4_filter_rx 决定上交还是丢：
 *   丢弃 0x34 外设事件、ACL 句柄 0x2edc 的控制器日志、0xff 厂商事件。bluetoothd 的 H4 接收只认包类型 1..4，别的字节进重同步。
 * 发送：pb_h4_filter_tx 解析协议栈写来的一包。OGF 0x3f 的厂商命令一律不下发，就地回 Command Complete 状态 0x01：博通和高通的
 *   厂商命令共用 0xfcxx，高通 0xfc00 是下载、0xfc0b 是 NVM、0xfc14 是写地址，透传可能写坏运行状态。HCI_Reset 照常透传。
 * 协议栈的写端是流套接字：一包可能分几次来，几包也可能粘在一起，pb_h4_tx_byte 先切成包，再过滤。
 * 阶段 4（BtBridge.h）走这里接 bluetoothd，阶段 2、3 的内核线程也走这里成帧。 */
#ifndef PAGANI_BT_H4_H
#define PAGANI_BT_H4_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {PbH4FrameMax = 2048};                 /* 控制器 ACL 上限 1024，日志包更短 */
enum {PbH4More, PbH4Ibs, PbH4Frame, PbH4Junk, PbH4Oversize};

typedef struct {
    uint8_t buf[PbH4FrameMax];
    uint32_t have, need, skip;              /* need：这一帧总长，0 表示头还没收齐；skip：超长帧还要吃掉的字节 */
    uint32_t junk, oversize, frames;
} PbH4Rx;

void pb_h4_rx_reset(PbH4Rx *r);
/* 喂一个字节。PbH4Ibs：这个字节是 IBS（在 r->buf[0]）。PbH4Frame：r->buf[0..r->have) 是完整一帧，下一次喂字节时清掉。 */
int pb_h4_rx_byte(PbH4Rx *r, uint8_t byte);

typedef struct {
    uint8_t buf[PbH4FrameMax];
    uint32_t have, need, skip;
    uint32_t junk, oversize, packets;
} PbH4Tx;

void pb_h4_tx_reset(PbH4Tx *t);
/* 协议栈写来的一个字节。包头只认 1、2、3，别的字节计数跳过。PbH4Frame：t->buf[0..t->have) 是完整一包，下一次喂字节时清掉。 */
int pb_h4_tx_byte(PbH4Tx *t, uint8_t byte);

enum {PbFilterPass, PbFilterDrop, PbFilterAnswered, PbFilterBad};
enum {PbDebugHandle = 0x2edc};
/* frame 是接收到的完整一帧。 */
int pb_h4_filter_rx(const uint8_t *frame, uint32_t bytes);
/* packet 是协议栈写来的一包（含 H4 类型字节）。PbFilterAnswered：不下发，把 *reply_bytes 字节的 reply（至少 7 字节）交回协议栈，
 * *vendor_opcode 记下命令号备查。PbFilterBad：包不完整或类型不认识，丢掉。 */
int pb_h4_filter_tx(const uint8_t *packet, uint32_t bytes, uint8_t *reply, uint32_t *reply_bytes, uint16_t *vendor_opcode);

#ifdef __cplusplus
}
#endif
#endif
