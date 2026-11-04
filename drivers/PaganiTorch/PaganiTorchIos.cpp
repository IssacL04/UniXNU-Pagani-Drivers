// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The kernel half of the iOS flashlight (TorchIos.h): the service CMCapture looks for, its user client, and /product/camera "flash".
// pagani_torch_start calls the plan (Stage7Probe runs it during the kernel's start, in bsd_autoconf): boot arguments only. load_init_program
// calls the commit after the overlay mount, on the thread that goes on to exec launchd. The classes are registered there: this file's
// class initializer is taken out of __mod_init_func by scripts/build-pagani-torch.py, so a kernel without the boot argument has no such
// class and no such service. The service attaches to the resource service like PaganiAudio's; the node is made the way IODeviceTreeAlloc
// makes the others, in the IODeviceTree plane only, like PaganiHaptics' /product/haptics. The user client takes one call, the level, and
// switches the LED off when its process goes away. Without pagani-torch-ios nothing here runs and nothing is logged; a refusal or failure
// is logged and leaves the tree as it was. Never panics.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
extern "C" {
#include "TorchIos.h"
#include "TorchRegs.h"
#include "TorchText.h"
#include "PaganiTorch.h"
void boot_progress_trace(const char *line);   // BootProgress's, or PaganiTorch.cpp's weak default
void proc_selfname(char *buf, int size);
volatile int32_t pagani_torch_ios_status[4];
extern kmod_info_t g_kernel_kmod_info;
void pagani_torch_ios_static_init(void) __asm__("__GLOBAL__sub_I_PaganiTorchIos.cpp");
}

class AppleH6CamIn : public IOService {
    OSDeclareDefaultStructors(AppleH6CamIn)
};
OSDefineMetaClassAndStructors(AppleH6CamIn, IOService)

class PaganiTorchUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PaganiTorchUserClient)
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        char name[17];
        pgt_zero(name, sizeof name);
        proc_selfname(name, sizeof name);
        static const char only[] = PGT_IOS_PROCESS;
        for (unsigned i = 0; i < sizeof only; ++i)
            if (name[i] != only[i]) return false;
        // The boot log's proof that the plug-in runs in mediaserverd; a process that restarts over and over does not fill it.
        if (__atomic_fetch_add(&pagani_torch_ios_status[1], 1, __ATOMIC_RELAXED) < 3) {
            boot_progress_trace("[torch] ios: mediaserverd opened the service");
            IOLog("[torch] ios: mediaserverd opened the service\n");
        }
        return IOUserClient::initWithTask(task, security, type, properties);
    }
    bool start(IOService *provider) override {
        return OSDynamicCast(AppleH6CamIn, provider) && IOUserClient::start(provider);
    }
    IOReturn clientClose() override {
        (void)pagani_torch_request(0, "ios-close");   // the plug-in's process went away: the LED does not stay on behind it
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if (selector != PGT_IOS_SELECTOR_SET || args->scalarInputCount != 1 || args->structureInputSize) return kIOReturnBadArgument;
        const int level = pgt_ios_level(args->scalarInput[0]);
        if (level < 0) return kIOReturnBadArgument;
        __atomic_fetch_add(&pagani_torch_ios_status[2], 1, __ATOMIC_RELAXED);
        const int rc = pagani_torch_request((uint32_t)level, "ios");
        if (rc) __atomic_fetch_add(&pagani_torch_ios_status[3], 1, __ATOMIC_RELAXED);
        return rc == 0 ? kIOReturnSuccess : rc == -19 ? kIOReturnNotReady : rc == -110 ? kIOReturnTimeout : kIOReturnError;
    }
};
OSDefineMetaClassAndStructors(PaganiTorchUserClient, IOUserClient)

