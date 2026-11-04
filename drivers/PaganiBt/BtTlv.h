/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 下载协议的纯函数：0x31 外设帧、HCI EDL 帧、243 字节 TLV 分段、NVM 的内存改写、控制器事件的解析、启动参数和本机地址。没有状态、
 * 不碰硬件，全部字节和 Android 的 hsuart 跟踪比对过（Test/replay-hsuart.py）。
 *
 * 帧结构来自本机 HAL 的反编译（PeriPatchDLManager、PatchDLManager），只取事实：
 *   发送  31 00 | opcode 小端 | len | subop | ss ...        0xfff0 外设 EDL，0xfff1 外设通用
 *         01 00 fc | len | subop ...                      HCI EDL（统一命令接口）
 *   接收  34 00 ff | len | f0 | kind ...                  kind 0 状态，1 完成，2 激活完成，3 补丁通知，4/5/7 转储
 *         04 0e | plen | 01 | opcode 小端 | status | subop ...
 * 接收格式这边没有亲眼见过：跟踪在 15.11 s 之前的 RX 全丢了，0x34 的布局只来自 HAL 的解析代码，第一轮上机要原样记录。 */
#ifndef PAGANI_BT_TLV_H
#define PAGANI_BT_TLV_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PbSegmentBytes = 243,                   /* TlvDnldReq：每段 243 字节 */
    PbPeriHeader = 8, PbEdlHeader = 6,
    PbPacketMax = 300,                      /* 够放最长的 251 字节段 */
    PbOpPeriEdl = 0xfff0, PbOpPeriGeneral = 0xfff1,
    PbSubDownload = 0x06, PbSubGetVer = 0x05, PbSubArbitrate = 0x08, PbSubBuildInfo = 0x09, PbSubBoardId = 0x0a,
    PbSubBaud = 0x02, PbSubReset = 0x03, PbSubActivate = 0x00,
    PbSsPeri = 0, PbSsBt = 1, PbSsTmel = 3,
    PbPatchNone = 0, PbPatchBusy = 11, PbPatchDone = 23,
    PbTmelAbsent1 = 5, PbTmelAbsent2 = 6,
    PbOpEdl = 0xfc00, PbEdlDownload = 0x1e, PbEdlGetVer = 0x19, PbEdlBuildInfo = 0x20, PbEdlBoardId = 0x23,
    PbOpAddOn = 0xfc1d, PbOpOtp = 0xfc09, PbOpLog = 0xfc17, PbLogSet = 0x14, PbLogCop = 0x19,
    PbOpReset = 0x0c03, PbOpLocalVersion = 0x1001, PbOpBdAddr = 0x1009, PbOpEventMask = 0x0c01, PbOpLeEventMask = 0x2001,
    PbOpLeScanParams = 0x200b, PbOpLeScanEnable = 0x200c,
    PbOpHostCompleted = 0x0c35,             /* Host_Number_Of_Completed_Packets：规范里没有应答事件 */
    PbOpErroneousData = 0x0c5b,             /* Write_Default_Erroneous_Data_Reporting：bluetoothd 初始化表最后一步（0x1008d4640 第 25 项） */
    PbH4Cmd = 1, PbH4Acl = 2, PbH4Sco = 3, PbH4Evt = 4, PbH4Iso = 5, PbH4PeriCmd = 0x31, PbH4Peri = 0x34,
    PbIbsWakeAck = 0xfc, PbIbsWake = 0xfd, PbIbsSleep = 0xfe,
    PbEvtCommandComplete = 0x0e, PbEvtCommandStatus = 0x0f, PbEvtHardwareError = 0x10, PbEvtLeMeta = 0x3e, PbEvtVendor = 0xff,
    PbLeAdvReport = 0x02,
    PbTagBdAddr = 2, PbTagHci = 17, PbTagDeepSleep = 27, PbTagAsd = 174,
    PbTlvNvm = 2, PbTlvContainer = 4,
};

/* ---- 发送帧：写进 out（至少 PbPacketMax 字节），返回长度 ---- */
unsigned pb_peri_getver(uint8_t *out, uint8_t ss);                /* 31 00 f0 ff 02 05 ss */
unsigned pb_peri_baud(uint8_t *out, uint8_t code);                /* 31 00 f1 ff 02 02 code */
unsigned pb_peri_arbitrate(uint8_t *out, uint8_t ss);             /* 31 00 f0 ff 03 08 ss 00 */
unsigned pb_peri_board_id(uint8_t *out);                          /* 31 00 f0 ff 01 0a：HAL 只发 6 字节，ss 不发 */
unsigned pb_peri_build_info(uint8_t *out, uint8_t ss);            /* 31 00 f0 ff 02 09 ss */
unsigned pb_peri_reset(uint8_t *out);                             /* 31 00 f1 ff 01 03：同上只发 6 字节 */
unsigned pb_peri_activate(uint8_t *out, uint8_t ss, uint8_t action);   /* 31 00 f1 ff 03 00 ss action */
unsigned pb_peri_segment(uint8_t *out, uint8_t ss, const uint8_t *data, unsigned bytes);
unsigned pb_edl_command(uint8_t *out, uint8_t sub);               /* 01 00 fc 01 sub */
unsigned pb_edl_segment(uint8_t *out, const uint8_t *data, unsigned bytes);
/* 一条 HCI 命令：01 opcode 小端 plen params */
unsigned pb_hci_command(uint8_t *out, uint16_t opcode, const uint8_t *params, unsigned plen);

