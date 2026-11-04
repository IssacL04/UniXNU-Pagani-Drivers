/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 跟踪回放的 C 一半，三种用法：
 *   replay <固件目录> <输出文件>   用手机上拷下来的真固件跑 BtDownload.c 的完整序列（8 Mbps，和 Android 那次一样），控制器用 ctrl_model.h，
 *                                  把每次写的字节按行导出，Test/replay-hsuart.py 拿去和 hsuart 跟踪比。
 *   replay --h4 <输入文件>         把跟踪里记全了的接收记录喂给 H4 成帧和过滤层，把发送记录喂给发送过滤，打印统计。
 *   replay --bridge <输入文件>     同样的记录按时间顺序走完整的桥（BtBridge.c）：发送记录当协议栈写来的，分两次写（类型字节一次，其余一次，和 Android 一样），
 *                                  接收记录当串口上来的字节，打印桥的统计。
 * 固件和跟踪只在本地读，不进被跟踪路径；这个程序也不把它们写到别处，除了调用方给的输出文件（内容是发送字节，放临时目录）。
 *
 * 校准：Android 那次带了 /data/vendor/bluetooth/calibration.bin（4380 字节），文件在手机的 data 分区，这里没有。这里用全零占位，长度相同，
 * 导出时标出占位的字节，比对时只比帧头。XNU 正常路径读不到校准文件，发的是不带容器头的纯 NVM。 */
#include <errno.h>
#include "../BtBridge.h"
#include "../BtDownload.h"
#include "ctrl_model.h"
#include "check.h"

#define CAL_BYTES 4380u
static uint8_t store[CmBufCount][1u << 20];
static uint8_t *const buffers[CmBufCount] = {store[0], store[1], store[2], store[3], store[4]};
static CtrlModel ctrl;
static uint64_t now_ms = 1000;
static FILE *out;
static uint32_t nvm_plain;                 /* 容器里 NVM 结束的位置 */

static int x_write(void *c, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    (void)c;
    (void)timeout_ms;
    /* 落在校准占位里的字节不和跟踪比：这一包第几个字节开始是占位 */
    uint32_t wild = bytes;
    if (bytes > 6 && data[0] == PbH4Cmd && data[1] == 0 && data[2] == 0xfc && data[4] == PbEdlDownload && ctrl.bt_elf_done) {
        const uint32_t at = ctrl.image_len[CmBufBtNvm];
        if (at + (bytes - 6) > nvm_plain) wild = at >= nvm_plain ? 6 : 6 + nvm_plain - at;
    }
    fprintf(out, "W %u ", wild);
    for (uint32_t i = 0; i < bytes; ++i) fprintf(out, "%02x", data[i]);
    fputc('\n', out);
    for (uint32_t i = 0; i < bytes; ++i) cm_rx(&ctrl, data[i]);
    return 0;
}
static uint32_t x_read(void *c, uint8_t *data, uint32_t max)
{
    (void)c;
    uint32_t n = 0;
    while (n < max && cm_pending(&ctrl)) data[n++] = cm_get(&ctrl);
    if (!cm_pending(&ctrl)) ctrl.pending_acks = 0;
    return n;
}
static void x_sleep(void *c, unsigned ms) {(void)c; now_ms += ms;}
static uint64_t x_now(void *c) {(void)c; return now_ms;}
static int x_rfr(void *c, int ready) {(void)c; fprintf(out, "F %d\n", ready); return 0;}
static int x_baud(void *c, uint32_t baud) {(void)c; fprintf(out, "B %u\n", baud); ctrl.baud = baud; return 0;}
static void x_log(void *c, const char *line) {(void)c; if (strstr(line, "FAIL")) fprintf(stderr, "%s\n", line);}
static const PbXport kX = {NULL, x_write, x_read, x_sleep, x_now, x_rfr, x_baud, x_log};

static uint8_t *load(const char *dir, const char *name, uint32_t *bytes, uint32_t extra)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    rewind(f);
    uint8_t *data = malloc((size_t)size + extra);
    CHECK(data && fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    *bytes = (uint32_t)size;
    return data;
}

static int generate(const char *dir, const char *path)
{
    PbDlConfig c;
    memset(&c, 0, sizeof c);
    uint32_t nvm_bytes = 0;
    c.baud = 8000000;
    c.peri = load(dir, "brhperifw20.mbn", &c.peri_bytes, 0);
    c.peri_nvm = load(dir, "brhperinv20.bin", &c.peri_nvm_bytes, 0);
    c.tmel = load(dir, "tmel_peach_20.elf", &c.tmel_bytes, 0);
    c.bt = load(dir, "brhbtfw20.mbn", &c.bt_bytes, 0);
    uint8_t *nvm = load(dir, "brhbtnv20.bin", &nvm_bytes, 0);
    /* 校准容器：Android 那次带了校准，头是 04 c7 44 00 */
    uint8_t *wrapped = malloc(nvm_bytes + CAL_BYTES + 4);
    uint8_t *cal = calloc(1, CAL_BYTES);
    CHECK(wrapped && cal);
    c.bt_nvm = wrapped;
    c.bt_nvm_bytes = pb_nvm_wrap(wrapped, nvm, nvm_bytes, cal, CAL_BYTES);
    nvm_plain = nvm_bytes + 4;
    cm_init(&ctrl, buffers);
    out = fopen(path, "w");
    CHECK(out != NULL);
    static PbDl dl;
    pb_dl_init(&dl, &kX, &c);
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(pb_dl_version(&dl) == 0);
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(!ctrl.violations && dl.step == PbStepReset);
    fclose(out);
    printf("PASS generated: %u writes, %u bytes\n", dl.packets, dl.bytes);
    return 0;
}

