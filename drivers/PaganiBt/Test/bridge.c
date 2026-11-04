/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtBridge.c 对假 bluetoothd 和控制器模型，单线程，时间由测试拨。
 *
 * 假 bluetoothd 按真 bluetoothd 的样子说话，这些都来自反编译（0x1008d4640 的初始化表、sub_1000DE46C 的写、sub_1000DF6E4 的 H4 接收）：
 *   - 26 步初始化表里实际发的 24 条命令，一次一条，等到应答才发下一条，命令号由描述符表 0x100925dc0 核对过；
 *   - 写是流：测试把一包切成任意段，也把几包粘在一起；
 *   - 收：第一个字节只认 1..4，长度上限 0x7f1，帧被截断或混进别的字节就算失败。
 * 控制器模型用替身的表当应答（值来自 Android 跟踪），额外能插控制器自己的日志、0xff 事件、0x34、IBS 字节。 */
#include "../BtBridge.h"
#include "../BtStandIn.h"
#include "../BtTlv.h"
#include "check.h"

#define BUF (1u << 21)
static const uint8_t kAddress[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
/* 初始化表的顺序，HCI 版本 0x0e、带 LE 的控制器走到的都在这里。 */
static const uint16_t kInit[] = {
    0x0c03, 0x1001, 0x1002, 0x1009, 0x1003, 0x1004, 0x2003, 0x0c01, 0x0c63, 0x2001, 0x0c56, 0x0c6d, 0x2060, 0x202f, 0x201c, 0x200f, 0x1005, 0x204a,
    0x2074, 0x2074, 0x207d, 0x0c33, 0x0c31, 0x0c5b,
};
#define INIT_COUNT (sizeof kInit / sizeof kInit[0])

typedef struct {
    PbBridge b;
    PbBridgeIo io;
    uint64_t now;
    int locked;
    /* 串口：写进来的字节，和等着被读的字节 */
    uint8_t wire[BUF];
    uint32_t wire_n, writes;
    uint8_t from_ctrl[BUF];
    uint32_t ctrl_head, ctrl_tail, reads;
    int mute, write_fails, answers_acl;
    uint32_t acks_on_wire;
    /* 套接字 */
    uint8_t sock[BUF];
    uint32_t sock_head, sock_tail, sock_limit, puts, put_refused;
    int sock_gone;
    /* 其他 */
    uint32_t kicks, waits, wakes, logs;
    int drain_in_wait;
    char last_log[160];
} World;

static World *w;

static void t_lock(void *c, int take)
{
    (void)c;
    CHECK(take ? !w->locked : w->locked);    /* 不重入，不多放 */
    w->locked = take;
}
static uint64_t t_now(void *c) {(void)c; return w->now;}

static void ctrl_out(const uint8_t *data, uint32_t bytes)
{
    CHECK(w->ctrl_tail + bytes <= BUF);
    memcpy(w->from_ctrl + w->ctrl_tail, data, bytes);
    w->ctrl_tail += bytes;
}
static int t_uart_write(void *c, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    (void)c;
    CHECK(!w->locked);                       /* 串口绝不在锁里 */
    CHECK(timeout_ms == PbBridgeWriteMs);
    if (w->write_fails) return -1;
    CHECK(w->wire_n + bytes <= BUF);
    memcpy(w->wire + w->wire_n, data, bytes);
    w->wire_n += bytes;
    w->writes++;
    if (data[0] == PbIbsWakeAck && bytes == 1) w->acks_on_wire++;
    if (w->mute) return 0;
    if (data[0] == PbH4Cmd) {
        const uint16_t op = (uint16_t)(data[1] | data[2] << 8);
        if (op == PbOpHostCompleted) return 0;
        uint8_t reply[PbStandInMax];
        ctrl_out(reply, pb_standin_reply(data, bytes, kAddress, reply));
    } else if (data[0] == PbH4Acl && w->answers_acl) {
        ctrl_out(data, bytes);               /* 回环一包 */
    }
    return 0;
}
static uint32_t t_uart_read(void *c, uint8_t *data, uint32_t max)
{
    (void)c;
    CHECK(!w->locked);
    CHECK(max >= 256);
    uint32_t n = w->ctrl_tail - w->ctrl_head;
    if (n > max) n = max;
    memcpy(data, w->from_ctrl + w->ctrl_head, n);
    w->ctrl_head += n;
    w->reads += n != 0;
    return n;
}
static int t_put(void *c, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    (void)c;
    (void)unit;
    CHECK(w->locked);                        /* client_put 在锁里 */
    w->puts++;
    if (w->sock_gone) return -1;
    if (w->sock_tail - w->sock_head + bytes > w->sock_limit) {
        w->put_refused++;
        return 1;
    }
    memcpy(w->sock + w->sock_tail, data, bytes);
    w->sock_tail += bytes;
    return 0;
}
static void t_kick(void *c) {(void)c; CHECK(w->locked); w->kicks++;}
static void t_wait(void *c, unsigned ms)
{
    (void)c;
    CHECK(w->locked);
    w->waits++;
    w->now += ms;
    if (w->drain_in_wait) {
        w->locked = 0;
        (void)pb_bridge_step(&w->b);
        w->locked = 1;
    }
}
static void t_wake(void *c) {(void)c; CHECK(w->locked); w->wakes++;}
static void t_log(void *c, const char *line)
{
    (void)c;
    CHECK(!w->locked);                       /* 日志绝不在锁里 */
    w->logs++;
    snprintf(w->last_log, sizeof w->last_log, "%s", line);
}

static void reset_world(int stand_in)
{
    memset(w, 0, sizeof *w);
    w->io = (PbBridgeIo){w, t_lock, t_now, t_uart_write, t_uart_read, t_put, t_kick, t_wait, t_wake, t_log};
    w->now = 1000;
    w->sock_limit = BUF;
    pb_bridge_init(&w->b, &w->io, kAddress, stand_in);
}
static void bring_up(void)
{
    pb_bridge_up(&w->b, NULL, NULL, 0);
    CHECK(w->b.link == PbLinkUp);
}

/* 跑几步：sleep 了多久时间就走多久。 */
static void run(unsigned steps)
{
    for (unsigned i = 0; i < steps; ++i) {
        const int ms = pb_bridge_step(&w->b);
        if (ms < 0) return;
        w->now += (unsigned)ms;
    }
}
/* 假 bluetoothd 的写：seed 决定切法。0：整包一次写完。 */
static uint32_t split_seed;
static void client_write(uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    uint32_t at = 0;
    while (at < bytes) {
        uint32_t piece = bytes - at;
        if (split_seed) {
            split_seed = split_seed * 1103515245u + 12345u;
            const uint32_t pick = (split_seed >> 16) % 7;
            if (pick + 1 < piece) piece = pick + 1;
        }
        pb_bridge_write(&w->b, unit, data + at, piece);
        at += piece;
    }
}
static uint32_t command(uint8_t *out, uint16_t opcode)
{
    /* 参数的内容模型不看，长度对就行 */
    const uint8_t plen = opcode == 0x0c01 || opcode == 0x0c63 || opcode == 0x2001 ? 8 : opcode == 0x1004 ? 1 : opcode == 0x0c56 ? 1 : opcode == 0x0c6d ? 2 :
                         opcode == 0x2074 ? 2 : opcode == 0x207d ? 10 : opcode == 0x0c33 ? 7 : opcode == 0x0c31 ? 1 : opcode == 0x0c5b ? 1 : 0;
    uint8_t params[16];
    memset(params, 0, sizeof params);
    return pb_hci_command(out, opcode, params, plen);
}
/* 假 bluetoothd 的收：sub_1000DF6E4 的规矩。取一帧到 f，返回长度，0 没有完整的帧。 */
static uint32_t client_read(uint8_t *f)
{
    const uint32_t have = w->sock_tail - w->sock_head;
    if (!have) return 0;
    const uint8_t *p = w->sock + w->sock_head;
    CHECK(p[0] >= 1 && p[0] <= 4);           /* 其他类型字节 bluetoothd 进重同步 */
    uint32_t total;
    if (p[0] == PbH4Evt) {
        if (have < 3) return 0;
        total = 3u + p[2];
    } else if (p[0] == PbH4Acl) {
        if (have < 5) return 0;
        total = 5u + (uint32_t)(p[3] | (uint32_t)p[4] << 8);
    } else {
        if (have < 4) return 0;
        total = 4u + p[3];
    }
    CHECK(total < 0x7f1);
    if (have < total) return 0;
    memcpy(f, p, total);
    w->sock_head += total;
    return total;
}
/* 发一条命令，等应答，检查是这条命令的 Command Complete。 */
static void init_step(uint32_t unit, uint16_t opcode)
{
    uint8_t p[PbPacketMax], f[PbH4FrameMax];
    client_write(unit, p, command(p, opcode));
    uint32_t n = 0;
    for (unsigned i = 0; i < 8 && !n; ++i) {
        n = client_read(f);
        if (!n) run(1);
    }
    uint16_t answered = 0;
    uint8_t status = 0xff;
    CHECK(n && pb_hci_complete(f, n, &answered, &status, NULL));
    CHECK(answered == opcode && status == 0);
}

static void init_sequence(void)
{
    static const uint32_t seeds[] = {0, 1, 2, 3, 77, 1234};
    for (unsigned s = 0; s < sizeof seeds / sizeof seeds[0]; ++s) {
        reset_world(0);
        bring_up();
        split_seed = seeds[s];
        CHECK(pb_bridge_connect(&w->b, 7, 321) == PbBridgeOk);
        for (unsigned i = 0; i < INIT_COUNT; ++i) init_step(7, kInit[i]);
        CHECK(w->b.ready == 1 && !w->b.guard && w->b.vsc_kinds == 0);
        CHECK(w->b.packets_out == INIT_COUNT && w->b.from_client.packets == INIT_COUNT);
        /* 串口上的字节和写的逐字节相同，顺序不变 */
        uint32_t at = 0;
        for (unsigned i = 0; i < INIT_COUNT; ++i) {
            uint8_t p[PbPacketMax];
            const uint32_t n = command(p, kInit[i]);
            CHECK(!memcmp(w->wire + at, p, n));
            at += n;
        }
        CHECK(at == w->wire_n);
    }
    /* 粘在一起：一次写进三包，一个命令夹在两个 ACL 中间 */
    reset_world(0);
    bring_up();
    split_seed = 0;
    CHECK(pb_bridge_connect(&w->b, 7, 321) == PbBridgeOk);
    uint8_t glue[64];
    const uint8_t acl[] = {PbH4Acl, 0x01, 0x20, 2, 0, 0xaa, 0xbb};
    memcpy(glue, acl, sizeof acl);
    const uint32_t cmd = command(glue + sizeof acl, 0x1001);
    memcpy(glue + sizeof acl + cmd, acl, sizeof acl);
    client_write(7, glue, 2 * sizeof acl + cmd);
    run(4);
    CHECK(w->wire_n == 2 * sizeof acl + cmd && !memcmp(w->wire, glue, w->wire_n));
    CHECK(w->b.packets_out == 3);
}

static void vsc_instant(void)
{
    /* 高通的下载、NVM、写地址和博通的号共用这段空间，一条都不能到串口 */
    static const uint16_t vendor[] = {0xfc00, 0xfc0b, 0xfc14, 0xfc1d, 0xfc4d, 0xfd53, 0xfd5f, 0xfd57, 0xffff};
    static const int links[] = {PbLinkStarting, PbLinkUp, PbLinkStandIn};
    for (unsigned l = 0; l < 3; ++l) {
        reset_world(links[l] == PbLinkStandIn);
        if (links[l] == PbLinkUp) bring_up();
        w->mute = 1;                         /* 控制器不答也照回 */
        CHECK(pb_bridge_connect(&w->b, 9, 5) == PbBridgeOk);
        for (unsigned i = 0; i < sizeof vendor / sizeof vendor[0]; ++i) {
            uint8_t p[8], f[32];
            const uint32_t n = pb_hci_command(p, vendor[i], (const uint8_t *)"\x01", 1);
            pb_bridge_write(&w->b, 9, p, n);     /* 不跑工作线程 */
            const uint32_t got = client_read(f);
            CHECK(got == 7);
            uint16_t op = 0;
            uint8_t status = 0;
            CHECK(pb_hci_complete(f, got, &op, &status, NULL) && op == vendor[i] && status == 1);
        }
        CHECK(w->b.vsc_kinds == sizeof vendor / sizeof vendor[0] && w->b.vsc_answered == w->b.vsc_kinds);
        run(3);
        CHECK(w->wire_n == 0 && w->b.packets_out == 0);
        /* 同一条再来：次数加，种类不加 */
        uint8_t p[8];
        pb_bridge_write(&w->b, 9, p, pb_hci_command(p, 0xfc4d, NULL, 0));
        CHECK(w->b.vsc_answered == 1 + sizeof vendor / sizeof vendor[0]);
        CHECK(w->b.vsc_kinds == sizeof vendor / sizeof vendor[0]);
    }
}

static void hold_until_up(void)
{
    reset_world(0);
    CHECK(pb_bridge_connect(&w->b, 3, 44) == PbBridgeOk);
    uint8_t p[PbPacketMax], f[PbH4FrameMax];
    pb_bridge_write(&w->b, 3, p, command(p, PbOpReset));
    CHECK(pb_bridge_step(&w->b) == PbBridgeIdlePollMs);   /* 没起来：不碰串口 */
    CHECK(w->wire_n == 0 && w->reads == 0);
    /* 下载路径读多了：一帧的前半截在成帧状态里，后半截在 rest 里，再加一帧 */
    static PbH4Rx rx;
    pb_h4_rx_reset(&rx);
    const uint8_t first[] = {0x04, 0x0e, 0x04, 0x01};
    for (unsigned i = 0; i < sizeof first; ++i) CHECK(pb_h4_rx_byte(&rx, first[i]) == PbH4More);
    const uint8_t rest[] = {0x00, 0x00, 0x00, 0x04, 0x0e, 0x04, 0x01, 0x01, 0x10, 0x00};
    pb_bridge_up(&w->b, &rx, rest, sizeof rest);
    CHECK(client_read(f) == 7 && f[4] == 0 && f[5] == 0);
    CHECK(client_read(f) == 7 && f[4] == 0x01 && f[5] == 0x10);
    run(2);
    CHECK(w->wire_n == 4 && !memcmp(w->wire, p, 4));    /* 留着的命令现在才发 */
    CHECK(client_read(f) == 7 && f[4] == 0x03 && f[5] == 0x0c);
    /* 第二次 up 不算数 */
    w->b.frames_in = 0;
    pb_bridge_up(&w->b, NULL, rest, sizeof rest);
    CHECK(w->b.frames_in == 0);
}

static void flow_control(void)
{
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    /* 控制器连发 4000 帧 ACL，长度 200，内容是序号。套接字只放得下 3 帧 */
    enum {kFrames = 4000, kBytes = 205};
    for (uint32_t i = 0; i < kFrames; ++i) {
        uint8_t f[kBytes];
        memset(f, (int)(i & 0xff), sizeof f);
        f[0] = PbH4Acl;
        f[1] = 0x40;
        f[2] = 0x20;
        f[3] = kBytes - 5;
        f[4] = 0;
        f[5] = (uint8_t)i;
        f[6] = (uint8_t)(i >> 8);
        ctrl_out(f, sizeof f);
    }
    w->sock_limit = 3 * kBytes;
    run(40);
    CHECK(w->put_refused > 0 && w->b.rx_paused > 0);    /* 客户端不读：环满了就不再取串口 */
    CHECK(w->ctrl_head < w->ctrl_tail);                  /* 字节留在串口上，等 RFR */
    CHECK(w->b.up_full == 0);
    uint32_t got = 0;
    for (unsigned spin = 0; spin < 200000 && got < kFrames; ++spin) {
        uint8_t f[PbH4FrameMax];
        const uint32_t n = client_read(f);
        if (!n) {
            run(1);
            continue;
        }
        CHECK(n == kBytes && f[5] == (uint8_t)got && f[6] == (uint8_t)(got >> 8));
        got++;
    }
    CHECK(got == kFrames && w->b.frames_up == kFrames);
    /* 一帧不能被切开：ENOBUFS 时整帧留着 */
    CHECK(w->sock_head == w->sock_tail);
    /* 交接时塞进来的字节没有 PbBridgeReadRoom 管着：环满了之后的帧丢掉计数，环里的内容不乱 */
    reset_world(0);
    w->sock_limit = 0;
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    enum {kMany = 1000};
    static uint8_t many[kMany * 64];
    for (uint32_t i = 0; i < kMany; ++i) {
        uint8_t *f = many + i * 64;
        f[0] = PbH4Evt;
        f[1] = 0x3e;
        f[2] = 61;
        f[3] = (uint8_t)i;
        f[4] = (uint8_t)(i >> 8);
    }
    pb_bridge_up(&w->b, NULL, many, sizeof many);
    const uint32_t kept = (PbBridgeRingBytes) / 66;
    CHECK(w->b.up_full == kMany - kept);
    w->sock_limit = BUF;
    run(1);
    for (uint32_t i = 0; i < kept; ++i) {
        uint8_t f[PbH4FrameMax];
        CHECK(client_read(f) == 64 && f[3] == (uint8_t)i && f[4] == (uint8_t)(i >> 8));
    }
    uint8_t rest[8];
    CHECK(client_read(rest) == 0);
}

static void client_stuck(void)
{
    /* 客户端写了不读：上行环满了以后厂商命令的应答等一会儿就丢，线程不卡死 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    w->sock_limit = 0;
    uint8_t p[8];
    const uint32_t n = pb_hci_command(p, 0xfc4d, NULL, 0);
    const uint32_t fit = PbBridgeRingBytes / 9;
    for (uint32_t i = 0; i < fit + 3; ++i) pb_bridge_write(&w->b, 1, p, n);
    CHECK(w->b.tx_dropped >= 1 && w->waits > 0);
    /* 客户端走了：环清掉 */
    pb_bridge_disconnect(&w->b, 1);
    CHECK(w->b.to_client.head == w->b.to_client.tail);
    /* put 返回 -1：套接字已经没了，丢掉，不重试 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    w->sock_gone = 1;
    pb_bridge_write(&w->b, 1, p, n);
    CHECK(w->b.put_lost == 1 && w->b.to_client.head == w->b.to_client.tail);
}

static void tx_full(void)
{
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    /* 塞满发送环：工作线程在等待时才跑 */
    w->drain_in_wait = 1;
    uint8_t acl[1029];
    memset(acl, 0x5a, sizeof acl);
    acl[0] = PbH4Acl;
    acl[1] = 1;
    acl[2] = 0;
    acl[3] = 0;
    acl[4] = 4;
    enum {kCount = 60};
    for (unsigned i = 0; i < kCount; ++i) pb_bridge_write(&w->b, 1, acl, sizeof acl);
    run(40);
    CHECK(w->waits > 0 && w->wakes > 0 && w->b.packets_out == kCount && w->b.tx_dropped == 0);
    CHECK(w->wire_n == kCount * sizeof acl);
    /* 工作线程不动：等到上限就丢这一包，客户端线程回来 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    for (unsigned i = 0; i < 16; ++i) pb_bridge_write(&w->b, 1, acl, sizeof acl);
    CHECK(w->b.tx_dropped >= 1 && w->now >= 1000 + PbBridgeRoomMs);
    /* 超大的 ACL：不进环，流接得上 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    static uint8_t huge[5 + 0x1000];
    memset(huge, 0x33, sizeof huge);
    huge[0] = PbH4Acl;
    huge[3] = 0x00;
    huge[4] = 0x10;
    pb_bridge_write(&w->b, 1, huge, sizeof huge);
    CHECK(w->b.from_client.oversize == 1);
    uint8_t p[PbPacketMax];
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    run(2);
    CHECK(w->wire_n == 4 && !memcmp(w->wire, p, 4));
    /* 不认识的类型字节：跳过，后面的包照常 */
    w->wire_n = 0;
    const uint8_t junk[] = {0x00, 0x05, 0xff};
    pb_bridge_write(&w->b, 1, junk, sizeof junk);
    CHECK(w->b.from_client.junk == 3);
    pb_bridge_write(&w->b, 1, p, 4);
    run(2);
    CHECK(w->wire_n == 4);
}

static void filters(void)
{
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    uint8_t f[PbH4FrameMax];
    const uint8_t log[] = {0x02, 0xdc, 0x2e, 0x04, 0x00, 1, 2, 3, 4};
    const uint8_t vendor[] = {0x04, 0xff, 0x02, 0x01, 0x02};
    const uint8_t peri[] = {0x34, 0x00, 0xff, 0x04, 0xf0, 0x03, 0x00, 0x00};
    const uint8_t iso[] = {0x05, 0x01, 0x00, 0x01, 0x00, 0xaa};
    const uint8_t pad[] = {0x00, 0x00, 0x00};
    const uint8_t cc[] = {0x04, 0x0e, 0x04, 0x01, 0x01, 0x20, 0x00};
    ctrl_out(log, sizeof log);
    ctrl_out(vendor, sizeof vendor);
    ctrl_out(peri, sizeof peri);
    ctrl_out(iso, sizeof iso);
    ctrl_out(pad, sizeof pad);
    ctrl_out(cc, sizeof cc);
    run(2);
    CHECK(client_read(f) == sizeof cc && !memcmp(f, cc, sizeof cc));
    CHECK(client_read(f) == 0);
    CHECK(w->b.frames_dropped == 3 && w->b.frames_bad == 1 && w->b.rx.junk == 3);
    /* IBS：对端唤醒回 0xfc，别的什么都不发 */
    const uint8_t wake[] = {PbIbsWake};
    ctrl_out(wake, 1);
    run(1);
    CHECK(w->acks_on_wire == 1 && w->wire_n == 1 && w->wire[0] == PbIbsWakeAck);
    const uint8_t sleep_byte[] = {PbIbsSleep, PbIbsWakeAck};
    ctrl_out(sleep_byte, 2);
    run(1);
    CHECK(w->acks_on_wire == 1 && w->wire_n == 1 && w->b.ibs.sleeps_seen == 1);
    /* 没有客户端：帧丢掉，IBS 照回 */
    pb_bridge_disconnect(&w->b, 1);
    run(2);
    w->wire_n = 0;
    ctrl_out(cc, sizeof cc);
    ctrl_out(wake, 1);
    run(2);
    CHECK(w->b.frames_no_client >= 1 && w->sock_head == w->sock_tail);
    CHECK(w->acks_on_wire == 2);
}

static void connections(void)
{
    reset_world(0);
    bring_up();
    uint8_t p[PbPacketMax], f[PbH4FrameMax];
    CHECK(pb_bridge_connect(&w->b, 1, 100) == PbBridgeOk);
    CHECK(pb_bridge_connect(&w->b, 2, 200) == PbBridgeBusy);
    /* 第二个的 disconnect 和写都不影响第一个 */
    pb_bridge_disconnect(&w->b, 2);
    CHECK(w->b.client == 1 && w->b.pid == 100);
    pb_bridge_write(&w->b, 2, p, command(p, 0x1001));
    CHECK(w->b.from_client.packets == 0);
    for (unsigned i = 0; i < INIT_COUNT; ++i) init_step(1, kInit[i]);
    /* 没发出去的包在断开时作废，发一条自己的 HCI_Reset，应答不给下一个客户端 */
    w->wire_n = 0;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    pb_bridge_disconnect(&w->b, 1);
    CHECK(w->b.failed_starts == 0);
    CHECK(pb_bridge_connect(&w->b, 3, 300) == PbBridgeOk);
    run(3);
    CHECK(w->wire_n == 4 && !memcmp(w->wire, "\x01\x03\x0c\x00", 4));
    CHECK(client_read(f) == 0);
    /* 下一个客户端自己的 HCI_Reset 应答要上交 */
    CHECK(w->b.client && !w->b.ready);
    init_step(3, PbOpReset);
    /* 没走完初始化就走，计一次；走完之后清零 */
    pb_bridge_disconnect(&w->b, 3);
    CHECK(w->b.failed_starts == 1);
    run(2);
    CHECK(pb_bridge_connect(&w->b, 4, 400) == PbBridgeOk);
    for (unsigned i = 0; i < INIT_COUNT; ++i) init_step(4, kInit[i]);
    CHECK(w->b.failed_starts == 0);
    pb_bridge_disconnect(&w->b, 4);
    CHECK(w->b.failed_starts == 0);
}

static void trips(unsigned expected, int kill_pid)
{
    CHECK(w->b.guard == expected && w->b.link == PbLinkDead);
    CHECK(w->b.kill_pid == kill_pid);
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    CHECK(pb_bridge_connect(&w->b, 50, 500) == PbBridgeRefused && w->b.refused >= 1);
    CHECK(strstr(w->last_log, "refused") != NULL);
}

static void guards(void)
{
    uint8_t p[PbPacketMax];
    /* 上电失败：连着的进程要杀 */
    reset_world(0);
    CHECK(pb_bridge_connect(&w->b, 1, 2222) == PbBridgeOk);
    pb_bridge_fail(&w->b);
    trips(PbGuardBringup, 2222);
    /* 没人连着：不杀 */
    reset_world(0);
    pb_bridge_fail(&w->b);
    trips(PbGuardBringup, 0);

    /* 命令没有应答：到点才 trip，差 1 ms 不算 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    w->mute = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    CHECK(pb_bridge_step(&w->b) == PbBridgeBusyPollMs);
    CHECK(w->b.pending == 1);
    w->now = w->b.pending_at + PbBridgeCommandMs - 1;
    CHECK(pb_bridge_step(&w->b) >= 0 && !w->b.guard);
    w->now += 1;
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardCommand, 77);
    CHECK(strstr(w->last_log, "refused") != NULL);

    /* Host_Number_Of_Completed_Packets 规范上没有应答，不算欠账 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    const uint8_t params[5] = {1, 0x40, 0x00, 0x01, 0x00};
    pb_bridge_write(&w->b, 1, p, pb_hci_command(p, PbOpHostCompleted, params, sizeof params));
    run(2);
    CHECK(w->b.pending == 0 && w->b.packets_out == 1);

    /* Command Status 也结清，opcode 0 不算应答 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    w->mute = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x0405));
    run(1);
    CHECK(w->b.pending == 1);
    const uint8_t nop[] = {0x04, 0x0e, 0x03, 0x01, 0x00, 0x00};
    ctrl_out(nop, sizeof nop);
    run(1);
    CHECK(w->b.pending == 1);
    const uint8_t status[] = {0x04, 0x0f, 0x04, 0x00, 0x01, 0x05, 0x04};
    ctrl_out(status, sizeof status);
    run(1);
    CHECK(w->b.pending == 0);

    /* 控制器回了错误状态：结清，也留一行，第一轮上机要靠它看是哪条命令不认 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    w->mute = 1;
    for (unsigned i = 0; i < PbBridgeLogRefusals + 3; ++i) {
        pb_bridge_write(&w->b, 1, p, command(p, 0x0c5b));
        run(2);
        const uint8_t no[] = {0x04, 0x0e, 0x04, 0x01, 0x5b, 0x0c, 0x01};
        ctrl_out(no, sizeof no);
        run(2);
        CHECK(w->b.pending == 0);
        if (!i) CHECK(strstr(w->last_log, "refused opcode=0xc5b status=0x1") != NULL);
    }
    CHECK(w->b.refused_logged == PbBridgeLogRefusals);
    const uint8_t cs_no[] = {0x04, 0x0f, 0x04, 0x0c, 0x01, 0x05, 0x04};
    reset_world(0);
    bring_up();
    ctrl_out(cs_no, sizeof cs_no);
    run(1);
    CHECK(strstr(w->last_log, "refused opcode=0x405 status=0xc") != NULL);

    /* 应答接着来，期限跟着往后走 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    w->mute = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x0c56));
    pb_bridge_write(&w->b, 1, p, command(p, 0x0c6d));
    run(3);
    CHECK(w->b.pending == 2);
    w->now += PbBridgeCommandMs - 1;
    const uint8_t cc[] = {0x04, 0x0e, 0x04, 0x01, 0x56, 0x0c, 0x00};
    ctrl_out(cc, sizeof cc);
    run(1);
    CHECK(w->b.pending == 1 && !w->b.guard);
    w->now = w->b.pending_at + PbBridgeCommandMs - 1;
    CHECK(pb_bridge_step(&w->b) >= 0 && !w->b.guard);
    w->now += 1;
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop && w->b.guard == PbGuardCommand);

    /* HCI_Reset 的应答清账：控制器复位了，别的命令不会再答 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 77) == PbBridgeOk);
    w->mute = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    pb_bridge_write(&w->b, 1, p, command(p, PbOpReset));
    run(3);
    CHECK(w->b.pending == 2);
    const uint8_t rcc[] = {0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00};
    ctrl_out(rcc, sizeof rcc);
    run(1);
    CHECK(w->b.pending == 0);

    /* 串口写失败 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 88) == PbBridgeOk);
    w->write_fails = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardWrite, 88);
    /* 0xfc 写失败也算 */
    reset_world(0);
    bring_up();
    w->write_fails = 1;
    const uint8_t wake[] = {PbIbsWake};
    ctrl_out(wake, 1);
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardWrite, 0);

    /* 控制器崩溃转储：两种形式 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 99) == PbBridgeOk);
    const uint8_t dump[] = {0x04, 0xff, 0x03, 0x01, 0x08, 0x00};
    ctrl_out(dump, sizeof dump);
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardCrash, 99);
    reset_world(0);
    bring_up();
    const uint8_t peri_dump[] = {0x34, 0x00, 0xff, 0x04, 0xf0, 0x04, 0x00, 0x00};
    ctrl_out(peri_dump, sizeof peri_dump);
    run(1);
    trips(PbGuardCrash, 0);

    /* 初始化走不完：从连接算，控制器起来得更晚时从起来算 */
    reset_world(0);
    CHECK(pb_bridge_connect(&w->b, 1, 66) == PbBridgeOk);
    w->now += 20000;
    bring_up();
    w->now += PbBridgeInitMs - 1;
    CHECK(pb_bridge_step(&w->b) >= 0 && !w->b.guard);
    w->now += 1;
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardInit, 66);
    /* 走完了就不管了 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 66) == PbBridgeOk);
    for (unsigned i = 0; i < INIT_COUNT; ++i) init_step(1, kInit[i]);
    w->now += 10 * PbBridgeInitMs;
    run(1);
    CHECK(!w->b.guard);
    /* 没有客户端时也不管 */
    pb_bridge_disconnect(&w->b, 1);
    run(2);
    w->now += 10 * PbBridgeInitMs;
    run(1);
    CHECK(!w->b.guard);

    /* 崩溃循环：连续 5 个走不完初始化 */
    reset_world(0);
    bring_up();
    for (unsigned i = 0; i < PbBridgeFailedStarts; ++i) {
        CHECK(!w->b.guard);
        CHECK(pb_bridge_connect(&w->b, 1, 10) == PbBridgeOk);
        pb_bridge_disconnect(&w->b, 1);
        if (i + 1 < PbBridgeFailedStarts) run(2);
    }
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    trips(PbGuardStarts, 0);
    /* Starting 期间的断开不算：控制器还没给人家机会 */
    reset_world(0);
    for (unsigned i = 0; i < 2 * PbBridgeFailedStarts; ++i) {
        CHECK(pb_bridge_connect(&w->b, 1, 10) == PbBridgeOk);
        pb_bridge_disconnect(&w->b, 1);
    }
    bring_up();
    run(2);
    CHECK(!w->b.guard && w->b.failed_starts == 0 && w->wire_n == 0);
    /* trip 之后写来的东西丢掉，厂商命令照回（客户端马上要被杀） */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 10) == PbBridgeOk);
    pb_bridge_fail(&w->b);
    w->mute = 1;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    CHECK(w->b.tx_dropped == 1);
    uint8_t f[32];
    pb_bridge_write(&w->b, 1, p, pb_hci_command(p, 0xfc4d, NULL, 0));
    CHECK(client_read(f) == 7);
    /* 第一个原因留着 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 10) == PbBridgeOk);
    ctrl_out(dump, sizeof dump);
    CHECK(pb_bridge_step(&w->b) == PbBridgeStop);
    pb_bridge_disconnect(&w->b, 1);
    pb_bridge_fail(&w->b);
    CHECK(w->b.guard == PbGuardCrash && w->b.kill_pid == 10);
}

static void polling(void)
{
    reset_world(0);
    bring_up();
    w->now += 1000;
    CHECK(pb_bridge_step(&w->b) == PbBridgeNoClientPollMs);
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    CHECK(pb_bridge_step(&w->b) == PbBridgeIdlePollMs);
    /* 有东西要发：一包发完还有下一包，不睡 */
    uint8_t p[PbPacketMax];
    const uint8_t acl[] = {PbH4Acl, 0x01, 0x20, 1, 0, 0xaa};
    pb_bridge_write(&w->b, 1, acl, sizeof acl);
    pb_bridge_write(&w->b, 1, acl, sizeof acl);
    CHECK(w->kicks >= 2);
    CHECK(pb_bridge_step(&w->b) == 0);
    /* 发完了：刚动过，1 ms 一轮；安静 PbBridgeActiveMs 之后退回空闲 */
    CHECK(pb_bridge_step(&w->b) == PbBridgeBusyPollMs);
    w->now += PbBridgeActiveMs - 1;
    CHECK(pb_bridge_step(&w->b) == PbBridgeBusyPollMs);
    w->now += 1;
    CHECK(pb_bridge_step(&w->b) == PbBridgeIdlePollMs);
    /* 等命令应答时 1 ms 一轮 */
    w->mute = 1;
    w->b.frames_in = 0;
    pb_bridge_write(&w->b, 1, p, command(p, 0x1001));
    (void)pb_bridge_step(&w->b);
    w->now += 1000;
    CHECK(pb_bridge_step(&w->b) == PbBridgeBusyPollMs);
    /* 收到字节也算动过 */
    reset_world(0);
    bring_up();
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    w->now += 1000;
    const uint8_t cc[] = {0x04, 0x0e, 0x04, 0x01, 0x01, 0x20, 0x00};
    ctrl_out(cc, sizeof cc);
    CHECK(pb_bridge_step(&w->b) == PbBridgeBusyPollMs);
    /* 一步最多读 PbBridgeReadRounds 轮 */
    static uint8_t lots[8192];
    memset(lots, PbIbsSleep, sizeof lots);
    ctrl_out(lots, sizeof lots);
    w->reads = 0;
    (void)pb_bridge_step(&w->b);
    CHECK(w->reads == PbBridgeReadRounds);
}

