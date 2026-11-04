/* SPDX-License-Identifier: MIT */
#include "A830Zap.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include <string.h>

/* The constants of the header are the same facts A830Regs.h records (ARCHITECTURE 4.11). */
static_assert(uint32_t(A830ZapPasId)==A830_ZAP_PAS_ID && uint64_t(A830ZapRegionPhys)==A830_ZAP_PHYS &&
              uint32_t(A830ZapRegionBytes)==A830_ZAP_BYTES && uint32_t(A830ZapPasId)==uint32_t(Sm8750ScmGpuPasId),
              "ZAP region and PAS id");
static_assert(A830_ZAP_METADATA_BYTES<=uint32_t(A830ZapMetadataMax) && A830_ZAP_MEM_SETUP_BYTES<=uint32_t(A830ZapRegionBytes),
              "PKX110's metadata fits one page and its span fits the region");

namespace {
enum : uint32_t {ElfHeaderBytes=52,ProgramHeaderBytes=32,SectionHeaderBytes=40,PtLoad=1};

uint16_t rd16(const uint8_t *p) {return uint16_t(p[0]|p[1]<<8);}
uint32_t rd32(const uint8_t *p) {return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}

struct ProgramHeader {uint32_t type,offset,vaddr,paddr,filesz,memsz,flags,align;};
ProgramHeader programHeader(const uint8_t *image,uint32_t phoff,uint32_t index)
{
    const uint8_t *p=image+phoff+size_t(index)*ProgramHeaderBytes;
    return {rd32(p),rd32(p+4),rd32(p+8),rd32(p+12),rd32(p+16),rd32(p+20),rd32(p+24),rd32(p+28)};
}

/* mdt_phdr_loadable */
bool loadable(const ProgramHeader &ph) {return ph.type==PtLoad && (ph.flags&A830ZapMdtTypeMask)!=A830ZapMdtTypeHash && ph.memsz;}

IOReturn refuse(A830ZapImage *out,A830ZapError error) {out->error=error;return kIOReturnBadArgument;}

uint64_t roundPage(uint64_t value) {return (value+A830ZapPageBytes-1)&~uint64_t(A830ZapPageBytes-1);}

/* Where the span of the image starts inside the region: relocatable images are moved to the region's start
 * (qcom_mdt_load_no_init: mem_reloc = lowest address), others keep their addresses (mem_reloc = the region). */
bool spanInRegion(const A830ZapImage &image,uint64_t region,uint64_t bytes,uint64_t *first,uint64_t *end)
{
    const uint64_t base=image.relocatable?image.min_paddr:region;
    if(image.min_paddr<base || image.max_paddr<image.min_paddr)return false;
    *first=image.min_paddr-base;
    *end=*first+(image.max_paddr-image.min_paddr);
    return *end<=bytes && *first<*end;
}

IOReturn ioreturnOf(int status)
{
    switch(status) {
    case Sm8750ScmOk: return kIOReturnSuccess;
    case Sm8750ScmArgument: return kIOReturnBadArgument;
    case Sm8750ScmBusy: return kIOReturnBusy;
    case Sm8750ScmTimeout: return kIOReturnTimeout;
    default: return kIOReturnIOError;
    }
}
}