namespace {
volatile uint32_t gIosDone, gIosCommitted;
PgtIosPlan gIosPlan;

void say(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

void fail(int32_t code, const char *what)
{
    pagani_torch_ios_status[0] = code;
    char line[160];
    PgtText t;
    pgt_text_start(&t, line, sizeof line);
    pgt_text_str(&t, "[torch] ios not made: ");
    pgt_text_str(&t, what);
    say(line);
}

bool registerClasses()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    pagani_torch_ios_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}

bool publishService()
{
    IOService *home = IOService::getResourceService();
    AppleH6CamIn *service = OSTypeAlloc(AppleH6CamIn);
    if (!service || !home || !service->init() || !service->attach(home)) {
        if (service) service->release();
        return false;
    }
    // mediaserverd's sandbox names no class of ours: the kernel (zzw-native-adreno-sandbox-grant) takes this property in place of its verdict.
    service->setProperty("NativeSandboxGrantProcess", PGT_IOS_PROCESS);
    service->setProperty("IOUserClientClass", PGT_IOS_CLIENT);
    if (!service->start(home)) {
        service->detach(home);
        service->release();
        return false;
    }
    service->registerService();
    service->release();   // the resource service's attachment keeps it until reboot
    return true;
}

bool putFlash(IORegistryEntry *node)
{
    const uint32_t one = 1;
    OSData *data = OSData::withBytes(&one, sizeof one);
    if (!data) return false;
    const bool ok = node->setProperty(PGT_IOS_FLASH, data);
    data->release();
    return ok;
}

// /product/camera "flash": on the node when the tree has one, else on a new node.
int makeNode()
{
    IORegistryEntry *product = IORegistryEntry::fromPath(PGT_IOS_PARENT, gIODTPlane);
    if (!product) return -2;
    IORegistryEntry *existing = product->childFromPath(PGT_IOS_NODE, gIODTPlane);
    if (existing) {
        const bool ok = putFlash(existing);
        existing->release();
        product->release();
        return ok ? 0 : -3;
    }
    IOService *node = new IOService;
    if (node && !node->init()) {
        node->release();
        node = nullptr;
    }
    if (!node) {
        product->release();
        return -3;
    }
    static const char name[] = PGT_IOS_NODE;
    OSData *nameData = OSData::withBytes(name, sizeof name);
    bool ok = nameData && node->setProperty("name", nameData);
    if (nameData) nameData->release();
    node->setName(PGT_IOS_NODE);
    ok = ok && putFlash(node) && node->attachToParent(product, gIODTPlane);
    node->release();      // the plane keeps it once attached
    product->release();
    return ok ? 0 : -3;
}
}  // namespace

extern "C" void pagani_torch_ios_plan(unsigned ios, unsigned torch)
{
    if (!ios || __atomic_exchange_n(&gIosDone, 1u, __ATOMIC_ACQ_REL)) return;
    if (pgt_ios_plan(ios, torch, &gIosPlan) || !gIosPlan.create) return fail(-1, gIosPlan.why);
    pagani_torch_ios_status[0] = 1;
}

extern "C" void pagani_torch_ios_commit(int plugin_visible)
{
    if (!gIosPlan.create || __atomic_exchange_n(&gIosCommitted, 1u, __ATOMIC_ACQ_REL)) return;
    // The driver's thread started in bsd_autoconf and has long finished; a short bounded wait covers a slow bus.
    int ready = 0;
    for (unsigned ms = 0; ms < 100 && !ready; ++ms) {
        ready = pagani_torch_level() >= 0;
        if (!ready) IOSleep(1);
    }
    PgtIosPlan plan;
    if (pgt_ios_final(&gIosPlan, ready, plugin_visible, &plan) || !plan.create) return fail(-5, plan.why);
    if (!registerClasses() || !publishService()) return fail(-4, "the service was not published");
    const int rc = makeNode();
    if (rc) return fail(rc, rc == -2 ? "no /product in the device tree" : "a property or the attach to /product failed");
    pagani_torch_ios_status[0] = 2;
    say("[torch] ios: " PGT_IOS_SERVICE " published, /product/camera flash set");
}