/* ---- H4 ---- */
static int hex(int c) {return c <= '9' ? c - '0' : c - 'a' + 10;}
static int h4_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    static char line[8192];
    static PbH4Rx rx;
    pb_h4_rx_reset(&rx);
    unsigned ibs = 0, frames = 0, pass = 0, drop = 0, bad = 0, tx_pass = 0, tx_answered = 0, tx_bad = 0;
    while (fgets(line, sizeof line, f)) {
        uint8_t data[4096];
        uint32_t n = 0;
        for (const char *p = line + 2; p[0] > ' ' && p[1] > ' '; p += 2) data[n++] = (uint8_t)(hex(p[0]) << 4 | hex(p[1]));
        if (line[0] == 'R') {
            for (uint32_t i = 0; i < n; ++i) {
                const int r = pb_h4_rx_byte(&rx, data[i]);
                if (r == PbH4Ibs) {
                    ++ibs;
                } else if (r == PbH4Frame) {
                    ++frames;
                    const int d = pb_h4_filter_rx(rx.buf, rx.have);
                    pass += d == PbFilterPass;
                    drop += d == PbFilterDrop;
                    bad += d == PbFilterBad;
                }
            }
        } else if (line[0] == 'T') {
            uint8_t reply[8];
            uint32_t size = 0;
            uint16_t vendor = 0;
            const int d = pb_h4_filter_tx(data, n, reply, &size, &vendor);
            tx_pass += d == PbFilterPass;
            tx_answered += d == PbFilterAnswered;
            tx_bad += d == PbFilterBad;
        }
    }
    fclose(f);
    printf("h4 ibs=%u frames=%u pass=%u drop=%u bad=%u junk=%u oversize=%u tx_pass=%u tx_answered=%u tx_bad=%u\n", ibs, frames, pass, drop, bad,
           rx.junk, rx.oversize, tx_pass, tx_answered, tx_bad);
    return 0;
}

/* ---- 桥 ---- */
static uint8_t wire[1u << 20], from_uart[1u << 16];
static uint32_t wire_bytes, uart_head, uart_tail, wire_acks, up_bytes;
static void b_lock(void *c, int take) {(void)c; (void)take;}
static uint64_t b_now(void *c) {(void)c; return now_ms;}
static int b_write(void *c, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    (void)c;
    (void)timeout_ms;
    if (bytes == 1 && data[0] == PbIbsWakeAck) {
        ++wire_acks;
        return 0;
    }
    CHECK(wire_bytes + bytes <= sizeof wire);
    memcpy(wire + wire_bytes, data, bytes);
    wire_bytes += bytes;
    return 0;
}
static uint32_t b_read(void *c, uint8_t *data, uint32_t max)
{
    (void)c;
    uint32_t n = uart_tail - uart_head;
    if (n > max) n = max;
    memcpy(data, from_uart + uart_head, n);
    uart_head += n;
    return n;
}
static int b_put(void *c, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    (void)c;
    (void)unit;
    (void)data;
    up_bytes += bytes;
    return 0;
}
static void b_kick(void *c) {(void)c;}
static void b_wait(void *c, unsigned ms) {(void)c; (void)ms;}
static void b_log(void *c, const char *line) {(void)c; (void)line;}
static const PbBridgeIo kBridgeIo = {NULL, b_lock, b_now, b_write, b_read, b_put, b_kick, b_wait, b_kick, b_log};

static int bridge_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    static char line[8192];
    static PbBridge b;
    pb_bridge_init(&b, &kBridgeIo, NULL, 0);
    CHECK(pb_bridge_connect(&b, 1, 2) == PbBridgeOk);
    pb_bridge_up(&b, NULL, NULL, 0);
    while (fgets(line, sizeof line, f)) {
        uint8_t data[4096];
        uint32_t n = 0;
        for (const char *p = line + 2; p[0] > ' ' && p[1] > ' '; p += 2) data[n++] = (uint8_t)(hex(p[0]) << 4 | hex(p[1]));
        if (line[0] == 'R') {
            CHECK(uart_tail + n <= sizeof from_uart);
            memcpy(from_uart + uart_tail, data, n);
            uart_tail += n;
        } else if (line[0] == 'T') {
            pb_bridge_write(&b, 1, data, 1);
            pb_bridge_write(&b, 1, data + 1, n - 1);
        }
        for (unsigned i = 0; i < 16 && (uart_head != uart_tail || b.to_uart.head != b.to_uart.tail); ++i) CHECK(pb_bridge_step(&b) >= 0);
        CHECK(uart_head == uart_tail && b.to_uart.head == b.to_uart.tail);
    }
    fclose(f);
    printf("bridge out=%u wire=%u answered=%u kinds=%u up=%u up_bytes=%u dropped=%u bad=%u acks=%u junk=%u guard=%u\n", b.packets_out, wire_bytes, b.vsc_answered,
           b.vsc_kinds, b.frames_up, up_bytes, b.frames_dropped, b.frames_bad, wire_acks, b.rx.junk, b.guard);
    for (uint32_t i = 0; i < b.vsc_kinds; ++i) printf("vsc %04x\n", b.vsc[i]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--h4")) return h4_replay(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--bridge")) return bridge_replay(argv[2]);
    if (argc == 3) return generate(argv[1], argv[2]);
    fprintf(stderr, "usage: replay <firmware dir> <output> | replay --h4 <input> | replay --bridge <input>\n");
    return 2;
}
