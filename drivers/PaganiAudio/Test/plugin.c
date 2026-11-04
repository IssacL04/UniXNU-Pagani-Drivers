// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the audio server plug-in (hal/PaganiAudioPlugIn.c) on macOS, against the real engine (PaganiAudioEngine.c) as the kernel
// service: the IOKit calls the plug-in makes (matching, open, memory maps, scalar methods, close) are answered here as PaganiAudio.cpp
// and PaganiAudioService.cpp answer them, and AudioGetHostClockFrequency is an adrp + ldr d0 function like CoreAudio's, so the plug-in's
// host clock address decoding and the service's correction run for real. Checked: the factory and interface, every property the
// audio server reads on sheng's list (plug-in, device with the Thunderbolt transport, streams, volume), settable properties, unknown
// properties recorded in the control page, the retry when the service is missing at load, StartIO/StopIO as kernel requests, zero
// time stamps on the service's timeline, the mix written into the playback ring at the output time (with the volume's gain) and taken
// by the silent backend, and input read from the capture ring. The route probe runs against a simulated in-process HAL (below): the
// routing layer's plug-in, its default virtual device and active ports, and this device, whose {'vold', output} the HAL translates to
// the plug-in's volume control as HALB_Info::TranslateDeviceAddressToControlAddress does.
// Build: cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. plugin.c ../PaganiAudioEngine.c ../hal/PaganiAudioPlugIn.c
//        -framework CoreFoundation -o plugin      (no IOKit, no CoreAudio: this file supplies what the plug-in takes from them)
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include "PaganiAudioEngine.h"
#include "PaganiAudioHaptics.h"
#include "hal/PaganiAudioMxLayout.h"

extern void *PaganiAudio_Create(CFAllocatorRef allocator, CFUUIDRef type);

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

// ---- the simulated kernel service -----------------------------------------------------------------------------------------------
static PaganiAudioEngine gEngine;
static void *gPage;
static int16_t *gOut, *gIn;
static int gKernelUp, gOpen, gMatches, gOpens, gCloses, gMaps, gClockCalls;
static PaganiAudioClientState gClient;
static mach_timebase_info_data_t gTimebase;
enum {kService = 0x2301, kConnect = 0x2302};

static uint64_t nowMs(void) {return mach_absolute_time() * gTimebase.numer / gTimebase.denom / 1000000;}
// Revision 2's actuator (the second test run, "plugin actuator"): its memory and the real bridge, feeding a recording vibrator.
static void *gActMemory;
static int gActMemoryRefused;   // clientMemoryForType refuses memory type 3
static PaganiAudioHaptics gBridge;
static int gVibBegins, gVibEnds;
static uint32_t gVibRate, gVibMv, gVibSamples;
static int8_t gVibPeak;
static int vibBegin(void *c, uint32_t rate, uint32_t mv) {(void)c; gVibBegins++; gVibRate = rate; gVibMv = mv; return 0;}
static int vibWrite(void *c, const int8_t *s, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; ++i) if (abs(s[i]) > gVibPeak) gVibPeak = (int8_t)abs(s[i]);
    gVibSamples += n;
    return (int)n;
}
static int vibEnd(void *c, int drain) {(void)c; (void)drain; gVibEnds++; return 0;}
static const PaganiAudioHapticsOps kVibOps = {vibBegin, vibWrite, vibEnd, NULL, NULL};
static void kernelRound(void)
{
    const uint64_t now = mach_absolute_time();
    pagani_audio_engine_poll(&gEngine, now, nowMs());
    if (gActMemory) pagani_audio_haptics_poll(&gBridge, &gEngine, now);
}

static void kernelUp(void)
{
    gPage = aligned_alloc(4096, PAGANI_AUDIO_CONTROL_BYTES);
    gOut = calloc(PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS, sizeof(int16_t));
    gIn = calloc(PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS, sizeof(int16_t));
    CHECK(pagani_audio_engine_init(&gEngine, gPage, gOut, gIn, mach_absolute_time(), gTimebase.numer, gTimebase.denom,
                                   &pagani_audio_silent_backend, 0) == 0);
    ((PaganiAudioControl *)gPage)->state |= PaganiAudioOutputReady | PaganiAudioInputReady;
    gKernelUp = 1;
}

// CoreAudio's constant and its getter: adrp x8, page; ldr d0, [x8, offset]; ret (the plug-in decodes the two instructions).
double gHostClockConstant = 24000000.0;
__asm__(".globl _AudioGetHostClockFrequency\n.p2align 2\n_AudioGetHostClockFrequency:\n"
        "  adrp x8, _gHostClockConstant@PAGE\n  ldr d0, [x8, _gHostClockConstant@PAGEOFF]\n  ret\n");

// ---- the IOKit calls of the plug-in ------------------------------------------------------------------------------------------------
const mach_port_t kIOMainPortDefault = MACH_PORT_NULL;

CFMutableDictionaryRef IOServiceMatching(const char *name)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef value = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, CFSTR("IOProviderClass"), value);
    CFRelease(value);
    return d;
}

io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching)
{
    char name[64] = "";
    CFStringGetCString(CFDictionaryGetValue(matching, CFSTR("IOProviderClass")), name, sizeof name, kCFStringEncodingUTF8);
    CFRelease(matching);   // the call consumes it
    gMatches++;
    return port == MACH_PORT_NULL && gKernelUp && !strcmp(name, PAGANI_AUDIO_CLASS) ? kService : 0;
}

kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect)
{
    (void)owningTask;
    if (service != kService || type != 0 || gOpen) return kIOReturnNotPermitted;
    gOpen = 1;
    gOpens++;
    gClient.bits = 0;   // a new user client (initWithTask): the released state of an earlier one does not carry over
    *connect = kConnect;
    return KERN_SUCCESS;
}

kern_return_t IOObjectRelease(io_object_t object) {(void)object; return KERN_SUCCESS;}

kern_return_t IOServiceClose(io_connect_t connect)
{
    if (connect != kConnect || !gOpen) return kIOReturnBadArgument;
    gOpen = 0;
    gCloses++;
    pagani_audio_engine_release(&gEngine, &gClient);   // clientClose
    kernelRound();
    return KERN_SUCCESS;
}

kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask, mach_vm_address_t *atAddress,
                                   mach_vm_size_t *ofSize, IOOptionBits options)
{
    (void)intoTask;
    if (connect != kConnect || !(options & kIOMapAnywhere)) return kIOReturnBadArgument;
    void *const memory[4] = {gPage, gOut, gIn, gActMemory};
    const mach_vm_size_t sizes[4] = {PAGANI_AUDIO_CONTROL_BYTES, PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS * 2,
                                     PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS * 2, PAGANI_AUDIO_ACT_BYTES};
    if (memoryType > PaganiAudioMemoryActuator || !memory[memoryType]) return kIOReturnBadArgument;   // clientMemoryForType
    if (memoryType == PaganiAudioMemoryActuator && gActMemoryRefused) return kIOReturnBadArgument;
    *atAddress = (mach_vm_address_t)(uintptr_t)memory[memoryType];
    *ofSize = sizes[memoryType];
    gMaps++;
    return KERN_SUCCESS;
}

kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCnt,
                                        uint64_t *output, uint32_t *outputCnt)
{
    (void)output;
    (void)outputCnt;
    if (connection != kConnect) return kIOReturnBadArgument;
    if (selector == PaganiAudioSetHostClock && inputCnt == 1) {
        // The service's setHostClock: only the 24 MHz constant (or the board's value, asked again), which becomes the board's 19.2 MHz.
        gClockCalls++;
        if (input[0] != (uint64_t)(uintptr_t)&gHostClockConstant ||
            (gHostClockConstant != 24000000.0 && gHostClockConstant != 19200000.0)) return kIOReturnUnsupported;
        gHostClockConstant = 19200000.0;
        return KERN_SUCCESS;
    }
    const int actuator = (selector == PaganiAudioStartActuator || selector == PaganiAudioStopActuator) && gActMemory;
    if ((selector > PaganiAudioStopInput && !actuator) || inputCnt) return kIOReturnBadArgument;
    if (pagani_audio_engine_request(&gEngine, &gClient, selector)) return kIOReturnNotReady;
    kernelRound();   // the service thread wakes up
    return KERN_SUCCESS;
}

// ---- the in-process HAL the route probe asks (looked up with dlsym, as on iOS) ---------------------------------------------------------
enum {kHalOurs = 44, kHalVad = 58, kHalVa = 37, kHalPort = 77, kHalPort2 = 78};
static int gHalCalls, gHalUidQualifier = 1, gHalVa = 1, gHalVad = 1, gHalVoldControl = 1, gHalRunning;
// The routing layer's state behind an empty route (the probe's t0/t1 when no output port is active): whether the default virtual device
// has its two active ports, MediaExperience's serialization listener ('srzl'), the process hogging this device ('oink'; 0 = the HAL does
// not answer) and the connected output ports ('prts').
static int gHalActive = 1, gHalSerial = 1, gHalHogKnown = 1, gHalConnected;
static pid_t gHalHog = -1;
static SInt32 gHalConnectedPorts[3];
static UInt32 gHalVcsp = 1;
static OSStatus gHalVcspStatus;
static AudioServerPlugInDriverRef gDriver;
enum {kHalActuator = 46, kHalVadActuator = 59};
static int gHalVadActuator;
static AudioObjectID halLookup(CFStringRef key)
{
    if (!key || CFGetTypeID(key) != CFStringGetTypeID()) return kAudioObjectUnknown;
    if (CFStringCompare(key, CFSTR("PaganiAudioDevice"), 0) == kCFCompareEqualTo) return kHalOurs;
    if (CFStringCompare(key, CFSTR("Actuator"), 0) == kCFCompareEqualTo) return kHalActuator;
    if (gHalVadActuator && CFStringCompare(key, CFSTR("VirtualAudioDevice_Actuator"), 0) == kCFCompareEqualTo) return kHalVadActuator;
    if (gHalVad && CFStringCompare(key, CFSTR("VirtualAudioDevice_Default"), 0) == kCFCompareEqualTo) return kHalVad;
    return kAudioObjectUnknown;
}
static CFDictionaryRef halPorts(CFStringRef uid, const SInt32 *ports, CFIndex count)
{
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; i < count; ++i) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &ports[i]);
        CFArrayAppendValue(list, n);
        CFRelease(n);
    }
    const void *keys[2] = {CFSTR("device UID"), CFSTR("output ports")}, *values[2] = {uid, list};
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(list);
    return d;
}
#define HAL_REPLY(type, value) do { if (*size < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)data = (value); \
                                    *size = sizeof(type); return 0; } while (0)
