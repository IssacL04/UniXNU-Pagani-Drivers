// SPDX-License-Identifier: MIT
// The registry-visible accelerator for the original Metal discovery (IOServiceMatching("IOAcceleratorES") ->
// MetalPluginName -> /System/Library/Extensions/<name>.bundle). It derives from the original IOAcceleratorES base exactly as
// qualified in drivers/AdrenoMetal/OriginalAcceleratorABI.h (drivers/NativeMetal is only the official alias of that component,
// ARCHITECTURE 2.2): the original base has no operational overrides and no fields, so this class adds no field either (136 bytes,
// native_adreno_accelerator_instance_size) and overrides only existing IOService virtuals (no new vtable entry): newUserClient,
// the property reads below (diagnostic counting only) and registerService (one more property). Its static initializer is staged
// out of __mod_init_func and run by the orchestrator's publishMetal after the original IOGPUFamily kext has loaded, because the
// parent metaclass does not exist before that. The class is named after the Apple accelerator class on purpose, so original
// sandbox profiles that name the service class accept the Adreno plugin's open; the Apple AGX kext is never loaded here.
#include "../AdrenoMetal/OriginalAcceleratorABI.h"
#include <IOKit/IOUserClient.h>
#include "NativeAdrenoPublish.h"

extern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);

/* The coherence level the plugin may skip cache maintenance for (user-space patch M8, ARCHITECTURE 4.14.1, 5.5): 0 none, 1 the clean
 * of GPU-read-only imports, 2 also the clean and invalidate of GPU-written surfaces. */
static const char NativeAdrenoCoherentImportsKey[]="NativeAdrenoCoherentImports";

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)
public:
    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties,
        IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSString *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const char *key) const APPLE_KEXT_OVERRIDE;
    void registerService(IOOptionBits options=0) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXAccelerator,IOAcceleratorES)

IOReturn AGXAccelerator::newUserClient(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties,
    IOUserClient **handler)
{
    /* sheng-gpu-metal-opens: connections ever granted to each process that runs the plugin (0 = unlimited; ARCHITECTURE 3.6). */
    unsigned limit=40;
    PE_parse_boot_argn("sheng-gpu-metal-opens",&limit,sizeof(limit));
    return native_adreno_new_user_client(this,owningTask,securityID,type,properties,handler,limit);
}
OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
    native_adreno_note_property_read(key?key->getCStringNoCopy():nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
    native_adreno_note_property_read(key?key->getCStringNoCopy():nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const char *key) const
{
    native_adreno_note_property_read(key);
    return IORegistryEntry::copyProperty(key);
}
void AGXAccelerator::registerService(IOOptionBits options)
{
    /* The plugin reads the level when it loads, right after the discovery finds this service: it must be set before registration.
     * publishMetal may have set it already (native_adreno_accelerator_set_coherent_imports); otherwise the shell's current level. */
    if(!getProperty(NativeAdrenoCoherentImportsKey))
        setProperty(NativeAdrenoCoherentImportsKey,(unsigned long long)native_adreno_coherent_imports(),32);
    setProperty(NativeAdrenoImportCleanKey,1ULL,32);
    /* IOService's, named directly: under -fapple-kext a qualified call goes through the named class's vtable, and the original base's
     * vtable is not one of the three audited imports (its registerService is IOService's: the base overrides nothing). */
    IOService::registerService(options);
}
extern "C" IOService *native_adreno_accelerator_allocate(void) {return new AGXAccelerator;}
extern "C" unsigned native_adreno_accelerator_instance_size(void) {return sizeof(AGXAccelerator);}
extern "C" void native_adreno_accelerator_set_coherent_imports(IOService *accelerator,uint32_t level)
{
    if(accelerator)accelerator->setProperty(NativeAdrenoCoherentImportsKey,(unsigned long long)(level>2?2:level),32);
}
