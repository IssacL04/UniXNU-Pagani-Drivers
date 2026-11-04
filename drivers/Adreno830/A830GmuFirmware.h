/* SPDX-License-Identifier: MIT */
#ifndef A830_GMU_FIRMWARE_H
#define A830_GMU_FIRMWARE_H
#include "NativeAdrenoGmuMemory.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* MMIO of the GMU block, shared by A830GmuFirmware, A830Gmu and A830Hfi (ARCHITECTURE 2.2: the window set is
 * {Gmu, GmuAo, Rscc, Gpu, CxMisc}, no PDC). Offsets are bytes inside the window:
 *   Gmu    gmu@3d37000 "gmu" (0x68000): GMUCX and GMUAO registers, A830_GMU_REG_BYTE(dword), the TCMs and the GX GDSC
 *          (0x3D68024) lie inside it as well;
 *   GmuAo  "gmu_ao_blk_dec0" 0x3D40000 (0x10000): optional, nothing here needs it (GMUAO is reached through Gmu);
 *   Rscc   GPU node "rscc" 0x3D50000 (0x10000): A830_RSCC_REG_BYTE(dword);
 *   Gpu    kgsl_3d0_reg_memory 0x3D00000 (0x40000): only the CX-side GBIF (0x3C00..) and, under OOB_GPU, GX registers;
 *   CxMisc GPU node "cx_misc" 0x3D9E000 (0x2000): A830_CX_MISC_REG_BYTE(dword).
 * read32/write32 return 0 on success and nonzero for an access outside a bound window (nothing is touched then); the
 * kernel adapter (A830GmuBindMmio) is a 32-bit Device-nGnRE access. barrier completes every earlier write (dsb sy). */
enum A830GmuWindow : unsigned {A830GmuWindowGmu,A830GmuWindowGmuAo,A830GmuWindowRscc,A830GmuWindowGpu,A830GmuWindowCxMisc,
                               A830GmuWindows};
struct A830GmuIo {
    void *cookie;
    int (*read32)(void *cookie,unsigned window,uint32_t offset,uint32_t *value);
    int (*write32)(void *cookie,unsigned window,uint32_t offset,uint32_t value);
    void (*barrier)(void *cookie);
};

/* gen80000_gmu.bin (ARCHITECTURE 4.7.1, KGSL gen8_gmu_parse_fw / gen8_gmu_load_fw): consecutive blocks, each a 16-byte
 * little-endian header {u32 addr, u32 size, u32 type, u32 value} followed by `size` bytes. size 0 marks a metadata block;
 * all of them precede the data blocks. Metadata type 1 (PREALLOC_REQ) and 7 (PREALLOC_PERSIST_REQ) ask for `value` bytes
 * of backing store at GMU address `addr`; other metadata types are counted and ignored. The type/value of a data block are
 * filler (0xEFBEADDE) and ignored. The PKX110 image (88896 bytes, sha256 1ad8175e...) has 11 metadata blocks and 4 data
 * blocks: ITCM 0x0 (0x1EC bytes, boot stub), CACHE 0x4000 (0x15824 bytes, main code), DTCM 0x10007F98 (0x28 bytes of 0),
 * DTCM 0x10007FDC (0x18 bytes, version words), ending at 0x15B40. */
enum : uint32_t {
    A830GmuMaxBlocks=64,
    A830GmuMaxRequests=32,
    A830GmuBlockHeaderBytes=16,
    A830GmuTypePrealloc=NativeAdrenoGmuRequestPrealloc,
    A830GmuTypePreallocPersist=NativeAdrenoGmuRequestPersist,
    /* DTCM addresses of the version words (KGSL gen8_gmu_version_info: DTCM_START + 0xFF8/0xFFA/0xFFC dwords) */
    A830GmuCoreVersionIova=0x10004000+4*0xFF8,A830GmuPwrVersionIova=0x10004000+4*0xFFA,A830GmuHfiVersionIova=0x10004000+4*0xFFC,
};
/* What load() writes. */
enum : unsigned {A830GmuLoadTcm=1u<<0,A830GmuLoadDdr=1u<<1,A830GmuLoadAll=A830GmuLoadTcm|A830GmuLoadDdr};
/* Where a block goes (ARCHITECTURE 4.7.1): ITCM [0,0x4000) and DTCM [0x10004000,0x10008000) through registers, anything
 * else into the region of NativeAdrenoGmuMemory holding it. */