IOReturn A830Zap::parseMdt(const uint8_t *image,size_t length,A830ZapImage *out)
{
    if(!out)return kIOReturnBadArgument;
    *out={};
    out->bytes=image;out->length=length;
    if(!image)return refuse(out,A830ZapErrorShort);
    /* mdt_header_valid: an ELF32 header, 32-byte program headers inside the file, section headers (if any) inside it. */
    if(length<ElfHeaderBytes)return refuse(out,A830ZapErrorShort);
    if(image[0]!=0x7f || image[1]!='E' || image[2]!='L' || image[3]!='F' || image[4]!=1 /* ELFCLASS32 */ ||
       image[5]!=1 /* ELFDATA2LSB */ || image[6]!=1 /* EV_CURRENT */)return refuse(out,A830ZapErrorIdent);
    const uint32_t phoff=rd32(image+28),shoff=rd32(image+32);
    const uint32_t phentsize=rd16(image+42),phnum=rd16(image+44),shentsize=rd16(image+46),shnum=rd16(image+48);
    out->entry=rd32(image+24);out->phnum=phnum;
    if(phentsize!=ProgramHeaderBytes || phnum>A830ZapMaxProgramHeaders ||
       uint64_t(phoff)+uint64_t(phnum)*ProgramHeaderBytes>length)return refuse(out,A830ZapErrorHeaders);
    if((shentsize || shnum) &&
       (shentsize!=SectionHeaderBytes || uint64_t(shoff)+uint64_t(shnum)*SectionHeaderBytes>length))
        return refuse(out,A830ZapErrorHeaders);
    /* qcom_mdt_read_metadata: at least two headers, the first not PT_LOAD, the first hash-type header after it. */
    if(phnum<2 || programHeader(image,phoff,0).type==PtLoad)return refuse(out,A830ZapErrorFirstHeader);
    uint32_t hash=0;
    for(uint32_t i=1;i<phnum && !hash;++i)
        if((programHeader(image,phoff,i).flags&A830ZapMdtTypeMask)==A830ZapMdtTypeHash)hash=i;
    if(!hash)return refuse(out,A830ZapErrorHash);
    const ProgramHeader first=programHeader(image,phoff,0),hashHeader=programHeader(image,phoff,hash);
    out->header_bytes=first.filesz;
    out->hash_index=hash;out->hash_bytes=hashHeader.filesz;
    if(out->header_bytes>length)return refuse(out,A830ZapErrorMetadata);
    /* The hash follows the header part directly in a split image (header + hash is the whole file); otherwise it is
     * its own segment inside the file. A hash segment beyond the file (a split .bNN file) is not supported. */
    if(uint64_t(out->header_bytes)+out->hash_bytes==length)out->hash_offset=out->header_bytes;
    else if(uint64_t(hashHeader.offset)+out->hash_bytes<=length)out->hash_offset=hashHeader.offset;
    else return refuse(out,A830ZapErrorMetadata);
    const uint64_t metadata=uint64_t(out->header_bytes)+out->hash_bytes;
    if(metadata>A830ZapMetadataMax)return refuse(out,A830ZapErrorMetadata);
    out->metadata_bytes=uint32_t(metadata);
    /* qcom_mdt_get_size / __qcom_mdt_pas_init over the loadable segments; ARCHITECTURE 4.11 wants exactly one. */
    uint64_t low=UINT64_MAX,high=0;
    for(uint32_t i=0;i<phnum;++i) {
        const ProgramHeader ph=programHeader(image,phoff,i);
        if(!loadable(ph))continue;
        if(!out->loads)out->load={i,ph.offset,ph.paddr,ph.filesz,ph.memsz,ph.flags};
        ++out->loads;
        if(ph.flags&A830ZapMdtRelocatable)out->relocatable=true;
        if(ph.paddr<low)low=ph.paddr;
        const uint64_t end=roundPage(uint64_t(ph.paddr)+ph.memsz);
        if(end>high)high=end;
    }
    if(out->loads!=1)return refuse(out,A830ZapErrorLoad);
    const A830ZapSegment &load=out->load;
    if(load.filesz>load.memsz || (load.filesz && uint64_t(load.offset)+load.filesz>length))
        return refuse(out,A830ZapErrorSegment);
    if(low&(A830ZapPageBytes-1))return refuse(out,A830ZapErrorAlignment);
    out->min_paddr=low;out->max_paddr=high;
    if(high-low>A830ZapRegionBytes)return refuse(out,A830ZapErrorSpan);   /* no region of this SoC is larger */
    out->span=uint32_t(high-low);
    return kIOReturnSuccess;
}