/* ---- 分段 ---- */
/* 文件切成多少段；*last 是最后一段的字节数。0 字节返回 0。 */
uint32_t pb_tlv_segments(uint32_t bytes, uint32_t *last);
/* 这个文件是不是每段都要等应答：ELF（以及 event_handling=3 的 TLV 补丁）只等最后一段，NVM 每段都等。 */
int pb_tlv_wait_each(const uint8_t *data, uint32_t bytes);

/* ---- NVM ---- */
typedef struct {
    int baud_code;              /* 写进 Tag 17 data[1]；负数不动 */
    int sibs;                   /* Tag 27 data[1] bit 0：1 置位，0 清除，负数不动 */
    int asd;                    /* Tag 174 data[0] bit 0：同上，HAL 默认属性 false，清除 */
    const uint8_t *bdaddr;      /* Tag 2 的 6 字节，低字节在前；空指针不动 */
} PbNvmEdit;
typedef struct {uint16_t tags; uint8_t saw_addr, saw_hci, saw_sleep, saw_asd;} PbNvmReport;
/* 改内存副本，绝不写回分区。只改 TLV 类型 2 的块，开头是类型 4 容器时跳过 4 字节头。文件是不可信数据，越界返回负数且不留半截：
 * 先走一遍只校验，再走一遍改。 */
int pb_nvm_edit(uint8_t *nvm, uint32_t bytes, const PbNvmEdit *edit, PbNvmReport *report);
/* HAL 的校准容器：04 len24 + NVM + 校准。out 至少 nvm_bytes + cal_bytes + 4。XNU 读不到校准文件，正常路径不走这里，只有跟踪回放用。 */
uint32_t pb_nvm_wrap(uint8_t *out, const uint8_t *nvm, uint32_t nvm_bytes, const uint8_t *cal, uint32_t cal_bytes);

/* ---- 事件解析 ---- */
enum {PbPeriOther, PbPeriStatus, PbPeriComplete, PbPeriActivated, PbPeriPatchNtf, PbPeriDump};
typedef struct {
    uint8_t kind;
    uint8_t status;             /* 状态/完成的状态字节；通知里的状态 */
    uint8_t sub;
    uint8_t ss;                 /* 完成事件的 a[11]；通知、激活里的子系统 */
    uint8_t action;
    uint16_t opcode;
} PbPeriEvent;
/* frame 是一整包 0x34（含 34 00 两字节）。长度不够的算 PbPeriOther。 */
void pb_peri_parse(const uint8_t *frame, uint32_t bytes, PbPeriEvent *event);
/* frame 是收到的一整帧。1：控制器自己崩了，0x34 转储事件，或 0xff 厂商事件里的崩溃转储（ReadHciEvent 的判法）。 */
int pb_frame_crashed(const uint8_t *frame, uint32_t bytes);
typedef struct {uint32_t product, soc; uint16_t patch, build; uint8_t security; uint8_t has_soc;} PbPeriVersion;
int pb_peri_version(const uint8_t *frame, uint32_t bytes, PbPeriVersion *version);

/* frame 是一整包 HCI 事件（含 04）。Command Complete 时返回 1，并给出 opcode、状态；厂商命令的 subop 在 *sub（没有时 -1）。 */
int pb_hci_complete(const uint8_t *frame, uint32_t bytes, uint16_t *opcode, uint8_t *status, int *sub);
typedef struct {uint32_t product, soc; uint16_t patch, build;} PbEdlVersion;
int pb_edl_version(const uint8_t *frame, uint32_t bytes, PbEdlVersion *version);
/* 取板号应答：*hi 是 data[0]，*lo 是 data[1]。 */
int pb_edl_board_id(const uint8_t *frame, uint32_t bytes, uint8_t *hi, uint8_t *lo);
/* 版本串：*text 指进 frame，*length 是字节数。 */
int pb_edl_build_label(const uint8_t *frame, uint32_t bytes, const uint8_t **text, uint32_t *length);

/* ---- 启动参数 pagani-bt=N ---- */
typedef struct {unsigned phase; uint32_t baud; int stop; int stand_in;} PbArg;   /* phase 0：关 */
/* 1、2、3、4：跑到这个阶段结束，4 之后留着给 bluetoothd。23、28、33、38、43、48：第二位是下载用的 Mbps。40：阶段 4 的替身，
 * stand_in 为 1，不碰硬件（BtBridge.h）。101..501：跑到这一步停（一个参数省字节）。其余都是关。 */
void pb_arg_parse(unsigned value, PbArg *arg);

/* 从 /chosen/unique-chip-id 派生的固定地址（低字节在前）：每台手机固定、每次开机相同，手机自己的地址在 Modem NV 里读不到。
 * 避开保留的 LAP 0x9e8b00..0x9e8b3f。seed 为 0 时返回 0，不给地址。 */
int pb_bdaddr_from_seed(uint64_t seed, uint8_t address[6]);
/* 线上的低字节在前换成书写顺序：local-mac-address 和 /chosen/mac-address-bluetooth0 第一个字节是最高字节（bluetoothd 按 %02x:... 顺序打印）。 */
void pb_bdaddr_written(const uint8_t wire[6], uint8_t written[6]);

#ifdef __cplusplus
}
#endif
#endif
