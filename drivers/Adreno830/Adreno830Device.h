/* SPDX-License-Identifier: MIT */
#ifndef ADRENO830_DEVICE_H
#define ADRENO830_DEVICE_H
#include "Adreno830Board.h"
#include "Adreno830Status.h"
#include "Sm8750GpuPower.h"
#include "Sm8750Rpmh.h"
#include "Sm8750Scm.h"
#include "Sm8750SocInfo.h"

/* The A830 device of ARCHITECTURE 2.3: one static object in Adreno830Start.cpp (g830), built by adreno830_start() from the
 * board, living from boot to shutdown and never freed. Its layout stays private to the orchestrator: the modules that run
 * after the bring-up get what they need injected when S3-S10 build them (A830PowerOps and A830RecoveryOps for the power
 * state machine and the recovery, the DCVS and IRQ hooks), and anything else reads the accessors below. Every pointer
 * returned is valid for the rest of the boot once the stage that creates it succeeded, and null before. */
struct Adreno830Device;

/* ---- Kernel services ----------------------------------------------------------------------------------------------
 * What the orchestrator itself needs from XNU, in one table: Adreno830Start.cpp binds the kernel implementations, the host
 * tests install their own (adreno830_set_os, host builds only). On the device the modules keep their own kernel bindings
 * (A830SmmuBindMmio, A830GmuKernelOs, A830ScmKernelOps, A830ZapKernelMemory, a830_kernel_power_env, ...); in host builds
 * the orchestrator builds those service tables from this one. Thread context unless noted. */
enum : uint32_t {Adreno830MapDevice=0,Adreno830MapWriteCombined=1};   /* Device-nGnRE (kIOMapInhibitCache) / Normal-NC */
struct Adreno830MetalReport {   /* publishMetal (ARCHITECTURE 3.3), for the S9 page */
    uint32_t step;              /* 1 IOGPUFamily loaded, 2 IOAcceleratorES metaclass (136 bytes), 3 own class registered and
                                   136 bytes, 4 object initialised with its three properties, 5 attached and registered */
    int32_t kext;               /* OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily") */
    uint32_t parent_bytes;      /* IOAcceleratorES getClassSize(), must be 136 */
    uint32_t instance_bytes;    /* native_adreno_accelerator_instance_size(), must be 136 */
};
struct Adreno830Os {
    uint64_t (*now_us)(void);                               /* monotonic, mach_absolute_time based */
    void (*delay_us)(uint32_t microseconds);                /* busy wait (IODelay); any context */
    void (*sleep_us)(uint32_t microseconds);                /* blocks (IOSleep + IODelay for the rest) */
    volatile void *(*map)(uint64_t phys,uint64_t bytes,uint32_t mode);   /* never unmapped for windows; null on failure */
    void (*unmap)(volatile void *va,uint64_t bytes);
    int (*smc)(void *cookie,uint64_t regs[8]);              /* Sm8750ScmSmcInstruction */
    /* A sleeping mutex (IOLock): the SCM serialisation, the HFI lock of ARCHITECTURE 2.3, the CP aperture lock. */
    void *(*mutex_alloc)(void);
    void (*mutex_lock)(void *mutex);
    void (*mutex_unlock)(void *mutex);
    /* Event waits (assert_wait_timeout + thread_block / thread_wakeup); wake is interrupt safe. */
    void (*wait)(const void *event,uint32_t timeout_us);
    void (*wake)(const void *event);
    /* A kernel thread that runs body(argument) once and ends (kernel_thread_start + thread_deallocate). */
    IOReturn (*thread_start)(void (*body)(void *argument),void *argument);
    /* A kernel-high, once-only thread call (thread_call_allocate_with_options); enter is interrupt safe. */
    void *(*call_allocate)(void (*function)(void *argument),void *argument);
    void (*call_enter)(void *call);
    /* PaganiArmCpu SPI interface (a missing export answers kIOReturnUnsupported / 0). */
    IOReturn (*spi_attach)(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router);
    IOReturn (*spi_disable)(uint32_t intid);
    IOReturn (*spi_enable)(uint32_t intid);
    uint64_t (*spi_count)(uint32_t intid);
    /* The CPU generic counter (mach_absolute_time's source) and interrupt masking on this CPU (the CX timer seed). */
    uint64_t (*counter)(void);
    uintptr_t (*irq_disable)(void);
    void (*irq_restore)(uintptr_t state);
    /* publishMetal split at the only point after which nothing can fail: prepare loads IOGPUFamily, checks the 136-byte
     * IOAcceleratorES, runs the staged class initializer between preModLoad/postModLoad, checks the 136-byte instance,
     * allocates, initialises and sets the three properties of AGXAccelerator; commit attaches it to the platform expert
     * and registers it; discard releases a prepared object that was never attached. */
    IOReturn (*metal_prepare)(Adreno830MetalReport *report);
    IOReturn (*metal_commit)(Adreno830MetalReport *report);
    void (*metal_discard)(void);
};
const Adreno830Os *adreno830_os(void);

