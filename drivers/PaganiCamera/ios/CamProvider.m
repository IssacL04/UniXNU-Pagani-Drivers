// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 阶段 1a：H6ISP.mediacapture 里同进程的 CMIOExtensionProvider，出 CamPattern.c 的测试图。不碰相机硬件。和 PaganiTorch ios/TorchPlugin.m
// 编进同一个文件，各有各的构造函数，互不依赖：手电筒替换厂商类的六个实例方法，这里替换它的初始化方法，都只是本进程内存里的实现。
//
// 一、厂商类必须先活着。+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]（21H461 0x1a693ee00）调
// -initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:cmioDiscoverySession:（0x1a693fcf4），里面（0x1a693fddc）：默认设备的
// 创建函数是空、又不在虚拟机里（FigCaptureIsRunningInVirtualization，kern.hv_vmm_present，这台手机的 /defaults 没有 vmm-present，是 0）就 bail，
// 返回 nil。这台手机上 dlsym H6ISPCaptureDeviceCreate 故意失败，所以不处理的话 sharedCaptureDeviceVendor 永远是 nil，发给它的消息全落空：外接相机
// 出不来，TorchPlugin.m 替换的六个方法也一次都不会被调用。插件在 FigCaptureGetCaptureDeviceCreateFunction 里 dlopen，正好在这次 init 之前，所以
// 构造函数换掉 init：默认函数是空时传一个永远失败的占位函数过这道检查，返回后把实例变量 _defaultDeviceCreateFunction 写回空。之后的状态和
// 虚拟机里没有默认设备时完全一样：deviceIDs 不含 Default（0x1a69aab60），_createDevice 走 "Cannot create device without create function!"。
//
// 二、绕过 launchd。正常的扩展由 launchd 按事件拉起（CMIOExtensionProviderServer），总表里没有这样的任务，+startServiceWithProvider:
// （0x21ccde8d8）和 -[CMIOExtensionProviderServer init]（0x21ccfcbb4）又要求主 bundle 有 CMIOExtension 字典，mediaserverd 没有：前者抛异常，后者返回
// nil。nil 正好无害：-initWithSource:clientQueue:（0x21ccde34c）和 -[CMIOExtensionProviderContext initWithConnection:]（0x21cd054c4）只问它
// internalDaemon，nil 答 0，客户的相机授权直接是 3（已授权）。所以这里不启动服务，自己做服务做的那两件事：
//   1. 匿名监听 xpc_connection_create(NULL, q)、xpc_endpoint_create（-[CMIOExtensionProviderServer init]，0x21ccfcc94）；
//   2. 每个新连接 [[CMIOExtensionProviderContext alloc] initWithConnection:] 加 -[CMIOExtensionProvider beginProviderContextRegistration:]
//      （-addConnection:，0x21ccfd25c，它不用 self）。
// 另一头照 -[CMIOExtensionDiscoverySession completeRegistrationForBundleID:token:] 的回调（0x21cd0b5c0）：在会话自己的队列上
// initWithEndpoint:delegate:、setBundleID:、放进 _providersByBundleIdentifier。设备到了之后 provider:availableDevicesChanged: →
// postDeviceChangeNotification → 厂商的 _updateCMIOExtensionDevices（0x1a69af35c）→ CMIOExtensionFigCaptureDeviceCreate，后面全是苹果自己的路。
//
// 构造函数里不做这些：dlopen 持着 dyld 的锁，苹果的类初始化和软链接要 dlopen，还要等 XPC，放到队列上等 dlopen 返回之后。
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include "CamPattern.h"
#include "CamProvider.h"

volatile int32_t pcam_status[8];

