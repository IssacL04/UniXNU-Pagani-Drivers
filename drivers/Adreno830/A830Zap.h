/* SPDX-License-Identifier: MIT */
#ifndef A830_ZAP_H
#define A830_ZAP_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "Sm8750Scm.h"

/* The ZAP shader of S7 (ARCHITECTURE 4.11, PLAN M5) and its resume (4.13.4, 4.17 P1; PLAN M10, M13).
 *
 * The CP comes out of reset in secure mode; it may leave it (CP_SET_SECURE_MODE 0) only once TrustZone has authenticated
 * the signed ZAP shader through the peripheral authentication service, PAS id 13. RBBM_SECVID_TRUST_CNTL is never
 * written: with a ZAP present it is secure-only and a write is an XPU violation. Once per boot, as Linux
 * (a6xx_zap_shader_init "loaded", qcom_mdt_load, zap_shader_load_mdt) and KGSL (adreno_zap_shader_load, zap_loaded):
 *   1  parse gen80000_zap.mbn as a Qualcomm MDT/ELF32 image: exactly one loadable PT_LOAD segment, the hash segment
 *      (p_flags type 2), the lowest load address 4 KiB aligned, the load span within the reserved region, the
 *      metadata (ELF header part + hash segment) at most one 4 KiB page;
 *   2  metadata = file[0, phdr[0].p_filesz) + the hash segment (0x94 + 0xF38 = 0xFCC bytes for PKX110's file), written
 *      through a write-combined CPU mapping into one physically contiguous, 4 KiB aligned page (TZ puts an XPU over it
 *      during the calls: no dirty CPU line may reach it later), then dsb sy;
 *   3  PAS_INIT_IMAGE(13, metadata PA)
 *   4  PAS_MEM_SETUP(13, 0x9B09A000, span) when the loadable segment is relocatable (QCOM_MDT_RELOCATABLE);
 *   5  write-combined CPU mapping of the region, the loadable segment copied to region + (p_paddr - lowest p_paddr)
 *      (not relocatable: p_paddr - region), the rest of the span zeroed, dsb sy;
 *   6  PAS_AUTH_AND_RESET(13); the region's CPU mapping is removed right after it returns (on success the region now
 *      belongs to TZ; on failure no later step needs it and TZ may already protect it);
 *   7  the metadata page is freed once AUTH_AND_RESET succeeded; when any PAS call failed it stays allocated until the
 *      next cold boot (TZ may still hold its XPU), and no further load is attempted this boot.
 * Steps 8-12 belong to A830Cp and the orchestrator (CP_SET_SECURE_MODE 0 and idle, RB_GC_GMEM_PROTECT, pwrup list,
 * HWCG, OOB_GPU clear); the status step numbers of S7 follow 4.11 throughout (ARCHITECTURE 8.3).
 * SMC calls go through Sm8750Scm (INTERRUPTED resumes, WAITQ sleep/wake through INTID 994 or 1 ms GET_WQ_CTX polls,
 * EBUSY 20 x 30 ms, 1 s per call); a PAS call succeeds only with x0 == 0 && x1[31:0] == 0. No memory assignment
 * (hyp_assign) is made: under Gunyah the hypervisor itself creates the SHM bridges of a PIL call issued at EL1
 * (Linux qcom_scm_pas_init_image and qcom_scm_pas_prepare_and_auth_reset), and the zap path of Linux and KGSL assigns
 * nothing.
 *
 * After the first load the ZAP stays resident: a GPU recovery (CX kept, 4.13.4) and, by default, a slumber wake only send
 * CP_SET_SECURE_MODE 0 again (KGSL gen8 never reloads; Linux a6xx+ loads once). A830ZapResumeRemoteState adds
 * SET_REMOTE_STATE(0, 13) (Linux a5xx_zap_shader_resume) for a wake after a power collapse that lost the ZAP's state,
 * the alternative ARCHITECTURE 4.17 P1 allows. */

enum : uint32_t {
    A830ZapPasId=13,                    /* GPU_PAS_ID */
    A830ZapRegionPhys=0x9B09A000,       /* DT reserved-memory gpu-microcode@9b09a000, no-map (board->zap) */
    A830ZapRegionBytes=0x2000,
    A830ZapMetadataMax=0x1000,          /* one 4 KiB page */
    A830ZapPageBytes=0x1000,
    A830ZapMaxProgramHeaders=64,
    A830ZapMdtTypeMask=7u<<24,          /* QCOM_MDT_TYPE_MASK in p_flags */
    A830ZapMdtTypeHash=2u<<24,          /* QCOM_MDT_TYPE_HASH */
    A830ZapMdtRelocatable=1u<<27,       /* QCOM_MDT_RELOCATABLE */
};

/* S7 step numbers (ARCHITECTURE 4.11 and 8.3); 13 is the resume of 4.17 P1. */
enum A830ZapStep : uint16_t {
    A830ZapStepNone,
    A830ZapStepParse=1,A830ZapStepMetadata=2,A830ZapStepInitImage=3,A830ZapStepMemSetup=4,A830ZapStepLoad=5,
    A830ZapStepAuthReset=6,A830ZapStepRelease=7,
    A830ZapStepSecureMode=8,A830ZapStepGmemProtect=9,A830ZapStepPwrup=10,A830ZapStepHwcg=11,A830ZapStepOobClear=12,
    A830ZapStepRemoteState=13,
};

