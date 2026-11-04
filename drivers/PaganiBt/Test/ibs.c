/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtIbs.c：两个方向互相独立，重发间隔和次数，空闲睡眠默认关。 */
#include "../BtIbs.h"
#include "../BtTlv.h"
#include "check.h"

int main(void)
{
    /* 数字本身来自 HAL：WakeUpSoC 10 次、select 10 ms，IbsHandler 空闲 250 ms、重发 176 次 */
    CHECK(PbIbsTriesBoot == 10 && PbIbsRetryMs == 10 && PbIbsIdleMs == 250 && PbIbsTriesHal == 176);
    PbIbs s;
    pb_ibs_init(&s, PbIbsTriesBoot, 0);
    CHECK(s.tx == PbIbsAsleep);

    /* 醒来：发 fd，10 ms 内不重发，到点重发，收到 fc 就绪 */
    CHECK(pb_ibs_want_tx(&s, 100) == PbIbsSendWake);
    CHECK(s.tx == PbIbsWaking && s.wakes_sent == 1);
    CHECK(pb_ibs_want_tx(&s, 101) == 0);               /* 不重复发 */
    CHECK(pb_ibs_tick(&s, 109) == 0);
    CHECK(pb_ibs_tick(&s, 110) == PbIbsSendWake);
    CHECK(s.wakes_sent == 2);
    CHECK(pb_ibs_rx(&s, PbIbsWakeAck, 112) == PbIbsTxReady);
    CHECK(s.tx == PbIbsAwake);
    CHECK(pb_ibs_want_tx(&s, 113) == PbIbsTxReady);

    /* 跟踪 15.399870：控制器发 fe，主机发送方向不受影响 */
    CHECK(pb_ibs_rx(&s, PbIbsSleep, 200) == 0);
    CHECK(!s.peer_awake && s.tx == PbIbsAwake);
    CHECK(pb_ibs_want_tx(&s, 201) == PbIbsTxReady);
    /* 跟踪 15.523821：控制器发 fd，主机回 fc */
    CHECK(pb_ibs_rx(&s, PbIbsWake, 300) == PbIbsSendAck);
    CHECK(s.peer_awake && s.acks_sent == 1);

    /* 没人等时来的 fc 计数不动作 */
    CHECK(pb_ibs_rx(&s, PbIbsWakeAck, 301) == 0);
    CHECK(s.stray_acks == 1);

    /* 默认常醒：空闲多久都不发 fe */
    pb_ibs_tx_done(&s, 400);
    CHECK(pb_ibs_tick(&s, 400 + PbIbsIdleMs + 10000) == 0);
    CHECK(s.tx == PbIbsAwake && s.sleeps_sent == 0);

    /* 次数用完：失败，回到睡着，下一次能重新开始 */
    pb_ibs_init(&s, PbIbsTriesBoot, 0);
    uint64_t now = 0;
    CHECK(pb_ibs_want_tx(&s, now) == PbIbsSendWake);
    unsigned sent = 1;
    for (unsigned i = 0; i < 100; ++i) {
        now += PbIbsRetryMs;
        const unsigned a = pb_ibs_tick(&s, now);
        if (a & PbIbsFailed) break;
        CHECK(a == PbIbsSendWake);
        ++sent;
    }
    CHECK(sent == PbIbsTriesBoot);                     /* WakeUpSoC：最多 10 次 */
    CHECK(s.tx == PbIbsAsleep && s.failures == 1);
    CHECK(pb_ibs_want_tx(&s, now) == PbIbsSendWake);

    /* 功耗阶段的空闲睡眠：开了才发 fe，发送途中不算空闲，睡下后要重新唤醒 */
    pb_ibs_init(&s, PbIbsTriesHal, 1);
    (void)pb_ibs_want_tx(&s, 0);
    (void)pb_ibs_rx(&s, PbIbsWakeAck, 1);
    pb_ibs_tx_done(&s, 10);
    CHECK(pb_ibs_tick(&s, 10 + PbIbsIdleMs - 1) == 0);
    CHECK(pb_ibs_want_tx(&s, 200) == PbIbsTxReady);
    CHECK(pb_ibs_tick(&s, 10 + PbIbsIdleMs) == 0);     /* 发送开始后计时清零 */
    pb_ibs_tx_done(&s, 300);
    CHECK(pb_ibs_tick(&s, 300 + PbIbsIdleMs) == PbIbsSendSleep);
    CHECK(s.tx == PbIbsAsleep && s.sleeps_sent == 1);
    CHECK(pb_ibs_tick(&s, 300 + 2 * PbIbsIdleMs) == 0);
    CHECK(pb_ibs_want_tx(&s, 1000) == PbIbsSendWake);

    printf("PASS ibs: %u checks\n", checks);
    return 0;
}
