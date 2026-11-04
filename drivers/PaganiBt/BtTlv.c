/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtTlv.h"
#include "BtUtil.h"

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static uint32_t le24(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;}

/* 0x31 帧头：31 00 opcode len subop */
static void peri_header(uint8_t *out, uint16_t opcode, uint8_t length, uint8_t sub)
{
    out[0] = PbH4PeriCmd;                   /* 发送方向的包类型是 0x31，接收方向才是 0x34 */
    out[1] = 0;
    out[2] = (uint8_t)opcode;
    out[3] = (uint8_t)(opcode >> 8);
    out[4] = length;
    out[5] = sub;
}

unsigned pb_peri_getver(uint8_t *out, uint8_t ss)
{
    peri_header(out, PbOpPeriEdl, 2, PbSubGetVer);
    out[6] = ss;
    return 7;
}
unsigned pb_peri_baud(uint8_t *out, uint8_t code)
{
    peri_header(out, PbOpPeriGeneral, 2, PbSubBaud);
    out[6] = code;
    return 7;
}
unsigned pb_peri_arbitrate(uint8_t *out, uint8_t ss)
{
    peri_header(out, PbOpPeriEdl, 3, PbSubArbitrate);
    out[6] = ss;
    out[7] = 0;
    return 8;
}
unsigned pb_peri_board_id(uint8_t *out)
{
    peri_header(out, PbOpPeriEdl, 1, PbSubBoardId);
    return 6;
}
unsigned pb_peri_build_info(uint8_t *out, uint8_t ss)
{
    peri_header(out, PbOpPeriEdl, 2, PbSubBuildInfo);
    out[6] = ss;
    return 7;
}
unsigned pb_peri_reset(uint8_t *out)
{
    peri_header(out, PbOpPeriGeneral, 1, PbSubReset);
    return 6;
}
unsigned pb_peri_activate(uint8_t *out, uint8_t ss, uint8_t action)
{
    peri_header(out, PbOpPeriGeneral, 3, PbSubActivate);
    out[6] = ss;
    out[7] = action;
    return 8;
}
unsigned pb_peri_segment(uint8_t *out, uint8_t ss, const uint8_t *data, unsigned bytes)
{
    peri_header(out, PbOpPeriEdl, (uint8_t)(bytes + 3), PbSubDownload);
    out[6] = ss;
    out[7] = (uint8_t)bytes;
    pb_copy(out + PbPeriHeader, data, bytes);
    return PbPeriHeader + bytes;
}
unsigned pb_edl_command(uint8_t *out, uint8_t sub)
{
    out[0] = PbH4Cmd;
    out[1] = (uint8_t)PbOpEdl;
    out[2] = (uint8_t)(PbOpEdl >> 8);
    out[3] = 1;
    out[4] = sub;
    return 5;
}
unsigned pb_edl_segment(uint8_t *out, const uint8_t *data, unsigned bytes)
{
    out[0] = PbH4Cmd;
    out[1] = (uint8_t)PbOpEdl;
    out[2] = (uint8_t)(PbOpEdl >> 8);
    out[3] = (uint8_t)(bytes + 2);
    out[4] = PbEdlDownload;
    out[5] = (uint8_t)bytes;
    pb_copy(out + PbEdlHeader, data, bytes);
    return PbEdlHeader + bytes;
}
unsigned pb_hci_command(uint8_t *out, uint16_t opcode, const uint8_t *params, unsigned plen)
{
    out[0] = PbH4Cmd;
    out[1] = (uint8_t)opcode;
    out[2] = (uint8_t)(opcode >> 8);
    out[3] = (uint8_t)plen;
    if (plen) pb_copy(out + 4, params, plen);
    return 4 + plen;
}

uint32_t pb_tlv_segments(uint32_t bytes, uint32_t *last)
{
    const uint32_t remainder = bytes % PbSegmentBytes, count = bytes / PbSegmentBytes + (remainder ? 1 : 0);
    if (last) *last = !bytes ? 0 : remainder ? remainder : PbSegmentBytes;
    return count;
}

