/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 控制器的引导序列，顺序是本机 HAL 的调用顺序（PeriPatchDLManager::PeriSocInit，PatchDLManager::SocInit），只取事实：
 *
 *   握手        0xfd 等 0xfc；0x31 取版本，等 0x34                                                 步 302、303
 *   换速（可选）RFR 手动关；发设波特率；20 ms；改主机时钟；RFR 还给硬件；读应答                        步 402
 *   外设        仲裁 ss0；ELF 不逐段等；取板号；NVM 逐段等；取构建信息；复位；等补丁通知               步 403
 *   TME-L       0xfd 等 0xfc；仲裁 ss3；取版本；ELF；等补丁通知                                     步 404
 *   激活        ActivateSS(BT)，之后 BT 子系统才响应 HCI                                           步 405
 *   BT          EDL 取版本；ELF 不逐段等                                                          步 406
 *               取板号；NVM 逐段等                                                                步 407
 *               版本串、附加特性、读 OTP、日志设置、CoP 版本、HCI_Reset                              步 408
 *   校验        Read_Local_Version、Read_BD_ADDR                                                 步 409
 *   自检        开 LE 被动扫描，数广播                                                             步 501
 *
 * Test/replay-hsuart.py 用真固件跑这条序列，把发出的字节和 Android 的跟踪逐条比对。这里不碰硬件，字节都走 PbXport，内核接 GENI，
 * 测试接控制器模型。失败只返回错误码和一行日志，不 panic，不重试。 */
#ifndef PAGANI_BT_DOWNLOAD_H
#define PAGANI_BT_DOWNLOAD_H
#include <stdint.h>
#include "BtH4.h"
#include "BtIbs.h"
#include "BtTlv.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 步号：百位是类别。负数的步号表示这一步失败。 */
enum PbStep {
    PbStepSurvey = 101, PbStepClocks = 102,
    PbStepPower = 201, PbStepBtEn = 202,
    PbStepUart = 301, PbStepWake = 302, PbStepVersion = 303, PbStepHciProbe = 304,
    PbStepFiles = 401, PbStepBaud = 402, PbStepPeri = 403, PbStepTmel = 404, PbStepActive = 405, PbStepBtFw = 406, PbStepBtNvm = 407,
    PbStepReset = 408, PbStepCheck = 409,
    PbStepScan = 501,
    PbStepHandover = 601,
};
enum PbError {
    PbErrNone = 0, PbErrStopped = -1, PbErrWrite = -2, PbErrTimeout = -3, PbErrStatus = -4, PbErrProtocol = -5, PbErrDump = -6,
    PbErrHwError = -7, PbErrNvm = -8, PbErrBaud = -9, PbErrArgument = -10, PbErrRail = -11, PbErrRailRead = -12, PbErrLock = -13,
    PbErrBranch = -14, PbErrRcg = -15, PbErrGeni = -16, PbErrBtEn = -17, PbErrFirmware = -18, PbErrIdentity = -19,
};

typedef struct {
    void *ctx;
    /* 发完才返回，超时返回负数 */
    int (*write)(void *ctx, const uint8_t *data, uint32_t bytes, unsigned timeout_ms);
    /* 取走已到的字节，不阻塞，max 至少 256 */
    uint32_t (*read)(void *ctx, uint8_t *data, uint32_t max);
    void (*sleep_ms)(void *ctx, unsigned ms);
    uint64_t (*now_ms)(void *ctx);
    /* ready 0：RFR 手动未就绪，叫控制器别发；1：交还硬件 */
    int (*rfr)(void *ctx, int ready);
    /* 停接收，改 RCG 和分频，开接收 */
    int (*host_baud)(void *ctx, uint32_t baud);
    void (*log)(void *ctx, const char *line);
} PbXport;

typedef struct {
    uint32_t baud;                  /* 下载用的波特率，115200 表示不换速（kPbRates 里的值） */
    int stop;                       /* 走完这一步就停（0：不停） */
    const uint8_t *bdaddr;          /* Tag 2，低字节在前；空指针保留文件里的占位值 */
    const uint8_t *peri, *tmel, *bt;
    uint8_t *peri_nvm, *bt_nvm;     /* 可写的内存副本，就地改 */
    uint32_t peri_bytes, tmel_bytes, bt_bytes, peri_nvm_bytes, bt_nvm_bytes;
} PbDlConfig;

typedef struct {
    const PbXport *x;
    PbDlConfig cfg;
    PbH4Rx h4;
    PbIbs ibs;
    uint8_t rx[256];
    uint32_t rx_pos, rx_len;
    int step, error;
    uint8_t code;                   /* cfg.baud 的波特率码 */
    uint8_t peri_status, tmel_status, activated, peri_ntf, tmel_ntf;
    PbPeriVersion peri_version, tmel_version;
    PbEdlVersion edl_version;
    uint8_t local_version[8], local_addr[6], label_ok;
    uint32_t packets, bytes, frames, ignored, acl_logs, log_budget, adverts;
    int hci_ok;                     /* 步 409 通过 */
} PbDl;

void pb_dl_init(PbDl *d, const PbXport *x, const PbDlConfig *cfg);
int pb_dl_wake(PbDl *d);            /* 步 302 */
int pb_dl_version(PbDl *d);         /* 步 303 */
int pb_dl_hci_probe(PbDl *d);       /* 步 304：可选，只记录 */
int pb_dl_boot(PbDl *d);            /* 步 402..408 */
int pb_dl_check(PbDl *d);           /* 步 409 */
int pb_dl_le_scan(PbDl *d, unsigned seconds);   /* 步 501 */
/* 步 601：把控制器交给 bluetoothd 之前再唤一次。下载路径多读的字节在 d->rx[rx_pos..rx_len)，成帧状态在 d->h4，由 BtBridge.c 接着用。 */
int pb_dl_handover(PbDl *d);

#ifdef __cplusplus
}
#endif
#endif