IOReturn A830Zap::buildMetadata(const A830ZapImage &image,uint8_t *out,size_t capacity,size_t *bytes)
{
    if(bytes)*bytes=0;
    if(!out || !image.bytes || image.error || !image.metadata_bytes || image.metadata_bytes>capacity ||
       uint64_t(image.header_bytes)+image.hash_bytes!=image.metadata_bytes ||
       image.header_bytes>image.length || uint64_t(image.hash_offset)+image.hash_bytes>image.length)
        return kIOReturnBadArgument;
    memcpy(out,image.bytes,image.header_bytes);
    memcpy(out+image.header_bytes,image.bytes+image.hash_offset,image.hash_bytes);
    memset(out+image.metadata_bytes,0,capacity-image.metadata_bytes);
    if(bytes)*bytes=image.metadata_bytes;
    return kIOReturnSuccess;
}

IOReturn A830Zap::loadSegments(const A830ZapImage &image,volatile uint8_t *region,uint64_t region_phys,uint64_t region_bytes)
{
    uint64_t first=0,end=0;
    if(!region || !image.bytes || image.error || image.loads!=1 || !spanInRegion(image,region_phys,region_bytes,&first,&end))
        return kIOReturnBadArgument;
    const A830ZapSegment &load=image.load;
    const uint64_t base=image.relocatable?image.min_paddr:region_phys;
    const uint64_t at=load.paddr-base;
    if(load.paddr<base || at+load.memsz>region_bytes || load.filesz>load.memsz ||
       uint64_t(load.offset)+load.filesz>image.length)return kIOReturnBadArgument;
    /* The whole span is written: zeros first ("the rest zero-filled", ARCHITECTURE 4.11 step 5; qcom_mdt_load_no_init
     * zeroes p_memsz - p_filesz), then the file bytes. Byte stores: the mapping may be write-combined or Device. */
    for(uint64_t i=first;i<end;++i)region[i]=0;
    for(uint64_t i=0;i<load.filesz;++i)region[at+i]=image.bytes[load.offset+i];
    return kIOReturnSuccess;
}

IOReturn A830Zap::init(Sm8750Scm *scm,const A830ZapMemoryOps *memory)
{
    if(!scm || !memory || !memory->metadataAlloc || !memory->metadataFree || !memory->regionMap || !memory->regionUnmap ||
       !memory->barrier)return kIOReturnBadArgument;
    scm_=scm;memory_=memory;
    return kIOReturnSuccess;
}

void A830Zap::fail(uint16_t step,uint16_t reason,uint64_t reg,uint64_t value)
{
    report_.step=step;failed_reason_=reason;failed_reg_=reg;failed_value_=value;
}

void A830Zap::account(const Sm8750SmcResult &result)
{
    report_.scm=result.status;report_.fid=result.fid;report_.x0=result.x0;report_.x1=result.x1;
    report_.smcs+=result.calls;report_.waitq+=result.waitq;report_.interrupted+=result.interrupted;report_.busy+=result.busy;
    report_.elapsed_us+=result.elapsed_us;
}

IOReturn A830Zap::smcFailed(uint16_t step,int status,const Sm8750SmcResult &result)
{
    /* The value that decided it: x0 when the call itself failed, else the PAS result in x1. */
    fail(step,A830ReasonSmc,result.fid,result.x0?uint64_t(result.x0):result.x1);
    return ioreturnOf(status);
}

