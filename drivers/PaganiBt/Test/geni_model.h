/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* GENI UART 串行引擎的模型，按硬件行为答话：字节按波特率和分频随时间移出，CTS 拦发送，RFR 拦对端，接收按 4 字节打成 FIFO 字，半个字等
 * stale 才带着 RX_LAST 出来，IRQ 状态只显示使能了的位。写错打包、用错水线、FIFO 写爆都会在 errors 里记下，测试检查它是 0。
 * 这是对寄存器语义的假设，不是证据：它只能证明驱动和这份假设一致。 */
#ifndef PAGANI_BT_TEST_GENI_MODEL_H
#define PAGANI_BT_TEST_GENI_MODEL_H
#include <stdint.h>
#include <string.h>
#include "../BtGeni.h"

#define GM_WIRE_MAX 400000u
typedef struct {
    uint32_t regs[0x1000 / 4];
    uint32_t rcg_hz;                    /* 喂给 SE 的时钟 */
    uint64_t us;
    /* 发送 */
    uint32_t tx_fifo[256], tx_head, tx_count, tx_depth;
    uint8_t tx_word[4];
    uint32_t tx_word_left, tx_remaining;
    uint8_t m_active;
    uint8_t wire[GM_WIRE_MAX];          /* 上线的字节 */
    uint32_t wire_len;
    uint64_t tx_next_us;
    /* 接收 */
    uint8_t peer[GM_WIRE_MAX];          /* 对端要发的字节 */
    uint32_t peer_head, peer_len;
    uint32_t rx_fifo[256], rx_head, rx_count, rx_depth, rx_last_bytes;
    uint32_t rx_word, rx_have;
    uint64_t rx_next_us, stale_at_us;
    uint8_t s_active;
    /* 中断状态 */
    uint32_t m_irq, s_irq;
    /* 外部条件 */
    uint8_t cts_blocked;                /* 对端拉高 RTS，不让发 */
    uint8_t cancel_stuck, abort_stuck;  /* cancel 没反应 */
    uint8_t s_cancel_stuck;
    uint32_t rx_status_or;              /* 读 RX_FIFO_STATUS 时额外或上的位 */
    uint32_t inject_m_error;            /* 下一次发送结束时打这些错误位 */
    /* 记录 */
    uint32_t errors, writes, m_cancels, m_aborts, s_cancels, s_aborts, rx_dropped;
    uint32_t max_rx_fill;
    uint8_t forbid_writes;              /* 探测阶段：任何写都算错 */
} GeniModel;