/* Why parseMdt refused an image (A830ZapImage::error, A830ZapReport::error). */
enum A830ZapError : uint16_t {
    A830ZapErrorNone,
    A830ZapErrorShort,          /* smaller than an ELF32 header */
    A830ZapErrorIdent,          /* not "\177ELF", ELFCLASS32, ELFDATA2LSB, EV_CURRENT */
    A830ZapErrorHeaders,        /* e_phentsize != 32, too many headers, or a header table outside the file */
    A830ZapErrorFirstHeader,    /* fewer than two program headers, or the first one is PT_LOAD */
    A830ZapErrorHash,           /* no hash segment after the first program header */
    A830ZapErrorMetadata,       /* ELF header part or hash segment outside the file, or metadata above 4 KiB */
    A830ZapErrorLoad,           /* not exactly one loadable PT_LOAD segment */
    A830ZapErrorSegment,        /* p_filesz > p_memsz, or the segment's file part outside the file */
    A830ZapErrorAlignment,      /* lowest load address not 4 KiB aligned */
    A830ZapErrorSpan,           /* the segments do not fit the reserved region */
};

/* A loadable segment (mdt_phdr_loadable: PT_LOAD, not the hash type, p_memsz != 0). */
struct A830ZapSegment {uint32_t index,offset,paddr,filesz,memsz,flags;};

/* What parseMdt found; the bytes stay owned by the caller (the embedded firmware image). */
struct A830ZapImage {
    const uint8_t *bytes;
    size_t length;
    uint32_t entry,phnum;
    uint32_t header_bytes;      /* phdr[0].p_filesz: the ELF header part of the metadata (0x94) */
    uint32_t hash_index,hash_offset,hash_bytes;   /* first program header with the hash type (2, 0x2000, 0xF38) */
    A830ZapSegment load;        /* the loadable segment (1, 0x1000, 0x1000, 0x3B4, 0x3B4, 0x08000007) */
    uint32_t loads;             /* loadable segments seen */
    uint64_t min_paddr,max_paddr;   /* lowest p_paddr; highest end rounded up to 4 KiB (qcom_mdt_get_size) */
    uint32_t span;              /* max_paddr - min_paddr: the PAS_MEM_SETUP size (0x1000) */
    uint32_t metadata_bytes;    /* header_bytes + hash_bytes (0xFCC) */
    bool relocatable;           /* QCOM_MDT_RELOCATABLE on a loadable segment */
    uint16_t error;             /* A830ZapError */
};

/* How resume() brings the ZAP back after the first load. */
enum A830ZapResume : uint32_t {
    A830ZapResumeSecureMode,    /* default: nothing through TZ, the CP only leaves secure mode again (KGSL gen8) */
    A830ZapResumeRemoteState,   /* SET_REMOTE_STATE(0, 13) first, for a wake after the ZAP's state was lost */
};

/* Memory the sequence needs. The kernel binding (A830ZapKernelMemory) uses one write-combined page of
 * NativeAdrenoMemory and a write-combined IOMemoryDescriptor mapping of the region; host tests use a model. */
struct A830ZapMemoryOps {
    void *cookie;
    /* bytes (<= one page) of physically contiguous, 4 KiB aligned memory, CPU mapped write-combined: *cpu, *phys and a
     * handle for metadataFree. */
    IOReturn (*metadataAlloc)(void *cookie,size_t bytes,void **cpu,uint64_t *phys,void **handle);
    void (*metadataFree)(void *cookie,void *handle);
    /* A write-combined CPU mapping of [phys, phys + bytes) of the reserved region, and its removal. */
    IOReturn (*regionMap)(void *cookie,uint64_t phys,uint64_t bytes,void **cpu,void **handle);
    void (*regionUnmap)(void *cookie,void *handle);
    void (*barrier)(void *cookie);       /* dsb sy: the CPU's stores have reached memory */
};

/* Result of the last load() or resume(), kept for the status block and the diagnostic page
 * ("ZAP x0 <x0> x1 <x1> step <n> wq <n>", ARCHITECTURE 8.4). */
struct A830ZapReport {
    uint16_t step;              /* A830ZapStep reached; on failure the step that failed */
    uint16_t error;             /* A830ZapError of a refused image */
    int32_t scm;                /* Sm8750ScmStatus of the deciding call (the failing one, else the last) */
    uint32_t fid;               /* its function id */
    int64_t x0;                 /* its final x0 */
    uint64_t x1;                /* its final x1 */
    uint32_t smcs;              /* SMC instructions of the PAS calls: INTERRUPTED resumes, WAITQ resumes, GET_WQ_CTX polls */
    uint32_t waitq,interrupted,busy;
    uint32_t elapsed_us;        /* the PAS calls together */
    uint64_t metadata_phys;
    uint32_t metadata_bytes;
    uint32_t span;
    uint32_t loads;             /* 1 once the ZAP is authenticated */
    uint32_t resumes,resume_failures;   /* SET_REMOTE_STATE resumes */
    bool metadata_held;         /* a PAS call failed: the metadata page stays allocated until the next cold boot */
};

