/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiCamera 的对外接口（README.md）：Stage7Probe 调用的启动入口和状态字。默认关：没有 pagani-cam=N 时 pagani_camera_start 不碰硬件，什么都不映射、不分配、
 * 不启动；只有 pagani-torch-ios=1 时多发布一个日志服务（PaganiCameraIos.cpp），让插件的进度进 U 行。 */
#ifndef PAGANI_CAMERA_H
#define PAGANI_CAMERA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe：蓝牙之后调用。起一个 "pagani-cam" 内核线程后立刻返回。 */
void pagani_camera_start(void);

/* 状态字（同样写进 ramoops 的 [cam] U 行）：
 *  [0] 走到的最后一步（CamBringup.h 的 PcamStep*），负数：失败的那一步     [1] 该步的错误码（0 无）
 *  [2] pagani-cam 的原值     [3] 线程运行的秒数     [4] 读到的 CCI 版本     [5] 读到的传感器 ID     [6] 回应的从地址
 *  [7] 闸门拒绝的写次数（应该永远是 0）     [8] 窗口外访问次数（应该永远是 0）     [9] 日志服务写入的行数     [10] 日志服务被拒的行数 */
extern volatile int32_t pagani_camera_status[16];

#ifdef __cplusplus
}
#endif
#endif
