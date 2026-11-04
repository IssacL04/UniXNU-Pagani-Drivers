/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CamCci.h */
#include "CamCci.h"
#include <stddef.h>

static uint32_t rd(const PcamCciIo *io, uint32_t off) {return io->read(io->ctx, off);}
static void wr(const PcamCciIo *io, uint32_t off, uint32_t value) {io->write(io->ctx, off, value);}

static void ack(const PcamCciIo *io, uint32_t status)
{
    wr(io, PcamCciIrqClear0, status);
    wr(io, PcamCciIrqGlobalClear, 1);
}

/* 等 wanted 里的位，同时看错误位。seen 累积：一次读可能分开给出几位。 */
static int wait_status(const PcamCciIo *io, uint32_t wanted, uint32_t *seen)
{
    uint32_t have = 0;
    for (unsigned n = 0; n < PcamCciPolls; ++n) {
        const uint32_t status = rd(io, PcamCciIrqStatus0);
        if (status) {
            ack(io, status);
            have |= status;
        }
        if (have & PCAM_CCI_IRQ_ERROR) {
            if (seen) *seen = have;
            return PcamCciNack;
        }
        if ((have & wanted) == wanted) {
            if (seen) *seen = have;
            return PcamCciOk;
        }
        io->delay_us(io->ctx, PcamCciPollUs);
    }
    if (seen) *seen = have;
    return PcamCciTimeout;
}

static int reset(const PcamCciIo *io)
{
    wr(io, PcamCciReset, PcamCciResetAll);
    return wait_status(io, PCAM_CCI_IRQ_RESET_DONE, NULL);
}

int pcam_cci_init(const PcamCciIo *io, const PcamCciTiming *t)
{
    if (!io || !io->read || !io->write || !io->delay_us || !t) return PcamCciArgument;
    wr(io, PcamCciIrqMask0, PCAM_CCI_IRQ_ENABLE);
    ack(io, 0xffffffffu);
    const int rc = reset(io);
    if (rc) return rc;
    wr(io, PcamCciSclCtl, (uint32_t)t->thigh << 16 | t->tlow);
    wr(io, PcamCciSdaCtl0, (uint32_t)t->tsu_sto << 16 | t->tsu_sta);
    wr(io, PcamCciSdaCtl1, (uint32_t)t->thd_dat << 16 | t->thd_sta);
    wr(io, PcamCciSdaCtl2, t->tbuf);
    wr(io, PcamCciMiscCtl, (uint32_t)t->scl_stretch << 8 | (uint32_t)t->trdhld << 4 | t->tsp);
    return PcamCciOk;
}

/* 出错后：让 master 停下，全局复位，重写时序。HALT 没应答也接着复位。 */
static void recover(const PcamCciIo *io, const PcamCciTiming *t)
{
    wr(io, PcamCciHalt, PcamCciHaltM0);
    uint32_t have = 0;
    for (unsigned n = 0; n < PcamCciPolls && !(have & PCAM_CCI_IRQ_HALT_ACK); ++n) {
        const uint32_t status = rd(io, PcamCciIrqStatus0);
        if (status) {
            ack(io, status);
            have |= status;
        }
        if (!(have & PCAM_CCI_IRQ_HALT_ACK)) io->delay_us(io->ctx, PcamCciPollUs);
    }
    (void)pcam_cci_init(io, t);
}

static int run(const PcamCciIo *io, const PcamCciTiming *t, uint32_t wanted)
{
    wr(io, PcamCciExecWords, rd(io, PcamCciCurWords));
    wr(io, PcamCciQueueStart, 1u);          /* bit (master × 2 + queue) */
    const int rc = wait_status(io, wanted, NULL);
    if (rc) recover(io, t);
    return rc;
}

/* 上一轮没跑完的话队列不是空的：不往里追加。 */
static int queue_empty(const PcamCciIo *io)
{
    return rd(io, PcamCciCurWords) == 0;
}

static uint32_t set_param(uint8_t addr)
{
    return PcamCciCmdSetParam | (uint32_t)(addr & 0x7fu) << 4 | (uint32_t)PcamCciRetries << PcamCciRetryShift;
}

int pcam_cci_write16(const PcamCciIo *io, const PcamCciTiming *t, uint8_t addr, uint16_t reg, uint16_t value)
{
    if (!io || !t) return PcamCciArgument;
    if (!queue_empty(io)) return PcamCciBusy;
    wr(io, PcamCciLoadData, set_param(addr));
    wr(io, PcamCciLoadData, PcamCciCmdWrite | 4u << 4 | (uint32_t)(reg >> 8) << 8 | (uint32_t)(reg & 0xffu) << 16 | (uint32_t)(value >> 8) << 24);
    wr(io, PcamCciLoadData, value & 0xffu);
    wr(io, PcamCciLoadData, PcamCciCmdReport | PcamCciReportIrq);
    return run(io, t, PCAM_CCI_IRQ_REPORT);
}

int pcam_cci_read16(const PcamCciIo *io, const PcamCciTiming *t, uint8_t addr, uint16_t reg, uint16_t *value)
{
    if (!io || !t || !value) return PcamCciArgument;
    if (!queue_empty(io)) return PcamCciBusy;
    wr(io, PcamCciLoadData, set_param(addr));
    wr(io, PcamCciLoadData, PcamCciCmdLock);
    wr(io, PcamCciLoadData, PcamCciCmdWriteNoStop | 2u << 4 | (uint32_t)(reg >> 8) << 8 | (uint32_t)(reg & 0xffu) << 16);
    wr(io, PcamCciLoadData, PcamCciCmdRead | 2u << 4);
    wr(io, PcamCciLoadData, PcamCciCmdUnlock);
    const int rc = run(io, t, PCAM_CCI_IRQ_RD_DONE);
    if (rc) return rc;
    /* 2 字节读回来是 1 个字：第 0 字节不是数据，第 1、2 字节是高低位 */
    if ((rd(io, PcamCciReadLevel) & 0xffu) != 1u) {
        recover(io, t);
        return PcamCciData;
    }
    const uint32_t word = rd(io, PcamCciReadData);
    *value = (uint16_t)(((word >> 8) & 0xffu) << 8 | ((word >> 16) & 0xffu));
    return PcamCciOk;
}
