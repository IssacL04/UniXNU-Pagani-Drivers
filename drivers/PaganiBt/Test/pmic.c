/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtPmic.c 对 SPMI 仲裁器 v7 模型：只写 PPID 0x58a（和睡眠时钟脚），owner、类型、子类型不对一个字节都不写，电平读回，配置已对时只写电平。 */
#include "../BtPmic.h"
#include "../PaganiBtBoard.h"
#include "check.h"

typedef struct {uint16_t ppid; uint8_t owner, stuck_source; uint8_t regs[256];} Periph;
static Periph periph[8];
static unsigned count;
static uint32_t obs_status, chan_status, version = 0x70000000;
static uint32_t obs_rdata, wdata;
static struct {uint16_t ppid; uint8_t reg, value;} writes[64];
static unsigned write_count;

static uint32_t core(void *c, uint32_t off)
{
    (void)c;
    if (off == 0) return version;
    if (off == 4) return count;
    if (off >= 0x2000) {
        const unsigned i = (off - 0x2000) / 4;
        return i < count ? (uint32_t)periph[i].ppid << 8 : 0;
    }
    return 0;
}
static uint32_t cnfg(void *c, uint32_t off) {(void)c; return periph[off / 4].owner;}
static void obs_write(void *c, uint32_t off, uint32_t value)
{
    (void)c;
    const unsigned apid = off / 0x20;
    CHECK(off % 0x20 == 0 && apid < count);
    CHECK((value >> 27) == 1);
    obs_rdata = periph[apid].regs[(value >> 4) & 0xff];
}
static uint32_t obs_read(void *c, uint32_t off)
{
    (void)c;
    return off % 0x20 == 8 ? obs_status : obs_rdata;
}
static uint32_t chan_read(void *c, unsigned apid, uint32_t off) {(void)c; (void)apid; (void)off; return chan_status;}
static void chan_write(void *c, unsigned apid, uint32_t off, uint32_t value)
{
    (void)c;
    CHECK(apid < count);
    if (off == 0x10) {
        wdata = value;
    } else if (off == 0) {
        const uint8_t reg = (uint8_t)(value >> 4);
        CHECK((value >> 27) == 0);
        CHECK(write_count < 64);
        writes[write_count].ppid = periph[apid].ppid;
        writes[write_count].reg = reg;
        writes[write_count++].value = (uint8_t)wdata;
        if (!(periph[apid].stuck_source && reg == 0x44)) periph[apid].regs[reg] = (uint8_t)wdata;
    }
}
static void delay(void *c, unsigned us) {(void)c; (void)us;}
static const PbSpmiIo kIo = {NULL, core, cnfg, obs_read, obs_write, chan_read, chan_write, delay};
static const PbSpmiIo kNoWritePath = {NULL, core, cnfg, obs_read, obs_write, NULL, NULL, delay};

static void reset_model(void)
{
    memset(periph, 0, sizeof periph);
    count = 0;
    obs_status = chan_status = 1;
    version = 0x70000000;
    write_count = 0;
}
static Periph *add(uint16_t ppid, uint8_t owner, uint8_t type, uint8_t subtype)
{
    Periph *p = &periph[count++];
    p->ppid = ppid;
    p->owner = owner;
    p->regs[0x04] = type;
    p->regs[0x05] = subtype;
    return p;
}

static void rails(void)
{
    reset_model();
    Periph *s7i = add(0x8ad, 0, 0x04, 0x20);
    s7i->regs[0x08] = 0x80;
    s7i->regs[0x46] = 0x80;
    s7i->regs[0x45] = 5;
    s7i->regs[0x40] = 0xc8;
    s7i->regs[0x41] = 0x04;                            /* 1224 mV */
    uint8_t st = 0, en = 0, mode = 0;
    uint16_t mv = 0;
    CHECK(pb_pmic_rail(&kIo, 0x8ad, &st, &en, &mode, &mv) == 0);
    CHECK(st == 0x80 && en == 0x80 && mode == 5 && mv == 1224);
    CHECK(pb_pmic_rail(&kIo, 0x5c3, &st, &en, &mode, &mv) == PbSpmiNotFound);
    obs_status = 2;
    CHECK(pb_pmic_rail(&kIo, 0x8ad, &st, &en, &mode, &mv) == PbSpmiIoError);
    obs_status = 0;
    CHECK(pb_pmic_rail(&kIo, 0x8ad, &st, &en, &mode, &mv) == PbSpmiTimeout);
    obs_status = 1;
    version = 0x60000000;
    CHECK(pb_pmic_rail(&kIo, 0x8ad, &st, &en, &mode, &mv) == PbSpmiLayout);
    CHECK(write_count == 0);                           /* 读轨道绝不写 */

    /* 同一个 PPID 两条映射：优先 apps 的 */
    reset_model();
    add(0x123, 2, 0, 0);
    add(0x123, 0, 0, 0);
    add(0x123, 3, 0, 0);
    int owner = -1;
    CHECK(pb_spmi_find(&kIo, 0x123, &owner) == 1 && owner == 0);
    CHECK(pb_spmi_find(&kIo, 0x1000, &owner) == PbSpmiArgument);
}

