/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CamCci.c 单独对一个小模型：逐字检查队列内容、启动位、错误恢复。比 bringup 的模型更苛刻：这里每个字都比。 */
#include "check.h"
#include "../CamCci.h"

enum {MaxWords = 64};
static uint32_t gLoad[MaxWords], gStatus, gLevel = 0, gData, gCurrent, gExec, gStart, gReset, gHalt, gMask, gCleared, gGlobalClears;
static unsigned gLoads, gPolls, gDeliver;   /* gDeliver：QUEUE_START 后要的结果：0 不来，1 成功，2 NACK */
static uint32_t gTiming[5];

static uint32_t f_read(void *ctx, uint32_t off)
{
    (void)ctx;
    switch (off) {
    case PcamCciCurWords: return gCurrent;
    case PcamCciIrqStatus0: ++gPolls; return gStatus;
    case PcamCciReadLevel: return gLevel;
    case PcamCciReadData: return gData;
    default: CHECK(0 && "unexpected read"); return 0;
    }
}

static void f_write(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx;
    switch (off) {
    case PcamCciLoadData:
        CHECK(gLoads < MaxWords);
        gLoad[gLoads++] = value;
        gCurrent++;
        return;
    case PcamCciExecWords: gExec = value; return;
    case PcamCciQueueStart:
        CHECK(gExec == gCurrent && gExec == gLoads);
        CHECK(value == 1);
        gStart++;
        if (gDeliver == 1) gStatus |= (gLoad[gLoads - 1] & 0xf) == PcamCciCmdReport ? PCAM_CCI_IRQ_REPORT : PCAM_CCI_IRQ_RD_DONE;
        if (gDeliver == 2) gStatus |= 0x08000000u;
        return;
    case PcamCciIrqMask0: gMask = value; return;
    case PcamCciIrqClear0: gCleared |= value; gStatus &= ~value; return;
    case PcamCciIrqGlobalClear: CHECK(value == 1); gGlobalClears++; return;
    case PcamCciReset: CHECK(value == PcamCciResetAll); gReset++; gCurrent = 0; gLoads = 0; gStatus |= PCAM_CCI_IRQ_RESET_DONE; return;
    case PcamCciHalt: CHECK(value == PcamCciHaltM0); gHalt++; gStatus |= PCAM_CCI_IRQ_HALT_ACK; return;
    case PcamCciSclCtl: gTiming[0] = value; return;
    case PcamCciSdaCtl0: gTiming[1] = value; return;
    case PcamCciSdaCtl1: gTiming[2] = value; return;
    case PcamCciSdaCtl2: gTiming[3] = value; return;
    case PcamCciMiscCtl: gTiming[4] = value; return;
    default: CHECK(0 && "unexpected write"); return;
    }
}
static void f_delay(void *ctx, unsigned us) {(void)ctx; (void)us;}
static const PcamCciIo kIo = {NULL, f_read, f_write, f_delay};

static void clear_load(void)
{
    memset(gLoad, 0, sizeof gLoad);
    gLoads = 0;
    gCurrent = 0;
}

