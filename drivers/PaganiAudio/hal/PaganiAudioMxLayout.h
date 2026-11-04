// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Where the plug-in looks inside the audio server for MediaExperience's progress and the 'srzl' repair (hal/PaganiAudioPlugIn.c, the
// "MediaExperience" section; route-re 6 R1/R2) and for VirtualAudio's actuator settings: addresses of the 21H461 dyld cache (arm64, unslid)
// and VirtualAudio.plugin's image offsets, with the code bytes that must match before any of them is used (the image offsets: its UUID;
// scripts/check-pagani-audio-host.py compares them with the binary where it is at hand). The plug-in's layout is 21H461's; Test/plugin.c points
// pagani_audio_mx_layout at one of its own (fake globals and functions in the test binary). Hidden symbols: not exported by the bundle.
#ifndef PAGANI_AUDIO_MX_LAYOUT_H
#define PAGANI_AUDIO_MX_LAYOUT_H
#include <stdint.h>
#include <mach-o/loader.h>

typedef struct {
    uint64_t initialize;              /* MXInitialize (exported; dlsym gives the slid address, the difference is the cache's slide) */
    uint8_t initialize_code[16];
    uint64_t once, session, screen, ids, routes, context, endpoint, va_plugin, serial_done, queue;   /* PaganiAudioMx bits 0..10 */
    uint64_t serial_listener;         /* vaemVADSerializationListener */
    uint8_t serial_code[32];
    uint64_t receiver_listener;       /* vaemReceiverWillBeUsedListener */
    uint8_t receiver_code[12];
    uint8_t va_uuid[16];              /* VirtualAudio.plugin's LC_UUID */
    uint64_t va_device_manager;       /* its DeviceManager pointer (image offset; the image's __TEXT starts at 0) */
    uint32_t va_listener, va_filter;  /* DeviceListener at the manager + va_listener, its filter mode at + va_filter */
    /* ActuatorSettingsFactory_Aspen's static, image offsets: the guard of its first use, the word that says "no I/V sense" and the format
     * word its product table sets for such a product; va_product: MGGetProductType of the product the table refuses (option VaSettings). */
    uint64_t va_settings_guard, va_settings_kind, va_settings_format, va_product;
} PaganiAudioMxLayout;

__attribute__((visibility("hidden"))) extern const PaganiAudioMxLayout *pagani_audio_mx_layout;
/* For the host test: the routing layer's image as dyld would report it, and a fresh boot of the repair's once-per-boot state. */
__attribute__((visibility("hidden"))) void pagani_audio_va_image(const struct mach_header_64 *header, intptr_t slide);
__attribute__((visibility("hidden"))) void pagani_audio_mx_reset(void);
#endif
