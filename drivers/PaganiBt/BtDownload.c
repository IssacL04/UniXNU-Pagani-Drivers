/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtDownload.h"
#include "BtUtil.h"
#include "PaganiBtBoard.h"

enum {
    kWakeWriteMs = 10,              /* 一次 0xfd：HAL 的 select 是 10 ms，CTS 不放时也在这个窗口里放弃 */
    kWriteMs = 2000,
    kReplyMs = 3000,                /* 跟踪里最长的应答是最后一段 NVM，约 160 ms；115200 下再放宽 */
    kNotifyMs = 5000,
    kBaudSettleMs = 20,             /* SetPeriBaudRateReq：usleep(20000) */
    kLogFrames = 64,                /* 原样记录的帧数上限，别冲掉 ramoops */
    kHexBytes = 40,
};

static void say(PbDl *d, const char *line)
{
    if (d->x->log) d->x->log(d->x->ctx, line);
}
static void say_kx(PbDl *d, const char *what, const char *k1, uint64_t v1, const char *k2, uint64_t v2)
{
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] ");
    pb_text_str(&t, what);
    if (k1) pb_text_kx(&t, k1, v1);
    if (k2) pb_text_kx(&t, k2, v2);
    say(d, line);
}
static int done(PbDl *d, int step)
{
    d->step = step;
    return d->cfg.stop == step ? PbErrStopped : 0;
}
static int fail(PbDl *d, int step, int error, const char *what)
{
    char line[160];
    PbText t;
    d->step = -step;
    d->error = error;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] FAIL");
    pb_text_kd(&t, "step", step);
    pb_text_kd(&t, "err", error);
    pb_text_char(&t, ' ');
    pb_text_str(&t, what);
    say(d, line);
    return error;
}
static void log_frame(PbDl *d, const uint8_t *f, uint32_t n)
{
    if (d->log_budget >= kLogFrames) return;
    d->log_budget++;
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] rx");
    pb_text_kd(&t, "n", n);
    pb_text_bytes(&t, f, n < kHexBytes ? n : kHexBytes);
    say(d, line);
}

static int send(PbDl *d, const uint8_t *p, uint32_t n, unsigned timeout_ms)
{
    const int rc = d->x->write(d->x->ctx, p, n, timeout_ms);
    if (rc) return PbErrWrite;
    d->packets++;
    d->bytes += n;
    return 0;
}

static void on_ibs(PbDl *d, uint8_t byte)
{
    const unsigned action = pb_ibs_rx(&d->ibs, byte, d->x->now_ms(d->x->ctx));
    if (action & PbIbsSendAck) {
        const uint8_t ack = PbIbsWakeAck;
        (void)send(d, &ack, 1, kWriteMs);
    }
}

/* 下一帧：1 有帧（d->h4.buf，d->h4.have），0 到点。途中的 IBS 字节就地处理，0xfd 回 0xfc，这是 ReadHciPeriEvent 的做法。 */
static int next_frame(PbDl *d, uint64_t deadline)
{
    for (;;) {
        while (d->rx_pos < d->rx_len) {
            const int r = pb_h4_rx_byte(&d->h4, d->rx[d->rx_pos++]);
            if (r == PbH4Ibs) {
                on_ibs(d, d->h4.buf[0]);
            } else if (r == PbH4Frame) {
                d->frames++;
                return 1;
            }
        }
        d->rx_pos = 0;
        d->rx_len = d->x->read(d->x->ctx, d->rx, sizeof d->rx);
        if (d->rx_len) continue;
        if (d->x->now_ms(d->x->ctx) >= deadline) return 0;
        d->x->sleep_ms(d->x->ctx, 1);
    }
}