int pb_tlv_wait_each(const uint8_t *data, uint32_t bytes)
{
    if (bytes >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') return 0;
    if (bytes >= 15 && data[0] == 1 && data[14] == 3) return 0;
    return 1;
}

/* 走一遍 TLV。edit 为空时只校验边界。 */
static int nvm_walk(uint8_t *d, uint32_t bytes, const PbNvmEdit *edit, PbNvmReport *report)
{
    uint32_t p = 0, end = bytes;
    if (bytes < 4) return -1;
    if (d[0] == PbTlvContainer) {
        const uint32_t declared = le24(d + 1);
        if (declared > bytes - 4) return -2;
        p = 4;
        end = 4 + declared;
    }
    while (end - p >= 5) {
        const uint8_t type = d[p];
        const uint32_t length = le24(d + p + 1);
        if (length > end - p - 4) return -3;
        const uint32_t stop = p + 4 + length;
        if (type == PbTlvNvm) {
            uint32_t q = p + 4;
            while (stop - q >= 12) {
                const uint16_t tag = le16(d + q), size = le16(d + q + 2);
                uint8_t *data = d + q + 12;
                if (size > stop - q - 12) return -4;
                if (report) report->tags++;
                if (tag == PbTagBdAddr && size >= 6) {
                    if (report) report->saw_addr = 1;
                    if (edit && edit->bdaddr) pb_copy(data, edit->bdaddr, 6);
                } else if (tag == PbTagHci && size >= 2) {
                    if (report) report->saw_hci = 1;
                    if (edit && edit->baud_code >= 0) data[1] = (uint8_t)edit->baud_code;
                } else if (tag == PbTagDeepSleep && size >= 2) {
                    if (report) report->saw_sleep = 1;
                    if (edit && edit->sibs >= 0) data[1] = (uint8_t)(edit->sibs ? data[1] | 1u : data[1] & 0xfeu);
                } else if (tag == PbTagAsd && size >= 1) {
                    if (report) report->saw_asd = 1;
                    if (edit && edit->asd >= 0) data[0] = (uint8_t)(edit->asd ? data[0] | 1u : data[0] & 0xfeu);
                }
                q += 12u + size;
            }
        }
        p = stop;
    }
    return 0;
}

int pb_nvm_edit(uint8_t *nvm, uint32_t bytes, const PbNvmEdit *edit, PbNvmReport *report)
{
    PbNvmReport local;
    pb_zero(&local, sizeof local);
    if (!nvm || !edit) return -1;
    int rc = nvm_walk(nvm, bytes, NULL, NULL);      /* 全部校验过才动字节 */
    if (rc) return rc;
    rc = nvm_walk(nvm, bytes, edit, &local);
    if (report) *report = local;
    return rc;
}

uint32_t pb_nvm_wrap(uint8_t *out, const uint8_t *nvm, uint32_t nvm_bytes, const uint8_t *cal, uint32_t cal_bytes)
{
    const uint32_t total = nvm_bytes + cal_bytes;
    out[0] = PbTlvContainer;
    out[1] = (uint8_t)total;
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total >> 16);
    pb_copy(out + 4, nvm, nvm_bytes);
    pb_copy(out + 4 + nvm_bytes, cal, cal_bytes);
    return total + 4;
}

/* a[0]=34 a[1]=00 a[2]=ff a[3]=len a[4]=f0 a[5]=kind。ReadHciEvent/GetVsHciEvent/ReadVsAsyncEvent 的读法。 */
void pb_peri_parse(const uint8_t *a, uint32_t bytes, PbPeriEvent *e)
{
    pb_zero(e, sizeof *e);
    e->kind = PbPeriOther;
    if (bytes < 6 || a[0] != PbH4Peri || a[2] != 0xff) return;
    const uint8_t kind = a[5];
    if (a[4] == 0xf0 && kind == 2 && bytes >= 8) {
        e->kind = PbPeriActivated;
        e->ss = a[6];
        e->action = a[7];
    } else if (a[4] == 0xf0 && kind == 3 && bytes >= 8) {
        e->kind = PbPeriPatchNtf;
        e->status = a[6];
        e->ss = a[7];
    } else if (a[4] == 0xf0 && (kind == 4 || kind == 5 || kind == 7)) {
        e->kind = PbPeriDump;
    } else if (kind == 1 && bytes >= 11) {
        e->kind = PbPeriComplete;
        e->opcode = le16(a + 7);
        e->status = a[9];
        e->sub = a[10];
        if (bytes >= 12) e->ss = a[11];
    } else if (kind == 0 && bytes >= 10) {
        e->kind = PbPeriStatus;
        e->status = a[6];
        e->opcode = le16(a + 8);
    }
}

