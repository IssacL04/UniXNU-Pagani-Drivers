/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtGeni.h"
#include "BtUtil.h"

enum {kCancelPolls = 1000, kCancelStepUs = 10, kDonePolls = 20, kDoneStepUs = 25};

static uint32_t rd(PbGeni *g, uint32_t reg) {return g->io.read(g->io.ctx, reg);}
static void wr(PbGeni *g, uint32_t reg, uint32_t value) {g->io.write(g->io.ctx, reg, value);}

/* SE_HW_PARAM 的 FIFO 深度在 23:16（QUP ≥ 3.10 的 256 字节版本，HSUartDxe 也这样取）；读到不像话的值时用主线的默认 16 字，水线要减 8，
 * 所以下限也是它。 */
static uint32_t fifo_depth(uint32_t param)
{
    const uint32_t depth = (param >> 16) & 0xffu;
    return depth >= 16 && depth <= 128 ? depth : 16;
}

int pb_geni_probe(PbGeni *g, const PbGeniIo *io, PbGeniInfo *info)
{
    pb_zero(g, sizeof *g);
    g->io = *io;
    PbGeniInfo i;
    pb_zero(&i, sizeof i);
    i.fw = rd(g, PB_SE_FW_REVISION);
    i.proto = (i.fw >> 8) & 0xffu;
    i.version = i.fw & 0xffu;
    i.if_disable = rd(g, PB_SE_IF_DISABLE);
    i.status = rd(g, PB_SE_STATUS);
    i.ios = rd(g, PB_SE_IOS);
    i.param0 = rd(g, PB_SE_HW_PARAM_0);
    i.param1 = rd(g, PB_SE_HW_PARAM_1);
    i.m_clk = rd(g, PB_SE_M_CLK_CFG);
    i.tx_depth = fifo_depth(i.param0);
    i.rx_depth = fifo_depth(i.param1);
    if (info) *info = i;
    if (i.proto != PB_PROTO_UART) return PbGeniErrProto;
    if (i.if_disable & 1u) return PbGeniErrNoFifo;
    g->tx_depth = i.tx_depth;
    g->rx_depth = i.rx_depth;
    return PbGeniOk;
}

/* 取消一个序列器的命令：先 cancel，等不到就 abort。 */
static int stop_command(PbGeni *g, uint32_t ctrl, uint32_t status, uint32_t clear)
{
    wr(g, ctrl, PB_CMD_CTRL_CANCEL);
    for (unsigned n = 0; n < kCancelPolls; ++n) {
        if (rd(g, status) & PB_IRQ_CANCEL) {
            wr(g, clear, PB_IRQ_CANCEL | PB_IRQ_DONE);
            return PbGeniOk;
        }
        g->io.delay_us(g->io.ctx, kCancelStepUs);
    }
    wr(g, ctrl, PB_CMD_CTRL_ABORT);
    for (unsigned n = 0; n < kCancelPolls; ++n) {
        if (rd(g, status) & PB_IRQ_ABORT) {
            wr(g, clear, PB_IRQ_ABORT | PB_IRQ_DONE);
            return PbGeniOk;
        }
        g->io.delay_us(g->io.ctx, kCancelStepUs);
    }
    return PbGeniErrCancel;
}

