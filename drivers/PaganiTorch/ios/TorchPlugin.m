// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The flashlight of iOS without a camera (TorchIos.h, README.md): /System/Library/MediaCapture/H6ISP.mediacapture, loaded into mediaserverd by
// FigCaptureGetCaptureDeviceCreateFunction because the kernel publishes an AppleH6CamIn. It exports nothing: dlsym of H6ISPCaptureDeviceCreate
// fails and the vendor goes on with no capture device, exactly as it does on this phone without the plug-in, so nothing else in the camera
// stack changes and no camera appears.
//
// FigFlashlight (21H461 CMCapture: flashlight_setupDevice, flashlight_setLevelInternal, flashlight_teardownDevice, flashlight_IsAvailable) sends
// BWFigCaptureDeviceVendor exactly these messages:
//     registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:deviceSharingWithOtherClientsAllowed:
//         deviceAvailabilityChangedHandler:                      description "FigFlashlight"
//     copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:
//     copyStreamForFlashlightWithPosition:deviceType:forDevice:  position 1, type 2; nil makes it give the device back
//     takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:prefersDeviceInvalidatedImmediately:
//     activeDefaultDeviceEquals:
//     unregisterClient:
// and the stream: supportedProperties (it only looks up HighCurrentTorchEnabled) and setProperty:value: with TorchLevel. The device is only
// kept, compared and released. So the constructor replaces those six methods (implementations in this process's memory; no Apple file and no
// code page changes): each passes everything through to the original, except for a client registered under the name FigFlashlight, which
// gets a token device and a stream object of this file. All six are found first, or none is replaced.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include "TorchPlugin.h"

#ifndef PAGANI_TORCH_PLUGIN_HOST
// The iOS SDK has the library but not the headers.
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owner, uint32_t type, io_connect_t *connect);
kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t input_count,
                                        uint64_t *output, uint32_t *output_count);
kern_return_t IOObjectRelease(io_object_t object);

// The kernel's user client (TorchIos.h), opened once: the connection lives as long as mediaserverd, and its end switches the LED off.
static io_connect_t torch_connection(void)
{
    static io_connect_t connection;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(PGT_PLUGIN_SERVICE));
        if (!service) return;
        if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) connection = 0;
        IOObjectRelease(service);
    });
    return connection;
}