/* 等待命令应答时顺手处理的事：补丁通知可能先到，控制器日志和厂商事件不算错。负数表示控制器出事了。 */
static int note_frame(PbDl *d, const uint8_t *f, uint32_t n, PbPeriEvent *e)
{
    e->kind = PbPeriOther;
    if (f[0] == PbH4Peri) {
        log_frame(d, f, n);
        pb_peri_parse(f, n, e);
        if (e->kind == PbPeriPatchNtf) {
            if (e->ss == PbSsPeri) d->peri_ntf = e->status == 0 ? 1 : 2;
            if (e->ss == PbSsTmel) d->tmel_ntf = e->status == 0 ? 1 : 2;
        } else if (e->kind == PbPeriActivated) {
            d->activated = 1;
        } else if (e->kind == PbPeriDump) {
            return PbErrDump;
        }
    } else if (f[0] == PbH4Evt) {
        if (f[1] == PbEvtHardwareError) {
            log_frame(d, f, n);
            return PbErrHwError;
        }
        if (pb_frame_crashed(f, n)) {
            log_frame(d, f, n);
            return PbErrDump;
        }
        if (f[1] == PbEvtLeMeta && n >= 5 && f[3] == PbLeAdvReport) d->adverts += f[4];
    } else if (f[0] == PbH4Acl) {
        if (pb_h4_filter_rx(f, n) == PbFilterDrop) d->acl_logs++;
    }
    return 0;
}

/* 等 0x34 的完成或状态事件。 */
static int peri_wait(PbDl *d, uint16_t opcode, uint8_t sub, PbPeriEvent *e, unsigned ms)
{
    const uint64_t deadline = d->x->now_ms(d->x->ctx) + ms;
    for (;;) {
        const int got = next_frame(d, deadline);
        if (!got) return PbErrTimeout;
        const uint8_t *f = d->h4.buf;
        const int rc = note_frame(d, f, d->h4.have, e);
        if (rc) return rc;
        if (e->kind == PbPeriComplete && e->opcode == opcode && e->sub == sub) return 0;
        if (e->kind == PbPeriStatus && e->opcode == opcode) return 0;
        d->ignored++;
    }
}
/* 等补丁通知；*flag 由 note_frame 置位，通知比应答先到也算数。 */
static int notify_wait(PbDl *d, const uint8_t *flag, unsigned ms)
{
    const uint64_t deadline = d->x->now_ms(d->x->ctx) + ms;
    PbPeriEvent e;
    while (!*flag) {
        const int got = next_frame(d, deadline);
        if (!got) return PbErrTimeout;
        const int rc = note_frame(d, d->h4.buf, d->h4.have, &e);
        if (rc) return rc;
    }
    return *flag == 1 ? 0 : PbErrStatus;
}
static int activated_wait(PbDl *d, unsigned ms)
{
    const uint64_t deadline = d->x->now_ms(d->x->ctx) + ms;
    PbPeriEvent e;
    while (!d->activated) {
        const int got = next_frame(d, deadline);
        if (!got) return PbErrTimeout;
        const int rc = note_frame(d, d->h4.buf, d->h4.have, &e);
        if (rc) return rc;
    }
    return 0;
}
/* 等 Command Complete。sub 小于 0 时只看命令号。 */
static int hci_wait(PbDl *d, uint16_t opcode, int sub, uint8_t *status, unsigned ms)
{
    const uint64_t deadline = d->x->now_ms(d->x->ctx) + ms;
    PbPeriEvent e;
    for (;;) {
        const int got = next_frame(d, deadline);
        if (!got) return PbErrTimeout;
        const uint8_t *f = d->h4.buf;
        const uint32_t n = d->h4.have;
        const int rc = note_frame(d, f, n, &e);
        if (rc) return rc;
        uint16_t op = 0;
        int s = -1;
        if (pb_hci_complete(f, n, &op, status, &s) && op == opcode && (sub < 0 || s == sub)) return 0;
        d->ignored++;
    }
}
/* 发一条 HCI 命令等完成，返回 0 或负数；*status 是状态字节。 */
static int hci_command(PbDl *d, uint16_t opcode, const uint8_t *params, unsigned plen, uint8_t *status)
{
    uint8_t p[PbPacketMax];
    const unsigned n = pb_hci_command(p, opcode, params, plen);
    const int rc = send(d, p, n, kWriteMs);
    if (rc) return rc;
    return hci_wait(d, opcode, -1, status, kReplyMs);
}

void pb_dl_init(PbDl *d, const PbXport *x, const PbDlConfig *cfg)
{
    pb_zero(d, sizeof *d);
    d->x = x;
    if (cfg) d->cfg = *cfg;
    pb_h4_rx_reset(&d->h4);
    pb_ibs_init(&d->ibs, PbIbsTriesBoot, 0);
    d->peri_status = d->tmel_status = 0xff;
    for (unsigned i = 0; i < PB_RATE_COUNT; ++i) if (kPbRates[i].baud == d->cfg.baud) d->code = kPbRates[i].code;
}

