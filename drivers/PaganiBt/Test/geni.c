/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtGeni.c 对 GENI 模型：探测不写寄存器、配置值、打包字节序、FIFO 补水、半个字、CTS 和 RFR、超时取消、错误位、回环。 */
#include "geni_model.h"
#include "check.h"

static GeniModel model;
static uint32_t io_read(void *c, uint32_t r) {return gm_read((GeniModel *)c, r);}
static void io_write(void *c, uint32_t r, uint32_t v) {gm_write((GeniModel *)c, r, v);}
static void io_delay(void *c, unsigned us) {GeniModel *m = (GeniModel *)c; gm_run(m, m->us + us);}
static void io_sleep(void *c, unsigned ms) {GeniModel *m = (GeniModel *)c; gm_run(m, m->us + ms * 1000ull);}
static uint64_t io_now(void *c) {return ((GeniModel *)c)->us / 1000;}
static const PbGeniIo kIo = {&model, io_read, io_write, io_delay, io_sleep, io_now};

static void open_uart(PbGeni *g, unsigned div)
{
    PbGeniInfo info;
    gm_init(&model, 64);
    CHECK(pb_geni_probe(g, &kIo, &info) == 0);
    CHECK(pb_geni_config(g, div, 0) == 0);
    CHECK(pb_geni_rx_start(g) == 0);
    pb_geni_flow(g, 1, 1);
}

static void probe(void)
{
    PbGeni g;
    PbGeniInfo info;
    gm_init(&model, 64);
    model.forbid_writes = 1;
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(info.proto == PB_PROTO_UART && info.version == 0x66 && info.tx_depth == 64 && info.rx_depth == 64);
    CHECK(g.tx_depth == 64 && !model.errors);

    /* 协议不是 UART：比如这个 SE 被配成 I2C。不写任何寄存器 */
    gm_init(&model, 64);
    model.forbid_writes = 1;
    model.regs[PB_SE_FW_REVISION / 4] = 3u << 8 | 0x66;
    CHECK(pb_geni_probe(&g, &kIo, &info) == PbGeniErrProto);
    CHECK(!g.tx_depth && !model.errors);
    CHECK(pb_geni_config(&g, 4, 0) == PbGeniErrArgument);
    /* FIFO 接口被关（只能走 GSI） */
    gm_init(&model, 64);
    model.forbid_writes = 1;
    model.regs[PB_SE_IF_DISABLE / 4] = 1;
    CHECK(pb_geni_probe(&g, &kIo, &info) == PbGeniErrNoFifo);
    CHECK(!model.errors);
    /* 深度读出来不像话时用默认 16 字：太大，太小（水线要减 8，不能下溢） */
    gm_init(&model, 0);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(info.tx_depth == 16 && info.rx_depth == 16);
    gm_init(&model, 255);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(info.tx_depth == 16);
    gm_init(&model, 8);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(info.rx_depth == 16);
}

