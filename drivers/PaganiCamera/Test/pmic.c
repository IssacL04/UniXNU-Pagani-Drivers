/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CamPmic.c 对 SPMI 仲裁器模型：observer 只读；找不到 PPID 的路径；优先 EE 0 的通道。这个文件没有写路径。 */
#include "check.h"
#include "../CamPmic.h"

enum {Channels = 8};
static uint32_t gMap[Channels], gOwner[Channels], gVersion = 0x70000000u, gCount = Channels;
static uint8_t gRegs[Channels][256];
static unsigned gCurrent, gReg, gFailStatus, gStuck, gWrites;

static uint32_t f_core(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off == 0) return gVersion;
    if (off == 4) return gCount;
    if (off >= 0x2000 && off < 0x2000 + 4 * Channels) return gMap[(off - 0x2000) / 4];
    CHECK(0 && "core");
    return 0;
}
static uint32_t f_cnfg(void *ctx, uint32_t off) {(void)ctx; return gOwner[off / 4];}
static uint32_t f_obs_read(void *ctx, uint32_t off)
{
    (void)ctx;
    const unsigned channel = off / 0x20;
    CHECK(channel < Channels && channel == gCurrent);
    if ((off & 0x1f) == 0x08) return gStuck ? 0 : gFailStatus ? 2u : 1u;
    if ((off & 0x1f) == 0x18) return gRegs[channel][gReg];
    CHECK(0 && "obs read");
    return 0;
}
static void f_obs_write(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx;
    CHECK((off & 0x1f) == 0);
    CHECK((value >> 27) == 1);       /* 只有读命令 */
    gCurrent = off / 0x20;
    gReg = (value >> 4) & 0xff;
    gWrites++;
}
static void f_delay(void *ctx, unsigned us) {(void)ctx; (void)us;}
static const PcamSpmiIo kIo = {NULL, f_core, f_cnfg, f_obs_read, f_obs_write, f_delay};

int main(void)
{
    gMap[1] = 0xc43u << 8 | 1;
    gMap[3] = 0x8c1u << 8 | 1;
    gMap[4] = 0x8c1u << 8 | 1;
    gOwner[3] = 0;                  /* 同一 PPID 两个通道：要 apps 的，它在前面，后面的不能盖掉 */
    gOwner[4] = 2;
    gRegs[3][0x08] = 0x80;
    gRegs[3][0x46] = 0x80;
    gRegs[3][0x45] = 7;
    gRegs[3][0x40] = 0xe8;
    gRegs[3][0x41] = 0x03;
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    CHECK(pcam_pmic_rail(&kIo, 0x8c1, &status, &enable, &mode, &mv) == 0);
    CHECK(status == 0x80 && enable == 0x80 && mode == 7 && mv == 1000);
    CHECK(pcam_pmic_rail(&kIo, 0xc44, &status, &enable, &mode, &mv) == PcamSpmiNotFound);   /* 在另一条总线上 */
    CHECK(pcam_pmic_rail(&kIo, 0x1000, &status, &enable, &mode, &mv) == PcamSpmiArgument);
    CHECK(pcam_pmic_rail(NULL, 0xc44, &status, &enable, &mode, &mv) == PcamSpmiArgument);
    gVersion = 0x60000000u;
    CHECK(pcam_pmic_rail(&kIo, 0x8c1, &status, &enable, &mode, &mv) == PcamSpmiLayout);
    gVersion = 0x80000000u;
    CHECK(pcam_pmic_rail(&kIo, 0x8c1, &status, &enable, &mode, &mv) == PcamSpmiLayout);
    gVersion = 0x70000000u;
    gFailStatus = 1;
    CHECK(pcam_pmic_rail(&kIo, 0x8c1, &status, &enable, &mode, &mv) == PcamSpmiIoError);
    gFailStatus = 0;
    gStuck = 1;
    CHECK(pcam_pmic_rail(&kIo, 0x8c1, &status, &enable, &mode, &mv) == PcamSpmiTimeout);
    gStuck = 0;
    printf("PASS pmic: %u checks\n", checks);
    return 0;
}
