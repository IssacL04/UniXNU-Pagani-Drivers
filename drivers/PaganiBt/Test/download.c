/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtDownload.c 对控制器模型：整条序列跑通，下到模型里的镜像逐字节等于文件（NVM 只差 Tag 改写），逐段等待规则，换速顺序，各种补丁状态，
 * 控制器报错、沉默、转储时停在对的步上，stop 选项之后不再发任何字节，坏 NVM 在发第一个字节之前被拒。 */
#include "../BtDownload.h"
#include "ctrl_model.h"
#include "check.h"

#define BIG (300u * 1024u)
static uint8_t store[CmBufCount][BIG];
static uint8_t *const buffers[CmBufCount] = {store[0], store[1], store[2], store[3], store[4]};
static CtrlModel ctrl;
static uint64_t now_ms;
static uint32_t tx_total;
static int rfr_ready = 1, rfr_events[8], rfr_count, write_fail_after = -1;
static uint32_t baud_calls[4], baud_count;
static char last_line[200];
static unsigned log_lines, fail_lines;
static uint64_t next_advert, baud_cmd_at, baud_host_at;

static int x_write(void *c, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    (void)c;
    (void)timeout_ms;
    if (write_fail_after == 0) return -3;
    if (write_fail_after > 0) --write_fail_after;
    if (bytes == 7 && data[0] == 0x31 && data[2] == 0xf1 && data[5] == PbSubBaud) baud_cmd_at = now_ms;
    for (uint32_t i = 0; i < bytes; ++i) cm_rx(&ctrl, data[i]);
    tx_total += bytes;
    return 0;
}
static uint32_t x_read(void *c, uint8_t *data, uint32_t max)
{
    (void)c;
    if (!rfr_ready) return 0;                          /* RFR 手动未就绪：控制器发不出来 */
    if (ctrl.scanning && now_ms >= next_advert) {
        static const uint8_t report[] = {PbH4Evt, PbEvtLeMeta, 12, PbLeAdvReport, 1, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0xc0};
        cm_out(&ctrl, report, sizeof report);
        next_advert = now_ms + 10;
    }
    uint32_t n = 0;
    while (n < max && cm_pending(&ctrl)) data[n++] = cm_get(&ctrl);
    if (!cm_pending(&ctrl)) ctrl.pending_acks = 0;
    return n;
}
static void x_sleep(void *c, unsigned ms) {(void)c; now_ms += ms;}
static uint64_t x_now(void *c) {(void)c; return now_ms;}
static int x_rfr(void *c, int ready)
{
    (void)c;
    rfr_ready = ready;
    if (rfr_count < 8) rfr_events[rfr_count++] = ready;
    return 0;
}
static int x_baud(void *c, uint32_t baud)
{
    (void)c;
    CHECK(!rfr_ready);                                 /* 改主机时钟时 RFR 必须是关的 */
    CHECK(baud_count < 4);
    baud_calls[baud_count++] = baud;
    baud_host_at = now_ms;
    ctrl.baud = baud;
    return 0;
}
static void x_log(void *c, const char *line)
{
    (void)c;
    ++log_lines;
    if (strstr(line, "FAIL")) ++fail_lines;
    strncpy(last_line, line, sizeof last_line - 1);
}
static const PbXport kX = {NULL, x_write, x_read, x_sleep, x_now, x_rfr, x_baud, x_log};

/* ---- 文件 ---- */
static uint8_t peri[BIG], tmel[BIG], bt[BIG], peri_nvm[4096], bt_nvm[8192], peri_nvm_orig[4096], bt_nvm_orig[8192];
static uint32_t peri_n, tmel_n, bt_n, peri_nvm_n, bt_nvm_n;