/* What the orchestrator's failure record gets (Adreno830Start Failure, ARCHITECTURE 8.3): reason A830ReasonFirmware for
 * a refused image (reg = A830ZapError, value = file length), A830ReasonNoMemory for the metadata page or the region
 * mapping (reg = bytes or region PA), A830ReasonSmc for a PAS call (reg = function id, value = x0, or x1 when x0 == 0),
 * A830ReasonUnexpectedState for a resume without a loaded ZAP or a load after a failed one. */
struct A830ZapFailure {uint16_t step,reason;uint64_t reg,value,expect;};

/* The S7 words of adreno830_status (ARCHITECTURE 8.4 S6/S7 page, third row; A830Cp keeps aux[0..3] for the CP rows):
 *   aux[A830ZapAuxX0]   = x0 of the deciding SMC
 *   aux[A830ZapAuxWord] = x1[31:0] | step << 32 | min(waitq, 255) << 40 | min(smcs, 0xffff) << 48 */
enum : unsigned {A830ZapAuxX0=4,A830ZapAuxWord=5};

class A830Zap {
public:
    /* Step 1 (pure): ELF32/MDT checks of ARCHITECTURE 4.11 and Linux mdt_header_valid, qcom_mdt_read_metadata,
     * qcom_mdt_get_size. kIOReturnBadArgument with out->error set when the image is refused. */
    static IOReturn parseMdt(const uint8_t *image,size_t length,A830ZapImage *out);
    /* Step 2 (pure): the metadata bytes into out[0, *bytes); the rest of capacity is zeroed. */
    static IOReturn buildMetadata(const A830ZapImage &image,uint8_t *out,size_t capacity,size_t *bytes);
    /* Step 5 (pure apart from the stores): the loadable segment into a CPU mapping of [region_phys, + region_bytes),
     * relocated as qcom_mdt_load_no_init does, then zeros over the rest of the span. */
    static IOReturn loadSegments(const A830ZapImage &image,volatile uint8_t *region,uint64_t region_phys,uint64_t region_bytes);

    /* scm: the boot's SCM instance (S2 attached its wait-queue interrupt when it could). */
    IOReturn init(Sm8750Scm *scm,const A830ZapMemoryOps *memory);
    /* Steps 1-7 once per boot; a second call after success returns success without SMCs, after a failed PAS call
     * kIOReturnNotReady. region/bytes: board->zap (0x9B09A000, 0x2000). Writes the S7 step and status words. */
    IOReturn load(const uint8_t *image,size_t length,uint64_t region,uint64_t bytes);
    /* Steps 2-7 for a parsed image; out[0..2] get INIT_IMAGE, MEM_SETUP (fid 0 when not issued) and AUTH_AND_RESET. */
    IOReturn authenticate(const A830ZapImage &image,uint64_t region,uint64_t bytes,Sm8750SmcResult out[3]);
    /* After the first load, before CP_SET_SECURE_MODE 0 of a recovery or a slumber wake: kIOReturnNotReady without a
     * loaded ZAP; A830ZapResumeSecureMode issues nothing; A830ZapResumeRemoteState issues SET_REMOTE_STATE(0, 13). */
    IOReturn resume(A830ZapResume mode);

    bool loaded() const {return loaded_;}
    const A830ZapReport &report() const {return report_;}
    void failure(A830ZapFailure *out) const;
    /* The step and the aux words above into adreno830_status (load() does it itself; a recovery may call it). */
    void noteStatus() const;

private:
    Sm8750Scm *scm_=nullptr;
    const A830ZapMemoryOps *memory_=nullptr;
    void *metadata_handle_=nullptr;
    A830ZapReport report_={};
    uint64_t failed_reg_=0,failed_value_=0;
    uint16_t failed_reason_=0;
    bool loaded_=false,poisoned_=false;
    void fail(uint16_t step,uint16_t reason,uint64_t reg,uint64_t value);
    void account(const Sm8750SmcResult &result);
    IOReturn smcFailed(uint16_t step,int status,const Sm8750SmcResult &result);
};

/* Kernel binding (not built for host tests): one page of NativeAdrenoMemory made write-combined (cleaned and invalidated
 * first, then every CPU mapping of it WC; 40-bit output, XNU's DRAM starts at 0x9_8000_0000) for the metadata, freed with
 * NativeAdrenoMemory::release (the default attribute comes back); a physical IOMemoryDescriptor of the region mapped
 * kIOMapWriteCombineCache into the kernel task, released on unmap; dsb sy as the barrier. The SCM instance and its
 * kernel services (SMC #0, mutex, sleep, wait-queue interrupt) are the orchestrator's (S2). */
extern "C" const A830ZapMemoryOps *A830ZapKernelMemory(void);
#endif
