// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng package/ShengAudioService.cpp (revision 08cde20). Classes renamed (PaganiAudio, PaganiAudioUserClient);
// each user client keeps its own requests, so a client that closes releases only what it asked for; the opener is recorded for the log;
// the host clock correction can be turned off (pagani-audio-hostclock=0).
//
// The audio service iOS's audio server plug-in opens (hal/PaganiAudioPlugIn.c, interface in PaganiAudioAbi.h). The service maps the
// control page and the two sample rings that PaganiAudio.cpp owns and passes start and stop requests to its thread; the samples never
// pass through here. PaganiAudio.cpp registers this file's classes (its static initializer, taken out of __mod_init_func by
// scripts/build-pagani-audio.py) and publishes the service.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/shared_region.h>
#include "PaganiAudioEngine.h"

extern "C" {
IOBufferMemoryDescriptor *pagani_audio_memory(uint32_t type);                    // retained; null while the service is not up
int pagani_audio_request(PaganiAudioClientState *client, uint32_t selector);    // 0 accepted
void pagani_audio_client_opened(const char *name, int pid);
void pagani_audio_client_closed(PaganiAudioClientState *client, const char *name);
int pagani_audio_host_clock_allowed(void);
int pagani_audio_actuator_enabled(void);                                        // revision 2: the haptic actuator is offered
void proc_selfname(char *buf, int size);
int proc_selfpid(void);
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(void *pmap, uint64_t va);
boolean_t pmap_valid_address(uint64_t pa);   // managed RAM (osfmk/arm/pmap pa_valid); phystokv panics on an address outside the RAM
// [audio-clock]: the last host clock request: the step it stopped at (0 done, 1 address, 2 prepare, 3 read through the caller, 4 physical
// address, 5 value), the caller's address, the physical address, the value the caller reads (bits).
volatile uint64_t pagani_audio_clock[4];
}

// CoreAudio takes the host clock to be 24 MHz (the timer of Apple's chips; AudioGetHostClockFrequency and the audio server's IO clock
// read one constant), so on this board's 19.2 MHz timer the audio server slept 512000 ticks (26.7 ms) for each 1024-frame cycle of
// 21.3 ms and fell behind until a time stamp pulled it forward (sheng i546: stutter). The constant lives in the dyld shared cache, on a
// page with code, so the caller cannot write it; this sets the shared page itself, once for every process. Only a 24 MHz value (or this
// board's, when asked again) inside the shared region is changed, and only on a page of managed RAM.
// Beyond sheng: the address must lie in the arm64 shared region itself (sheng allowed up to 0x400000000, past its end at 0x300000000), and
// the physical page must be managed RAM before the physical aperture is used: phystokv panics on an address outside the RAM, and a
// device or reserved page mapped into the caller is no constant of the shared cache.
constexpr uint64_t kSharedRegionBase = SHARED_REGION_BASE_ARM64, kSharedRegionEnd = SHARED_REGION_BASE_ARM64 + SHARED_REGION_SIZE_ARM64;
static IOReturn setHostClock(task_t task, uint64_t address) {
    constexpr uint64_t kApple = 0x4176e36000000000ULL;   // 24000000.0
    pagani_audio_clock[1] = address;
    pagani_audio_clock[2] = 0;
    pagani_audio_clock[3] = 0;
    pagani_audio_clock[0] = 1;
    if (!pagani_audio_host_clock_allowed()) return kIOReturnUnsupported;
    if ((address & 7) || address < kSharedRegionBase || address > kSharedRegionEnd - 8) return kIOReturnBadArgument;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!memory) return kIOReturnNoMemory;
    pagani_audio_clock[0] = 2;
    IOReturn r = memory->prepare();
    if (r != kIOReturnSuccess) {memory->release(); return r;}
    // The physical address from the caller's own page table: the one IOKit gave (getPhysicalSegment) held 0 (sheng i549, i551).
    uint64_t seen = 0;
    pagani_audio_clock[0] = 3;
    r = copyin(address, &seen, 8) ? kIOReturnNotReadable : kIOReturnNoResources;
    pagani_audio_clock[3] = seen;
    const uint64_t pa = r == kIOReturnNoResources ? pmap_find_pa(get_task_pmap(task), address) : 0;
    pagani_audio_clock[2] = pa;
    if (pa && !pmap_valid_address(pa)) {
        pagani_audio_clock[0] = 4;
        r = kIOReturnNotWritable;
    } else if (pa) {
        pagani_audio_clock[0] = 4;
        volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
        mach_timebase_info_data_t tb;
        clock_timebase_info(&tb);
        // The timer frequency as a double, built from the integer (no floating point in the kernel).
        const uint64_t hz = 1000000000ULL * tb.denom / tb.numer;
        const unsigned top = 63 - (unsigned)__builtin_clzll(hz);
        const uint64_t bits = (uint64_t)(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
        r = kIOReturnUnsupported;
        if (*value == seen && (seen == kApple || seen == bits)) {
            pagani_audio_clock[0] = 5;
            *value = bits;
            __asm__ volatile("dsb sy" ::: "memory");
            pagani_audio_clock[0] = 0;
            r = kIOReturnSuccess;
        }
    }
    memory->complete();
    memory->release();
    return r;
}

