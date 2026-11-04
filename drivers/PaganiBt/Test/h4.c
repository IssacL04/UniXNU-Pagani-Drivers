/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtH4.c：成帧、IBS 只在帧边界、超长帧、过滤矩阵。接收样本是 Android 跟踪里记全了的真实字节。 */
#include "../BtH4.h"
#include "../BtTlv.h"
#include "check.h"

typedef struct {unsigned ibs, frames, junk, oversize; uint32_t last; uint8_t last_first;} Tally;

static void feed(PbH4Rx *r, const uint8_t *bytes, uint32_t n, Tally *t)
{
    for (uint32_t i = 0; i < n; ++i) {
        const int rc = pb_h4_rx_byte(r, bytes[i]);
        if (rc == PbH4Ibs) t->ibs++;
        else if (rc == PbH4Junk) t->junk++;
        else if (rc == PbH4Oversize) t->oversize++;
        else if (rc == PbH4Frame) {
            t->frames++;
            t->last = r->have;
            t->last_first = r->buf[0];
        }
    }
}

static void framing(void)
{
    PbH4Rx r;
    Tally t;
    memset(&t, 0, sizeof t);
    pb_h4_rx_reset(&r);
    /* 跟踪 15.183418 fe，15.300820 fd，15.300963 事件 */
    const uint8_t sample[] = {0xfe, 0xfd, 0x04, 0x0e, 0x05, 0x01, 0x00, 0xfc, 0x00, 0x1e};
    feed(&r, sample, sizeof sample, &t);
    CHECK(t.ibs == 2 && t.frames == 1 && t.last == 8 && t.last_first == 4 && !t.junk);

    /* 帧里的 0xfc/0xfd/0xfe 是数据：这个事件的 opcode 高字节就是 0xfc */
    memset(&t, 0, sizeof t);
    const uint8_t inside[] = {0x04, 0x0e, 0x0d, 0x01, 0x1d, 0xfc, 0x00, 0x21, 0x00, 0x01, 0x00, 0x9c, 0x7d, 0x7f, 0xfc, 0xf0};
    feed(&r, inside, sizeof inside, &t);
    CHECK(t.ibs == 0 && t.frames == 1 && t.last == sizeof inside);

    /* 一包 DMA 里两帧首尾相接：跟踪 15.304467，事件后面紧跟日志 ACL */
    memset(&t, 0, sizeof t);
    uint8_t two[64] = {0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00, 0x02, 0xdc, 0x2e, 0x04, 0x00, 1, 2, 3, 4};
    feed(&r, two, 16, &t);
    CHECK(t.frames == 2 && t.last == 9 && t.last_first == 2);

    /* 字节一个个来，和一次来结果相同：0x34 外设帧 */
    memset(&t, 0, sizeof t);
    const uint8_t peri[] = {0x34, 0x00, 0xff, 0x04, 0xf0, 0x03, 0x00, 0x00};
    for (unsigned i = 0; i < sizeof peri; ++i) feed(&r, peri + i, 1, &t);
    CHECK(t.frames == 1 && t.last == 8 && t.last_first == 0x34);

    /* SCO、ISO 的长度字段 */
    memset(&t, 0, sizeof t);
    const uint8_t sco[] = {0x03, 0x01, 0x00, 0x02, 0xaa, 0xbb};
    feed(&r, sco, sizeof sco, &t);
    CHECK(t.frames == 1 && t.last == 6);
    const uint8_t iso[] = {0x05, 0x01, 0x00, 0x02, 0xc0, 0xaa, 0xbb};   /* 高 2 位是标志 */
    feed(&r, iso, sizeof iso, &t);
    CHECK(t.frames == 2 && t.last == 7);

    /* 零长度帧 */
    memset(&t, 0, sizeof t);
    const uint8_t empty[] = {0x04, 0x0e, 0x00, 0xfc};
    feed(&r, empty, sizeof empty, &t);
    CHECK(t.frames == 1 && t.last == 3 && t.ibs == 1);

    /* 不认识的字节计数，不进帧 */
    memset(&t, 0, sizeof t);
    pb_h4_rx_reset(&r);
    const uint8_t junk[] = {0x00, 0x7f, 0xff, 0x04, 0x0e, 0x00};
    feed(&r, junk, sizeof junk, &t);
    CHECK(t.junk == 3 && t.frames == 1 && r.junk == 3);

    /* 轮询的半个字后面又进了新字：补位的 0 落在帧边界，IBS 和帧都不丢（BtGeni.h） */
    memset(&t, 0, sizeof t);
    pb_h4_rx_reset(&r);
    const uint8_t padded[] = {0xfc, 0, 0, 0, 0x04, 0x0e, 0x02, 0x01, 0x02, 0, 0, 0, 0x04, 0x0e, 0x00};
    feed(&r, padded, sizeof padded, &t);
    CHECK(t.ibs == 1 && t.frames == 2 && t.junk == 6 && t.last == 3);

    /* 超长帧：吃掉声明的长度，不写出缓冲区（ASan 看着），之后接得上 */
    memset(&t, 0, sizeof t);
    pb_h4_rx_reset(&r);
    const uint8_t huge[] = {0x02, 0x01, 0x00, 0xff, 0xff};
    feed(&r, huge, sizeof huge, &t);
    CHECK(t.oversize == 1 && r.skip == 0xffff);
    for (uint32_t i = 0; i < 0xffff; ++i) {
        const uint8_t b = 0xfd;
        feed(&r, &b, 1, &t);
    }
    CHECK(t.ibs == 0 && !r.skip);
    const uint8_t ok[] = {0x04, 0x0e, 0x00};
    feed(&r, ok, sizeof ok, &t);
    CHECK(t.frames == 1);

    /* 正好最大的帧能过，再大一字节不行 */
    memset(&t, 0, sizeof t);
    pb_h4_rx_reset(&r);
    uint8_t edge[5] = {0x02, 0x01, 0x00, 0, 0};
    edge[3] = (uint8_t)(PbH4FrameMax - 5);
    edge[4] = (uint8_t)((PbH4FrameMax - 5) >> 8);
    feed(&r, edge, 5, &t);
    CHECK(t.oversize == 0);
    for (uint32_t i = 0; i < PbH4FrameMax - 5; ++i) {
        const uint8_t b = 0x11;
        feed(&r, &b, 1, &t);
    }
    CHECK(t.frames == 1 && t.last == PbH4FrameMax);
    edge[3] = (uint8_t)(PbH4FrameMax - 4);
    edge[4] = (uint8_t)((PbH4FrameMax - 4) >> 8);
    memset(&t, 0, sizeof t);
    feed(&r, edge, 5, &t);
    CHECK(t.oversize == 1);
}