OSStatus AudioObjectGetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *data);
OSStatus AudioObjectGetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *data)
{
    gHalCalls++;
    const AudioObjectPropertySelector s = a->mSelector;
    if (o == 1 && s == 'dev#') {
        const AudioObjectID devices[4] = {kHalOurs, kHalVad, 11, 12};
        if (*size < sizeof devices) return kAudioHardwareBadPropertySizeError;
        memcpy(data, devices, sizeof devices);
        *size = sizeof devices;
        return 0;
    }
    if (o == 1 && s == 'uidd') {
        if (!gHalUidQualifier || qs != sizeof(CFStringRef) || !q) return kAudioHardwareUnknownPropertyError;
        HAL_REPLY(AudioObjectID, halLookup(*(const CFStringRef *)q));
    }
    if (o == 1 && (s == 'duid' || s == 'pibi')) {
        AudioValueTranslation *t = data;
        if (*size != sizeof *t || t->mInputDataSize != sizeof(CFStringRef) || t->mOutputDataSize != sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
        CFStringRef key = *(const CFStringRef *)t->mInputData;
        AudioObjectID found = s == 'duid' ? halLookup(key) :
                              gHalVa && CFStringCompare(key, CFSTR("com.apple.audio.CoreAudio.VirtualAudio"), 0) == kCFCompareEqualTo ? kHalVa : 0;
        *(AudioObjectID *)t->mOutputData = found;
        return 0;
    }
    if (o == kHalOurs && s == 'gone') HAL_REPLY(UInt32, (UInt32)gHalRunning);
    if (o == kHalVad && gHalVad && s == 'vcsp' && a->mScope == kAudioObjectPropertyScopeOutput) {
        if (gHalVcspStatus) return gHalVcspStatus;
        HAL_REPLY(UInt32, gHalVcsp);
    }
    if (o == kHalVad && gHalVad && s == 'volm' && a->mScope == kAudioObjectPropertyScopeOutput) HAL_REPLY(Float32, 0.5f);
    if (o == kHalVa && gHalVa && s == 'rctx') HAL_REPLY(UInt32, 'cmed');
    if (o == kHalVa && gHalVa && s == 'inpm') HAL_REPLY(UInt32, 'imdf');
    if (o == kHalVa && gHalVa && s == 'aprc' && a->mScope == kAudioObjectPropertyScopeOutput) {
        // An array of per device dictionaries, the default virtual device's second: this device's output port and a speaker (none when
        // gHalActive is 0, as on the 13T of 2026-10-31).
        const SInt32 other[1] = {99}, ports[2] = {kHalPort, kHalPort2};
        CFDictionaryRef d0 = halPorts(CFSTR("VirtualAudioDevice_LowLatency"), other, 1),
                        d1 = halPorts(CFSTR("VirtualAudioDevice_Default"), ports, gHalActive ? 2 : 0);
        const void *values[2] = {d0, d1};
        CFArrayRef list = CFArrayCreate(NULL, values, 2, &kCFTypeArrayCallBacks);
        CFRelease(d0);
        CFRelease(d1);
        HAL_REPLY(CFArrayRef, list);
    }
    if (o == kHalVa && gHalVa && s == 'srzl') {
        // VirtualAudio answers {listener proc, client data}; the proc is set once MediaExperience installed it.
        if (*size != 2 * sizeof(uint64_t)) return kAudioHardwareBadPropertySizeError;
        ((uint64_t *)data)[0] = gHalSerial ? 0x1987f0000ull : 0;
        ((uint64_t *)data)[1] = 0;
        return 0;
    }
    if (o == kHalActuator && s == 'oink') HAL_REPLY(pid_t, gHalVadActuator ? getpid() : -1);
    if (o == kHalOurs && s == 'oink') {
        if (!gHalHogKnown) return kAudioHardwareUnknownPropertyError;
        HAL_REPLY(pid_t, gHalHog);
    }
    if (o == kHalVa && gHalVa && s == 'prts' && a->mScope == kAudioObjectPropertyScopeOutput && !qs) {
        CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        for (int i = 0; i < gHalConnected; ++i) {
            CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &gHalConnectedPorts[i]);
            CFArrayAppendValue(list, n);
            CFRelease(n);
        }
        HAL_REPLY(CFArrayRef, list);
    }
    if (o == kHalPort && s == 'ptyp') HAL_REPLY(UInt32, 'ptbo');
    if (o == kHalPort2 && s == 'ptyp') HAL_REPLY(UInt32, 'pspk');
    return kAudioHardwareUnknownPropertyError;
}
// {'vold', output, main} of this device: the HAL finds the volume control by class, scope and element and asks the plug-in for its
// decibel value (gHalVoldControl = 0: a HAL that finds no such control).
Boolean AudioObjectHasProperty(AudioObjectID o, const AudioObjectPropertyAddress *a);
Boolean AudioObjectHasProperty(AudioObjectID o, const AudioObjectPropertyAddress *a)
{
    gHalCalls++;
    if (o != kHalOurs || a->mSelector != 'vold' || a->mScope != kAudioObjectPropertyScopeOutput || !gHalVoldControl) return 0;
    const AudioObjectPropertyAddress control = {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return (*gDriver)->HasProperty(gDriver, 5, 0, &control);
}
extern void pagani_audio_probe_route(uint32_t reason);
extern uint32_t pagani_audio_probe_why(int serial, int hog, OSStatus connStatus, uint32_t conn, int thunderbolt);
extern void pagani_audio_probe_mx(void);
extern void pagani_audio_probe_actuator(void);

// ---- MediaExperience and VirtualAudio as the repair meets them (the "plugin actuator" run) ---------------------------------------------
// MediaExperience's exported MXInitialize and its listeners are functions of this binary; its globals are variables here. The plug-in's
// layout is pointed at them (fakeLayout), so the slide it works out is this binary's distance to 21H461's addresses.
void MXInitialize(void);
void MXInitialize(void) {__asm__ volatile("nop\n nop\n nop\n nop" ::: "memory");}
// libMobileGestalt's answer to what VirtualAudio's deferred initialisation asks (DeviceSupportsHaptics, obfuscated): the plug-in asks it only
// without a service.
static int gMgHaptics, gMgCalls;
Boolean MGGetBoolAnswer(CFStringRef key);
Boolean MGGetBoolAnswer(CFStringRef key)
{
    gMgCalls++;
    return gMgHaptics && CFStringCompare(key, CFSTR("cBy4BcYs5YWtFHbBpt4C6A"), 0) == kCFCompareEqualTo;
}
// MGGetProductType: what VirtualAudio's product table is asked with (0x80ac7ae0: J120, the device tree's product-id).
static uint64_t gMgProduct = 0x80ac7ae0ull;
uint64_t MGGetProductType(void);
uint64_t MGGetProductType(void) {return gMgProduct;}
static void fakeSerializationListener(void) {__asm__ volatile("nop\n nop" ::: "memory");}
static void fakeReceiverListener(void) {__asm__ volatile("nop" ::: "memory");}
static volatile uint64_t gMxOnce, gMxSession, gMxScreen, gMxIds, gMxRoutes, gMxContext, gMxEndpoint, gMxQueue;
static volatile uint32_t gMxVaPlugin, gMxSerialDone[2];
static PaganiAudioMxLayout gFakeLayout;
// VirtualAudio's image: a Mach-O header with its UUID, the DeviceManager pointer at the layout's offset, a manager whose listener holds
// the filter mode.
static struct {
    struct mach_header_64 header;
    struct uuid_command uuid;
    uint8_t pad[64];
    uint64_t manager;            // at the image offset va_device_manager
    uint64_t settings_guard;     // ActuatorSettingsFactory_Aspen's static: the guard, its kind word and its format word
    uint32_t settings_kind, settings_format;
} gVaImage;
static uint64_t gVaManager[16], gVaListener[32];
static int gSetCalls;
static uint64_t gSetSerial[2], gSetReceiver[2];
static OSStatus gSetResult;
static void fakeLayout(void)
{
    const uintptr_t slide = (uintptr_t)(void *)MXInitialize - (uintptr_t)0x19886ad24ull;
    PaganiAudioMxLayout *L = &gFakeLayout;
    memset(L, 0, sizeof *L);
    L->initialize = 0x19886ad24ull;
    memcpy(L->initialize_code, (const void *)(uintptr_t)MXInitialize, sizeof L->initialize_code);
    L->once = (uintptr_t)&gMxOnce - slide;
    L->session = (uintptr_t)&gMxSession - slide;
    L->screen = (uintptr_t)&gMxScreen - slide;
    L->ids = (uintptr_t)&gMxIds - slide;
    L->routes = (uintptr_t)&gMxRoutes - slide;
    L->context = (uintptr_t)&gMxContext - slide;
    L->endpoint = (uintptr_t)&gMxEndpoint - slide;
    L->va_plugin = (uintptr_t)&gMxVaPlugin - slide;
    L->serial_done = (uintptr_t)&gMxSerialDone[0] - slide;
    L->queue = (uintptr_t)&gMxQueue - slide;
    L->serial_listener = (uintptr_t)(void *)fakeSerializationListener - slide;
    memcpy(L->serial_code, (const void *)(uintptr_t)fakeSerializationListener, sizeof L->serial_code);
    L->receiver_listener = (uintptr_t)(void *)fakeReceiverListener - slide;
    memcpy(L->receiver_code, (const void *)(uintptr_t)fakeReceiverListener, sizeof L->receiver_code);
    static const uint8_t uuid[16] = {0x23, 0xc2, 0xd0, 0x62, 0x74, 0xb1, 0x3e, 0x5b, 0x81, 0xab, 0x64, 0x5d, 0x16, 0x58, 0xd5, 0x73};
    memcpy(L->va_uuid, uuid, 16);
    L->va_device_manager = offsetof(__typeof__(gVaImage), manager);
    L->va_listener = 0x58;
    L->va_filter = 0x88;
    L->va_settings_guard = offsetof(__typeof__(gVaImage), settings_guard);
    L->va_settings_kind = offsetof(__typeof__(gVaImage), settings_kind);
    L->va_settings_format = offsetof(__typeof__(gVaImage), settings_format);
    L->va_product = 0x80ac7ae0ull;
    gVaImage.header.magic = MH_MAGIC_64;
    gVaImage.header.ncmds = 1;
    gVaImage.header.sizeofcmds = sizeof gVaImage.uuid;
    gVaImage.uuid.cmd = LC_UUID;
    gVaImage.uuid.cmdsize = sizeof gVaImage.uuid;
    memcpy(gVaImage.uuid.uuid, uuid, 16);
    gVaImage.manager = (uint64_t)(uintptr_t)gVaManager;
    gVaManager[0x58 / 8] = (uint64_t)(uintptr_t)gVaListener;
    ((uint32_t *)gVaListener)[0x88 / 4] = 2;   // only the six special UIDs, as before 'srzl'
    pagani_audio_mx_layout = L;
}
// The HAL's write, as VirtualAudio answers it: 'srzl' installs the listener and lets every device in (filter mode 3), 'rwbu' is taken.
OSStatus AudioObjectSetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, const void *data);
OSStatus AudioObjectSetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, const void *data)
{
    (void)qs; (void)q;
    gSetCalls++;
    if (o != kHalVa || size != 16) return kAudioHardwareBadPropertySizeError;
    if (gSetResult) return gSetResult;
    if (a->mSelector == 'srzl') {
        memcpy(gSetSerial, data, 16);
        gHalSerial = 1;
        ((uint32_t *)gVaListener)[0x88 / 4] = 3;
        return 0;
    }
    if (a->mSelector == 'rwbu') {memcpy(gSetReceiver, data, 16); return 0;}
    return kAudioHardwareUnknownPropertyError;
}

