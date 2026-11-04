/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What ios/TorchPlugin.m shares with its host test. The names are TorchIos.h's (scripts/build-pagani-torch-plugin.py checks they match). */
#ifndef PAGANI_TORCH_PLUGIN_H
#define PAGANI_TORCH_PLUGIN_H
#include <stdint.h>

#define PGT_PLUGIN_SERVICE     "AppleH6CamIn"
#define PGT_PLUGIN_PROCESS     "mediaserverd"
#define PGT_PLUGIN_SELECTOR    0u
#ifndef PGT_PLUGIN_KEY_SYMBOL
#define PGT_PLUGIN_KEY_SYMBOL  "kFigCaptureStreamProperty_TorchLevel"    /* exported by CMCapture */
#endif
enum { PGT_PLUGIN_UNSUPPORTED = -12787, PGT_PLUGIN_FAILED = -12780 };   /* kCMBaseObjectError_ValueNotAvailable family: any nonzero fails the call */

/* FigFlashlight's level, 0.0 to 1.0, as the kernel's 0..1000: anything above 0 is at least 1. */
uint32_t pgt_plugin_level(float value);
/* Replaces the vendor's six methods, all or none. 0, or negative: nothing replaced. */
int pgt_plugin_install(void);
/* The kernel's user client (the host test provides its own). 0, or negative. */
int pgt_plugin_kernel_set(uint32_t level);
#endif
