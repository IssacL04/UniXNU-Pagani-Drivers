/* SPDX-License-Identifier: MIT */
#ifndef A830_PROFILE_H
#define A830_PROFILE_H
#include <stdint.h>

/* Bring-up profile, boot-arg pagani-gpu-profile (Adreno830Start sets it before S1):
 *  - A830ProfileAnnibale (1, default): the sequences and values the REDMI K90 (annibale, same SM8750/A830) port
 *    proved on hardware under XNU, where they differ from the design (docs/ARCHITECTURE.md section 11 and
 *    the annibale table); 13T measurements still win (power-up, PLL, host rail and DDR votes, SCM init).
 *  - A830ProfileDesign (0): the design defaults derived from Linux/KGSL.
 * Every module reads it at run time through adreno830_profile(); host tests set adreno830_profile_value. */
enum {A830ProfileDesign = 0, A830ProfileAnnibale = 1, A830ProfileCount = 2};

#ifdef __cplusplus
extern "C" {
#endif
/* Weak so that every translation unit may define it; the linker keeps one. */
__attribute__((weak)) volatile uint32_t adreno830_profile_value = A830ProfileAnnibale;
#ifdef __cplusplus
}
#endif

static inline uint32_t adreno830_profile(void)
{
    return adreno830_profile_value;
}
static inline int adreno830_annibale(void)
{
    return adreno830_profile() == A830ProfileAnnibale;
}
#endif