// ---- the host (the audio server) ---------------------------------------------------------------------------------------------------
static UInt32 gChangedCount, gChangedAddresses;
static AudioObjectID gChangedObject;
static OSStatus HostPropertiesChanged(AudioServerPlugInHostRef host, AudioObjectID object, UInt32 count,
                                      const AudioObjectPropertyAddress *addresses)
{
    (void)host;
    (void)addresses;
    gChangedCount++;
    gChangedAddresses += count;
    gChangedObject = object;
    return 0;
}
static OSStatus HostCopyFromStorage(AudioServerPlugInHostRef host, CFStringRef key, CFPropertyListRef *data)
{
    (void)host; (void)key; *data = NULL; return 0;
}
static OSStatus HostWriteToStorage(AudioServerPlugInHostRef host, CFStringRef key, CFPropertyListRef data) {(void)host; (void)key; (void)data; return 0;}
static OSStatus HostDeleteFromStorage(AudioServerPlugInHostRef host, CFStringRef key) {(void)host; (void)key; return 0;}
static OSStatus HostRequestChange(AudioServerPlugInHostRef host, AudioObjectID device, UInt64 action, void *info)
{
    (void)host; (void)device; (void)action; (void)info; return 0;
}
static const AudioServerPlugInHostInterface gHostInterface = {HostPropertiesChanged, HostCopyFromStorage, HostWriteToStorage,
                                                              HostDeleteFromStorage, HostRequestChange};

#define D (*gDriver)

