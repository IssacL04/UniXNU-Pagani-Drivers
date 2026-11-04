/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* kern.pagani_torch: the torch level, 0 (off) to 1000. Reading gives the level now; writing (root) is pagani_torch_set. The node exists only
 * while pagani-torch=1: it is registered by hand (SYSCTL_OID_MANUAL, sysctl_register_oid, after lockdown; kern is an extensible node), so
 * a kernel without the boot argument has no such name. Compiled with the kernel's BSD flags, which give the real struct sysctl_oid. */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "PaganiTorch.h"

/* The driver's status to the errno of a sysctl. */
static int errno_of(int rc)
{
    switch (rc) {
    case 0: return 0;
    case -22: return EINVAL;
    case -19: return ENODEV;
    case -110: return ETIMEDOUT;
    default: return EIO;
    }
}

static int sysctl_pagani_torch SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
    const int now = pagani_torch_level();
    int value = now < 0 ? 0 : now, changed = 0;
    const int error = sysctl_io_number(req, value, sizeof value, &value, &changed);
    if (error || !changed) return error;
    if (value < 0) return EINVAL;
    return errno_of(pagani_torch_request((uint32_t)value, "sysctl"));
}

SYSCTL_OID_MANUAL(_kern, OID_AUTO, pagani_torch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_pagani_torch, "I",
                  "torch level, 0 off to 1000");

void pagani_torch_sysctl_register(void);
void pagani_torch_sysctl_register(void)
{
    sysctl_register_oid(&sysctl__kern_pagani_torch);
}