// 这些类在 iOS 的 SDK 里没有头文件，CoreMediaIO.tbd 里有，运行时按名字取。签名和 macOS SDK 的 CMIOExtension*.h 一致，21H461 逐个核对过（README.md）。
@interface NSObject (PaganiCamCmio)
+ (id)providerWithSource:(id)source clientQueue:(dispatch_queue_t)queue;
- (BOOL)addDevice:(id)device error:(NSError **)error;
- (void)beginProviderContextRegistration:(id)context;
+ (id)deviceWithLocalizedName:(NSString *)name deviceID:(NSUUID *)device legacyDeviceID:(NSString *)legacy source:(id)source;
- (BOOL)addStream:(id)stream error:(NSError **)error;
+ (id)streamWithLocalizedName:(NSString *)name streamID:(NSUUID *)stream direction:(long)direction clockType:(long)clock source:(id)source;
- (void)sendSampleBuffer:(CMSampleBufferRef)sample discontinuity:(uint32_t)flags hostTimeInNanoseconds:(uint64_t)nanoseconds;
+ (id)streamFormatWithFormatDescription:(CMFormatDescriptionRef)format maxFrameDuration:(CMTime)longest minFrameDuration:(CMTime)shortest
                     validFrameDurations:(NSArray *)valid;
+ (id)providerPropertiesWithDictionary:(NSDictionary *)dictionary;
+ (id)devicePropertiesWithDictionary:(NSDictionary *)dictionary;
+ (id)streamPropertiesWithDictionary:(NSDictionary *)dictionary;
- (void)setManufacturer:(NSString *)manufacturer;
- (void)setName:(NSString *)name;
- (void)setModel:(NSString *)model;
- (void)setTransportType:(NSNumber *)type;
- (void)setPropertyState:(id)state forProperty:(NSString *)property;
+ (id)propertyStateWithValue:(id)value;
- (void)setActiveFormatIndex:(NSNumber *)index;
- (NSNumber *)activeFormatIndex;
- (void)setFrameDuration:(NSDictionary *)duration;
- (void)setMaxFrameDuration:(NSDictionary *)duration;
- (id)initWithConnection:(xpc_connection_t)connection;
- (id)initWithEndpoint:(xpc_endpoint_t)endpoint delegate:(id)delegate;
- (void)setBundleID:(NSString *)bundle;
+ (id)sharedInstance;
@end

// ---- 厂商类 ----
typedef id (*VendorInit)(id, SEL, void *, void *, id);
static VendorInit gVendorInit;
static ptrdiff_t gDefaultOffset;
#define PCAM_VENDOR_INIT "initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:cmioDiscoverySession:"

// 占位的默认设备创建函数：过了 init 就清掉。万一被调用，也只是报错，不给设备。
static int placeholder_create(CFAllocatorRef allocator, void *argument, void **device)
{
    (void)allocator;
    (void)argument;
    if (device) *device = NULL;
    return -12782;
}

static id new_vendor_init(id self, SEL cmd, void *create, void *cmio, id session)
{
    if (create || !cmio) return gVendorInit(self, cmd, create, cmio, session);
    id vendor = gVendorInit(self, cmd, (void *)placeholder_create, cmio, session);
    if (vendor) *(void **)((char *)vendor + gDefaultOffset) = NULL;
    return vendor;
}

int pcam_vendor_install(void)
{
    if (gVendorInit) return PcamVendorInstalled;
    Class vendor = objc_getClass("BWFigCaptureDeviceVendor");
    if (!vendor) return PcamVendorNoClass;
    Method method = class_getInstanceMethod(vendor, sel_registerName(PCAM_VENDOR_INIT));
    if (!method) return PcamVendorNoMethod;
    Ivar ivar = class_getInstanceVariable(vendor, "_defaultDeviceCreateFunction");
    if (!ivar) return PcamVendorNoIvar;
    gDefaultOffset = ivar_getOffset(ivar);
    gVendorInit = (VendorInit)method_setImplementation(method, (IMP)new_vendor_init);
    return PcamVendorInstalled;
}