static uint32_t rng_state = 0x1101;
static uint32_t rng(void) {rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state;}
static void fill_elf(uint8_t *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) d[i] = (uint8_t)rng();
    d[0] = 0x7f;
    d[1] = 'E';
    d[2] = 'L';
    d[3] = 'F';
}
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
static uint32_t make_nvm(uint8_t *d, int with_addr, uint32_t padding)
{
    static const uint8_t addr[6] = {0xad, 0x5a, 0x01, 0x00, 0x55, 0x78};
    static const uint8_t hci[6] = {0x15, 0x11, 0x22, 0x00, 0x2e, 0x00};
    static const uint8_t deep[14] = {0x01, 0x00, 0x0a, 0x28};
    static const uint8_t asd[2] = {0x01, 0x00};
    static uint8_t pad[4096];
    uint32_t p = 4;
    p += put_tag(d + p, 1, pad, 64);
    if (with_addr) p += put_tag(d + p, PbTagBdAddr, addr, 6);
    p += put_tag(d + p, PbTagHci, hci, 6);
    p += put_tag(d + p, PbTagDeepSleep, deep, 14);
    p += put_tag(d + p, PbTagAsd, asd, 2);
    p += put_tag(d + p, 99, pad, (uint16_t)padding);
    const uint32_t len = p - 4;
    d[0] = PbTlvNvm;
    d[1] = (uint8_t)len;
    d[2] = (uint8_t)(len >> 8);
    d[3] = 0;
    return p;
}
static uint32_t tag_offset(const uint8_t *d, uint16_t wanted)
{
    uint32_t p = 4;
    for (;;) {
        const uint16_t tag = (uint16_t)(d[p] | d[p + 1] << 8), size = (uint16_t)(d[p + 2] | d[p + 3] << 8);
        if (tag == wanted) return p + 12;
        p += 12u + size;
    }
}
/* 小文件给失败路径用，大文件给完整路径用。 */
static void make_files(int big)
{
    peri_n = big ? 25420 : 1000;
    tmel_n = big ? 55204 : 700;
    bt_n = big ? 228872 : 1500;
    fill_elf(peri, peri_n);
    fill_elf(tmel, tmel_n);
    fill_elf(bt, bt_n);
    peri_nvm_n = make_nvm(peri_nvm, 0, 600);
    bt_nvm_n = make_nvm(bt_nvm, 1, 4000);
    memcpy(peri_nvm_orig, peri_nvm, peri_nvm_n);
    memcpy(bt_nvm_orig, bt_nvm, bt_nvm_n);
}
static const uint8_t kAddress[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

static PbDl dl;
static PbDlConfig base_config(uint32_t baud)
{
    PbDlConfig c;
    memset(&c, 0, sizeof c);
    c.baud = baud;
    c.bdaddr = kAddress;
    c.peri = peri;
    c.peri_bytes = peri_n;
    c.peri_nvm = peri_nvm;
    c.peri_nvm_bytes = peri_nvm_n;
    c.tmel = tmel;
    c.tmel_bytes = tmel_n;
    c.bt = bt;
    c.bt_bytes = bt_n;
    c.bt_nvm = bt_nvm;
    c.bt_nvm_bytes = bt_nvm_n;
    return c;
}
static void start(int big, uint32_t baud)
{
    make_files(big);
    memset(store, 0, sizeof store);
    cm_init(&ctrl, buffers);
    now_ms = 1000;
    tx_total = 0;
    rfr_ready = 1;
    rfr_count = 0;
    baud_count = 0;
    log_lines = fail_lines = 0;
    write_fail_after = -1;
    next_advert = 0;
    const PbDlConfig c = base_config(baud);
    pb_dl_init(&dl, &kX, &c);
}
static void handshake(void)
{
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(pb_dl_version(&dl) == 0);
}

static void happy(uint32_t baud, uint8_t code)
{
    start(1, baud);
    /* 握手判据：0xfc 和 0x34 版本应答 */
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(dl.step == PbStepWake && ctrl.wakes == 1 && dl.ibs.acks_seen == 1);
    CHECK(pb_dl_version(&dl) == 0);
    CHECK(dl.step == PbStepVersion);
    CHECK(dl.peri_version.product == 0x1e && dl.peri_version.soc == 0x40210200 && dl.peri_version.patch == 0x1234 && dl.peri_version.build == 0x0200);
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(dl.step == PbStepReset);
    CHECK(pb_dl_check(&dl) == 0);
    CHECK(dl.step == PbStepCheck && dl.hci_ok);
    CHECK(!ctrl.violations && !ctrl.hci_ignored && !fail_lines);
    CHECK(ctrl.peri_done && ctrl.tmel_done && ctrl.active && ctrl.reset_done);
    CHECK(ctrl.wakes == 2);                            /* HAL：TME-L 之前再唤醒一次 */

    /* 收尾命令的顺序和 Android 一样：附加特性、读 OTP、日志设置、CoP 版本、HCI_Reset，然后校验的两条 */
    static const uint16_t tail[] = {PbOpAddOn, PbOpOtp, PbOpLog, PbOpLog, PbOpReset, PbOpLocalVersion, PbOpBdAddr};
    CHECK(ctrl.history_count == sizeof tail / sizeof tail[0]);
    CHECK(!memcmp(ctrl.history, tail, sizeof tail));

    /* 字节：ELF 原样，NVM 只差四个 Tag */
    CHECK(ctrl.image_len[CmBufPeri] == peri_n && !memcmp(store[CmBufPeri], peri, peri_n));
    CHECK(ctrl.image_len[CmBufTmel] == tmel_n && !memcmp(store[CmBufTmel], tmel, tmel_n));
    CHECK(ctrl.image_len[CmBufBt] == bt_n && !memcmp(store[CmBufBt], bt, bt_n));
    CHECK(ctrl.image_len[CmBufPeriNvm] == peri_nvm_n && ctrl.image_len[CmBufBtNvm] == bt_nvm_n);
    uint8_t want_bt[8192], want_peri[4096];
    memcpy(want_bt, bt_nvm_orig, bt_nvm_n);
    memcpy(want_peri, peri_nvm_orig, peri_nvm_n);
    want_bt[tag_offset(bt_nvm_orig, PbTagHci) + 1] = code;
    want_peri[tag_offset(peri_nvm_orig, PbTagHci) + 1] = code;
    memcpy(want_bt + tag_offset(bt_nvm_orig, PbTagBdAddr), kAddress, 6);
    want_bt[tag_offset(bt_nvm_orig, PbTagAsd)] &= 0xfe;
    want_bt[tag_offset(bt_nvm_orig, PbTagDeepSleep) + 1] |= 1;
    want_peri[tag_offset(peri_nvm_orig, PbTagDeepSleep) + 1] |= 1;
    CHECK(!memcmp(store[CmBufBtNvm], want_bt, bt_nvm_n));
    CHECK(!memcmp(store[CmBufPeriNvm], want_peri, peri_nvm_n));

    /* 段数 */
    uint32_t last;
    CHECK(ctrl.peri_segments == pb_tlv_segments(peri_n, &last));
    CHECK(ctrl.tmel_segments == pb_tlv_segments(tmel_n, &last));
    CHECK(ctrl.bt_segments == pb_tlv_segments(bt_n, &last));
    CHECK(ctrl.bt_nvm_segments == pb_tlv_segments(bt_nvm_n, &last));

    /* LE 扫描：数到广播，结束时关掉 */
    CHECK(pb_dl_le_scan(&dl, 5) == 0);
    CHECK(dl.step == PbStepScan && !ctrl.scanning);
    CHECK(dl.adverts >= 400 && dl.adverts <= 501);
}

static void baud_order(void)
{
    start(0, 8000000);
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(baud_count == 1 && baud_calls[0] == 8000000);
    CHECK(ctrl.requested_baud == 0x15);
    CHECK(rfr_count == 2 && rfr_events[0] == 0 && rfr_events[1] == 1);   /* 先关 RFR，改完时钟才开 */
    CHECK(baud_host_at - baud_cmd_at >= 20);           /* SetPeriBaudRateReq：发完等 20 ms 才改主机时钟 */

    start(0, 3000000);
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(ctrl.requested_baud == 0x0e && store[CmBufBtNvm][tag_offset(bt_nvm_orig, PbTagHci) + 1] == 0x0e);

    /* 不换速：不发设波特率，主机时钟不动 */
    start(0, 115200);
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(baud_count == 0 && rfr_count == 0 && !ctrl.requested_baud);
    CHECK(store[CmBufBtNvm][tag_offset(bt_nvm_orig, PbTagHci) + 1] == 0x00);
}

static void statuses(void)
{
    /* 外设已经打过补丁：不下外设，TME-L 和激活照走 */
    start(0, 115200);
    ctrl.peri_status = PbPatchDone;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(!ctrl.image_len[CmBufPeri] && ctrl.image_len[CmBufTmel] == tmel_n && ctrl.active);
    CHECK(!ctrl.violations);

    /* 别的子系统正在打：等通知 */
    start(0, 115200);
    ctrl.peri_status = PbPatchBusy;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(!ctrl.image_len[CmBufPeri] && dl.peri_status == PbPatchDone);

    /* TME-L 不存在：跳过，不取版本 */
    start(0, 115200);
    ctrl.tmel_status = PbTmelAbsent1;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(!ctrl.image_len[CmBufTmel]);
    start(0, 115200);
    ctrl.tmel_status = PbTmelAbsent2;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    /* TME-L 已打过、正在打 */
    start(0, 115200);
    ctrl.tmel_status = PbPatchDone;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0 && !ctrl.image_len[CmBufTmel]);
    start(0, 115200);
    ctrl.tmel_status = PbPatchBusy;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0 && !ctrl.image_len[CmBufTmel]);

    /* ActivateSS 用状态事件带 23 答：不再等完成事件 */
    start(0, 115200);
    ctrl.status_form_activate = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);

    /* 补丁通知比复位应答先到：不丢 */
    start(0, 115200);
    ctrl.ntf_before_reply = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);

    /* 应答之间夹着控制器日志和 IBS：日志丢弃，0xfd 回 0xfc */
    start(0, 115200);
    ctrl.acl_noise = ctrl.sleep_noise = 1;
    handshake();
    const uint32_t ignored = dl.ibs.wakes_seen;
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(pb_dl_check(&dl) == 0);
    CHECK(dl.acl_logs > 0 && dl.ibs.wakes_seen > ignored && dl.ibs.acks_sent == dl.ibs.wakes_seen);
    CHECK(ctrl.wakes_sent > 0 && ctrl.host_acks == ctrl.wakes_sent);   /* 模型发的每个 0xfd 都收到了 0xfc */
    CHECK(!ctrl.violations);

    /* ELF 途中来个不相干的事件：不会读乱 */
    start(0, 115200);
    ctrl.elf_ack_after = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
}