int main(void)
{
    /* 这几个数模型和实现共用，这里写死：i2c-qcom-cci.c 的事实 */
    CHECK(PcamCciResetAll == 0x0f73f3f7 && PcamCciHalt == 0x34 && PcamCciLoadData == 0x310 && PcamCciCurWords == 0x304 && PcamCciExecWords == 0x300);
    CHECK(PcamCciIrqStatus0 == 0xc0c && PcamCciIrqClear0 == 0xc08 && PcamCciIrqMask0 == 0xc04 && PcamCciIrqGlobalClear == 0xc00);
    CHECK(PcamCciReadData == 0x118 && PcamCciReadLevel == 0x11c && PcamCciSclCtl == 0x100 && PcamCciMiscCtl == 0x110);
    CHECK(pcam_cci_init(NULL, &kPcamCciFast) == PcamCciArgument);
    CHECK(pcam_cci_init(&kIo, NULL) == PcamCciArgument);
    CHECK(pcam_cci_init(&kIo, &kPcamCciFast) == 0);
    CHECK(gReset == 1 && (gMask & PCAM_CCI_IRQ_ERROR) == PCAM_CCI_IRQ_ERROR && (gMask & PCAM_CCI_IRQ_RD_DONE) && (gMask & PCAM_CCI_IRQ_RESET_DONE) && (gMask & PCAM_CCI_IRQ_HALT_ACK));
    CHECK(gTiming[0] == 0x00260038u && gTiming[1] == 0x00280028u && gTiming[2] == 0x00160023u && gTiming[3] == 0x3e && gTiming[4] == 0x63);
    CHECK(gStatus == 0);

    /* 写 0xfcfc = 0x4000，地址 0x2d */
    clear_load();
    gDeliver = 1;
    CHECK(pcam_cci_write16(&kIo, &kPcamCciFast, 0x2d, 0xfcfc, 0x4000) == 0);
    CHECK(gLoads == 4);
    CHECK(gLoad[0] == (1u | 0x2du << 4 | 3u << 16));
    CHECK(gLoad[1] == (9u | 4u << 4 | 0xfcu << 8 | 0xfcu << 16 | 0x40u << 24));
    CHECK(gLoad[2] == 0x00);
    CHECK(gLoad[3] == (8u | 1u << 8));
    CHECK(gStatus == 0 && gStart == 1);

    /* 读 0x0000，回 0x38e5 */
    clear_load();
    gLevel = 1;
    gData = 0x00e5385au;
    uint16_t value = 0;
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0x0000, &value) == 0);
    CHECK(value == 0x38e5);
    CHECK(gLoads == 5);
    CHECK(gLoad[1] == 6u);
    CHECK(gLoad[2] == (11u | 2u << 4));
    CHECK(gLoad[3] == (10u | 2u << 4));
    CHECK(gLoad[4] == 7u);
    /* 高低字节不交换地址 */
    clear_load();
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x10, 0x1234, &value) == 0);
    CHECK(gLoad[0] == (1u | 0x10u << 4 | 3u << 16));
    CHECK(gLoad[2] == (11u | 2u << 4 | 0x12u << 8 | 0x34u << 16));

    /* 只取 7 位地址 */
    clear_load();
    CHECK(pcam_cci_write16(&kIo, &kPcamCciFast, 0xad, 1, 2) == 0);
    CHECK(((gLoad[0] >> 4) & 0xff) == 0x2d);

    /* 读回来的字数不对 */
    clear_load();
    gLevel = 3;
    const unsigned resets = gReset;
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0, &value) == PcamCciData);
    CHECK(gReset == resets + 1);
    gLevel = 1;

    /* NACK：HALT、复位、时序重写 */
    clear_load();
    gDeliver = 2;
    const unsigned halts = gHalt;
    memset(gTiming, 0, sizeof gTiming);
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0, &value) == PcamCciNack);
    CHECK(gHalt == halts + 1 && gReset == resets + 2);
    CHECK(gTiming[0] == 0x00260038u);
    CHECK(gCurrent == 0);
    /* 之后还能用 */
    gDeliver = 1;
    clear_load();
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0, &value) == 0);

    /* 超时也恢复，轮询有界 */
    clear_load();
    gDeliver = 0;
    gPolls = 0;
    CHECK(pcam_cci_write16(&kIo, &kPcamCciFast, 0x2d, 0, 0) == PcamCciTimeout);
    CHECK(gPolls >= PcamCciPolls);
    gDeliver = 1;
    CHECK(gCurrent == 0);

    /* 队列不空：不追加 */
    clear_load();
    gCurrent = 2;
    const unsigned starts = gStart;
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0, &value) == PcamCciBusy);
    CHECK(pcam_cci_write16(&kIo, &kPcamCciFast, 0x2d, 0, 0) == PcamCciBusy);
    CHECK(gLoads == 0 && gStart == starts);
    gCurrent = 0;
    CHECK(pcam_cci_read16(&kIo, &kPcamCciFast, 0x2d, 0, NULL) == PcamCciArgument);
    printf("PASS cci: %u checks\n", checks);
    return 0;
}