IOReturn A830Zap::authenticate(const A830ZapImage &image,uint64_t region,uint64_t bytes,Sm8750SmcResult out[3])
{
    Sm8750SmcResult local[3];
    Sm8750SmcResult *results=out?out:local;
    for(unsigned i=0;i<3;++i)results[i]={};
    if(!scm_ || !memory_) {fail(A830ZapStepMetadata,A830ReasonArgument,0,0);return kIOReturnNotReady;}
    if(loaded_)return kIOReturnSuccess;
    if(poisoned_) {fail(report_.step,A830ReasonUnexpectedState,report_.fid,report_.metadata_phys);return kIOReturnNotReady;}
    uint64_t first=0,end=0;
    if(!image.bytes || image.error || image.loads!=1 || !region || (region&(A830ZapPageBytes-1)) || !bytes ||
       !spanInRegion(image,region,bytes,&first,&end)) {
        report_.error=image.error?image.error:uint16_t(A830ZapErrorSpan);
        fail(A830ZapStepParse,A830ReasonFirmware,report_.error,image.span);
        return kIOReturnBadArgument;
    }
    report_.metadata_bytes=image.metadata_bytes;report_.span=image.span;

    /* 2. The metadata page: physically contiguous, 4 KiB aligned, written through its write-combined mapping. */
    report_.step=A830ZapStepMetadata;adreno830_status_step(report_.step);
    void *cpu=nullptr,*handle=nullptr;
    uint64_t phys=0;
    IOReturn status=memory_->metadataAlloc(memory_->cookie,A830ZapMetadataMax,&cpu,&phys,&handle);
    if(status!=kIOReturnSuccess || !cpu || !phys || (phys&(A830ZapPageBytes-1))) {
        if(status==kIOReturnSuccess) {memory_->metadataFree(memory_->cookie,handle);status=kIOReturnNoMemory;}
        fail(A830ZapStepMetadata,A830ReasonNoMemory,A830ZapMetadataMax,phys);
        return status;
    }
    size_t built=0;
    status=buildMetadata(image,static_cast<uint8_t *>(cpu),A830ZapMetadataMax,&built);
    if(status!=kIOReturnSuccess) {
        memory_->metadataFree(memory_->cookie,handle);
        fail(A830ZapStepMetadata,A830ReasonFirmware,A830ZapErrorMetadata,image.metadata_bytes);
        return status;
    }
    memory_->barrier(memory_->cookie);
    report_.metadata_phys=phys;
    metadata_handle_=handle;

    /* From PAS_INIT_IMAGE on TZ may hold an XPU over the metadata page: any failure keeps it and ends ZAP loading for
     * this boot (ARCHITECTURE 4.11 step 7). */
    const auto heldFailure=[&](uint16_t step,int scm,const Sm8750SmcResult &result) {
        poisoned_=true;report_.metadata_held=true;
        return smcFailed(step,scm,result);
    };

    /* 3. PAS_INIT_IMAGE(13, metadata). */
    report_.step=A830ZapStepInitImage;adreno830_status_step(report_.step);
    int scm=Sm8750ScmPasInitImage(scm_,A830ZapPasId,phys,&results[0]);
    account(results[0]);
    if(scm!=Sm8750ScmOk)return heldFailure(A830ZapStepInitImage,scm,results[0]);

    /* 4. PAS_MEM_SETUP(13, region, span) for a relocatable image (__qcom_mdt_pas_init). */
    report_.step=A830ZapStepMemSetup;adreno830_status_step(report_.step);
    if(image.relocatable) {
        scm=Sm8750ScmPasMemSetup(scm_,A830ZapPasId,region,image.span,&results[1]);
        account(results[1]);
        if(scm!=Sm8750ScmOk)return heldFailure(A830ZapStepMemSetup,scm,results[1]);
    }

    /* 5. The segment into the region through a write-combined mapping of the span, then dsb sy. */
    report_.step=A830ZapStepLoad;adreno830_status_step(report_.step);
    void *mapped=nullptr,*mapping=nullptr;
    status=memory_->regionMap(memory_->cookie,region,end,&mapped,&mapping);
    if(status!=kIOReturnSuccess || !mapped) {
        if(status==kIOReturnSuccess) {memory_->regionUnmap(memory_->cookie,mapping);status=kIOReturnNoMemory;}
        poisoned_=true;report_.metadata_held=true;
        fail(A830ZapStepLoad,A830ReasonNoMemory,region,end);
        return status;
    }
    status=loadSegments(image,static_cast<volatile uint8_t *>(mapped),region,end);
    memory_->barrier(memory_->cookie);
    if(status!=kIOReturnSuccess) {
        memory_->regionUnmap(memory_->cookie,mapping);
        poisoned_=true;report_.metadata_held=true;
        fail(A830ZapStepLoad,A830ReasonFirmware,A830ZapErrorSpan,end);
        return status;
    }

    /* 6. PAS_AUTH_AND_RESET(13); the CPU view of the region goes as soon as it returns. */
    report_.step=A830ZapStepAuthReset;adreno830_status_step(report_.step);
    scm=Sm8750ScmPasAuthAndReset(scm_,A830ZapPasId,&results[2]);
    memory_->regionUnmap(memory_->cookie,mapping);
    account(results[2]);
    if(scm!=Sm8750ScmOk)return heldFailure(A830ZapStepAuthReset,scm,results[2]);

    /* 7. Authenticated: the metadata page goes back. */
    report_.step=A830ZapStepRelease;adreno830_status_step(report_.step);
    memory_->metadataFree(memory_->cookie,metadata_handle_);
    metadata_handle_=nullptr;
    loaded_=true;report_.loads=1;
    failed_reason_=A830ReasonNone;failed_reg_=failed_value_=0;
    return kIOReturnSuccess;
}

