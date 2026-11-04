/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengAudioReach.h (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). Changes for the
 * OnePlus 13T: the device side has its own format (the 13T's secondary MI2S runs 32-bit slots, /odm/etc/backend_conf.xml
 * "MI2S-LPAIF-RX-SECONDARY 48000 2 32"; sheng's ran 16) and its own I2S data line; the stream configuration takes both formats; the
 * PRM clock request names the sending port; the microphone graphs are left out (the 13T microphones sit behind another codec path);
 * the functions carry a pagani_ prefix. */
/* The audio DSP's graph manager (APM) commands of the 13T speaker path, as GPR packets. Two graphs: a device graph that owns the
 * hardware endpoint (I2S sink on LPAIF secondary MI2S) and a stream graph that owns the shared memory endpoint
 * (WR_SHMEM -> PCM_DEC -> PCM_CNV); the stream graph's open carries the connection into the device graph. The instance numbers are the
 * client's choice (sheng's board topology values: device subgraph 0x4005, stream subgraph 0x4001). Each packet stays within one GLINK
 * intent (GPR_MAX_PACKET). Pure encoding: no I/O, no waits. Field layouts: Linux sound/soc/qcom/qdsp6/audioreach.h (v6.12) and the
 * downstream audio_prm.h, as facts. */
#ifndef PAGANI_REACH_H
#define PAGANI_REACH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AR_APM_IID 1u
#define AR_OP_GRAPH_OPEN 0x01001000u
#define AR_OP_GRAPH_PREPARE 0x01001001u
#define AR_OP_GRAPH_START 0x01001002u
#define AR_OP_GRAPH_STOP 0x01001003u
#define AR_OP_GRAPH_CLOSE 0x01001004u
#define AR_OP_SET_CFG 0x01001006u
#define AR_OP_MEM_MAP 0x0100100Cu
#define AR_OP_MEM_UNMAP 0x0100100Du
#define AR_OP_MEM_MAP_DONE 0x02001001u          /* payload {handle} */
#define AR_OP_WRITE 0x0400100Au                 /* DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 */
#define AR_OP_WRITE_DONE 0x05001004u
#define AR_OP_EOS_RENDERED 0x05001001u

/* Module ids and the instance ids of the 13T speaker graphs. */
#define AR_MOD_WR_SHMEM 0x07001000u
#define AR_MOD_PCM_CNV 0x07001003u
#define AR_MOD_PCM_DEC 0x07001005u
#define AR_MOD_I2S_SINK 0x0700100Au

#define AR_SG_PLAY 0x4001u
#define AR_SG_SPEAKER 0x4005u
#define AR_IID_PLAY_SHMEM 0x6001u
#define AR_IID_PLAY_DEC 0x6002u
#define AR_IID_PLAY_CNV 0x6003u
#define AR_IID_SPEAKER_I2S 0x6051u

/* PARAM_ID_I2S_INTF_CFG values: LPAIF (type 0), secondary interface, data line index, word select from the SoC. */
#define AR_I2S_SECONDARY 1u
#define AR_I2S_SD0 1u
#define AR_I2S_SD1 2u
#define AR_I2S_WS_INTERNAL 1u

typedef struct {
    uint32_t rate;           /* Hz */
    uint16_t bits;           /* 16 or 32 (sample width = slot width; Q bits - 1) */
    uint16_t channels;       /* 1 or 2 */
} ArFormat;

/* Each builder writes one complete GPR packet into out (capacity bytes) and returns its size, or 0 when it does not fit or an argument is
 * out of range. port: the GPR port the answers come back to; token: echoed in the answer. */
uint32_t pagani_ar_open_speaker_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* The I2S sink: LPAIF secondary MI2S on data line sd_line (AR_I2S_SD0 / AR_I2S_SD1), internal word select, the device format as the
 * hardware endpoint media format, frame size factor 1. */
uint32_t pagani_ar_config_speaker_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *device,
                                         uint32_t sd_line);
uint32_t pagani_ar_open_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* The decoder outputs the stream format interleaved; the converter turns it into the device's sample width (Q = bits - 1),
 * deinterleaved unpacked, the form the I2S sink takes. */
uint32_t pagani_ar_config_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *stream,
                                          const ArFormat *device);
/* The input media format of the shared memory endpoint, sent to the endpoint itself. */
uint32_t pagani_ar_config_playback_shmem(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *stream);
/* opcode: AR_OP_GRAPH_PREPARE, _START, _STOP or _CLOSE for the one subgraph sg. */
uint32_t pagani_ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t opcode, uint32_t sg);
/* Maps one physically contiguous region (rounded up to 4 KiB) for the DSP; the answer is AR_OP_MEM_MAP_DONE with the handle. address:
 * as the DSP takes it (32-bit address, stream index in the upper word). */
uint32_t pagani_ar_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t bytes);
uint32_t pagani_ar_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t handle);
/* One buffer for the playback shared memory endpoint; the answer is AR_OP_WRITE_DONE with the same token. */
uint32_t pagani_ar_write(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t handle,
                         uint32_t bytes);

/* The DSP's power and resource manager (PRM, instance 2). A clock request (hz != 0: one clock, its frequency, attribute, default root)
 * or release (hz == 0: the clock id only); the answer is AR_OP_PRM_REQUEST_DONE / _RELEASE_DONE, or a basic result with an error. The
 * sending port is the caller's: sheng's finding is that PRM commands are taken from the PRM's own port (2); the downstream client sends
 * from GPR_SVC_ASM (7). */
#define AR_PRM_IID 2u
#define AR_PRM_PORT_ASM 7u
#define AR_OP_PRM_REQUEST 0x0100100Fu
#define AR_OP_PRM_RELEASE 0x01001010u
#define AR_OP_PRM_REQUEST_DONE 0x02001002u
#define AR_OP_PRM_RELEASE_DONE 0x02001003u
#define AR_PARAM_PRM_HW_CLOCK 0x0800102Cu    /* PARAM_ID_RSC_AUDIO_HW_CLK */
#define AR_CLK_SEC_MI2S_IBIT 0x102u           /* CLOCK_ID_SEC_MI2S_IBIT */
#define AR_CLK_ATTRIBUTE_COUPLE_NO 1u
uint32_t pagani_ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz,
                             uint32_t attribute);

/* Answers. */
typedef struct {
    uint32_t opcode, token, src_port, dst_port;
    uint32_t result_opcode, status;  /* basic result: the command and its status (0 success); framework state: 1 ready; PRM: status */
    uint32_t handle;                 /* AR_OP_MEM_MAP_DONE, AR_OP_WRITE_DONE */
    uint64_t address;                /* write done: the buffer */
} ArAnswer;
enum {ArUnknown = 0, ArBasicResult = 1, ArMapped = 2, ArWriteDone = 3, ArFrameworkState = 5, ArPrmDone = 6, ArBad = -1};
int pagani_ar_parse(const uint8_t *in, uint32_t length, ArAnswer *a);

#ifdef __cplusplus
}
#endif
#endif