static int wake_soc(PbDl *d)
{
    const PbXport *x = d->x;
    const uint8_t wake = PbIbsWake;
    d->ibs.tx = PbIbsAsleep;
    unsigned action = pb_ibs_want_tx(&d->ibs, x->now_ms(x->ctx));
    while (d->ibs.tx != PbIbsAwake) {
        if (action & PbIbsFailed) return PbErrTimeout;
        /* CTS 不放时这次写在窗口内放弃，算一次没成功的尝试 */
        if (action & PbIbsSendWake) (void)send(d, &wake, 1, kWakeWriteMs);
        PbPeriEvent e;
        const int got = next_frame(d, x->now_ms(x->ctx) + 1);
        if (got) {
            const int rc = note_frame(d, d->h4.buf, d->h4.have, &e);
            if (rc) return rc;
        }
        action = pb_ibs_tick(&d->ibs, x->now_ms(x->ctx));
    }
    return 0;
}

int pb_dl_wake(PbDl *d)
{
    const int rc = wake_soc(d);
    say_kx(d, "wake", "fd", d->ibs.wakes_sent, "fc", d->ibs.acks_seen);
    if (rc) return fail(d, PbStepWake, rc, "no 0xfc from the controller");
    return done(d, PbStepWake);
}

static int get_version(PbDl *d, uint8_t ss, PbPeriVersion *v)
{
    uint8_t p[PbPacketMax];
    PbPeriEvent e;
    int rc = send(d, p, pb_peri_getver(p, ss), kWriteMs);
    if (!rc) rc = peri_wait(d, PbOpPeriEdl, PbSubGetVer, &e, kReplyMs);
    if (rc) return rc;
    if (e.status != 0 || e.ss != ss) return PbErrStatus;
    return pb_peri_version(d->h4.buf, d->h4.have, v) ? PbErrProtocol : 0;
}

int pb_dl_version(PbDl *d)
{
    int rc = get_version(d, PbSsPeri, &d->peri_version);
    if (rc) return fail(d, PbStepVersion, rc, "no 0x34 version answer");
    say_kx(d, "peri version", "product", d->peri_version.product, "soc", d->peri_version.soc);
    say_kx(d, "peri version", "patch", d->peri_version.patch, "build", d->peri_version.build);
    return done(d, PbStepVersion);
}

int pb_dl_hci_probe(PbDl *d)
{
    /* 跟踪里 Android 在固件下载完之前从不发 HCI，这一步只为看控制器此刻怎么反应，结果不作判据 */
    uint8_t status = 0xff;
    const int rc = hci_command(d, PbOpReset, NULL, 0, &status);
    say_kx(d, "hci reset probe", "rc", (uint64_t)(int64_t)rc, "status", status);
    return done(d, PbStepHciProbe);
}

static int baud_switch(PbDl *d)
{
    const PbXport *x = d->x;
    uint8_t p[PbPacketMax];
    PbPeriEvent e;
    if (x->rfr(x->ctx, 0)) return PbErrBaud;
    int rc = send(d, p, pb_peri_baud(p, d->code), kWriteMs);
    if (rc) return rc;
    x->sleep_ms(x->ctx, kBaudSettleMs);
    if (x->host_baud(x->ctx, d->cfg.baud)) return PbErrBaud;
    if (x->rfr(x->ctx, 1)) return PbErrBaud;
    rc = peri_wait(d, PbOpPeriGeneral, PbSubBaud, &e, kReplyMs);
    if (rc) return rc;
    if (e.status != 1) say_kx(d, "WARN baud answer", "status", e.status, NULL, 0);   /* HAL 也只是记录 */
    return 0;
}