// ---- 帧 ----
typedef struct {
    unsigned width, height;
    PcamPattern pattern;
    CVPixelBufferPoolRef pool;
    CMFormatDescriptionRef format;          // 流表里的格式
    CMFormatDescriptionRef sample_format;   // 从第一个缓冲取，才和缓冲的附件一致
} Mode;

static Mode gModes[PcamModes] = {{.width = 1920, .height = 1080}, {.width = 1280, .height = 720}};
static unsigned gActive = 1;                // 默认 720p：省 CPU，扫码够用
static uint32_t gFrame;
static dispatch_queue_t gFrameQueue;        // 串行：定时器、格式切换、启停都在它上面
static dispatch_source_t gTimer;
static id gStream;

static void record_error(int code)
{
    pcam_status[6] = code;
}

static int mode_prepare(Mode *m)
{
    if (pcam_pattern_init(&m->pattern, m->width, m->height)) return -1;
    NSDictionary *buffer = @{(id)kCVPixelBufferPixelFormatTypeKey: @(PCAM_PIXEL_FORMAT), (id)kCVPixelBufferWidthKey: @(m->width),
                             (id)kCVPixelBufferHeightKey: @(m->height), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    NSDictionary *pool = @{(id)kCVPixelBufferPoolMinimumBufferCountKey: @4};
    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)pool, (__bridge CFDictionaryRef)buffer, &m->pool)) return -2;
    if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, PCAM_PIXEL_FORMAT, (int32_t)m->width, (int32_t)m->height, NULL, &m->format)) return -3;
    return 0;
}

static void frame_tick(void)
{
    Mode *m = &gModes[gActive];
    CVPixelBufferRef buffer = NULL;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, m->pool, &buffer)) {
        __atomic_fetch_add(&pcam_status[5], 1, __ATOMIC_RELAXED);
        return;
    }
    if (CVPixelBufferLockBaseAddress(buffer, 0)) {
        CVPixelBufferRelease(buffer);
        __atomic_fetch_add(&pcam_status[5], 1, __ATOMIC_RELAXED);
        return;
    }
    pcam_pattern_fill(&m->pattern, gFrame, CVPixelBufferGetBaseAddressOfPlane(buffer, 0), CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
                      CVPixelBufferGetBaseAddressOfPlane(buffer, 1), CVPixelBufferGetBytesPerRowOfPlane(buffer, 1));
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    if (!m->sample_format && CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer, &m->sample_format)) m->sample_format = NULL;
    const CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
    CMSampleTimingInfo timing = {CMTimeMake(1, PcamFrameRate), now, kCMTimeInvalid};
    CMSampleBufferRef sample = NULL;
    OSStatus status = m->sample_format ? CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, buffer, true, NULL, NULL, m->sample_format, &timing, &sample) : -1;
    CVPixelBufferRelease(buffer);
    if (status || !sample) {
        record_error((int)status);
        __atomic_fetch_add(&pcam_status[5], 1, __ATOMIC_RELAXED);
        return;
    }
    @autoreleasepool {
        [gStream sendSampleBuffer:sample discontinuity:0 hostTimeInNanoseconds:(uint64_t)CMTimeConvertScale(now, 1000000000, kCMTimeRoundingMethod_Default).value];
    }
    CFRelease(sample);
    ++gFrame;
    const int32_t sent = __atomic_add_fetch(&pcam_status[4], 1, __ATOMIC_RELAXED);
    if (sent == 1 || sent % PcamFrameLogEvery == 0) {
        char line[PcamLogMax];
        snprintf(line, sizeof line, "frames=%d dropped=%d mode=%ux%u", sent, pcam_status[5], m->width, m->height);
        pcam_kernel_line(line);
    }
}

static void timer_start(void)
{
    if (gTimer) return;
    gTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, gFrameQueue);
    dispatch_source_set_timer(gTimer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / PcamFrameRate, 0);
    dispatch_source_set_event_handler(gTimer, ^{frame_tick();});
    dispatch_resume(gTimer);
}

