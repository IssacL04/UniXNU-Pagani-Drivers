/* SPDX-License-Identifier: MIT */
/* Host model of the SM8750 AOSS QMP mailbox for Sm8750AossQmp (ARCHITECTURE 4.17 P3, section 10 item 19): the AOP side of
 * the message RAM (Linux qcom_aoss.c descriptor, link/channel handshake, 64-byte messages) and the IPCC doorbell. The AOP
 * reacts to doorbells after a configurable number of 100 us polls, or never; descriptors that do not read back right must
 * be refused without a single write; message body, length and doorbell must come in that order.
 * host-test: platform/sm8750/Sm8750AossQmp.c */
#include "../platform/sm8750/Sm8750AossQmp.h"
#include "../A830Regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);   \
            abort();                                                                \
        }                                                                           \
    } while (0)

/* The self-contained platform enum carries the facts A830Regs.h sections 18 and 21 hold. */
_Static_assert(Sm8750AossQmpMagic == SM8750_AOSS_QMP_MAGIC && Sm8750AossQmpVersion == SM8750_AOSS_QMP_VERSION &&
               Sm8750AossQmpMsgramBytes == SM8750_AOSS_QMP_BYTES && Sm8750AossQmpDescFeatures == SM8750_AOSS_QMP_DESC_FEATURES &&
               Sm8750AossQmpUcoreLinkState == SM8750_AOSS_QMP_UCORE_LINK_STATE &&
               Sm8750AossQmpUcoreLinkStateAck == SM8750_AOSS_QMP_UCORE_LINK_STATE_ACK &&
               Sm8750AossQmpUcoreChState == SM8750_AOSS_QMP_UCORE_CH_STATE &&
               Sm8750AossQmpUcoreChStateAck == SM8750_AOSS_QMP_UCORE_CH_STATE_ACK &&
               Sm8750AossQmpUcoreMboxSize == SM8750_AOSS_QMP_UCORE_MBOX_SIZE &&
               Sm8750AossQmpUcoreMboxOffset == SM8750_AOSS_QMP_UCORE_MBOX_OFFSET &&
               Sm8750AossQmpMcoreLinkState == SM8750_AOSS_QMP_MCORE_LINK_STATE &&
               Sm8750AossQmpMcoreLinkStateAck == SM8750_AOSS_QMP_MCORE_LINK_STATE_ACK &&
               Sm8750AossQmpMcoreChState == SM8750_AOSS_QMP_MCORE_CH_STATE &&
               Sm8750AossQmpMcoreChStateAck == SM8750_AOSS_QMP_MCORE_CH_STATE_ACK &&
               Sm8750AossQmpMcoreMboxSize == SM8750_AOSS_QMP_MCORE_MBOX_SIZE &&
               Sm8750AossQmpMcoreMboxOffset == SM8750_AOSS_QMP_MCORE_MBOX_OFFSET &&
               (uint32_t)Sm8750AossQmpStateUp == SM8750_AOSS_QMP_STATE_UP &&
               (uint32_t)Sm8750AossQmpStateDown == SM8750_AOSS_QMP_STATE_DOWN &&
               Sm8750AossQmpMessageBytes == SM8750_AOSS_QMP_MSG_BYTES && Sm8750AossQmpIpccSendId == SM8750_IPCC_SEND_ID,
               "Sm8750AossQmp.h agrees with A830Regs.h");

enum { Up = 0x0000ffff, Down = 0xffff0000u, Box = 0x100, BoxSize = 0x200, UcoreBox = 0x300 };

typedef struct {
    uint32_t ram[Sm8750AossQmpMsgramBytes / 4];
    /* AOP behaviour */
    int ack_link, open_channel, ack_channel, consume;
    unsigned latency;                       /* polls between a doorbell and the AOP's reaction */
    unsigned consume_after;                 /* consume an earlier (busy) message after this many polls; 0 never */
    /* bookkeeping */
    unsigned countdown, pending;
    unsigned kicks, writes, reads, delays, ipcc_writes;
    unsigned messages;
    char last[Sm8750AossQmpMessageBytes + 1];
    uint32_t last_length;
    unsigned ordering_errors;
    uint32_t write_offsets[512];
    unsigned nwrites;
} Aop;

static Aop aop;
static int checks;

static uint32_t at(uint32_t offset)
{
    return aop.ram[offset / 4];
}

