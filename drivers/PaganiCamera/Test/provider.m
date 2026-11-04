// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// ios/CamProvider.m 的主机测试。提供者一侧和会话一侧都是这台 Mac 上真的 CoreMediaIO：CMIOExtensionProvider、CMIOExtensionProviderContext、
// CMIOExtensionSessionProvider、CMIOExtensionSessionStream，帧真的走一圈匿名 XPC 回到本进程。mac 上没有 CMIOExtensionDiscoverySession 和
// BWFigCaptureDeviceVendor，这里按 21H461 的实例变量和行为（CamProvider.m 开头写的地址）写替身：厂商类的 init 在没有默认函数、又不在虚拟机里时
// 返回 nil，deviceIDs 按 _defaultDeviceCreateFunction 决定有没有 Default。
//
// 构建三种：普通的；-DNO_CLASS（少一个类：什么都不建）；-DNO_VENDOR（没有厂商类：什么都不替换）。
// 不能证明的：21H461 的 CoreMediaIO 和 mac 27 的版本有差别（接口逐个核对过，行为只有上机才算数），mediaserverd 的沙盒，后面的 CMCapture 管线。
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceObjC.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include "check.h"
#include "../ios/CamPattern.h"
#include "../ios/CamProvider.h"

static char gLast[PcamLogMax];
static unsigned gLines;
void pcam_kernel_line(const char *line)
{
    snprintf(gLast, sizeof gLast, "%s", line);
    gLines++;
}

// ---- 替身 ----
static int gCreateCalls;
// 21H461 的 CMIOExtensionSessionProvider 有 bundleID 属性（0x21ccf6ed8），mac 的没有：发现会话是 iOS 才有的。
static id gBundleID;
static void set_bundle_id(id self, SEL cmd, id value)
{
    (void)self;
    (void)cmd;
    gBundleID = [value retain];
}
static int cmio_create(void) {return 0;}
static int default_create(void) {return 1;}

#ifndef NO_VENDOR
@interface BWFigCaptureDeviceVendor : NSObject {
@public
    void *_defaultDeviceCreateFunction;
    void *_cmioExtensionDeviceCreateFunction;
}
- (id)initWithDefaultDeviceCreateFunction:(void *)create cmioExtensionDeviceCreateFunction:(void *)cmio cmioDiscoverySession:(id)session;
- (NSArray *)deviceIDs;
@end
@implementation BWFigCaptureDeviceVendor
- (id)initWithDefaultDeviceCreateFunction:(void *)create cmioExtensionDeviceCreateFunction:(void *)cmio cmioDiscoverySession:(id)session
{
    (void)session;
    self = [super init];
    gCreateCalls++;
    if (!create && !cmio) {[self release]; return nil;}
    if (!create) {[self release]; return nil;}      // 0x1a693fde0：不在虚拟机里
    _defaultDeviceCreateFunction = create;
    _cmioExtensionDeviceCreateFunction = cmio;
    return self;
}
- (NSArray *)deviceIDs
{
    return _defaultDeviceCreateFunction ? @[@"Default"] : @[];
}
@end
#endif

#ifndef NO_CLASS
@interface CMIOExtensionDiscoverySession : NSObject {
@public
    dispatch_queue_t _queue;
    NSMutableDictionary *_providersByBundleIdentifier;
    dispatch_semaphore_t _devices;
    int _failed;
}
+ (id)sharedInstance;
@end
@implementation CMIOExtensionDiscoverySession
+ (id)sharedInstance
{
    static CMIOExtensionDiscoverySession *session;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        session = [[CMIOExtensionDiscoverySession alloc] init];
        session->_queue = dispatch_queue_create("discovery", DISPATCH_QUEUE_SERIAL);
        session->_providersByBundleIdentifier = [[NSMutableDictionary alloc] init];
        session->_devices = dispatch_semaphore_create(0);
    });
    return session;
}
- (id)copyProviderBundleID {return [@PCAM_BUNDLE copy];}
- (void)provider:(id)provider availableDevicesChanged:(id)changed {(void)provider; (void)changed; dispatch_semaphore_signal(_devices);}
- (void)provider:(id)provider didFailWithError:(id)error {(void)provider; (void)error; _failed = 1;}
- (void)provider:(id)provider initializeDevices:(id)devices properties:(id)properties {(void)provider; (void)devices; (void)properties;}
- (void)provider:(id)provider propertiesChanged:(id)changed {(void)provider; (void)changed;}
- (void)providerHasBeenInvalidated:(id)invalidated {(void)invalidated; _failed = 2;}
- (void)postDeviceChangeNotification {}
@end
#endif

@interface NSObject (TestSession)
- (NSArray *)devices;
- (NSArray *)streams;
- (NSArray *)formats;
- (NSUUID *)deviceID;
- (NSString *)localizedName;
- (NSString *)manufacturer;
- (NSString *)modelID;
- (void)startStream:(void (^)(NSError *))reply;
- (void)stopStream:(void (^)(NSError *))reply;
- (void)setActiveFormatIndex:(unsigned long long)index reply:(void (^)(NSError *))reply;
- (unsigned long long)activeFormatIndex;
- (CMFormatDescriptionRef)formatDescription;
@end