/* 替身：每条初始化命令的应答长度按 HCI 规范查。 */
static uint8_t expected_params(uint16_t opcode)
{
    switch (opcode) {
    case 0x1001: return 9;                   /* 状态 + 版本 8 */
    case 0x1002: return 65;
    case 0x1003: return 9;
    case 0x1004: return 11;
    case 0x1005: return 8;
    case 0x1009: return 7;
    case 0x2002: return 4;
    case 0x2003: return 9;
    case 0x200f: return 2;
    case 0x201c: return 9;
    case 0x202f: return 9;
    case 0x204a: return 2;
    case 0x2060: return 7;
    default: return 1;
    }
}

static void stand_in(void)
{
    reset_world(1);
    CHECK(w->b.link == PbLinkStandIn);
    split_seed = 5;
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk);
    for (unsigned i = 0; i < INIT_COUNT; ++i) {
        uint8_t p[PbPacketMax], f[PbH4FrameMax];
        client_write(1, p, command(p, kInit[i]));
        const uint32_t n = client_read(f);
        uint16_t op = 0;
        uint8_t status = 0xff;
        CHECK(n && pb_hci_complete(f, n, &op, &status, NULL) && op == kInit[i] && status == 0);
        CHECK(f[2] == 3u + expected_params(kInit[i]) && n == 6u + expected_params(kInit[i]));
        if (kInit[i] == 0x1001) CHECK_BYTES(f + 7, 8, 0x0e, 0x00, 0x00, 0x0e, 0x1d, 0x00, 0x21, 0x7e);   /* 跟踪 15.338943 */
        if (kInit[i] == 0x1009) CHECK(!memcmp(f + 7, kAddress, 6));
        if (kInit[i] == 0x1005) CHECK_BYTES(f + 7, 7, 0x00, 0x04, 0xf0, 0x08, 0x00, 0x04, 0x00);         /* 15.345341 */
        if (kInit[i] == 0x2060) CHECK_BYTES(f + 7, 6, 0xfb, 0x00, 0x10, 0xc8, 0x00, 0x04);               /* 15.347207 */
        if (kInit[i] == 0x2003) CHECK(f[7] == 0xff && f[8] == 0xf9);                                     /* 15.340192 */
        if (kInit[i] == 0x1002) {
            /* 初始化要用到的命令，描述符表里的位都要置上，否则 sub_100050934 返回 628：字节索引、掩码 */
            static const uint8_t needed[][2] = {
                {0x05, 0x80}, {0x0e, 0x08}, {0x0e, 0x10}, {0x0f, 0x02}, {0x0e, 0x20}, {0x0e, 0x40}, {0x19, 0x04}, {0x05, 0x40}, {0x16, 0x04}, {0x19, 0x01},
                {0x11, 0x40}, {0x18, 0x40}, {0x29, 0x20}, {0x23, 0x08}, {0x1c, 0x08}, {0x1a, 0x40}, {0x0e, 0x80}, {0x26, 0x40}, {0x2c, 0x02}, {0x2e, 0x01},
                {0x0a, 0x40}, {0x0a, 0x20}, {0x12, 0x08},
            };
            for (unsigned b = 0; b < sizeof needed / sizeof needed[0]; ++b) CHECK(f[7 + needed[b][0]] & needed[b][1]);
        }
    }
    CHECK(w->b.ready == 1 && strstr(w->last_log, "stack init finished") != NULL);
    /* 第 1、2 页也是跟踪里的 */
    uint8_t p[PbPacketMax], f[PbH4FrameMax];
    const uint8_t one[] = {1}, two[] = {2};
    client_write(1, p, pb_hci_command(p, 0x1004, one, 1));
    CHECK(client_read(f) == 17 && f[7] == 1 && f[8] == 2 && f[9] == 0x02);
    client_write(1, p, pb_hci_command(p, 0x1004, two, 1));
    CHECK(client_read(f) == 17 && f[7] == 2 && f[9] == 0x5d && f[10] == 0x03);
    /* 走 Command Status 的命令：回 Command Status，状态不是 0，协议栈不会等后续事件 */
    static const uint16_t status_only[] = {0x0401, 0x0405, 0x041b, 0x200d, 0x2043};
    for (unsigned i = 0; i < sizeof status_only / sizeof status_only[0]; ++i) {
        client_write(1, p, command(p, status_only[i]));
        CHECK(client_read(f) == 7 && f[1] == PbEvtCommandStatus && f[3] == PbStandInDisallowed);
        CHECK(f[4] == 1 && f[5] == (uint8_t)status_only[i] && f[6] == (uint8_t)(status_only[i] >> 8));
    }
    /* Inquiry_Cancel 例外 */
    client_write(1, p, command(p, 0x0402));
    CHECK(client_read(f) == 7 && f[1] == PbEvtCommandComplete);
    /* Host_Number_Of_Completed_Packets 没有应答，替身也不回 */
    const uint8_t done[5] = {1, 0x40, 0x00, 0x01, 0x00};
    client_write(1, p, pb_hci_command(p, PbOpHostCompleted, done, sizeof done));
    CHECK(client_read(f) == 0);
    /* ACL 丢掉，什么都不回 */
    const uint8_t acl[] = {PbH4Acl, 0x01, 0x20, 1, 0, 0xaa};
    pb_bridge_write(&w->b, 1, acl, sizeof acl);
    CHECK(client_read(f) == 0);
    /* 完全不碰串口，也用不着工作线程 */
    CHECK(w->wire_n == 0 && w->reads == 0 && w->kicks == 0);
    CHECK(pb_bridge_step(&w->b) == PbBridgeIdlePollMs && w->wire_n == 0);
    /* 替身不会 trip */
    pb_bridge_disconnect(&w->b, 1);
    CHECK(pb_bridge_connect(&w->b, 1, 2) == PbBridgeOk && !w->b.guard);
}

