/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtStandIn.h"
#include "BtH4.h"
#include "BtTlv.h"
#include "BtUtil.h"

enum {
    kOpInquiryCancel = 0x0402, kOpLocalCommands = 0x1002, kOpLocalFeatures = 0x1003, kOpExtendedFeatures = 0x1004, kOpBufferSize = 0x1005,
    kCommandBytes = 64,
};
typedef struct {uint16_t opcode; uint8_t bytes; const uint8_t *data;} Answer;

/* Android 不发 Read_Local_Features，kFeatures 是 Read_Local_Extended_Features 第 0 页的内容，规范上两者相同。 */
static const uint8_t kVersion[] = {0x00, 0x0e, 0x00, 0x00, 0x0e, 0x1d, 0x00, 0x21, 0x7e};
static const uint8_t kFeatures[] = {0x00, 0xff, 0xfe, 0x0f, 0xfe, 0xd8, 0x3f, 0x5b, 0x87};
static const uint8_t kBuffer[] = {0x00, 0x00, 0x04, 0xf0, 0x08, 0x00, 0x04, 0x00};
static const uint8_t kLeBuffer[] = {0x00, 0xfb, 0x00, 0x10};
static const uint8_t kLeBuffer2[] = {0x00, 0xfb, 0x00, 0x10, 0xc8, 0x00, 0x04};
static const uint8_t kLeFeatures[] = {0x00, 0xff, 0xf9, 0x01, 0xff, 0xbe, 0x65, 0x00, 0x00};
static const uint8_t kLeStates[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00};
static const uint8_t kLeAcceptList[] = {0x00, 0x80};
static const uint8_t kLeResolving[] = {0x00, 0x20};
static const uint8_t kLeDefaultData[] = {0x00, 0x1b, 0x00, 0x48, 0x01};
static const uint8_t kLeMaxData[] = {0x00, 0xfb, 0x00, 0x90, 0x42, 0xfb, 0x00, 0x90, 0x42};
static const uint8_t kLePeriodicList[] = {0x00, 0x03};
#define ANSWER(op, table) {op, (uint8_t)sizeof table, table}
static const Answer kAnswers[] = {
    ANSWER(PbOpLocalVersion, kVersion), ANSWER(kOpLocalFeatures, kFeatures), ANSWER(kOpBufferSize, kBuffer), ANSWER(0x2002, kLeBuffer),
    ANSWER(0x2060, kLeBuffer2), ANSWER(0x2003, kLeFeatures), ANSWER(0x201c, kLeStates), ANSWER(0x200f, kLeAcceptList),
    ANSWER(0x202a, kLeResolving), ANSWER(0x2023, kLeDefaultData), ANSWER(0x202f, kLeMaxData), ANSWER(0x204a, kLePeriodicList),
};
/* 跟踪里的前 14 字节，后面 0xff 占位（BtStandIn.h）。 */
static const uint8_t kCommandsHead[14] = {0xff, 0xff, 0xfb, 0x03, 0xce, 0xff, 0xef, 0xff, 0xff, 0xff, 0xff, 0x1f, 0xfe, 0x0f};
/* 扩展特性：第 1、2 页也是跟踪里的。 */
static const uint8_t kPage1[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kPage2[8] = {0x5d, 0x03, 0, 0, 0, 0, 0, 0};
/* 走 Command Status 的 LE 命令：连接、更新、加密、PHY、ECDH、周期同步。 */
static const uint16_t kLeStatusOnly[] = {0x200d, 0x2013, 0x2016, 0x2019, 0x2025, 0x2026, 0x2032, 0x2043, 0x2044};

static uint32_t complete(uint8_t *out, uint16_t opcode, const uint8_t *params, uint32_t bytes)
{
    const uint8_t head[6] = {PbH4Evt, PbEvtCommandComplete, (uint8_t)(3 + bytes), 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
    pb_copy(out, head, sizeof head);
    pb_copy(out + sizeof head, params, bytes);
    return (uint32_t)sizeof head + bytes;
}

static int status_only(uint16_t opcode)
{
    if ((opcode >> 10) == 1) return opcode != kOpInquiryCancel;
    for (unsigned i = 0; i < sizeof kLeStatusOnly / sizeof kLeStatusOnly[0]; ++i) if (kLeStatusOnly[i] == opcode) return 1;
    return 0;
}

uint32_t pb_standin_reply(const uint8_t *p, uint32_t bytes, const uint8_t address[6], uint8_t *out)
{
    const uint16_t opcode = (uint16_t)(p[1] | (uint16_t)p[2] << 8);
    uint8_t params[1 + kCommandBytes];
    pb_zero(params, sizeof params);
    if (opcode == PbOpHostCompleted) return 0;  /* 规范上没有应答 */
    if (status_only(opcode)) {
        const uint8_t event[7] = {PbH4Evt, PbEvtCommandStatus, 4, PbStandInDisallowed, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
        pb_copy(out, event, sizeof event);
        return sizeof event;
    }
    for (unsigned i = 0; i < sizeof kAnswers / sizeof kAnswers[0]; ++i)
        if (kAnswers[i].opcode == opcode) return complete(out, opcode, kAnswers[i].data, kAnswers[i].bytes);
    if (opcode == PbOpBdAddr) {
        pb_copy(params + 1, address, 6);
        return complete(out, opcode, params, 7);
    }
    if (opcode == kOpLocalCommands) {
        for (uint32_t i = sizeof kCommandsHead; i < kCommandBytes; ++i) params[1 + i] = 0xff;
        pb_copy(params + 1, kCommandsHead, sizeof kCommandsHead);
        return complete(out, opcode, params, 1 + kCommandBytes);
    }
    if (opcode == kOpExtendedFeatures) {
        const uint8_t page = bytes >= 5 ? p[4] : 0;
        params[1] = page;
        params[2] = 2;                      /* 最大页号 */
        pb_copy(params + 3, page == 1 ? kPage1 : page == 2 ? kPage2 : kFeatures + 1, 8);
        return complete(out, opcode, params, 11);
    }
    return complete(out, opcode, params, 1);    /* 写类命令：状态 0 */
}
