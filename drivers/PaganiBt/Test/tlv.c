/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtTlv.c 的纯函数。期望的字节都抄自 Android 的 hsuart 跟踪（发送方向）和本机 HAL 的解析代码（接收方向，跟踪里那段 RX 丢了）。 */
#include "../BtTlv.h"
#include "check.h"

static void frames(void)
{
    uint8_t p[PbPacketMax];
    /* 跟踪 14.2748 起 */
    CHECK_BYTES(p, pb_peri_getver(p, PbSsPeri), 0x31, 0x00, 0xf0, 0xff, 0x02, 0x05, 0x00);
    CHECK_BYTES(p, pb_peri_baud(p, 0x15), 0x31, 0x00, 0xf1, 0xff, 0x02, 0x02, 0x15);
    CHECK_BYTES(p, pb_peri_arbitrate(p, PbSsPeri), 0x31, 0x00, 0xf0, 0xff, 0x03, 0x08, 0x00, 0x00);
    CHECK_BYTES(p, pb_peri_board_id(p), 0x31, 0x00, 0xf0, 0xff, 0x01, 0x0a);
    CHECK_BYTES(p, pb_peri_build_info(p, PbSsPeri), 0x31, 0x00, 0xf0, 0xff, 0x02, 0x09, 0x00);
    CHECK_BYTES(p, pb_peri_arbitrate(p, PbSsTmel), 0x31, 0x00, 0xf0, 0xff, 0x03, 0x08, 0x03, 0x00);
    CHECK_BYTES(p, pb_peri_getver(p, PbSsTmel), 0x31, 0x00, 0xf0, 0xff, 0x02, 0x05, 0x03);
    CHECK_BYTES(p, pb_peri_activate(p, PbSsBt, 1), 0x31, 0x00, 0xf1, 0xff, 0x03, 0x00, 0x01, 0x01);
    CHECK_BYTES(p, pb_peri_reset(p), 0x31, 0x00, 0xf1, 0xff, 0x01, 0x03);
    CHECK_BYTES(p, pb_edl_command(p, PbEdlGetVer), 0x01, 0x00, 0xfc, 0x01, 0x19);
    CHECK_BYTES(p, pb_edl_command(p, PbEdlBoardId), 0x01, 0x00, 0xfc, 0x01, 0x23);
    CHECK_BYTES(p, pb_edl_command(p, PbEdlBuildInfo), 0x01, 0x00, 0xfc, 0x01, 0x20);
    CHECK_BYTES(p, pb_hci_command(p, PbOpReset, NULL, 0), 0x01, 0x03, 0x0c, 0x00);

    /* 整段：31 00 f0 ff f6 06 00 f3 + 243；最后一段 31 00 f0 ff 0f 06 00 0c + 12 */
    uint8_t data[243];
    for (unsigned i = 0; i < sizeof data; ++i) data[i] = (uint8_t)i;
    unsigned n = pb_peri_segment(p, PbSsPeri, data, 243);
    CHECK(n == 251);
    CHECK_BYTES(p, 8, 0x31, 0x00, 0xf0, 0xff, 0xf6, 0x06, 0x00, 0xf3);
    CHECK(!memcmp(p + 8, data, 243));
    n = pb_peri_segment(p, PbSsTmel, data, 12);
    CHECK(n == 20);
    CHECK_BYTES(p, 8, 0x31, 0x00, 0xf0, 0xff, 0x0f, 0x06, 0x03, 0x0c);
    /* HCI：01 00 fc f5 1e f3 + 243；最后一段 01 00 fc 75 1e 73 + 115 */
    n = pb_edl_segment(p, data, 243);
    CHECK(n == 249);
    CHECK_BYTES(p, 6, 0x01, 0x00, 0xfc, 0xf5, 0x1e, 0xf3);
    n = pb_edl_segment(p, data, 115);
    CHECK(n == 121);
    CHECK_BYTES(p, 6, 0x01, 0x00, 0xfc, 0x75, 0x1e, 0x73);
}

