/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoPwrup.h"
#include <IOKit/IOLib.h>
#include <string.h>

size_t NativeAdrenoPwrup::requiredBytes(const NativeAdrenoPwrupCounts &counts)
{
    return HeaderBytes+size_t(counts.ifpc+counts.preemption)*PairBytes+size_t(counts.dynamic)*TripletBytes;
}
IOReturn NativeAdrenoPwrup::pack(const NativeAdrenoPwrupCounts &counts,
    const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
    const uint32_t *preemption_regs,const uint32_t *preemption_values,
    const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written)
{
    if(!out || !written)return kIOReturnBadArgument;
    *written=0;
    if(counts.ifpc>0xff || counts.preemption>0xff || counts.dynamic>0xffff)return kIOReturnBadArgument;
    const size_t needed=requiredBytes(counts);
    if(capacity<needed)return kIOReturnNoResources;
    if((counts.ifpc && (!ifpc_regs || !ifpc_values)) || (counts.preemption && (!preemption_regs || !preemption_values)) ||
        (counts.dynamic && !dynamic_triplets))return kIOReturnBadArgument;
    memset(out,0,needed);
    /* gpu_req/cpu_req/turn stay zero; the length bytes follow (little endian, as the CP reads the struct). */
    out[12]=static_cast<uint8_t>(counts.ifpc);
    out[13]=static_cast<uint8_t>(counts.preemption);
    out[14]=static_cast<uint8_t>(counts.dynamic&0xff);
    out[15]=static_cast<uint8_t>((counts.dynamic>>8)&0xff);
    uint8_t *cursor=out+HeaderBytes;
    auto put=[&cursor](uint32_t value) {memcpy(cursor,&value,4);cursor+=4;};
    for(uint32_t i=0;i<counts.ifpc;++i) {put(ifpc_regs[i]);put(ifpc_values[i]);}
    for(uint32_t i=0;i<counts.preemption;++i) {put(preemption_regs[i]);put(preemption_values[i]);}
    for(uint32_t i=0;i<counts.dynamic;++i) {put(dynamic_triplets[i*3]);put(dynamic_triplets[i*3+1]);put(dynamic_triplets[i*3+2]);}
    *written=needed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoPwrup::buildGen8(const A830PwrupTables &tables,void *page,size_t bytes,size_t *written)
{
    if(written)*written=0;
    if(!page)return kIOReturnBadArgument;
    for(uint32_t i=0;i<tables.dynamic_count;++i) {
        if(!tables.dynamic)return kIOReturnBadArgument;
        const uint32_t aperture=tables.dynamic[i*3];
        /* CP_APERTURE_CNTL_HOST PIPEID only: BR (1) .. DDE_BV (7); no slice selection, no PIPE_NONE entry. */
        if((aperture&~0xf000u) || !(aperture>>12) || (aperture>>12)>7)return kIOReturnBadArgument;
    }
    const NativeAdrenoPwrupCounts counts={tables.ifpc_count,tables.pwrup_count,tables.dynamic_count};
    size_t image=0;
    const IOReturn status=pack(counts,tables.ifpc_regs,tables.ifpc_values,tables.pwrup_regs,tables.pwrup_values,
        tables.dynamic,static_cast<uint8_t *>(page),bytes,&image);
    if(status)return status;
    memset(static_cast<uint8_t *>(page)+image,0,bytes-image);
    if(written)*written=image;
    return kIOReturnSuccess;
}