static void aop_react(void)
{
    if (aop.ack_link && at(Sm8750AossQmpMcoreLinkState) == Up)
        aop.ram[Sm8750AossQmpMcoreLinkStateAck / 4] = Up;
    if (aop.open_channel && at(Sm8750AossQmpMcoreChState) == Up)
        aop.ram[Sm8750AossQmpUcoreChState / 4] = Up;
    if (aop.ack_channel && at(Sm8750AossQmpUcoreChStateAck) == Up && at(Sm8750AossQmpMcoreChState) == Up)
        aop.ram[Sm8750AossQmpMcoreChStateAck / 4] = Up;
    if (aop.consume && at(Box) != 0) {
        /* The AOP takes the length word and the 64 bytes behind it as one zero-padded string. */
        aop.last_length = at(Box);
        memset(aop.last, 0, sizeof(aop.last));
        for (unsigned i = 0; i < Sm8750AossQmpMessageBytes; i++)
            aop.last[i] = (char)(aop.ram[(Box + 4 + i) / 4] >> (8 * (i % 4)));
        aop.messages++;
        aop.ram[Box / 4] = 0;
    }
}

static uint32_t ram_read(void *context, uint32_t offset)
{
    (void)context;
    CHECK(!(offset & 3) && offset < Sm8750AossQmpMsgramBytes);
    aop.reads++;
    return at(offset);
}

static void ram_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    CHECK(!(offset & 3) && offset < Sm8750AossQmpMsgramBytes);
    /* Our side only writes its own words: the ucore acks, our link/channel state, our mailbox. */
    CHECK(offset == Sm8750AossQmpUcoreLinkStateAck || offset == Sm8750AossQmpUcoreChStateAck ||
        offset == Sm8750AossQmpMcoreLinkState || offset == Sm8750AossQmpMcoreChState ||
        (offset >= Box && offset < Box + 4 + Sm8750AossQmpMessageBytes));
    /* The length goes in only after the whole body, and only when the box is empty. */
    if (offset == Box && value && at(Box))
        aop.ordering_errors++;
    aop.ram[offset / 4] = value;
    aop.writes++;
    if (aop.nwrites < 512)
        aop.write_offsets[aop.nwrites++] = offset;
}

static void ram_delay(void *context, unsigned microseconds)
{
    (void)context;
    CHECK(microseconds == Sm8750AossQmpPollUs);
    aop.delays++;
    if (aop.consume_after && aop.delays == aop.consume_after && at(Box)) {
        aop.ram[Box / 4] = 0;               /* an earlier message finally taken */
        aop.consume_after = 0;
    }
    if (aop.pending && aop.countdown && --aop.countdown == 0) {
        aop.pending = 0;
        aop_react();
    }
}

static uint32_t ipcc_read(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    CHECK(0);                               /* the doorbell is write only */
    return 0;
}

static void ipcc_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    CHECK(offset == Sm8750AossQmpIpccSendId && value == 0);   /* <&ipcc 0 0>: client 0, signal 0 */
    aop.ipcc_writes++;
    aop.kicks++;
    /* A doorbell for a message (the last RAM write was its length): the whole zero-terminated body is already there. */
    if (aop.nwrites && aop.write_offsets[aop.nwrites - 1] == Box) {
        char text[Sm8750AossQmpMessageBytes + 1] = {0};
        for (unsigned i = 0; i < Sm8750AossQmpMessageBytes; i++)
            text[i] = (char)(aop.ram[(Box + 4 + i) / 4] >> (8 * (i % 4)));
        if (at(Box) != Sm8750AossQmpMessageBytes || !text[0] || text[Sm8750AossQmpMessageBytes - 1])
            aop.ordering_errors++;
    }
    if (aop.latency) {
        aop.pending = 1;
        aop.countdown = aop.latency;
    } else {
        aop_react();
    }
}

static void ipcc_delay(void *context, unsigned microseconds)
{
    (void)context;
    (void)microseconds;
    CHECK(0);
}

static const Sm8750AossQmpIo msgram_io = {NULL, ram_read, ram_write, ram_delay};
static const Sm8750AossQmpIo ipcc_io = {NULL, ipcc_read, ipcc_write, ipcc_delay};

static void reset(void)
{
    memset(&aop, 0, sizeof(aop));
    aop.ram[Sm8750AossQmpDescMagic / 4] = Sm8750AossQmpMagic;
    aop.ram[Sm8750AossQmpDescVersion / 4] = Sm8750AossQmpVersion;
    aop.ram[Sm8750AossQmpUcoreLinkState / 4] = Up;   /* the AOP's link is up from its boot */
    aop.ram[Sm8750AossQmpUcoreMboxSize / 4] = 0x100;
    aop.ram[Sm8750AossQmpUcoreMboxOffset / 4] = UcoreBox;
    aop.ram[Sm8750AossQmpMcoreMboxSize / 4] = BoxSize;
    aop.ram[Sm8750AossQmpMcoreMboxOffset / 4] = Box;
    aop.ack_link = aop.open_channel = aop.ack_channel = aop.consume = 1;
}