static void segments(void)
{
    uint32_t last = 99;
    CHECK(pb_tlv_segments(0, &last) == 0 && last == 0);
    CHECK(pb_tlv_segments(1, &last) == 1 && last == 1);
    CHECK(pb_tlv_segments(242, &last) == 1 && last == 242);
    CHECK(pb_tlv_segments(243, &last) == 1 && last == 243);
    CHECK(pb_tlv_segments(244, &last) == 2 && last == 1);
    CHECK(pb_tlv_segments(486, &last) == 2 && last == 243);
    CHECK(pb_tlv_segments(487, &last) == 3 && last == 1);
    /* 实际文件：外设 NVM 741 字节是 3 整段加 12（跟踪末段 0f 06 00 0c）；BT NVM 加校准后 17611 字节是 72 整段加 115（跟踪末段 75 1e 73） */
    CHECK(pb_tlv_segments(741, &last) == 4 && last == 12);
    CHECK(pb_tlv_segments(17611, &last) == 73 && last == 115);
    static const uint8_t elf[16] = {0x7f, 'E', 'L', 'F'};
    uint8_t nvm[16] = {PbTlvNvm}, patch[16] = {1};
    CHECK(!pb_tlv_wait_each(elf, sizeof elf));
    CHECK(pb_tlv_wait_each(nvm, sizeof nvm));
    CHECK(pb_tlv_wait_each(patch, sizeof patch));
    patch[14] = 3;
    CHECK(!pb_tlv_wait_each(patch, sizeof patch));
}

/* 造一个 NVM：tag 2、tag 17、tag 27、tag 174、一个无关 tag。 */
static uint32_t put_tag(uint8_t *at, uint16_t tag, const uint8_t *data, uint16_t size)
{
    memset(at, 0, 12);
    at[0] = (uint8_t)tag;
    at[1] = (uint8_t)(tag >> 8);
    at[2] = (uint8_t)size;
    at[3] = (uint8_t)(size >> 8);
    memcpy(at + 12, data, size);
    return 12u + size;
}
static uint32_t make_nvm(uint8_t *d)
{
    static const uint8_t addr[6] = {0xad, 0x5a, 0x01, 0x00, 0x55, 0x78};    /* 文件里的占位值 */
    static const uint8_t hci[6] = {0x15, 0x11, 0x22, 0x00, 0x2e, 0x00};      /* brhbtnv20.bin 的 tag 17 */
    static const uint8_t sleep_tag[14] = {0x01, 0x00, 0x0a, 0x28};
    static const uint8_t asd[2] = {0x03, 0x77}, other[3] = {1, 2, 3};
    uint32_t p = 4;
    p += put_tag(d + p, 1, other, 3);
    p += put_tag(d + p, PbTagBdAddr, addr, 6);
    p += put_tag(d + p, PbTagHci, hci, 6);
    p += put_tag(d + p, PbTagDeepSleep, sleep_tag, 14);
    p += put_tag(d + p, PbTagAsd, asd, 2);
    const uint32_t len = p - 4;
    d[0] = PbTlvNvm;
    d[1] = (uint8_t)len;
    d[2] = (uint8_t)(len >> 8);
    d[3] = 0;
    return p;
}
static uint8_t *tag_data(uint8_t *d, uint32_t skip_tags)
{
    uint32_t p = d[0] == PbTlvContainer ? 8 : 4;
    while (skip_tags--) p += 12u + (uint32_t)(d[p + 2] | d[p + 3] << 8);
    return d + p + 12;
}

