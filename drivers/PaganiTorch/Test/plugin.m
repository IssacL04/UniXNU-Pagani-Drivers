// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of ios/TorchPlugin.m: the real file, on this Mac's Objective-C runtime, against a stand-in BWFigCaptureDeviceVendor with the selectors
// of 21H461. flashlight() sends what FigFlashlight sends, in its order (the disassembly of flashlight_setupDevice, flashlight_setLevelInternal,
// flashlight_teardownDevice and flashlight_IsAvailable): other clients must reach the originals untouched. Built three ways: normally,
// with -DNO_METHOD (a vendor without one of the six: nothing replaced), with -DNO_KEY (the key's symbol missing: nothing replaced).
#import <Foundation/Foundation.h>
#include <math.h>
#include "check.h"
#include "../ios/TorchPlugin.h"

__attribute__((used, visibility("default"))) CFStringRef kFigCaptureStreamProperty_TorchLevel = CFSTR("TorchLevel");

static unsigned gCalls[6], gLevels, gFail;
static uint32_t gLast;
int pgt_plugin_kernel_set(uint32_t level)
{
    gLevels++;
    gLast = level;
    return gFail ? -1 : 0;
}

@interface RealDevice : NSObject
@end
@implementation RealDevice
@end
@interface RealStream : NSObject
@end
@implementation RealStream
@end

@interface BWFigCaptureDeviceVendor : NSObject
@end
@implementation BWFigCaptureDeviceVendor
- (int)registerClientWithPID:(int)pid clientDescription:(id)d clientPriority:(int)p canStealFromClientsWithSamePriority:(BOOL)s
        deviceSharingWithOtherClientsAllowed:(BOOL)a deviceAvailabilityChangedHandler:(id)h
{
    gCalls[0]++;
    static int next = 100;
    return ++next;
}
#ifndef NO_METHOD
- (int)unregisterClient:(int)client
{
    gCalls[1]++;
    return 0;
}
#endif
- (id)copyDeviceForClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform error:(int *)error
{
    gCalls[2]++;
    if (error) *error = -1234;           // this phone's vendor: no capture device
    return nil;
}
- (id)copyStreamForFlashlightWithPosition:(int)position deviceType:(int)type forDevice:(id)device
{
    gCalls[3]++;
    return [[RealStream alloc] init];
}
- (void)takeBackDevice:(id)device forClient:(int)client informClientWhenDeviceAvailableAgain:(BOOL)inform
        prefersDeviceInvalidatedImmediately:(BOOL)prefers
{
    gCalls[4]++;
}
- (BOOL)activeDefaultDeviceEquals:(id)device
{
    gCalls[5]++;
    return device == nil;
}
@end

@interface NSObject (Stream)
- (NSDictionary *)supportedProperties;
- (int)setProperty:(CFStringRef)key value:(id)value;
@end

#if !defined(NO_METHOD) && !defined(NO_KEY)
static void levels(void)
{
    CHECK(pgt_plugin_level(0.0f) == 0 && pgt_plugin_level(-1.0f) == 0 && pgt_plugin_level(NAN) == 0);
    CHECK(pgt_plugin_level(1.0f) == 1000 && pgt_plugin_level(2.0f) == 1000 && pgt_plugin_level(INFINITY) == 1000);
    CHECK(pgt_plugin_level(0.5f) == 500 && pgt_plugin_level(0.25f) == 250 && pgt_plugin_level(0.75f) == 750);
    CHECK(pgt_plugin_level(0.0001f) == 1 && pgt_plugin_level(1e-30f) == 1);   /* above 0 is on */
    for (unsigned i = 1; i <= 1000; ++i) {
        const uint32_t l = pgt_plugin_level((float)i / 1000.0f);
        CHECK(l == i);
    }
}

// What another capture client does: everything reaches the originals.
static void other_client(BWFigCaptureDeviceVendor *vendor)
{
    unsigned before[6];
    memcpy(before, gCalls, sizeof before);
    const int client = [vendor registerClientWithPID:1 clientDescription:@"FigCaptureSession" clientPriority:1
                 canStealFromClientsWithSamePriority:NO deviceSharingWithOtherClientsAllowed:NO deviceAvailabilityChangedHandler:nil];
    int error = 0;
    CHECK([vendor copyDeviceForClient:client informClientWhenDeviceAvailableAgain:NO error:&error] == nil && error == -1234);
    RealDevice *device = [[RealDevice alloc] init];
    id stream = [vendor copyStreamForFlashlightWithPosition:1 deviceType:2 forDevice:device];
    CHECK([stream isKindOfClass:[RealStream class]]);
    [vendor takeBackDevice:device forClient:client informClientWhenDeviceAvailableAgain:NO prefersDeviceInvalidatedImmediately:NO];
    CHECK(![vendor activeDefaultDeviceEquals:device] && [vendor activeDefaultDeviceEquals:nil]);
    CHECK([vendor unregisterClient:client] == 0);
    CHECK(gCalls[0] == before[0] + 1 && gCalls[1] == before[1] + 1 && gCalls[2] == before[2] + 1 && gCalls[3] == before[3] + 1);
    CHECK(gCalls[4] == before[4] + 1 && gCalls[5] == before[5] + 2);
    [device release];
    [stream release];
}