int pgt_plugin_kernel_set(uint32_t level)
{
    const io_connect_t connection = torch_connection();
    if (!connection) return -1;
    const uint64_t scalar = level;
    return IOConnectCallScalarMethod(connection, PGT_PLUGIN_SELECTOR, &scalar, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -1;
}
#endif

uint32_t pgt_plugin_level(float value)
{
    if (!(value > 0.0f)) return 0;      // also NaN
    if (value >= 1.0f) return 1000;
    const uint32_t level = (uint32_t)(value * 1000.0f + 0.5f);
    return level ? level : 1;
}

static CFStringRef gTorchLevelKey;      // kFigCaptureStreamProperty_TorchLevel
static NSMutableSet *gClients;          // the IDs of clients named FigFlashlight

static BOOL is_flashlight_client(int client)
{
    @synchronized (gClients) {
        return [gClients containsObject:@(client)];
    }
}

// ---- what FigFlashlight holds ----
@interface PaganiTorchDevice : NSObject
@end
@implementation PaganiTorchDevice
@end

@interface PaganiTorchStream : NSObject
- (NSDictionary *)supportedProperties;
- (int)setProperty:(CFStringRef)key value:(id)value;
@end
@implementation PaganiTorchStream
- (NSDictionary *)supportedProperties
{
    return [NSDictionary dictionary];
}
- (int)setProperty:(CFStringRef)key value:(id)value
{
    if (!key || !CFEqual(key, gTorchLevelKey) || ![value respondsToSelector:@selector(floatValue)]) return PGT_PLUGIN_UNSUPPORTED;
    const int rc = pgt_plugin_kernel_set(pgt_plugin_level([value floatValue]));
    if (rc) os_log_error(OS_LOG_DEFAULT, "PaganiTorch: the kernel refused the level");
    return rc ? PGT_PLUGIN_FAILED : 0;
}
@end

// ---- the vendor's six methods ----
typedef int (*RegisterImp)(id, SEL, int, id, int, BOOL, BOOL, id);
typedef int (*UnregisterImp)(id, SEL, int);
typedef id (*CopyDeviceImp)(id, SEL, int, BOOL, int *);
typedef id (*CopyStreamImp)(id, SEL, int, int, id);
typedef void (*TakeBackImp)(id, SEL, id, int, BOOL, BOOL);
typedef BOOL (*ActiveImp)(id, SEL, id);
static RegisterImp gRegister;
static UnregisterImp gUnregister;
static CopyDeviceImp gCopyDevice;
static CopyStreamImp gCopyStream;
static TakeBackImp gTakeBack;
static ActiveImp gActive;

static int new_register(id self, SEL cmd, int pid, id description, int priority, BOOL steal, BOOL share, id handler)
{
    const int client = gRegister(self, cmd, pid, description, priority, steal, share, handler);
    if ([description isKindOfClass:[NSString class]] && [description isEqualToString:@"FigFlashlight"]) {
        @synchronized (gClients) {
            [gClients addObject:@(client)];
        }
    }
    return client;
}

static int new_unregister(id self, SEL cmd, int client)
{
    @synchronized (gClients) {
        [gClients removeObject:@(client)];
    }
    return gUnregister(self, cmd, client);
}

static id new_copy_device(id self, SEL cmd, int client, BOOL inform, int *error)
{
    if (!is_flashlight_client(client)) return gCopyDevice(self, cmd, client, inform, error);
    if (error) *error = 0;
    return [[PaganiTorchDevice alloc] init];     // a copy: the caller releases it
}

static id new_copy_stream(id self, SEL cmd, int position, int type, id device)
{
    if (![device isKindOfClass:[PaganiTorchDevice class]]) return gCopyStream(self, cmd, position, type, device);
    return [[PaganiTorchStream alloc] init];     // the same
}

static void new_take_back(id self, SEL cmd, id device, int client, BOOL inform, BOOL prefers)
{
    if (![device isKindOfClass:[PaganiTorchDevice class]]) gTakeBack(self, cmd, device, client, inform, prefers);
}

static BOOL new_active(id self, SEL cmd, id device)
{
    return [device isKindOfClass:[PaganiTorchDevice class]] ? YES : gActive(self, cmd, device);
}

int pgt_plugin_install(void)
{
    if (gRegister) return 0;
    Class vendor = objc_getClass("BWFigCaptureDeviceVendor");
    const CFStringRef *key = dlsym(RTLD_DEFAULT, PGT_PLUGIN_KEY_SYMBOL);
    if (!vendor || !key || !*key) return -1;
    static const char *const names[6] = {
        ("registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
         "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:"),
        "unregisterClient:",
        "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:",
        "copyStreamForFlashlightWithPosition:deviceType:forDevice:",
        "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:prefersDeviceInvalidatedImmediately:",
        "activeDefaultDeviceEquals:"};
    Method methods[6];
    for (unsigned i = 0; i < 6; ++i) {
        methods[i] = class_getInstanceMethod(vendor, sel_registerName(names[i]));
        if (!methods[i]) return -2;
    }
    gTorchLevelKey = *key;
    gClients = [[NSMutableSet alloc] init];
    gRegister = (RegisterImp)method_setImplementation(methods[0], (IMP)new_register);
    gUnregister = (UnregisterImp)method_setImplementation(methods[1], (IMP)new_unregister);
    gCopyDevice = (CopyDeviceImp)method_setImplementation(methods[2], (IMP)new_copy_device);
    gCopyStream = (CopyStreamImp)method_setImplementation(methods[3], (IMP)new_copy_stream);
    gTakeBack = (TakeBackImp)method_setImplementation(methods[4], (IMP)new_take_back);
    gActive = (ActiveImp)method_setImplementation(methods[5], (IMP)new_active);
    return 0;
}

#ifndef PAGANI_TORCH_PLUGIN_HOST
__attribute__((constructor)) static void pgt_plugin_load(void)
{
    if (strcmp(getprogname(), PGT_PLUGIN_PROCESS)) return;
    const int rc = pgt_plugin_install();
    os_log(OS_LOG_DEFAULT, "PaganiTorch: flashlight plug-in loaded, install %d", rc);
}
#endif