/* 分段下发。ELF 只有最后一段等应答（SKIP_EVT_VSE_CC），NVM 每段都等。 */
static int download_peri(PbDl *d, uint8_t ss, const uint8_t *data, uint32_t bytes)
{
    uint32_t last = 0;
    const uint32_t count = pb_tlv_segments(bytes, &last);
    const int each = pb_tlv_wait_each(data, bytes);
    uint8_t p[PbPacketMax];
    if (!count) return PbErrArgument;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t n = i + 1 == count ? last : PbSegmentBytes;
        int rc = send(d, p, pb_peri_segment(p, ss, data + i * PbSegmentBytes, n), kWriteMs);
        if (rc) return rc;
        if (each || i + 1 == count) {
            PbPeriEvent e;
            if ((rc = peri_wait(d, PbOpPeriEdl, PbSubDownload, &e, kReplyMs))) return rc;
            if (e.status != 0) {
                say_kx(d, "segment refused", "status", e.status, "segment", i);
                return PbErrStatus;
            }
            if (ss == PbSsPeri && e.ss != 0) return PbErrProtocol;   /* TlvDnldSegment：Invalid/wrong Subsystem ID */
        }
    }
    return 0;
}

static int download_bt(PbDl *d, const uint8_t *data, uint32_t bytes)
{
    uint32_t last = 0;
    const uint32_t count = pb_tlv_segments(bytes, &last);
    const int each = pb_tlv_wait_each(data, bytes);
    uint8_t p[PbPacketMax];
    if (!count) return PbErrArgument;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t n = i + 1 == count ? last : PbSegmentBytes;
        int rc = send(d, p, pb_edl_segment(p, data + i * PbSegmentBytes, n), kWriteMs);
        if (rc) return rc;
        if (each || i + 1 == count) {
            uint8_t status = 0xff;
            if ((rc = hci_wait(d, PbOpEdl, PbEdlDownload, &status, kReplyMs))) return rc;
            if (status != 0) {
                say_kx(d, "segment refused", "status", status, "segment", i);
                return PbErrStatus;
            }
        }
    }
    return 0;
}

static int arbitrate(PbDl *d, uint8_t ss, uint8_t *status)
{
    uint8_t p[PbPacketMax];
    PbPeriEvent e;
    int rc = send(d, p, pb_peri_arbitrate(p, ss), kWriteMs);
    if (!rc) rc = peri_wait(d, PbOpPeriEdl, PbSubArbitrate, &e, kReplyMs);
    if (rc) return rc;
    if (e.ss != ss) return PbErrProtocol;
    *status = e.status;
    return 0;
}

static int patch_peri(PbDl *d)
{
    uint8_t p[PbPacketMax];
    PbPeriEvent e;
    int rc = arbitrate(d, PbSsPeri, &d->peri_status);
    if (rc) return rc;
    say_kx(d, "peri arbitration", "status", d->peri_status, NULL, 0);
    if (d->peri_status == PbPatchDone) return 0;
    if (d->peri_status == PbPatchBusy) {
        if ((rc = notify_wait(d, &d->peri_ntf, kNotifyMs))) return rc;
        d->peri_status = PbPatchDone;
        return 0;
    }
    if (d->peri_status != PbPatchNone) return PbErrStatus;
    if ((rc = download_peri(d, PbSsPeri, d->cfg.peri, d->cfg.peri_bytes))) return rc;
    if ((rc = send(d, p, pb_peri_board_id(p), kWriteMs))) return rc;
    rc = peri_wait(d, PbOpPeriEdl, PbSubBoardId, &e, kReplyMs);
    if (rc && rc != PbErrTimeout) return rc;                 /* HAL 取不到板号也继续，只是用默认 NVM */
    if (!rc && d->h4.have >= 14) say_kx(d, "peri board id", "hi", d->h4.buf[12], "lo", d->h4.buf[13]);
    if ((rc = download_peri(d, PbSsPeri, d->cfg.peri_nvm, d->cfg.peri_nvm_bytes))) return rc;
    if ((rc = send(d, p, pb_peri_build_info(p, PbSsPeri), kWriteMs))) return rc;
    if ((rc = peri_wait(d, PbOpPeriEdl, PbSubBuildInfo, &e, kReplyMs))) return rc;
    if ((rc = send(d, p, pb_peri_reset(p), kWriteMs))) return rc;
    if ((rc = peri_wait(d, PbOpPeriGeneral, PbSubReset, &e, kReplyMs))) return rc;
    if (e.status != 0) say_kx(d, "WARN peri reset", "status", e.status, NULL, 0);        /* HAL 只记录，判据是下面的通知 */
    if ((rc = notify_wait(d, &d->peri_ntf, kNotifyMs))) return rc;
    d->peri_status = PbPatchDone;
    return 0;
}