static int send_acd(Sm8750AossQmp *q, int val)
{
    Sm8750AossQmpInit(q, &msgram_io, Sm8750AossQmpMsgramBytes, &ipcc_io);
    return Sm8750AossQmpSendAcdState(q, val);
}

static void test_message_text(void)
{
    char out[Sm8750AossQmpMessageBytes];
    memset(out, 0x55, sizeof(out));
    unsigned n = Sm8750AossQmpAcdMessage(0, out);
    CHECK(n == strlen("{class: gpu, res: acd, val: 0}") && !strcmp(out, "{class: gpu, res: acd, val: 0}"));
    for (unsigned i = n; i < sizeof(out); i++)
        CHECK(out[i] == 0);
    n = Sm8750AossQmpAcdMessage(1, out);
    CHECK(!strcmp(out, "{class: gpu, res: acd, val: 1}") && n == 30);
    checks += 3;
}

static void test_handshake_and_message(int val, unsigned latency)
{
    reset();
    aop.latency = latency;
    Sm8750AossQmp q;
    CHECK(send_acd(&q, val) == Sm8750AossQmpOk);
    CHECK(q.open && !q.reused && q.step == Sm8750AossQmpStepDone && q.messages == 1);
    CHECK(q.mbox_offset == Box && q.mbox_size == BoxSize);
    /* Linux qmp_open: ucore link acked with its own value, our link and channel up, the ucore channel acked. */
    CHECK(at(Sm8750AossQmpUcoreLinkStateAck) == Up && at(Sm8750AossQmpMcoreLinkState) == Up &&
        at(Sm8750AossQmpMcoreChState) == Up && at(Sm8750AossQmpUcoreChStateAck) == Up);
    CHECK(aop.kicks == 4 && q.kicks == 4);  /* link, channel, channel ack, message */
    CHECK(aop.messages == 1 && aop.last_length == Sm8750AossQmpMessageBytes);
    CHECK(!strcmp(aop.last, val ? "{class: gpu, res: acd, val: 1}" : "{class: gpu, res: acd, val: 0}"));
    for (unsigned i = strlen(aop.last); i < Sm8750AossQmpMessageBytes; i++)
        CHECK(aop.last[i] == 0);
    CHECK(at(Box) == 0 && !aop.ordering_errors);
    /* The length is the last message RAM write of the send (body first, all 16 words). */
    unsigned body = 0, length_at = 0;
    for (unsigned i = 0; i < aop.nwrites; i++) {
        if (aop.write_offsets[i] > Box && aop.write_offsets[i] < Box + 4 + Sm8750AossQmpMessageBytes)
            body++;
        if (aop.write_offsets[i] == Box && !length_at)
            length_at = i;
    }
    CHECK(body == 16 && length_at == aop.nwrites - 1);
    CHECK(Sm8750AossQmpStatusCode(Sm8750AossQmpOk) == 1);
    if (latency)
        CHECK(q.polls >= 4 * (latency - 1));
    /* A second message reuses the open channel: no handshake. */
    unsigned kicks = aop.kicks;
    CHECK(Sm8750AossQmpSendAcdState(&q, !val) == Sm8750AossQmpOk && aop.kicks == kicks + 1 && aop.messages == 2);
    checks += 14;
}

static void expect_refused_without_writes(void (*spoil)(void), const char *what)
{
    reset();
    spoil();
    Sm8750AossQmp q;
    int rc = send_acd(&q, 0);
    if (rc != Sm8750AossQmpRefused) {
        fprintf(stderr, "%s: rc %d\n", what, rc);
        CHECK(0);
    }
    CHECK(aop.writes == 0 && aop.kicks == 0 && q.step == Sm8750AossQmpStepDescriptor);
    CHECK(Sm8750AossQmpStatusCode(rc) == 3);
    checks += 3;
}

