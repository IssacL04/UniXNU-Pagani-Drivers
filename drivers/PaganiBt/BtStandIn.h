/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* pagani-bt=40 的替身控制器：不碰任何硬件，HCI 命令当场回。只为了走完 bluetoothd 的初始化，用来单独验证 iOS 一侧（控制套接字、服务节点、text-fixups、
 * SpringBoard），不是模拟器：没有连接，没有扫描结果，ACL 全部丢掉。
 *
 * 读类命令的返回值是这台手机的控制器在 Android 上答的（hsuart 跟踪 15.33..15.36 s），写类命令一律状态 0。例外写在表旁边：Read_Local_Supported_Commands
 * 的应答在跟踪里只留了前 14 字节，后面 50 字节这里按“全支持”填，这是占位，不是事实。
 * 走 Command Status 的命令（OGF 1 和几条 LE 命令）回 Command Status，状态 0x0c（现在不允许）：没有后续事件，协议栈不会空等。 */
#ifndef PAGANI_BT_STAND_IN_H
#define PAGANI_BT_STAND_IN_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {PbStandInMax = 80, PbStandInDisallowed = 0x0c};
/* packet 是一条完整的 HCI 命令（含 01）。应答写进 out（至少 PbStandInMax 字节），返回字节数，0 表示这条没有应答。address 是线上顺序。 */
uint32_t pb_standin_reply(const uint8_t *packet, uint32_t bytes, const uint8_t address[6], uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
