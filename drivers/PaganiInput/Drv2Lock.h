// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared lock of the apps RSC DRV-2 (Adreno830 ARCHITECTURE 4.3.4): the touch rail votes and the A830 GPU driver's
// host votes (Adreno830 platform/sm8750/Sm8750Rpmh.c) use the same ACTIVE TCSes. One 32-bit word, 0 = free, any other
// value = owner; it is defined weak in every user, so the kernel link keeps a single word whichever drivers are in it.
// Acquire swaps 0 for the owner atomically, waiting 10 us between tries for at most 2 s; release stores 0.
#pragma once
#include <stdint.h>

#ifndef SM8750_RPMH_DRV2_OWNER_DEFINED
#define SM8750_RPMH_DRV2_OWNER_DEFINED 1
extern "C" {
__attribute__((weak)) volatile uint32_t sm8750_rpmh_drv2_owner;
}
#endif

enum : uint32_t {
    PaganiTouchDrv2Owner=0x54554348,   // 'TUCH'; the GPU driver uses 'A830' (0x41383330)
    PaganiDrv2StepUs=10,
    PaganiDrv2Steps=200000,            // 2 s
};

// 0 when taken, -16 when the word stayed owned for 2 s (nothing was sent). delay_us is IODelay in the kernel.
static inline int pagani_drv2_acquire(uint32_t owner,void (*delay_us)(unsigned),uint32_t *waits=nullptr) {
    for (uint32_t step=0;;++step) {
        uint32_t expected=0;
        if (__atomic_compare_exchange_n(&sm8750_rpmh_drv2_owner,&expected,owner,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            if (waits) *waits=step;
            return 0;
        }
        if (step>=PaganiDrv2Steps) {if (waits) *waits=step;return -16;}
        delay_us(PaganiDrv2StepUs);
    }
}
static inline void pagani_drv2_release(void) {__atomic_store_n(&sm8750_rpmh_drv2_owner,0u,__ATOMIC_RELEASE);}