static OSStatus get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope, UInt32 qs, const void *q, UInt32 size,
                    UInt32 *outSize, void *data)
{
    const AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    return D->GetPropertyData(gDriver, o, 0, &a, qs, q, size, outSize, data);
}
static UInt32 get32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope)
{
    UInt32 value = 0xdeadbeef, size = 0;
    CHECK(get(o, s, scope, 0, NULL, sizeof value, &size, &value) == 0 && size == sizeof value);
    return value;
}
static int string_is(AudioObjectID o, AudioObjectPropertySelector s, const char *expected)
{
    CFStringRef value = NULL;
    UInt32 size = 0;
    if (get(o, s, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof value, &size, &value) || size != sizeof value || !value) return 0;
    char text[128] = "";
    CFStringGetCString(value, text, sizeof text, kCFStringEncodingUTF8);
    return !strcmp(text, expected);
}
static OSStatus set_float(AudioObjectID o, AudioObjectPropertySelector s, Float32 value)
{
    const AudioObjectPropertyAddress a = {s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return D->SetPropertyData(gDriver, o, 0, &a, 0, NULL, sizeof value, &value);
}
static Float32 get_float(AudioObjectID o, AudioObjectPropertySelector s)
{
    Float32 value = -1;
    UInt32 size = 0;
    CHECK(get(o, s, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof value, &size, &value) == 0 && size == sizeof value);
    return value;
}
static PaganiAudioDiag *diag(void) {return (PaganiAudioDiag *)((uint8_t *)gPage + PAGANI_AUDIO_DIAG_OFFSET);}

static void test_interface(void)
{
    CHECK(PaganiAudio_Create(kCFAllocatorDefault, kAudioServerPlugInDriverInterfaceUUID) == NULL);   // only the plug-in type
    void *factory = PaganiAudio_Create(kCFAllocatorDefault, kAudioServerPlugInTypeUUID);
    CHECK(factory != NULL);
    AudioServerPlugInDriverRef driver = factory;
    void *out = NULL;
    CHECK((*driver)->QueryInterface(driver, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &out) == S_OK && out == factory);
    CHECK((*driver)->QueryInterface(driver, CFUUIDGetUUIDBytes(IUnknownUUID), &out) == S_OK);
    CHECK((*driver)->QueryInterface(driver, CFUUIDGetUUIDBytes(kAudioServerPlugInTypeUUID), &out) == E_NOINTERFACE);
    gDriver = driver;
}

static void test_without_service(void)
{
    // The audio server loads the plug-in before the kernel service runs (or without pagani-audio): the device is listed, IO is refused.
    CHECK(D->Initialize(gDriver, &gHostInterface) == 0);
    CHECK(gMatches == 1 && gOpens == 0);
    // MobileGestalt was asked (no service to say) and said no: the speaker device alone.
    AudioObjectID devices[4];
    UInt32 listed = 0;
    CHECK(gMgCalls == 1 && get(1, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof devices, &listed, devices) == 0 &&
          listed == sizeof(AudioObjectID) && devices[0] == 2);
    CHECK(D->StartIO(gDriver, 6, 1) == kAudioHardwareBadObjectError);
    CHECK(D->StartIO(gDriver, 2, 1) == kAudioHardwareNotRunningError);
    CHECK(D->StartIO(gDriver, 3, 1) == kAudioHardwareBadObjectError);
    UInt32 transport = get32(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
    CHECK(transport == kAudioDeviceTransportTypeThunderbolt);
    CHECK(get32(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) == 2 * PAGANI_AUDIO_PERIOD_FRAMES);
    Float64 sampleTime = 0;
    UInt64 host = 0, seed = 0;
    CHECK(D->GetZeroTimeStamp(gDriver, 2, 1, &sampleTime, &host, &seed) == kAudioHardwareNotRunningError);
    // The service comes up: the next StartIO connects (and corrects the host clock), and IO runs.
    kernelUp();
    CHECK(D->StartIO(gDriver, 2, 1) == 0);
    // connectService sets the clock once and StartIO again (the shared page may have been read back from disk since).
    CHECK(gOpens == 1 && gMaps == 3 && gClockCalls == 2 && gHostClockConstant == 19200000.0);
    CHECK(diag()->host_clock == 19200000 && diag()->host_clock_result == 0);
    CHECK(gEngine.running && gEngine.capturing && gEngine.out_users == 1 && gEngine.in_users == 1);
    CHECK(D->StopIO(gDriver, 2, 1) == 0);
    CHECK(gEngine.out_users == 0 && gEngine.in_users == 0 && gEngine.running && !gEngine.capturing);   // output keeps 5 s
}

static void test_properties(void)
{
    AudioObjectID ids[8];
    UInt32 size = 0;
    // The plug-in object.
    CHECK(get32(1, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioPlugInClassID);
    CHECK(get32(1, kAudioObjectPropertyBaseClass, kAudioObjectPropertyScopeGlobal) == kAudioObjectClassID);
    CHECK(get(1, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0);
    CHECK(size == sizeof(AudioObjectID) && ids[0] == 2);
    CHECK(get(1, kAudioPlugInPropertyBoxList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    CHECK(get(1, kAudioPlugInPropertyClockDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    CFStringRef uid = CFSTR("PaganiAudioDevice"), other = CFSTR("BuiltInSpeakerDevice");
    AudioObjectID found = 0;
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof uid, &uid, sizeof found, &size, &found) == 0);
    CHECK(found == 2);
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof other, &other, sizeof found, &size, &found) == 0);
    CHECK(found == kAudioObjectUnknown);
    CHECK(string_is(1, kAudioObjectPropertyManufacturer, "UniXNU"));
    // The device: a Thunderbolt driver device, as iOS 21H461 routes a plug-in device with one output and one input port.
    CHECK(get32(2, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClassID);
    CHECK(get32(2, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(get32(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeThunderbolt);
    CHECK(string_is(2, kAudioObjectPropertyName, "Speaker") && string_is(2, kAudioDevicePropertyDeviceUID, "PaganiAudioDevice"));
    CHECK(string_is(2, kAudioDevicePropertyModelUID, "PaganiAudioModel") && string_is(2, kAudioDevicePropertyConfigurationApplication,
                                                                                   "com.apple.Preferences"));
    CHECK(get32(2, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(get32(2, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == 1);
    CHECK(get32(2, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeOutput) == 1);
    CHECK(get32(2, 'accs', kAudioObjectPropertyScopeGlobal) == 0 && get32(2, 'auto', kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(get32(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) == PAGANI_AUDIO_LEAD_FRAMES);
    CHECK(get32(2, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) == PAGANI_AUDIO_LEAD_FRAMES + 2 * PAGANI_AUDIO_PERIOD_FRAMES);
    CHECK(get32(2, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) == PAGANI_AUDIO_RING_FRAMES);
    CHECK(get32(2, kAudioDevicePropertyClockAlgorithm, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClockAlgorithmRaw);
    CHECK(get32(2, kAudioDevicePropertyClockIsStable, kAudioObjectPropertyScopeGlobal) == 1);
    Float64 rate = 0;
    CHECK(get(2, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof rate, &size, &rate) == 0 && rate == 48000);
    AudioValueRange range = {0, 0};
    CHECK(get(2, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof range, &size, &range) == 0);
    CHECK(range.mMinimum == 48000 && range.mMaximum == 48000);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0);
    CHECK(size == 2 * sizeof(AudioObjectID) && ids[0] == 3 && ids[1] == 4);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0, NULL, sizeof ids, &size, ids) == 0 && size == 4 && ids[0] == 3);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, 0, NULL, sizeof ids, &size, ids) == 0 && size == 4 && ids[0] == 4);
    CHECK(get(2, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0);
    CHECK(size == sizeof(AudioObjectID) && ids[0] == 5);
    CHECK(get(2, kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 12);
    UInt32 stereo[2] = {0, 0};
    CHECK(get(2, kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput, 0, NULL, sizeof stereo, &size, stereo) == 0);
    CHECK(stereo[0] == 1 && stereo[1] == 2);
    uint8_t layout[256];
    CHECK(get(2, kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, 0, NULL, sizeof layout, &size, layout) == 0);
    const AudioChannelLayout *l = (const AudioChannelLayout *)layout;
    CHECK(l->mNumberChannelDescriptions == 2 && l->mChannelDescriptions[0].mChannelLabel == kAudioChannelLabel_Left &&
          l->mChannelDescriptions[1].mChannelLabel == kAudioChannelLabel_Right);
    CHECK(get(2, kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeInput, 0, NULL, sizeof layout, &size, layout) == 0);
    CHECK(l->mNumberChannelDescriptions == 1 && l->mChannelDescriptions[0].mChannelLabel == kAudioChannelLabel_Mono);
    // The streams: 48 kHz float, stereo out and mono in.
    for (AudioObjectID s = 3; s <= 4; ++s) {
        const UInt32 channels = s == 3 ? 2 : 1;
        CHECK(get32(s, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioStreamClassID);
        CHECK(get32(s, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == (s == 4));
        CHECK(get32(s, kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal) ==
              (s == 3 ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone));
        CHECK(get32(s, kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal) == 1);
        CHECK(get32(s, kAudioStreamPropertyStartingChannel, kAudioObjectPropertyScopeGlobal) == 1);
        AudioStreamBasicDescription f;
        CHECK(get(s, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof f, &size, &f) == 0 && size == sizeof f);
        CHECK(f.mSampleRate == 48000 && f.mFormatID == kAudioFormatLinearPCM && f.mChannelsPerFrame == channels &&
              f.mBitsPerChannel == 32 && f.mBytesPerFrame == 4 * channels && (f.mFormatFlags & kAudioFormatFlagIsFloat));
        AudioStreamRangedDescription r;
        CHECK(get(s, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof r, &size, &r) == 0);
        CHECK(r.mFormat.mChannelsPerFrame == channels && r.mSampleRateRange.mMinimum == 48000);
        CHECK(get(s, 'taps', kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
        const AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0};
        Boolean settable = 0;
        CHECK(D->IsPropertySettable(gDriver, s, 0, &fa, &settable) == 0 && settable);
        f.mSampleRate = 44100;
        CHECK(D->SetPropertyData(gDriver, s, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
        f.mSampleRate = 48000;
        CHECK(D->SetPropertyData(gDriver, s, 0, &fa, 0, NULL, sizeof f, &f) == 0);
    }
    // The volume: a level control in the output scope, -36..0 dB on a squared curve, settable; each set is announced to the host.
    CHECK(get32(5, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioVolumeControlClassID);
    CHECK(get32(5, kAudioObjectPropertyBaseClass, kAudioObjectPropertyScopeGlobal) == kAudioLevelControlClassID);
    CHECK(get32(5, kAudioControlPropertyScope, kAudioObjectPropertyScopeGlobal) == kAudioObjectPropertyScopeOutput);
    CHECK(get_float(5, kAudioLevelControlPropertyScalarValue) == 1.0f && get_float(5, kAudioLevelControlPropertyDecibelValue) == 0.0f);
    const UInt32 sets = diag()->volume_sets, changed = gChangedCount;
    CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 0.5f) == 0);
    CHECK(gChangedCount == changed + 1 && gChangedAddresses >= 2 && gChangedObject == 5);
    CHECK(fabsf(get_float(5, kAudioLevelControlPropertyDecibelValue) + 9.0f) < 1e-4f && diag()->volume_milli == 500);
    CHECK(set_float(5, kAudioLevelControlPropertyDecibelValue, -36.0f) == 0 && get_float(5, kAudioLevelControlPropertyScalarValue) == 0.0f);
    CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 7.0f) == 0 && get_float(5, kAudioLevelControlPropertyScalarValue) == 1.0f);
    CHECK(diag()->volume_sets == sets + 3);
    AudioValueRange db = {0, 0};
    CHECK(get(5, kAudioLevelControlPropertyDecibelRange, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof db, &size, &db) == 0);
    CHECK(db.mMinimum == -36.0 && db.mMaximum == 0.0);
    Float32 convert = 0.5f;
    CHECK(get(5, kAudioLevelControlPropertyConvertScalarToDecibels, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof convert, &size, &convert) == 0);
    CHECK(fabsf(convert + 9.0f) < 1e-4f);
    const AudioObjectPropertyAddress va = {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0};
    const AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0};
    const AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0};
    Boolean settable = 0;
    CHECK(D->IsPropertySettable(gDriver, 5, 0, &va, &settable) == 0 && settable);
    CHECK(D->IsPropertySettable(gDriver, 2, 0, &na, &settable) == 0 && !settable);
    CHECK(D->IsPropertySettable(gDriver, 2, 0, &ra, &settable) == 0 && settable);
    Float64 bad = 44100, good = 48000;
    CHECK(D->SetPropertyData(gDriver, 2, 0, &ra, 0, NULL, sizeof bad, &bad) == kAudioDeviceUnsupportedFormatError);
    CHECK(D->SetPropertyData(gDriver, 2, 0, &ra, 0, NULL, sizeof good, &good) == 0);
    CHECK(D->HasProperty(gDriver, 2, 0, &ra) && D->HasProperty(gDriver, 5, 0, &va) && !D->HasProperty(gDriver, 9, 0, &va));
    // What the plug-in cannot answer is recorded once in the control page (selector, object << 24 | scope letter) for the boot log.
    const UInt32 before = diag()->count;
    UInt32 value = 0;
    CHECK(get(2, 'zzzz', kAudioObjectPropertyScopeOutput, 0, NULL, sizeof value, &size, &value) == kAudioHardwareUnknownPropertyError);
    CHECK(get(2, 'zzzz', kAudioObjectPropertyScopeOutput, 0, NULL, sizeof value, &size, &value) == kAudioHardwareUnknownPropertyError);
    CHECK(diag()->count == before + 1 && diag()->selector[before] == 'zzzz' && diag()->where[before] == (2u << 24 | 'o'));
    CHECK(get(9, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof value, &size, &value) == kAudioHardwareBadObjectError);
    // Too small a buffer is refused, not overrun.
    uint8_t small[2];
    CHECK(get(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof small, &size, small) ==
          kAudioHardwareBadPropertySizeError);
    // Every distinct access went into the trace section, in order.
    const PaganiAudioTrace *trace = (const PaganiAudioTrace *)((const uint8_t *)gPage + PAGANI_AUDIO_TRACE_OFFSET);
    CHECK(trace->count > 40 && trace->count <= PAGANI_AUDIO_TRACE_SLOTS);
}

static void test_io(void)
{
    PaganiAudioControl *c = gPage;
    CHECK(get32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(D->StartIO(gDriver, 2, 7) == 0 && D->StartIO(gDriver, 2, 8) == 0);   // two IO clients of the audio server
    CHECK(get32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(gEngine.out_users == 1 && gEngine.in_users == 1 && gEngine.running && gEngine.capturing);
    // The zero time stamp lies on the service's timeline: host time = anchor + sample time x ticks per frame.
    Float64 sampleTime = -1;
    UInt64 host = 0, seed = 0;
    CHECK(D->GetZeroTimeStamp(gDriver, 2, 7, &sampleTime, &host, &seed) == 0 && seed > 0);
    const uint64_t expected = pagani_audio_engine_host_at(&gEngine, (uint64_t)sampleTime);
    CHECK(host + 1 >= expected && host <= expected + 1 && (uint64_t)sampleTime <= pagani_audio_engine_frame_at(&gEngine, mach_absolute_time()));
    // The audio server mixes 512 frames 20000 frames ahead (output time) at volume 1: they land in the playback ring at their sample
    // time, added to what is there.
    CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 1.0f) == 0);
    const uint64_t now = pagani_audio_engine_frame_at(&gEngine, mach_absolute_time());
    static Float32 mix[512 * 2];
    for (unsigned i = 0; i < 512; ++i) {mix[2 * i] = 0.25f; mix[2 * i + 1] = -0.5f;}
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);
    cycle.mOutputTime.mSampleTime = (Float64)(now + 20000);
    cycle.mCurrentTime.mSampleTime = (Float64)now;
    Boolean will = 0, inPlace = 0;
    CHECK(D->WillDoIOOperation(gDriver, 2, 7, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace) == 0 && will && inPlace);
    CHECK(D->WillDoIOOperation(gDriver, 2, 7, kAudioServerPlugInIOOperationReadInput, &will, &inPlace) == 0 && will);
    CHECK(D->WillDoIOOperation(gDriver, 2, 7, kAudioServerPlugInIOOperationMixOutput, &will, &inPlace) == 0 && !will);
    CHECK(D->DoIOOperation(gDriver, 2, 3, 7, kAudioServerPlugInIOOperationWriteMix, 512, &cycle, mix, NULL) == 0);
    const uint64_t t = now + 20000;
    CHECK(gOut[2 * (t % PAGANI_AUDIO_RING_FRAMES)] == 8191 && gOut[2 * (t % PAGANI_AUDIO_RING_FRAMES) + 1] == -16383);
    CHECK(c->out_written == t + 512 && diag()->writes == 1 && diag()->io_frames == 512 && diag()->mix_peak == 16383);
    CHECK(diag()->ahead_min >= 19990 && diag()->ahead_min <= 20000);
    // A second IO context mixes into the same frames: the sum, saturated.
    CHECK(D->DoIOOperation(gDriver, 2, 3, 8, kAudioServerPlugInIOOperationWriteMix, 512, &cycle, mix, NULL) == 0);
    CHECK(gOut[2 * (t % PAGANI_AUDIO_RING_FRAMES)] == 16382 && gOut[2 * (t % PAGANI_AUDIO_RING_FRAMES) + 1] == -32766);
    CHECK(diag()->overlaps == 1 && diag()->overlap_max == 512);
    // At volume 0 nothing is added.
    CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 0.0f) == 0);
    cycle.mOutputTime.mSampleTime = (Float64)(t + 512);
    CHECK(D->DoIOOperation(gDriver, 2, 3, 7, kAudioServerPlugInIOOperationWriteMix, 512, &cycle, mix, NULL) == 0);
    CHECK(gOut[2 * ((t + 512) % PAGANI_AUDIO_RING_FRAMES)] == 0 && c->out_written == t + 1024);
    // A late IO cycle: frames behind what the service has taken are dropped (they would play a ring turn later); the rest is mixed.
    CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 1.0f) == 0);
    kernelRound();
    const uint64_t taken = c->out_taken, R = PAGANI_AUDIO_RING_FRAMES;
    CHECK(taken % PAGANI_AUDIO_PERIOD_FRAMES == 0 && taken == gEngine.next * PAGANI_AUDIO_PERIOD_FRAMES);
    cycle.mOutputTime.mSampleTime = (Float64)(taken - 32);
    CHECK(D->DoIOOperation(gDriver, 2, 3, 7, kAudioServerPlugInIOOperationWriteMix, 64, &cycle, mix, NULL) == 0);
    CHECK(diag()->dropped_frames == 32 && diag()->dropped_writes == 1);
    CHECK(gOut[2 * ((taken - 32) % R)] == 0 && gOut[2 * ((taken - 1) % R)] == 0 && gOut[2 * (taken % R)] == 8191);
    // What lies ahead of it is taken by the silent backend once the play position is within four periods: 100 ms later it is clear.
    usleep(100000);
    kernelRound();
    CHECK(gOut[2 * (taken % R)] == 0 && gOut[2 * ((taken + 31) % R)] == 0 && c->out_taken > taken + 4 * PAGANI_AUDIO_PERIOD_FRAMES);
    CHECK(gEngine.stats.peak_out >= 8191 && gEngine.stats.late == 0);
    // Frames 20000 ahead are still there (416 ms ahead of the play position when written).
    CHECK(gOut[2 * (t % R)] == 16382);
    // Input: frames the service delivered read from the capture ring; frames not yet delivered read silence and count as a miss.
    kernelRound();
    const uint64_t written = c->in_written;
    CHECK(written > 0);
    gIn[(written - 10) % PAGANI_AUDIO_RING_FRAMES] = 16384;
    static Float32 input[512];
    cycle.mInputTime.mSampleTime = (Float64)(written - 100);
    CHECK(D->DoIOOperation(gDriver, 2, 4, 7, kAudioServerPlugInIOOperationReadInput, 100, &cycle, input, NULL) == 0);
    CHECK(input[90] == 0.5f && input[0] == 0.0f && diag()->in_misses == 0);
    cycle.mInputTime.mSampleTime = (Float64)(written - 10);
    CHECK(D->DoIOOperation(gDriver, 2, 4, 7, kAudioServerPlugInIOOperationReadInput, 100, &cycle, input, NULL) == 0);
    CHECK(input[0] == 0.5f && input[50] == 0.0f && diag()->in_misses == 1);
    // Both IO clients stop: the requests reach the service once the last one does.
    CHECK(D->StopIO(gDriver, 2, 7) == 0 && gEngine.out_users == 1);
    CHECK(D->StopIO(gDriver, 2, 8) == 0 && gEngine.out_users == 0 && gEngine.in_users == 0);
    CHECK(D->StopIO(gDriver, 2, 8) == 0 && gEngine.out_users == 0);   // one too many is harmless
    CHECK(get32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
}

static PaganiAudioProbe *probe(void) {return (PaganiAudioProbe *)((uint8_t *)gPage + PAGANI_AUDIO_PROBE_OFFSET);}

static void test_probe(void)
{
    PaganiAudioControl *c = gPage;
    PaganiAudioProbe *p = probe();
    // Without the option (the kernel's pagani-audio-probe=0) the probe asks the HAL nothing and writes nothing.
    c->options &= ~(uint32_t)PaganiAudioOptionProbeRoute;
    int calls = gHalCalls;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(gHalCalls == calls && p->seq == 0);
    // With it: the device list, this device (HAL id 44) and its translated 'vold', the default virtual device's 'vcsp' and volume, the
    // routing layer's category, mode and the default device's two active output ports with their types; seq even once complete.
    c->options |= PaganiAudioOptionProbeRoute;
    const uint32_t queries = p->vol_queries;
    gHalRunning = 1;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->seq == 2 && p->reason == PaganiAudioProbeTimer && p->devices == 4 && p->ours == kHalOurs && p->vad == kHalVad && p->va_plugin == kHalVa);
    CHECK(p->ours_vold == 1 && p->ours_running == 1 && p->vcsp_status == 0 && p->vcsp == 1 && p->vad_volume_milli == 500);
    CHECK(p->category == 'cmed' && p->mode == 'imdf' && p->ports_status == 0 && p->out_ports == 2);
    CHECK(p->port_type[0] == 'ptbo' && p->port_type[1] == 'pspk');
    CHECK(p->vol_queries > queries);   // the HAL asked the volume control for 'lcdv' on behalf of 'vold'
    // The answers the HUD depends on, when they go wrong: no volume control found for 'vold', 'vcsp' 0 or failing, no routing layer.
    gHalVoldControl = 0;
    gHalVcsp = 0;
    gHalRunning = 0;
    pagani_audio_probe_route(PaganiAudioProbeVolume);
    CHECK(p->seq == 4 && p->reason == PaganiAudioProbeVolume && p->ours_vold == 0 && p->vcsp == 0 && p->ours_running == 0);
    gHalVcspStatus = kAudioHardwareUnknownPropertyError;
    gHalVa = 0;
    pagani_audio_probe_route(PaganiAudioProbeStart);
    CHECK(p->seq == 6 && p->vcsp_status == kAudioHardwareUnknownPropertyError && p->va_plugin == 0 && p->category == 0 && p->mode == 0);
    // No active output port: t0/t1 say why; without the routing layer nothing of it is readable but this device's hog mode (free).
    CHECK(p->ports_status == kAudioHardwareBadObjectError && p->out_ports == 0 && p->port_type[0] == 0 && p->port_type[1] == '?h?t');
    // No default virtual device: no 'vcsp', no volume; the UID found through the translation form when the qualified one is refused.
    gHalVad = 0;
    gHalUidQualifier = 0;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->seq == 8 && p->vad == 0 && p->vcsp_status == kAudioHardwareBadObjectError && p->vad_volume_milli == 0xffffffffu);
    CHECK(p->ours == kHalOurs && p->devices == 4);
    // A volume change queues one probe 2 s later (one at a time: the rest of a burst rides along), run on the probe queue.
    gHalVad = gHalVa = gHalVoldControl = gHalUidQualifier = 1;
    gHalVcsp = 1;
    gHalVcspStatus = 0;
    for (int i = 0; i < 5; ++i) CHECK(set_float(5, kAudioLevelControlPropertyScalarValue, 0.25f * (Float32)(i % 4)) == 0);
    CHECK(p->seq == 8);
    usleep(2600000);
    CHECK(p->seq == 10 && p->reason == PaganiAudioProbeVolume && p->vcsp == 1 && p->vad == kHalVad && p->va_plugin == kHalVa);
}

// The 13T of 2026-10-31: the routing layer answers, but the default virtual device has no active output port (out=0). The probe then
// writes in t0 the first connected output port and in t1 why the route is empty: MediaExperience's serialization listener, this
// device hogged by the audio server (the routing layer took it), the number of connected output ports, a Thunderbolt one among them.
static void test_probe_why(void)
{
    CHECK(pagani_audio_probe_why(1, 1, 0, 1, 1) == 'SH1T' && pagani_audio_probe_why(0, 0, 0, 0, 0) == 'sh0t');
    CHECK(pagani_audio_probe_why(-1, -2, kAudioHardwareBadObjectError, 0, 0) == '???t' && pagani_audio_probe_why(1, -1, 0, 12, 0) == 'Sx+t');
    PaganiAudioProbe *p = probe();
    gHalActive = 0;
    gHalVcsp = 0;
    // No serialization listener yet: the routing layer takes no device of ours (only the six special UIDs) and no route changes.
    gHalSerial = 0;
    gHalHog = -1;
    gHalConnected = 0;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->out_ports == 0 && p->ports_status == 0 && p->vcsp == 0 && p->port_type[0] == 0 && p->port_type[1] == 'sh0t');
    // The listener is there but the device was not taken (not hogged, nothing connected).
    gHalSerial = 1;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->port_type[0] == 0 && p->port_type[1] == 'Sh0t');
    // Taken and connected, but not routed: hogged by this process, the Thunderbolt port connected (after a speaker).
    gHalHog = getpid();
    gHalConnected = 2;
    gHalConnectedPorts[0] = kHalPort2;
    gHalConnectedPorts[1] = kHalPort;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->port_type[0] == 'pspk' && p->port_type[1] == 'SH2T');
    // Hogged by another process; a HAL that does not answer 'oink'; a port whose type cannot be read is counted but not typed.
    gHalHog = 4242;
    gHalConnected = 1;
    gHalConnectedPorts[0] = 1234;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->port_type[0] == 0 && p->port_type[1] == 'Sx1t');
    gHalHogKnown = 0;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->port_type[1] == 'S?1t');
    // Active ports again: t0/t1 are their types, as before.
    gHalHogKnown = gHalActive = 1;
    gHalVcsp = 1;
    pagani_audio_probe_route(PaganiAudioProbeTimer);
    CHECK(p->out_ports == 2 && p->port_type[0] == 'ptbo' && p->port_type[1] == 'pspk' && p->vcsp == 1);
}