static void nvm(void)
{
    uint8_t d[256], orig[256], wrapped[300];
    const uint8_t address[6] = {1, 2, 3, 4, 5, 6};
    PbNvmEdit edit = {0x15, 1, 0, address};
    PbNvmReport report;
    uint32_t n = make_nvm(d);
    memcpy(orig, d, n);
    CHECK(pb_nvm_edit(d, n, &edit, &report) == 0);
    CHECK(report.tags == 5 && report.saw_addr && report.saw_hci && report.saw_sleep && report.saw_asd);
    CHECK(!memcmp(tag_data(d, 1), address, 6));
    CHECK(tag_data(d, 2)[0] == 0x15 && tag_data(d, 2)[1] == 0x15 && tag_data(d, 2)[2] == 0x22);     /* 只改 data[1] */
    CHECK(tag_data(d, 3)[0] == 0x01 && tag_data(d, 3)[1] == 0x01);                                   /* SIBS */
    CHECK(tag_data(d, 4)[0] == 0x02 && tag_data(d, 4)[1] == 0x77);                                   /* ASD 清 bit 0 */
    CHECK(!memcmp(tag_data(d, 0), tag_data(orig, 0), 3));                                            /* 无关 tag 不动 */

    /* 负数和空指针表示不动，外设 NVM 就是这样用的 */
    memcpy(d, orig, n);
    PbNvmEdit peri = {0x00, 1, -1, NULL};
    CHECK(pb_nvm_edit(d, n, &peri, &report) == 0);
    CHECK(tag_data(d, 2)[1] == 0x00);
    CHECK(!memcmp(tag_data(d, 1), tag_data(orig, 1), 6));
    CHECK(tag_data(d, 4)[0] == 0x03);
    PbNvmEdit clear = {-1, 0, 1, NULL};
    memcpy(d, orig, n);
    tag_data(d, 3)[1] = 1;
    tag_data(d, 4)[0] = 2;
    CHECK(pb_nvm_edit(d, n, &clear, &report) == 0);
    CHECK(tag_data(d, 3)[1] == 0 && tag_data(d, 4)[0] == 3);
    CHECK(tag_data(d, 2)[1] == 0x11);

    /* 校准容器：头 04 len24，内层照改，校准字节不算进 NVM 的块 */
    uint8_t cal[20];
    memset(cal, 0, sizeof cal);
    cal[0] = PbTlvNvm;                                 /* 真的校准文件也是类型 2 的块，HAL 打开时检查第一个字节 */
    cal[1] = sizeof cal - 4;
    cal[sizeof cal - 1] = 0xee;
    const uint32_t wn = pb_nvm_wrap(wrapped, orig, n, cal, sizeof cal);
    CHECK(wn == n + sizeof cal + 4);
    CHECK(wrapped[0] == 4 && wrapped[1] == (uint8_t)(n + sizeof cal) && wrapped[2] == 0 && wrapped[3] == 0);
    CHECK(pb_nvm_edit(wrapped, wn, &edit, &report) == 0 && report.saw_hci);
    CHECK(tag_data(wrapped, 2)[1] == 0x15);
    CHECK(wrapped[wn - 1] == 0xee);

    /* 只改类型 2 的块：别的类型里长得像 Tag 17 的字节不动 */
    uint8_t blocks[128];
    memset(blocks, 0, sizeof blocks);
    uint32_t q = 4;
    static const uint8_t look_alike[6] = {0x15, 0x11, 0x22, 0, 0, 0};
    q += put_tag(blocks + q, PbTagHci, look_alike, 6);
    blocks[0] = 5;
    blocks[1] = (uint8_t)(q - 4);
    const uint32_t block_bytes = q;
    CHECK(pb_nvm_edit(blocks, block_bytes, &edit, &report) == 0);
    CHECK(blocks[4 + 12 + 1] == 0x11 && !report.saw_hci);

    /* 不可信数据：越界全部拒绝，而且不留半截修改 */
    /* 块长超过文件，超出部分正好全零：不能靠后面的垃圾字节碰巧失败 */
    uint8_t tail_zero[64];
    memset(tail_zero, 0, sizeof tail_zero);
    tail_zero[0] = PbTlvNvm;
    tail_zero[1] = 12 + 8;
    CHECK(pb_nvm_edit(tail_zero, 12, &edit, &report) == -3);
    memcpy(d, orig, n);
    d[1] = (uint8_t)(n + 10);                          /* 块长超过文件 */
    uint8_t copy[256];
    memcpy(copy, d, n);
    CHECK(pb_nvm_edit(d, n, &edit, &report) < 0);
    CHECK(!memcmp(copy, d, n));
    memcpy(d, orig, n);
    const uint32_t last_tag_len_at = n - 2 - 12 + 2;   /* 最后一个 tag 的 size 字段 */
    d[last_tag_len_at] = 0xff;
    memcpy(copy, d, n);
    CHECK(pb_nvm_edit(d, n, &edit, &report) < 0);
    CHECK(!memcmp(copy, d, n));                        /* 前面几个合法 tag 也没被改 */
    memcpy(d, wrapped, 16);
    d[1] = 0xff;
    d[2] = 0xff;
    CHECK(pb_nvm_edit(d, 16, &edit, &report) < 0);     /* 容器声明的长度超过文件 */
    CHECK(pb_nvm_edit(d, 3, &edit, &report) < 0);
    CHECK(pb_nvm_edit(NULL, 16, &edit, &report) < 0);
}