int pb_geni_config(PbGeni *g, unsigned div, unsigned level)
{
    if (!g->tx_depth || !div || div > 0xfffu) return PbGeniErrArgument;
    const uint32_t status = rd(g, PB_SE_STATUS);
    if (status & PB_STATUS_M_ACTIVE) (void)stop_command(g, PB_SE_M_CMD_CTRL, PB_SE_M_IRQ_STATUS, PB_SE_M_IRQ_CLEAR);
    if (status & PB_STATUS_S_ACTIVE) (void)stop_command(g, PB_SE_S_CMD_CTRL, PB_SE_S_IRQ_STATUS, PB_SE_S_IRQ_CLEAR);
    g->rx_running = 0;
    wr(g, PB_SE_CGC_CTRL, rd(g, PB_SE_CGC_CTRL) | 0x7fu);
    wr(g, PB_SE_DMA_GENERAL_CFG, rd(g, PB_SE_DMA_GENERAL_CFG) | 0xfu);
    wr(g, PB_SE_OUTPUT_CTRL, 0x7fu);
    wr(g, PB_SE_FORCE_DEFAULT, 1u);
    wr(g, PB_SE_GSI_EVENT_EN, 0);
    wr(g, PB_SE_DMA_MODE, 0);
    wr(g, PB_SE_TX_PACK0, PB_PACK0_4X8);
    wr(g, PB_SE_TX_PACK1, PB_PACK1_4X8);
    wr(g, PB_SE_RX_PACK0, PB_PACK0_4X8);
    wr(g, PB_SE_RX_PACK1, PB_PACK1_4X8);
    wr(g, PB_SE_BYTE_GRAN, 0);
    wr(g, PB_SE_TX_WATERMARK, PB_TX_WATERMARK);
    wr(g, PB_SE_RX_WATERMARK, g->rx_depth - PB_RX_WATERMARK_GAP);
    wr(g, PB_SE_RFR_WATERMARK, g->rx_depth - PB_RFR_WATERMARK_GAP);
    wr(g, PB_SE_M_IRQ_EN, PB_M_IRQ_WANTED);
    wr(g, PB_SE_S_IRQ_EN, PB_S_IRQ_WANTED);
    wr(g, PB_SE_M_IRQ_CLEAR, 0xffffffffu);
    wr(g, PB_SE_S_IRQ_CLEAR, 0xffffffffu);
    wr(g, PB_SE_UART_LOOPBACK, 0);
    wr(g, PB_SE_UART_TX_TRANS_CFG, PB_UART_CTS_MASK);
    wr(g, PB_SE_UART_TX_PARITY, 0);
    wr(g, PB_SE_UART_RX_TRANS_CFG, 0);
    wr(g, PB_SE_UART_RX_PARITY, 0);
    wr(g, PB_SE_UART_TX_WORD_LEN, 8u);
    wr(g, PB_SE_UART_RX_WORD_LEN, 8u);
    wr(g, PB_SE_UART_TX_STOP_LEN, 0);
    wr(g, PB_SE_UART_RX_STALE, PB_STALE_BITS);
    wr(g, PB_SE_UART_MANUAL_RFR, PB_UART_RFR_MANUAL | PB_UART_RFR_NOT_READY);
    g->ready = 1;
    return pb_geni_set_clock(g, div, level);
}

int pb_geni_set_clock(PbGeni *g, unsigned div, unsigned level)
{
    if (!g->ready || !div || div > 0xfffu) return PbGeniErrArgument;
    const uint32_t cfg = (uint32_t)div << 4 | 1u;
    wr(g, PB_SE_CLK_SEL, level);
    wr(g, PB_SE_M_CLK_CFG, cfg);
    wr(g, PB_SE_S_CLK_CFG, cfg);
    return PbGeniOk;
}

void pb_geni_flow(PbGeni *g, int cts, int rfr_auto)
{
    wr(g, PB_SE_UART_TX_TRANS_CFG, cts ? 0 : PB_UART_CTS_MASK);
    wr(g, PB_SE_UART_MANUAL_RFR, rfr_auto ? 0 : PB_UART_RFR_MANUAL | PB_UART_RFR_NOT_READY);
}

int pb_geni_rx_start(PbGeni *g)
{
    if (!g->ready) return PbGeniErrArgument;
    if (g->rx_running) return PbGeniOk;
    wr(g, PB_SE_S_IRQ_CLEAR, 0xffffffffu);
    wr(g, PB_SE_S_CMD0, PB_UART_START_RX);
    g->rx_running = 1;
    return PbGeniOk;
}

int pb_geni_rx_stop(PbGeni *g)
{
    if (!g->rx_running) return PbGeniOk;
    g->rx_running = 0;
    return stop_command(g, PB_SE_S_CMD_CTRL, PB_SE_S_IRQ_STATUS, PB_SE_S_IRQ_CLEAR);
}

void pb_geni_loopback(PbGeni *g, int on) {wr(g, PB_SE_UART_LOOPBACK, on ? PB_UART_LOOPBACK_ALL : 0);}
uint32_t pb_geni_ios(PbGeni *g) {return rd(g, PB_SE_IOS);}

/* 等 CMD_DONE：先短等几轮（小包），再按毫秒睡。 */
static int wait_done(PbGeni *g, uint64_t deadline)
{
    for (unsigned n = 0;; ++n) {
        const uint32_t status = rd(g, PB_SE_M_IRQ_STATUS);
        if (status & PB_M_IRQ_ERRORS) {
            g->m_sticky |= status;
            wr(g, PB_SE_M_IRQ_CLEAR, status);
            return PbGeniErrBus;
        }
        if (status & PB_IRQ_DONE) {
            wr(g, PB_SE_M_IRQ_CLEAR, status);
            return PbGeniOk;
        }
        if (g->io.now_ms(g->io.ctx) >= deadline) return PbGeniErrTimeout;
        if (n < kDonePolls) g->io.delay_us(g->io.ctx, kDoneStepUs);
        else g->io.sleep_ms(g->io.ctx, 1);
    }
}

