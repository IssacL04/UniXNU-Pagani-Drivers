/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 控制器模型：按本机 HAL 解析代码里的格式答话（0x34 事件没有真机样本，这是对 HAL 的复述），并检查主机守规矩：
 *   - 固件下完之前 HCI 没人理，ActivateSS 之后才响应；
 *   - 外设 ELF 不逐段答，只答最后一段；NVM 每段答一次，主机不等应答就发下一段算违规；
 *   - 外设下完要有 PeriReset，之后才有补丁通知；
 *   - 收齐的字节按子系统存下来，测试和原文件比。
 * 字节进出走 cm_rx（主机发来的）和 cm_out（发给主机的）。geni 模型和 download 测试都接它。 */
#ifndef PAGANI_BT_TEST_CTRL_MODEL_H
#define PAGANI_BT_TEST_CTRL_MODEL_H
#include <stdint.h>
#include <string.h>
#include "../BtTlv.h"

#define CM_IMAGE_MAX (1u << 20)
#define CM_OUT_MAX (1u << 16)
enum {CmBufPeri, CmBufPeriNvm, CmBufTmel, CmBufBt, CmBufBtNvm, CmBufCount};
typedef struct {
    /* 解析主机来的字节 */
    uint8_t in[512];
    uint32_t in_have;
    /* 发给主机的字节 */
    uint8_t out[CM_OUT_MAX];
    uint32_t out_head, out_tail;
    /* 收到的镜像 */
    uint8_t *image[CmBufCount];
    uint32_t image_len[CmBufCount];
    /* 状态 */
    uint8_t awake, active, peri_done, tmel_done, peri_elf_done, peri_nvm_done, peri_reset, bt_elf_done, bt_nvm_done, reset_done;
    uint8_t peri_arbitrated, tmel_arbitrated;
    uint32_t baud, requested_baud;
    uint8_t bd_addr[6];
    uint32_t wakes, hci_ignored, violations, peri_segments, tmel_segments, bt_segments, bt_nvm_segments, peri_nvm_segments;
    uint32_t peri_elf_bytes, tmel_bytes, bt_elf_bytes;
    uint32_t pending_acks;              /* 主机还没取走的逐段应答 */
    uint32_t wakes_sent, host_acks;     /* 模型发的 0xfd、主机回的 0xfc */
    uint16_t history[32];               /* 校验和收尾阶段的命令，按到达顺序 */
    uint32_t history_count;
    uint8_t peri_board_seen, tmel_version_seen;
    uint64_t adverts_left;
    uint8_t scanning;
    /* 注入 */
    uint8_t mute;                       /* 什么都不答 */
    uint8_t no_wake_ack;
    uint8_t peri_version_bad, peri_status, tmel_status;   /* 仲裁答的状态 */
    uint8_t seg_status, nvm_seg_status;                   /* 段应答的状态 */
    uint8_t ntf_status, dump_on_activate, hw_error_on_bt, version_mute;
    uint8_t wrong_ss, wrong_arb_ss, wrong_local_version, wrong_bd_addr, no_label;
    uint8_t bt_seg_status, bt_nvm_seg_status, no_activated;
    uint8_t status_form_activate;       /* ActivateSS 用状态事件答，带 23：不再等完成事件 */
    uint8_t ntf_before_reply;           /* 补丁通知先于复位应答 */
    uint8_t acl_noise, sleep_noise;     /* 应答之间夹控制器日志、IBS 字节 */
    uint32_t elf_ack_after;             /* 非 0：外设 ELF 第 N 段后多答一次，看主机会不会乱 */
} CtrlModel;