static void filter_rx(void)
{
    /* 控制器日志和外设事件留在内核层 */
    const uint8_t log[] = {0x02, 0xdc, 0x2e, 0x04, 0x00, 0, 1, 2, 3};
    CHECK(pb_h4_filter_rx(log, sizeof log) == PbFilterDrop);
    const uint8_t acl[] = {0x02, 0x01, 0x20, 0x04, 0x00, 0, 1, 2, 3};
    CHECK(pb_h4_filter_rx(acl, sizeof acl) == PbFilterPass);
    const uint8_t vendor[] = {0x04, 0xff, 0x02, 0x01, 0x02};
    CHECK(pb_h4_filter_rx(vendor, sizeof vendor) == PbFilterDrop);
    const uint8_t cc[] = {0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00};
    CHECK(pb_h4_filter_rx(cc, sizeof cc) == PbFilterPass);
    const uint8_t peri[] = {0x34, 0x00, 0xff, 0x00};
    CHECK(pb_h4_filter_rx(peri, sizeof peri) == PbFilterDrop);
    const uint8_t sco[] = {0x03, 0x01, 0x00, 0x00};
    CHECK(pb_h4_filter_rx(sco, sizeof sco) == PbFilterPass);
    const uint8_t iso[] = {0x05, 0x01, 0x00, 0x00, 0x00};
    CHECK(pb_h4_filter_rx(iso, sizeof iso) == PbFilterBad);
    CHECK(pb_h4_filter_rx(cc, 2) == PbFilterBad);
    CHECK(pb_h4_filter_rx(acl, 4) == PbFilterBad);
    CHECK(pb_h4_filter_rx(cc, 0) == PbFilterBad);
}

