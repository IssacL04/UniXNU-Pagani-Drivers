/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Host tests of the GPR codec, the framework readiness query (PaganiGpr.c; the GPR part follows UniXNU-sheng package/tests/gpr_host.c,
 * commit 08cde20e) and the 13T AudioReach/PRM packets (PaganiReach.c; the layout checks follow sheng's tests/audioreach_host.c): every
 * field the DSP reads, the 32-bit device format, both data lines, the PRM bit clock from either port, malformed answers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PaganiGpr.h"
#include "PaganiReach.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}

/* What the DSP would send back to our query. */
static uint32_t reply(uint8_t *out, const uint8_t *query, uint32_t opcode, const uint32_t *words, unsigned n)
{
    GprPacket q, p;
    CHECK(pagani_gpr_decode(query, GPR_HEADER_BYTES, &q) == GprOk);
    memset(&p, 0, sizeof p);
    p.dst_domain = q.src_domain; p.src_domain = q.dst_domain; p.src_port = q.dst_port; p.dst_port = q.src_port; p.token = q.token;
    p.opcode = opcode;
    return pagani_gpr_encode(out, GPR_MAX_PACKET, &p, words, 4 * n);
}

static void gpr(void)
{
    uint8_t buffer[GPR_MAX_PACKET], query[GPR_HEADER_BYTES], answer[GPR_MAX_PACKET];
    GprPacket p, d;
    memset(&p, 0, sizeof p);
    p.dst_domain = GPR_DOMAIN_ADSP; p.src_domain = GPR_DOMAIN_APPS; p.src_port = 1; p.dst_port = 1; p.token = 0x1234;
    p.opcode = GPR_OP_APM_GET_SPF_STATE;
    const uint8_t expected[24] = {0x60, 0x18, 0, 0, 0x02, 0x03, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x34, 0x12, 0, 0, 0x21, 0x10, 0x00, 0x01};
    CHECK(pagani_gpr_encode(buffer, sizeof buffer, &p, NULL, 0) == 24 && !memcmp(buffer, expected, 24));
    const uint32_t words[3] = {7, 8, 9};
    CHECK(pagani_gpr_encode(buffer, sizeof buffer, &p, words, 12) == 36 && le32(buffer) == (0x60u | 36u << 8));
    CHECK(pagani_gpr_decode(buffer, 36, &d) == GprOk && d.size == 36 && d.header_bytes == 24 && d.payload_bytes == 12 && le32(d.payload + 8) == 9);
    CHECK(d.dst_domain == 2 && d.src_domain == 3 && d.src_port == 1 && d.dst_port == 1 && d.token == 0x1234);
    CHECK(pagani_gpr_decode(buffer, 512, &d) == GprOk && d.payload_bytes == 12);   /* an intent longer than the packet */
    CHECK(pagani_gpr_encode(buffer, 35, &p, words, 12) == 0 && pagani_gpr_encode(buffer, sizeof buffer, &p, NULL, 4) == 0);
    CHECK(pagani_gpr_encode(buffer, sizeof buffer, &p, buffer, GPR_MAX_PACKET - 23) == 0);
    pagani_gpr_encode(buffer, sizeof buffer, &p, words, 12);
    CHECK(pagani_gpr_decode(buffer, 23, &d) == GprShort && pagani_gpr_decode(buffer, 35, &d) == GprBadSize);
    buffer[0] = 0x61; CHECK(pagani_gpr_decode(buffer, 36, &d) == GprBadVersion);
    buffer[0] = 0x50; CHECK(pagani_gpr_decode(buffer, 36, &d) == GprBadHeader);
    buffer[0] = 0x70; CHECK(pagani_gpr_decode(buffer, 36, &d) == GprOk && d.header_bytes == 28 && d.payload_bytes == 8);
    buffer[0] = 0x60; buffer[1] = 0x10; CHECK(pagani_gpr_decode(buffer, 36, &d) == GprBadSize);
    uint32_t op = 0, status = 0;
    const uint32_t basic[2] = {GPR_OP_APM_GET_SPF_STATE, 0x0b};
    memset(&p, 0, sizeof p); p.opcode = GPR_OP_BASIC_RESULT;
    pagani_gpr_encode(buffer, sizeof buffer, &p, basic, 8);
    CHECK(pagani_gpr_decode(buffer, 32, &d) == GprOk && pagani_gpr_basic_result(&d, &op, &status) == 0 && op == GPR_OP_APM_GET_SPF_STATE &&
          status == 0x0b);
    pagani_gpr_encode(buffer, sizeof buffer, &p, basic, 4);
    CHECK(pagani_gpr_decode(buffer, 28, &d) == GprOk && pagani_gpr_basic_result(&d, &op, &status) == -1);

    /* Readiness: not ready, a stale and a foreign answer, a timeout, a refusal, then ready. */
    GprSpfProbe s;
    pagani_gpr_spf_init(&s, 0x2001, 5);
    CHECK(pagani_gpr_spf_next(&s, query) == 24 && le32(query + 20) == GPR_OP_APM_GET_SPF_STATE && le32(query + 8) == 0x2001 &&
          le32(query + 12) == GPR_PORT_APM && s.state == SpfWaiting);
    CHECK(pagani_gpr_spf_next(&s, query) == 0);
    uint32_t zero = 0, one = 1;
    uint32_t n = reply(answer, query, GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(pagani_gpr_spf_receive(&s, answer, n) == SpfNotReady && s.state == SpfIdle);
    CHECK(pagani_gpr_spf_receive(&s, answer, n) == SpfIgnored);
    CHECK(pagani_gpr_spf_next(&s, query) == 24 && le32(query + 16) == 2);
    uint8_t stale[GPR_MAX_PACKET];
    memcpy(stale, answer, n);
    CHECK(pagani_gpr_spf_receive(&s, stale, n) == SpfIgnored);
    n = reply(answer, query, GPR_OP_APM_SPF_STATE, &one, 1);
    answer[12] = 9;
    CHECK(pagani_gpr_spf_receive(&s, answer, n) == SpfIgnored);
    answer[12] = 1;
    CHECK(pagani_gpr_spf_receive(&s, answer, 10) == SpfIgnored);
    CHECK(pagani_gpr_spf_timeout(&s) == SpfIgnored && s.timeouts == 1 && s.state == SpfIdle);
    CHECK(pagani_gpr_spf_next(&s, query) == 24);
    n = reply(answer, query, GPR_OP_BASIC_RESULT, basic, 2);
    CHECK(pagani_gpr_spf_receive(&s, answer, n) == SpfError && s.errors == 1 && s.last_status == 0x0b && s.state == SpfIdle);
    CHECK(pagani_gpr_spf_next(&s, query) == 24);
    n = reply(answer, query, GPR_OP_APM_SPF_STATE, &one, 1);
    CHECK(pagani_gpr_spf_receive(&s, answer, n) == SpfIsReady && s.state == SpfReady && pagani_gpr_spf_next(&s, query) == 0);
    pagani_gpr_spf_init(&s, 2, 2);
    CHECK(pagani_gpr_spf_next(&s, query) == 24 && pagani_gpr_spf_timeout(&s) == SpfIgnored && pagani_gpr_spf_next(&s, query) == 24);
    CHECK(pagani_gpr_spf_timeout(&s) == SpfDone && s.state == SpfGaveUp && pagani_gpr_spf_next(&s, query) == 0);
}

/* Walks the parameter blocks of an APM command: each 8-byte sized, inside the payload, error code 0. */
static unsigned blocks(const uint8_t *pkt, uint32_t n, uint32_t *iid, uint32_t *id, uint32_t *offset, unsigned max)
{
    GprPacket p;
    CHECK(pagani_gpr_decode(pkt, n, &p) == GprOk && p.size == n);
    CHECK(p.payload_bytes >= 16 && le32(p.payload + 12) == p.payload_bytes - 16 && !le32(p.payload) && !le32(p.payload + 8));
    unsigned count = 0;
    for (uint32_t at = 16; at < p.payload_bytes;) {
        CHECK(at + 16 <= p.payload_bytes);
        const uint32_t size = le32(p.payload + at + 8);
        CHECK(at + 16 + size <= p.payload_bytes && le32(p.payload + at + 12) == 0 && count < max);
        iid[count] = le32(p.payload + at); id[count] = le32(p.payload + at + 4); offset[count] = 24 + at + 16;
        ++count;
        at += 16 + size;
    }
    return count;
}

static void reach(void)
{
    uint8_t b[GPR_MAX_PACKET];
    uint32_t iid[8], id[8], off[8];
    const ArFormat stream = {48000, 16, 2}, device32 = {48000, 32, 2}, device16 = {48000, 16, 2};
    GprPacket p;

    /* Device graph: subgraph 0x4005 (low latency, TX as sheng's board topology, playback), one container, the I2S sink. */
    uint32_t n = pagani_ar_open_speaker_device(b, sizeof b, 0x2001, 7);
    CHECK(n && n <= GPR_MAX_PACKET && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_GRAPH_OPEN && p.dst_port == AR_APM_IID &&
          p.src_port == 0x2001 && p.token == 7 && p.dst_domain == GPR_DOMAIN_ADSP && p.src_domain == GPR_DOMAIN_APPS);
    CHECK(blocks(b, n, iid, id, off, 8) == 5);
    CHECK(id[0] == 0x08001001 && id[1] == 0x08001000 && id[2] == 0x08001002 && id[3] == 0x08001003 && id[4] == 0x08001004);
    for (int i = 0; i < 5; ++i) CHECK(iid[i] == AR_APM_IID && !(le32(b + off[i] - 8) % 8));
    CHECK(le32(b + off[0]) == 1 && le32(b + off[0] + 4) == AR_SG_SPEAKER && le32(b + off[0] + 8) == 3 && le32(b + off[0] + 20) == 2 &&
          le32(b + off[0] + 32) == 1 && le32(b + off[0] + 44) == 1);
    CHECK(le32(b + off[1] + 4) == AR_SG_SPEAKER && le32(b + off[1] + 24) == 0x0B001001 && le32(b + off[1] + 48) == 8192 &&
          le32(b + off[1] + 60) == 2);
    CHECK(le32(b + off[2] + 12) == 1 && le32(b + off[2] + 16) == AR_MOD_I2S_SINK && le32(b + off[2] + 20) == AR_IID_SPEAKER_I2S);
    CHECK(le32(b + off[3] + 4) == AR_IID_SPEAKER_I2S && le32(b + off[3] + 12) == 0x08001015 && le32(b + off[3] + 20) == 1 &&
          le32(b + off[3] + 24) == 0 && le32(b + off[4]) == 0);

    /* Stream graph: WR_SHMEM -> PCM_DEC -> PCM_CNV and the link into the device graph's I2S sink. */
    n = pagani_ar_open_playback_stream(b, sizeof b, 0x2001, 8);
    CHECK(n && n <= GPR_MAX_PACKET && blocks(b, n, iid, id, off, 8) == 5);
    CHECK(le32(b + off[0] + 4) == AR_SG_PLAY && le32(b + off[0] + 32) == 2);
    CHECK(le32(b + off[2] + 12) == 3 && le32(b + off[2] + 16) == AR_MOD_WR_SHMEM && le32(b + off[2] + 24) == AR_MOD_PCM_DEC &&
          le32(b + off[2] + 32) == AR_MOD_PCM_CNV);
    CHECK(le32(b + off[3]) == 3 && le32(b + off[3] + 20) == 0 && le32(b + off[3] + 24) == 1);
    CHECK(le32(b + off[4]) == 3 && le32(b + off[4] + 4) == AR_IID_PLAY_SHMEM && le32(b + off[4] + 12) == AR_IID_PLAY_DEC &&
          le32(b + off[4] + 36) == AR_IID_PLAY_CNV && le32(b + off[4] + 40) == 1 && le32(b + off[4] + 44) == AR_IID_SPEAKER_I2S &&
          le32(b + off[4] + 48) == 2);

    /* I2S sink on the 13T: LPAIF 0, secondary, the chosen data line, internal word select; 48 kHz, 32 bit, 2 ch, fixed point; factor 1. */
    for (uint32_t sd = AR_I2S_SD0; sd <= AR_I2S_SD1; ++sd) {
        n = pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, &device32, sd);
        CHECK(n && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_SET_CFG && p.dst_port == AR_APM_IID);
        CHECK(blocks(b, n, iid, id, off, 8) == 3 && iid[0] == AR_IID_SPEAKER_I2S && id[0] == 0x08001019 && le32(b + off[0] - 8) == 16);
        CHECK(le32(b + off[0]) == 0 && le32(b + off[0] + 4) == AR_I2S_SECONDARY && le16(b + off[0] + 8) == sd && le16(b + off[0] + 10) == 1);
        CHECK(id[1] == 0x08001017 && le32(b + off[1]) == 48000 && le16(b + off[1] + 4) == 32 && le16(b + off[1] + 6) == 2 &&
              le32(b + off[1] + 8) == 1);
        CHECK(id[2] == 0x08001018 && le32(b + off[2]) == 1);
    }
    n = pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, &device16, AR_I2S_SD0);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 3 && le16(b + off[1] + 4) == 16);
    CHECK(pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, &device32, 3) == 0);
    CHECK(pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, &device32, 0) == 0);
    const ArFormat bad_bits = {48000, 20, 2}, bad_channels = {48000, 16, 3}, mono = {48000, 16, 1}, other_rate = {44100, 32, 2};
    CHECK(pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, &bad_bits, AR_I2S_SD0) == 0);
    CHECK(pagani_ar_config_speaker_device(b, sizeof b, 0x2001, 9, NULL, AR_I2S_SD0) == 0);

    /* Decoder: 16-bit interleaved; converter: the device's 32-bit Q31, deinterleaved unpacked; channel maps FL FR. */
    n = pagani_ar_config_playback_stream(b, sizeof b, 0x2001, 10, &stream, &device32);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 2 && iid[0] == AR_IID_PLAY_DEC && iid[1] == AR_IID_PLAY_CNV);
    for (int i = 0; i < 2; ++i) {
        const uint32_t bits = i ? 32 : 16;
        CHECK(id[i] == 0x08001008 && le32(b + off[i]) == 1 && le32(b + off[i] + 4) == 0x09001000 && le32(b + off[i] + 8) == 20);
        CHECK(le16(b + off[i] + 12) == bits && le16(b + off[i] + 14) == 1 && le16(b + off[i] + 16) == bits && le16(b + off[i] + 18) == bits - 1);
        CHECK(le16(b + off[i] + 20) == 1 && le16(b + off[i] + 22) == (i ? 3u : 1u) && le16(b + off[i] + 26) == 2);
        CHECK(b[off[i] + 28] == 1 && b[off[i] + 29] == 2 && le32(b + off[i] - 8) == 32);
    }
    n = pagani_ar_config_playback_stream(b, sizeof b, 0x2001, 10, &stream, &device16);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 2 && le16(b + off[1] + 12) == 16 && le16(b + off[1] + 18) == 15);
    CHECK(pagani_ar_config_playback_stream(b, sizeof b, 0x2001, 10, &mono, &device32) == 0);
    CHECK(pagani_ar_config_playback_stream(b, sizeof b, 0x2001, 10, &stream, &other_rate) == 0);
    CHECK(pagani_ar_config_playback_stream(b, sizeof b, 0x2001, 10, &bad_channels, &device32) == 0);

    /* Shared memory media format: to the endpoint's own port; 16-bit stereo 48 kHz; the reference client's payload size. */
    n = pagani_ar_config_playback_shmem(b, sizeof b, 0x2001, 11, &stream);
    CHECK(n && pagani_gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && blocks(b, n, iid, id, off, 8) == 1);
    CHECK(id[0] == 0x0800100C && le32(b + off[0] - 8) == 32 && le32(b + off[0]) == 1 && le32(b + off[0] + 4) == 0x09001000 &&
          le32(b + off[0] + 8) == 36 && le32(b + off[0] + 12) == 48000);
    CHECK(le16(b + off[0] + 16) == 16 && le16(b + off[0] + 18) == 1 && le16(b + off[0] + 20) == 16 && le16(b + off[0] + 22) == 15 &&
          le16(b + off[0] + 24) == 1 && le16(b + off[0] + 26) == 2 && b[off[0] + 28] == 1 && b[off[0] + 29] == 2);

    /* Graph commands: one subgraph each; anything else is refused. */
    const uint32_t ops[4] = {AR_OP_GRAPH_PREPARE, AR_OP_GRAPH_START, AR_OP_GRAPH_STOP, AR_OP_GRAPH_CLOSE};
    for (int i = 0; i < 4; ++i) {
        n = pagani_ar_graph_command(b, sizeof b, 0x2001, 12, ops[i], AR_SG_PLAY);
        CHECK(n == 24 + 16 + 24 && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == ops[i] && blocks(b, n, iid, id, off, 8) == 1 &&
              id[0] == 0x08001005 && le32(b + off[0]) == 1 && le32(b + off[0] + 4) == AR_SG_PLAY);
    }
    CHECK(pagani_ar_graph_command(b, sizeof b, 0x2001, 12, AR_OP_SET_CFG, AR_SG_PLAY) == 0);

    /* Memory map: the 13T buffer as the DSP takes it (0xd0000000 | stream index 1 << 32), 64 KiB; the trailing header space. */
    n = pagani_ar_map(b, sizeof b, 0x2001, 13, 0x1d0000000ull, 0x10000);
    CHECK(n == 24 + 20 + 16 && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_MEM_MAP && p.dst_port == AR_APM_IID);
    CHECK(le16(b + 24) == 3 && le16(b + 26) == 1 && le32(b + 28) == 0 && le32(b + 32) == 0xd0000000u && le32(b + 36) == 1 &&
          le32(b + 40) == 0x10000);
    n = pagani_ar_map(b, sizeof b, 0x2001, 13, 0x8812345000ull, 5000);
    CHECK(n && le32(b + 32) == 0x12345000 && le32(b + 36) == 0x88 && le32(b + 40) == 8192);
    CHECK(pagani_ar_map(b, sizeof b, 0x2001, 13, 0x1000, 0) == 0);
    n = pagani_ar_unmap(b, sizeof b, 0x2001, 14, 0x55);
    CHECK(n == 24 + 4 + 16 && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_MEM_UNMAP && le32(b + 24) == 0x55);

    /* One period: 480 frames x 2 x 16 bit from slot 3 of the DSP buffer, to the shared memory endpoint, no time stamp. */
    n = pagani_ar_write(b, sizeof b, 0x2001, 0x00050007u, 0x1d0000000ull + 3 * 1920, 0x55, 1920);
    CHECK(n == 24 + 44 && pagani_gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && p.opcode == AR_OP_WRITE &&
          p.token == 0x00050007u);
    CHECK(le32(b + 24) == 0xd0000000u + 3 * 1920 && le32(b + 28) == 1 && le32(b + 32) == 0x55 && le32(b + 36) == 1920);
    for (int i = 40; i < 68; i += 4) CHECK(le32(b + i) == 0);

    /* PRM: the secondary MI2S bit clock (CLOCK_ID_SEC_MI2S_IBIT 0x102) at 48000 x 2 x 32 = 3.072 MHz, "couple no", default root, sent
     * from the PRM's port 2 or the downstream client's 7; its release carries the id only. */
    const uint32_t ports[2] = {AR_PRM_IID, AR_PRM_PORT_ASM};
    for (int i = 0; i < 2; ++i) {
        n = pagani_ar_prm_clock(b, sizeof b, ports[i], 20, AR_CLK_SEC_MI2S_IBIT, 48000 * 2 * 32, AR_CLK_ATTRIBUTE_COUPLE_NO);
        CHECK(n == 24 + 16 + 16 + 20 && pagani_gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_PRM_IID && p.src_port == ports[i] &&
              p.opcode == AR_OP_PRM_REQUEST);
        CHECK(le32(b + 36) == 36 && le32(b + 40) == AR_PRM_IID && le32(b + 44) == 0x0800102C && le32(b + 48) == 20 && le32(b + 52) == 0);
        CHECK(le32(b + 56) == 1 && le32(b + 60) == 0x102 && le32(b + 64) == 3072000 && le32(b + 68) == 1 && le32(b + 72) == 0);
        n = pagani_ar_prm_clock(b, sizeof b, ports[i], 21, AR_CLK_SEC_MI2S_IBIT, 0, AR_CLK_ATTRIBUTE_COUPLE_NO);
        CHECK(n == 24 + 16 + 16 + 8 && pagani_gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_PRM_RELEASE && le32(b + 48) == 8 &&
              le32(b + 56) == 1 && le32(b + 60) == 0x102);
    }

    /* Too small an output refuses every builder. */
    CHECK(pagani_ar_open_playback_stream(b, 100, 1, 1) == 0 && pagani_ar_config_playback_stream(b, 60, 1, 1, &stream, &device32) == 0);
    CHECK(pagani_ar_write(b, 40, 1, 1, 0, 0, 1920) == 0 && pagani_ar_prm_clock(b, 30, 2, 1, 0x102, 1, 1) == 0);

    /* Answers. */
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = 1, .dst_port = 0x2001, .token = 5,
                   .opcode = GPR_OP_BASIC_RESULT};
    uint32_t words[10] = {AR_OP_GRAPH_OPEN, 0};
    ArAnswer a;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(pagani_ar_parse(b, n, &a) == ArBasicResult && a.result_opcode == AR_OP_GRAPH_OPEN && a.status == 0 && a.token == 5 &&
          a.dst_port == 0x2001);
    CHECK(pagani_ar_parse(b, n - 4, &a) == ArBad);
    r.opcode = AR_OP_MEM_MAP_DONE; words[0] = 0x1234;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(pagani_ar_parse(b, n, &a) == ArMapped && a.handle == 0x1234);
    r.opcode = AR_OP_WRITE_DONE; r.token = 0x00030009u; words[0] = 0xd0000780u; words[1] = 1; words[2] = 0x1234; words[3] = 0;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 32);
    CHECK(pagani_ar_parse(b, n, &a) == ArWriteDone && a.address == 0x1d0000780ull && a.handle == 0x1234 && a.status == 0 &&
          a.token == 0x00030009u);
    r.opcode = AR_OP_WRITE_DONE;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 12);
    CHECK(pagani_ar_parse(b, n, &a) == ArBad);
    r.opcode = GPR_OP_APM_SPF_STATE; words[0] = 1;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(pagani_ar_parse(b, n, &a) == ArFrameworkState && a.status == 1);
    r.opcode = AR_OP_PRM_REQUEST_DONE; words[0] = AR_OP_PRM_REQUEST; words[1] = 0;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(pagani_ar_parse(b, n, &a) == ArPrmDone && a.status == 0);
    words[1] = 0x0c;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(pagani_ar_parse(b, n, &a) == ArPrmDone && a.status == 0x0c);
    r.opcode = 0x12345678;
    n = pagani_gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(pagani_ar_parse(b, n, &a) == ArUnknown && a.opcode == 0x12345678);
    CHECK(pagani_ar_parse(b, 10, &a) == ArBad);
}

int main(void)
{
    gpr();
    reach();
    printf("PASS protocol: %u checks\n", checks);
    return 0;
}
