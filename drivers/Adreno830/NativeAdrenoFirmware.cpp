/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoSha256.h"
#include <string.h>

namespace {
/* The gen80000 images of PKX110 (一加13T原厂备份/GPU固件; device repository scripts/data/sm8750-gpu-inputs.json). The images
 * themselves are proprietary and never in this repository; the board embeds them and S0 checks them against these pins. */
const NativeAdrenoFirmwarePin pins[NativeAdrenoFirmwareCount]={
    {"gen80000_gmu.bin",88896,
     {0x1a,0xd8,0x17,0x5e,0x6c,0xd0,0x1e,0xa7,0x6c,0x64,0xd2,0x0b,0x83,0x60,0x00,0x38,
      0xc5,0x2d,0xd2,0x98,0x44,0xb4,0x8d,0x57,0xb1,0x1b,0x76,0x9b,0x03,0x88,0x63,0xa5},0},
    {"gen80000_sqe.fw",111892,
     {0x9c,0x8f,0x00,0x9b,0xea,0x11,0x2d,0x7c,0x06,0xdd,0x2c,0xcb,0x67,0xe8,0x89,0x99,
      0x94,0x59,0xe5,0x3b,0x92,0xf0,0xf4,0x85,0x9f,0x5e,0xa0,0xdb,0x06,0x7a,0xd6,0xb5},4},
    {"gen80000_aqe.fw",25340,
     {0xf0,0x4c,0xb9,0x7e,0x1e,0x62,0x9b,0x90,0x03,0xa2,0x6c,0x56,0x70,0xb8,0xf7,0xe5,
      0xb7,0xbf,0xec,0xa9,0x5d,0x35,0xb2,0xab,0xd5,0x81,0xfd,0x86,0x72,0xef,0xed,0x20},4},
    {"gen80000_zap.mbn",12088,
     {0x1a,0xa7,0xaf,0x12,0x0c,0x4e,0xda,0x02,0xa3,0x72,0xc6,0xb1,0x93,0xf4,0xea,0xe9,
      0x27,0x2d,0x92,0xe2,0x49,0x43,0x00,0xa1,0xb9,0xbb,0x0d,0x5a,0xd0,0xd9,0x3c,0x43},0},
};
}

IOReturn NativeAdrenoFirmware::validate(const uint8_t *image,uint64_t length,
    const NativeAdrenoFirmwareRules *rules,NativeAdrenoFirmwareView *view)
{
    if(!view)return kIOReturnBadArgument;
    *view={};
    if(!rules || !image || !length || length!=rules->bytes ||
        length>uint64_t(SIZE_MAX))return kIOReturnBadArgument;
    uint8_t digest[32];
    NativeAdrenoSha256::hash(image,size_t(length),digest);
    if(memcmp(digest,rules->sha256,32))return kIOReturnIOError;
    view->bytes=image;view->length=size_t(length);
    memcpy(view->sha256,digest,32);
    return kIOReturnSuccess;
}
const NativeAdrenoFirmwarePin *NativeAdrenoFirmware::pin(unsigned image)
{
    return image<NativeAdrenoFirmwareCount?&pins[image]:nullptr;
}
IOReturn NativeAdrenoFirmware::pinnedRules(unsigned image,NativeAdrenoFirmwareRules *rules)
{
    const NativeAdrenoFirmwarePin *pinned=pin(image);
    if(!pinned || !rules)return kIOReturnBadArgument;
    rules->bytes=pinned->bytes;
    memcpy(rules->sha256,pinned->sha256,32);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoFirmware::validatePinned(unsigned image,const uint8_t *bytes,uint64_t length,NativeAdrenoFirmwareView *view)
{
    NativeAdrenoFirmwareRules rules;
    if(view)*view={};
    if(pinnedRules(image,&rules))return kIOReturnBadArgument;
    return validate(bytes,length,&rules,view);
}
bool NativeAdrenoFirmware::matchesPin(unsigned image,const NativeAdrenoFirmwareRules &rules)
{
    const NativeAdrenoFirmwarePin *pinned=pin(image);
    return pinned && rules.bytes==pinned->bytes && !memcmp(rules.sha256,pinned->sha256,32);
}