int pb_frame_crashed(const uint8_t *f, uint32_t bytes)
{
    if (!bytes) return 0;
    if (f[0] == PbH4Peri) {
        PbPeriEvent e;
        pb_peri_parse(f, bytes, &e);
        return e.kind == PbPeriDump;
    }
    return f[0] == PbH4Evt && bytes >= 5 && f[1] == PbEvtVendor && f[3] == 1 && (f[4] & 0xfe) == 8;
}

int pb_peri_version(const uint8_t *a, uint32_t bytes, PbPeriVersion *v)
{
    pb_zero(v, sizeof *v);
    if (bytes < 20) return -1;
    v->product = le32(a + 12);
    v->patch = le16(a + 16);
    v->build = le16(a + 18);
    if (bytes >= 24 && a[3] != 10) {
        v->soc = le32(a + 20);
        v->has_soc = 1;
    }
    if (bytes >= 25) v->security = a[24];
    return 0;
}

int pb_hci_complete(const uint8_t *f, uint32_t bytes, uint16_t *opcode, uint8_t *status, int *sub)
{
    if (bytes < 7 || f[0] != PbH4Evt || f[1] != PbEvtCommandComplete) return 0;
    if (opcode) *opcode = le16(f + 4);
    if (status) *status = f[6];
    if (sub) *sub = (f[5] & 0xfc) == 0xfc && bytes >= 8 ? f[7] : -1;
    return 1;
}

int pb_edl_version(const uint8_t *f, uint32_t bytes, PbEdlVersion *v)
{
    pb_zero(v, sizeof *v);
    if (bytes < 21) return -1;
    v->product = le32(f + 9);
    v->patch = le16(f + 13);
    v->build = le16(f + 15);
    v->soc = le32(f + 17);
    return 0;
}

int pb_edl_board_id(const uint8_t *f, uint32_t bytes, uint8_t *hi, uint8_t *lo)
{
    if (bytes < 11 || f[2] <= 7 || f[8] != 2) return -1;
    *hi = f[9];
    *lo = f[10];
    return 0;
}

int pb_edl_build_label(const uint8_t *f, uint32_t bytes, const uint8_t **text, uint32_t *length)
{
    if (bytes < 9 || (uint32_t)f[8] > bytes - 9) return -1;
    *text = f + 9;
    *length = f[8];
    return 0;
}

static unsigned phase_of_step(unsigned step)
{
    switch (step) {
    case 101: case 102: case 201: case 202: case 301: case 302: case 303: case 304: return 1;
    case 401: case 402: case 403: case 404: case 405: case 406: case 407: case 408: case 409: return 2;
    case 501: return 3;
    default: return 0;
    }
}

void pb_arg_parse(unsigned value, PbArg *a)
{
    a->phase = 0;
    a->baud = 115200;
    a->stop = 0;
    a->stand_in = 0;
    if (value >= 1 && value <= 4) {
        a->phase = value;
    } else if (value == 23 || value == 28 || value == 33 || value == 38 || value == 43 || value == 48) {
        a->phase = value / 10;
        a->baud = value % 10 == 3 ? 3000000 : 8000000;
    } else if (value == 40) {
        a->phase = 4;
        a->stand_in = 1;
    } else if (value >= 100) {
        a->phase = phase_of_step(value);
        if (a->phase) a->stop = (int)value;
    }
}

int pb_bdaddr_from_seed(uint64_t seed, uint8_t m[6])
{
    if (!seed) return 0;
    uint64_t h = 0xcbf29ce484222325ull ^ 0x42u;   /* 和 Wi-Fi 的 FNV-1a 差一个盐，两个地址不相同 */
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = (uint8_t)(h >> (8 * i));
    if (m[2] == 0x9e && m[1] == 0x8b && m[0] < 0x40) m[2] ^= 1;
    return 1;
}

void pb_bdaddr_written(const uint8_t wire[6], uint8_t written[6])
{
    for (unsigned i = 0; i < 6; ++i) written[i] = wire[5 - i];
}