static void events(void)
{
    /* 完成事件：a[7..8] 命令号，a[9] 状态，a[10] subop，a[11] 子系统 */
    uint8_t cc[32] = {0x34, 0x00, 0xff, 21, 0xf0, 1, 1, 0xf0, 0xff, 0x00, 0x05, 0x00};
    cc[12] = 0x1e;
    cc[16] = 0x34; cc[17] = 0x12;
    cc[18] = 0x00; cc[19] = 0x02;
    cc[20] = 0x00; cc[21] = 0x02; cc[22] = 0x21; cc[23] = 0x40;
    cc[24] = 7;
    PbPeriEvent e;
    pb_peri_parse(cc, 25, &e);
    CHECK(e.kind == PbPeriComplete && e.opcode == PbOpPeriEdl && e.sub == PbSubGetVer && e.status == 0 && e.ss == 0);
    PbPeriVersion v;
    CHECK(pb_peri_version(cc, 25, &v) == 0);
    CHECK(v.product == 0x1e && v.patch == 0x1234 && v.build == 0x0200 && v.soc == 0x40210200 && v.security == 7 && v.has_soc);
    cc[3] = 10;                                        /* 长度 10 时没有 SoC 字段 */
    CHECK(pb_peri_version(cc, 25, &v) == 0 && !v.has_soc && v.soc == 0);
    CHECK(pb_peri_version(cc, 19, &v) < 0);

    const uint8_t status[] = {0x34, 0x00, 0xff, 6, 0xf0, 0, 23, 1, 0xf1, 0xff};
    pb_peri_parse(status, sizeof status, &e);
    CHECK(e.kind == PbPeriStatus && e.status == 23 && e.opcode == PbOpPeriGeneral);
    const uint8_t active[] = {0x34, 0x00, 0xff, 4, 0xf0, 2, 1, 1};
    pb_peri_parse(active, sizeof active, &e);
    CHECK(e.kind == PbPeriActivated && e.ss == 1 && e.action == 1);
    const uint8_t patched[] = {0x34, 0x00, 0xff, 4, 0xf0, 3, 0, 3};
    pb_peri_parse(patched, sizeof patched, &e);
    CHECK(e.kind == PbPeriPatchNtf && e.status == 0 && e.ss == 3);
    for (uint8_t kind = 4; kind <= 7; ++kind) {
        const uint8_t dump[] = {0x34, 0x00, 0xff, 4, 0xf0, kind, 0, 0};
        pb_peri_parse(dump, sizeof dump, &e);
        CHECK(e.kind == (kind == 6 ? PbPeriOther : PbPeriDump));
    }
    pb_peri_parse(cc, 5, &e);
    CHECK(e.kind == PbPeriOther);
    const uint8_t not_vendor[] = {0x34, 0x00, 0x0e, 4, 0xf0, 1, 0, 0, 0, 0, 0, 0};
    pb_peri_parse(not_vendor, sizeof not_vendor, &e);
    CHECK(e.kind == PbPeriOther);

    /* 跟踪 15.11 起的真实应答 */
    const uint8_t ack[] = {0x04, 0x0e, 0x05, 0x01, 0x00, 0xfc, 0x00, 0x1e};
    uint16_t op = 0;
    uint8_t st = 0xff;
    int sub = 0;
    CHECK(pb_hci_complete(ack, sizeof ack, &op, &st, &sub) == 1);
    CHECK(op == PbOpEdl && st == 0 && sub == PbEdlDownload);
    const uint8_t reset[] = {0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00};
    CHECK(pb_hci_complete(reset, sizeof reset, &op, &st, &sub) == 1);
    CHECK(op == PbOpReset && st == 0 && sub == -1);
    const uint8_t other[] = {0x04, 0x13, 0x05, 0x01, 0x00, 0xfc, 0x00, 0x1e};
    CHECK(pb_hci_complete(other, sizeof other, &op, &st, &sub) == 0);

    uint8_t ver[21] = {0x04, 0x0e, 18, 1, 0x00, 0xfc, 0x00, 0x19, 0x00, 0x1e, 0, 0, 0, 0x34, 0x12, 0x00, 0x02, 0x00, 0x02, 0x21, 0x40};
    PbEdlVersion ev;
    CHECK(pb_edl_version(ver, sizeof ver, &ev) == 0);
    CHECK(ev.product == 0x1e && ev.patch == 0x1234 && ev.build == 0x0200 && ev.soc == 0x40210200);
    CHECK(pb_edl_version(ver, 20, &ev) < 0);
    const uint8_t board[] = {0x04, 0x0e, 8, 1, 0x00, 0xfc, 0x00, 0x23, 2, 0x00, 0x33};
    uint8_t hi = 9, lo = 9;
    CHECK(pb_edl_board_id(board, sizeof board, &hi, &lo) == 0 && hi == 0 && lo == 0x33);
    /* 版本串：跟踪 15.301529 的开头 */
    const uint8_t label[] = {0x04, 0x0e, 0x6b, 0x01, 0x00, 0xfc, 0x00, 0x20, 5, 'B', 'T', 'F', 'W', '.'};
    const uint8_t *text = NULL;
    uint32_t length = 0;
    CHECK(pb_edl_build_label(label, sizeof label, &text, &length) == 0 && length == 5 && text[0] == 'B');
    CHECK(pb_edl_build_label(label, sizeof label - 1, &text, &length) < 0);
}

