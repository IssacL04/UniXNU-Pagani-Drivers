// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 测试图的主机测试。二维码的判据不是和一份自己算的表比，而是让 macOS 的 CoreImage（CIDetector）和 Vision（代码扫描器用的同一套检测）从画好的
// NV12 帧里把文本读回来：滚动的每个相位、两种分辨率、缩小到 640 宽（预览的量级）。RS 的判据是独立的：码字代入生成多项式的每个根，伴随式全 0。
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#include "check.h"
#include "../ios/CamPattern.h"

static uint8_t mul(uint8_t a, uint8_t b)
{
    unsigned r = 0, x = a;
    for (int i = 0; i < 8; ++i) {
        if (b & (1 << i)) r ^= x;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    return (uint8_t)r;
}

static void rs_roots(void)
{
    uint8_t data[PcamQrDataWords], ecc[PcamQrEcWords];
    for (unsigned seed = 1; seed < 40; ++seed) {
        for (unsigned i = 0; i < PcamQrDataWords; ++i) data[i] = (uint8_t)(i * seed * 37 + seed);
        pcam_qr_ecc(data, ecc);
        uint8_t root = 1;
        for (unsigned k = 0; k < PcamQrEcWords; ++k) {
            uint8_t sum = 0;
            for (unsigned i = 0; i < PcamQrDataWords; ++i) sum = (uint8_t)(mul(sum, root) ^ data[i]);
            for (unsigned i = 0; i < PcamQrEcWords; ++i) sum = (uint8_t)(mul(sum, root) ^ ecc[i]);
            CHECK(sum == 0);
            root = mul(root, 2);
        }
    }
}

/* ISO/IEC 18004 表 C.1：纠错 M、掩码 0..7 的 15 位格式信息。这个表和解码器无关：单比特错 BCH 能纠回来，CoreImage 看不出来。 */
static const uint16_t kFormat[8] = {0x5412, 0x5125, 0x5e7c, 0x5b4b, 0x45f9, 0x40ce, 0x4f97, 0x4aa0};

static unsigned format_read(const PcamQr *qr)
{
    unsigned bits = 0;
    for (int i = 0; i <= 5; ++i) bits |= (unsigned)qr->module[i][8] << i;
    bits |= (unsigned)qr->module[7][8] << 6;
    bits |= (unsigned)qr->module[8][8] << 7;
    bits |= (unsigned)qr->module[8][7] << 8;
    for (int i = 9; i < 15; ++i) bits |= (unsigned)qr->module[8][14 - i] << i;
    return bits;
}

static unsigned format_read_second(const PcamQr *qr)
{
    unsigned bits = 0;
    for (int i = 0; i < 8; ++i) bits |= (unsigned)qr->module[8][PcamQrSize - 1 - i] << i;
    for (int i = 8; i < 15; ++i) bits |= (unsigned)qr->module[PcamQrSize - 15 + i][8] << i;
    return bits;
}

static void structure(void)
{
    PcamQr qr;
    CHECK(pcam_qr_encode("x", 0, 0, &qr) != 0);
    CHECK(pcam_qr_encode(NULL, 1, 0, &qr) != 0);
    CHECK(pcam_qr_encode("x", 1, 8, &qr) != 0);
    char big[PcamQrMaxText + 2];
    memset(big, 'a', sizeof big);
    CHECK(pcam_qr_encode(big, PcamQrMaxText + 1, 0, &qr) != 0);
    for (unsigned mask = 0; mask < 8; ++mask) {
        CHECK(pcam_qr_encode(big, PcamQrMaxText, mask, &qr) == 0);
        /* 三个定位图形的中心和定时列 */
        CHECK(qr.module[3][3] && qr.module[3][PcamQrSize - 4] && qr.module[PcamQrSize - 4][3]);
        CHECK(!qr.module[1][1] && !qr.module[7][7]);
        CHECK(qr.module[6][8] && !qr.module[6][9] && qr.module[8][6] && !qr.module[9][6]);
        CHECK(qr.module[22][22] && !qr.module[21][21] && qr.module[20][20]);
        CHECK(qr.module[PcamQrSize - 8][8]);
        CHECK(format_read(&qr) == kFormat[mask] && format_read_second(&qr) == kFormat[mask]);
    }
}

static CVPixelBufferRef frame_buffer(const PcamPattern *p, uint32_t frame)
{
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    CVPixelBufferRef buffer = NULL;
    CHECK(CVPixelBufferCreate(kCFAllocatorDefault, p->width, p->height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                              (__bridge CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess);
    CVPixelBufferLockBaseAddress(buffer, 0);
    pcam_pattern_fill(p, frame, CVPixelBufferGetBaseAddressOfPlane(buffer, 0), CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
                      CVPixelBufferGetBaseAddressOfPlane(buffer, 1), CVPixelBufferGetBytesPerRowOfPlane(buffer, 1));
    CHECK(pcam_pattern_stamp(CVPixelBufferGetBaseAddressOfPlane(buffer, 0), CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)) == frame);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

static void decodes(CIImage *image, CVPixelBufferRef buffer)
{
    static CIDetector *detector;
    if (!detector) detector = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil options:@{CIDetectorAccuracy: CIDetectorAccuracyHigh}];
    NSArray *found = [detector featuresInImage:image];
    CHECK(found.count == 1);
    CHECK([((CIQRCodeFeature *)found[0]).messageString isEqualToString:@PCAM_QR_TEXT]);
    VNDetectBarcodesRequest *request = [[VNDetectBarcodesRequest alloc] init];
    request.symbologies = @[VNBarcodeSymbologyQR];
    VNImageRequestHandler *handler = buffer ? [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buffer options:@{}]
                                            : [[VNImageRequestHandler alloc] initWithCIImage:image options:@{}];
    NSError *error = nil;
    CHECK([handler performRequests:@[request] error:&error]);
    CHECK(request.results.count == 1);
    CHECK([request.results[0].payloadStringValue isEqualToString:@PCAM_QR_TEXT]);
}

static void frames(unsigned width, unsigned height)
{
    PcamPattern p;
    CHECK(pcam_pattern_init(&p, width, height) == 0);
    CHECK(!(p.tile_left & 1) && !(p.tile_top & 1));
    CHECK(p.tile_left + p.tile <= width && p.tile_top + p.tile <= height);
    /* 帧号条不能压到二维码的静区 */
    CHECK(p.tile_top >= PcamStampCell || p.tile_left >= PcamStampBits * PcamStampCell);
    const uint32_t step = width / PcamScrollPixels / 7 + 1;
    for (uint32_t frame = 0; frame < 8 * step; frame += step) {
        CVPixelBufferRef buffer = frame_buffer(&p, frame);
        CIImage *image = [CIImage imageWithCVPixelBuffer:buffer];
        decodes(image, buffer);
        /* 预览的量级：缩到 640 宽 */
        const CGFloat scale = 640.0 / width;
        decodes([image imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)], NULL);
        CVPixelBufferRelease(buffer);
    }
    /* 二维码下面的色度是中性：白底不发彩 */
    {
        CVPixelBufferRef buffer = frame_buffer(&p, 3);
        CVPixelBufferLockBaseAddress(buffer, 0);
        const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
        const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
        for (unsigned r = 0; r < p.tile / 2; ++r)
            for (unsigned k = 0; k < p.tile; ++k) CHECK(uv[(p.tile_top / 2 + r) * stride + p.tile_left + k] == 128);
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        CVPixelBufferRelease(buffer);
    }
    /* 相邻两帧确实不同：滚动在动 */
    CVPixelBufferRef a = frame_buffer(&p, 100), b = frame_buffer(&p, 101);
    CVPixelBufferLockBaseAddress(a, 0);
    CVPixelBufferLockBaseAddress(b, 0);
    const size_t row = CVPixelBufferGetBytesPerRowOfPlane(a, 0) * (height - 2);
    CHECK(memcmp((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(a, 0) + row, (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(b, 0) + row, width) != 0);
    CVPixelBufferUnlockBaseAddress(a, 0);
    CVPixelBufferUnlockBaseAddress(b, 0);
    CVPixelBufferRelease(a);
    CVPixelBufferRelease(b);
    pcam_pattern_free(&p);
}

int main(void)
{
    @autoreleasepool {
        rs_roots();
        structure();
        PcamPattern p;
        CHECK(pcam_pattern_init(&p, 63, 480) != 0);
        CHECK(pcam_pattern_init(&p, 641, 480) != 0);
        CHECK(pcam_pattern_init(&p, 640, 481) != 0);
        CHECK(pcam_pattern_init(NULL, 640, 480) != 0);
        frames(1920, 1080);
        frames(1280, 720);
        printf("PASS pattern: %u checks\n", checks);
    }
    return 0;
}