static void timer_stop(void)
{
    if (!gTimer) return;
    dispatch_source_cancel(gTimer);
    dispatch_release(gTimer);
    gTimer = NULL;
}

// ---- 三个 source，协议在 CMIOExtensionProvider.h、CMIOExtensionDevice.h、CMIOExtensionStream.h ----
static CFStringRef gKeyManufacturer, gKeyProviderName, gKeyModel, gKeyTransport, gKeyDeviceMaker, gKeyDeviceName, gKeyFormatIndex, gKeyFrameDuration,
    gKeyMaxFrameDuration;
static id gFormats;

@interface PaganiCamProviderSource : NSObject
@end
@implementation PaganiCamProviderSource
- (BOOL)connectClient:(id)client error:(NSError **)error
{
    (void)client;
    (void)error;
    __atomic_fetch_add(&pcam_status[0], 1, __ATOMIC_RELAXED);
    return YES;
}
- (void)disconnectClient:(id)client
{
    (void)client;
    __atomic_fetch_add(&pcam_status[1], 1, __ATOMIC_RELAXED);
}
- (NSSet *)availableProperties
{
    return [NSSet setWithObjects:(id)gKeyManufacturer, (id)gKeyProviderName, nil];
}
- (id)providerPropertiesForProperties:(NSSet *)properties error:(NSError **)error
{
    (void)error;
    id out = [objc_getClass("CMIOExtensionProviderProperties") providerPropertiesWithDictionary:@{}];
    if ([properties containsObject:(id)gKeyManufacturer]) [out setManufacturer:@"UniXNU"];
    if ([properties containsObject:(id)gKeyProviderName]) [out setName:@"Pagani Test Pattern"];
    return out;
}
- (BOOL)setProviderProperties:(id)properties error:(NSError **)error
{
    (void)properties;
    (void)error;
    return YES;
}
@end

@interface PaganiCamDeviceSource : NSObject
@end
@implementation PaganiCamDeviceSource
- (NSSet *)availableProperties
{
    return [NSSet setWithObjects:(id)gKeyTransport, (id)gKeyModel, (id)gKeyDeviceMaker, (id)gKeyDeviceName, nil];
}
- (id)devicePropertiesForProperties:(NSSet *)properties error:(NSError **)error
{
    (void)error;
    id out = [objc_getClass("CMIOExtensionDeviceProperties") devicePropertiesWithDictionary:@{}];
    if ([properties containsObject:(id)gKeyTransport]) [out setTransportType:@(0x75736220)];   // 'usb '：kIOAudioDeviceTransportTypeUSB
    if ([properties containsObject:(id)gKeyModel]) [out setModel:@"PaganiTestPattern"];
    // -[BWFigVideoCaptureDevice _initWithCaptureDevice:...] 外接时强制读厂商和非本地化名，会话设备从这两个键取（0x21ccf4a88）。
    Class state = objc_getClass("CMIOExtensionPropertyState");
    if ([properties containsObject:(id)gKeyDeviceMaker]) [out setPropertyState:[state propertyStateWithValue:@"UniXNU"] forProperty:(id)gKeyDeviceMaker];
    if ([properties containsObject:(id)gKeyDeviceName]) [out setPropertyState:[state propertyStateWithValue:@"Pagani Test Pattern"] forProperty:(id)gKeyDeviceName];
    return out;
}
- (BOOL)setDeviceProperties:(id)properties error:(NSError **)error
{
    (void)properties;
    (void)error;
    return YES;
}
@end