static inline void cm_init(CtrlModel *c, uint8_t *const buffers[CmBufCount])
{
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < CmBufCount; ++i) c->image[i] = buffers[i];
    c->baud = 115200;
    c->peri_status = PbPatchNone;
    c->tmel_status = PbPatchNone;
}
static inline void cm_out(CtrlModel *c, const uint8_t *data, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) c->out[(c->out_tail + i) % CM_OUT_MAX] = data[i];
    c->out_tail += bytes;
}
static inline uint32_t cm_pending(const CtrlModel *c) {return c->out_tail - c->out_head;}
static inline uint8_t cm_get(CtrlModel *c) {return c->out[c->out_head++ % CM_OUT_MAX];}
/* 34 00 ff len f0 kind ... */
static inline void cm_peri(CtrlModel *c, uint8_t kind, const uint8_t *params, uint8_t n)
{
    uint8_t f[300] = {PbH4Peri, 0, 0xff, (uint8_t)(n + 2), 0xf0, kind};
    if (n) memcpy(f + 6, params, n);
    cm_out(c, f, 6u + n);
}
/* 完成事件：a[6] 额度，a[7..8] 命令号，a[9] 状态，a[10] subop，a[11] 子系统，a[12..] 数据 */
static inline void cm_peri_cc(CtrlModel *c, uint16_t opcode, uint8_t status, uint8_t sub, uint8_t ss, const uint8_t *data, uint8_t n)
{
    uint8_t p[260] = {1, (uint8_t)opcode, (uint8_t)(opcode >> 8), status, sub, ss};
    if (n) memcpy(p + 6, data, n);
    cm_peri(c, 1, p, (uint8_t)(n + 6));
}
static inline void cm_hci_cc(CtrlModel *c, uint16_t opcode, const uint8_t *params, uint8_t n)
{
    uint8_t f[300] = {PbH4Evt, PbEvtCommandComplete, (uint8_t)(n + 3), 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
    if (n) memcpy(f + 6, params, n);
    cm_out(c, f, 6u + n);
}
static inline void cm_noise(CtrlModel *c)
{
    if (c->acl_noise) {
        static const uint8_t log[] = {PbH4Acl, 0xdc, 0x2e, 4, 0, 1, 2, 3, 4};
        cm_out(c, log, sizeof log);
    }
    if (c->sleep_noise) {
        static const uint8_t ibs[] = {PbIbsSleep, PbIbsWake};
        cm_out(c, ibs, sizeof ibs);
        c->wakes_sent++;
    }
}
static inline void cm_peri_command(CtrlModel *c, const uint8_t *p, uint32_t n)
{
    const uint16_t opcode = (uint16_t)(p[2] | p[3] << 8);
    const uint8_t sub = p[5], ss = n > 6 ? p[6] : 0;
    if (opcode == PbOpPeriEdl && sub == PbSubDownload) {
        const uint8_t bytes = p[7];
        const int tmel = ss == PbSsTmel;
        int nvm = 0;
        if (tmel) {
            if (!c->tmel_version_seen) c->violations++;     /* HAL：TME-L 先取版本才下载 */
            memcpy(c->image[CmBufTmel] + c->image_len[CmBufTmel], p + 8, bytes);
            c->image_len[CmBufTmel] += bytes;
            c->tmel_segments++;
        } else if (!c->peri_elf_done) {
            memcpy(c->image[CmBufPeri] + c->image_len[CmBufPeri], p + 8, bytes);
            c->image_len[CmBufPeri] += bytes;
            c->peri_segments++;
        } else {
            if (!c->peri_board_seen) c->violations++;       /* HAL：外设 ELF 下完先取板号才发 NVM */
            memcpy(c->image[CmBufPeriNvm] + c->image_len[CmBufPeriNvm], p + 8, bytes);
            c->image_len[CmBufPeriNvm] += bytes;
            c->peri_nvm_segments++;
            nvm = 1;
        }
        if (nvm) {
            if (c->pending_acks) c->violations++;      /* 上一段的应答还没取走就发了这一段 */
            c->pending_acks = 1;
            cm_noise(c);
            cm_peri_cc(c, PbOpPeriEdl, c->nvm_seg_status, PbSubDownload, c->wrong_ss ? 2 : ss, NULL, 0);
            return;
        }
        if (bytes < PbSegmentBytes) {                  /* ELF 只答最后一段 */
            cm_peri_cc(c, PbOpPeriEdl, c->seg_status, PbSubDownload, ss, NULL, 0);
            if (!tmel) {
                c->peri_elf_done = 1;
            } else if (!c->tmel_done) {
                c->tmel_done = 1;
                const uint8_t ntf[2] = {c->ntf_status, PbSsTmel};
                cm_peri(c, 3, ntf, 2);
            }
        } else if (c->elf_ack_after && c->peri_segments == c->elf_ack_after) {
            cm_peri_cc(c, PbOpPeriEdl, 0, PbSubBoardId, ss, NULL, 0);   /* 不相干的事件 */
        }
        return;
    }
    if (opcode == PbOpPeriEdl && sub == PbSubGetVer) {
        uint8_t data[13] = {0x1e, 0, 0, 0, 0x34, 0x12, 0x00, 0x02, 0x00, 0x02, 0x21, 0x40, 1};
        if (c->version_mute) return;
        if (ss == PbSsTmel) c->tmel_version_seen = 1;
        cm_noise(c);
        cm_peri_cc(c, PbOpPeriEdl, c->peri_version_bad ? 5 : 0, PbSubGetVer, ss, data, sizeof data);
    } else if (opcode == PbOpPeriEdl && sub == PbSubArbitrate) {
        if (ss == PbSsPeri) c->peri_arbitrated = 1;
        else c->tmel_arbitrated = 1;
        cm_peri_cc(c, PbOpPeriEdl, ss == PbSsTmel ? c->tmel_status : c->peri_status, PbSubArbitrate, c->wrong_arb_ss ? 2 : ss, NULL, 0);
        if (ss == PbSsPeri && c->peri_status == PbPatchBusy) {
            const uint8_t ntf[2] = {0, PbSsPeri};
            cm_peri(c, 3, ntf, 2);
        }
        if (ss == PbSsTmel && c->tmel_status == PbPatchBusy) {
            const uint8_t ntf[2] = {0, PbSsTmel};
            cm_peri(c, 3, ntf, 2);
        }
    } else if (opcode == PbOpPeriEdl && sub == PbSubBoardId) {
        const uint8_t data[2] = {0x00, 0x33};
        c->peri_nvm_done = 0;
        c->peri_board_seen = 1;
        cm_peri_cc(c, PbOpPeriEdl, 0, PbSubBoardId, 2, data, 2);
    } else if (opcode == PbOpPeriEdl && sub == PbSubBuildInfo) {
        const uint8_t data[5] = {4, 'P', 'E', 'R', 'I'};
        c->peri_nvm_done = 1;
        cm_peri_cc(c, PbOpPeriEdl, 0, PbSubBuildInfo, 0, data, 5);
    } else if (opcode == PbOpPeriGeneral && sub == PbSubBaud) {
        c->requested_baud = p[6];
        /* 设波特率的应答按新速率发，主机改好时钟放开 RFR 才收得到 */
        cm_peri_cc(c, PbOpPeriGeneral, 1, PbSubBaud, 0, NULL, 0);
    } else if (opcode == PbOpPeriGeneral && sub == PbSubReset) {
        c->peri_reset = 1;
        if (!c->peri_nvm_done) c->violations++;        /* 复位前要先下完 NVM、取过构建信息 */
        const uint8_t ntf[2] = {c->ntf_status, PbSsPeri};
        if (c->ntf_before_reply) cm_peri(c, 3, ntf, 2);
        cm_peri_cc(c, PbOpPeriGeneral, 0, PbSubReset, 0, NULL, 0);
        if (!c->ntf_before_reply) cm_peri(c, 3, ntf, 2);
        c->peri_done = 1;
    } else if (opcode == PbOpPeriGeneral && sub == PbSubActivate) {
        if (c->dump_on_activate) {
            const uint8_t dump[2] = {0, 0};
            cm_peri(c, 4, dump, 2);
            return;
        }
        if (!c->peri_done && c->peri_status != PbPatchDone && c->peri_status != PbPatchBusy) c->violations++;
        c->active = 1;
        if (c->status_form_activate) {
            const uint8_t status[4] = {PbPatchDone, 1, 0xf1, 0xff};
            cm_peri(c, 0, status, 4);
            return;
        }
        cm_peri_cc(c, PbOpPeriGeneral, 0, PbSubActivate, 0, NULL, 0);
        if (c->no_activated) return;
        const uint8_t done[2] = {PbSsBt, 1};
        cm_peri(c, 2, done, 2);
    }
}
static inline void cm_edl(CtrlModel *c, const uint8_t *p)
{
    const uint8_t sub = p[4];
    if (sub == PbEdlDownload) {
        const uint8_t bytes = p[5];
        const int nvm = c->bt_elf_done;
        const unsigned which = nvm ? CmBufBtNvm : CmBufBt;
        memcpy(c->image[which] + c->image_len[which], p + 6, bytes);
        c->image_len[which] += bytes;
        if (nvm) c->bt_nvm_segments++;
        else c->bt_segments++;
        if (nvm) {
            if (c->pending_acks) c->violations++;
            c->pending_acks = 1;
            const uint8_t ack[2] = {c->bt_nvm_seg_status, PbEdlDownload};
            cm_noise(c);
            cm_hci_cc(c, PbOpEdl, ack, 2);
        } else if (bytes < PbSegmentBytes) {
            const uint8_t ack[2] = {c->bt_seg_status, PbEdlDownload};
            c->bt_elf_done = 1;
            cm_hci_cc(c, PbOpEdl, ack, 2);
        }
    } else if (sub == PbEdlGetVer) {
        uint8_t data[15] = {0, PbEdlGetVer, 0, 0x1e, 0, 0, 0, 0x34, 0x12, 0x00, 0x02, 0x00, 0x02, 0x21, 0x40};
        cm_hci_cc(c, PbOpEdl, data, sizeof data);
    } else if (sub == PbEdlBoardId) {
        const uint8_t data[5] = {0, PbEdlBoardId, 2, 0x00, 0x33};
        cm_hci_cc(c, PbOpEdl, data, sizeof data);
    } else if (sub == PbEdlBuildInfo) {
        static const char label[] = "BTFW.BRAHMA.2.0.0-00262-PATCHZ-1.92902.28_DEF";
        uint8_t data[64] = {0, PbEdlBuildInfo, (uint8_t)(sizeof label - 1)};
        memcpy(data + 3, label, sizeof label - 1);
        if (c->no_label) data[3] = 'X';
        c->bt_nvm_done = 1;
        cm_hci_cc(c, PbOpEdl, data, (uint8_t)(3 + sizeof label - 1));
    }
}
static inline void cm_nvm_addr(CtrlModel *c);
static inline void cm_hci_command(CtrlModel *c, const uint8_t *p, uint32_t n)
{
    (void)n;
    const uint16_t opcode = (uint16_t)(p[1] | p[2] << 8);
    if (!c->active) {                                  /* 激活之前 HCI 没人理 */
        c->hci_ignored++;
        return;
    }
    if (c->hw_error_on_bt) {
        const uint8_t error[] = {PbH4Evt, PbEvtHardwareError, 1, 0};
        cm_out(c, error, sizeof error);
        return;
    }
    const uint8_t ok[1] = {0};
    if (opcode != PbOpEdl && c->history_count < 32) c->history[c->history_count++] = opcode;
    switch (opcode) {
    case PbOpEdl: cm_edl(c, p); break;
    case PbOpReset:
        c->reset_done = 1;
        cm_hci_cc(c, PbOpReset, ok, 1);
        break;
    case PbOpLocalVersion: {
        uint8_t v[9] = {0, 0x0e, 0, 0, 0x0e, 0x1d, 0, 0x21, 0x7e};
        if (c->wrong_local_version) v[5] = 0x0f;
        cm_hci_cc(c, opcode, v, sizeof v);
        break;
    }
    case PbOpBdAddr: {
        uint8_t a[7] = {0};
        cm_nvm_addr(c);
        memcpy(a + 1, c->bd_addr, 6);
        if (c->wrong_bd_addr) a[1] ^= 1;
        cm_hci_cc(c, opcode, a, sizeof a);
        break;
    }
    case PbOpAddOn: {
        const uint8_t a[9] = {0, 0x21, 0, 1, 0, 0x9c, 0x7d, 0x7f, 0xfc};
        cm_hci_cc(c, opcode, a, sizeof a);
        break;
    }
    case PbOpOtp: {
        const uint8_t a[3] = {0, 0, 0x0a};
        cm_hci_cc(c, opcode, a, sizeof a);
        break;
    }
    case PbOpLog: {
        const uint8_t a[2] = {0, p[4]};
        cm_hci_cc(c, opcode, a, sizeof a);
        break;
    }
    case PbOpLeScanEnable:
        c->scanning = p[4];
        cm_hci_cc(c, opcode, ok, 1);
        break;
    default:
        cm_hci_cc(c, opcode, ok, 1);
        break;
    }
}
/* 控制器从 NVM 的 Tag 2 取地址 */
static inline void cm_nvm_addr(CtrlModel *c)
{
    const uint8_t *d = c->image[CmBufBtNvm];
    uint32_t p = d[0] == PbTlvContainer ? 8 : 4;
    const uint32_t end = c->image_len[CmBufBtNvm];
    while (p + 12 <= end) {
        const uint32_t tag = (uint32_t)(d[p] | d[p + 1] << 8), size = (uint32_t)(d[p + 2] | d[p + 3] << 8);
        if (tag == PbTagBdAddr) {
            memcpy(c->bd_addr, d + p + 12, 6);
            return;
        }
        p += 12 + size;
    }
}
/* 主机发来的一个字节。 */
static inline void cm_rx(CtrlModel *c, uint8_t byte)
{
    if (!c->in_have) {
        if (byte == PbIbsWake) {
            c->wakes++;
            c->awake = 1;
            if (!c->mute && !c->no_wake_ack) {
                const uint8_t ack = PbIbsWakeAck;
                cm_out(c, &ack, 1);
            }
            return;
        }
        if (byte == PbIbsWakeAck) {
            c->host_acks++;
            return;
        }
        if (byte == PbIbsSleep) return;
        if (byte != PbH4PeriCmd && byte != PbH4Cmd) {
            c->violations++;
            return;
        }
    }
    c->in[c->in_have++] = byte;
    uint32_t need = 0;
    if (c->in[0] == PbH4PeriCmd && c->in_have >= 5) need = 5u + c->in[4];
    if (c->in[0] == PbH4Cmd && c->in_have >= 4) need = 4u + c->in[3];
    if (need && c->in_have == need) {
        c->in_have = 0;
        if (c->mute) return;
        if (c->in[0] == PbH4PeriCmd) cm_peri_command(c, c->in, need);
        else cm_hci_command(c, c->in, need);
    }
}
#endif
