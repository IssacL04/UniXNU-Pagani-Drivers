/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_FIRMWARE_H
#define NATIVE_ADRENO_FIRMWARE_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
struct NativeAdrenoFirmwareRules {
    uint64_t bytes;
    uint8_t sha256[32];
};
struct NativeAdrenoFirmwareView {
    const uint8_t *bytes;
    size_t length;
    uint8_t sha256[32];
};
/* The four gen80000 images of this phone (ARCHITECTURE 4.7.1, 4.9, 4.11), in Adreno830Board order. */
enum NativeAdrenoFirmwareImage : unsigned {
    NativeAdrenoFirmwareGmu,   /* gen80000_gmu.bin  88896 bytes, GMU blocks (A830GmuFirmware) */
    NativeAdrenoFirmwareSqe,   /* gen80000_sqe.fw  111892 bytes, copied from offset 4 to GPU VA 0x100000 */
    NativeAdrenoFirmwareAqe,   /* gen80000_aqe.fw   25340 bytes, copied from offset 4 to GPU VA 0x180000 */
    NativeAdrenoFirmwareZap,   /* gen80000_zap.mbn  12088 bytes, MDT for PAS 13 (A830Zap) */
    NativeAdrenoFirmwareCount
};
/* The pins: exact length and sha256 of each image as shipped on PKX110 (the same values as the device repository's
 * scripts/data/sm8750-gpu-inputs.json). header_bytes is what the CP loader skips (adreno_fw_create_bo copies size-4). */
struct NativeAdrenoFirmwarePin {
    const char *name;
    uint64_t bytes;
    uint8_t sha256[32];
    uint32_t header_bytes;
};
/* Validated view of an in-image firmware blob. The exact byte count is checked before any hashing, then SHA-256 must match.
 * Refusal never returns a view and never copies the image; the caller keeps ownership and lifetime. This proves the bytes
 * handed to a loader are the pinned ones; it does not prove the payload is loadable. */
class NativeAdrenoFirmware final {
    NativeAdrenoFirmware()=delete;
public:
    static IOReturn validate(const uint8_t *image,uint64_t length,
        const NativeAdrenoFirmwareRules *rules,NativeAdrenoFirmwareView *view);
    /* The driver's pin of one image; null for an index out of range. */
    static const NativeAdrenoFirmwarePin *pin(unsigned image);
    /* The pin as validation rules (kIOReturnBadArgument for an unknown image). */
    static IOReturn pinnedRules(unsigned image,NativeAdrenoFirmwareRules *rules);
    /* validate() against the driver's own pin of that image. */
    static IOReturn validatePinned(unsigned image,const uint8_t *bytes,uint64_t length,NativeAdrenoFirmwareView *view);
    /* True when board rules (length and sha256 the board was generated with) equal the driver's pin of that image: a board
     * built for other firmware is refused in S0 before any image is used. */
    static bool matchesPin(unsigned image,const NativeAdrenoFirmwareRules &rules);
};
#endif