// ==== the second run ("plugin actuator"): a service of revision 2 offering the actuator, MediaExperience's progress and the repair ======
static PaganiAudioActuatorControl *act(void) {return (PaganiAudioActuatorControl *)gActMemory;}
static PaganiAudioMx *mx(void) {return (PaganiAudioMx *)((uint8_t *)gPage + PAGANI_AUDIO_MX_OFFSET);}

// The kernel's start with pagani-haptics-ios and pagani-haptics=1 (and pagani-audio-mx, pagani-audio-srzl): what PaganiAudio.cpp does.
static void kernelUpRevision2(void)
{
    gPage = aligned_alloc(4096, PAGANI_AUDIO_CONTROL_BYTES);
    gOut = calloc(PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS, sizeof(int16_t));
    gIn = calloc(PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS, sizeof(int16_t));
    gActMemory = aligned_alloc(4096, PAGANI_AUDIO_ACT_BYTES);
    const uint32_t options = PaganiAudioOptionProbeRoute | PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair | PaganiAudioOptionActuator;
    CHECK(pagani_audio_engine_init(&gEngine, gPage, gOut, gIn, mach_absolute_time(), gTimebase.numer, gTimebase.denom,
                                   &pagani_audio_silent_backend, options) == 0);
    CHECK(pagani_audio_haptics_init(&gBridge, &gEngine, gActMemory, &kVibOps, 8000, 0, 0) == 0);
    PaganiAudioRevision *r = (PaganiAudioRevision *)((uint8_t *)gPage + PAGANI_AUDIO_REVISION_OFFSET);
    r->actuator_frames = PAGANI_AUDIO_ACT_RING_FRAMES;
    r->actuator_rate = PAGANI_AUDIO_ACT_RATE;
    r->revision = PAGANI_AUDIO_REVISION;
    r->magic = PAGANI_AUDIO_REVISION_MAGIC;
    ((PaganiAudioControl *)gPage)->state |= PaganiAudioOutputReady | PaganiAudioInputReady;
    gKernelUp = 1;
}

