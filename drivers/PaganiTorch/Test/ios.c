/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of TorchIos.c: the plan of the boot arguments, the last check before launchd, the level of a user client call. */
#include <string.h>
#include "check.h"
#include "../TorchIos.h"

int main(void)
{
    PgtIosPlan p, f;
    /* off: silent, nothing made */
    CHECK(pgt_ios_plan(0, 0, &p) == 0 && !p.create && !strcmp(p.why, "off"));
    CHECK(pgt_ios_plan(0, 1, &p) == 0 && !p.create);
    /* refused, logged */
    CHECK(pgt_ios_plan(2, 1, &p) == -1 && !p.create && strstr(p.why, "takes 1"));
    CHECK(pgt_ios_plan(1, 0, &p) == -1 && !p.create && strstr(p.why, "pagani-torch=1"));
    CHECK(pgt_ios_plan(1, 2, &p) == -1 && !p.create);      /* a dry run lights nothing, so no button */
    CHECK(pgt_ios_plan(1, 1, 0) == -1);
    /* on */
    CHECK(pgt_ios_plan(1, 1, &p) == 0 && p.create == 1 && !p.why[0]);
    /* the last check: nothing is made without the driver and the plug-in */
    CHECK(pgt_ios_final(&p, 1, 1, &f) == 0 && f.create == 1);
    CHECK(pgt_ios_final(&p, 0, 1, &f) == -1 && !f.create && strstr(f.why, "driver"));
    CHECK(pgt_ios_final(&p, 1, 0, &f) == -1 && !f.create && strstr(f.why, "plug-in"));
    CHECK(pgt_ios_final(&p, 0, 0, &f) == -1 && !f.create);
    CHECK(pgt_ios_final(0, 1, 1, &f) == -1 && pgt_ios_final(&p, 1, 1, 0) == -1);
    /* a plan that was off stays off, without a refusal */
    PgtIosPlan off;
    CHECK(pgt_ios_plan(0, 1, &off) == 0 && pgt_ios_final(&off, 0, 0, &f) == 0 && !f.create);
    /* the user client's scalar */
    CHECK(pgt_ios_level(0) == 0 && pgt_ios_level(1000) == 1000 && pgt_ios_level(1001) == -22 && pgt_ios_level(0x100000000ull) == -22);
    CHECK(pgt_ios_level(~0ull) == -22);
    /* the names the other side looks for */
    CHECK(!strcmp(PGT_IOS_SERVICE, "AppleH6CamIn") && !strcmp(PGT_IOS_PARENT "/" PGT_IOS_NODE, "IODeviceTree:/product/camera"));
    CHECK(!strcmp(PGT_IOS_FLASH, "flash") && !strcmp(PGT_IOS_PROCESS, "mediaserverd"));
    printf("PASS ios: %u checks\n", checks);
    return 0;
}
