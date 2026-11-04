/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The iOS haptics switch (boot argument pagani-haptics-ios): which IODeviceTree node PaganiHapticsIos.cpp creates, decided here in
 * portable C so the host tests run the same rules.
 *
 * libMobileGestalt (21H461) answers the haptics questions from the device tree, comparing names by their obfuscated form
 * base64(md5("MGCopyAnswer" + name)) truncated to 22 characters, after a first-character check (MGIOKitSupport.c, -[MGIOKitHelper
 * copyDeviceTreeStructureNext:withFirstChar:], sub_1A020CA7C):
 *   DeviceSupportsHaptics (and VibratorCapability)  any node of the IODeviceTree plane, iterated recursively from the root
 *                         (IORegistryCreateIterator(kIODeviceTreePlane, kIORegistryIterateRecursively)), whose IORegistryEntryGetName is
 *                         "haptics" ('h', 6S9CvPHPtzHQqKudHSfsag)
 *   deviceSupports3RdPartyHaptics  IODeviceTree:/product/haptics has a property "supports-3rd-party-haptics" ('s',
 *                         OtnrTIhWBtyPo45BcApMTw), found among its IORegistryEntryPropertyKeys
 *   DeviceSupportsClosedLoopHaptics  IODeviceTree:/product/haptics "closed-loop" ('c', XKpK5v7PGzbe7igjloNBpQ), a number or 4 bytes,
 *                         not 0: the master switch of CoreHaptics, UIKit's level-2 feedback (keyboard clicks) and the unified vibe engine
 * Stage A (pagani-haptics-ios=1): the node with supports-3rd-party-haptics only: vibrations through the legacy vibe engine, nothing else
 * changes. Stage B (=2): closed-loop as well.
 * The node is planned from the boot arguments in pagani_haptics_start and made just before launchd starts, by
 * pagani_haptics_ios_commit from load_init_program (phone-haptics-overlays.patch), the first moment the overlays can be looked at:
 *  - no node without the audio service offering the actuator and the plug-in visible on the root: with a node and no "Actuator" device
 *    VirtualAudio waits 8 s for one at every boot and MediaExperience's start can lose its routing listener (route-re R4);
 *  - no closed-loop without /Library/Audio/Tunings/J120/Haptics/hapticengineconfig.plist visible: the haptic server fails every client
 *    with -43 without it, so stage B without the tunings overlay falls back to stage A;
 *  - a kernel without the commit (or an image without sheng-gpu-addon=1, which mounts nothing) makes no node: MobileGestalt answers no. */
#ifndef PAGANI_HAPTICS_IOS_H
#define PAGANI_HAPTICS_IOS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t create;            /* 1: create /product/haptics */
    uint8_t third_party;       /* with "supports-3rd-party-haptics" = 1 */
    uint8_t closed_loop;       /* with "closed-loop" = 1 */
    uint8_t stage;             /* 'A' or 'B' when created, 0 otherwise */
    const char *why;           /* why not (create 0): "off", or the refusal for the boot log; with create 1 why stage B fell back, else "" */
} PghIosPlan;

/* The plan for the boot arguments pagani-haptics-ios (ios), pagani-haptics (haptics) and pagani-audio (audio). 0: create (stage A or B)
 * or off (ios 0); -1: refused, the reason in plan->why. */
int pgh_ios_plan(unsigned ios, unsigned haptics, unsigned audio, PghIosPlan *plan);
/* The plan as it stands just before launchd: actuator, the audio service offers the actuator; audio_visible, the plug-in is on the root
 * (its overlay's stage 5); tunings_visible, the haptic server's preset is. Writes *final (a copy of *plan when nothing changes it);
 * 0, with create 0 only when *plan had it so; -1: refused, the reason in final->why. */
int pgh_ios_final(const PghIosPlan *plan, int actuator, int audio_visible, int tunings_visible, PghIosPlan *final);
/* The audio service's answer (PaganiAudio.cpp); a weak default in HapticsIos.c answers no in a kernel without it. */
int pagani_audio_actuator_enabled(void);

/* Names and values PaganiHapticsIos.cpp uses (the host test hashes them as libMobileGestalt does). */
#define PGH_IOS_PARENT "/product"
#define PGH_IOS_NODE "haptics"
#define PGH_IOS_THIRD_PARTY "supports-3rd-party-haptics"
#define PGH_IOS_CLOSED_LOOP "closed-loop"

/* The kernel side (PaganiHapticsIos.cpp). publish: called first thing in pagani_haptics_start with pagani-haptics-ios and pagani-haptics
 * (mode); reads pagani-audio itself; plans the node, at most once per boot, and logs a refusal. commit: called from load_init_program after
 * the overlay mounts with what it found; creates the node, at most once per boot. Both log "[haptics] ios ..." and fill
 * pagani_haptics_ios_status:
 * [0] 0 off, 1 planned, 'A'/'B' created, -1 refused, -2 no /product, -3 a "haptics" node already there (left alone), -4 no memory,
 *     -5 attach failed, -6 refused just before launchd (no actuator or no plug-in);
 * [1] closed-loop written; [2] supports-3rd-party-haptics written; [3] the node's property count. */
void pagani_haptics_ios_publish(unsigned ios, unsigned haptics);
void pagani_haptics_ios_commit(int audio_visible, int tunings_visible);
extern volatile int32_t pagani_haptics_ios_status[4];

#ifdef __cplusplus
}
#endif
#endif
