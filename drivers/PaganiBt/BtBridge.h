/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 阶段 4：真 bluetoothd 和控制器之间的桥（README.md 的阶段 4）。协议栈一侧是流式控制套接字 com.apple.uart.sk.bluetooth，控制器一侧是 GENI 串口，
 * 中间过 H4 成帧和过滤（BtH4.h）。这里不碰硬件也不碰内核接口，全部走 PbBridgeIo，内核的外壳在 PaganiBtHost.cpp，测试接假 bluetoothd 和控制器模型。
 *
 * 发送：写来的字节切成包。OGF 0x3f 就地回 Command Complete（状态 0x01），在 bluetoothd 自己的线程上当场回：它同步等，15 s 等不到就 VSC stall 崩溃，
 *       这条路不经过串口，控制器死了也照回。其余进发送环，工作线程发出去。
 * 接收：工作线程取字节，成帧，IBS 就地处理（0xfd 回 0xfc），0x34、0xff 事件、ACL 句柄 0x2edc 丢掉，其余进上行环，按帧交给 client_put。放不下（ENOBUFS）
 *       就留在环里下一轮再试，上行环快满时不再取串口：字节留在 FIFO 里，RFR 硬件流控叫控制器等，一帧都不丢。
 * 主机方向常醒：不发 0xfe，所以发送前不用 0xfd（BtIbs.h），功耗留到阶段 7。
 *
 * 控制器还没起来（Starting）：连接照收，命令留在发送环里，起来之后再发。连接里绝不等：bluetoothd 在 connect 时持着协议栈的大锁。等得太久由
 * bluetoothd 自己的计时处理（Command Complete 10 s，Reset 失败它不管，第二条失败就崩溃），launchd 十秒后重开，下一次就接上了。
 *
 * 守卫：bluetoothd 卡住或崩溃循环时不能拖住 SpringBoard。它的传输打不开、HCI 初始化失败都是 abort（sub_10016BBC8、sub_10016C020 到 sub_100107728），拒绝连接
 * 只会变成崩溃循环，控制器没了它也不会自己停下，所以判定和收尾都在这里。下面任何一条成立就 trip：这次开机蓝牙作废，link 变 Dead，以后的连接都拒绝。
 *   PbGuardBringup  阶段 2 的上电或下载失败
 *   PbGuardWrite    串口写失败（CTS 一直不放）
 *   PbGuardCommand  一条发下去的命令 PbBridgeCommandMs 没有应答，比 bluetoothd 的 10 s 先到
 *   PbGuardCrash    控制器报崩溃转储
 *   PbGuardInit     连上之后 PbBridgeInitMs 走不到初始化表的最后一步，bluetoothd 卡在不知道哪里
 *   PbGuardStarts   连续 PbBridgeFailedStarts 个 bluetoothd 走不完初始化就退了（崩溃循环）
 * 外壳接到 trip 后撤掉 bluetooth 服务和 MobileGestalt 的两个键，再 SIGKILL 连着的进程：同步 XPC 立刻出错返回，launchd 重开的 bluetoothd 在 start 里
 * 找不到服务，No bluetooth on this device，退出码 0 不再重开；MobileBluetooth 的 BTSessionAttachWithQueue 看不到服务直接返回 9，不发任何 XPC。最后的状态和没开阶段 4 一样。
 *
 * 替身（pagani-bt=40）：不起工作线程，不碰任何寄存器，命令由 BtStandIn.c 当场回。用来先单独验证 bluetoothd、控制套接字、服务节点、text-fixups 和 SpringBoard 的关系。
 *
 * 线程：pb_bridge_connect、write、disconnect 在客户端线程上（kern_control 回调，套接字锁已放开），pb_bridge_step 在工作线程上，只有它碰串口。共享状态全在
 * io->lock 下，串口和日志都在锁外。client_put 在锁内调用：kern_control 调回调之前都放开了套接字锁，这把锁不会嵌在套接字锁里面，顺序只有这把锁在外。 */
