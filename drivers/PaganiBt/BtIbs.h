/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 带内睡眠（IBS）状态机，事实来自本机 HAL 的 IbsHandler 和 hsuart 跟踪，不含 GPL 代码。两个方向各管各的：
 *   主机发送方向：睡着时要发数据，先发 0xfd，等 0xfc；0xfd 每 10 ms 重发一次（StartWackTimer），最多 tries 次；
 *                 空闲 250 ms 发 0xfe 睡下（InitInt、StartIdleTimer）。这一步留到功耗阶段，sleep_enabled 默认 0，先常醒。
 *   控制器发送方向：它发 0xfd，主机回 0xfc，它发 0xfe 表示它不再发了。
 * 所以控制器发 0xfe 不代表主机要重新唤醒它：跟踪 15.399870 收到 fe 之后，15.523526 主机直接发数据，控制器接着回 fd。
 * 纯状态机：不碰硬件，不看时钟，调用方把时间传进来，再按返回的动作位去发字节。 */
#ifndef PAGANI_BT_IBS_H
#define PAGANI_BT_IBS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {PbIbsAsleep, PbIbsWaking, PbIbsAwake};
/* 动作位 */
enum {PbIbsSendWake = 1, PbIbsSendAck = 2, PbIbsSendSleep = 4, PbIbsTxReady = 8, PbIbsFailed = 16};
enum {PbIbsRetryMs = 10, PbIbsIdleMs = 250, PbIbsTriesHal = 176, PbIbsTriesBoot = 10};

typedef struct {
    uint8_t tx, peer_awake, sleep_enabled;
    uint16_t tries, max_tries;
    uint64_t retry_at, idle_at;
    uint32_t wakes_sent, acks_sent, sleeps_sent, acks_seen, wakes_seen, sleeps_seen, stray_acks, failures;
} PbIbs;

void pb_ibs_init(PbIbs *s, unsigned max_tries, int sleep_enabled);
/* 有数据要发。Awake：PbIbsTxReady。睡着：PbIbsSendWake，转到 Waking。Waking：没有动作，等应答。 */
unsigned pb_ibs_want_tx(PbIbs *s, uint64_t now_ms);
/* 一发完：重置空闲计时。 */
void pb_ibs_tx_done(PbIbs *s, uint64_t now_ms);
/* 收到一个 IBS 字节（0xfc/0xfd/0xfe）。0xfc 在 Waking 时返回 PbIbsTxReady；0xfd 返回 PbIbsSendAck。 */
unsigned pb_ibs_rx(PbIbs *s, uint8_t byte, uint64_t now_ms);
/* 定时：Waking 到点重发（超过次数 PbIbsFailed，回到 Asleep）；开了 sleep_enabled 时 Awake 空闲到点发 0xfe。 */
unsigned pb_ibs_tick(PbIbs *s, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