static inline uint32_t gm_baud(const GeniModel *m)
{
    const uint32_t cfg = m->regs[PB_SE_M_CLK_CFG / 4], div = (cfg >> 4) & 0xfffu;
    if (!(cfg & 1u) || !div) return 0;
    return m->rcg_hz / 16u / div;
}
static inline uint64_t gm_byte_us(const GeniModel *m)
{
    const uint32_t baud = gm_baud(m);
    return baud ? 1 + 10000000ull / baud : 1000000ull;
}
static inline void gm_rx_push(GeniModel *m, uint32_t word)
{
    if (m->rx_count >= m->rx_depth) {
        m->rx_dropped++;
        m->s_irq |= PB_IRQ_RX_WR_ERR;
        return;
    }
    m->rx_fifo[(m->rx_head + m->rx_count++) % 256] = word;
    if (m->rx_count > m->max_rx_fill) m->max_rx_fill = m->rx_count;
}
static inline void gm_rx_byte(GeniModel *m, uint8_t byte)
{
    m->rx_word |= (uint32_t)byte << (8 * m->rx_have);
    if (++m->rx_have == 4) {
        gm_rx_push(m, m->rx_word);
        m->rx_word = 0;
        m->rx_have = 0;
        m->rx_last_bytes = 0;
    }
    m->stale_at_us = m->us + gm_byte_us(m) * m->regs[PB_SE_UART_RX_STALE / 4] / 10u;
}
static inline int gm_rfr_ready(const GeniModel *m)
{
    const uint32_t manual = m->regs[PB_SE_UART_MANUAL_RFR / 4];
    if (manual & PB_UART_RFR_MANUAL) return !(manual & PB_UART_RFR_NOT_READY);
    return m->s_active && m->rx_count < m->regs[PB_SE_RFR_WATERMARK / 4];
}
/* 让时间走到 target。 */
static inline void gm_run(GeniModel *m, uint64_t target)
{
    while (m->us < target) {
        uint64_t next = target;
        const int can_tx = m->m_active && m->tx_remaining && (m->tx_word_left || m->tx_count) &&
                           (!m->cts_blocked || (m->regs[PB_SE_UART_TX_TRANS_CFG / 4] & PB_UART_CTS_MASK) ||
                            (m->regs[PB_SE_UART_LOOPBACK / 4] & 2u));
        const int loop = m->regs[PB_SE_UART_LOOPBACK / 4] & 1u;
        const int can_rx = m->peer_head < m->peer_len && gm_rfr_ready(m) && !loop;
        if (can_tx && m->tx_next_us < next) next = m->tx_next_us > m->us ? m->tx_next_us : m->us;
        if (can_rx && m->rx_next_us < next) next = m->rx_next_us > m->us ? m->rx_next_us : m->us;
        if (m->rx_have && m->stale_at_us && m->stale_at_us < next) next = m->stale_at_us;
        m->us = next;
        if (can_tx && m->us >= m->tx_next_us) {
            if (!m->tx_word_left) {
                const uint32_t word = m->tx_fifo[m->tx_head];
                m->tx_head = (m->tx_head + 1) % 256;
                m->tx_count--;
                for (unsigned i = 0; i < 4; ++i) m->tx_word[i] = (uint8_t)(word >> (8 * i));
                m->tx_word_left = 4;
            }
            const uint8_t byte = m->tx_word[4 - m->tx_word_left--];
            if (m->wire_len < GM_WIRE_MAX) m->wire[m->wire_len++] = byte;
            if (loop && m->s_active) gm_rx_byte(m, byte);
            m->tx_next_us = m->us + gm_byte_us(m);
            if (!--m->tx_remaining) {
                m->tx_word_left = 0;
                m->m_active = 0;
                m->m_irq |= PB_IRQ_DONE | m->inject_m_error;
                m->inject_m_error = 0;
            }
        }
        if (can_rx && m->us >= m->rx_next_us) {
            if (m->s_active) gm_rx_byte(m, m->peer[m->peer_head]);
            else m->rx_dropped++;
            m->peer_head++;
            m->rx_next_us = m->us + gm_byte_us(m);
        }
        if (m->rx_have && m->stale_at_us && m->us >= m->stale_at_us) {
            m->rx_last_bytes = m->rx_have;
            gm_rx_push(m, m->rx_word);
            m->rx_word = 0;
            m->rx_have = 0;
            m->stale_at_us = 0;
            m->s_irq |= PB_IRQ_RX_LAST;
        }
        if (m->us >= target) break;
    }
}
static inline void gm_init(GeniModel *m, uint32_t words)
{
    memset(m, 0, sizeof *m);
    m->regs[PB_SE_FW_REVISION / 4] = PB_PROTO_UART << 8 | 0x66;
    m->regs[PB_SE_HW_PARAM_0 / 4] = 32u << 24 | words << 16;
    m->regs[PB_SE_HW_PARAM_1 / 4] = 32u << 24 | words << 16;
    m->regs[PB_SE_IOS / 4] = PB_IOS_RX;
    m->regs[PB_SE_M_CLK_CFG / 4] = 2u << 4 | 1u;
    m->rcg_hz = 7372800;
    m->tx_depth = m->rx_depth = words;
}
static inline void gm_peer(GeniModel *m, const uint8_t *data, uint32_t bytes)
{
    memcpy(m->peer + m->peer_len, data, bytes);
    m->peer_len += bytes;
}
static inline uint32_t gm_read(GeniModel *m, uint32_t reg)
{
    if (reg & 3 || reg >= sizeof m->regs) {
        m->errors++;
        return 0;
    }
    m->us += 1;
    gm_run(m, m->us);
    switch (reg) {
    case PB_SE_STATUS: return (m->m_active ? PB_STATUS_M_ACTIVE : 0) | (m->s_active ? PB_STATUS_S_ACTIVE : 0);
    case PB_SE_M_IRQ_STATUS: return m->m_irq & m->regs[PB_SE_M_IRQ_EN / 4];
    case PB_SE_S_IRQ_STATUS: return m->s_irq & m->regs[PB_SE_S_IRQ_EN / 4];
    case PB_SE_TX_FIFO_STATUS: return m->tx_count;
    case PB_SE_RX_FIFO_STATUS: {
        uint32_t v = m->rx_count;
        if (m->rx_count && m->rx_last_bytes) v |= PB_RX_FIFO_LAST | m->rx_last_bytes << PB_RX_FIFO_LAST_SHIFT;
        return v | m->rx_status_or;
    }
    case PB_SE_RX_FIFO: {
        if (!m->rx_count) {
            m->errors++;
            return 0xdeadbeefu;
        }
        const uint32_t word = m->rx_fifo[m->rx_head];
        m->rx_head = (m->rx_head + 1) % 256;
        if (!--m->rx_count) m->rx_last_bytes = 0;
        return word;
    }
    case PB_SE_IOS: return (m->regs[PB_SE_IOS / 4] & ~PB_IOS_CTS) | (m->cts_blocked ? PB_IOS_CTS : 0);
    default: return m->regs[reg / 4];
    }
}
static inline void gm_write(GeniModel *m, uint32_t reg, uint32_t value)
{
    if (reg & 3 || reg >= sizeof m->regs || m->forbid_writes) {
        m->errors++;
        return;
    }
    m->writes++;
    m->us += 1;
    gm_run(m, m->us);
    switch (reg) {
    case PB_SE_M_CMD0:
        if (m->m_active || value != PB_UART_START_TX) m->errors++;
        m->m_active = 1;
        m->tx_remaining = m->regs[PB_SE_UART_TX_TRANS_LEN / 4];
        m->tx_next_us = m->us;
        break;
    case PB_SE_S_CMD0:
        if (m->s_active || value != PB_UART_START_RX) m->errors++;
        m->s_active = 1;
        m->rx_next_us = m->us;
        break;
    case PB_SE_M_CMD_CTRL:
        if (value == PB_CMD_CTRL_CANCEL) {
            m->m_cancels++;
            if (!m->cancel_stuck) {
                m->m_active = 0;
                m->tx_count = 0;
                m->tx_word_left = 0;
                m->m_irq |= PB_IRQ_CANCEL | PB_IRQ_DONE;
            }
        } else if (value == PB_CMD_CTRL_ABORT) {
            m->m_aborts++;
            if (!m->abort_stuck) {
                m->m_active = 0;
                m->tx_count = 0;
                m->tx_word_left = 0;
                m->m_irq |= PB_IRQ_ABORT | PB_IRQ_DONE;
            }
        }
        break;
    case PB_SE_S_CMD_CTRL:
        if (value == PB_CMD_CTRL_CANCEL) {
            m->s_cancels++;
            if (!m->s_cancel_stuck) {
                m->s_active = 0;
                m->s_irq |= PB_IRQ_CANCEL | PB_IRQ_DONE;
            }
        } else if (value == PB_CMD_CTRL_ABORT) {
            m->s_aborts++;
            m->s_active = 0;
            m->s_irq |= PB_IRQ_ABORT | PB_IRQ_DONE;
        }
        break;
    case PB_SE_M_IRQ_CLEAR: m->m_irq &= ~value; break;
    case PB_SE_S_IRQ_CLEAR: m->s_irq &= ~value; break;
    case PB_SE_TX_FIFO:
        if (!m->m_active) m->errors++;
        if (m->tx_count >= m->tx_depth) {
            m->errors++;
            m->m_irq |= PB_IRQ_TX_WR_ERR;
            break;
        }
        m->tx_fifo[(m->tx_head + m->tx_count++) % 256] = value;
        break;
    default:
        m->regs[reg / 4] = value;
        break;
    }
}
#endif