@interface PaganiCamStreamSource : NSObject
@end
@implementation PaganiCamStreamSource
- (NSArray *)formats
{
    return gFormats;
}
- (NSSet *)availableProperties
{
    return [NSSet setWithObjects:(id)gKeyFormatIndex, (id)gKeyFrameDuration, (id)gKeyMaxFrameDuration, nil];
}
- (id)streamPropertiesForProperties:(NSSet *)properties error:(NSError **)error
{
    (void)error;
    id out = [objc_getClass("CMIOExtensionStreamProperties") streamPropertiesWithDictionary:@{}];
    __block unsigned active = 0;
    dispatch_sync(gFrameQueue, ^{active = gActive;});
    if ([properties containsObject:(id)gKeyFormatIndex]) [out setActiveFormatIndex:@(active)];
    NSDictionary *duration = [(id)CMTimeCopyAsDictionary(CMTimeMake(1, PcamFrameRate), kCFAllocatorDefault) autorelease];
    if ([properties containsObject:(id)gKeyFrameDuration]) [out setFrameDuration:duration];
    if ([properties containsObject:(id)gKeyMaxFrameDuration]) [out setMaxFrameDuration:duration];
    return out;
}
- (BOOL)setStreamProperties:(id)properties error:(NSError **)error
{
    (void)error;
    NSNumber *index = [properties activeFormatIndex];
    if (!index) return YES;                                 // 帧率固定 30：别的属性收下不动
    const unsigned value = [index unsignedIntValue];
    if (value >= PcamModes) return NO;
    dispatch_sync(gFrameQueue, ^{
        gActive = value;
        pcam_status[7] = (int32_t)value;
    });
    return YES;
}
- (BOOL)authorizedToStartStreamForClient:(id)client
{
    (void)client;
    return YES;
}
- (BOOL)startStreamAndReturnError:(NSError **)error
{
    (void)error;
    dispatch_sync(gFrameQueue, ^{timer_start();});
    __atomic_fetch_add(&pcam_status[2], 1, __ATOMIC_RELAXED);
    pcam_kernel_line("stream start");
    return YES;
}
- (BOOL)stopStreamAndReturnError:(NSError **)error
{
    (void)error;
    dispatch_sync(gFrameQueue, ^{timer_stop();});
    __atomic_fetch_add(&pcam_status[3], 1, __ATOMIC_RELAXED);
    pcam_kernel_line("stream stop");
    return YES;
}
@end

// ---- 把它们接起来 ----
static CFStringRef key_named(const char *name)
{
    const CFStringRef *key = dlsym(RTLD_DEFAULT, name);
    return key ? *key : NULL;
}

static int keys_load(void)
{
    gKeyManufacturer = key_named("CMIOExtensionPropertyProviderManufacturer");
    gKeyProviderName = key_named("CMIOExtensionPropertyProviderName");
    gKeyModel = key_named("CMIOExtensionPropertyDeviceModel");
    gKeyTransport = key_named("CMIOExtensionPropertyDeviceTransportType");
    gKeyDeviceMaker = key_named("CMIOExtensionPropertyDeviceManufacturer");
    gKeyDeviceName = key_named("CMIOExtensionPropertyDeviceNameNonLocalized");
    gKeyFormatIndex = key_named("CMIOExtensionPropertyStreamActiveFormatIndex");
    gKeyFrameDuration = key_named("CMIOExtensionPropertyStreamFrameDuration");
    gKeyMaxFrameDuration = key_named("CMIOExtensionPropertyStreamMaxFrameDuration");
    return gKeyManufacturer && gKeyProviderName && gKeyModel && gKeyTransport && gKeyDeviceMaker && gKeyDeviceName && gKeyFormatIndex && gKeyFrameDuration &&
           gKeyMaxFrameDuration ? 0 : -1;
}

