/* SPDX-License-Identifier: MIT */
#include "Sm8750AossQmp.h"
#include <stddef.h>

static uint32_t rd(const Sm8750AossQmp *q, uint32_t offset)
{
    return q->msgram->read(q->msgram->context, offset);
}

static void wr(const Sm8750AossQmp *q, uint32_t offset, uint32_t value)
{
    q->msgram->write(q->msgram->context, offset, value);
}

/* The doorbell (Linux qmp_kick: one IPCC SEND_ID write per kick). The message RAM writes before it have completed:
 * the write callback returns only after the store (str + dsb sy). */
static void kick(Sm8750AossQmp *q)
{
    q->ipcc->write(q->ipcc->context, Sm8750AossQmpIpccSendId, Sm8750AossQmpIpccSignal);
    q->kicks++;
}

/* Polls offset until it reads `want` (or, with equal = 0, until it stops reading `want`), at most 1 s. */
static int wait_word(Sm8750AossQmp *q, uint32_t offset, uint32_t want, int equal)
{
    for (unsigned i = 0; i <= Sm8750AossQmpPolls; i++) {
        uint32_t value = rd(q, offset);
        if ((value == want) == (equal != 0))
            return Sm8750AossQmpOk;
        if (i == Sm8750AossQmpPolls) {
            q->reg = offset;
            q->value = value;
            q->expect = want;
            break;
        }
        q->msgram->delay(q->msgram->context, Sm8750AossQmpPollUs);
        q->polls++;
    }
    return Sm8750AossQmpTimeout;
}

static int io_ok(const Sm8750AossQmpIo *io)
{
    return io && io->read && io->write && io->delay;
}

void Sm8750AossQmpInit(Sm8750AossQmp *qmp, const Sm8750AossQmpIo *msgram, uint32_t msgram_bytes,
    const Sm8750AossQmpIo *ipcc)
{
    if (!qmp)
        return;
    *qmp = (Sm8750AossQmp){0};
    qmp->msgram = msgram;
    qmp->ipcc = ipcc;
    qmp->msgram_bytes = msgram_bytes;
}

/* Magic, version and our mailbox, read only. The mailbox must hold the length word and a full 64-byte message, be
 * word aligned and lie behind the descriptor inside the mapped RAM. */
static int descriptor(Sm8750AossQmp *q)
{
    static const struct {
        uint32_t offset, want;
    } fixed[] = {{Sm8750AossQmpDescMagic, Sm8750AossQmpMagic}, {Sm8750AossQmpDescVersion, Sm8750AossQmpVersion}};
    q->step = Sm8750AossQmpStepDescriptor;
    for (unsigned i = 0; i < 2; i++) {
        uint32_t value = rd(q, fixed[i].offset);
        if (value != fixed[i].want) {
            q->reg = fixed[i].offset;
            q->value = value;
            q->expect = fixed[i].want;
            return Sm8750AossQmpRefused;
        }
    }
    uint32_t offset = rd(q, Sm8750AossQmpMcoreMboxOffset), size = rd(q, Sm8750AossQmpMcoreMboxSize);
    if (!size || size < 4 + Sm8750AossQmpMessageBytes || (offset & 3) || offset < Sm8750AossQmpDescBytes ||
        offset > q->msgram_bytes || size > q->msgram_bytes - offset) {
        q->reg = Sm8750AossQmpMcoreMboxOffset;
        q->value = offset;
        q->expect = size;
        return Sm8750AossQmpRefused;
    }
    q->mbox_offset = offset;
    q->mbox_size = size;
    return Sm8750AossQmpOk;
}

static int already_open(const Sm8750AossQmp *q)
{
    return rd(q, Sm8750AossQmpMcoreLinkState) == Sm8750AossQmpStateUp &&
        rd(q, Sm8750AossQmpMcoreLinkStateAck) == Sm8750AossQmpStateUp &&
        rd(q, Sm8750AossQmpMcoreChState) == Sm8750AossQmpStateUp &&
        rd(q, Sm8750AossQmpUcoreChState) == Sm8750AossQmpStateUp &&
        rd(q, Sm8750AossQmpMcoreChStateAck) == Sm8750AossQmpStateUp;
}

