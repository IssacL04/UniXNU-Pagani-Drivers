/* SPDX-License-Identifier: MIT */
#ifndef SM8750_AOSS_QMP_H
#define SM8750_AOSS_QMP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The AOSS QMP mailbox of the SM8750 (ARCHITECTURE 4.17 P3 row, section 10 item 19): a message RAM shared with the AOP
 * (DT aoss_qmp, 0x0C300000, 0x400 bytes) and the IPCC doorbell that wakes the AOP (DT ipcc_mproc 0x00406000,
 * mboxes = <&ipcc 0 0>: client 0, signal 0, so the doorbell is SEND_ID (+0x0C) = 0). Semantics of Linux
 * drivers/soc/qcom/qcom_aoss.c:
 *  - descriptor: magic 0x4D41494C at +0x00, version 1 at +0x04; the AOP (ucore) link/channel state and acks at
 *    +0x0C..+0x18, its mailbox at +0x1C/+0x20; ours (mcore) at +0x24..+0x30, our mailbox size/offset at +0x34/+0x38;
 *  - open: ack the AOP's link state, link up, kick, wait for the link ack; channel up, kick, wait for the AOP's channel;
 *    ack it, kick, wait for the ack of ours. Each wait at most 1 s. A channel a boot stage left fully open is used as is;
 *  - send: a zero-padded 64-byte string written as 16 words at mailbox+4, then the length 64 at mailbox+0, read back,
 *    kick, wait at most 1 s for the AOP to clear the length; on timeout the length is cleared again.
 * KGSL tells the AOP the GPU ACD state before the first GMU boot ("{class: gpu, res: acd, val: %d}",
 * adreno_gen8_gmu.c:1760-1777,1838): the driver sends val 0 at the start of S1 with pagani-gpu-qmp=1 (M3) and val 1 when
 * ACD is enabled (M13). Whether EL1 may touch the message RAM and the IPCC under Gunyah is unknown (a refused access is a
 * synchronous external abort): the caller only runs this with pagani-gpu-qmp=1. A descriptor that does not read back
 * right is refused before anything is written.
 * No allocation, no logging, no interrupt: the AOP's answers are polled through the delay callback. */

/* 32-bit access at a byte offset inside one mapped window (Device-nGnRE), the same shape as struct peripheral_io of SOC
 * kits/Sm8750Platform/PeripheralIO.h. write must not return before the store completed (str + dsb sy): the message words
 * must be in the RAM before the length, and the length before the doorbell. delay may sleep. */
typedef struct Sm8750AossQmpIo {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void (*delay)(void *context, unsigned microseconds);
} Sm8750AossQmpIo;

/* Return codes: 0 or a negative errno value. */
enum {
    Sm8750AossQmpOk = 0,
    Sm8750AossQmpTooLong = -7,          /* message of 64 bytes or more (the terminating zero must fit) */
    Sm8750AossQmpBusy = -16,            /* the mailbox still held an earlier message after 1 s */
    Sm8750AossQmpArgument = -22,
    Sm8750AossQmpRefused = -71,         /* descriptor: magic, version, mailbox size/offset; nothing was written */
    Sm8750AossQmpTimeout = -110,        /* the AOP did not answer within 1 s */
};

/* The QMP descriptor layout (Linux qcom_aoss.c). The platform layer stays self-contained, so the values are spelled here;
 * A830Regs.h sections 18 and 21 carry the same facts (SM8750_AOSS_QMP_*), and tests/test_sm8750_aossqmp.c compares them. */