IOReturn A830Zap::load(const uint8_t *image,size_t length,uint64_t region,uint64_t bytes)
{
    if(loaded_)return kIOReturnSuccess;
    if(poisoned_) {
        fail(report_.step,A830ReasonUnexpectedState,report_.fid,report_.metadata_phys);
        noteStatus();
        return kIOReturnNotReady;
    }
    const uint32_t resumes=report_.resumes,resumeFailures=report_.resume_failures;
    report_={};report_.resumes=resumes;report_.resume_failures=resumeFailures;
    failed_reason_=A830ReasonNone;failed_reg_=failed_value_=0;
    report_.step=A830ZapStepParse;adreno830_status_step(report_.step);
    A830ZapImage parsed;
    IOReturn status=parseMdt(image,length,&parsed);
    if(status!=kIOReturnSuccess) {
        report_.error=parsed.error;
        fail(A830ZapStepParse,A830ReasonFirmware,parsed.error,length);
    } else status=authenticate(parsed,region,bytes,nullptr);
    noteStatus();
    return status;
}

IOReturn A830Zap::resume(A830ZapResume mode)
{
    if(!loaded_ || !scm_) {
        fail(A830ZapStepRemoteState,A830ReasonUnexpectedState,0,loaded_);
        return kIOReturnNotReady;
    }
    if(mode==A830ZapResumeSecureMode)return kIOReturnSuccess;
    if(mode!=A830ZapResumeRemoteState)return kIOReturnBadArgument;
    Sm8750SmcResult result;
    report_.step=A830ZapStepRemoteState;
    const int scm=Sm8750ScmPasSetRemoteState(scm_,Sm8750ScmZapShaderResume,A830ZapPasId,&result);
    report_.scm=result.status;report_.fid=result.fid;report_.x0=result.x0;report_.x1=result.x1;
    report_.smcs+=result.calls;report_.waitq+=result.waitq;report_.interrupted+=result.interrupted;report_.busy+=result.busy;
    if(scm!=Sm8750ScmOk) {
        ++report_.resume_failures;
        return smcFailed(A830ZapStepRemoteState,scm,result);
    }
    ++report_.resumes;
    failed_reason_=A830ReasonNone;failed_reg_=failed_value_=0;
    return kIOReturnSuccess;
}

void A830Zap::failure(A830ZapFailure *out) const
{
    if(!out)return;
    *out={report_.step,failed_reason_,failed_reg_,failed_value_,0};
}