static void arguments(void)
{
    PbArg a;
    pb_arg_parse(0, &a);
    CHECK(a.phase == 0);
    pb_arg_parse(1, &a);
    CHECK(a.phase == 1 && a.stop == 0 && a.baud == 115200);
    pb_arg_parse(2, &a);
    CHECK(a.phase == 2 && a.baud == 115200);
    pb_arg_parse(3, &a);
    CHECK(a.phase == 3);
    pb_arg_parse(4, &a);
    CHECK(a.phase == 4 && a.baud == 115200 && !a.stand_in && a.stop == 0);
    pb_arg_parse(43, &a);
    CHECK(a.phase == 4 && a.baud == 3000000 && !a.stand_in);
    pb_arg_parse(48, &a);
    CHECK(a.phase == 4 && a.baud == 8000000 && !a.stand_in);
    pb_arg_parse(40, &a);
    CHECK(a.phase == 4 && a.stand_in == 1);
    pb_arg_parse(1, &a);                               /* 上一次的 stand_in 不留下来 */
    CHECK(!a.stand_in);
    pb_arg_parse(44, &a);
    CHECK(a.phase == 0);
    pb_arg_parse(5, &a);
    CHECK(a.phase == 0);
    pb_arg_parse(601, &a);                             /* 阶段 4 的步号不能当停止点 */
    CHECK(a.phase == 0);
    pb_arg_parse(23, &a);
    CHECK(a.phase == 2 && a.baud == 3000000);
    pb_arg_parse(28, &a);
    CHECK(a.phase == 2 && a.baud == 8000000);
    pb_arg_parse(38, &a);
    CHECK(a.phase == 3 && a.baud == 8000000);
    pb_arg_parse(24, &a);
    CHECK(a.phase == 0);
    static const unsigned one[] = {101, 102, 201, 202, 301, 302, 303, 304};
    for (unsigned i = 0; i < sizeof one / sizeof one[0]; ++i) {
        pb_arg_parse(one[i], &a);
        CHECK(a.phase == 1 && a.stop == (int)one[i]);
    }
    pb_arg_parse(407, &a);
    CHECK(a.phase == 2 && a.stop == 407);
    pb_arg_parse(501, &a);
    CHECK(a.phase == 3 && a.stop == 501);
    pb_arg_parse(103, &a);
    CHECK(a.phase == 0);
    pb_arg_parse(999, &a);
    CHECK(a.phase == 0);
}