enum {
    Sm8750AossQmpMsgramBytes = 0x400,
    Sm8750AossQmpMagic = 0x4d41494c,    /* "LIAM" read as a little-endian word */
    Sm8750AossQmpVersion = 1,
    Sm8750AossQmpDescMagic = 0x00,
    Sm8750AossQmpDescVersion = 0x04,
    Sm8750AossQmpDescFeatures = 0x08,
    Sm8750AossQmpUcoreLinkState = 0x0c,
    Sm8750AossQmpUcoreLinkStateAck = 0x10,
    Sm8750AossQmpUcoreChState = 0x14,
    Sm8750AossQmpUcoreChStateAck = 0x18,
    Sm8750AossQmpUcoreMboxSize = 0x1c,
    Sm8750AossQmpUcoreMboxOffset = 0x20,
    Sm8750AossQmpMcoreLinkState = 0x24,
    Sm8750AossQmpMcoreLinkStateAck = 0x28,
    Sm8750AossQmpMcoreChState = 0x2c,
    Sm8750AossQmpMcoreChStateAck = 0x30,
    Sm8750AossQmpMcoreMboxSize = 0x34,
    Sm8750AossQmpMcoreMboxOffset = 0x38,
    Sm8750AossQmpDescBytes = 0x3c,
    Sm8750AossQmpStateUp = 0x0000ffff,
    Sm8750AossQmpStateDown = (int)0xffff0000,
    Sm8750AossQmpMessageBytes = 64,     /* Linux QMP_MSG_LEN */
    Sm8750AossQmpIpccSendId = 0x0c,     /* IPCC_REG_SEND_ID: client_id << 16 | signal_id */
    Sm8750AossQmpIpccSignal = 0,        /* <&ipcc 0 0>: IPCC_CLIENT_AOP, IPCC_MPROC_SIGNAL_GLINK_QMP */
    Sm8750AossQmpPollUs = 100,
    Sm8750AossQmpPolls = 10000,         /* 1 s per wait */
};

/* Steps, in order; Sm8750AossQmp::step keeps the one reached (or the one that failed). */
typedef enum {
    Sm8750AossQmpStepNone,
    Sm8750AossQmpStepDescriptor,
    Sm8750AossQmpStepLink,              /* waiting for MCORE_LINK_STATE_ACK */
    Sm8750AossQmpStepChannel,           /* waiting for UCORE_CH_STATE */
    Sm8750AossQmpStepChannelAck,        /* waiting for MCORE_CH_STATE_ACK */
    Sm8750AossQmpStepOpen,
    Sm8750AossQmpStepMailbox,           /* waiting for an earlier message to be taken */
    Sm8750AossQmpStepSend,              /* waiting for this message to be taken */
    Sm8750AossQmpStepDone,
} Sm8750AossQmpStep;

typedef struct Sm8750AossQmp {
    const Sm8750AossQmpIo *msgram;
    const Sm8750AossQmpIo *ipcc;
    uint32_t msgram_bytes;
    uint32_t mbox_offset, mbox_size;    /* our (mcore) mailbox */
    uint32_t open;                      /* the link and channel are up */
    uint32_t reused;                    /* the channel was found open: no handshake was run */
    uint32_t step;                      /* Sm8750AossQmpStep reached, or where it failed */
    uint32_t polls, kicks, messages;
    uint32_t reg, value, expect;        /* last failed check: message RAM offset, value read, value wanted */
} Sm8750AossQmp;

/* Binds the two windows; nothing is read or written. msgram_bytes: the mapped message RAM (0x400). */
void Sm8750AossQmpInit(Sm8750AossQmp *qmp, const Sm8750AossQmpIo *msgram, uint32_t msgram_bytes,
    const Sm8750AossQmpIo *ipcc);
/* Checks the descriptor and runs the link/channel handshake (once; later calls return Ok). */
int Sm8750AossQmpOpen(Sm8750AossQmp *qmp);
/* Opens if needed, then sends one message (strlen < 64) and waits until the AOP took it. */
int Sm8750AossQmpSend(Sm8750AossQmp *qmp, const char *message);
/* "{class: gpu, res: acd, val: 0}" or "... val: 1}" through qmp. */
int Sm8750AossQmpSendAcdState(Sm8750AossQmp *qmp, int val);
/* The design's one-shot form (IMPLEMENTATION_PLAN M3): open on a 0x400-byte message RAM, send the ACD state. */
int Sm8750AossQmpSendAcd(const Sm8750AossQmpIo *msgram, const Sm8750AossQmpIo *ipcc, int val);
/* The text of the ACD state message (zero padded to 64 bytes in out). Returns its length. */
unsigned Sm8750AossQmpAcdMessage(int val, char out[Sm8750AossQmpMessageBytes]);
/* The status block value of a result (ARCHITECTURE 8.2 qmp): 1 done, 2 timeout or busy, 3 refused (bad descriptor or
 * arguments). 0 (not run) is the caller's when it never called. */
unsigned Sm8750AossQmpStatusCode(int rc);

#ifdef __cplusplus
}
#endif
#endif