#ifndef PAGANI_BT_BRIDGE_H
#define PAGANI_BT_BRIDGE_H
#include <stdint.h>
#include "BtH4.h"
#include "BtIbs.h"
#ifdef __cplusplus
extern "C" {
#endif

enum PbLink {PbLinkStarting, PbLinkUp, PbLinkDead, PbLinkStandIn};
enum PbGuard {PbGuardNone, PbGuardBringup, PbGuardWrite, PbGuardCommand, PbGuardCrash, PbGuardInit, PbGuardStarts};
enum {
    PbBridgeOk = 0, PbBridgeBusy = -1, PbBridgeRefused = -2,    /* pb_bridge_connect */
    PbBridgeStop = -1,                                          /* pb_bridge_step：trip 了，线程该收尾 */
};
enum {
    PbBridgeRingBytes = 16384,      /* 2 的幂。8 个 1024 字节的 ACL 额度放得下，协议栈按额度发，正常不会满 */
    PbBridgeReadRoom = 4096,        /* 上行环剩这么多才取串口：取一次最多凑齐一个 PbH4FrameMax 的帧加 256 字节的小帧 */
    PbBridgeReadRounds = 8,         /* 一步最多取几轮：连续收的时候也要回去看发送 */
    PbBridgeCommandMs = 4000,       /* 比 bluetoothd 的 10 s（配置表 46 的 +3）短，判定权留在这里 */
    PbBridgeInitMs = 30000,
    PbBridgeFailedStarts = 5,
    PbBridgeWriteMs = 1000,
    PbBridgeRoomMs = 1000,          /* 客户端线程等环腾地方的上限 */
    PbBridgeWaitStepMs = 50,
    PbBridgeActiveMs = 64,          /* 最近这么久有过字节：1 ms 一轮 */
    PbBridgeBusyPollMs = 1, PbBridgeIdlePollMs = 16, PbBridgeNoClientPollMs = 100,
    PbBridgeVscMax = 64, PbBridgeRecent = 16, PbBridgeNotes = 32,
    PbBridgeLogConnections = 32,    /* 崩溃循环别冲掉 ramoops */
    PbBridgeLogRefusals = 16,
};

typedef struct {
    void *ctx;
    void (*lock)(void *ctx, int take);
    uint64_t (*now_ms)(void *ctx);
    /* 工作线程，锁外。阻塞到发完，失败返回非 0 */
    int (*uart_write)(void *ctx, const uint8_t *data, uint32_t bytes, unsigned timeout_ms);
    /* 工作线程，锁外。不阻塞，max 至少 256 */
    uint32_t (*uart_read)(void *ctx, uint8_t *data, uint32_t max);
    /* 持锁。把一帧交给客户端，要么全收要么不收。0 收下；大于 0 现在放不下，下一轮再试；小于 0 客户端已经没了，丢掉 */
    int (*client_put)(void *ctx, uint32_t unit, const uint8_t *data, uint32_t bytes);
    /* 持锁：工作线程睡着时叫醒它 */
    void (*kick)(void *ctx);
    /* 持锁，睡的时候放开锁：等环腾地方，最多 ms；醒来时又持锁 */
    void (*wait_room)(void *ctx, unsigned ms);
    /* 持锁：叫醒 wait_room */
    void (*wake_room)(void *ctx);
    void (*log)(void *ctx, const char *line);   /* 锁外 */
} PbBridgeIo;

typedef struct {
    uint32_t head, tail;            /* 自由增长，取模用掩码 */
    uint8_t data[PbBridgeRingBytes];
} PbRing;

typedef struct {uint32_t kind, a, b;} PbBridgeNote;

typedef struct {
    const PbBridgeIo *io;
    uint8_t link, guard, client, ready, ready_noted, ack_due, own_reset;
    uint32_t unit;
    int pid, kill_pid;              /* kill_pid：trip 时连着的进程，0 没有 */
    uint64_t connected_at, up_at, pending_at, active_at;
    uint32_t pending;               /* 发下去、还没等到 Command Complete 或 Command Status 的命令数 */
    uint32_t failed_starts;
    uint8_t address[6];             /* 线上顺序，替身的 Read_BD_ADDR 用 */
    PbH4Tx from_client;             /* 客户端线程独占，disconnect 由 kern_control 排在 send 之后 */
    PbH4Rx rx;                      /* 工作线程独占 */
    PbIbs ibs;
    PbRing to_uart, to_client;
    uint8_t scratch[PbH4FrameMax];  /* 持锁时用 */
    uint8_t out[PbH4FrameMax];      /* 工作线程独占 */
    uint8_t in[256];                /* 工作线程独占 */
    uint16_t recent[PbBridgeRecent];
    uint32_t recent_count;
    uint16_t vsc[PbBridgeVscMax];
    uint32_t vsc_kinds, vsc_answered;
    PbBridgeNote notes[PbBridgeNotes];
    uint32_t note_head, note_tail, notes_lost;
    uint32_t connections, refused, logged_connections, refused_logged;
    uint32_t packets_out, frames_in, frames_up, frames_dropped, frames_bad, frames_no_client, put_full, put_lost;
    uint32_t tx_dropped, rx_paused, up_full;
} PbBridge;

/* address 是线上顺序，可以为空。stand_in 非 0：替身，不用工作线程。其余情况从 Starting 开始。 */
void pb_bridge_init(PbBridge *b, const PbBridgeIo *io, const uint8_t address[6], int stand_in);
/* 客户端线程。PbBridgeOk，PbBridgeBusy 已有客户端，PbBridgeRefused 这次开机蓝牙作废。 */
int pb_bridge_connect(PbBridge *b, uint32_t unit, int pid);
void pb_bridge_disconnect(PbBridge *b, uint32_t unit);
/* 客户端线程：写来的字节，边界任意。 */
void pb_bridge_write(PbBridge *b, uint32_t unit, const uint8_t *data, uint32_t bytes);
/* 下载完成：接过成帧状态和下载路径多读的字节，开始工作。 */
void pb_bridge_up(PbBridge *b, const PbH4Rx *rx, const uint8_t *rest, uint32_t rest_bytes);
/* 上电或下载失败。 */
void pb_bridge_fail(PbBridge *b);
/* 工作线程的一步。大于等于 0：下一步之前最多睡这么多毫秒（kick 能提前叫醒）；PbBridgeStop：trip 了。 */
int pb_bridge_step(PbBridge *b);

#ifdef __cplusplus
}
#endif
#endif