int pb_geni_write(PbGeni *g, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    if (!g->ready || !data || !bytes) return PbGeniErrArgument;
    if (rd(g, PB_SE_STATUS) & PB_STATUS_M_ACTIVE) return PbGeniErrBusy;
    wr(g, PB_SE_M_IRQ_CLEAR, 0xffffffffu);
    wr(g, PB_SE_UART_TX_TRANS_LEN, bytes);
    wr(g, PB_SE_M_CMD0, PB_UART_START_TX);
    const uint64_t deadline = g->io.now_ms(g->io.ctx) + timeout_ms;
    uint32_t sent = 0;
    int rc = PbGeniOk;
    while (sent < bytes) {
        const uint32_t used = rd(g, PB_SE_TX_FIFO_STATUS) & PB_TX_FIFO_WC;
        uint32_t room = used < g->tx_depth ? g->tx_depth - used : 0;
        if (!room) {
            const uint32_t status = rd(g, PB_SE_M_IRQ_STATUS);
            if (status & PB_M_IRQ_ERRORS) {
                g->m_sticky |= status;
                wr(g, PB_SE_M_IRQ_CLEAR, status);
                rc = PbGeniErrBus;
                break;
            }
            if (g->io.now_ms(g->io.ctx) >= deadline) {
                rc = PbGeniErrTimeout;
                break;
            }
            g->io.sleep_ms(g->io.ctx, 1);
            continue;
        }
        for (; room && sent < bytes; --room) {
            uint32_t word = 0;
            for (uint32_t i = 0; i < PB_BYTES_PER_WORD && sent < bytes; ++i) word |= (uint32_t)data[sent++] << (8 * i);
            wr(g, PB_SE_TX_FIFO, word);
        }
    }
    if (rc == PbGeniOk) rc = wait_done(g, deadline);
    if (rc == PbGeniErrTimeout || rc == PbGeniErrBus) {
        g->tx_timeouts += rc == PbGeniErrTimeout;
        if (stop_command(g, PB_SE_M_CMD_CTRL, PB_SE_M_IRQ_STATUS, PB_SE_M_IRQ_CLEAR)) return PbGeniErrCancel;
        return rc;
    }
    g->tx_bytes += bytes;
    return PbGeniOk;
}

uint32_t pb_geni_read(PbGeni *g, uint8_t *out, uint32_t max)
{
    if (!g->rx_running || max < PB_BYTES_PER_WORD) return 0;
    const uint32_t status = rd(g, PB_SE_S_IRQ_STATUS);
    if (status) {
        g->s_sticky |= status & (PB_S_IRQ_ERRORS | PB_S_IRQ_GP);
        wr(g, PB_SE_S_IRQ_CLEAR, status);
    }
    const uint32_t fifo = rd(g, PB_SE_RX_FIFO_STATUS);
    uint32_t words = fifo & PB_RX_FIFO_WC;
    if (!words) return 0;
    /* 最高位和字节数都在才算半个字（主线的判法）。HSUartDxe 不看最高位，只看 29:28，两者只在“最高位清零、字段 1..3”时不同，这个状态语义
     * 不明：这里按整字取，多出的补位在帧边界能跳过，少取了真字节却救不回来。记下次数，上机看它是不是真有。 */
    const uint32_t last = fifo >> PB_RX_FIFO_LAST_SHIFT & 7u;
    const uint32_t tail = (fifo & PB_RX_FIFO_LAST) && last && last < PB_BYTES_PER_WORD ? last : PB_BYTES_PER_WORD;
    if (!(fifo & PB_RX_FIFO_LAST) && (last & 3u)) g->rx_odd_tails++;
    const uint32_t fits = max / PB_BYTES_PER_WORD;
    const int partial = words <= fits && tail < PB_BYTES_PER_WORD;   /* 半个字只可能是这一批的最后一个 */
    if (words > fits) words = fits;
    uint32_t got = 0;
    for (uint32_t w = 0; w < words; ++w) {
        const uint32_t word = rd(g, PB_SE_RX_FIFO);
        const uint32_t take = partial && w + 1 == words ? tail : PB_BYTES_PER_WORD;
        for (uint32_t i = 0; i < take; ++i) out[got++] = (uint8_t)(word >> (8 * i));
    }
    g->rx_bytes += got;
    return got;
}