// 全部先查：少一个类或方法就什么都不建，mediaserverd 照旧。
static int interface_check(void)
{
    static const struct {const char *cls, *sel; int instance;} need[] = {
        {"CMIOExtensionProvider", "providerWithSource:clientQueue:", 0},
        {"CMIOExtensionProvider", "addDevice:error:", 1},
        {"CMIOExtensionProvider", "beginProviderContextRegistration:", 1},
        {"CMIOExtensionDevice", "deviceWithLocalizedName:deviceID:legacyDeviceID:source:", 0},
        {"CMIOExtensionDevice", "addStream:error:", 1},
        {"CMIOExtensionStream", "streamWithLocalizedName:streamID:direction:clockType:source:", 0},
        {"CMIOExtensionStream", "sendSampleBuffer:discontinuity:hostTimeInNanoseconds:", 1},
        {"CMIOExtensionStreamFormat", "streamFormatWithFormatDescription:maxFrameDuration:minFrameDuration:validFrameDurations:", 0},
        {"CMIOExtensionProviderProperties", "providerPropertiesWithDictionary:", 0},
        {"CMIOExtensionDeviceProperties", "devicePropertiesWithDictionary:", 0},
        {"CMIOExtensionDeviceProperties", "setPropertyState:forProperty:", 1},
        {"CMIOExtensionPropertyState", "propertyStateWithValue:", 0},
        {"CMIOExtensionStreamProperties", "streamPropertiesWithDictionary:", 0},
        {"CMIOExtensionProviderContext", "initWithConnection:", 1},
        {"CMIOExtensionSessionProvider", "initWithEndpoint:delegate:", 1},
        {"CMIOExtensionSessionProvider", "setBundleID:", 1},
        {"CMIOExtensionDiscoverySession", "sharedInstance", 0},
        {"CMIOExtensionDiscoverySession", "postDeviceChangeNotification", 1},
    };
    for (unsigned i = 0; i < sizeof need / sizeof need[0]; ++i) {
        Class c = objc_getClass(need[i].cls);
        if (!c) return PcamStartNoClass;
        const SEL s = sel_registerName(need[i].sel);
        if (need[i].instance ? !class_getInstanceMethod(c, s) : !class_getClassMethod(c, s)) return PcamStartNoSelector;
    }
    return PcamStartOk;
}

static id ivar_object(id object, const char *name)
{
    Ivar ivar = class_getInstanceVariable(object_getClass(object), name);
    return ivar ? object_getIvar(object, ivar) : nil;
}

static void session_register(id session, xpc_endpoint_t endpoint)
{
    NSMutableDictionary *providers = ivar_object(session, "_providersByBundleIdentifier");
    @try {
        id provider = [[objc_getClass("CMIOExtensionSessionProvider") alloc] initWithEndpoint:endpoint delegate:session];
        [provider setBundleID:@PCAM_BUNDLE];
        [providers setObject:provider forKey:@PCAM_BUNDLE];
        [provider release];
    } @catch (NSException *exception) {
        record_error(PcamStartListener);
        os_log_error(OS_LOG_DEFAULT, "PaganiCamera: session provider: %{public}@", exception);
    }
}