static void spoil_magic(void) {aop.ram[0] = 0;}
static void spoil_magic_ones(void) {aop.ram[0] = 0xffffffffu;}
static void spoil_version(void) {aop.ram[1] = 2;}
static void spoil_size_zero(void) {aop.ram[Sm8750AossQmpMcoreMboxSize / 4] = 0;}
static void spoil_size_small(void) {aop.ram[Sm8750AossQmpMcoreMboxSize / 4] = 64;}
static void spoil_offset_unaligned(void) {aop.ram[Sm8750AossQmpMcoreMboxOffset / 4] = Box + 2;}
static void spoil_offset_beyond(void) {aop.ram[Sm8750AossQmpMcoreMboxOffset / 4] = 0x3e0;}
static void spoil_offset_descriptor(void) {aop.ram[Sm8750AossQmpMcoreMboxOffset / 4] = 0x20;}
static void spoil_size_beyond(void) {aop.ram[Sm8750AossQmpMcoreMboxSize / 4] = 0x400;}

static void test_refusals(void)
{
    expect_refused_without_writes(spoil_magic, "magic 0");
    expect_refused_without_writes(spoil_magic_ones, "magic ~0 (refused bus access)");
    expect_refused_without_writes(spoil_version, "version 2");
    expect_refused_without_writes(spoil_size_zero, "size 0");
    expect_refused_without_writes(spoil_size_small, "size < 68");
    expect_refused_without_writes(spoil_offset_unaligned, "unaligned box");
    expect_refused_without_writes(spoil_offset_beyond, "box beyond the RAM");
    expect_refused_without_writes(spoil_offset_descriptor, "box inside the descriptor");
    expect_refused_without_writes(spoil_size_beyond, "box size beyond the RAM");
}

static void test_timeouts(void)
{
    Sm8750AossQmp q;
    /* Link never acked: 10000 polls of 100 us (1 s), then our link down again. */
    reset();
    aop.ack_link = 0;
    CHECK(send_acd(&q, 0) == Sm8750AossQmpTimeout);
    CHECK(q.step == Sm8750AossQmpStepLink && aop.delays == Sm8750AossQmpPolls && q.reg == Sm8750AossQmpMcoreLinkStateAck);
    CHECK(at(Sm8750AossQmpMcoreLinkState) == Down && at(Sm8750AossQmpMcoreChState) == 0 && aop.kicks == 2);
    CHECK(Sm8750AossQmpStatusCode(Sm8750AossQmpTimeout) == 2 && aop.messages == 0 && !q.open);
    /* Channel never opened by the AOP. */
    reset();
    aop.open_channel = 0;
    CHECK(send_acd(&q, 0) == Sm8750AossQmpTimeout && q.step == Sm8750AossQmpStepChannel);
    CHECK(at(Sm8750AossQmpMcoreChState) == Down && at(Sm8750AossQmpMcoreLinkState) == Down && aop.kicks == 3);
    /* Our channel never acked. */
    reset();
    aop.ack_channel = 0;
    CHECK(send_acd(&q, 0) == Sm8750AossQmpTimeout && q.step == Sm8750AossQmpStepChannelAck && aop.kicks == 4);
    CHECK(at(Sm8750AossQmpMcoreChState) == Down && at(Sm8750AossQmpMcoreLinkState) == Down);
    /* A message the AOP never takes is withdrawn (length back to 0). */
    reset();
    aop.consume = 0;
    CHECK(send_acd(&q, 0) == Sm8750AossQmpTimeout && q.step == Sm8750AossQmpStepSend);
    CHECK(at(Box) == 0 && q.open && aop.delays == Sm8750AossQmpPolls);
    /* A late AOP (37 polls per answer) is fine. */
    test_handshake_and_message(0, 37);
    checks += 11;
}

static void test_busy_mailbox(void)
{
    Sm8750AossQmp q;
    /* An earlier message still in the box: waited for, then ours goes in. */
    reset();
    aop.ram[Box / 4] = Sm8750AossQmpMessageBytes;
    for (unsigned i = 0; i < 16; i++)
        aop.ram[(Box + 4) / 4 + i] = 0x6c6c6568;   /* "hell" */
    aop.consume = 0;
    aop.consume_after = 50;
    Sm8750AossQmpInit(&q, &msgram_io, Sm8750AossQmpMsgramBytes, &ipcc_io);
    CHECK(Sm8750AossQmpOpen(&q) == Sm8750AossQmpOk);
    aop.consume = 1;
    CHECK(Sm8750AossQmpSendAcdState(&q, 1) == Sm8750AossQmpOk && !strcmp(aop.last, "{class: gpu, res: acd, val: 1}"));
    CHECK(!aop.ordering_errors);
    /* One that never leaves: Busy, and nothing of it is overwritten. */
    reset();
    aop.ram[Box / 4] = Sm8750AossQmpMessageBytes;
    for (unsigned i = 0; i < 16; i++)
        aop.ram[(Box + 4) / 4 + i] = 0x6c6c6568;
    aop.consume = 0;
    Sm8750AossQmpInit(&q, &msgram_io, Sm8750AossQmpMsgramBytes, &ipcc_io);
    CHECK(Sm8750AossQmpSendAcdState(&q, 0) == Sm8750AossQmpBusy && q.step == Sm8750AossQmpStepMailbox);
    for (unsigned i = 0; i < 16; i++)
        CHECK(aop.ram[(Box + 4) / 4 + i] == 0x6c6c6568);
    CHECK(at(Box) == Sm8750AossQmpMessageBytes && Sm8750AossQmpStatusCode(Sm8750AossQmpBusy) == 2);
    checks += 6;
}