static void configuration(void)
{
    PbGeni g;
    PbGeniInfo info;
    gm_init(&model, 64);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(pb_geni_config(&g, 0, 0) == PbGeniErrArgument);
    CHECK(pb_geni_config(&g, 4, 0) == 0);
    const uint32_t *r = model.regs;
    CHECK(r[PB_SE_TX_PACK0 / 4] == 0x4380e && r[PB_SE_TX_PACK1 / 4] == 0xc3e0e);
    CHECK(r[PB_SE_RX_PACK0 / 4] == 0x4380e && r[PB_SE_RX_PACK1 / 4] == 0xc3e0e);
    CHECK(r[PB_SE_DMA_MODE / 4] == 0);                  /* FIFO 模式 */
    CHECK(r[PB_SE_UART_TX_WORD_LEN / 4] == 8 && r[PB_SE_UART_RX_WORD_LEN / 4] == 8);
    CHECK(r[PB_SE_UART_TX_STOP_LEN / 4] == 0);
    CHECK(r[PB_SE_UART_TX_PARITY / 4] == 0 && r[PB_SE_UART_RX_PARITY / 4] == 0);
    CHECK(r[PB_SE_UART_RX_TRANS_CFG / 4] == 0);
    /* HSUartDxe sub_1F70 的值：TX 4，RX 深度减 8，RFR 深度减 4，stale 220 */
    CHECK(r[PB_SE_TX_WATERMARK / 4] == 4);
    CHECK(r[PB_SE_RX_WATERMARK / 4] == 56);
    CHECK(r[PB_SE_RFR_WATERMARK / 4] == 60);
    CHECK(r[PB_SE_UART_RX_STALE / 4] == 220);
    CHECK(r[PB_SE_UART_LOOPBACK / 4] == 0);
    /* 跟踪 "ser_clk_cfg:65"：分频 4 */
    CHECK(r[PB_SE_M_CLK_CFG / 4] == 65 && r[PB_SE_S_CLK_CFG / 4] == 65);
    CHECK(r[PB_SE_CLK_SEL / 4] == 0);
    CHECK(gm_baud(&model) == 115200);
    /* 收尾状态：跟踪 "Manual Flow Enabled, HW Flow OFF rfr = 0x80000002"，"trans_cfg2" */
    CHECK(r[PB_SE_UART_MANUAL_RFR / 4] == 0x80000002u);
    CHECK(r[PB_SE_UART_TX_TRANS_CFG / 4] == 2);
    CHECK(!model.s_active);
    pb_geni_flow(&g, 1, 1);
    CHECK(r[PB_SE_UART_MANUAL_RFR / 4] == 0 && r[PB_SE_UART_TX_TRANS_CFG / 4] == 0);
    CHECK(pb_geni_set_clock(&g, 1, 3) == 0);
    CHECK(r[PB_SE_M_CLK_CFG / 4] == 17 && r[PB_SE_CLK_SEL / 4] == 3);   /* "ser_clk_cfg:17" */
    CHECK(pb_geni_set_clock(&g, 0x1000, 0) == PbGeniErrArgument);

    /* 引导留下的命令要先停掉 */
    gm_init(&model, 64);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    model.m_active = 1;
    model.s_active = 1;
    model.regs[PB_SE_M_IRQ_EN / 4] = PB_IRQ_CANCEL;
    model.regs[PB_SE_S_IRQ_EN / 4] = PB_IRQ_CANCEL;
    CHECK(pb_geni_config(&g, 4, 0) == 0);
    CHECK(model.m_cancels == 1 && model.s_cancels == 1 && !model.m_active && !model.s_active);
}

static void transmit(void)
{
    PbGeni g;
    open_uart(&g, 4);
    uint8_t data[1000];
    for (unsigned i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i * 7 + 1);
    /* 各种长度：字节顺序对、不多不少，最后半个字靠 TRANS_LEN 截断 */
    static const uint32_t lengths[] = {1, 2, 3, 4, 5, 7, 8, 251, 257, 1000};
    uint32_t total = 0;
    for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        const uint32_t start = model.wire_len;
        CHECK(pb_geni_write(&g, data, lengths[i], 2000) == 0);
        CHECK(model.wire_len - start == lengths[i]);
        CHECK(!memcmp(model.wire + start, data, lengths[i]));
        total += lengths[i];
    }
    CHECK(g.tx_bytes == total && !model.errors);
    CHECK(pb_geni_write(&g, data, 0, 10) == PbGeniErrArgument);
    /* 时间合理：115200 下 1000 字节约 87 ms */
    const uint64_t t0 = model.us;
    CHECK(pb_geni_write(&g, data, 1000, 2000) == 0);
    CHECK(model.us - t0 > 85000 && model.us - t0 < 100000);

    /* 8 Mbps */
    model.rcg_hz = 128000000;
    CHECK(pb_geni_set_clock(&g, 1, 0) == 0);
    CHECK(gm_baud(&model) == 8000000);
    const uint32_t start = model.wire_len;
    CHECK(pb_geni_write(&g, data, 1000, 2000) == 0);
    CHECK(!memcmp(model.wire + start, data, 1000));
}