static int patch_tmel(PbDl *d)
{
    int rc = arbitrate(d, PbSsTmel, &d->tmel_status);
    if (rc) return rc;
    say_kx(d, "tmel arbitration", "status", d->tmel_status, NULL, 0);
    if (d->tmel_status == PbTmelAbsent1 || d->tmel_status == PbTmelAbsent2) return 0;
    if ((rc = get_version(d, PbSsTmel, &d->tmel_version))) return rc;
    say_kx(d, "tmel version", "product", d->tmel_version.product, "build", d->tmel_version.build);
    if (d->tmel_status == PbPatchDone) return 0;
    if (d->tmel_status == PbPatchBusy) return notify_wait(d, &d->tmel_ntf, kNotifyMs);
    if (d->tmel_status != PbPatchNone) return PbErrStatus;
    if ((rc = download_peri(d, PbSsTmel, d->cfg.tmel, d->cfg.tmel_bytes))) return rc;
    if ((rc = notify_wait(d, &d->tmel_ntf, kNotifyMs))) return rc;
    d->tmel_status = PbPatchDone;
    return 0;
}

static int activate(PbDl *d)
{
    uint8_t p[PbPacketMax];
    PbPeriEvent e;
    int rc = send(d, p, pb_peri_activate(p, PbSsBt, 1), kWriteMs);
    if (!rc) rc = peri_wait(d, PbOpPeriGeneral, PbSubActivate, &e, kReplyMs);
    if (rc) return rc;
    if (e.status == PbPatchDone) return 0;                  /* ActivateSS：skip waiting for notification event */
    return activated_wait(d, kNotifyMs);
}

/* HAL 忽略这几条的结果，这里也只记录。 */
static void best_effort(PbDl *d, uint16_t opcode, int sub, const uint8_t *packet, unsigned n)
{
    uint8_t status = 0xff;
    int rc = send(d, packet, n, kWriteMs);
    if (!rc) rc = hci_wait(d, opcode, sub, &status, kReplyMs);
    if (rc || status) say_kx(d, "WARN optional command", "opcode", opcode, "status", rc ? 0x100u : status);
}

static int bt_firmware(PbDl *d)
{
    uint8_t p[PbPacketMax], status = 0xff;
    int rc = send(d, p, pb_edl_command(p, PbEdlGetVer), kWriteMs);
    if (!rc) rc = hci_wait(d, PbOpEdl, PbEdlGetVer, &status, kReplyMs);
    if (rc) return rc;
    if (pb_edl_version(d->h4.buf, d->h4.have, &d->edl_version)) return PbErrProtocol;
    say_kx(d, "edl version", "product", d->edl_version.product, "soc", d->edl_version.soc);
    if (d->edl_version.soc != PB_SOC_VERSION) say_kx(d, "WARN unexpected soc", "want", PB_SOC_VERSION, NULL, 0);
    return download_bt(d, d->cfg.bt, d->cfg.bt_bytes);
}

static int bt_nvm(PbDl *d)
{
    uint8_t p[PbPacketMax], status = 0xff;
    int rc = send(d, p, pb_edl_command(p, PbEdlBoardId), kWriteMs);
    if (rc) return rc;
    rc = hci_wait(d, PbOpEdl, PbEdlBoardId, &status, kReplyMs);
    if (rc && rc != PbErrTimeout) return rc;                 /* 同上 */
    uint8_t hi = 0, lo = 0;
    if (!rc && !pb_edl_board_id(d->h4.buf, d->h4.have, &hi, &lo)) say_kx(d, "bt board id", "hi", hi, "lo", lo);
    return download_bt(d, d->cfg.bt_nvm, d->cfg.bt_nvm_bytes);
}