class PaganiAudio : public IOService {
    OSDeclareDefaultStructors(PaganiAudio)
public:
    bool publish(const char *backend) {
        // iOS's audio server is sandboxed against unknown user clients; the kernel (zzw-native-adreno-sandbox-grant) lets the named
        // processes open this service in place of the sandbox's verdict. audiomxd runs the HAL plug-ins on 21H461; mediaserverd stays
        // listed for builds where it still does.
        setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd");
        setProperty("IOUserClientClass", PAGANI_AUDIO_USER_CLIENT_CLASS);
        setProperty("PaganiAudioBackend", backend);
        setProperty("PaganiAudioABI", (unsigned long long)PAGANI_AUDIO_VERSION, 32);
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(PaganiAudio, IOService)

class PaganiAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PaganiAudioUserClient)
    task_t owner;
    PaganiAudioClientState wants;
    char name[17];
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        if (!IOUserClient::initWithTask(task, security, type, properties)) return false;
        owner = task;
        wants.bits = 0;
        // The open runs on the opener's thread: its process name and pid, for the log.
        bzero(name, sizeof name);
        proc_selfname(name, sizeof name);
        pagani_audio_client_opened(name, proc_selfpid());
        return true;
    }
    bool start(IOService *provider) override {
        return OSDynamicCast(PaganiAudio, provider) && IOUserClient::start(provider);
    }
    IOReturn clientClose() override {
        // The plug-in's process went away or closed: what it asked for is released; other clients keep theirs.
        pagani_audio_client_closed(&wants, name);
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
        IOBufferMemoryDescriptor *m = pagani_audio_memory(type);
        if (!m) return kIOReturnBadArgument;
        *options = 0;
        *memory = m;   // the caller consumes the reference
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if (selector == PaganiAudioSetHostClock && args->scalarInputCount == 1 && !args->structureInputSize)
            return setHostClock(owner, args->scalarInput[0]);
        // Revision 2: the actuator's start and stop, only while the actuator is offered (otherwise refused as before).
        const bool actuator = (selector == PaganiAudioStartActuator || selector == PaganiAudioStopActuator) && pagani_audio_actuator_enabled();
        if ((selector > PaganiAudioStopInput && !actuator) || args->scalarInputCount || args->structureInputSize) return kIOReturnBadArgument;
        return pagani_audio_request(&wants, selector) ? kIOReturnNotReady : kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(PaganiAudioUserClient, IOUserClient)

// Called by PaganiAudio.cpp once this file's classes are registered and the rings are up. 1 published; negative: the step that failed.
extern "C" int pagani_audio_service_publish(const char *backend) {
    IOService *home = IOService::getResourceService();
    PaganiAudio *audio = OSTypeAlloc(PaganiAudio);
    if (!audio || !home || !audio->init() || !audio->attach(home)) {
        if (audio) audio->release();
        return -1;
    }
    if (!audio->start(home) || !audio->publish(backend)) {
        audio->detach(home);
        audio->release();
        return -2;
    }
    audio->release();   // the resource service's attachment keeps it until reboot
    return 1;
}
