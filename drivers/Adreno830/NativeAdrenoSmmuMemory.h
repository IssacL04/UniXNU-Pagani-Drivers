/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SMMU_MEMORY_H
#define NATIVE_ADRENO_SMMU_MEMORY_H
#include "NativeAdrenoMemory.h"
#include "ArmSmmu4KLive.h"
/* Bind the actual contiguous raw-PA backing allocation to an INACTIVE native
 * arena, publish its used table pages and take a device-lifetime reference.
 * Caller retains memory/arena/state/visibility under an exclusive resource
 * gate; all outputs and the backing pool are disjoint. State starts zeroed.
 * Never invoke on an active or foreign table pool. This does not program a
 * context, route streams, allocate MMIO/IRQs or establish power/security.
 * On success the memory reference remains held until real masters are stopped,
 * streams faulted, context disabled and native TLB synchronization completes.
 * On prepare/publication failure no context has been exposed; caller retains
 * allocation and poisoned state and handles cleanup. No automatic replay.
 * The table walk of the A830 SMMU is coherent (IDR0.CTTW, DT dma-coherent, inner write-back walk attributes): the pages are
 * still cleaned to PoC here and on every change until the S8 self-test V4 proved it (ARCHITECTURE 4.6.1, 4.14.2). */
IOReturn NativeAdrenoPrepareSmmuMemory(NativeAdrenoMemory *,unsigned input_bits,
    unsigned output_bits,uint64_t iova,uint64_t bytes,unsigned upper_region,
    size_t leaf_budget,ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *,ArmSmmu4KLiveVisibility *);
/* Bind only the native PoC publisher to an already retained contiguous table
 * allocation. No caller-supplied success callback or page-table copy. */
IOReturn NativeAdrenoBindSmmuVisibility(NativeAdrenoMemory *,ArmSmmu4KLiveVisibility *);
/* M10: the same binding for a proved coherent table walk: a changed page takes only dsb ishst (NativeAdrenoStoreBarrier), the
 * page's bounds are checked as for the PoC publisher. */
IOReturn NativeAdrenoBindSmmuVisibilityCoherent(NativeAdrenoMemory *,ArmSmmu4KLiveVisibility *);
/* One zeroed 4 KiB table page that translates nothing (the TTBR1 guard of CB0, ARCHITECTURE 4.5.4 step 9): allocated physically
 * below 2^output_bits, cleaned to PoC, held for the device for the context's life. */
IOReturn NativeAdrenoPrepareGuardTable(NativeAdrenoMemory *,unsigned output_bits);
#endif
