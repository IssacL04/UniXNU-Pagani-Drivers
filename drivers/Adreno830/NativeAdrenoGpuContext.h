/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_CONTEXT_H
#define NATIVE_ADRENO_GPU_CONTEXT_H
#include "NativeAdrenoGmuContext.h"

/* The GPU's own SMMU context (ARCHITECTURE 4.5.4 second half, 4.5.5, 4.6): a native4K lower-region (TTBR0) table pool for the
 * GPU's 0..4 GiB space (pagani-gpu-va=2: 0..2 GiB; pagani-gpu-va=16: 0..16 GiB with a 34-bit input, falling back to 4 GiB when the
 * SMMU's IDR2.UBS is below 34 bits or the 32 MiB table pool cannot be had), an empty zeroed TTBR1 guard table (a CP that clears
 * TCR.EPD1 then walks nothing), one context bank programmed and routed through the board's programmer (CB0, ASID 1, SID 0 on slot 0),
 * the LPAC stream (SID 1 on slot 3) added once the bank is enabled, then the kernel buffers the command processor needs at fixed GPU
 * addresses below the 12 MiB GMEM window (UCHE_CCHE_GC_GMEM_RANGE_MIN 0x4000000), which the VM refuses to map, as it refuses
 * the null guard below 0x100000 (ARCHITECTURE 4.6.2):
 *   sqe      0x100000  0x1C000  the SQE microcode (gen80000_sqe.fw without its first dword), read + execute, unprivileged readable
 *   aqe      0x180000  0x7000   the AQE microcode, the same attributes
 *   ring     0x200000  0x8000   ring buffer 0, aligned to its size, privileged read-only
 *   memptrs  0x300000  0x1000   msm_rbmemptrs layout: +0 rptr, +4 fence, +8 bv_rptr, +12 bv_fence; privileged read/write
 *   pwrup    0x301000  0x1000   the power-up register list; privileged read/write
 *   scratch  0x302000  0x1000   the first submissions' scratch; privileged read/write
 * Every buffer is GPU non-cacheable outer shareable and CPU write-combined (ARCHITECTURE 4.14.1: barriers only, no cache
 * maintenance; the CPU view `cpu` is the kernel's mapping after its pages became write-combined). The write-combined BO pool
 * of the clients is reserved first (pool_mib, halved until a reservation succeeds, ARCHITECTURE 3.4 S3). Bank, ASID, slots and
 * stream IDs are board inputs. Nothing is torn down. */
enum : uint64_t {
    GpuSqeIova=0x100000, GpuAqeIova=0x180000, GpuRingIova=0x200000, GpuMemptrsIova=0x300000, GpuPwrupIova=0x301000,
    GpuScratchIova=0x302000, GpuNullGuardBytes=0x100000, GpuGmemIova=0x4000000, GpuGmemBytes=0xC00000,
    GpuApertureBytes=0x100000000, GpuApertureBytes2G=0x80000000, GpuApertureBytes16G=0x400000000, GpuClientBase=0x20000000
};
enum : size_t {GpuSqeBytes=0x1C000, GpuAqeBytes=0x7000, GpuRingBytes=0x8000, GpuPageBytes=0x1000};
enum NativeAdrenoGpuBufferIndex : unsigned {
    GpuBufferSqe,GpuBufferAqe,GpuBufferRing,GpuBufferMemptrs,GpuBufferPwrup,GpuBufferScratch,GpuBufferCount
};
struct NativeAdrenoGpuBuffer {uint64_t physical,bytes,iova;volatile void *cpu;};
struct NativeAdrenoGpuContextConfig {
    const NativeAdrenoSmmuProgrammer *programmer;
    unsigned bank,asid;             /* CB0, 1 */
    unsigned slot,lpac_slot;        /* the surveyed GPU and LPAC slots (preferred 0 and 3) */
    uint16_t sid,lpac_sid;          /* 0, 1 */
    unsigned output_bits;           /* 0: from the programmer's IDR2 (capped at 40) */
    uint32_t aperture_gib;          /* pagani-gpu-va: 16 (8209 table pages, a 34-bit input; else 4), 4 (2053) or 2 (1027) */
    uint32_t pool_mib;              /* pagani-gpu-pool (512); 0: no pool, every BO scattered */
    bool write_combined_pool;       /* the pool's pages write-combined once (user BOs are write-combined) */
};
class NativeAdrenoGpuContext final {
    NativeAdrenoMemory tables_,guard_,buffers_[GpuBufferCount];
    uint64_t handles_[GpuBufferCount]{};
    NativeAdrenoSmmuContext smmu_;
    NativeAdrenoVm vm_;
    /* The alias checks' sort scratch: one entry per range of the smaller compared side, which is never more than the VM's 262144
     * table ranges: AliasScratchEntries whatever the aperture (half the 4 GiB aperture's leaves, an eighth of the 16 GiB one's). */
    uint32_t *alias_scratch_=nullptr;
    uint64_t aperture_=0;
    unsigned output_bits_=0;
    uint32_t pool_mib_=0;
    bool ready_=false,used_=false;
public:
    enum Step : uint32_t {Allocate=1,Prepare,Program,Attach,Lpac,VmAttach,Map,Ready};
    /* 4 GiB with a 32-bit input: 1 L1 + 4 L2 + 2048 L3 pages; 2 GiB: 1 + 2 + 1024; 16 GiB with a 34-bit input: 1 + 16 + 8192, 32 MiB
     * (ARCHITECTURE 4.6.1). The alias scratch holds one entry per range of the smaller compared side: never more than the VM's
     * 262144 table ranges, whatever the aperture. */
    enum : size_t {TablePages=2053,TablePages2G=1027,TablePages16G=8209,AliasScratchEntries=size_t(1)<<19};
    NativeAdrenoGpuContext()=default;
    NativeAdrenoGpuContext(const NativeAdrenoGpuContext &)=delete;
    NativeAdrenoGpuContext &operator=(const NativeAdrenoGpuContext &)=delete;
    /* One attempt per object. progress (may be null) is called before each step. The report's streams counts both routes. */
    IOReturn bringUp(const NativeAdrenoGpuContextConfig &config,void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);
    bool ready() const {return ready_;}
    const NativeAdrenoMemory &buffer(unsigned index) const {return buffers_[index];}
    /* physical, bytes, iova and the CPU (write-combined) view of every kernel buffer, only after Ready. */
    IOReturn buffers(NativeAdrenoGpuBuffer out[GpuBufferCount]) const;
    NativeAdrenoVm *vm() {return ready_?&vm_:nullptr;}
    NativeAdrenoSmmuContext &smmu() {return smmu_;}
    uint64_t apertureBytes() const {return aperture_;}
    /* The input size the context was prepared with: 34 bits for the 16 GiB aperture, else 32 (the publish shell's input_bits). */
    unsigned inputBits() const {return aperture_>GpuApertureBytes?34u:32u;}
    uint64_t guardTable() const {return guard_.spanCount()?guard_.spans()[0].physical:0;}
    unsigned outputBits() const {return output_bits_;}
    uint32_t poolMib() const {return pool_mib_;}   /* the pool reserved (0: none) */
    /* The client window of the aperture: [GpuClientBase, aperture end) (ARCHITECTURE 6.2). */
    uint64_t clientEnd() const {return aperture_;}
};
/* The context that last reached Ready (the board's; null before): its VM and its buffers for the publish shell. */
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[GpuBufferCount]);
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm();
#endif