void A830Zap::noteStatus() const
{
    Adreno830Status *s=&adreno830_status;
    /* Steps 8-12 are A830Cp's and the orchestrator's: a later S7 step already in the block stays, so the row of a
     * complete secure exit reads "step 12" (PLAN M5 acceptance). */
    uint16_t step=report_.step;
    if(s->stage==A830SZap && s->step>step && s->step<=A830ZapStepOobClear)step=s->step;
    adreno830_status_step(step);
    s->aux[A830ZapAuxX0]=uint64_t(report_.x0);
    s->aux[A830ZapAuxWord]=uint64_t(uint32_t(report_.x1))|uint64_t(step&0xff)<<32|
        uint64_t(report_.waitq<255?report_.waitq:255)<<40|uint64_t(report_.smcs<0xffff?report_.smcs:0xffff)<<48;
}

#ifndef ADRENO830_HOST_TEST
#include "NativeAdrenoMemory.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace {
NativeAdrenoMemory zapMetadata;   /* one page per boot; kept for good when a PAS call failed */
struct RegionMapping {IOMemoryDescriptor *descriptor;IOMemoryMap *map;} zapRegion;

IOReturn kernelMetadataAlloc(void *,size_t bytes,void **cpu,uint64_t *phys,void **handle)
{
    if(!cpu || !phys || !handle || !bytes || bytes>A830ZapPageBytes)return kIOReturnBadArgument;
    if(zapMetadata.backed())return kIOReturnBusy;
    /* 40 output bits: any DRAM page of this SoC; XNU's DRAM starts at 0x9_8000_0000. */
    IOReturn status=zapMetadata.allocate(A830ZapPageBytes,40,true,A830ZapPageBytes);
    if(status!=kIOReturnSuccess)return status;
    if(zapMetadata.spanCount()!=1 || !zapMetadata.mapping()) {(void)zapMetadata.release();return kIOReturnUnsupported;}
    /* Cleaned, invalidated and remapped write-combined: no dirty line of the page can be written back once TZ owns it. */
    status=zapMetadata.makeWriteCombined();
    if(status!=kIOReturnSuccess) {(void)zapMetadata.release();return status;}
    *cpu=zapMetadata.mapping();*phys=zapMetadata.spans()[0].physical;*handle=&zapMetadata;
    return kIOReturnSuccess;
}

void kernelMetadataFree(void *,void *handle)
{
    if(handle==&zapMetadata)(void)zapMetadata.release();
}

IOReturn kernelRegionMap(void *,uint64_t phys,uint64_t bytes,void **cpu,void **handle)
{
    if(!cpu || !handle || !phys || !bytes || ((phys|bytes)&(A830ZapPageBytes-1)))return kIOReturnBadArgument;
    if(zapRegion.map)return kIOReturnBusy;
    /* The region is reserved no-map memory outside XNU's DRAM: a physical descriptor, mapped write-combined. */
    IOMemoryDescriptor *descriptor=IOMemoryDescriptor::withAddressRange(phys,bytes,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone),TASK_NULL);
    if(!descriptor)return kIOReturnNoMemory;
    IOMemoryMap *map=descriptor->createMappingInTask(kernel_task,0,kIOMapAnywhere|kIOMapWriteCombineCache);
    if(!map) {descriptor->release();return kIOReturnNoMemory;}
    zapRegion={descriptor,map};
    *cpu=reinterpret_cast<void *>(map->getVirtualAddress());*handle=&zapRegion;
    return kIOReturnSuccess;
}

void kernelRegionUnmap(void *,void *handle)
{
    if(handle!=&zapRegion || !zapRegion.map)return;
    zapRegion.map->release();zapRegion.descriptor->release();zapRegion={};
}

void kernelBarrier(void *) {__asm__ volatile("dsb sy":::"memory");}

const A830ZapMemoryOps kernelMemory={nullptr,kernelMetadataAlloc,kernelMetadataFree,kernelRegionMap,kernelRegionUnmap,
                                     kernelBarrier};
}

extern "C" const A830ZapMemoryOps *A830ZapKernelMemory(void) {return &kernelMemory;}
#endif
