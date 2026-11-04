/* SPDX-License-Identifier: MIT */
/* Empty weak defaults for the four embedded gen80000 images (ARCHITECTURE 2.2, 3.1). The kernel is linked statically, where
 * an undefined weak reference is an error, so an image without the board's firmware archive still needs a definition. The
 * device repository's strong definitions (build-pagani-gpu.py appends them to libAdreno830.a) replace these. A zero length
 * is refused by NativeAdrenoFirmware::validate in S0, so nothing reaches the GMU, the CP or TrustZone without real firmware:
 *   native_adreno_firmware_image      gen80000_gmu.bin
 *   native_adreno_sqe_firmware_image  gen80000_sqe.fw
 *   native_adreno_aqe_firmware_image  gen80000_aqe.fw
 *   native_adreno_zap_image           gen80000_zap.mbn */
#include <stdint.h>
extern "C" __attribute__((weak)) const uint8_t native_adreno_firmware_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_firmware_image_size=0;
extern "C" __attribute__((weak)) const uint8_t native_adreno_sqe_firmware_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_sqe_firmware_image_size=0;
extern "C" __attribute__((weak)) const uint8_t native_adreno_aqe_firmware_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_aqe_firmware_image_size=0;
extern "C" __attribute__((weak)) const uint8_t native_adreno_zap_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_zap_image_size=0;
