/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PWRUP_H
#define NATIVE_ADRENO_PWRUP_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* The power-up register list CP_ME_INIT points the CP at (ARCHITECTURE 4.9, 4.11 step 10, 4.17 P2). The layout is Linux
 * struct cpu_gpu_lock: three u32 lock words (gpu_req, cpu_req, turn; written 0), {u8 ifpc_list_len, u8 preemption_list_len,
 * u16 dynamic_list_len}, then {offset, value} pairs of the IFPC list, pairs of the IFPC+preemption list, and the dynamic list
 * of 12-byte triplets {aperture = pipe << 12, offset, value} (Gen8 concurrent binning: the CP selects the pipe before it
 * restores the register, which CP_ME_INIT announces with BIT(31) of its last word). Values must come from register reads
 * at bring-up (A830Cp::patchPwrup, the a8xx_patch_pwrup_reglist equivalent); these functions never read MMIO and never
 * invent a value. Capacity and count limits are checked before any byte is written. */
struct NativeAdrenoPwrupCounts {
    uint32_t ifpc;
    uint32_t preemption;
    uint32_t dynamic;
};
/* The lists with their current values (A830HwInit.h holds the register lists; A830Cp::patchPwrup reads the values). */
struct A830PwrupTables {
    const uint32_t *ifpc_regs,*ifpc_values;uint32_t ifpc_count;            /* A830HwInitIfpc */
    const uint32_t *pwrup_regs,*pwrup_values;uint32_t pwrup_count;         /* A830HwInitPwrup */
    const uint32_t *dynamic;uint32_t dynamic_count;                        /* triplets {pipe << 12, offset, value} */
};
class NativeAdrenoPwrup final {
    NativeAdrenoPwrup()=delete;
public:
    enum : size_t {HeaderBytes=16,PairBytes=8,TripletBytes=12};
    static size_t requiredBytes(const NativeAdrenoPwrupCounts &);
    /* regs/values are parallel arrays; dynamic triplets are {aperture, offset, value} in that order. out receives the exact
     * byte image; *written its length on success. */
    static IOReturn pack(const NativeAdrenoPwrupCounts &,
        const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
        const uint32_t *preemption_regs,const uint32_t *preemption_values,
        const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written);
    /* The Gen8 list (Linux a8xx_patch_pwrup_reglist layout) into page[0..bytes): the image, then zeros to the end of the page
     * so stale entries from an earlier list can never follow it. Every dynamic triplet must carry a pipe in [1,7] << 12 and
     * nothing else in its aperture word. written (optional) receives the image length. */
    static IOReturn buildGen8(const A830PwrupTables &,void *page,size_t bytes,size_t *written=nullptr);
};
/* The pre-Gen8 name of the packer. */
using NativeAdrenoPwrupList=NativeAdrenoPwrup;
#endif