static void test_open_channel_reused(void)
{
    reset();
    aop.ram[Sm8750AossQmpMcoreLinkState / 4] = Up;
    aop.ram[Sm8750AossQmpMcoreLinkStateAck / 4] = Up;
    aop.ram[Sm8750AossQmpMcoreChState / 4] = Up;
    aop.ram[Sm8750AossQmpUcoreChState / 4] = Up;
    aop.ram[Sm8750AossQmpMcoreChStateAck / 4] = Up;
    Sm8750AossQmp q;
    CHECK(send_acd(&q, 0) == Sm8750AossQmpOk && q.reused && aop.kicks == 1);
    for (unsigned i = 0; i < aop.nwrites; i++)
        CHECK(aop.write_offsets[i] >= Box);  /* only the mailbox was written */
    checks += 2;
}

static void test_arguments(void)
{
    Sm8750AossQmp q;
    reset();
    char longer[80];
    memset(longer, 'a', sizeof(longer));
    longer[64] = 0;
    Sm8750AossQmpInit(&q, &msgram_io, Sm8750AossQmpMsgramBytes, &ipcc_io);
    CHECK(Sm8750AossQmpSend(&q, longer) == Sm8750AossQmpTooLong && aop.reads == 0 && aop.writes == 0);
    longer[63] = 0;
    CHECK(Sm8750AossQmpSend(&q, longer) == Sm8750AossQmpOk);   /* 63 characters and the terminator fit */
    CHECK(Sm8750AossQmpSend(&q, "") == Sm8750AossQmpArgument && Sm8750AossQmpSend(&q, NULL) == Sm8750AossQmpArgument);
    CHECK(Sm8750AossQmpSend(NULL, "x") == Sm8750AossQmpArgument && Sm8750AossQmpOpen(NULL) == Sm8750AossQmpArgument);
    reset();
    Sm8750AossQmpInit(&q, NULL, Sm8750AossQmpMsgramBytes, &ipcc_io);
    CHECK(Sm8750AossQmpOpen(&q) == Sm8750AossQmpArgument);
    Sm8750AossQmpInit(&q, &msgram_io, Sm8750AossQmpMsgramBytes, NULL);
    CHECK(Sm8750AossQmpOpen(&q) == Sm8750AossQmpArgument);
    Sm8750AossQmpInit(&q, &msgram_io, 0x40, &ipcc_io);
    CHECK(Sm8750AossQmpOpen(&q) == Sm8750AossQmpArgument && aop.reads == 0);
    CHECK(Sm8750AossQmpStatusCode(Sm8750AossQmpArgument) == 3 && Sm8750AossQmpStatusCode(Sm8750AossQmpRefused) == 3);
    /* The design's one-shot form. */
    reset();
    CHECK(Sm8750AossQmpSendAcd(&msgram_io, &ipcc_io, 0) == Sm8750AossQmpOk &&
        !strcmp(aop.last, "{class: gpu, res: acd, val: 0}"));
    reset();
    CHECK(Sm8750AossQmpSendAcd(&msgram_io, &ipcc_io, 1) == Sm8750AossQmpOk &&
        !strcmp(aop.last, "{class: gpu, res: acd, val: 1}"));
    checks += 10;
}

int main(void)
{
    test_message_text();
    test_handshake_and_message(0, 0);
    test_handshake_and_message(1, 0);
    test_handshake_and_message(1, 3);
    test_refusals();
    test_timeouts();
    test_busy_mailbox();
    test_open_channel_reused();
    test_arguments();
    printf("PASS: AOSS QMP handshake, 64-byte ACD messages (val 0 and 1), IPCC doorbell, timeouts and refusals (%d checks)\n",
        checks);
    return 0;
}