/* ---- Register windows -------------------------------------------------------------------------------------------
 * The orchestrator maps every board window once (S0: cmd-db, SMEM, qfprom; S1: the rest; Device-nGnRE) and never unmaps
 * it. Accesses are 32-bit; writes are followed by dsb sy (a write that must land before a poll or before the next block is
 * touched, ARCHITECTURE 0.3). */
enum Adreno830WindowId : unsigned {
    A830WinGpu,A830WinRscc,A830WinCxDbgc,A830WinCxMisc,A830WinGmu,A830WinGmuAo,A830WinGpuCc,A830WinGcc,A830WinSmmu,
    A830WinCmdDb,A830WinSmem,A830WinZap,A830WinQfprom,A830WinRscDrv2,A830WinAossMsgram,A830WinIpcc,A830WinTsensTm,
    A830WinLlcc0,A830WinLlcc1,A830WinLlcc2,A830WinLlcc3,A830WinLlccOr,A830WinLlccAnd,A830WinCount
};
struct Adreno830Mmio {volatile uint8_t *base;uint64_t phys,bytes;};
/* The mapped window (base null when the board leaves an optional window out or the stage that maps it has not run). */
const Adreno830Mmio *adreno830_window(unsigned id);
static inline uint32_t adreno830_read32(const Adreno830Mmio *window,uint32_t offset)
{
    return *reinterpret_cast<volatile const uint32_t *>(window->base+offset);
}
static inline void adreno830_write32(const Adreno830Mmio *window,uint32_t offset,uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(window->base+offset)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- What S0-S2 established, for the modules that run later ------------------------------------------------------ */
const Adreno830Board *adreno830_board(void);
const Sm8750CmdDb *adreno830_cmd_db(void);                    /* S0 snapshot (static storage, 128 KiB) */
const Sm8750RpmhResources *adreno830_rpmh_resources(void);    /* S0 */
const Sm8750SocInfo *adreno830_soc_info(void);                /* S0 */
const Sm8750GpuLevelTable *adreno830_levels(void);            /* S0: the GMU level table (level = index + 1) */
const Sm8750RpmhIo *adreno830_drv2_io(void);                  /* S1: apps RSC DRV-2 */
Sm8750RpmhHost *adreno830_rpmh_host(void);                    /* S1: the host votes (one owner at a time) */
const Sm8750GpuPowerIo *adreno830_power_io(void);             /* S1: GCC + GPU_CC for Sm8750GpuPowerOn/Off */
Sm8750Scm *adreno830_scm(void);                               /* S2 */
/* The DDR level's kBps of the board's bus table (0 above the table). */
uint32_t adreno830_ddr_kbps(unsigned level);
/* A stage failure as adreno830_status records it (ARCHITECTURE 8.3). */
struct Adreno830Failure {uint8_t stage;uint16_t step,reason;IOReturn ior;uint64_t reg,value,expect;};
/* The last bring-up failure (adreno830_start or a restart through the power operations); zero when none. */
const Adreno830Failure *adreno830_last_failure(void);
/* true once S10 reached Running (stays true across recoveries; the gate of A830Recovery says whether work is accepted). */
bool adreno830_running(void);
#endif