static void flashlight(BWFigCaptureDeviceVendor *vendor)
{
    unsigned before[6];
    memcpy(before, gCalls, sizeof before);
    /* flashlight_NotifyForCurrentState with no device yet: the original's answer */
    CHECK([vendor activeDefaultDeviceEquals:nil] == YES && gCalls[5] == before[5] + 1);
    /* flashlight_setupDevice */
    const int client = [vendor registerClientWithPID:40 clientDescription:@"FigFlashlight" clientPriority:2
                 canStealFromClientsWithSamePriority:NO deviceSharingWithOtherClientsAllowed:YES deviceAvailabilityChangedHandler:nil];
    CHECK(gCalls[0] == before[0] + 1 && client > 100);
    int error = -7;
    id device = [vendor copyDeviceForClient:client informClientWhenDeviceAvailableAgain:NO error:&error];
    CHECK(device && error == 0 && gCalls[2] == before[2]);      /* never the original: it has no device */
    id stream = [vendor copyStreamForFlashlightWithPosition:1 deviceType:2 forDevice:device];
    CHECK(stream && gCalls[3] == before[3]);
    CHECK([stream supportedProperties][@"HighCurrentTorchEnabled"] == nil);
    /* flashlight_IsAvailable with the device */
    const unsigned active = gCalls[5];
    CHECK([vendor activeDefaultDeviceEquals:device] == YES && gCalls[5] == active);
    /* flashlight_setLevelInternal */
    gLevels = 0;
    CHECK([stream setProperty:CFSTR("TorchLevel") value:[NSNumber numberWithFloat:0.5f]] == 0 && gLast == 500 && gLevels == 1);
    CHECK([stream setProperty:CFSTR("TorchLevel") value:[NSNumber numberWithFloat:1.0f]] == 0 && gLast == 1000);
    CHECK([stream setProperty:CFSTR("TorchLevel") value:[NSNumber numberWithInt:0]] == 0 && gLast == 0 && gLevels == 3);
    /* anything else reaches nothing */
    CHECK([stream setProperty:CFSTR("Other") value:[NSNumber numberWithFloat:1.0f]] == PGT_PLUGIN_UNSUPPORTED && gLevels == 3);
    CHECK([stream setProperty:CFSTR("TorchLevel") value:@"1"] == 0 || gLevels >= 3);   /* NSString answers floatValue: still 0..1000 */
    gLevels = 3;
    CHECK([stream setProperty:CFSTR("TorchLevel") value:(id)[NSArray array]] == PGT_PLUGIN_UNSUPPORTED && gLevels == 3);
    CHECK([stream setProperty:NULL value:[NSNumber numberWithFloat:1.0f]] == PGT_PLUGIN_UNSUPPORTED && gLevels == 3);
    /* the kernel refuses: the caller hears it */
    gFail = 1;
    CHECK([stream setProperty:CFSTR("TorchLevel") value:[NSNumber numberWithFloat:1.0f]] == PGT_PLUGIN_FAILED);
    gFail = 0;
    /* flashlight_teardownDevice: the level to 0, the device back (not to the original), both released */
    CHECK([stream setProperty:CFSTR("TorchLevel") value:[NSNumber numberWithInt:0]] == 0 && gLast == 0);
    const unsigned back = gCalls[4];
    [vendor takeBackDevice:device forClient:client informClientWhenDeviceAvailableAgain:NO prefersDeviceInvalidatedImmediately:NO];
    CHECK(gCalls[4] == back);
    CFRelease((CFTypeRef)device);
    [stream release];
    /* and again on the same client */
    id again = [vendor copyDeviceForClient:client informClientWhenDeviceAvailableAgain:NO error:&error];
    CHECK(again && gCalls[2] == before[2]);
    [again release];
    /* unregistered: the ID is an ordinary one again */
    CHECK([vendor unregisterClient:client] == 0);
    CHECK([vendor copyDeviceForClient:client informClientWhenDeviceAvailableAgain:NO error:&error] == nil && gCalls[2] == before[2] + 1);
    /* a null error pointer */
    const int second = [vendor registerClientWithPID:41 clientDescription:@"FigFlashlight" clientPriority:2
                 canStealFromClientsWithSamePriority:NO deviceSharingWithOtherClientsAllowed:YES deviceAvailabilityChangedHandler:nil];
    id d2 = [vendor copyDeviceForClient:second informClientWhenDeviceAvailableAgain:NO error:NULL];
    CHECK(d2 != nil);
    [d2 release];
    CHECK([vendor unregisterClient:second] == 0);
}
#endif

int main(void)
{
    @autoreleasepool {
        BWFigCaptureDeviceVendor *vendor = [[BWFigCaptureDeviceVendor alloc] init];
#if defined(NO_METHOD)
        CHECK(pgt_plugin_install() == -2);
        [vendor registerClientWithPID:1 clientDescription:@"FigFlashlight" clientPriority:1 canStealFromClientsWithSamePriority:NO
             deviceSharingWithOtherClientsAllowed:NO deviceAvailabilityChangedHandler:nil];
        int error = 0;
        CHECK([vendor copyDeviceForClient:101 informClientWhenDeviceAvailableAgain:NO error:&error] == nil && error == -1234 && gCalls[2] == 1);
        printf("PASS plugin-no-method: %u checks\n", checks);
#elif defined(NO_KEY)
        CHECK(pgt_plugin_install() == -1);
        int error = 0;
        CHECK([vendor copyDeviceForClient:101 informClientWhenDeviceAvailableAgain:NO error:&error] == nil && gCalls[2] == 1);
        printf("PASS plugin-no-key: %u checks\n", checks);
#else
        levels();
        other_client(vendor);        /* before the install */
        CHECK(pgt_plugin_install() == 0 && pgt_plugin_install() == 0);
        other_client(vendor);        /* and after */
        flashlight(vendor);
        other_client(vendor);
        flashlight(vendor);          /* a second session */
        printf("PASS plugin: %u checks\n", checks);
#endif
        [vendor release];
    }
    return 0;
}
