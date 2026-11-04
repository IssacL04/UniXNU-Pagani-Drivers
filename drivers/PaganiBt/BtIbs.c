/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtIbs.h"
#include "BtTlv.h"
#include "BtUtil.h"

void pb_ibs_init(PbIbs *s, unsigned max_tries, int sleep_enabled)
{
    pb_zero(s, sizeof *s);
    s->tx = PbIbsAsleep;
    s->max_tries = (uint16_t)max_tries;
    s->sleep_enabled = sleep_enabled ? 1 : 0;
}

unsigned pb_ibs_want_tx(PbIbs *s, uint64_t now_ms)
{
    if (s->tx == PbIbsAwake) {
        s->idle_at = 0;                      /* 发送途中不算空闲 */
        return PbIbsTxReady;
    }
    if (s->tx == PbIbsWaking) return 0;
    s->tx = PbIbsWaking;
    s->tries = 1;
    s->retry_at = now_ms + PbIbsRetryMs;
    s->wakes_sent++;
    return PbIbsSendWake;
}

void pb_ibs_tx_done(PbIbs *s, uint64_t now_ms)
{
    if (s->tx == PbIbsAwake) s->idle_at = now_ms + PbIbsIdleMs;
}

unsigned pb_ibs_rx(PbIbs *s, uint8_t byte, uint64_t now_ms)
{
    switch (byte) {
    case PbIbsWakeAck:
        s->acks_seen++;
        if (s->tx != PbIbsWaking) {
            s->stray_acks++;
            return 0;
        }
        s->tx = PbIbsAwake;
        s->idle_at = now_ms + PbIbsIdleMs;
        return PbIbsTxReady;
    case PbIbsWake:
        s->wakes_seen++;
        s->peer_awake = 1;
        s->acks_sent++;
        return PbIbsSendAck;
    case PbIbsSleep:
        s->sleeps_seen++;
        s->peer_awake = 0;
        return 0;
    default:
        return 0;
    }
}

unsigned pb_ibs_tick(PbIbs *s, uint64_t now_ms)
{
    if (s->tx == PbIbsWaking && now_ms >= s->retry_at) {
        if (s->tries >= s->max_tries) {
            s->tx = PbIbsAsleep;
            s->failures++;
            return PbIbsFailed;
        }
        s->tries++;
        s->retry_at = now_ms + PbIbsRetryMs;
        s->wakes_sent++;
        return PbIbsSendWake;
    }
    if (s->tx == PbIbsAwake && s->sleep_enabled && s->idle_at && now_ms >= s->idle_at) {
        s->tx = PbIbsAsleep;
        s->idle_at = 0;
        s->sleeps_sent++;
        return PbIbsSendSleep;
    }
    return 0;
}