static void expect_fail(int step, int error)
{
    CHECK(dl.step == -step);
    CHECK(dl.error == error);
    CHECK(fail_lines >= 1);
}

static void failures(void)
{
    /* 控制器不答：试满 10 次，报超时 */
    start(0, 115200);
    ctrl.mute = 1;
    CHECK(pb_dl_wake(&dl) == PbErrTimeout);
    CHECK(ctrl.wakes == PbIbsTriesBoot);
    expect_fail(PbStepWake, PbErrTimeout);
    /* 只答一次 0xfc 以外什么都不答 */
    start(0, 115200);
    ctrl.version_mute = 1;
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(pb_dl_version(&dl) == PbErrTimeout);
    expect_fail(PbStepVersion, PbErrTimeout);
    /* 版本应答状态错 */
    start(0, 115200);
    ctrl.peri_version_bad = 1;
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(pb_dl_version(&dl) == PbErrStatus);
    /* 未知的补丁状态 */
    start(0, 115200);
    ctrl.peri_status = 7;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    expect_fail(PbStepPeri, PbErrStatus);
    CHECK(!ctrl.image_len[CmBufPeri]);
    /* 仲裁答错子系统：一个字节的固件都不发 */
    start(0, 115200);
    handshake();
    ctrl.wrong_arb_ss = 1;
    CHECK(pb_dl_boot(&dl) == PbErrProtocol);
    CHECK(!ctrl.image_len[CmBufPeri]);
    /* NVM 段的应答答错子系统 */
    start(0, 115200);
    handshake();
    ctrl.wrong_ss = 1;
    CHECK(pb_dl_boot(&dl) == PbErrProtocol);
    /* 段被拒：ELF 最后一段 */
    start(0, 115200);
    ctrl.seg_status = 3;                               /* SIGNATURE_ERROR */
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    expect_fail(PbStepPeri, PbErrStatus);
    CHECK(!ctrl.active);
    /* 段被拒：NVM */
    start(0, 115200);
    ctrl.nvm_seg_status = 5;                           /* NVM_TAG_ERROR */
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    /* 补丁通知带错 */
    start(0, 115200);
    ctrl.ntf_status = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    expect_fail(PbStepPeri, PbErrStatus);
    /* 激活时转储 */
    start(0, 115200);
    ctrl.dump_on_activate = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrDump);
    expect_fail(PbStepActive, PbErrDump);
    /* 激活没有完成事件：不往下走，HCI 这时没人理 */
    start(0, 115200);
    ctrl.no_activated = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrTimeout);
    expect_fail(PbStepActive, PbErrTimeout);
    CHECK(!ctrl.image_len[CmBufBt] && !ctrl.hci_ignored);
    /* BT 固件的段被拒：签名错 */
    start(0, 115200);
    ctrl.bt_seg_status = 3;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    expect_fail(PbStepBtFw, PbErrStatus);
    CHECK(!ctrl.image_len[CmBufBtNvm]);
    /* BT NVM 的段被拒 */
    start(0, 115200);
    ctrl.bt_nvm_seg_status = 5;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrStatus);
    expect_fail(PbStepBtNvm, PbErrStatus);
    CHECK(ctrl.image_len[CmBufBtNvm] == 243 && !ctrl.reset_done);
    /* HCI 上报硬件错误 */
    start(0, 115200);
    ctrl.hw_error_on_bt = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == PbErrHwError);
    expect_fail(PbStepBtFw, PbErrHwError);
    /* 中途沉默 */
    start(0, 115200);
    handshake();
    ctrl.mute = 1;
    CHECK(pb_dl_boot(&dl) == PbErrTimeout);
    expect_fail(PbStepPeri, PbErrTimeout);
    /* 写失败 */
    start(0, 115200);
    handshake();
    write_fail_after = 3;
    CHECK(pb_dl_boot(&dl) == PbErrWrite);
    /* 校验：身份不对、地址不对、没有版本串 */
    start(0, 115200);
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    ctrl.wrong_local_version = 1;
    CHECK(pb_dl_check(&dl) == PbErrIdentity);
    expect_fail(PbStepCheck, PbErrIdentity);
    start(0, 115200);
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    ctrl.wrong_bd_addr = 1;
    CHECK(pb_dl_check(&dl) == PbErrIdentity);
    start(0, 115200);
    ctrl.no_label = 1;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
    CHECK(pb_dl_check(&dl) == PbErrIdentity);
    CHECK(!dl.hci_ok);

    /* 坏 NVM：第一个字节发出去之前就停 */
    start(0, 115200);
    handshake();
    const uint32_t sent = tx_total;
    bt_nvm[1] = 0xff;
    bt_nvm[2] = 0xff;
    CHECK(pb_dl_boot(&dl) == PbErrNvm);
    CHECK(tx_total == sent);
    /* 缺 Tag 17 */
    start(0, 115200);
    handshake();
    peri_nvm[tag_offset(peri_nvm, PbTagHci) - 12] = 0x55;
    CHECK(pb_dl_boot(&dl) == PbErrNvm);
    CHECK(tx_total == sent);
    /* 要写地址却没有 Tag 2 */
    start(0, 115200);
    handshake();
    bt_nvm[tag_offset(bt_nvm, PbTagBdAddr) - 12] = 0x55;
    CHECK(pb_dl_boot(&dl) == PbErrNvm);
    CHECK(tx_total == sent);
    /* 不写地址时占位值留着，也不要求 Tag 2 */
    start(0, 115200);
    dl.cfg.bdaddr = NULL;
    bt_nvm[tag_offset(bt_nvm, PbTagBdAddr) - 12] = 0x55;
    handshake();
    CHECK(pb_dl_boot(&dl) == 0);
}

