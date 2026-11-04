// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 插件的进度进 U 行。mediaserverd 是 mobile 用户、带沙盒，写不了 kern.pagani_diag_line（只收 root）和 root 的 spool，os_log 这台手机又读不出来，所以测试图走到哪一步从外面看不见。
// 这里发布一个只有 mediaserverd 能打开的服务，用户客户端收一行文本，写成 "[cam] ios: ..."。写法照 PaganiTorchIos.cpp：类初始化由 scripts/build-pagani-camera.py 从
// __mod_init_func 摘掉，没有 pagani-torch-ios=1 的内核里没有这个类也没有这个服务；NativeSandboxGrantProcess 让沙盒放行（zzw-native-adreno-sandbox-grant）。
// 每次启动最多 PcamLogLines 行，失控的进程写不满日志环。不 panic。
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <mach/kmod.h>
extern "C" {
#include "CamUtil.h"
#include "PaganiCamera.h"
#include "ios/CamProvider.h"
void boot_progress_trace(const char *line);   // BootProgress 的，或 PaganiCamera.cpp 的弱默认
void proc_selfname(char *buf, int size);
extern kmod_info_t g_kernel_kmod_info;
void pagani_camera_ios_static_init(void) __asm__("__GLOBAL__sub_I_PaganiCameraIos.cpp");
void pagani_camera_ios_publish(void);
}

namespace {
enum {PcamLogLines = 48};
}

class PaganiCameraLog : public IOService {
    OSDeclareDefaultStructors(PaganiCameraLog)
};
OSDefineMetaClassAndStructors(PaganiCameraLog, IOService)

class PaganiCameraLogClient : public IOUserClient {
    OSDeclareDefaultStructors(PaganiCameraLogClient)
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        char name[17];
        pcam_zero(name, sizeof name);
        proc_selfname(name, sizeof name);
        static const char only[] = PCAM_PROCESS;
        for (unsigned i = 0; i < sizeof only; ++i)
            if (name[i] != only[i]) return false;
        return IOUserClient::initWithTask(task, security, type, properties);
    }
    bool start(IOService *provider) override {
        return OSDynamicCast(PaganiCameraLog, provider) && IOUserClient::start(provider);
    }
    IOReturn clientClose() override {
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if (selector != PCAM_LOG_SELECTOR || !args->structureInput || !args->structureInputSize || args->structureInputSize > PcamLogMax) return kIOReturnBadArgument;
        if (__atomic_fetch_add(&pagani_camera_status[9], 1, __ATOMIC_RELAXED) >= PcamLogLines) {
            __atomic_fetch_add(&pagani_camera_status[10], 1, __ATOMIC_RELAXED);
            return kIOReturnNoSpace;
        }
        char line[160];
        PcamText t;
        pcam_text_start(&t, line, sizeof line);
        pcam_text_str(&t, "[cam] ios: ");
        pcam_text_printable(&t, static_cast<const uint8_t *>(args->structureInput), args->structureInputSize);
        boot_progress_trace(line);
        IOLog("%s\n", line);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(PaganiCameraLogClient, IOUserClient)

namespace {
volatile uint32_t gPublished;
bool registerClasses()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    pagani_camera_ios_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}
}  // namespace

extern "C" void pagani_camera_ios_publish(void)
{
    if (__atomic_exchange_n(&gPublished, 1u, __ATOMIC_ACQ_REL)) return;
    if (!registerClasses()) {
        boot_progress_trace("[cam] ios: classes not registered");
        return;
    }
    IOService *home = IOService::getResourceService();
    PaganiCameraLog *service = OSTypeAlloc(PaganiCameraLog);
    if (!service || !home || !service->init() || !service->attach(home)) {
        if (service) service->release();
        boot_progress_trace("[cam] ios: service not attached");
        return;
    }
    service->setProperty("NativeSandboxGrantProcess", PCAM_PROCESS);
    service->setProperty("IOUserClientClass", "PaganiCameraLogClient");
    if (!service->start(home)) {
        service->detach(home);
        service->release();
        boot_progress_trace("[cam] ios: service not started");
        return;
    }
    service->registerService();
    service->release();   // 资源服务的挂接留着它，到重启为止
    boot_progress_trace("[cam] ios: log service published");
}