static void framer(void)
{
    /* 发送方向的成帧单独过一遍边界：正好最大能过，再大一字节不行，零长度 */
    static PbH4Tx t;
    pb_h4_tx_reset(&t);
    uint8_t edge[5] = {PbH4Acl, 1, 0, 0, 0};
    edge[3] = (uint8_t)(PbH4FrameMax - 5);
    edge[4] = (uint8_t)((PbH4FrameMax - 5) >> 8);
    for (unsigned i = 0; i < 5; ++i) CHECK(pb_h4_tx_byte(&t, edge[i]) == PbH4More);
    for (uint32_t i = 0; i < PbH4FrameMax - 6; ++i) CHECK(pb_h4_tx_byte(&t, 0xfd) == PbH4More);   /* 发送方向 0xfd 就是数据 */
    CHECK(pb_h4_tx_byte(&t, 0xfd) == PbH4Frame && t.have == PbH4FrameMax);
    edge[3] = (uint8_t)(PbH4FrameMax - 4);
    edge[4] = (uint8_t)((PbH4FrameMax - 4) >> 8);
    int last = PbH4More;
    for (unsigned i = 0; i < 5; ++i) last = pb_h4_tx_byte(&t, edge[i]);
    CHECK(last == PbH4Oversize && t.oversize == 1 && t.skip == PbH4FrameMax - 4);
    for (uint32_t i = 0; i < PbH4FrameMax - 4; ++i) CHECK(pb_h4_tx_byte(&t, 0x01) == PbH4More);
    const uint8_t empty[] = {PbH4Cmd, 0x03, 0x0c, 0x00};
    for (unsigned i = 0; i < 3; ++i) CHECK(pb_h4_tx_byte(&t, empty[i]) == PbH4More);
    CHECK(pb_h4_tx_byte(&t, empty[3]) == PbH4Frame && t.have == 4);
    const uint8_t sco[] = {PbH4Sco, 0x01, 0x00, 0x00};
    for (unsigned i = 0; i < 3; ++i) CHECK(pb_h4_tx_byte(&t, sco[i]) == PbH4More);
    CHECK(pb_h4_tx_byte(&t, sco[3]) == PbH4Frame && t.buf[0] == PbH4Sco);
    /* 事件和 ISO 不是主机发的东西 */
    CHECK(pb_h4_tx_byte(&t, PbH4Evt) == PbH4Junk);
    CHECK(pb_h4_tx_byte(&t, PbH4Iso) == PbH4Junk);
}

int main(void)
{
    w = calloc(1, sizeof *w);
    CHECK(w != NULL);
    framer();
    init_sequence();
    vsc_instant();
    hold_until_up();
    flow_control();
    client_stuck();
    tx_full();
    filters();
    connections();
    guards();
    polling();
    stand_in();
    printf("PASS bridge: %u checks\n", checks);
    return 0;
}