static int bt_finish(PbDl *d)
{
    uint8_t p[PbPacketMax], status = 0xff;
    int rc = send(d, p, pb_edl_command(p, PbEdlBuildInfo), kWriteMs);
    if (!rc) rc = hci_wait(d, PbOpEdl, PbEdlBuildInfo, &status, kReplyMs);
    if (rc) return rc;
    const uint8_t *label = NULL;
    uint32_t bytes = 0;
    if (!pb_edl_build_label(d->h4.buf, d->h4.have, &label, &bytes)) {
        char line[160];
        PbText t;
        pb_text_start(&t, line, sizeof line);
        pb_text_str(&t, "[bt] firmware ");
        pb_text_printable(&t, label, bytes);
        say(d, line);
        d->label_ok = (uint8_t)pb_contains(label, bytes, PB_BUILD_LABEL);
    }
    /* 下面四条 Android 都发，这里照发，保持字节流相同：附加特性、读 OTP（只读）、日志设置（值 0，关）、CoP 版本 */
    static const uint8_t addon[] = {PbH4Cmd, 0x1d, 0xfc, 0};
    static const uint8_t otp[] = {PbH4Cmd, 0x09, 0xfc, 2, 0x00, 0x0a};
    static const uint8_t logs[] = {PbH4Cmd, 0x17, 0xfc, 2, PbLogSet, 0};
    static const uint8_t cop[] = {PbH4Cmd, 0x17, 0xfc, 1, PbLogCop};
    best_effort(d, PbOpAddOn, -1, addon, sizeof addon);
    best_effort(d, PbOpOtp, -1, otp, sizeof otp);
    best_effort(d, PbOpLog, PbLogSet, logs, sizeof logs);
    best_effort(d, PbOpLog, PbLogCop, cop, sizeof cop);
    if ((rc = hci_command(d, PbOpReset, NULL, 0, &status))) return rc;
    return status ? PbErrStatus : 0;
}

int pb_dl_boot(PbDl *d)
{
    /* 只改内存副本。波特率写当前实际用的：HAL 总写 0x15，是因为它总切 8 Mbps */
    PbNvmEdit peri = {d->code, 1, -1, NULL}, bt = {d->code, 1, 0, d->cfg.bdaddr};
    PbNvmReport report;
    if (pb_nvm_edit(d->cfg.peri_nvm, d->cfg.peri_nvm_bytes, &peri, &report)) return fail(d, PbStepPeri, PbErrNvm, "peri NVM malformed");
    if (!report.saw_hci) return fail(d, PbStepPeri, PbErrNvm, "peri NVM without tag 17");
    if (pb_nvm_edit(d->cfg.bt_nvm, d->cfg.bt_nvm_bytes, &bt, &report)) return fail(d, PbStepBtNvm, PbErrNvm, "BT NVM malformed");
    if (!report.saw_hci || (d->cfg.bdaddr && !report.saw_addr)) return fail(d, PbStepBtNvm, PbErrNvm, "BT NVM without tag 2 or 17");

    int rc;
    if (d->cfg.baud != 115200) {
        if ((rc = baud_switch(d))) return fail(d, PbStepBaud, rc, "baud switch");
        say_kx(d, "baud switched", "baud", d->cfg.baud, "code", d->code);
    }
    if ((rc = done(d, PbStepBaud))) return rc;
    if ((rc = patch_peri(d))) return fail(d, PbStepPeri, rc, "peripheral patch");
    if ((rc = done(d, PbStepPeri))) return rc;
    if ((rc = wake_soc(d))) return fail(d, PbStepTmel, rc, "second wake");
    if ((rc = patch_tmel(d))) return fail(d, PbStepTmel, rc, "TME-L patch");
    if ((rc = done(d, PbStepTmel))) return rc;
    if ((rc = activate(d))) return fail(d, PbStepActive, rc, "activate BT subsystem");
    if ((rc = done(d, PbStepActive))) return rc;
    if ((rc = bt_firmware(d))) return fail(d, PbStepBtFw, rc, "BT firmware");
    if ((rc = done(d, PbStepBtFw))) return rc;
    if ((rc = bt_nvm(d))) return fail(d, PbStepBtNvm, rc, "BT NVM");
    if ((rc = done(d, PbStepBtNvm))) return rc;
    if ((rc = bt_finish(d))) return fail(d, PbStepReset, rc, "HCI reset");
    return done(d, PbStepReset);
}

