/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSmmuMemory.h"
#include <string.h>
static bool pageOfPool(const NativeAdrenoMemory *memory,volatile uint64_t *page,size_t bytes)
{
    if(!memory || !page || bytes!=4096 || ((uintptr_t)page&4095) ||
        (uintptr_t)page<(uintptr_t)memory->mapping())return false;
    const uintptr_t offset=(uintptr_t)page-(uintptr_t)memory->mapping();
    return offset<=memory->bytes() && bytes<=memory->bytes()-offset;
}
static int publishTable(void *cookie,volatile uint64_t *page,size_t bytes)
{
    auto *memory=static_cast<NativeAdrenoMemory *>(cookie);
    if(!pageOfPool(memory,page,bytes))return 1;
    const uintptr_t offset=(uintptr_t)page-(uintptr_t)memory->mapping();
    return memory->publishForDevice(offset,bytes,false)==kIOReturnSuccess?0:1;
}
/* The table walk is coherent (proved by V4): the descriptor stores need only be ordered before the walker can see them. */
static int publishTableCoherent(void *cookie,volatile uint64_t *page,size_t bytes)
{
    auto *memory=static_cast<NativeAdrenoMemory *>(cookie);
    if(!pageOfPool(memory,page,bytes) || memory->quarantined())return 1;
    NativeAdrenoStoreBarrier();
    return 0;
}
static IOReturn bindVisibility(NativeAdrenoMemory *memory,ArmSmmu4KLiveVisibility *visibility,
    int (*publish)(void *,volatile uint64_t *,size_t))
{
    if(!visibility)return kIOReturnBadArgument;*visibility={};
    if(!memory || memory->quarantined() || !memory->deviceReferences() || !memory->mapping() ||
        memory->spanCount()!=1 || memory->spans()[0].offset ||
        memory->spans()[0].bytes!=memory->bytes())return kIOReturnBadArgument;
    *visibility={memory,publish};return kIOReturnSuccess;
}
IOReturn NativeAdrenoBindSmmuVisibility(NativeAdrenoMemory *memory,ArmSmmu4KLiveVisibility *visibility)
{return bindVisibility(memory,visibility,publishTable);}
IOReturn NativeAdrenoBindSmmuVisibilityCoherent(NativeAdrenoMemory *memory,ArmSmmu4KLiveVisibility *visibility)
{return bindVisibility(memory,visibility,publishTableCoherent);}
IOReturn NativeAdrenoPrepareSmmuMemory(NativeAdrenoMemory *memory,unsigned input_bits,
    unsigned output_bits,uint64_t iova,uint64_t bytes,unsigned upper,size_t budget,
    ArmSmmu4KLiveArena *arena,ArmSmmu4KLiveState *state,ArmSmmu4KLiveVisibility *visibility)
{
    if(!memory || !arena || !state || !visibility)return kIOReturnBadArgument;
    *visibility={};
    if(state->prepared || state->poisoned || memory->quarantined() || memory->deviceReferences())
        return kIOReturnBusy;
    if(!memory->mapping() || memory->spanCount()!=1 ||
        memory->spans()[0].offset || memory->spans()[0].bytes!=memory->bytes() ||
        !memory->bytes() || (memory->bytes()&4095))return kIOReturnUnsupported;
    *arena={{input_bits,output_bits,memory->spans()[0].physical,budget},iova,bytes,upper,
        static_cast<volatile uint64_t *>(memory->mapping()),memory->bytes()/4096,nullptr,0};
    ArmSmmu4KStatus status=ArmSmmu4KLivePrepare(arena,state);
    if(status!=ArmSmmu4KSuccess)return status==ArmSmmu4KCapacity?kIOReturnNoResources:kIOReturnBadArgument;
    ArmSmmu4KLiveResult audit;
    status=ArmSmmu4KLiveAudit(arena,state,nullptr,0,&audit);
    if(status!=ArmSmmu4KSuccess) {state->poisoned=1;return kIOReturnIOError;}
    IOReturn result=memory->publishForDevice(0,state->tables.table_pages*4096,false);
    if(result==kIOReturnSuccess)result=memory->holdForDevice();
    if(result!=kIOReturnSuccess) {state->poisoned=1;return result;}
    return NativeAdrenoBindSmmuVisibility(memory,visibility);
}
IOReturn NativeAdrenoPrepareGuardTable(NativeAdrenoMemory *memory,unsigned output_bits)
{
    if(!memory)return kIOReturnBadArgument;
    /* One page, physically contiguous, below 2^output_bits; zero descriptors are invalid at every level, so a walk through
     * TTBR1 ends in a translation fault at the first level. Allocation zeroes and cleans it to PoC. */
    IOReturn status=memory->allocate(4096,output_bits,true,4096);
    if(status!=kIOReturnSuccess)return status;
    if(!memory->mapping() || memory->spanCount()!=1) {(void)memory->release();return kIOReturnUnsupported;}
    memset(memory->mapping(),0,4096);
    status=memory->publishForDevice(0,4096,false);
    if(status==kIOReturnSuccess)status=memory->holdForDevice();
    return status;
}