int Sm8750AossQmpOpen(Sm8750AossQmp *qmp)
{
    if (!qmp || !io_ok(qmp->msgram) || !qmp->ipcc || !qmp->ipcc->write ||
        qmp->msgram_bytes < Sm8750AossQmpDescBytes + 4 + Sm8750AossQmpMessageBytes)
        return Sm8750AossQmpArgument;
    if (qmp->open)
        return Sm8750AossQmpOk;
    int rc = descriptor(qmp);
    if (rc)
        return rc;
    if (already_open(qmp)) {
        qmp->open = 1;
        qmp->reused = 1;
        qmp->step = Sm8750AossQmpStepOpen;
        return Sm8750AossQmpOk;
    }
    /* Ack the AOP's link state, bring ours up. */
    wr(qmp, Sm8750AossQmpUcoreLinkStateAck, rd(qmp, Sm8750AossQmpUcoreLinkState));
    wr(qmp, Sm8750AossQmpMcoreLinkState, Sm8750AossQmpStateUp);
    kick(qmp);
    qmp->step = Sm8750AossQmpStepLink;
    if ((rc = wait_word(qmp, Sm8750AossQmpMcoreLinkStateAck, Sm8750AossQmpStateUp, 1)))
        goto close_link;
    /* Our channel up; wait for the AOP's channel, ack it, wait for its ack of ours. */
    wr(qmp, Sm8750AossQmpMcoreChState, Sm8750AossQmpStateUp);
    kick(qmp);
    qmp->step = Sm8750AossQmpStepChannel;
    if ((rc = wait_word(qmp, Sm8750AossQmpUcoreChState, Sm8750AossQmpStateUp, 1)))
        goto close_channel;
    wr(qmp, Sm8750AossQmpUcoreChStateAck, Sm8750AossQmpStateUp);
    kick(qmp);
    qmp->step = Sm8750AossQmpStepChannelAck;
    if ((rc = wait_word(qmp, Sm8750AossQmpMcoreChStateAck, Sm8750AossQmpStateUp, 1)))
        goto close_channel;
    qmp->open = 1;
    qmp->step = Sm8750AossQmpStepOpen;
    return Sm8750AossQmpOk;

    /* As Linux qmp_open on a timeout: take our side down again so a later open starts clean. */
close_channel:
    wr(qmp, Sm8750AossQmpMcoreChState, (uint32_t)Sm8750AossQmpStateDown);
close_link:
    wr(qmp, Sm8750AossQmpMcoreLinkState, (uint32_t)Sm8750AossQmpStateDown);
    kick(qmp);
    return rc;
}

int Sm8750AossQmpSend(Sm8750AossQmp *qmp, const char *message)
{
    if (!qmp || !message)
        return Sm8750AossQmpArgument;
    uint32_t words[Sm8750AossQmpMessageBytes / 4] = {0};
    unsigned length = 0;
    while (message[length]) {
        if (length >= Sm8750AossQmpMessageBytes - 1)
            return Sm8750AossQmpTooLong;
        words[length / 4] |= (uint32_t)(uint8_t)message[length] << (8 * (length % 4));
        length++;
    }
    if (!length)
        return Sm8750AossQmpArgument;
    int rc = Sm8750AossQmpOpen(qmp);
    if (rc)
        return rc;
    const uint32_t box = qmp->mbox_offset;
    /* A message an earlier user left in the mailbox is given its second to be taken; it is never overwritten. */
    qmp->step = Sm8750AossQmpStepMailbox;
    if (wait_word(qmp, box, 0, 1))
        return Sm8750AossQmpBusy;
    /* The message RAM only takes 32-bit accesses: body first, then the length, read back, then the doorbell. */
    for (unsigned i = 0; i < Sm8750AossQmpMessageBytes / 4; i++)
        wr(qmp, box + 4 + 4 * i, words[i]);
    wr(qmp, box, Sm8750AossQmpMessageBytes);
    (void)rd(qmp, box);
    kick(qmp);
    qmp->step = Sm8750AossQmpStepSend;
    if ((rc = wait_word(qmp, box, 0, 1))) {
        wr(qmp, box, 0);                /* withdraw it, as Linux qmp_send does */
        return rc;
    }
    qmp->messages++;
    qmp->step = Sm8750AossQmpStepDone;
    return Sm8750AossQmpOk;
}

unsigned Sm8750AossQmpAcdMessage(int val, char out[Sm8750AossQmpMessageBytes])
{
    static const char text[] = "{class: gpu, res: acd, val: ?}";
    unsigned n = 0;
    for (; n < Sm8750AossQmpMessageBytes; n++)
        out[n] = 0;
    for (n = 0; text[n]; n++)
        out[n] = text[n] == '?' ? (val ? '1' : '0') : text[n];
    return n;
}

int Sm8750AossQmpSendAcdState(Sm8750AossQmp *qmp, int val)
{
    char message[Sm8750AossQmpMessageBytes];
    Sm8750AossQmpAcdMessage(val, message);
    return Sm8750AossQmpSend(qmp, message);
}

int Sm8750AossQmpSendAcd(const Sm8750AossQmpIo *msgram, const Sm8750AossQmpIo *ipcc, int val)
{
    Sm8750AossQmp qmp;
    Sm8750AossQmpInit(&qmp, msgram, Sm8750AossQmpMsgramBytes, ipcc);
    return Sm8750AossQmpSendAcdState(&qmp, val);
}

unsigned Sm8750AossQmpStatusCode(int rc)
{
    switch (rc) {
    case Sm8750AossQmpOk:
        return 1;
    case Sm8750AossQmpTimeout:
    case Sm8750AossQmpBusy:
        return 2;
    default:
        return 3;
    }
}