static void stops(void)
{
    static const int steps[] = {PbStepBaud, PbStepPeri, PbStepTmel, PbStepActive, PbStepBtFw, PbStepBtNvm, PbStepReset};
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        start(0, 115200);
        dl.cfg.stop = steps[i];
        handshake();
        CHECK(pb_dl_boot(&dl) == PbErrStopped);
        CHECK(dl.step == steps[i] && !dl.error);
        CHECK(!fail_lines);
        CHECK(ctrl.active == (steps[i] >= PbStepActive));
        CHECK(!!ctrl.image_len[CmBufBt] == (steps[i] >= PbStepBtFw));
        CHECK(ctrl.reset_done == (steps[i] >= PbStepReset));
    }
    start(0, 115200);
    dl.cfg.stop = PbStepWake;
    CHECK(pb_dl_wake(&dl) == PbErrStopped);
    start(0, 115200);
    dl.cfg.stop = PbStepVersion;
    CHECK(pb_dl_wake(&dl) == 0);
    CHECK(pb_dl_version(&dl) == PbErrStopped);
    /* 304 只记录：控制器此刻不理 HCI 也不算失败 */
    start(0, 115200);
    handshake();
    dl.cfg.stop = PbStepHciProbe;
    CHECK(pb_dl_hci_probe(&dl) == PbErrStopped);
    CHECK(ctrl.hci_ignored == 1 && dl.step == PbStepHciProbe);
}

static void handover(void)
{
    /* 交给桥之前再唤一次，和 Android 在 HCI_Reset 之后第一次发送之前的 0xfd 一样 */
    start(0, 115200);
    handshake();
    const uint32_t wakes = ctrl.wakes;
    CHECK(pb_dl_handover(&dl) == 0 && dl.step == PbStepHandover && ctrl.wakes == wakes + 1);
    /* 没有 0xfc：HAL 也往下走，这里只记录 */
    ctrl.no_wake_ack = 1;
    CHECK(pb_dl_handover(&dl) == 0 && dl.step == PbStepHandover && !fail_lines);
    /* 控制器这时候崩了：不能交出去 */
    start(0, 115200);
    handshake();
    static const uint8_t dump[] = {PbH4Evt, PbEvtVendor, 3, 1, 8, 0};
    cm_out(&ctrl, dump, sizeof dump);
    CHECK(pb_dl_handover(&dl) == PbErrDump && dl.step == -PbStepHandover && fail_lines == 1);
}

int main(void)
{
    happy(115200, 0x00);
    happy(3000000, 0x0e);
    happy(8000000, 0x15);
    baud_order();
    statuses();
    failures();
    stops();
    handover();
    printf("PASS download: %u checks\n", checks);
    return 0;
}
