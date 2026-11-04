/* SPDX-License-Identifier: MIT */
#include "A830GmuFirmware.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include <IOKit/IOLib.h>
#include <string.h>

namespace {
uint32_t le32(const uint8_t *p) {return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
bool within(uint64_t start,uint64_t end,uint64_t base,uint64_t limit) {return start>=base && end<=limit && start<end;}

/* Where a data block of [start, end) goes; false when it lies in no GMU range or crosses a range end. The DDR ranges are the
 * CACHE range and the uncached kernel range (KGSL gen8_gmu_vma: GMU_CACHE 16K..16M, GMU_NONCACHED_KERNEL 0x60000000 +
 * 512 MiB); whether a region really backs the block is decided when it is loaded. */
bool target(uint64_t start,uint64_t end,uint32_t *out)
{
    if(within(start,end,NativeAdrenoGmuItcmIova,NativeAdrenoGmuItcmIova+NativeAdrenoGmuItcmBytes)) {*out=A830GmuTargetItcm;return true;}
    if(within(start,end,NativeAdrenoGmuDtcmIova,NativeAdrenoGmuDtcmIova+NativeAdrenoGmuDtcmBytes)) {*out=A830GmuTargetDtcm;return true;}
    if(within(start,end,NativeAdrenoGmuCacheIova,NativeAdrenoGmuCacheEnd) ||
        within(start,end,NativeAdrenoGmuUncachedIova,NativeAdrenoGmuUncachedEnd)) {*out=A830GmuTargetDdr;return true;}
    return false;
}

/* The word a TCM data block of the image writes at `addr`. */
bool imageWord(const A830GmuBlocks &b,uint32_t addr,uint32_t *value)
{
    for(uint32_t i=0;i<b.count;++i) {
        const A830GmuBlock &blk=b.block[i];
        if(!blk.size || blk.target==A830GmuTargetDdr)continue;
        if(addr>=blk.addr && uint64_t(addr)+4<=uint64_t(blk.addr)+blk.size) {
            *value=le32(b.image+blk.offset+(addr-blk.addr));
            return true;
        }
    }
    return false;
}

void completeStores()
{
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
}

uint32_t A830GmuFirmware::tcmOffset(uint32_t addr)
{
    if(addr<NativeAdrenoGmuItcmIova+NativeAdrenoGmuItcmBytes)
        return A830_GMU_REG_BYTE(A8XX_GMU_CM3_ITCM_START)+(addr-NativeAdrenoGmuItcmIova);
    if(addr>=NativeAdrenoGmuDtcmIova && addr-NativeAdrenoGmuDtcmIova<NativeAdrenoGmuDtcmBytes)
        return A830_GMU_REG_BYTE(A8XX_GMU_CM3_DTCM_START)+(addr-NativeAdrenoGmuDtcmIova);
    return ~0u;
}

IOReturn A830GmuFirmware::parse(const uint8_t *image,size_t bytes,A830GmuBlocks *out)
{
    if(!out)return kIOReturnBadArgument;
    /* Parsed in place: the structure is about 2 KiB, too much for a 16 KiB kernel thread stack. */
    memset(out,0,sizeof *out);
    A830GmuBlocks &parsed=*out;
    auto refuse=[out] {memset(out,0,sizeof *out);return kIOReturnBadArgument;};
    if(!image || bytes<A830GmuBlockHeaderBytes || bytes>UINT32_MAX)return refuse();
    parsed.image=image;parsed.bytes=bytes;
    size_t offset=0;
    while(offset<bytes) {
        if(bytes-offset<A830GmuBlockHeaderBytes || parsed.count==A830GmuMaxBlocks)return refuse();
        const uint8_t *header=image+offset;
        A830GmuBlock blk={le32(header),le32(header+4),le32(header+8),le32(header+12),uint32_t(offset+A830GmuBlockHeaderBytes),
                          A830GmuTargetDdr};
        offset+=A830GmuBlockHeaderBytes;
        if(blk.size>bytes-offset)return refuse();
        if(!blk.size) {
            /* KGSL relies on every metadata block preceding the data; one after data would be silently skipped there. */
            if(parsed.data)return refuse();
            ++parsed.metadata;
            if(blk.type==A830GmuTypePrealloc || blk.type==A830GmuTypePreallocPersist) {
                if(!blk.value || uint64_t(blk.addr)+blk.value>0x100000000ull || parsed.requests==A830GmuMaxRequests)
                    return refuse();
                parsed.request[parsed.requests++]={blk.addr,blk.value,blk.type};
            } else ++parsed.ignored_metadata;
        } else {
            const uint64_t start=blk.addr,end=start+blk.size;
            if(!target(start,end,&blk.target))return refuse();
            if(blk.target!=A830GmuTargetDdr && ((blk.addr|blk.size)&3))return refuse();
            ++parsed.data;
            offset+=blk.size;
        }
        parsed.block[parsed.count++]=blk;
    }
    if(!parsed.data)return refuse();
    parsed.end=uint32_t(offset);
    for(uint32_t i=0;i<4;++i)(void)imageWord(parsed,NativeAdrenoGmuItcmIova+4*i,&parsed.itcm[i]);
    if(imageWord(parsed,A830GmuCoreVersionIova,&parsed.core))parsed.versions|=1u<<0;
    if(imageWord(parsed,A830GmuPwrVersionIova,&parsed.pwr))parsed.versions|=1u<<1;
    if(imageWord(parsed,A830GmuHfiVersionIova,&parsed.hfi))parsed.versions|=1u<<2;
    return kIOReturnSuccess;
}

IOReturn A830GmuFirmware::plan(const A830GmuBlocks &blocks,size_t table_bytes,NativeAdrenoGmuPlan *out)
{
    if(!out)return kIOReturnBadArgument;
    if(!blocks.image || !blocks.count) {*out={};return kIOReturnBadArgument;}
    return NativeAdrenoGmuPlanRequests(blocks.request,blocks.requests,table_bytes,out);
}

IOReturn A830GmuFirmware::load(const A830GmuBlocks &blocks,A830GmuIo &io,NativeAdrenoGmuMemory &memory,unsigned what,
                               A830GmuLoadCheck *check)
{
    A830GmuLoadCheck local{};
    A830GmuLoadCheck &verified=check?*check:local;
    verified={};
    if(!blocks.image || !blocks.count || !(what&A830GmuLoadAll) || (what&~unsigned(A830GmuLoadAll)))return kIOReturnBadArgument;
    if((what&A830GmuLoadTcm) && !io.write32)return kIOReturnBadArgument;
    if((what&A830GmuLoadDdr) && !memory.ready())return kIOReturnNotReady;
    /* annibale verifies every block right after writing it (K/adreno8_gmu.c:517-546): the TCM words read back through the
     * window, the DDR copy compared with the image. The design profile writes only. */
    const bool verify=adreno830_annibale();
    if(verify && (what&A830GmuLoadTcm) && !io.read32)return kIOReturnBadArgument;
    /* Every DDR block must have its backing region before anything is written: a layout that does not match the image
     * loads nothing ("falls in no area", ARCHITECTURE 4.7.1). */
    if(what&A830GmuLoadDdr)
        for(uint32_t i=0;i<blocks.count;++i) {
            const A830GmuBlock &blk=blocks.block[i];
            if(blk.size && blk.target==A830GmuTargetDdr && !memory.cpu(blk.addr,blk.size))return kIOReturnBadArgument;
        }
    bool tcm=false;
    for(uint32_t i=0;i<blocks.count;++i) {
        const A830GmuBlock &blk=blocks.block[i];
        if(!blk.size)continue;
        const uint8_t *source=blocks.image+blk.offset;
        if(blk.target==A830GmuTargetDdr) {
            if(!(what&A830GmuLoadDdr))continue;
            uint8_t *target=static_cast<uint8_t *>(memory.cpu(blk.addr,blk.size));
            memcpy(target,source,blk.size);
            if(!verify)continue;
            /* The region is write-combined: this CPU reads its own stores back in program order, no maintenance needed. */
            if(memcmp(target,source,blk.size)) {
                uint32_t at=0;
                while(at<blk.size && target[at]==source[at])++at;
                at&=~3u;
                uint32_t read=0,expect=0;
                for(uint32_t k=0;k<4 && at+k<blk.size;++k) {read|=uint32_t(target[at+k])<<(8*k);expect|=uint32_t(source[at+k])<<(8*k);}
                verified.mismatch=true;verified.addr=blk.addr+at;verified.read=read;verified.expect=expect;
                return kIOReturnIOError;
            }
            verified.ddr_bytes+=blk.size;
            continue;
        }
        if(!(what&A830GmuLoadTcm))continue;
        const uint32_t base=tcmOffset(blk.addr);
        if(base==~0u)return kIOReturnBadArgument;
        for(uint32_t word=0;word<blk.size/4;++word)
            if(io.write32(io.cookie,A830GmuWindowGmu,base+4*word,le32(source+4*word)))return kIOReturnIOError;
        tcm=true;
        if(!verify)continue;
        /* The block's stores complete before its words are read back (annibale adreno8_settle, dsb sy). */
        completeStores();
        if(io.barrier)io.barrier(io.cookie);
        for(uint32_t word=0;word<blk.size/4;++word) {
            const uint32_t expect=le32(source+4*word);
            uint32_t read=0;
            const bool failed=io.read32(io.cookie,A830GmuWindowGmu,base+4*word,&read)!=0;
            if(failed || read!=expect) {
                verified.mismatch=!failed;verified.read_failed=failed;
                verified.addr=blk.addr+4*word;verified.read=failed?0xffffffffu:read;verified.expect=expect;
                return kIOReturnIOError;
            }
            ++verified.tcm_words;
        }
    }
    completeStores();
    if(tcm && io.barrier)io.barrier(io.cookie);
    return kIOReturnSuccess;
}

IOReturn A830GmuFirmware::checkItcm(const A830GmuBlocks &blocks,A830GmuIo &io,uint32_t words[4])
{
    if(!words || !io.read32)return kIOReturnBadArgument;
    bool same=true;
    for(uint32_t i=0;i<4;++i) {
        words[i]=0;
        if(io.read32(io.cookie,A830GmuWindowGmu,tcmOffset(NativeAdrenoGmuItcmIova+4*i),&words[i]))return kIOReturnIOError;
        same=same && words[i]==blocks.itcm[i];
    }
    return same?kIOReturnSuccess:kIOReturnNotReady;
}
