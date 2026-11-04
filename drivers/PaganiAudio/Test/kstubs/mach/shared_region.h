// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host stand-in (Test/glue.cpp) for xnu-10063 osfmk/mach/shared_region.h: the arm64 shared region of the kernel the service runs on
// (the macOS SDK's header has a larger region and must not decide the test).
#ifndef PAGANI_AUDIO_KSTUB_SHARED_REGION_H
#define PAGANI_AUDIO_KSTUB_SHARED_REGION_H
#define SHARED_REGION_BASE_ARM64 0x180000000ULL
#define SHARED_REGION_SIZE_ARM64 0x180000000ULL
#endif
