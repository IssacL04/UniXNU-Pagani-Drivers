/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The iOS haptics switch's rules; see HapticsIos.h. Freestanding (the kernel archive and the host tests compile it). */
#include "HapticsIos.h"

static int refuse(PghIosPlan *plan, const char *why)
{
    plan->create = 0;
    plan->third_party = plan->closed_loop = plan->stage = 0;
    plan->why = why;
    return -1;
}

int pgh_ios_plan(unsigned ios, unsigned haptics, unsigned audio, PghIosPlan *plan)
{
    if (!plan) return -1;
    if (!ios) {
        refuse(plan, "off");
        return 0;
    }
    if (ios > 2) return refuse(plan, "pagani-haptics-ios takes 1 (stage A) or 2 (stage B)");
    if (haptics != 1) return refuse(plan, "needs pagani-haptics=1 (the vibrator running)");
    if (!audio) return refuse(plan, "needs pagani-audio=1 (the Actuator device of the audio plug-in)");
    plan->create = 1;
    plan->third_party = 1;
    plan->closed_loop = ios == 2;
    plan->stage = ios == 2 ? 'B' : 'A';
    plan->why = "";
    return 0;
}

int pgh_ios_final(const PghIosPlan *plan, int actuator, int audio_visible, int tunings_visible, PghIosPlan *final)
{
    if (!plan || !final) return -1;
    *final = *plan;
    if (!plan->create) return 0;
    if (!actuator) return refuse(final, "the audio service does not offer the actuator");
    if (!audio_visible) return refuse(final, "the audio plug-in is not on the root");
    if (plan->closed_loop && !tunings_visible) {
        final->closed_loop = 0;
        final->stage = 'A';
        final->why = "no closed-loop: hapticengineconfig.plist is not visible";
    }
    return 0;
}

/* PaganiAudio.cpp's replaces it wherever the audio service is linked (the kernel links statically and refuses undefined weak references). */
__attribute__((weak)) int pagani_audio_actuator_enabled(void) {return 0;}