static void flow(void)
{
    PbGeni g;
    open_uart(&g, 4);
    const uint8_t fd = 0xfd;
    CHECK(!(pb_geni_ios(&g) & PB_IOS_CTS));             /* 跟踪：geni_ios:0x1 */
    /* 对端不放 CTS：10 ms 窗口内放弃，命令被取消，什么都没上线 */
    model.cts_blocked = 1;
    CHECK(pb_geni_ios(&g) & PB_IOS_CTS);
    uint32_t start = model.wire_len;
    CHECK(pb_geni_write(&g, &fd, 1, 10) == PbGeniErrTimeout);
    CHECK(model.wire_len == start && model.m_cancels == 1 && !model.m_active && g.tx_timeouts == 1);
    /* 放开后下一次正常 */
    model.cts_blocked = 0;
    CHECK(pb_geni_write(&g, &fd, 1, 10) == 0);
    CHECK(model.wire_len == start + 1 && model.wire[start] == 0xfd);
    /* 大包写到一半 CTS 收回：FIFO 满了等，收回时间内写不完就超时，不会写爆 FIFO */
    uint8_t big[2000];
    memset(big, 0x5a, sizeof big);
    model.cts_blocked = 1;
    CHECK(pb_geni_write(&g, big, sizeof big, 50) == PbGeniErrTimeout);
    CHECK(!model.errors);
    model.cts_blocked = 0;
    /* CTS 屏蔽时不看对端（配置收尾的状态） */
    pb_geni_flow(&g, 0, 1);
    model.cts_blocked = 1;
    CHECK(pb_geni_write(&g, &fd, 1, 10) == 0);
    model.cts_blocked = 0;
    pb_geni_flow(&g, 1, 1);

    /* cancel 不理：走 abort */
    model.cts_blocked = 1;
    model.cancel_stuck = 1;
    const uint32_t aborts = model.m_aborts;
    CHECK(pb_geni_write(&g, &fd, 1, 10) == PbGeniErrTimeout);
    CHECK(model.m_aborts == aborts + 1 && !model.m_active);
    /* abort 也不理：报告出来，不假装成功 */
    model.abort_stuck = 1;
    CHECK(pb_geni_write(&g, &fd, 1, 10) == PbGeniErrCancel);
    CHECK(pb_geni_write(&g, &fd, 1, 10) == PbGeniErrBusy);   /* 命令还占着，不往里写 */
    model.cancel_stuck = model.abort_stuck = 0;
    model.cts_blocked = 0;
    model.m_active = 0;
    model.tx_count = 0;

    /* 错误位：记下来，返回总线错误，不当成发完 */
    open_uart(&g, 4);
    model.inject_m_error = PB_IRQ_FAILURE;
    CHECK(pb_geni_write(&g, &fd, 1, 100) == PbGeniErrBus);
    CHECK(g.m_sticky & PB_IRQ_FAILURE);
    CHECK(pb_geni_write(&g, &fd, 1, 100) == 0);
    model.inject_m_error = PB_IRQ_ILLEGAL;
    CHECK(pb_geni_write(&g, &fd, 1, 100) == PbGeniErrBus);
}

