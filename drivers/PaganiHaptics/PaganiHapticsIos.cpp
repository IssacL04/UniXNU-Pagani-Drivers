// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The iOS haptics switch (HapticsIos.h): with pagani-haptics-ios=1 or 2 (and pagani-haptics=1, pagani-audio=1) the kernel adds the node
// IODeviceTree:/product/haptics before launchd starts, as the device tree of an iPhone carries it, so libMobileGestalt answers
// DeviceSupportsHaptics (stage A and B) and DeviceSupportsClosedLoopHaptics (stage B). pagani_haptics_start calls the first half first,
// synchronously (Stage7Probe runs it during the kernel's start, in bsd_autoconf): the plan and its refusals; nothing else of PaganiHaptics
// depends on it. load_init_program calls the second half after the overlay mounts, on the thread that goes on to exec launchd: the node.
// It is made the way IODeviceTreeAlloc makes the others (IODeviceTreeSupport.cpp MakeReferenceTable: an IOService with its properties as
// OSData, "name" among them, the registry name set from it) and attached to /product in the IODeviceTree plane only; no nub, so no driver
// matches it. Without pagani-haptics-ios nothing here runs and nothing is logged; a refusal or failure is logged and leaves the tree as it
// was. Never panics.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "HapticsIos.h"
#include "HapticsText.h"
void boot_progress_trace(const char *line);   // BootProgress's, or PaganiHaptics.cpp's weak default
volatile int32_t pagani_haptics_ios_status[4];
}

namespace {
volatile uint32_t gIosDone, gIosCommitted;
PghIosPlan gIosPlan;

void say(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

bool putU32(IORegistryEntry *node, const char *key, uint32_t value)
{
    OSData *data = OSData::withBytes(&value, sizeof value);
    if (!data) return false;
    const bool ok = node->setProperty(key, data);
    data->release();
    return ok;
}

void fail(int32_t code, const char *what)
{
    pagani_haptics_ios_status[0] = code;
    char line[160];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, "[haptics] ios node not created: ");
    pgh_text_str(&t, what);
    say(line);
}
}  // namespace

extern "C" void pagani_haptics_ios_publish(unsigned ios, unsigned haptics)
{
    if (!ios || __atomic_exchange_n(&gIosDone, 1u, __ATOMIC_ACQ_REL)) return;
    unsigned audio = 0;
    if (!PE_parse_boot_argn("pagani-audio", &audio, sizeof audio)) audio = 0;
    if (pgh_ios_plan(ios, haptics, audio, &gIosPlan) || !gIosPlan.create) return fail(-1, gIosPlan.why);
    pagani_haptics_ios_status[0] = 1;
}

extern "C" void pagani_haptics_ios_commit(int audio_visible, int tunings_visible)
{
    if (!gIosPlan.create || __atomic_exchange_n(&gIosCommitted, 1u, __ATOMIC_ACQ_REL)) return;
    PghIosPlan plan;
    if (pgh_ios_final(&gIosPlan, pagani_audio_actuator_enabled(), audio_visible, tunings_visible, &plan) || !plan.create)
        return fail(-6, plan.why);
    IORegistryEntry *product = IORegistryEntry::fromPath(PGH_IOS_PARENT, gIODTPlane);
    if (!product) return fail(-2, "no /product in the device tree");
    IORegistryEntry *existing = product->childFromPath(PGH_IOS_NODE, gIODTPlane);
    if (existing) {
        existing->release();
        product->release();
        return fail(-3, "/product/haptics is already there (left as it is)");
    }
    IOService *node = new IOService;
    if (node && !node->init()) {
        node->release();
        node = nullptr;
    }
    if (!node) {
        product->release();
        return fail(-4, "no memory");
    }
    static const char name[] = PGH_IOS_NODE;
    OSData *nameData = OSData::withBytes(name, sizeof name);
    bool ok = nameData && node->setProperty("name", nameData);
    if (nameData) nameData->release();
    node->setName(PGH_IOS_NODE);
    ok = ok && putU32(node, PGH_IOS_THIRD_PARTY, 1);
    if (plan.closed_loop) ok = ok && putU32(node, PGH_IOS_CLOSED_LOOP, 1);
    ok = ok && node->attachToParent(product, gIODTPlane);
    node->release();      // the plane keeps it once attached
    product->release();
    if (!ok) return fail(-5, "a property or the attach to /product failed");
    pagani_haptics_ios_status[1] = plan.closed_loop;
    pagani_haptics_ios_status[2] = plan.third_party;
    pagani_haptics_ios_status[3] = plan.closed_loop ? 3 : 2;
    pagani_haptics_ios_status[0] = plan.stage;
    char line[160];
    PghText t;
    pgh_text_start(&t, line, sizeof line);
    pgh_text_str(&t, plan.stage == 'B' ? "[haptics] ios stage B: /product/haptics created (supports-3rd-party-haptics=1 closed-loop=1)"
                                       : "[haptics] ios stage A: /product/haptics created (supports-3rd-party-haptics=1, no closed-loop)");
    if (plan.why[0]) {
        pgh_text_str(&t, ": ");
        pgh_text_str(&t, plan.why);
    }
    say(line);
}