static void filter_tx(void)
{
    uint8_t reply[16];
    uint32_t bytes = 99;
    uint16_t vendor = 0;
    /* HCI_Reset 透传 */
    const uint8_t reset[] = {0x01, 0x03, 0x0c, 0x00};
    CHECK(pb_h4_filter_tx(reset, sizeof reset, reply, &bytes, &vendor) == PbFilterPass && bytes == 0);
    const uint8_t version[] = {0x01, 0x01, 0x10, 0x00};
    CHECK(pb_h4_filter_tx(version, sizeof version, reply, &bytes, &vendor) == PbFilterPass);
    /* OGF 0x3f 全部不下发：高通 0xfc00 下载、0xfc0b NVM、0xfc14 写地址，都和博通号共用空间 */
    static const uint8_t opcodes[][2] = {{0x00, 0xfc}, {0x0b, 0xfc}, {0x14, 0xfc}, {0x01, 0xfc}, {0xff, 0xff}, {0x00, 0xfd}, {0x53, 0xfd}};
    for (unsigned i = 0; i < sizeof opcodes / sizeof opcodes[0]; ++i) {
        const uint8_t cmd[] = {0x01, opcodes[i][0], opcodes[i][1], 0x01, 0x19};
        vendor = 0;
        CHECK(pb_h4_filter_tx(cmd, sizeof cmd, reply, &bytes, &vendor) == PbFilterAnswered);
        CHECK(vendor == (uint16_t)(opcodes[i][0] | opcodes[i][1] << 8));
        CHECK_BYTES(reply, bytes, 0x04, 0x0e, 0x04, 0x01, opcodes[i][0], opcodes[i][1], 0x01);
    }
    /* OGF 0x3e 不是厂商命令 */
    const uint8_t ogf3e[] = {0x01, 0x00, 0xf8, 0x00};
    CHECK(pb_h4_filter_tx(ogf3e, sizeof ogf3e, reply, &bytes, &vendor) == PbFilterPass);
    /* 长度对不上的包不下发 */
    const uint8_t short_cmd[] = {0x01, 0x03, 0x0c, 0x02, 0x00};
    CHECK(pb_h4_filter_tx(short_cmd, sizeof short_cmd, reply, &bytes, &vendor) == PbFilterBad);
    const uint8_t acl[] = {0x02, 0x01, 0x00, 0x02, 0x00, 0xaa, 0xbb};
    CHECK(pb_h4_filter_tx(acl, sizeof acl, reply, &bytes, &vendor) == PbFilterPass);
    CHECK(pb_h4_filter_tx(acl, 6, reply, &bytes, &vendor) == PbFilterBad);
    const uint8_t sco[] = {0x03, 0x01, 0x00, 0x01, 0xaa};
    CHECK(pb_h4_filter_tx(sco, sizeof sco, reply, &bytes, &vendor) == PbFilterPass);
    CHECK(pb_h4_filter_tx(sco, 4, reply, &bytes, &vendor) == PbFilterBad);
    const uint8_t evt[] = {0x04, 0x0e, 0x00};
    CHECK(pb_h4_filter_tx(evt, sizeof evt, reply, &bytes, &vendor) == PbFilterBad);
    CHECK(pb_h4_filter_tx(reset, 1, reply, &bytes, &vendor) == PbFilterBad);
}

int main(void)
{
    framing();
    filter_rx();
    filter_tx();
    printf("PASS h4: %u checks\n", checks);
    return 0;
}