enum A830GmuTarget : uint32_t {A830GmuTargetItcm,A830GmuTargetDtcm,A830GmuTargetDdr};
struct A830GmuBlock {
    uint32_t addr,size,type,value;
    uint32_t offset;           /* byte offset of the block's data in the image */
    uint32_t target;           /* A830GmuTarget of a data block (metadata: A830GmuTargetDdr, unused) */
};
struct A830GmuBlocks {
    const uint8_t *image;      /* the parsed image; must outlive every load() */
    size_t bytes;
    A830GmuBlock block[A830GmuMaxBlocks];
    uint32_t count,metadata,data;                 /* all blocks; the leading metadata ones; the data ones */
    uint32_t ignored_metadata;                    /* metadata blocks of other types */
    NativeAdrenoGmuRequest request[A830GmuMaxRequests];   /* PREALLOC requests, in image order */
    uint32_t requests;
    uint32_t itcm[4];          /* first 4 ITCM words of the image (B4 check): 0x10007F80 0x105 0x145 0x159 on PKX110 */
    uint32_t versions;         /* bit0 core, bit1 pwr, bit2 hfi: present in a DTCM data block */
    uint32_t core,pwr,hfi;     /* version words the image writes: 0x50010019, 0x50000112, 0x20070001 on PKX110 */
    uint32_t end;              /* offset after the last block (= bytes) */
};

/* What load() verified. Only the annibale profile (A830Profile.h) verifies: K/adreno8_gmu.c:470-554 (kernel-sequences 4.5
 * step 4, section 14 row 16) writes each TCM block, completes the stores (dsb sy) and reads every word of the block back,
 * and compares each DDR block with the image after the copy; any difference is fatal there and here. The design profile
 * only checks the first 4 ITCM words afterwards (checkItcm, record only) and leaves this structure zero. */
struct A830GmuLoadCheck {
    uint32_t tcm_words;        /* TCM words read back equal to the image */
    uint32_t ddr_bytes;        /* DDR bytes compared equal to the image */
    bool mismatch;             /* a word differed (load() returned kIOReturnIOError) */
    bool read_failed;          /* a TCM read-back access failed (kIOReturnIOError; read is 0xFFFFFFFF) */
    uint32_t addr;             /* GMU address of the word that differed or could not be read */
    uint32_t read,expect;      /* the word read there and the image's word */
};

/* Parse failures are kIOReturnBadArgument (A830ReasonFirmware): a header or data past the end, metadata after data, a zero
 * byte PREALLOC request, more than A830GmuMaxBlocks blocks or A830GmuMaxRequests requests, a data block outside every GMU
 * range or crossing a range end, a TCM block not 4-byte aligned in address and size, an empty image. Load failures:
 * kIOReturnNotReady (memory not allocated), kIOReturnBadArgument (a DDR block in no region: "falls in no area"; the annibale
 * profile without io.read32 for its TCM read-back), kIOReturnIOError (a TCM register write failed; annibale profile: a TCM
 * read failed or a TCM or DDR word read back differently, see A830GmuLoadCheck). Nothing here sleeps, allocates or logs. */
class A830GmuFirmware final {
public:
    static IOReturn parse(const uint8_t *image,size_t bytes,A830GmuBlocks *out);
    /* The memory plan of the image's PREALLOC requests (NativeAdrenoGmuPlanRequests). */
    static IOReturn plan(const A830GmuBlocks &blocks,size_t table_bytes,NativeAdrenoGmuPlan *out);
    /* Every data block of the kinds asked for: TCM blocks dword by dword through the Gmu window at CM3_ITCM_START /
     * CM3_DTCM_START (0x3D6D000 / 0x3D71000), DDR blocks copied into their region; then one barrier. TCM contents are lost
     * whenever CX loses power, so every GMU start loads again (B4). S4 may load the DDR blocks alone first. The annibale
     * profile verifies every block as it goes (A830GmuLoadCheck); `check` (may be null) gets what was verified. */
    static IOReturn load(const A830GmuBlocks &blocks,A830GmuIo &io,NativeAdrenoGmuMemory &memory,unsigned what=A830GmuLoadAll,
                         A830GmuLoadCheck *check=nullptr);
    /* B4 path check (record only): reads the first 4 ITCM words into `words`; returns kIOReturnSuccess when they equal the
     * image's, kIOReturnNotReady otherwise (or kIOReturnIOError when a read failed). */
    static IOReturn checkItcm(const A830GmuBlocks &blocks,A830GmuIo &io,uint32_t words[4]);
    /* Byte offset in the Gmu window of a TCM address, or ~0u when `addr` is in no TCM. */
    static uint32_t tcmOffset(uint32_t addr);
};
#endif