// ---- 收帧 ----
@interface Sink : NSObject {
@public
    dispatch_semaphore_t frames;
    unsigned received, iosurface, nv12, order_errors, qr;
    int width, height;
    uint32_t last_stamp;
    int have_stamp;
}
@end
@implementation Sink
- (void)stream:(id)stream propertiesChanged:(id)changed {(void)stream; (void)changed;}
- (void)stream:(id)stream scheduledOutputChanged:(id)output {(void)stream; (void)output;}
- (void)stream:(id)stream activeFormatIndexChanged:(unsigned long long)index {(void)stream; (void)index;}
- (void)streamHasBeenInvalidated:(id)stream {(void)stream;}
- (CMSampleBufferRef)stream:(id)stream copySampleBuffer:(id)sample error:(id *)error {(void)stream; (void)sample; (void)error; return NULL;}
- (void)stream:(id)stream receivedSampleBuffer:(CMSampleBufferRef)sample
{
    (void)stream;
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (!image) return;
    received++;
    if (CVPixelBufferGetIOSurface(image)) iosurface++;
    if (CVPixelBufferGetPixelFormatType(image) == PCAM_PIXEL_FORMAT) nv12++;
    width = (int)CVPixelBufferGetWidth(image);
    height = (int)CVPixelBufferGetHeight(image);
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint32_t stamp = pcam_pattern_stamp(CVPixelBufferGetBaseAddressOfPlane(image, 0), CVPixelBufferGetBytesPerRowOfPlane(image, 0));
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    if (have_stamp && stamp <= last_stamp) order_errors++;
    last_stamp = stamp;
    have_stamp = 1;
    if (received % 8 == 1) {
        CIDetector *detector = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil options:@{CIDetectorAccuracy: CIDetectorAccuracyHigh}];
        NSArray *found = [detector featuresInImage:[CIImage imageWithCVImageBuffer:image]];
        if (found.count == 1 && [((CIQRCodeFeature *)found[0]).messageString isEqualToString:@PCAM_QR_TEXT]) qr++;
    }
    dispatch_semaphore_signal(frames);
}
@end

static void wait_reply(void (^call)(void (^)(NSError *)))
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    call(^(NSError *error) {
        failure = [error retain];
        dispatch_semaphore_signal(done);
    });
    CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    if (failure) fprintf(stderr, "reply error: %s\n", failure.description.UTF8String);
    CHECK(failure == nil);
}

static void wait_frames(Sink *sink, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) CHECK(dispatch_semaphore_wait(sink->frames, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0);
}

#if !defined(NO_CLASS) && !defined(NO_VENDOR)
static void vendor(void)
{
    /* 不替换：和手机一样是 nil */
    id before = [[BWFigCaptureDeviceVendor alloc] initWithDefaultDeviceCreateFunction:NULL cmioExtensionDeviceCreateFunction:(void *)cmio_create cmioDiscoverySession:nil];
    CHECK(before == nil);
    CHECK(pcam_vendor_install() == PcamVendorInstalled);
    CHECK(pcam_vendor_install() == PcamVendorInstalled);
    /* 替换后：活着，默认函数清回空，deviceIDs 没有 Default */
    BWFigCaptureDeviceVendor *alive = [[BWFigCaptureDeviceVendor alloc] initWithDefaultDeviceCreateFunction:NULL cmioExtensionDeviceCreateFunction:(void *)cmio_create cmioDiscoverySession:nil];
    CHECK(alive != nil && alive->_defaultDeviceCreateFunction == NULL && alive->_cmioExtensionDeviceCreateFunction == (void *)cmio_create);
    CHECK(alive.deviceIDs.count == 0);
    [alive release];
    /* 真有默认函数：原样 */
    BWFigCaptureDeviceVendor *real = [[BWFigCaptureDeviceVendor alloc] initWithDefaultDeviceCreateFunction:(void *)default_create cmioExtensionDeviceCreateFunction:(void *)cmio_create cmioDiscoverySession:nil];
    CHECK(real != nil && real->_defaultDeviceCreateFunction == (void *)default_create);
    [real release];
    /* 两个都没有：原样失败，不代它撑着 */
    CHECK([[BWFigCaptureDeviceVendor alloc] initWithDefaultDeviceCreateFunction:NULL cmioExtensionDeviceCreateFunction:NULL cmioDiscoverySession:nil] == nil);
}