static void receive(void)
{
    PbGeni g;
    uint8_t out[300];
    open_uart(&g, 4);
    CHECK(pb_geni_read(&g, out, sizeof out) == 0);

    /* 一个孤立的 0xfc：等 stale（220 比特时间，约 1.9 ms）才出来，还在 10 ms 唤醒窗口里 */
    const uint8_t fc = 0xfc;
    gm_peer(&model, &fc, 1);
    gm_run(&model, model.us + 200);
    CHECK(pb_geni_read(&g, out, sizeof out) == 0);
    gm_run(&model, model.us + 2000);
    CHECK(pb_geni_read(&g, out, sizeof out) == 1 && out[0] == 0xfc);
    CHECK(pb_geni_read(&g, out, sizeof out) == 0);

    /* 整字加半个字 */
    static const uint8_t event[7] = {0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00};
    gm_peer(&model, event, sizeof event);
    gm_run(&model, model.us + 3000);
    uint32_t got = pb_geni_read(&g, out, sizeof out);
    CHECK(got == 7 && !memcmp(out, event, 7));

    /* 长包：分几次取，字节不丢不重 */
    uint8_t data[600];
    for (unsigned i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i ^ 0xa5);
    gm_peer(&model, data, sizeof data);
    uint8_t in[700];
    got = 0;
    for (unsigned spin = 0; spin < 2000 && got < sizeof data; ++spin) {
        gm_run(&model, model.us + 500);
        got += pb_geni_read(&g, in + got, (uint32_t)sizeof in - got < 64 ? (uint32_t)sizeof in - got : 64);
    }
    CHECK(got == sizeof data && !memcmp(in, data, sizeof data));
    CHECK(g.rx_bytes == 1 + 7 + sizeof data);
    /* max 不够一个字：不取 */
    gm_peer(&model, event, 5);
    gm_run(&model, model.us + 5000);
    CHECK(pb_geni_read(&g, out, 3) == 0);
    CHECK(pb_geni_read(&g, out, sizeof out) == 5);

    /* 主机迟迟不读：RFR 水线拦住对端，FIFO 不溢出，字节全在 */
    gm_peer(&model, data, sizeof data);
    gm_run(&model, model.us + 200000);
    CHECK(model.rx_count <= 60 && !model.rx_dropped);
    CHECK(model.peer_head < model.peer_len);
    got = 0;
    for (unsigned spin = 0; spin < 2000 && got < sizeof data; ++spin) {
        got += pb_geni_read(&g, in + got, (uint32_t)sizeof in - got);
        gm_run(&model, model.us + 500);
    }
    CHECK(got == sizeof data && !memcmp(in, data, sizeof data));
    CHECK(!model.errors && !model.rx_dropped);

    /* RX_LAST 的判法：最高位和字节数都在才算半个字。字节数是 0 或 4 时整字有效（主线 get_char 的注释）；最高位清零、字段 1..3 时按整字取
     * 并记一笔，HSUartDxe 这时只取字段那么多字节 */
    static const uint8_t four[4] = {1, 2, 3, 4};
    static const uint32_t whole[] = {PB_RX_FIFO_LAST, PB_RX_FIFO_LAST | 4u << PB_RX_FIFO_LAST_SHIFT, 4u << PB_RX_FIFO_LAST_SHIFT, 0};
    for (unsigned i = 0; i < sizeof whole / sizeof whole[0]; ++i) {
        gm_peer(&model, four, 4);
        gm_run(&model, model.us + 3000);
        model.rx_status_or = whole[i];
        CHECK(pb_geni_read(&g, out, sizeof out) == 4 && !memcmp(out, four, 4));
    }
    CHECK(g.rx_odd_tails == 0);
    gm_peer(&model, four, 4);
    gm_run(&model, model.us + 3000);
    model.rx_status_or = 2u << PB_RX_FIFO_LAST_SHIFT;
    CHECK(pb_geni_read(&g, out, sizeof out) == 4 && !memcmp(out, four, 4));
    CHECK(g.rx_odd_tails == 1);
    model.rx_status_or = 0;

    /* 半个字没读走又收满一个字：RX_LAST 说不清了，补位混进字节流，H4 层在帧边界跳过（h4.c）。这是轮询的已知代价，这里钉住行为 */
    gm_peer(&model, &fc, 1);
    gm_run(&model, model.us + 3000);
    CHECK(model.rx_count == 1 && model.rx_last_bytes == 1);
    gm_peer(&model, four, 4);
    gm_run(&model, model.us + 1000);
    got = pb_geni_read(&g, out, sizeof out);
    CHECK(got == 8 && out[0] == 0xfc && !out[1] && !out[2] && !out[3] && !memcmp(out + 4, four, 4));

    /* 手动 RFR 未就绪：对端发不进来，换速时靠它 */
    pb_geni_flow(&g, 1, 0);
    gm_peer(&model, event, sizeof event);
    gm_run(&model, model.us + 10000);
    CHECK(model.peer_head < model.peer_len);
    pb_geni_flow(&g, 1, 1);
    gm_run(&model, model.us + 10000);
    CHECK(pb_geni_read(&g, out, sizeof out) == 7);

    /* 接收错误位累计，不影响取数 */
    model.s_irq |= PB_IRQ_RX_WR_ERR;
    (void)pb_geni_read(&g, out, sizeof out);
    CHECK(g.s_sticky & PB_IRQ_RX_WR_ERR);

    /* 停接收：cancel；不理时 abort；没启动时不动作 */
    CHECK(pb_geni_rx_stop(&g) == 0 && model.s_cancels == 1 && !model.s_active);
    CHECK(pb_geni_rx_stop(&g) == 0 && model.s_cancels == 1);
    CHECK(pb_geni_read(&g, out, sizeof out) == 0);
    CHECK(pb_geni_rx_start(&g) == 0 && model.s_active);
    CHECK(pb_geni_rx_start(&g) == 0);                   /* 重复启动不重发命令，模型会记错 */
    model.s_cancel_stuck = 1;
    CHECK(pb_geni_rx_stop(&g) == 0 && model.s_aborts == 1);
    CHECK(!model.errors);
}

static void loopback(void)
{
    PbGeni g;
    PbGeniInfo info;
    gm_init(&model, 64);
    CHECK(pb_geni_probe(&g, &kIo, &info) == 0);
    CHECK(pb_geni_config(&g, 4, 0) == 0);
    pb_geni_loopback(&g, 1);
    CHECK(model.regs[PB_SE_UART_LOOPBACK / 4] == 3);
    pb_geni_flow(&g, 0, 1);
    CHECK(pb_geni_rx_start(&g) == 0);
    static const uint8_t probe_bytes[8] = {0x55, 0xaa, 0x31, 0x00, 0xf0, 0xff, 0x02, 0x05};
    model.cts_blocked = 1;                              /* 回环时 RFR 接 CTS，外面的 CTS 不算数 */
    CHECK(pb_geni_write(&g, probe_bytes, sizeof probe_bytes, 100) == 0);
    gm_run(&model, model.us + 3000);
    uint8_t out[16];
    CHECK(pb_geni_read(&g, out, sizeof out) == sizeof probe_bytes);
    CHECK(!memcmp(out, probe_bytes, sizeof probe_bytes));
    pb_geni_loopback(&g, 0);
    CHECK(model.regs[PB_SE_UART_LOOPBACK / 4] == 0);
}

int main(void)
{
    probe();
    configuration();
    transmit();
    flow();
    receive();
    loopback();
    printf("PASS geni: %u checks\n", checks);
    return 0;
}
