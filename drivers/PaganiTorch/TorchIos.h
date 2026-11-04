/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The flashlight of iOS without a camera (boot argument pagani-torch-ios=1): what PaganiTorchIos.cpp makes, decided here in portable C so the
 * host tests run the same rules. The evidence is in README.md ("The flashlight of iOS").
 *
 * The chain (21H461, static reading): Control Center's FlashlightModule and the lock screen ask +[AVFlashlight hasFlashlight], which is
 * MobileGestalt's camera-flash: the property "flash" of IODeviceTree:/product/camera exists (any value). AVFlashlight talks over XPC to
 * FigFlashlight in mediaserverd. FigFlashlight takes "a device" and "a stream" from BWFigCaptureDeviceVendor and sets the stream's
 * TorchLevel. The vendor, once in the process, picks its capture service with FigCaptureGetCaptureDeviceCreateFunction: for each of
 * AppleH16CamIn, H13, H10, H9, H6, when an IOService of that class exists, dlopen /System/Library/MediaCapture/<H..>ISP.mediacapture. The
 * iPad root's directory is empty and only H9ISP is in the shared cache, so:
 *  - the kernel publishes an IOService of class AppleH6CamIn (the last entry, and not a class of the pinned kernelcache, which has H9, H10 and
 *    H13; besides that table only libMobileGestalt and libZhuGeArmory name it, to read camera module serial numbers from such a service:
 *    the properties are absent, so those answers stay empty) and its user client;
 *  - the plug-in ios/TorchPlugin.m, /System/Library/MediaCapture/H6ISP.mediacapture through the owner overlay, is loaded by that lookup. It does
 *    not export H6ISPCaptureDeviceCreate, so the vendor stays as it is today, without a capture device: no camera appears anywhere. It
 *    answers the vendor's few messages for the client named FigFlashlight only, with a stream whose TorchLevel goes to the user client;
 *  - the kernel adds /product/camera with "flash", only when the plug-in is visible and the driver runs: never a button that does nothing.
 * A kernel without the commit (phone-torch-overlay.patch) makes nothing. */
#ifndef PAGANI_TORCH_IOS_H
#define PAGANI_TORCH_IOS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t create;            /* 1: publish the service and the node */
    const char *why;           /* why not: "off", or the refusal for the boot log */
} PgtIosPlan;

/* The plan for the boot arguments pagani-torch-ios (ios) and pagani-torch (torch). 0: create, or off (ios 0); -1: refused (plan->why). */
int pgt_ios_plan(unsigned ios, unsigned torch, PgtIosPlan *plan);
/* The plan just before launchd: driver_ready, the torch driver answers; plugin_visible, the overlay's stage 5. 0, with create as *plan had
 * it; -1: refused (final->why). */
int pgt_ios_final(const PgtIosPlan *plan, int driver_ready, int plugin_visible, PgtIosPlan *final);
/* The level of an IOConnect call's scalar: 0..1000, else -22. */
int pgt_ios_level(uint64_t scalar);

/* The kernel half (PaganiTorchIos.cpp). The plan is made in pagani_torch_start; the commit is called by load_init_program after the overlay
 * mounts, on the thread that goes on to exec launchd (phone-torch-overlay.patch). */
void pagani_torch_ios_plan(unsigned ios, unsigned torch);
void pagani_torch_ios_commit(int plugin_visible);
/* [0] 1 planned, 2 made; negative: -1 plan refused, -2 no /product, -3 no memory or a property failed, -4 service, -5 final refusal.
 * [1] user client opens, [2] level calls, [3] failed level calls. */
extern volatile int32_t pagani_torch_ios_status[4];

/* Names. */
#define PGT_IOS_PARENT       "IODeviceTree:/product"
#define PGT_IOS_NODE         "camera"
#define PGT_IOS_FLASH        "flash"
#define PGT_IOS_SERVICE      "AppleH6CamIn"            /* CMCapture's table, the last entry */
#define PGT_IOS_CLIENT       "PaganiTorchUserClient"
#define PGT_IOS_PROCESS      "mediaserverd"            /* the only process that may open it, and the sandbox grant's name */
#define PGT_IOS_SELECTOR_SET 0u                        /* one scalar in: the level, 0..1000 */

#ifdef __cplusplus
}
#endif
#endif