static void addresses(void)
{
    uint8_t a[6] = {0}, b[6] = {0};
    CHECK(pb_bdaddr_from_seed(0, a) == 0);
    CHECK(pb_bdaddr_from_seed(0x1122334455667788ull, a) == 1);
    CHECK(pb_bdaddr_from_seed(0x1122334455667788ull, b) == 1);
    CHECK(!memcmp(a, b, 6));                           /* 每次开机相同 */
    CHECK(pb_bdaddr_from_seed(0x1122334455667789ull, b) == 1);
    CHECK(memcmp(a, b, 6));
    /* 这个种子的原始散列正好落进保留 LAP，要被挪开 */
    CHECK(pb_bdaddr_from_seed(0xe90733c4e06ace4aull, a) == 1);
    CHECK(a[0] == 0x38 && a[1] == 0x8b && a[2] == 0x9f && a[3] == 0xdf);
    /* 扫一批种子：保留的 LAP 0x9e8b00..0x9e8b3f 永远不出现 */
    for (uint64_t seed = 1; seed < 20000; ++seed) {
        CHECK(pb_bdaddr_from_seed(seed * 0x9e3779b97f4a7c15ull, a) == 1);
        CHECK(!(a[2] == 0x9e && a[1] == 0x8b && a[0] < 0x40));
    }
    /* 书写顺序是线上顺序反过来 */
    const uint8_t wire[6] = {0x30, 0x36, 0xa9, 0x75, 0xbd, 0x0c};
    pb_bdaddr_written(wire, a);
    CHECK_BYTES(a, 6, 0x0c, 0xbd, 0x75, 0xa9, 0x36, 0x30);
}

static void crashes(void)
{
    const uint8_t dump[] = {0x04, 0xff, 0x03, 0x01, 0x08, 0x00};
    const uint8_t dump2[] = {0x04, 0xff, 0x03, 0x01, 0x09, 0x00};
    const uint8_t other[] = {0x04, 0xff, 0x03, 0x01, 0x0a, 0x00};
    const uint8_t other2[] = {0x04, 0xff, 0x03, 0x02, 0x08, 0x00};
    const uint8_t cc[] = {0x04, 0x0e, 0x04, 0x01, 0x08, 0x00, 0x00};
    const uint8_t peri_dump[] = {0x34, 0x00, 0xff, 0x04, 0xf0, 0x05, 0x00, 0x00};
    const uint8_t peri_ntf[] = {0x34, 0x00, 0xff, 0x04, 0xf0, 0x03, 0x00, 0x00};
    const uint8_t acl[] = {0x02, 0xff, 0x03, 0x01, 0x08, 0x00};
    CHECK(pb_frame_crashed(dump, sizeof dump) && pb_frame_crashed(dump2, sizeof dump2));
    CHECK(!pb_frame_crashed(other, sizeof other) && !pb_frame_crashed(other2, sizeof other2));
    CHECK(!pb_frame_crashed(cc, sizeof cc) && !pb_frame_crashed(acl, sizeof acl));
    CHECK(pb_frame_crashed(peri_dump, sizeof peri_dump) && !pb_frame_crashed(peri_ntf, sizeof peri_ntf));
    CHECK(!pb_frame_crashed(dump, 4) && !pb_frame_crashed(dump, 0));
}

int main(void)
{
    frames();
    segments();
    nvm();
    events();
    arguments();
    addresses();
    crashes();
    printf("PASS tlv: %u checks\n", checks);
    return 0;
}