int pb_dl_check(PbDl *d)
{
    uint8_t status = 0xff;
    int rc = hci_command(d, PbOpLocalVersion, NULL, 0, &status);
    if (rc) return fail(d, PbStepCheck, rc, "Read_Local_Version");
    const uint8_t *f = d->h4.buf;
    if (status || d->h4.have < 15) return fail(d, PbStepCheck, PbErrStatus, "Read_Local_Version answer");
    pb_copy(d->local_version, f + 7, 8);
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] local version");
    pb_text_bytes(&t, d->local_version, 8);
    say(d, line);
    /* 判据：HCI 0x0e，厂商 0x001d，子版本 0x7e21 */
    if (d->local_version[0] != 0x0e || d->local_version[4] != 0x1d || d->local_version[5] != 0x00)
        return fail(d, PbStepCheck, PbErrIdentity, "not the expected controller");
    if ((rc = hci_command(d, PbOpBdAddr, NULL, 0, &status))) return fail(d, PbStepCheck, rc, "Read_BD_ADDR");
    if (status || d->h4.have < 13) return fail(d, PbStepCheck, PbErrStatus, "Read_BD_ADDR answer");
    pb_copy(d->local_addr, d->h4.buf + 7, 6);
    if (d->cfg.bdaddr) {
        unsigned same = 0;
        for (unsigned i = 0; i < 6; ++i) same += d->local_addr[i] == d->cfg.bdaddr[i];
        if (same != 6) return fail(d, PbStepCheck, PbErrIdentity, "BD address is not the one in tag 2");
    }
    if (!d->label_ok) return fail(d, PbStepCheck, PbErrIdentity, "version string lacks " PB_BUILD_LABEL);
    d->hci_ok = 1;
    say(d, "[bt] HCI up");
    return done(d, PbStepCheck);
}

int pb_dl_handover(PbDl *d)
{
    /* Android 在 HCI_Reset 之后、协议栈第一次发送之前发一个 0xfd（跟踪 15.329925）。没有应答 HAL 也往下走，这里也只记录 */
    const int rc = wake_soc(d);
    say_kx(d, "handover wake", "fd", d->ibs.wakes_sent, "fc", d->ibs.acks_seen);
    if (rc && rc != PbErrTimeout) return fail(d, PbStepHandover, rc, "controller failed at the handover");
    return done(d, PbStepHandover);
}

int pb_dl_le_scan(PbDl *d, unsigned seconds)
{
    const PbXport *x = d->x;
    /* 事件掩码用跟踪里 Android 的值，默认掩码没开 LE Meta */
    static const uint8_t mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xbf, 0x3d};
    static const uint8_t le_mask[8] = {0x7f, 0xfe, 0x02, 0x4d, 0x07, 0xf8, 0x07, 0x00};
    static const uint8_t params[7] = {0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00};   /* 被动，10 ms 间隔和窗口，公共地址，不过滤 */
    static const uint8_t on[2] = {1, 0}, off[2] = {0, 0};
    uint8_t status = 0xff;
    int rc = hci_command(d, PbOpEventMask, mask, sizeof mask, &status);
    if (!rc) rc = hci_command(d, PbOpLeEventMask, le_mask, sizeof le_mask, &status);
    if (!rc) rc = hci_command(d, PbOpLeScanParams, params, sizeof params, &status);
    if (!rc) rc = hci_command(d, PbOpLeScanEnable, on, sizeof on, &status);
    if (rc) return fail(d, PbStepScan, rc, "LE scan set-up");
    if (status) return fail(d, PbStepScan, PbErrStatus, "LE scan refused");
    d->adverts = 0;
    const uint64_t end = x->now_ms(x->ctx) + (uint64_t)seconds * 1000u;
    uint64_t report = x->now_ms(x->ctx) + 10000u;
    for (;;) {
        const uint64_t now = x->now_ms(x->ctx);
        if (now >= end) break;
        PbPeriEvent e;
        const int got = next_frame(d, now + 100);
        if (got && (rc = note_frame(d, d->h4.buf, d->h4.have, &e))) return fail(d, PbStepScan, rc, "controller failed during the scan");
        if (x->now_ms(x->ctx) >= report) {
            report += 10000u;
            say_kx(d, "scanning", "adverts", d->adverts, NULL, 0);
        }
    }
    const uint32_t seen = d->adverts;
    (void)hci_command(d, PbOpLeScanEnable, off, sizeof off, &status);
    d->adverts = seen;
    say_kx(d, "scan done", "adverts", d->adverts, "seconds", seconds);
    return done(d, PbStepScan);
}
