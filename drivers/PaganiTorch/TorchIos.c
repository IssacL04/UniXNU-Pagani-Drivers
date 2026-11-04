/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The rules of the iOS switch; see TorchIos.h. Freestanding (the kernel archive and the host tests compile it). */
#include "TorchIos.h"

enum { LEVEL_MAX = 1000 };

static int refuse(PgtIosPlan *plan, const char *why)
{
    plan->create = 0;
    plan->why = why;
    return -1;
}

int pgt_ios_plan(unsigned ios, unsigned torch, PgtIosPlan *plan)
{
    if (!plan) return -1;
    if (!ios) {
        refuse(plan, "off");
        return 0;
    }
    if (ios != 1) return refuse(plan, "pagani-torch-ios takes 1");
    if (torch != 1) return refuse(plan, "needs pagani-torch=1 (the driver running)");
    plan->create = 1;
    plan->why = "";
    return 0;
}

int pgt_ios_final(const PgtIosPlan *plan, int driver_ready, int plugin_visible, PgtIosPlan *final)
{
    if (!plan || !final) return -1;
    *final = *plan;
    if (!plan->create) return 0;
    if (!driver_ready) return refuse(final, "the torch driver is not running");
    if (!plugin_visible) return refuse(final, "the plug-in is not on the root");
    return 0;
}

int pgt_ios_level(uint64_t scalar) {return scalar <= LEVEL_MAX ? (int)scalar : -22;}
