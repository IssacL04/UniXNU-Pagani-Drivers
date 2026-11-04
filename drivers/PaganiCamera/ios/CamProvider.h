/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* ios/CamProvider.m 和它的主机测试共用的名字。README.md 的“阶段 1a”写了来龙去脉和反汇编地址。 */
#ifndef PAGANI_CAM_PROVIDER_H
#define PAGANI_CAM_PROVIDER_H
#include <stdint.h>

#define PCAM_PROCESS       "mediaserverd"
/* 启用标记：种子里 ngpu/mediacapture 下的一个空文件，叠加层把它和插件一起显示在这里（scripts/add-pagani-camera-data.py --test-pattern）。
 * 用标记文件不用启动参数：命令行只剩 22 字节，而这个开关只有带这个插件的种子才有意义。 */
#define PCAM_MARKER        "/System/Library/MediaCapture/PaganiCameraTestPattern"
#define PCAM_BUNDLE        "org.unixnu.pagani.camera"
#define PCAM_DEVICE_ID     "8F1E7E7C-6A0B-4C43-9B7E-50A6A1C0CA01"
#define PCAM_STREAM_ID     "8F1E7E7C-6A0B-4C43-9B7E-50A6A1C0CA02"
#define PCAM_LOG_SERVICE   "PaganiCameraLog"    /* 内核的日志服务（PaganiCameraIos.cpp），没有时什么也不发 */
#define PCAM_LOG_SELECTOR  0u                   /* 结构输入：一行文本，含结尾 0 */
enum {PcamLogMax = 96, PcamFrameRate = 30, PcamModes = 2, PcamFrameLogEvery = 900};
/* 流的 FourCC：420f。CMIOExtensionFigCaptureStreamCreate 只原样保留 420f、420v、BGRA，别的会被改写成 420v。 */
#define PCAM_PIXEL_FORMAT  0x34323066u

/* pcam_vendor_install 的返回值。 */
enum {PcamVendorInstalled = 0, PcamVendorNoClass = -1, PcamVendorNoMethod = -2, PcamVendorNoIvar = -3};
/* pcam_provider_start 的返回值。 */
enum {PcamStartOk = 0, PcamStartNoClass = -1, PcamStartNoSelector = -2, PcamStartNoIvar = -3, PcamStartNoKey = -4, PcamStartMode = -5,
      PcamStartProvider = -6, PcamStartListener = -7, PcamStartAgain = -8};

/* 让 BWFigCaptureDeviceVendor 在没有默认设备时也能建起来（CamProvider.m 开头的说明）。0 或负数，负数时什么都没替换。 */
int pcam_vendor_install(void);
/* 建提供者、设备、流和匿名监听，把会话提供者挂进 CMIOExtensionDiscoverySession。不能在构造函数里调用。 */
int pcam_provider_start(void);
/* 标记文件在不在。 */
int pcam_marker_present(void);
/* 一行日志到内核，内核写成 "[cam] ios: ..." 的 U 行。 */
void pcam_kernel_line(const char *line);

/* 取证计数：[0] 客户连接 [1] 客户断开 [2] 流启动 [3] 流停止 [4] 发出的帧 [5] 丢掉的帧 [6] 最后一个错误 [7] 当前格式 */
extern volatile int32_t pcam_status[8];
#endif