int pcam_provider_start(void)
{
    static int started;
    if (started) return PcamStartAgain;
    int rc = interface_check();
    if (rc) return rc;
    id session = [objc_getClass("CMIOExtensionDiscoverySession") sharedInstance];
    dispatch_queue_t session_queue = ivar_object(session, "_queue");
    if (!session_queue || !ivar_object(session, "_providersByBundleIdentifier")) return PcamStartNoIvar;
    if (keys_load()) return PcamStartNoKey;
    gFrameQueue = dispatch_queue_create("org.unixnu.pagani.camera.frames", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    NSMutableArray *formats = [[NSMutableArray alloc] init];
    const CMTime duration = CMTimeMake(1, PcamFrameRate);
    for (unsigned i = 0; i < PcamModes; ++i) {
        if (mode_prepare(&gModes[i])) return PcamStartMode;
        [formats addObject:[objc_getClass("CMIOExtensionStreamFormat") streamFormatWithFormatDescription:gModes[i].format maxFrameDuration:duration
                                                                                        minFrameDuration:duration validFrameDurations:nil]];
    }
    gFormats = formats;
    pcam_status[7] = (int32_t)gActive;
    started = 1;
    // 三个 source 由 CMIOExtension 弱引用，留到进程结束。
    id provider_source = [[PaganiCamProviderSource alloc] init], device_source = [[PaganiCamDeviceSource alloc] init], stream_source = [[PaganiCamStreamSource alloc] init];
    NSUUID *device_id = [[NSUUID alloc] initWithUUIDString:@PCAM_DEVICE_ID], *stream_id = [[NSUUID alloc] initWithUUIDString:@PCAM_STREAM_ID];
    id provider = nil;
    @try {
        // 进程里只能有一个（0x21ccde3a0，第二个抛异常）；mediaserverd 本来一个都没有。
        provider = [[objc_getClass("CMIOExtensionProvider") providerWithSource:provider_source clientQueue:nil] retain];
        id device = [objc_getClass("CMIOExtensionDevice") deviceWithLocalizedName:@"Pagani Test Pattern" deviceID:device_id legacyDeviceID:nil source:device_source];
        gStream = [[objc_getClass("CMIOExtensionStream") streamWithLocalizedName:@"Pagani Test Pattern" streamID:stream_id direction:0 clockType:0 source:stream_source] retain];
        NSError *error = nil;
        if (![device addStream:gStream error:&error] || ![provider addDevice:device error:&error]) {
            os_log_error(OS_LOG_DEFAULT, "PaganiCamera: %{public}@", error);
            return PcamStartProvider;
        }
    } @catch (NSException *exception) {
        os_log_error(OS_LOG_DEFAULT, "PaganiCamera: provider: %{public}@", exception);
        return PcamStartProvider;
    }
    dispatch_queue_t listener_queue = dispatch_queue_create("org.unixnu.pagani.camera.listener", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = xpc_connection_create(NULL, listener_queue);
    if (!listener) return PcamStartListener;
    Class context_class = objc_getClass("CMIOExtensionProviderContext");
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        @autoreleasepool {
            @try {
                id context = [[[context_class alloc] initWithConnection:peer] autorelease];
                if (context) [provider beginProviderContextRegistration:context];
            } @catch (NSException *exception) {
                record_error(PcamStartListener);
                os_log_error(OS_LOG_DEFAULT, "PaganiCamera: connection: %{public}@", exception);
            }
        }
    });
    xpc_connection_activate(listener);
    xpc_endpoint_t endpoint = xpc_endpoint_create(listener);
    if (!endpoint) return PcamStartListener;
    dispatch_async(session_queue, ^{session_register(session, endpoint);});
    return PcamStartOk;
}

int pcam_marker_present(void)
{
    return access(PCAM_MARKER, F_OK) == 0;
}

#ifndef PAGANI_CAMERA_PLUGIN_HOST
// iOS 的 SDK 有库没有头文件。
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owner, uint32_t type, io_connect_t *connect);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *input, size_t input_size, void *output, size_t *output_size);
kern_return_t IOObjectRelease(io_object_t object);

void pcam_kernel_line(const char *line)
{
    static io_connect_t connection;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(PCAM_LOG_SERVICE));
        if (!service) return;
        if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) connection = 0;
        IOObjectRelease(service);
    });
    if (connection) (void)IOConnectCallStructMethod(connection, PCAM_LOG_SELECTOR, line, strnlen(line, PcamLogMax - 1) + 1, NULL, NULL);
}

// 构造函数：厂商类的初始化必须在这里换（dlopen 之后紧接着就是那次 init）；提供者等 dlopen 返回。
__attribute__((constructor)) static void pcam_plugin_load(void)
{
    if (strcmp(getprogname(), PCAM_PROCESS)) return;
    const int vendor = pcam_vendor_install();
    const int marker = pcam_marker_present();
    os_log(OS_LOG_DEFAULT, "PaganiCamera: plug-in loaded, vendor %d, marker %d", vendor, marker);
    if (vendor || !marker) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const int rc = pcam_provider_start();
        char line[PcamLogMax];
        snprintf(line, sizeof line, "provider start rc=%d", rc);
        pcam_kernel_line(line);
    });
}
#endif