static void test_two_devices(void)
{
    test_interface();
    CHECK(D->Initialize(gDriver, &gHostInterface) == 0);
    // Four memories mapped; the header checked out; the plug-in counted its load there. With the service the option decides: MobileGestalt
    // is not asked.
    CHECK(gOpens == 1 && gMaps == 4 && act()->loads == 1 && gMgCalls == 0);
    AudioObjectID ids[8];
    UInt32 size = 0;
    CHECK(get(1, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0);
    CHECK(size == 2 * sizeof(AudioObjectID) && ids[0] == 2 && ids[1] == 6);
    CHECK(get(1, kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 8);
    CHECK(get(1, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, 4, &size, ids) == 0 && size == 4);   // what fits
    CFStringRef actuatorUid = CFSTR("Actuator"), ours = CFSTR("PaganiAudioDevice"), lower = CFSTR("actuator");
    AudioObjectID found = 0;
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof actuatorUid, &actuatorUid, sizeof found,
              &size, &found) == 0 && found == 6);
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof ours, &ours, sizeof found, &size, &found) == 0 &&
          found == 2);
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof lower, &lower, sizeof found, &size, &found) == 0 &&
          found == kAudioObjectUnknown);
    // The actuator device: UID exactly "Actuator", built in, never a default, output only, no controls, a 1.5 cycle lead.
    CHECK(get32(6, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClassID);
    CHECK(get32(6, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(string_is(6, kAudioDevicePropertyDeviceUID, "Actuator") && string_is(6, kAudioObjectPropertyName, "Actuator"));
    CHECK(string_is(6, kAudioDevicePropertyModelUID, "PaganiActuatorModel"));
    CHECK(get32(6, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeBuiltIn);
    CHECK(get32(6, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == 0);
    CHECK(get32(6, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeOutput) == 0);
    CHECK(get32(6, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) == PAGANI_AUDIO_ACT_SAFETY_FRAMES);
    CHECK(get32(6, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) == 0);
    CHECK(get32(6, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) == PAGANI_AUDIO_ACT_RING_FRAMES);
    CHECK(get32(6, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(get32(6, 'accs', kAudioObjectPropertyScopeGlobal) == 0 && get32(6, 'auto', kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(get(6, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 4 && ids[0] == 7);
    CHECK(get(6, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0, NULL, sizeof ids, &size, ids) == 0 && size == 4);
    CHECK(get(6, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    CHECK(get(6, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    CHECK(get(6, kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    uint8_t layout[64];
    CHECK(get(6, kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, 0, NULL, sizeof layout, &size, layout) == 0);
    CHECK(((const AudioChannelLayout *)layout)->mNumberChannelDescriptions == 1);
    // No input, no data source, no volume or mute: what would make VirtualAudio's Device_Actuator_Aspen wait or set controls.
    UInt32 value = 0;
    CHECK(get(6, 'ssrc', kAudioObjectPropertyScopeInput, 0, NULL, sizeof value, &size, &value) == kAudioHardwareUnknownPropertyError);
    const AudioObjectPropertyAddress vold = {'vold', kAudioObjectPropertyScopeOutput, 0}, mute = {'mute', kAudioObjectPropertyScopeOutput, 0};
    CHECK(!D->HasProperty(gDriver, 6, 0, &vold) && !D->HasProperty(gDriver, 6, 0, &mute));
    // The stream: output, 48 kHz mono Float32 packed (flags 9), one format.
    CHECK(get32(7, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioStreamClassID);
    CHECK(get32(7, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == 6 && get32(7, kAudioStreamPropertyDirection, 0) == 0);
    AudioStreamBasicDescription f;
    CHECK(get(7, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof f, &size, &f) == 0);
    CHECK(f.mSampleRate == 48000 && f.mChannelsPerFrame == 1 && f.mFormatFlags == 9 && f.mBytesPerFrame == 4 && f.mBitsPerChannel == 32);
    CHECK(get(7, 'taps', kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0 && size == 0);
    // Writes: its format and rate accepted, others refused; anything unknown accepted and recorded.
    const AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0},
                                     ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0},
                                     unknown = {'zork', kAudioObjectPropertyScopeOutput, 0};
    Boolean settable = 0;
    CHECK(D->IsPropertySettable(gDriver, 7, 0, &fa, &settable) == 0 && settable);
    CHECK(D->SetPropertyData(gDriver, 7, 0, &fa, 0, NULL, sizeof f, &f) == 0 && act()->format_sets == 1);
    f.mChannelsPerFrame = 2;
    CHECK(D->SetPropertyData(gDriver, 7, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
    Float64 rate = 48000, bad = 44100;
    CHECK(D->SetPropertyData(gDriver, 6, 0, &ra, 0, NULL, sizeof rate, &rate) == 0 && act()->format_sets == 2);
    CHECK(D->SetPropertyData(gDriver, 6, 0, &ra, 0, NULL, sizeof bad, &bad) == kAudioDeviceUnsupportedFormatError);
    CHECK(D->SetPropertyData(gDriver, 6, 0, &unknown, 0, NULL, sizeof value, &value) == 0);
    CHECK(act()->unknown_sets == 1 && act()->unknown_selector == 'zork' && act()->unknown_where >> 24 == 6);
    // The speaker device is as before.
    CHECK(get32(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeThunderbolt);
    CHECK(get32(2, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == 1);
}

static void test_actuator_io(void)
{
    // IO of the actuator is its own: one kernel request, the bridge's run, the speaker device untouched.
    CHECK(get32(6, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(D->StartIO(gDriver, 6, 21) == 0 && D->StartIO(gDriver, 6, 22) == 0);
    CHECK(gEngine.act_users == 1 && gEngine.out_users == 0 && act()->io_starts == 1 && gBridge.running);
    CHECK(get32(6, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(get32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    Float64 sampleTime = -1;
    UInt64 host = 0, seed = 0;
    CHECK(D->GetZeroTimeStamp(gDriver, 6, 21, &sampleTime, &host, &seed) == 0 && seed > 0);
    CHECK(host + 1 >= pagani_audio_engine_host_at(&gEngine, (uint64_t)sampleTime) && host <= pagani_audio_engine_host_at(&gEngine, (uint64_t)sampleTime) + 1);
    Boolean will = 0, inPlace = 0;
    CHECK(D->WillDoIOOperation(gDriver, 6, 21, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace) == 0 && will);
    CHECK(D->WillDoIOOperation(gDriver, 6, 21, kAudioServerPlugInIOOperationReadInput, &will, &inPlace) == 0 && !will);
    // The haptic server's IO cycle: 256 frames of a 170 Hz tap written 384 frames ahead (the safety offset plus a cycle).
    const uint64_t now = pagani_audio_engine_frame_at(&gEngine, mach_absolute_time());
    static Float32 tap[256];
    for (unsigned i = 0; i < 256; ++i) tap[i] = 0.5f * sinf(2.0f * (float)M_PI * 170.0f * (float)i / 48000.0f);
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);
    cycle.mOutputTime.mSampleTime = (Float64)(now + 384);
    CHECK(D->DoIOOperation(gDriver, 6, 7, 21, kAudioServerPlugInIOOperationWriteMix, 256, &cycle, tap, NULL) == 0);
    const int16_t *ring = (const int16_t *)((const uint8_t *)gActMemory + PAGANI_AUDIO_ACT_HEADER_BYTES);
    const uint64_t t = now + 384, mask = PAGANI_AUDIO_ACT_RING_FRAMES - 1;
    CHECK(ring[(t + 10) & mask] == (int16_t)(tap[10] * 32767.0f) && act()->written == t + 256 && act()->writes == 1);
    CHECK(act()->ahead_min >= 380 && act()->ahead_min <= 384 && act()->peak >= 16383 && act()->sound_frame == t + 1 && act()->sound_host);
    // A second IO context mixes into the same frames: the sum.
    CHECK(D->DoIOOperation(gDriver, 6, 7, 22, kAudioServerPlugInIOOperationWriteMix, 256, &cycle, tap, NULL) == 0);
    CHECK(abs(ring[(t + 10) & mask] - 2 * (int16_t)(tap[10] * 32767.0f)) <= 1);
    // The speaker device's ring saw nothing of it.
    CHECK(((PaganiAudioControl *)gPage)->out_written == 0);
    // The kernel takes it as it comes due: the vibrator gets a stream at 8 kHz and the default cap, 0.5 * 2 = 1.0 -> clipped at 127.
    for (int i = 0; i < 40; ++i) {usleep(2000); kernelRound();}
    CHECK(gVibBegins == 1 && gVibRate == 8000 && gVibMv == 3600 && gVibPeak == 127 && gBridge.stats.latency_us >= 0);
    CHECK(ring[(t + 10) & mask] == 0 && act()->taken > t + 256);
    // Too late: frames behind what the kernel took are dropped and counted.
    cycle.mOutputTime.mSampleTime = (Float64)(act()->taken - 32);
    CHECK(D->DoIOOperation(gDriver, 6, 7, 21, kAudioServerPlugInIOOperationWriteMix, 256, &cycle, tap, NULL) == 0);
    CHECK(act()->dropped_frames == 32 && act()->dropped_writes == 1);
    // Both clients stop: one request, the run ends after the keep time.
    CHECK(D->StopIO(gDriver, 6, 21) == 0 && gEngine.act_users == 1 && D->StopIO(gDriver, 6, 22) == 0 && gEngine.act_users == 0);
    CHECK(act()->io_stops == 1);
    for (int i = 0; i < 20; ++i) {usleep(2000); kernelRound();}
    CHECK(!gBridge.running && gBridge.stats.run_ends == 1 && gVibEnds >= 1);
    // The speaker device's IO does not touch the actuator.
    CHECK(D->StartIO(gDriver, 2, 1) == 0 && gEngine.act_users == 0 && gEngine.out_users == 1 && D->StopIO(gDriver, 2, 1) == 0);
    // What the audio server made of it: before VirtualAudio took it (no virtual actuator, the device free), then after.
    pagani_audio_probe_actuator();
    CHECK(act()->va_device == 0 && act()->hog == 0);
    gHalVadActuator = 1;
    pagani_audio_probe_actuator();
    CHECK(act()->va_device == kHalVadActuator && act()->hog == 1);
}

static void test_mx(void)
{
    PaganiAudioMx *m = mx();
    // Not 21H461's code at MXInitialize: nothing read, no repair.
    gHalSerial = 0;
    pagani_audio_probe_mx();
    CHECK(m->gen == 1 && m->progress == PaganiAudioMxUnverified && m->acq_ms == -2 && gSetCalls == 0);
    // MediaExperience before its first step; then step by step. The repair waits until it made its attempt (B and C).
    fakeLayout();
    pagani_audio_va_image(&gVaImage.header, 0);
    pagani_audio_probe_mx();
    CHECK(m->gen == 2 && m->progress == (2u << PaganiAudioMxFilterShift) && m->srzl_state == PaganiAudioSrzlWaiting && gSetCalls == 0);
    gMxOnce = 0x1234;
    gMxSession = gMxScreen = gMxIds = gMxRoutes = gMxContext = gMxEndpoint = 1;
    pagani_audio_probe_mx();
    CHECK(m->progress == (0xfdu | 2u << PaganiAudioMxFilterShift) && m->srzl_state == PaganiAudioSrzlWaiting && gSetCalls == 0);
    // The once predicate as dispatch_once_f leaves it: a thread port while it runs (with waiters: low bit), then a generation (low bits 2).
    gMxOnce = 0x1235;
    pagani_audio_probe_mx();
    CHECK(!(m->progress & PaganiAudioMxDone));
    gMxOnce = (77u << 2) + 2u;
    pagani_audio_probe_mx();
    CHECK(m->progress & PaganiAudioMxDone);
    gMxOnce = 0x1234;
    // The audio control queue: set and answering.
    dispatch_queue_t acq = dispatch_queue_create("test.acq", DISPATCH_QUEUE_SERIAL);
    gMxQueue = (uint64_t)(uintptr_t)acq;
    gMxVaPlugin = 63;
    pagani_audio_probe_mx();
    CHECK((m->progress & (PaganiAudioMxQueue | PaganiAudioMxQueueAlive | PaganiAudioMxVaPlugin)) ==
          (PaganiAudioMxQueue | PaganiAudioMxQueueAlive | PaganiAudioMxVaPlugin) && m->acq_ms >= 0 && m->acq_ms < 1000);
    CHECK(!(m->progress & PaganiAudioMxSerial) && m->srzl_state == PaganiAudioSrzlWaiting && gSetCalls == 0);
    // A blocked queue: no answer within 2 s, and no second block sent while the first waits.
    dispatch_suspend(acq);
    pagani_audio_probe_mx();
    CHECK(m->acq_ms == -1 && !(m->progress & PaganiAudioMxQueueAlive));
    pagani_audio_probe_mx();
    CHECK(m->acq_ms == -1);
    dispatch_resume(acq);
    usleep(100000);
    // C: the attempt was made and lost (listener empty, VirtualAudio initialised): the repair, once, with MediaExperience's own listeners.
    gMxSerialDone[0] = 'csav';
    gMxOnce = ~0ull;
    pagani_audio_probe_mx();
    CHECK((m->progress & 0x3ffu) == 0x3ffu && m->srzl_state == PaganiAudioSrzlDone && m->attempts == 1 && gSetCalls == 2);
    CHECK(gSetSerial[0] == (uint64_t)(uintptr_t)fakeSerializationListener && gSetSerial[1] == 0);
    CHECK(gSetReceiver[0] == (uint64_t)(uintptr_t)fakeReceiverListener && gSetReceiver[1] == 0);
    CHECK(m->srzl_status == 0 && m->rwbu_status == 0 && m->serial_after == 'S' && m->filter_before == 2 && m->filter_after == 3);
    // Afterwards the listener is there: no second attempt, the state stays.
    pagani_audio_probe_mx();
    CHECK(gSetCalls == 2 && m->srzl_state == PaganiAudioSrzlDone && m->attempts == 1 && (m->progress >> PaganiAudioMxFilterShift & 3) == 3);
    // A fresh boot where the listener's code is not 21H461's: refused, nothing set.
    pagani_audio_mx_reset();
    gHalSerial = 0;
    gFakeLayout.serial_code[3] ^= 0xff;
    m->srzl_state = 0;
    pagani_audio_probe_mx();
    CHECK(m->srzl_state == PaganiAudioSrzlRefused && gSetCalls == 2 && m->attempts == 2);
    // And one where the HAL refuses the write: failed, the status kept, 'rwbu' not tried.
    pagani_audio_mx_reset();
    gFakeLayout.serial_code[3] ^= 0xff;
    gSetResult = kAudioHardwareIllegalOperationError;
    pagani_audio_probe_mx();
    CHECK(m->srzl_state == PaganiAudioSrzlFailed && m->srzl_status == kAudioHardwareIllegalOperationError && gSetCalls == 3);
    gSetResult = 0;
    // Without either option nothing is looked at.
    ((PaganiAudioControl *)gPage)->options &= ~(uint32_t)(PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair);
    const uint32_t gen = m->gen;
    pagani_audio_probe_mx();
    CHECK(m->gen == gen);
    dispatch_release(acq);
}

// ==== "plugin vasettings <case>": VirtualAudio's actuator settings (option PaganiAudioOptionVaSettings), each case one process (once per boot).
// "before": VirtualAudio's image is known when the service answers; "after": it arrives after Initialize (Pagani sorts before Virtual); "late":
// its device manager exists already; "present": the static is initialised; "uuid", "product": not what the addresses are for; "nosymbol": no
// answer from MobileGestalt; "off": no option; "noactuator": option but no actuator. Nothing is written in the refusing cases.
static void test_va_settings(const char *which)
{
    test_interface();
    fakeLayout();
    gVaImage.manager = 0;
    if (!strcmp(which, "late")) gVaImage.manager = (uint64_t)(uintptr_t)gVaManager;
    if (!strcmp(which, "present")) gVaImage.settings_guard = 1;
    if (!strcmp(which, "uuid")) gVaImage.uuid.uuid[3] ^= 1;
    if (!strcmp(which, "product")) gMgProduct = 3892022640ull;
    if (!strcmp(which, "nosymbol")) gMgProduct = 0;
    kernelUpRevision2();
    PaganiAudioControl *c = gPage;
    if (strcmp(which, "off")) c->options |= PaganiAudioOptionVaSettings;
    if (!strcmp(which, "noactuator")) c->options &= ~(uint32_t)PaganiAudioOptionActuator;
    const int after = !strcmp(which, "after");
    if (!after) pagani_audio_va_image(&gVaImage.header, 0);
    CHECK(D->Initialize(gDriver, &gHostInterface) == 0);
    PaganiAudioMx *m = mx();
    if (after) {
        CHECK(m->va_settings == PaganiAudioVaSettingsNone && gVaImage.settings_guard == 0);   // nothing yet: the image is not there
        pagani_audio_va_image(&gVaImage.header, 0);
    }
    const int given = !strcmp(which, "before") || after || !strcmp(which, "late");
    if (given) {
        // The table's row for a product without I/V sense, the guard's first byte done; what a use of the static then finds.
        CHECK(gVaImage.settings_kind == 1 && gVaImage.settings_format == 'dflt' && (gVaImage.settings_guard & 0xffu) == 1);
        CHECK((gVaImage.settings_guard >> 8) == 0);
        CHECK(m->va_settings == (!strcmp(which, "late") ? PaganiAudioVaSettingsLate : PaganiAudioVaSettingsGiven));
    } else {
        CHECK(gVaImage.settings_kind == 0 && gVaImage.settings_format == 0);
        CHECK(gVaImage.settings_guard == (uint64_t)!strcmp(which, "present"));
        CHECK(m->va_settings == (!strcmp(which, "present") ? PaganiAudioVaSettingsPresent :
                                 !strcmp(which, "uuid") ? PaganiAudioVaSettingsOtherBuild :
                                 !strcmp(which, "product") || !strcmp(which, "nosymbol") ? PaganiAudioVaSettingsOtherProduct :
                                 PaganiAudioVaSettingsNone));
    }
    // Once: a second report of the image changes nothing, not even the state.
    gVaImage.settings_kind ^= 0x100u;
    const uint32_t kind = gVaImage.settings_kind, state = m->va_settings;
    pagani_audio_va_image(&gVaImage.header, 0);
    CHECK(gVaImage.settings_kind == kind && m->va_settings == state);
}

// ==== the third run ("plugin listed <case>"): iOS expects the actuator and the ring is not there: the device is listed at Initialize all the
// same (VirtualAudio would wait 8 s for it), writes are answered as ever, its IO is refused like the speaker's without a service.
static void check_listed(void)
{
    AudioObjectID ids[4];
    UInt32 size = 0;
    CHECK(get(1, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof ids, &size, ids) == 0);
    CHECK(size == 2 * sizeof(AudioObjectID) && ids[0] == 2 && ids[1] == 6);
    CFStringRef uid = CFSTR("Actuator");
    AudioObjectID found = 0;
    CHECK(get(1, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof uid, &uid, sizeof found, &size, &found) == 0 &&
          found == 6);
    CHECK(string_is(6, kAudioDevicePropertyDeviceUID, "Actuator") && get32(6, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(get32(7, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 0);
    const Float64 rate = 48000.0;
    const AudioObjectPropertyAddress nominal = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                     unknown = {'wxyz', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK(D->SetPropertyData(gDriver, 6, 0, &nominal, 0, NULL, sizeof rate, &rate) == 0);
    CHECK(D->SetPropertyData(gDriver, 6, 0, &unknown, 0, NULL, sizeof rate, &rate) == 0);
    CHECK(D->StartIO(gDriver, 6, 1) == kAudioHardwareNotRunningError && D->StopIO(gDriver, 6, 1) == 0);
    Float64 sampleTime = 0;
    UInt64 host = 0, seed = 0;
    CHECK(D->GetZeroTimeStamp(gDriver, 6, 1, &sampleTime, &host, &seed) == kAudioHardwareNotRunningError);
    static Float32 frames[16];
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);
    CHECK(D->DoIOOperation(gDriver, 6, 7, 1, kAudioServerPlugInIOOperationWriteMix, 16, &cycle, frames, NULL) == 0);
    pagani_audio_probe_actuator();
}

static void test_listed(const char *which)
{
    test_interface();
    if (!strcmp(which, "expected")) {
        // No service at Initialize, and MobileGestalt says haptics.
        gMgHaptics = 1;
        CHECK(D->Initialize(gDriver, &gHostInterface) == 0);
        CHECK(gOpens == 0 && gMgCalls == 1);
        check_listed();
        // The service comes up late: the actuator's next StartIO connects, the ring maps, IO runs.
        kernelUpRevision2();
        CHECK(D->StartIO(gDriver, 6, 1) == 0 && gOpens == 1 && gMaps == 4 && gEngine.act_users == 1 && act()->io_starts == 1);
        CHECK(D->StopIO(gDriver, 6, 1) == 0 && gEngine.act_users == 0);
        return;
    }
    // A service whose option says the node is there, and a ring that cannot be used.
    kernelUpRevision2();
    if (!strcmp(which, "header")) act()->magic = 0;
    else if (!strcmp(which, "revision")) ((PaganiAudioRevision *)((uint8_t *)gPage + PAGANI_AUDIO_REVISION_OFFSET))->actuator_frames = 4096;
    else if (!strcmp(which, "memory")) gActMemoryRefused = 1;
    else CHECK(!"known case");
    CHECK(D->Initialize(gDriver, &gHostInterface) == 0);
    CHECK(gOpens == 1 && gMgCalls == 0);
    check_listed();
    // The speaker device is not touched by any of it.
    CHECK(D->StartIO(gDriver, 2, 1) == 0 && gEngine.out_users == 1 && D->StopIO(gDriver, 2, 1) == 0);
}

int main(int argc, char **argv)
{
    mach_timebase_info(&gTimebase);
    if (argc > 2 && !strcmp(argv[1], "vasettings")) {
        test_va_settings(argv[2]);
        printf("PASS plugin vasettings %s: %u checks (VirtualAudio's actuator settings given or refused, once)\n", argv[2], checks);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "listed")) {
        test_listed(argv[2]);
        printf("PASS plugin listed %s: %u checks (the actuator device without its ring, IO refused)\n", argv[2], checks);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "actuator")) {
        kernelUpRevision2();
        test_two_devices();
        test_actuator_io();
        test_mx();
        printf("PASS plugin actuator: %u checks (two devices, actuator IO into the bridge, MediaExperience progress and the 'srzl' repair)\n",
               checks);
        return 0;
    }
    test_interface();
    test_without_service();
    test_properties();
    test_io();
    test_probe();
    test_probe_why();
    printf("PASS plugin: %u checks (host timebase %u/%u, %llu periods taken)\n", checks, gTimebase.numer, gTimebase.denom,
           (unsigned long long)gEngine.stats.taken);
    return 0;
}
