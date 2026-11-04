/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiBt 的对外接口（README.md）：Stage7Probe 调用的启动入口和状态字。默认关：没有 pagani-bt=N 时 pagani_bt_start 立刻返回，
 * 什么都不映射、不分配、不启动，也不发布任何给 iOS 看的东西。 */
#ifndef PAGANI_BT_H
#define PAGANI_BT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe：在 Wi-Fi 启动之后调用，起一个 "pagani-bt" 内核线程后立刻返回。阶段 4 先同步发布控制套接字和 bluetooth 服务，launchd 之前做完。 */
void pagani_bt_start(void);

/* 状态字（同样写进 ramoops 的 [bt] U 行）：
 *  [0] 走到的最后一步（BtBringup.h 的 PbStep*），负数：失败的那一步     [1] 该步的错误码（0 无）
 *  [2] pagani-bt 的原值     [3] 阶段     [4] 波特率     [5] 线程运行的秒数
 *  [6] SE 固件修订（GENI_FW_REVISION）     [7] 上电后的 SE_GENI_IOS     [8] 收到 0xFC 的次数
 *  [9] 外设补丁状态     [10] TME-L 补丁状态     [11] SoC 版本     [12] 产品号
 *  [13] 写失败（超时）次数     [14] M 错误位累计     [15] S 错误位累计     [16] LE 广播计数
 *  [17] RX_FIFO_STATUS 半字异常次数     [18] H4 杂字节数（轮询的补位字节算在这里，BtGeni.h）
 * 线程结束前 [13..15]、[17..18] 再写一行 [bt] uart stats，上机只能看到 U 行。
 * 阶段 4（BtBridge.h），写日志时刷新，同样写进 [bt] bridge 行：
 *  [19] 链路（PbLink）     [20] 守卫（PbGuard，0 没有 trip）     [21] 客户端连接次数     [22] 发给控制器的包     [23] 交给客户端的帧
 *  [24] 就地回的厂商命令条数     [25] 厂商命令种数     [26] 过滤层丢的帧     [27] 坏包     [28] 客户端放不下的次数
 *  [29] 连续走不完初始化的次数     [30] 拒绝的连接     [31] 没发出去的包 */
extern volatile int32_t pagani_bt_status[32];

#ifdef __cplusplus
}
#endif
#endif