static void flow(void)
{
    CMIOExtensionDiscoverySession *session = [CMIOExtensionDiscoverySession sharedInstance];
    /* 这台 Mac 没有 setBundleID:：接口检查要拦住 */
    CHECK(pcam_provider_start() == PcamStartNoSelector);
    CHECK(class_addMethod(objc_getClass("CMIOExtensionSessionProvider"), sel_registerName("setBundleID:"), (IMP)set_bundle_id, "v@:@"));
    CHECK(pcam_provider_start() == PcamStartOk);
    CHECK(pcam_provider_start() == PcamStartAgain);
    CHECK(dispatch_semaphore_wait(session->_devices, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    __block id provider = nil;
    dispatch_sync(session->_queue, ^{provider = [session->_providersByBundleIdentifier[@PCAM_BUNDLE] retain];});
    CHECK(provider != nil && [gBundleID isEqualToString:@PCAM_BUNDLE]);
    NSArray *devices = [provider devices];
    CHECK(devices.count == 1);
    id device = devices[0];
    CHECK([[[device deviceID] UUIDString] isEqualToString:@PCAM_DEVICE_ID]);
    CHECK([[device localizedName] isEqualToString:@"Pagani Test Pattern"]);
    CHECK([[device manufacturer] isEqualToString:@"UniXNU"]);       /* BWFigVideoCaptureDevice 外接时强制读这几个 */
    CHECK([[device modelID] isEqualToString:@"PaganiTestPattern"]);
    NSArray *streams = [device streams];
    CHECK(streams.count == 1);
    id stream = streams[0];
    CHECK([stream formats].count == PcamModes);
    for (unsigned i = 0; i < PcamModes; ++i) {
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions([[stream formats][i] formatDescription]);
        CHECK(size.width == (i ? 1280 : 1920) && size.height == (i ? 720 : 1080));
        CHECK(CMFormatDescriptionGetMediaSubType([[stream formats][i] formatDescription]) == PCAM_PIXEL_FORMAT);
    }
    CHECK([stream activeFormatIndex] == 1);
    Sink *sink = [[Sink alloc] init];
    sink->frames = dispatch_semaphore_create(0);
    ((void (*)(id, SEL, id))objc_msgSend)(stream, sel_registerName("setDelegate:"), sink);
    wait_reply(^(void (^reply)(NSError *)) {[stream startStream:reply];});
    wait_frames(sink, 30);
    CHECK(sink->width == 1280 && sink->height == 720);
    /* 切到 1080p */
    wait_reply(^(void (^reply)(NSError *)) {[stream setActiveFormatIndex:0 reply:reply];});
    usleep(200000);
    while (dispatch_semaphore_wait(sink->frames, DISPATCH_TIME_NOW) == 0) {}
    wait_frames(sink, 15);
    CHECK(sink->width == 1920 && sink->height == 1080);
    wait_reply(^(void (^reply)(NSError *)) {[stream stopStream:reply];});
    usleep(100000);
    while (dispatch_semaphore_wait(sink->frames, DISPATCH_TIME_NOW) == 0) {}
    const unsigned stopped_at = sink->received;
    usleep(300000);
    CHECK(sink->received == stopped_at);
    const int32_t sent = pcam_status[4];
    usleep(200000);
    CHECK(pcam_status[4] == sent);                                  /* 停了就真停：定时器不留，不是光没人收 */
    /* 再开一次 */
    wait_reply(^(void (^reply)(NSError *)) {[stream startStream:reply];});
    wait_frames(sink, 10);
    wait_reply(^(void (^reply)(NSError *)) {[stream stopStream:reply];});
    CHECK(sink->iosurface == sink->received);                       /* 帧靠 IOSurface 过 XPC */
    CHECK(sink->nv12 == sink->received);
    CHECK(sink->order_errors == 0);
    CHECK(sink->qr >= 4);                                           /* 回到这一头的帧里读得出二维码 */
    CHECK(pcam_status[0] >= 1 && pcam_status[2] == 2 && pcam_status[3] == 2 && pcam_status[5] == 0 && pcam_status[6] == 0);
    CHECK(pcam_status[4] >= 55);
    CHECK(session->_failed == 0);
    CHECK(gLines >= 3 && strcmp(gLast, "stream stop") == 0);
    printf("  (%u frames over XPC, %u QR reads)\n", sink->received, sink->qr);
}
#endif

int main(void)
{
    @autoreleasepool {
        CHECK(!pcam_marker_present());          /* 这台 Mac 上没有 */
#if defined(NO_VENDOR)
        CHECK(pcam_vendor_install() == PcamVendorNoClass);
        printf("PASS provider-no-vendor: %u checks\n", checks);
#elif defined(NO_CLASS)
        CHECK(class_addMethod(objc_getClass("CMIOExtensionSessionProvider"), sel_registerName("setBundleID:"), (IMP)set_bundle_id, "v@:@"));
        CHECK(pcam_provider_start() == PcamStartNoClass);
        CHECK(pcam_status[4] == 0);
        printf("PASS provider-no-class: %u checks\n", checks);
#else
        vendor();
        flow();
        printf("PASS provider: %u checks\n", checks);
#endif
    }
    return 0;
}
