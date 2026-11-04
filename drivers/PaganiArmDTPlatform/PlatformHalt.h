/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What ArmDTPlatform::haltRestart does with a request of PEHaltRestart. Without a PE_halt_restart hook the inherited haltRestart returns
 * -1: halt_all_cpus spins, the 30 s timer of PEHaltRestartInternal panics "Halt/Restart Timed Out" and pagani-panic-reset brings Android
 * back about 50 s after iOS said it powers off.
 *
 * pagani-halt=1 gives that a meaning: a restart takes BootProgress's reset (ramoops_log_restart: its records, watchdog handling and
 * PSCI choice), a power off takes PSCI SYSTEM_OFF on the same path (ramoops_log_power_off). pagani-halt=2 restarts for a power off too:
 * the phone comes back to Android and the DRAM log stays readable, which a real power off cannot give. Anything else, a missing
 * boot-arg included, keeps the inherited behaviour for every request; so does every other request (kPEHangCPU, kPEPanicSync, the
 * panic notifications) in every mode.
 *
 * Portable and free of IOKit, so that the host test covers this table. */
#ifndef PAGANI_PLATFORM_HALT_H
#define PAGANI_PLATFORM_HALT_H

/* IOKit/IOPlatformExpert.h's request numbers; ArmDTPlatform.cpp checks them at compile time. */
enum {
    PHALT_TYPE_HALT = 0,            /* kPEHaltCPU */
    PHALT_TYPE_RESTART = 1,         /* kPERestartCPU */
    PHALT_TYPE_UPS_HALT = 3,        /* kPEUPSDelayHaltCPU: the inherited haltRestart turns it into kPEHaltCPU */
    PHALT_TYPE_PANIC_RESTART = 4    /* kPEPanicRestartCPU; PEHaltRestartInternal maps kPEPanicRestartCPUNoCallouts to it */
};

enum { PHALT_INHERIT, PHALT_RESTART, PHALT_POWER_OFF };

/* The action for request `type` under boot-arg value `mode`; *reason names it for the reset record (untouched for PHALT_INHERIT). */
static inline int pagani_halt_action(unsigned mode, unsigned type, const char **reason)
{
    if (mode != 1 && mode != 2) return PHALT_INHERIT;
    switch (type) {
    case PHALT_TYPE_RESTART:
        *reason = "restart";
        return PHALT_RESTART;
    case PHALT_TYPE_PANIC_RESTART:
        *reason = "panic restart";
        return PHALT_RESTART;
    case PHALT_TYPE_HALT:
    case PHALT_TYPE_UPS_HALT:
        *reason = mode == 2 ? "power off as restart" : "power off";
        return mode == 2 ? PHALT_RESTART : PHALT_POWER_OFF;
    default:
        return PHALT_INHERIT;
    }
}

#endif