static void bt_en(void)
{
    PbGpioState s;
    /* 只读：一个字节不写 */
    reset_model();
    Periph *en = add(PB_BT_EN_PPID, 0, 0x10, 0x11);
    add(0x8ad, 0, 0x04, 0x20);
    en->regs[0x45] = 0x13;                             /* 开漏、强度 3：类型要改，强度留着 */
    CHECK(pb_pmic_bt_en(&kIo, -1, &s) == 0);
    CHECK(write_count == 0 && s.owner == 0 && s.regs[1] == 0x11);

    /* 没配置的脚：先定上下拉、电源、类型、电平、模式，最后才使能 */
    CHECK(pb_pmic_bt_en(&kIo, 0, &s) == 0);
    CHECK(write_count == 6 && s.written == 6);
    static const uint8_t want[6][2] = {{0x42, 5}, {0x41, PbBtEnVin}, {0x45, 0x03}, {0x44, 0}, {0x40, 1}, {0x46, 0x80}};
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(writes[i].ppid == PB_BT_EN_PPID);
        CHECK(writes[i].reg == want[i][0] && writes[i].value == want[i][1]);
    }
    /* 已配置：拉高只写一个寄存器 */
    write_count = 0;
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == 0);
    CHECK(write_count == 1 && writes[0].reg == 0x44 && writes[0].value == 0x80 && writes[0].ppid == PB_BT_EN_PPID);
    CHECK(s.after[5] == 0x80);
    write_count = 0;
    CHECK(pb_pmic_bt_en(&kIo, 0, &s) == 0);
    CHECK(write_count == 1 && writes[0].value == 0);

    /* 引导把它配成上下拉：判定没配好，重新写一遍 */
    en->regs[0x42] = 4;
    write_count = 0;
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == 0);
    CHECK(write_count == 6 && writes[3].reg == 0x44 && writes[3].value == 0x80);

    /* 读回不对：报错 */
    en->stuck_source = 1;
    en->regs[0x44] = 0;
    write_count = 0;
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiIoError);
    en->stuck_source = 0;

    /* 拒绝：别的 EE 占着、不是 GPIO、不是 LV/MV、没有写路径、仲裁器出错 */
    reset_model();
    add(PB_BT_EN_PPID, 2, 0x10, 0x11);
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiNotOwner);
    reset_model();
    add(PB_BT_EN_PPID, 0, 0x04, 0x11);
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiLayout);
    reset_model();
    add(PB_BT_EN_PPID, 0, 0x10, 0x09);
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiLayout);
    reset_model();
    add(PB_BT_EN_PPID, 0, 0x10, 0x14);
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiLayout);
    reset_model();
    add(PB_BT_EN_PPID, 0, 0x10, 0x11);
    CHECK(pb_pmic_bt_en(&kNoWritePath, 1, &s) == PbSpmiNotOwner);
    CHECK(write_count == 0);
    reset_model();
    add(0x123, 0, 0x10, 0x11);
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiNotFound);
    reset_model();
    add(PB_BT_EN_PPID, 0, 0x10, 0x11);
    chan_status = 4;
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiIoError);
    chan_status = 0;
    CHECK(pb_pmic_bt_en(&kIo, 1, &s) == PbSpmiTimeout);
    obs_status = 2;
    CHECK(pb_pmic_bt_en(&kIo, -1, &s) == PbSpmiIoError);
}

static void sleep_clock(void)
{
    PbGpioState s;
    reset_model();
    Periph *pin = add(PB_SLEEP_CLOCK_PPID, 0, 0x10, 0x11);
    /* 已经开着：不写 */
    pin->regs[0x46] = 0x80;
    pin->regs[0x40] = 1;
    pin->regs[0x44] = 2;
    CHECK(pb_pmic_sleep_clock(&kIo, 1, &s) == 1 && write_count == 0);
    /* 没开：只读时报 0，写时走 sheng 的五步 */
    pin->regs[0x46] = 0;
    CHECK(pb_pmic_sleep_clock(&kIo, 0, &s) == 0 && write_count == 0);
    CHECK(pb_pmic_sleep_clock(&kIo, 1, &s) == 2);
    CHECK(write_count == 5 && s.written == 5);
    static const uint8_t want[5][2] = {{0x42, 5}, {0x41, 0}, {0x44, 2}, {0x40, 1}, {0x46, 0x80}};
    for (unsigned i = 0; i < 5; ++i) CHECK(writes[i].ppid == PB_SLEEP_CLOCK_PPID && writes[i].reg == want[i][0] && writes[i].value == want[i][1]);
    /* 另一个 EE 占着 */
    reset_model();
    add(PB_SLEEP_CLOCK_PPID, 1, 0x10, 0x11);
    CHECK(pb_pmic_sleep_clock(&kIo, 1, &s) == PbSpmiNotOwner && write_count == 0);
    reset_model();
    add(PB_SLEEP_CLOCK_PPID, 0, 0x10, 0x20);
    CHECK(pb_pmic_sleep_clock(&kIo, 1, &s) == PbSpmiLayout && write_count == 0);
}

int main(void)
{
    CHECK(PB_BT_EN_PPID == 0x58a && PB_SLEEP_CLOCK_PPID == 0x0ba);
    rails();
    bt_en();
    sleep_clock();
    printf("PASS pmic: %u checks\n", checks);
    return 0;
}
