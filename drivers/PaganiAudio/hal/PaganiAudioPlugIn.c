// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng hal/ShengAudioPlugIn.c (revision 08cde20): names, bundle ID and factory UUID changed; the routing-layer trace
// (VirtualAudio rebinding, below) runs only when the kernel service asks for it (boot argument pagani-audio-trace=1); the service's
// geometry is checked before its rings are used; frames mixed behind what the service has taken are dropped (out_taken); the route
// probe (below, option PaganiAudioOptionProbeRoute) reads what the audio server makes of the device for the boot log. Properties,
// time stamps and IO are otherwise sheng's, which iOS 21H461 accepted on sheng: the device reports the Thunderbolt transport type, so the
// audio server's routing layer makes it a driver device with one output and one input port.
//
// The OnePlus 13T built-in audio device for the iOS audio server: one device with a stereo output stream (the two speakers) and a mono
// input stream (the microphones). Samples move through rings shared with the kernel service PaganiAudio (PaganiAudioAbi.h); the
// service's backend carries them (the silent backend for now: no sound, but a running device, a volume control and working playback).
// Objects: the plug-in (1), the device (2), the output stream (3), the input stream (4), the output volume (5); with the kernel's actuator
// (ABI revision 2, option PaganiAudioOptionActuator: boot arguments pagani-haptics-ios=1 or 2 and pagani-haptics=1) also the haptic actuator
// device (6, UID "Actuator") and its output stream (7), fed through a ring of their own (memory type 3) to the vibrator. The device is
// listed at Initialize whatever becomes of its ring (mapActuator, expectActuator): iOS waits for it once the device tree has a "haptics" node.
// Also new: with pagani-audio-mx=1 the route probe looks at how far MediaExperience's start got, and with pagani-audio-srzl=1 it puts
// MediaExperience's lost serialization listener on the routing layer once; with bit 1 of pagani-audio-srzl it gives VirtualAudio the actuator
// settings its table lacks for the J120 product (the "MediaExperience" section below).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <os/log.h>
#include "PaganiAudioAbi.h"
#include "PaganiAudioMxLayout.h"

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5, kActuator = 6, kActStream = 7};
#define kPlugInBundleID "org.unixnu.pagani.audio"
#define kDeviceUID "PaganiAudioDevice"
#define kModelUID "PaganiAudioModel"
// The UID iOS's haptic server and vibe engine play to (VirtualAudio's DeviceFactory_Aspen makes Device_Actuator_Aspen of it, port 'pact').
#define kActuatorUID "Actuator"
#define kActuatorModelUID "PaganiActuatorModel"
static const Float64 kRate = PAGANI_AUDIO_RATE;
// The time stamp period. It must exceed the safety offset plus an IO cycle, or the audio server never runs IO (i543: 8192, no writes).
static const UInt32 kZeroPeriod = PAGANI_AUDIO_RING_FRAMES;
// The time stamps start at the frame IO started on, as in Apple's sample driver, not on a grid from boot: the audio server took the first
// stamp, up to a period old, for the start, and wrote that much late (least write-ahead -734 frames with a 16384 period, i539; about
// -9700 with 65536, i540 to i542). Sample times stay those of the shared timeline. Playback is smooth this way (i544); i545 went back to
// the grid with a 32768 period and SimpleIIR, and both directions stuttered.
static uint64_t gZeroOrigin, gZeroSeed;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static PaganiAudioControl *gControl;
static int16_t *gOut, *gIn;
static UInt32 gIoUsers;
static Float32 gVolume = 1.0f;   // the volume control's scalar (slider position)
static Float64 gHostTicksPerFrame;
// The haptic actuator (revision 2): whether the device is listed; its header and ring once the service offers them and both checked out
// (gAct stays NULL otherwise: the device is listed and its IO refused); its IO clients and time stamps.
static atomic_int gActListed;
static PaganiAudioActuatorControl *gAct;
static int16_t *gActRing;
static UInt32 gActIoUsers;
static uint64_t gActZeroOrigin, gActZeroSeed;
static int haveActuator(void) {return atomic_load(&gActListed);}
static void mapActuator(PaganiAudioControl *c);
static void expectActuator(void);
static void setHostClock(void);
static void traceRouting(void);
static void vaSettingsAsk(void);
static void vaSettingsGive(const struct mach_header_64 *h);
static void probeStart(void);
static void probeSoon(uint32_t reason);
static void probeCountVolumeRead(void);

// ---- Kernel service ------------------------------------------------------------------------------------------------------------
static int mapType(uint32_t type, void **out) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess) return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(PAGANI_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return -1;
    void *control = NULL, *out = NULL, *in = NULL;
    if (mapType(PaganiAudioMemoryControl, &control) || mapType(PaganiAudioMemoryOutput, &out) || mapType(PaganiAudioMemoryInput, &in)) {
        IOServiceClose(gConnect); gConnect = 0; return -1;
    }
    PaganiAudioControl *c = control;
    // The rings are indexed with this file's constants: a service of another geometry (or no timeline) is not used.
    if (c->magic != PAGANI_AUDIO_MAGIC || c->version != PAGANI_AUDIO_VERSION || c->rate != PAGANI_AUDIO_RATE ||
        c->ring_frames != PAGANI_AUDIO_RING_FRAMES || c->out_channels != PAGANI_AUDIO_OUT_CHANNELS ||
        c->in_channels != PAGANI_AUDIO_IN_CHANNELS || c->period_frames != PAGANI_AUDIO_PERIOD_FRAMES || !c->ticks_per_frame_q32) {
        IOServiceClose(gConnect); gConnect = 0; return -1;
    }
    gOut = out; gIn = in; gControl = c;
    gHostTicksPerFrame = (Float64)c->ticks_per_frame_q32 / 4294967296.0;
    setHostClock();
    if (c->options & PaganiAudioOptionTraceRouting) traceRouting();
    mapActuator(c);
    if ((c->options & PaganiAudioOptionVaSettings) && haveActuator()) vaSettingsAsk();
    return 0;
}

// The actuator: with the service's option the device is listed from here on, whatever becomes of its ring. The option means the kernel
// puts a "haptics" node in the device tree, and for that node VirtualAudio's deferred initialisation waits up to 8 s for the port of a device
// with this UID (VirtualAudio @0x14d630), which can push it past MediaExperience's 9 s wait (route-re 2.4); with no device iOS loses more
// than the vibrations. The ring (memory type 3) needs a revision 2 block with its geometry and a header of this file's geometry on the same
// timeline; anything else leaves the device listed with its IO refused. Without the option: no device, the speaker device as before.
static void mapActuator(PaganiAudioControl *c)
{
    if (!(c->options & PaganiAudioOptionActuator)) return;
    atomic_store(&gActListed, 1);
    const PaganiAudioRevision *r = (const PaganiAudioRevision *)((const uint8_t *)c + PAGANI_AUDIO_REVISION_OFFSET);
    if (r->magic != PAGANI_AUDIO_REVISION_MAGIC || r->revision < 2 || r->actuator_frames != PAGANI_AUDIO_ACT_RING_FRAMES ||
        r->actuator_rate != PAGANI_AUDIO_ACT_RATE) {
        os_log_error(OS_LOG_DEFAULT, "PaganiAudio: actuator offered by a service of another geometry: listed, IO refused");
        return;
    }
    void *memory = NULL;
    if (mapType(PaganiAudioMemoryActuator, &memory)) {
        os_log_error(OS_LOG_DEFAULT, "PaganiAudio: actuator offered but memory type 3 did not map: listed, IO refused");
        return;
    }
    PaganiAudioActuatorControl *a = memory;
    if (a->magic != PAGANI_AUDIO_ACT_MAGIC || a->version != PAGANI_AUDIO_ACT_VERSION || a->rate != PAGANI_AUDIO_ACT_RATE ||
        a->ring_frames != PAGANI_AUDIO_ACT_RING_FRAMES || a->ticks_per_frame_q32 != c->ticks_per_frame_q32 ||
        a->anchor_host != c->out_anchor_host) {
        os_log_error(OS_LOG_DEFAULT, "PaganiAudio: actuator header of another geometry: listed, IO refused");
        return;
    }
    a->loads += 1;
    gActRing = (int16_t *)((uint8_t *)memory + PAGANI_AUDIO_ACT_HEADER_BYTES);
    __atomic_store_n(&gAct, a, __ATOMIC_RELEASE);
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: actuator device offered (UID " kActuatorUID ")");
}

// No service when the audio server initialises the plug-in (its start failed, or the open was refused): the option is out of reach, so the
// plug-in asks what VirtualAudio's deferred initialisation is about to ask, in the same process: MGGetBoolAnswer of DeviceSupportsHaptics
// (obfuscated, VirtualAudio @0x14d630). Only on this failure path; with the service the option decides and nothing is asked.
static void expectActuator(void)
{
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "MGGetBoolAnswer") & (((uintptr_t)1 << 47) - 1);
    if (!symbol || !((Boolean (*)(CFStringRef))symbol)(CFSTR("cBy4BcYs5YWtFHbBpt4C6A"))) return;
    atomic_store(&gActListed, 1);
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: no service but iOS expects the actuator: listed, IO refused");
}
static kern_return_t call(uint32_t selector) {return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;}

// CoreAudio's host clock frequency is a constant of 24 MHz (Apple's timer) that its IO clock and IO thread read directly; this board's timer
// runs at 19.2 MHz, so the audio server's IO cycles came 25 % late (i546), and each recording fell silent from about 1.0 to 1.8 s until its
// clock caught up (i548 to i551; gone with the constant set, i552). AudioGetHostClockFrequency loads it with adrp and ldr d0: the address
// is taken from those two instructions and the service sets the constant (on the shared page it cannot be written from here).
static void setHostClock(void) {
    PaganiAudioDiag *diag = (PaganiAudioDiag *)((uint8_t *)gControl + PAGANI_AUDIO_DIAG_OFFSET);
    // The audio server is arm64e: dlsym signs the pointer, and this arm64 code neither reads nor calls through the signature bits.
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency") & (((uintptr_t)1 << 47) - 1);
    if (!symbol) {diag->host_clock_result = -1; return;}
    Float64 (*frequency)(void) = (Float64 (*)(void))symbol;
    const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
    if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u || ((ldr >> 5) & 31) != (adrp & 31)) {
        diag->host_clock_result = -1;
        return;
    }
    int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
    if (page & (1 << 20)) page -= 1 << 21;
    const uint64_t address = ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
    const kern_return_t r = IOConnectCallScalarMethod(gConnect, PaganiAudioSetHostClock, &address, 1, NULL, NULL);
    diag->host_clock_result = r;
    diag->host_clock = (uint32_t)frequency();
}

// ---- Helpers ---------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return gControl ? gControl->lead_frames : 2 * PAGANI_AUDIO_PERIOD_FRAMES;}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

// Connects at once, so the service sees the audio server load the plug-in; a failure here is retried at the first StartIO.
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    const int connected = connectService();
    pthread_mutex_unlock(&gLock);
    if (connected) expectActuator();
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: loaded, service %{public}s", connected ? "not connected" : "connected");
    if (gControl) {
        PaganiAudioDiag *diag = (PaganiAudioDiag *)((uint8_t *)gControl + PAGANI_AUDIO_DIAG_OFFSET);
        diag->loads += 1;
        diag->volume_milli = (UInt32)(gVolume * 1000.0f);
    }
    probeStart();
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static void trace(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 kind, OSStatus r) {
    if (!gControl) return;
    PaganiAudioTrace *tr = (PaganiAudioTrace *)((uint8_t *)gControl + PAGANI_AUDIO_TRACE_OFFSET);
    const UInt32 info = (UInt32)o << 24 | (a->mScope >> 24) << 16 | kind | (r ? 2u : 0u);
    const UInt32 n = tr->count < PAGANI_AUDIO_TRACE_SLOTS ? tr->count : PAGANI_AUDIO_TRACE_SLOTS;
    for (UInt32 i = 0; i < n; ++i) if (tr->entry[i][0] == a->mSelector && tr->entry[i][1] == info) return;
    if (n < PAGANI_AUDIO_TRACE_SLOTS) {tr->entry[n][0] = a->mSelector; tr->entry[n][1] = info; tr->count = n + 1;}
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    *out = (o == kVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           ((o == kOutStream || o == kInStream || o == kActStream) &&
            (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
             a->mSelector == kAudioStreamPropertyIsActive)) ||
           ((o == kDevice || o == kActuator) && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

// The output volume spans kMinDb..0 dB on a squared curve over the scalar (loud in the middle, fine steps near the bottom); scalar 0
// mutes. The audio server maps its user volume onto this range: a -96 dB floor made a third of full volume -56 dB (i532), a linear
// -40 dB range still -26 dB (i533, quiet); now it is -16 dB.
static const Float32 kMinDb = -36.0f;
static Float32 toDb(Float32 scalar) {const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar); return kMinDb * r * r;}
static Float32 toScalar(Float32 db) {return db <= kMinDb ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / kMinDb);}
static Float32 gain(void) {return gVolume <= 0.0f ? 0.0f : powf(10.0f, toDb(gVolume) / 20.0f);}

// The actuator's properties as the routing layer and the haptic server set them: its one format (48 kHz mono float) and rate, the
// stream's activity; anything else is accepted and recorded (the header's unknown_sets and last selector, the trace, the unified log),
// so a write the plug-in does not know cannot make VirtualAudio drop the device. Without a ring the same answers, nothing recorded.
static OSStatus setActuatorProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 size, const void *data) {
    if (!haveActuator()) return kAudioHardwareBadObjectError;
    PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
    const AudioObjectPropertySelector s = a->mSelector;
    if (o == kActStream && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        if (size < sizeof *f || f->mSampleRate != kRate || f->mChannelsPerFrame != 1 || f->mFormatID != kAudioFormatLinearPCM ||
            !(f->mFormatFlags & kAudioFormatFlagIsFloat) || f->mBitsPerChannel != 32) return kAudioDeviceUnsupportedFormatError;
        if (act) act->format_sets += 1;
        return 0;
    }
    if (o == kActuator && s == kAudioDevicePropertyNominalSampleRate) {
        if (size < sizeof(Float64) || *(const Float64 *)data != kRate) return kAudioDeviceUnsupportedFormatError;
        if (act) act->format_sets += 1;
        return 0;
    }
    if (o == kActStream && s == kAudioStreamPropertyIsActive) return size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    if (act) {
        act->unknown_sets += 1;
        act->unknown_selector = s;
        act->unknown_where = (UInt32)o << 24 | (a->mScope >> 24) << 16 | (size & 0xffffu);
    }
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: actuator write accepted: object %u selector '%c%c%c%c' scope '%c%c%c%c' size %u", o,
                 (char)(s >> 24), (char)(s >> 16), (char)(s >> 8), (char)s, (char)(a->mScope >> 24), (char)(a->mScope >> 16),
                 (char)(a->mScope >> 8), (char)a->mScope, size);
    return 0;
}

static OSStatus setProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 size, const void *data) {
    if (o == kActuator || o == kActStream) return setActuatorProperty(o, a, size, data);
    if (o == kVolume && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = toScalar(v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue) return kAudioHardwareUnknownPropertyError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        if (gControl) {
            PaganiAudioDiag *diag = (PaganiAudioDiag *)((uint8_t *)gControl + PAGANI_AUDIO_DIAG_OFFSET);
            diag->volume_milli = (UInt32)(gVolume * 1000.0f);
            diag->volume_sets += 1;
        }
        AudioObjectPropertyAddress changed[2] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                 {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        probeSoon(PaganiAudioProbeVolume);
        return 0;
    }
    if ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        return size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == (o == kOutStream ? 2u : 1u) ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    // The audio server turns streams it does not use off and on (i530); both streams stay running, so this only acknowledges it.
    if ((o == kOutStream || o == kInStream) && a->mSelector == kAudioStreamPropertyIsActive) return size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    const OSStatus r = setProperty(o, a, size, data);
    trace(o, a, 1, r);
    return r;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    UInt8 scratch[1024];
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn) {
        list[n++] = kDevice;
        if (haveActuator()) list[n++] = kActuator;
    } else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kInStream;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
    } else if (o == kActuator && haveActuator()) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kActStream;   // output only: no input stream, no data source
    }
    return n;
}
// The objects that exist now: the actuator's two only while the service offers it.
static int objectExists(AudioObjectID o) {return (o >= kPlugIn && o <= kVolume) || ((o == kActuator || o == kActStream) && haveActuator());}
static UInt32 copyList(const AudioObjectID *list, UInt32 n, UInt32 inDataSize, void *outData) {
    UInt32 out = 0;
    for (UInt32 i = 0; i < n && (out + 1) * sizeof(AudioObjectID) <= inDataSize; ++i) ((AudioObjectID *)outData)[out++] = list[i];
    return out * (UInt32)sizeof(AudioObjectID);
}

// The haptic actuator: what iOS's haptic server and vibe engine look for (route-re 2.4, haptics RE): a device whose UID is exactly
// "Actuator", output only (no input stream, so VirtualAudio's Device_Actuator_Aspen never waits on a data source 'ssrc'), 48 kHz mono
// Float32 packed (flags 9), never a default device, no volume or mute control (Device_Actuator_Aspen would set them to 0 dB / unmuted),
// built in, a safety offset that keeps the audio server's writes within 1.5 IO cycles of the play position.
static AudioStreamBasicDescription actuatorFormat(void) {return format(1);}
static OSStatus actuatorData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    if (o == kActuator) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Actuator");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kActuatorUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kActuatorModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Actuator");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kActuator);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gActIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 0);
        case kAudioDevicePropertyLatency: REPLY(UInt32, 0);
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, PAGANI_AUDIO_ACT_SAFETY_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, PAGANI_AUDIO_ACT_RING_FRAMES);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 1;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = 1;
            l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID list[1];
            *outDataSize = copyList(list, objects(kActuator, a->mScope, list), inDataSize, outData);
            return 0;
        }
        case kAudioObjectPropertyControlList: *outDataSize = 0; return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    switch (s) {   // kActStream
    case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kActuator);
    case kAudioObjectPropertyName: REPLY_STR("Actuator");
    case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
    case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
    case kAudioStreamPropertyDirection: REPLY(UInt32, 0);
    case kAudioStreamPropertyTerminalType: REPLY(UInt32, kAudioStreamTerminalTypeSpeaker);
    case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
    case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, actuatorFormat());
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r = {actuatorFormat(), {kRate, kRate}};
        REPLY(AudioStreamRangedDescription, r);
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    // Lists every object answers, empty here: custom properties ('cust'), and on the plug-in its boxes and clock devices. The audio
    // server's device manager reads them for each new device and drops a device whose answer is an unknown-property error (i525).
    if (objectExists(o) && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) ||
        ((s == 'taps') && (o == kOutStream || o == kInStream || o == kActStream)))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID list[2];
            *outDataSize = copyList(list, objects(kPlugIn, a->mScope, list), inDataSize, outData);
            return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            const int ours = uid && CFGetTypeID(uid) == CFStringGetTypeID() && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo;
            const int actuator = uid && CFGetTypeID(uid) == CFStringGetTypeID() && haveActuator() &&
                                 CFStringCompare(uid, CFSTR(kActuatorUID), 0) == kCFCompareEqualTo;
            REPLY(AudioObjectID, ours ? kDevice : actuator ? kActuator : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        // Private device properties the audio server reads for each new device (i526: 'accs' and 'auto' were its last unanswered ones).
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        // Thunderbolt: the audio server's routing layer makes a device of this transport a driver device with one output and one input
        // port of its own. Built-in devices are only used through Apple's port descriptions, and a USB device must carry a USB location
        // ('lcID') that the HAL does not take from a plug-in (i529).
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeThunderbolt);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        // The service takes a period lead_frames before it plays (its own queue and the DSP's); the service measures and publishes it, so
        // the audio server writes each frame at least that early.
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * PAGANI_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        // Raw: the zero time stamps are exact (one host clock), so smoothing only delays the audio server's view of them.
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            l->mChannelDescriptions[0].mChannelLabel = channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[3];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && list[i] == kVolume) continue;
                if (s == kAudioObjectPropertyControlList && list[i] != kVolume) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream: {
        const UInt32 channels = o == kOutStream ? 2 : 1;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Speaker"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o == kInStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kActuator:
    case kActStream:
        return haveActuator() ? actuatorData(o, a, inDataSize, outDataSize, outData) : kAudioHardwareBadObjectError;
    default:
        return kAudioHardwareBadObjectError;
    }
}

// Every property the audio server asks for and the plug-in does not answer is logged once (object, selector, scope), so a client that
// drops the device after an unknown-property error names what it wanted.
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d;
    const OSStatus r = propertyData(o, a, qs, q, inDataSize, outDataSize, outData);
    trace(o, a, 0, r);
    if (o == kVolume) probeCountVolumeRead();
    if (r == kAudioHardwareUnknownPropertyError) {
        static UInt64 seen[128];
        static atomic_uint count;
        const UInt64 key = (UInt64)o << 56 ^ (UInt64)a->mSelector << 24 ^ a->mScope;
        unsigned n = atomic_load(&count);
        for (unsigned i = 0; i < n && i < 128; ++i) if (seen[i] == key) return r;
        n = atomic_fetch_add(&count, 1);
        if (n < 128) seen[n] = key;
        const UInt32 v = a->mSelector, sc = a->mScope;
        if (gControl && n < PAGANI_AUDIO_DIAG_SLOTS) {
            PaganiAudioDiag *diag = (PaganiAudioDiag *)((uint8_t *)gControl + PAGANI_AUDIO_DIAG_OFFSET);
            diag->selector[n] = v;
            diag->where[n] = (UInt32)o << 24 | (sc >> 24);
            diag->count = n + 1;
        }
        os_log_error(OS_LOG_DEFAULT, "PaganiAudio: unknown property object %u selector '%c%c%c%c' scope '%c%c%c%c' element %u pid %d", o,
               (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
               a->mElement, pid);
    }
    return r;
}

// ---- The audio server's view of the device ---------------------------------------------------------------------------------------
// The routing layer of the audio server (VirtualAudio.plugin, in the same process) drops the device after a 'who?' error that never
// reaches this plug-in (i528). Its HAL calls and C++ throws are rebound here through its symbol pointers: every failing call (object,
// selector, scope, status) and every throw (the thrower's offset in the image, the exception's first word) is logged once and added to the
// trace with kind 4 (call) or 8 (throw, the offset in place of the selector).
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
static OSStatus (*gRealGet)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
static OSStatus (*gRealGetSize)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static Boolean (*gRealHas)(AudioObjectID, const AudioObjectPropertyAddress *);
static void (*gRealThrow)(void *, void *, void (*)(void *));
static uintptr_t gVaBase;

static void noteCall(AudioObjectID o, const AudioObjectPropertyAddress *a, OSStatus r, const char *what) {
    const AudioObjectPropertyAddress t = {a->mSelector, a->mScope, 0};
    if (!gControl) return;
    PaganiAudioTrace *tr = (PaganiAudioTrace *)((uint8_t *)gControl + PAGANI_AUDIO_TRACE_OFFSET);
    const UInt32 before = tr->count;
    trace(o & 0xff, &t, 4, r ? r : 1);
    if (tr->count == before) return;
    const UInt32 v = a->mSelector, sc = a->mScope;
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: va %{public}s object %u '%c%c%c%c' scope '%c%c%c%c' element %u status %d", what, o,
                 (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
                 a->mElement, (int)r);
}
static OSStatus hookGet(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *data) {
    const OSStatus r = gRealGet(o, a, qs, q, size, data);
    if (r && o) noteCall(o, a, r, "get");
    return r;
}
static OSStatus hookGetSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    const OSStatus r = gRealGetSize(o, a, qs, q, size);
    if (r && o) noteCall(o, a, r, "size");
    return r;
}
static Boolean hookHas(AudioObjectID o, const AudioObjectPropertyAddress *a) {
    const Boolean r = gRealHas(o, a);
    if (!r && o) noteCall(o, a, 0, "has");
    return r;
}
static void hookThrow(void *thrown, void *type, void (*destroy)(void *)) {
    const uintptr_t at = (uintptr_t)__builtin_return_address(0) - gVaBase;
    const UInt32 first = thrown ? *(const UInt32 *)thrown : 0;
    const char *name = type ? ((const char *const *)type)[1] : "";
    if (gControl) {
        const AudioObjectPropertyAddress t = {(UInt32)at, kAudioObjectPropertyScopeGlobal, 0};
        trace(0, &t, 8, (OSStatus)(first | 1));
    }
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: va throw at 0x%lx type %{public}s first 0x%08x", (unsigned long)at, name, first);
    __attribute__((musttail)) return gRealThrow(thrown, type, destroy);
}

// Rebinds the named imports of one image: its non-lazy and lazy symbol pointer sections, through the indirect symbol table.
static void rebind(const struct mach_header_64 *h, intptr_t slide) {
    const struct load_command *lc = (const void *)(h + 1);
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysymtab = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysymtab) return;
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const void *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *indirect = (const void *)(base + dysymtab->indirectsymoff);
    static const char *const names[] = {"_AudioObjectGetPropertyData", "_AudioObjectGetPropertyDataSize", "_AudioObjectHasProperty", "___cxa_throw"};
    void *const hooks[] = {(void *)hookGet, (void *)hookGetSize, (void *)hookHas, (void *)hookThrow};
    void **const reals[] = {(void **)&gRealGet, (void **)&gRealGetSize, (void **)&gRealHas, (void **)&gRealThrow};
    lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        const struct section_64 *sect = (const void *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; ++s, ++sect) {
            const uint32_t type = sect->flags & SECTION_TYPE;
            if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)((uintptr_t)slide + sect->addr);
            for (uint64_t k = 0; k < sect->size / sizeof(void *); ++k) {
                const uint32_t index = indirect[sect->reserved1 + k];
                if (index & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
                const char *name = strs + syms[index].n_un.n_strx;
                for (unsigned n = 0; n < 4; ++n) {
                    if (strcmp(name, names[n]) || slots[k] == hooks[n]) continue;
                    const uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(getpagesize() - 1);
                    if (mprotect((void *)page, (size_t)getpagesize(), PROT_READ | PROT_WRITE) &&
                        vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
                        os_log_error(OS_LOG_DEFAULT, "PaganiAudio: va rebind %{public}s failed", name);
                        continue;
                    }
                    if (!*reals[n]) *reals[n] = slots[k];
                    slots[k] = hooks[n];
                    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: va rebind %{public}s", name);
                }
            }
        }
    }
}
// The routing layer's image is remembered when it loads; its imports are rebound only once the service asked for the trace (control
// page option PaganiAudioOptionTraceRouting, boot argument pagani-audio-trace=1), whichever of the two comes first.
static atomic_int gTraceRouting, gVaSettingsAsked;
static const struct mach_header_64 *gVaHeader;
static intptr_t gVaSlide;
void pagani_audio_va_image(const struct mach_header_64 *h, intptr_t slide) {
    gVaBase = (uintptr_t)h;
    gVaSlide = slide;
    __atomic_store_n(&gVaHeader, h, __ATOMIC_SEQ_CST);
    if (atomic_load(&gTraceRouting)) rebind(h, slide);
    if (atomic_load(&gVaSettingsAsked)) vaSettingsGive(h);
}
static void imageAdded(const struct mach_header *h, intptr_t slide) {
    Dl_info info;
    if (h->magic != MH_MAGIC_64 || !dladdr(h, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio.plugin/")) return;
    pagani_audio_va_image((const struct mach_header_64 *)h, slide);
}
static void traceRouting(void) {
    if (atomic_exchange(&gTraceRouting, 1)) return;
    const struct mach_header_64 *h = __atomic_load_n(&gVaHeader, __ATOMIC_ACQUIRE);
    if (h) rebind(h, gVaSlide);
}
__attribute__((constructor)) static void watchAudioServer(void) {_dyld_register_func_for_add_image(imageAdded);}

// ---- Route probe ---------------------------------------------------------------------------------------------------------------------
// What decides the volume HUD (PaganiAudioAbi.h, PaganiAudioProbe), read only through the HAL and written to the control page for the
// kernel's boot log: the device as the HAL lists it, whether the HAL finds {'vold', output, main} on it (the routing layer gives the
// Thunderbolt port a volume control only then: Device_Thunderbolt_Aspen.cpp; the HAL translates the address to the volume control here,
// HALB_Info::TranslateDeviceAddressToControlAddress), the default virtual device's 'vcsp' (MediaExperience's CurrentRouteHasVolumeControl,
// vaemCurrentRouteHasVolumeControl, which SpringBoard requires before it changes the volume), the routing layer's category, mode, active
// output ports and their types, and the default virtual device's volume. The calls run on a serial queue of their own, a while after the
// plug-in loads and after volume changes and IO starts (a few times each), never under gLock: the HAL's answer to 'vold' comes back into
// this plug-in. The HAL functions are looked up at run time, as AudioGetHostClockFrequency is: the iOS SDK does not export them.
typedef OSStatus (*HalGetFn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
typedef Boolean (*HalHasFn)(AudioObjectID, const AudioObjectPropertyAddress *);
static HalGetFn gHalGet;
static HalHasFn gHalHas;
static dispatch_queue_t gProbeQueue;
static atomic_uint gProbeBudget[4];
static atomic_int gProbePending[4];
static const unsigned kProbeAfterLoad[] = {20, 60, 150, 400};   // seconds
static void probeWork(void *context);
static dispatch_time_t probeTime(unsigned seconds);
enum {kProbeEventBudget = 8, kProbeEventDelay = 2};
// Selectors of AudioHardware.h (not in the plug-in headers): the system object's device list ('dev#'), UID to device ('uidd' with the
// UID as qualifier; 'duid' with an AudioValueTranslation), bundle ID to plug-in ('pibi'); a device running in any process ('gone'); the
// process hogging a device ('oink', -1 none).
enum {kSystemObject = 1, kSelDevices = 'dev#', kSelUIDToDevice = 'uidd', kSelDeviceForUID = 'duid', kSelPlugInForBundleID = 'pibi',
      kSelRunningSomewhere = 'gone', kSelHogMode = 'oink'};

static PaganiAudioProbe *probePage(void) {return gControl ? (PaganiAudioProbe *)((uint8_t *)gControl + PAGANI_AUDIO_PROBE_OFFSET) : NULL;}
static void probeCountVolumeRead(void) {
    PaganiAudioProbe *p = probePage();
    if (p) __atomic_fetch_add(&p->vol_queries, 1u, __ATOMIC_RELAXED);
}

static int halResolve(void) {
    // Masked as setHostClock does (a signed pointer would be called through its signature bits otherwise).
    const uintptr_t mask = ((uintptr_t)1 << 47) - 1;
    if (!gHalGet) gHalGet = (HalGetFn)((uintptr_t)dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData") & mask);
    if (!gHalHas) gHalHas = (HalHasFn)((uintptr_t)dlsym(RTLD_DEFAULT, "AudioObjectHasProperty") & mask);
    return gHalGet && gHalHas;
}
static OSStatus halGet(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope, UInt32 qs, const void *q,
                       UInt32 *size, void *data) {
    const AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    return gHalGet(o, &a, qs, q, size, data);
}
static OSStatus halGet32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope, UInt32 *value) {
    UInt32 size = sizeof *value;
    const OSStatus r = halGet(o, s, scope, 0, NULL, &size, value);
    return r ? r : size == sizeof *value ? 0 : kAudioHardwareBadPropertySizeError;
}
// An object by key: the qualifier form ('uidd', the UID as qualifier) first if given, then the AudioValueTranslation form ('duid',
// 'pibi'). kAudioObjectUnknown when neither answers.
static AudioObjectID halTranslate(AudioObjectPropertySelector qualified, AudioObjectPropertySelector translation, CFStringRef key) {
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    if (qualified && !halGet(kSystemObject, qualified, kAudioObjectPropertyScopeGlobal, sizeof key, &key, &size, &id) &&
        size == sizeof id && id != kAudioObjectUnknown) return id;
    id = kAudioObjectUnknown;
    AudioValueTranslation t = {&key, sizeof key, &id, sizeof id};
    size = sizeof t;
    return halGet(kSystemObject, translation, kAudioObjectPropertyScopeGlobal, 0, NULL, &size, &t) ? kAudioObjectUnknown : id;
}

// Why the default virtual device has no active output port, when it has none (VirtualAudio.plugin of 21H461, read in IDA): the routing
// layer's device manager takes only the six UIDs Null_Device, Actuator, Halogen, Hawking, Flicker and Penrose until MediaExperience
// installs its serialization listener ('srzl' set on the routing layer's plug-in object by FigVAEndpointManagerCreate: "SerializationContext
// is ready. Allowing all devices"), and refuses every route change before that ("Attempt to change routes prior to establishing a valid
// serialization context"). A device it takes (DeviceFactory_Aspen: transport 'thun' makes Device_Thunderbolt_Aspen) is hogged by the audio
// server first (Device_HAL_Common takes hog mode, 'oink' = its pid) and gets a Thunderbolt output port ('ptbo'), connected at once
// (PortManager's connected ports, 'prts'). So: no listener -> the device was never taken; listener but not hogged -> taking it failed;
// hogged and 'ptbo' connected but not active -> the routing itself left it out. Read only, like the rest of the probe; written in place of
// the (absent) active port types: t0 the first connected output port's type, t1 = [S listener | s none | ? unread] [H hogged by this
// process | h free | x another process | ? unread] [connected output ports 0-9, + more, ? unread] [T one of them is 'ptbo' | t none].
static int probeSerialization(AudioObjectID va) {
    uint64_t listener[2] = {0, 0};
    UInt32 size = sizeof listener;
    if (!va || halGet(va, 'srzl', kAudioObjectPropertyScopeGlobal, 0, NULL, &size, listener) || size != sizeof listener) return -1;
    return listener[0] || listener[1];
}
static int probeHog(AudioObjectID ours) {
    pid_t owner = 0;
    UInt32 size = sizeof owner;
    if (!ours || halGet(ours, kSelHogMode, kAudioObjectPropertyScopeGlobal, 0, NULL, &size, &owner) || size != sizeof owner)
        return -2;
    return owner == getpid() ? 1 : owner == -1 ? 0 : -1;
}
static OSStatus probeConnected(AudioObjectID va, uint32_t *count, uint32_t *first, int *thunderbolt) {
    CFTypeRef list = NULL;
    UInt32 size = sizeof list;
    if (!va) return kAudioHardwareBadObjectError;
    OSStatus r = halGet(va, 'prts', kAudioObjectPropertyScopeOutput, 0, NULL, &size, &list);
    if (r || size != sizeof list || !list) return r ? r : kAudioHardwareBadPropertySizeError;
    if (CFGetTypeID(list) == CFArrayGetTypeID()) {
        const CFIndex n = CFArrayGetCount(list);
        *count = (uint32_t)n;
        for (CFIndex k = 0; k < n && k < 16; ++k) {
            CFNumberRef number = CFArrayGetValueAtIndex(list, k);
            SInt32 port = 0;
            UInt32 type = 0;
            if (!number || CFGetTypeID(number) != CFNumberGetTypeID() || !CFNumberGetValue(number, kCFNumberSInt32Type, &port) || !port ||
                halGet32((AudioObjectID)port, 'ptyp', kAudioObjectPropertyScopeGlobal, &type)) continue;
            if (!*first) *first = type;
            if (type == 'ptbo') *thunderbolt = 1;
        }
    } else {
        r = kAudioHardwareUnspecifiedError;
    }
    CFRelease(list);
    return r;
}
__attribute__((visibility("hidden"))) uint32_t pagani_audio_probe_why(int serial, int hog, OSStatus connStatus, uint32_t conn, int thunderbolt);
uint32_t pagani_audio_probe_why(int serial, int hog, OSStatus connStatus, uint32_t conn, int thunderbolt) {
    const uint32_t c0 = serial > 0 ? 'S' : serial == 0 ? 's' : '?';
    const uint32_t c1 = hog == 1 ? 'H' : hog == 0 ? 'h' : hog == -1 ? 'x' : '?';
    const uint32_t c2 = connStatus ? '?' : conn > 9 ? '+' : '0' + conn;
    const uint32_t c3 = thunderbolt ? 'T' : 't';
    return c0 << 24 | c1 << 16 | c2 << 8 | c3;
}

// The default virtual device's active output ports ('aprc' of the routing layer's plug-in object: an array of {"device UID", "output
// ports"} dictionaries, as vaemGetPortListForDevice reads it): how many, and the type of the first two.
static OSStatus probePorts(AudioObjectID va, uint32_t *count, uint32_t types[2]) {
    CFTypeRef list = NULL;
    UInt32 size = sizeof list;
    OSStatus r = halGet(va, 'aprc', kAudioObjectPropertyScopeOutput, 0, NULL, &size, &list);
    if (r || size != sizeof list || !list) return r ? r : kAudioHardwareBadPropertySizeError;
    if (CFGetTypeID(list) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
            CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
            if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
            CFStringRef uid = CFDictionaryGetValue(d, CFSTR("device UID"));
            CFArrayRef ports = CFDictionaryGetValue(d, CFSTR("output ports"));
            if (!uid || CFGetTypeID(uid) != CFStringGetTypeID() || CFStringCompare(uid, CFSTR("VirtualAudioDevice_Default"), 0) ||
                !ports || CFGetTypeID(ports) != CFArrayGetTypeID()) continue;
            *count = (uint32_t)CFArrayGetCount(ports);
            for (CFIndex k = 0; k < CFArrayGetCount(ports) && k < 2; ++k) {
                CFNumberRef n = CFArrayGetValueAtIndex(ports, k);
                SInt32 port = 0;
                UInt32 type = 0;
                if (n && CFGetTypeID(n) == CFNumberGetTypeID() && CFNumberGetValue(n, kCFNumberSInt32Type, &port) && port &&
                    !halGet32((AudioObjectID)port, 'ptyp', kAudioObjectPropertyScopeGlobal, &type)) types[k] = type;
            }
            break;
        }
    } else {
        r = kAudioHardwareUnspecifiedError;
    }
    CFRelease(list);
    return r;
}

__attribute__((visibility("hidden"))) void pagani_audio_probe_route(uint32_t reason);   // the host test calls it directly
void pagani_audio_probe_route(uint32_t reason) {
    PaganiAudioProbe *p = probePage();
    if (!p || !(gControl->options & PaganiAudioOptionProbeRoute) || !halResolve()) return;
    AudioObjectID ids[64];
    UInt32 size = sizeof ids;
    const uint32_t devices = halGet(kSystemObject, kSelDevices, kAudioObjectPropertyScopeGlobal, 0, NULL, &size, ids) ? 0
                             : size / (UInt32)sizeof(AudioObjectID);
    const AudioObjectID ours = halTranslate(kSelUIDToDevice, kSelDeviceForUID, CFSTR(kDeviceUID));
    const AudioObjectID vad = halTranslate(kSelUIDToDevice, kSelDeviceForUID, CFSTR("VirtualAudioDevice_Default"));
    const AudioObjectID va = halTranslate(0, kSelPlugInForBundleID, CFSTR("com.apple.audio.CoreAudio.VirtualAudio"));
    int32_t vold = -1, running = -1;
    UInt32 value = 0;
    if (ours) {
        const AudioObjectPropertyAddress a = {'vold', kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
        vold = gHalHas(ours, &a) ? 1 : 0;
        if (!halGet32(ours, kSelRunningSomewhere, kAudioObjectPropertyScopeGlobal, &value)) running = value != 0;
    }
    UInt32 vcsp = 0, category = 0, mode = 0, volume = 0xffffffffu;
    const OSStatus vcspStatus = vad ? halGet32(vad, 'vcsp', kAudioObjectPropertyScopeOutput, &vcsp) : kAudioHardwareBadObjectError;
    Float32 scalar = 0;
    size = sizeof scalar;
    if (vad && !halGet(vad, 'volm', kAudioObjectPropertyScopeOutput, 0, NULL, &size, &scalar) && size == sizeof scalar &&
        scalar >= 0 && scalar <= 1) volume = (UInt32)(scalar * 1000.0f + 0.5f);
    uint32_t outPorts = 0, types[2] = {0, 0};
    OSStatus portsStatus = kAudioHardwareBadObjectError;
    if (va) {
        if (halGet32(va, 'rctx', kAudioObjectPropertyScopeGlobal, &category)) category = 0;
        if (halGet32(va, 'inpm', kAudioObjectPropertyScopeGlobal, &mode)) mode = 0;
        portsStatus = probePorts(va, &outPorts, types);
    }
    if (!outPorts) {
        // No active output port: t0 and t1 tell why (see probeSerialization above).
        uint32_t connected = 0, first = 0;
        int thunderbolt = 0;
        const OSStatus connStatus = probeConnected(va, &connected, &first, &thunderbolt);
        types[0] = connStatus ? 0 : first;
        types[1] = pagani_audio_probe_why(probeSerialization(va), probeHog(ours), connStatus, connected, thunderbolt);
    }
    // A seqlock, odd while the fields change, so the kernel never prints a half-written probe (its reader: routeRow in PaganiAudio.cpp).
    // The release fence (dmb ish) makes the odd seq visible before any field; the release store of the even seq, after all of them.
    const uint32_t seq = __atomic_load_n(&p->seq, __ATOMIC_RELAXED) | 1u;
    __atomic_store_n(&p->seq, seq, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->reason = reason;
    p->devices = devices;
    p->va_plugin = va;
    p->vad = vad;
    p->ours = ours;
    p->ours_vold = vold;
    p->ours_running = running;
    p->vcsp_status = vcspStatus;
    p->vcsp = vcsp;
    p->category = category;
    p->mode = mode;
    p->ports_status = portsStatus;
    p->out_ports = outPorts;
    p->port_type[0] = types[0];
    p->port_type[1] = types[1];
    p->vad_volume_milli = volume;
    __atomic_store_n(&p->seq, seq + 1, __ATOMIC_RELEASE);
}
// ---- MediaExperience (route-re 6: R1 progress, R2 the 'srzl' repair) -------------------------------------------------------------------
// MediaExperience sets the routing layer's serialization listener ('srzl') exactly once, at the end of FigVAEndpointManagerCreate, does
// not look at the result and never tries again; VirtualAudio refuses every property write until its deferred initialisation is done
// (VirtualAudio @0x1e210, "VirtualAudio PlugIn is not initialized yet" @0x232ac). A write that lands before that is lost for good: the
// device filter keeps the six special UIDs, this device is never taken, every route change is refused (the 13T of 2026-10-31). The
// progress word says how far MediaExperience got (its zero-initialised globals, in the order its start sets them); the repair, only with
// the kernel's option (pagani-audio-srzl=1), once per boot, only when MediaExperience did make its attempt (C: 'srzl' returned; B: the
// routing layer's plug-in id it uses later is cached), VirtualAudio is initialised ('rctx' reads) and the listener is still empty, sets
// MediaExperience's own two listeners, as its own call would have: vaemVADSerializationListener on 'srzl' and
// vaemReceiverWillBeUsedListener on 'rwbu'. The addresses are 21H461's (dyld cache, arm64, no pointer authentication; one slide for the
// whole cache) and are used only after the code at MXInitialize, and before a repair the code of both listeners, matched byte for byte
// (their ADRP/BL are PC-relative, so the slide leaves the bytes alone). VirtualAudio's device filter is read only when the loaded image's
// UUID is 21H461's. A test points pagani_audio_mx_layout at a layout of its own.
static const PaganiAudioMxLayout kMx21H461 = {
    .initialize = 0x19886ad24ull,
    .initialize_code = {0xe8, 0xa6, 0x2a, 0xf0, 0x08, 0xb9, 0x47, 0xf9, 0x1f, 0x05, 0x00, 0xb1, 0x41, 0x00, 0x00, 0x54},
    .once = 0x1edd49f70ull, .session = 0x1edd499e8ull, .screen = 0x1edd49ff8ull, .ids = 0x1edd49750ull, .routes = 0x1edd49a90ull,
    .context = 0x1edd49b60ull, .endpoint = 0x1edd49a40ull, .va_plugin = 0x1edd49a7cull, .serial_done = 0x1edd49a4cull,
    .queue = 0x1f4a7f678ull,
    .serial_listener = 0x1987aed74ull,
    .serial_code = {0xff, 0x43, 0x01, 0xd1, 0xf4, 0x4f, 0x03, 0xa9, 0xfd, 0x7b, 0x04, 0xa9, 0xfd, 0x03, 0x01, 0x91,
                    0xf3, 0x03, 0x00, 0xaa, 0x29, 0xf8, 0xfe, 0x97, 0xe5, 0x03, 0x00, 0xaa, 0xe8, 0x73, 0x29, 0xb0},
    .receiver_listener = 0x1987ff4e0ull,
    .receiver_code = {0xe0, 0x06, 0x00, 0xb0, 0x00, 0x04, 0x01, 0x91, 0xdd, 0xa3, 0x24, 0x14},
    .va_uuid = {0x23, 0xc2, 0xd0, 0x62, 0x74, 0xb1, 0x3e, 0x5b, 0x81, 0xab, 0x64, 0x5d, 0x16, 0x58, 0xd5, 0x73},
    .va_device_manager = 0x5ced90ull, .va_listener = 0x58, .va_filter = 0x88,
    .va_settings_guard = 0x5d2c78ull, .va_settings_kind = 0x5d2bd8ull, .va_settings_format = 0x5d2c28ull, .va_product = 0x80ac7ae0ull,
};
const PaganiAudioMxLayout *pagani_audio_mx_layout = &kMx21H461;

static PaganiAudioMx *mxPage(void) {return gControl ? (PaganiAudioMx *)((uint8_t *)gControl + PAGANI_AUDIO_MX_OFFSET) : NULL;}
static uint64_t peek64(uintptr_t at) {return *(const volatile uint64_t *)at;}
static uint32_t peek32(uintptr_t at) {return *(const volatile uint32_t *)at;}

// MediaExperience's slide, or 0 with the reason in *progress (nothing else is read then).
static uintptr_t mxSlide(const PaganiAudioMxLayout *L, uint32_t *progress) {
    const uintptr_t at = (uintptr_t)dlsym(RTLD_DEFAULT, "MXInitialize") & (((uintptr_t)1 << 47) - 1);
    if (!at) {*progress = PaganiAudioMxNoSymbol | PaganiAudioMxUnverified; return 0;}
    if (memcmp((const void *)at, L->initialize_code, sizeof L->initialize_code)) {*progress = PaganiAudioMxUnverified; return 0;}
    *progress = 0;
    return at - (uintptr_t)L->initialize;
}

// The routing layer's device filter mode (2: the six special UIDs, 3: every device), 0 when VirtualAudio is not loaded, is another build
// or has no device manager yet.
static int vaImageMatches(const struct mach_header_64 *h, const uint8_t uuid[16]) {
    const struct load_command *lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize))
        if (lc->cmd == LC_UUID) return !memcmp(((const struct uuid_command *)lc)->uuid, uuid, 16);
    return 0;
}
static uint32_t vaFilterMode(const PaganiAudioMxLayout *L) {
    const struct mach_header_64 *h = __atomic_load_n(&gVaHeader, __ATOMIC_ACQUIRE);
    if (!h || !vaImageMatches(h, L->va_uuid)) return 0;
    const uint64_t manager = peek64((uintptr_t)h + (uintptr_t)L->va_device_manager);
    const uint64_t listener = manager ? peek64((uintptr_t)manager + L->va_listener) : 0;
    return listener ? peek32((uintptr_t)listener + L->va_filter) & 3u : 0;
}

// VirtualAudio's actuator settings. With a "haptics" node in the device tree the routing database's builder adds the actuator's virtual
// device (sub_306FE0: alternate VAD type 12) and reads ActuatorSettingsFactory_Aspen's static for it (sub_308DDC case 12, sub_2AC470); the
// device itself reads it as well (Device_Actuator_Aspen, sub_1489EC). Its product table has no row for the J120 product (64): sub_2AC4DC
// throws std::logic_error("Invalid Product Type"), inside the RoutingManager's constructor, so the deferred initialisation
// (sub_14CC58, an ExceptionBarrier around all of it, line 1682) ends there: the plug-in object stays at +108 == 1, no virtual device is ever
// published, the routing layer answers no property and MediaExperience's route reads fail (the 13T since pagani-haptics-ios=2: "dev=0 vad=0
// cat=.... ?h?t", vcsp status '!obj', and vaemCurrentRouteHasVolumeControl counts that failure as a volume control, so the HUD shows
// with nothing behind it). The table's row for products without I/V sense sets the kind word to 1 and the format word and leaves the rest
// of the static zero: the plug-in writes those two and marks the static's guard done, before the deferred initialisation can start (the
// HAL starts it when every plug-in is initialised; either the image is known at Initialize or it arrives after: nothing in VirtualAudio writes
// these words at load, so a callback before its initialisers is early enough). Only with the service's option and the actuator offered,
// only in 21H461's image on the product the table refuses; asked once. The kind word also selects the virtual device's ports: {'pact', 'pnul'},
// no 'paiv': what this device is. MGGetProductType is asked where a call is fine (Initialize), not in dyld's callback.
static atomic_int gVaSettingsDone;
static uint64_t gVaProduct;
static void vaSettingsAsk(void) {
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "MGGetProductType") & (((uintptr_t)1 << 47) - 1);
    gVaProduct = symbol ? ((uint64_t (*)(void))symbol)() : 0;
    if (atomic_exchange(&gVaSettingsAsked, 1)) return;
    const struct mach_header_64 *h = __atomic_load_n(&gVaHeader, __ATOMIC_SEQ_CST);
    if (h) vaSettingsGive(h);
}
static void vaSettingsGive(const struct mach_header_64 *h) {
    const PaganiAudioMxLayout *L = pagani_audio_mx_layout;
    PaganiAudioMx *m = mxPage();
    if (!m || atomic_exchange(&gVaSettingsDone, 1)) return;
    const uintptr_t image = (uintptr_t)h;
    volatile uint8_t *guard = (volatile uint8_t *)(image + (uintptr_t)L->va_settings_guard);
    if (!vaImageMatches(h, L->va_uuid)) m->va_settings = PaganiAudioVaSettingsOtherBuild;
    else if (gVaProduct != L->va_product) m->va_settings = PaganiAudioVaSettingsOtherProduct;
    else if (*guard & 1u) m->va_settings = PaganiAudioVaSettingsPresent;
    else {
        const int late = peek64(image + (uintptr_t)L->va_device_manager) != 0;
        *(volatile uint32_t *)(image + (uintptr_t)L->va_settings_kind) = 1;
        *(volatile uint32_t *)(image + (uintptr_t)L->va_settings_format) = 'dflt';
        __atomic_store_n(guard, (uint8_t)1, __ATOMIC_RELEASE);
        m->va_settings = late ? PaganiAudioVaSettingsLate : PaganiAudioVaSettingsGiven;
    }
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: VirtualAudio's actuator settings: state %u", m->va_settings);
}

// The audio control queue's answer: an empty block sent to it, waited for 2 s on this (the probe's own) queue. A block still waiting from
// an earlier probe is not followed by another.
static dispatch_semaphore_t gAcqPong;
static atomic_int gAcqPending;
static void acqPong(void *context) {(void)context; atomic_store(&gAcqPending, 0); dispatch_semaphore_signal(gAcqPong);}
static int32_t acqPing(dispatch_queue_t queue) {
    if (!gAcqPong) gAcqPong = dispatch_semaphore_create(0);
    if (!gAcqPong || atomic_exchange(&gAcqPending, 1)) return -1;
    while (!dispatch_semaphore_wait(gAcqPong, DISPATCH_TIME_NOW)) {}
    const uint64_t start = mach_absolute_time();
    dispatch_async_f(queue, NULL, acqPong);
    if (dispatch_semaphore_wait(gAcqPong, dispatch_time(DISPATCH_TIME_NOW, 2 * (int64_t)NSEC_PER_SEC))) return -1;
    return (int32_t)((Float64)(mach_absolute_time() - start) / gHostTicksPerFrame / (kRate / 1000.0));
}

static uint32_t serialLetter(int serial) {return serial > 0 ? 'S' : serial == 0 ? 's' : '?';}

// R2: once per boot, under the conditions above; the state, the HAL's results and what VirtualAudio shows afterwards go to the page.
static atomic_int gSrzlTried;
void pagani_audio_mx_reset(void) {atomic_store(&gSrzlTried, 0); atomic_store(&gVaSettingsDone, 0); atomic_store(&gVaSettingsAsked, 0);}
static void srzlRepair(const PaganiAudioMxLayout *L, uintptr_t slide, uint32_t progress, AudioObjectID va, PaganiAudioMx *m) {
    atomic_int *const tried = &gSrzlTried;
    uint32_t category = 0;
    const int serial = probeSerialization(va);
    if (serial > 0) {
        if (m->srzl_state != PaganiAudioSrzlDone && m->srzl_state != PaganiAudioSrzlFailed) m->srzl_state = PaganiAudioSrzlPresent;
        return;
    }
    if (atomic_load(tried)) return;
    if (!slide || serial < 0 || halGet32(va, 'rctx', kAudioObjectPropertyScopeGlobal, &category) || !category ||
        (progress & (PaganiAudioMxVaPlugin | PaganiAudioMxSerial)) != (PaganiAudioMxVaPlugin | PaganiAudioMxSerial)) {
        m->srzl_state = PaganiAudioSrzlWaiting;
        return;
    }
    atomic_store(tried, 1);
    m->attempts += 1;
    if (memcmp((const void *)(slide + (uintptr_t)L->serial_listener), L->serial_code, sizeof L->serial_code) ||
        memcmp((const void *)(slide + (uintptr_t)L->receiver_listener), L->receiver_code, sizeof L->receiver_code)) {
        m->srzl_state = PaganiAudioSrzlRefused;
        return;
    }
    typedef OSStatus (*HalSetFn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
    const HalSetFn set = (HalSetFn)((uintptr_t)dlsym(RTLD_DEFAULT, "AudioObjectSetPropertyData") & (((uintptr_t)1 << 47) - 1));
    m->filter_before = vaFilterMode(L);
    if (!set) {
        m->srzl_status = -1;
        m->srzl_state = PaganiAudioSrzlFailed;
        return;
    }
    const AudioObjectPropertyAddress serialAddress = {'srzl', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                     receiverAddress = {'rwbu', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    const uint64_t serialListener[2] = {(uint64_t)(slide + (uintptr_t)L->serial_listener), 0},
                   receiverListener[2] = {(uint64_t)(slide + (uintptr_t)L->receiver_listener), 0};
    const OSStatus r1 = set(va, &serialAddress, 0, NULL, sizeof serialListener, serialListener);
    const OSStatus r2 = r1 ? 0 : set(va, &receiverAddress, 0, NULL, sizeof receiverListener, receiverListener);
    m->srzl_status = r1;
    m->rwbu_status = r2;
    m->serial_after = serialLetter(probeSerialization(va));
    m->filter_after = vaFilterMode(L);
    m->srzl_state = !r1 && !r2 ? PaganiAudioSrzlDone : PaganiAudioSrzlFailed;
    os_log_error(OS_LOG_DEFAULT, "PaganiAudio: srzl repair: srzl %d rwbu %d, listener now %c, filter %u -> %u", (int)r1, (int)r2,
                 (char)m->serial_after, m->filter_before, m->filter_after);
}

// R1 (and R2 when asked): MediaExperience's progress, the queue's answer, VirtualAudio's filter, into the MediaExperience block.
__attribute__((visibility("hidden"), noinline)) void pagani_audio_probe_mx(void);   // the host test calls it directly
void pagani_audio_probe_mx(void) {
    PaganiAudioMx *m = mxPage();
    if (!m || !(gControl->options & (PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair | PaganiAudioOptionVaSettings)) || !halResolve()) return;
    const PaganiAudioMxLayout *L = pagani_audio_mx_layout;
    uint32_t progress = 0;
    const uintptr_t slide = mxSlide(L, &progress);
    int32_t acq = -2;
    if (slide) {
        // A dispatch_once predicate: 0 not begun; the owner's thread port (low bits 0, or 1 with waiters) while it runs; then a generation
        // (low bits 2, dispatch_once_f of 21H461), which only a later call on the predicate turns into ~0: MXInitialize is called once.
        const uint64_t once = peek64(slide + (uintptr_t)L->once);
        progress |= (once ? PaganiAudioMxStarted : 0u) | (once == ~0ull || (once & 3u) == 2u ? PaganiAudioMxDone : 0u);
        progress |= peek64(slide + (uintptr_t)L->session) ? PaganiAudioMxSession : 0u;
        progress |= peek64(slide + (uintptr_t)L->screen) ? PaganiAudioMxScreen : 0u;
        progress |= peek64(slide + (uintptr_t)L->ids) ? PaganiAudioMxIds : 0u;
        progress |= peek64(slide + (uintptr_t)L->routes) ? PaganiAudioMxRoutes : 0u;
        progress |= peek64(slide + (uintptr_t)L->context) ? PaganiAudioMxContext : 0u;
        progress |= peek64(slide + (uintptr_t)L->endpoint) ? PaganiAudioMxEndpoint : 0u;
        progress |= peek32(slide + (uintptr_t)L->va_plugin) ? PaganiAudioMxVaPlugin : 0u;
        progress |= peek32(slide + (uintptr_t)L->serial_done) || peek32(slide + (uintptr_t)L->serial_done + 4) ? PaganiAudioMxSerial : 0u;
        const uint64_t queue = peek64(slide + (uintptr_t)L->queue);
        if (queue) {
            progress |= PaganiAudioMxQueue;
            if (gControl->options & PaganiAudioOptionMxProbe) {
                acq = acqPing((dispatch_queue_t)(uintptr_t)queue);
                if (acq >= 0) progress |= PaganiAudioMxQueueAlive;
            }
        }
        progress |= vaFilterMode(L) << PaganiAudioMxFilterShift;
    }
    m->progress = progress;
    m->acq_ms = acq;
    if (gControl->options & PaganiAudioOptionSrzlRepair) {
        const AudioObjectID va = halTranslate(0, kSelPlugInForBundleID, CFSTR("com.apple.audio.CoreAudio.VirtualAudio"));
        const uint32_t state = m->srzl_state;
        srzlRepair(L, slide, progress, va, m);
        if (state != m->srzl_state && (m->srzl_state == PaganiAudioSrzlDone || m->srzl_state == PaganiAudioSrzlFailed) && gProbeQueue)
            dispatch_after_f(probeTime(5), gProbeQueue, (void *)(uintptr_t)PaganiAudioProbeTimer, probeWork);   // its effect, soon
    }
    __atomic_store_n(&m->gen, m->gen + 1, __ATOMIC_RELEASE);
}

// The actuator as the audio server took it (with the route probe, when the actuator is offered): VirtualAudio's virtual device for it
// exists once Device_Actuator_Aspen made the port 'pact' ("Found actuator port!": its deferred initialisation stops waiting), and the
// routing layer hogs the device it takes.
__attribute__((visibility("hidden"))) void pagani_audio_probe_actuator(void);   // the host test calls it directly
void pagani_audio_probe_actuator(void) {
    PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
    if (!act || !gControl || !(gControl->options & PaganiAudioOptionProbeRoute) || !halResolve()) return;
    act->va_device = halTranslate(kSelUIDToDevice, kSelDeviceForUID, CFSTR("VirtualAudioDevice_Actuator"));
    act->hog = probeHog(halTranslate(kSelUIDToDevice, kSelDeviceForUID, CFSTR(kActuatorUID)));
}

static void probeWork(void *context) {
    const uint32_t reason = (uint32_t)(uintptr_t)context;
    if (reason != PaganiAudioProbeTimer) atomic_store(&gProbePending[reason], 0);
    pagani_audio_probe_route(reason);
    pagani_audio_probe_mx();
    pagani_audio_probe_actuator();
}
static dispatch_time_t probeTime(unsigned seconds) {return dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * (int64_t)NSEC_PER_SEC);}
// At load: a few probes while the audio server settles (its routing layer and MediaExperience start after the plug-ins load).
static void probeStart(void) {
    static atomic_int started;
    if (atomic_exchange(&started, 1)) return;
    gProbeQueue = dispatch_queue_create("org.unixnu.pagani.audio.probe", DISPATCH_QUEUE_SERIAL);
    if (!gProbeQueue) return;
    atomic_store(&gProbeBudget[PaganiAudioProbeVolume], kProbeEventBudget);
    atomic_store(&gProbeBudget[PaganiAudioProbeStart], kProbeEventBudget);
    for (unsigned i = 0; i < sizeof kProbeAfterLoad / sizeof kProbeAfterLoad[0]; ++i)
        dispatch_after_f(probeTime(kProbeAfterLoad[i]), gProbeQueue, (void *)(uintptr_t)PaganiAudioProbeTimer, probeWork);
}
// After a volume change or an IO start: one probe a moment later (one queued at a time per reason, a few per boot).
static void probeSoon(uint32_t reason) {
    if (!gProbeQueue || reason <= PaganiAudioProbeTimer || reason > PaganiAudioProbeStart) return;
    if (!gControl || !(gControl->options & PaganiAudioOptionProbeRoute) || atomic_exchange(&gProbePending[reason], 1)) return;
    unsigned left = atomic_load(&gProbeBudget[reason]);
    do {
        if (!left) {atomic_store(&gProbePending[reason], 0); return;}
    } while (!atomic_compare_exchange_weak(&gProbeBudget[reason], &left, left - 1));
    dispatch_after_f(probeTime(kProbeEventDelay), gProbeQueue, (void *)(uintptr_t)reason, probeWork);
}

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
// The device list changed (the service came up after the plug-in loaded and offers the actuator): the audio server is told.
static void announceDevices(void) {
    const AudioObjectPropertyAddress changed[2] = {{kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                   {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
    if (gHost) gHost->PropertiesChanged(gHost, kPlugIn, 2, changed);
}
static OSStatus startActuator(void);
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o == kActuator) return startActuator();
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    const int actuatorBefore = haveActuator();
    if (gIoUsers == 0) {
        if (connectService()) r = kAudioHardwareNotRunningError;
        else if (!gControl) r = kAudioHardwareNotRunningError;
        else {
            setHostClock();   // again: the shared page may have been read back from disk since
            if (call(PaganiAudioStartOutput) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
            else {
                call(PaganiAudioStartInput);
                const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
                gZeroOrigin = host > anchor ? (uint64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
                gZeroSeed++;
            }
        }
    }
    const int first = !r && gIoUsers == 0;
    if (!r) gIoUsers++;
    const int appeared = !actuatorBefore && haveActuator();
    pthread_mutex_unlock(&gLock);
    if (appeared) announceDevices();
    if (first) probeSoon(PaganiAudioProbeStart);
    return r;
}
// The actuator's IO: one kernel request for its first client (the service's haptics bridge then takes its ring), time stamps of its own.
// A device listed without a ring refuses it as the speaker device does without the service; a service that came up late is connected first.
static OSStatus startActuator(void) {
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (!haveActuator()) {
        pthread_mutex_unlock(&gLock);
        return kAudioHardwareBadObjectError;
    }
    if (!__atomic_load_n(&gAct, __ATOMIC_ACQUIRE)) (void)connectService();
    PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
    if (!act) r = kAudioHardwareNotRunningError;
    else if (gActIoUsers == 0) {
        if (call(PaganiAudioStartActuator) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
        else {
            const uint64_t host = mach_absolute_time(), anchor = act->anchor_host;
            gActZeroOrigin = host > anchor ? (uint64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
            gActZeroSeed++;
            act->io_starts += 1;
        }
    }
    if (!r) gActIoUsers++;
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o == kActuator) {
        pthread_mutex_lock(&gLock);
        PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
        if (act && gActIoUsers && --gActIoUsers == 0) {
            call(PaganiAudioStopActuator);
            act->io_stops += 1;
        }
        pthread_mutex_unlock(&gLock);
        return haveActuator() ? 0 : kAudioHardwareBadObjectError;
    }
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {call(PaganiAudioStopOutput); call(PaganiAudioStopInput);}
    pthread_mutex_unlock(&gLock);
    return 0;
}
// The actuator's zero time stamps: the main timeline (the same anchor and frame length), counted from its own IO start, a period of
// its ring.
static OSStatus actuatorZeroTimeStamp(Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    const PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
    if (!act || !gControl) return kAudioHardwareNotRunningError;
    const uint64_t now = mach_absolute_time(), anchor = act->anchor_host;
    const Float64 period = gHostTicksPerFrame * PAGANI_AUDIO_ACT_RING_FRAMES;
    const uint64_t origin = anchor + (uint64_t)((Float64)gActZeroOrigin * gHostTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gActZeroOrigin + wraps * PAGANI_AUDIO_ACT_RING_FRAMES);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = 1 + gActZeroSeed;
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)client;
    if (o == kActuator) return actuatorZeroTimeStamp(sampleTime, hostTime, seed);
    if (!gControl) return kAudioHardwareNotRunningError;
    uint32_t seq;
    uint64_t anchor;
    do {seq = gControl->out_seq; anchor = gControl->out_anchor_host;} while ((seq & 1) || seq != gControl->out_seq);
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * kZeroPeriod;
    const uint64_t origin = anchor + (uint64_t)((Float64)gZeroOrigin * gHostTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gZeroOrigin + wraps * kZeroPeriod);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = 1 + seq / 2 + gZeroSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix || (op == kAudioServerPlugInIOOperationReadInput && o != kActuator);
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
// Records how the audio server schedules its IO thread (i545: 98 gaps of more than 160 ms between IO cycles).
static void noteThread(PaganiAudioDiag *diag) {
    const thread_act_t self = mach_thread_self();
    thread_extended_info_data_t info;
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
        diag->io_pri = info.pth_curpri;
        diag->io_base = info.pth_priority;
        diag->io_policy = info.pth_policy;
        if (!diag->io_pri_min || info.pth_curpri < diag->io_pri_min) diag->io_pri_min = info.pth_curpri;
    }
    thread_time_constraint_policy_data_t tc;
    count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = false;
    if (thread_policy_get(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &count, &def) == KERN_SUCCESS) {
        diag->io_tc = def ? 0 : 1;
        diag->io_period = tc.period;
        diag->io_computation = tc.computation;
        diag->io_constraint = tc.constraint;
    } else diag->io_tc = -1;
    mach_port_deallocate(mach_task_self(), self);
}
static inline int16_t toS16(Float32 v) {v *= 32767.0f; return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);}
// The actuator's mix: added to the actuator ring at its sample times (several IO contexts of the audio server add up, saturated), frames
// behind what the kernel has taken dropped and counted. The write that begins a sound after silence records its first loud frame and its
// host time, from which the kernel measures how long the vibrator took to start.
static void writeActuator(const AudioServerPlugInIOCycleInfo *info, UInt32 frames, const Float32 *src) {
    PaganiAudioActuatorControl *act = __atomic_load_n(&gAct, __ATOMIC_ACQUIRE);
    if (!act || !src) return;
    static uint64_t lastLoudEnd;   // the sample time after the last loud frame written, 0 none yet (this device's IO thread only)
    const uint64_t host = mach_absolute_time(), anchor = act->anchor_host;
    const int64_t nowFrames = host > anchor ? (int64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
    const int32_t ahead = (int32_t)((int64_t)info->mOutputTime.mSampleTime - nowFrames);
    if (!act->writes || ahead < act->ahead_min) act->ahead_min = ahead;
    if (!act->writes || ahead > act->ahead_max) act->ahead_max = ahead;
    act->writes += 1;
    uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
    const uint64_t taken = __atomic_load_n(&act->taken, __ATOMIC_ACQUIRE);
    UInt32 first = 0;
    if (t < taken) {
        first = taken - t < frames ? (UInt32)(taken - t) : frames;
        act->dropped_frames += first;
        act->dropped_writes += 1;
        t += first;
    }
    Float32 peak = 0;
    uint64_t loud = 0;
    for (UInt32 i = first; i < frames; ++i, ++t) {
        const Float32 v = src[i];
        int16_t *dst = gActRing + (t & (PAGANI_AUDIO_ACT_RING_FRAMES - 1));
        *dst = toS16(*dst / 32767.0f + v);
        const Float32 m = fabsf(v);
        if (m > peak) peak = m;
        if (m >= 1.0f / 32767.0f && !loud) loud = t + 1;
    }
    if (loud) {
        if (!lastLoudEnd || loud - 1 > lastLoudEnd + 4800) {   // the first sound, or one after 100 ms of silence
            act->sound_frame = loud - 1;
            act->sound_host = host;
        }
        lastLoudEnd = t;
    }
    const UInt32 p = (UInt32)(fminf(peak, 1.0f) * 32767.0f);
    if (p > act->peak) act->peak = p;
    act->frames += frames - first;
    if (t > act->written) act->written = t;
    act->written_host = host;
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)client; (void)secondary;
    if (o == kActuator) {
        if (op == kAudioServerPlugInIOOperationWriteMix && stream == kActStream) writeActuator(info, frames, main);
        return 0;
    }
    if (!gControl) return 0;
    // The play position now, on the timeline GetZeroTimeStamp gives the audio server.
    const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
    const int64_t nowFrames = host > anchor ? (int64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
    PaganiAudioDiag *diag = (PaganiAudioDiag *)((uint8_t *)gControl + PAGANI_AUDIO_DIAG_OFFSET);
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream) {
        const int32_t ahead = (int32_t)((int64_t)info->mOutputTime.mSampleTime - nowFrames);
        // Where the audio server places now, against the timeline.
        const int32_t skew = (int32_t)((int64_t)info->mCurrentTime.mSampleTime - nowFrames);
        if (!diag->writes || skew < diag->skew_min) diag->skew_min = skew;
        if (!diag->writes || skew > diag->skew_max) diag->skew_max = skew;
        if (!diag->writes || ahead < diag->ahead_min) diag->ahead_min = ahead;
        if (!diag->writes || ahead > diag->ahead_max) diag->ahead_max = ahead;
        diag->io_frames = frames;
        if (diag->writes % 256 == 0) noteThread(diag);
        diag->writes += 1;
        const Float32 *src = main;
        uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
        const Float32 g = gain();
        Float32 peak = 0;
        for (UInt32 i = 0; i < 2 * frames; ++i) {const Float32 v = fabsf(src[i]); if (v > peak) peak = v;}
        const UInt32 p = (UInt32)(fminf(peak, 1.0f) * 32767.0f);
        if (p > diag->mix_peak) diag->mix_peak = p;
        // The audio server runs more than one IO context on the device (i539: 65291 writes of 1024 frames in a 600 s run, 2.3 times the
        // frames that played), each with its own mix. Each is added to what the ring holds, so a context writing silence does not replace
        // another one's sound; the service clears every period it takes.
        if (diag->last_host) {
            const Float64 ms = (Float64)(host - diag->last_host) / gHostTicksPerFrame / (kRate / 1000.0);
            diag->interval[ms < 15 ? 0 : ms < 25 ? 1 : ms < 40 ? 2 : ms < 80 ? 3 : ms < 160 ? 4 : 5] += 1;
            if (ms < 160 && t > diag->last_end) {diag->gaps += 1; diag->gap_frames += (uint32_t)(t - diag->last_end);}
        }
        diag->last_host = host;
        diag->last_end = t + frames;
        diag->frames_total += frames;
        const uint64_t written = gControl->out_written;
        if (t < written) {
            diag->overlaps += 1;
            if (written - t > diag->overlap_max) diag->overlap_max = (uint32_t)(written - t);
        }
        // Frames the service has already taken (a late IO cycle) are dropped: in the ring they would wait a whole turn and play 1.37 s
        // late, mixed into other sound.
        const uint64_t taken = __atomic_load_n(&gControl->out_taken, __ATOMIC_ACQUIRE);
        UInt32 first = 0;
        if (t < taken) {
            first = taken - t < frames ? (UInt32)(taken - t) : frames;
            diag->dropped_frames += first;
            diag->dropped_writes += 1;
            t += first;
        }
        for (UInt32 i = first; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % PAGANI_AUDIO_RING_FRAMES);
            dst[0] = toS16(dst[0] / 32767.0f + src[2 * i] * g);
            dst[1] = toS16(dst[1] / 32767.0f + src[2 * i + 1] * g);
        }
        if (t > gControl->out_written) gControl->out_written = t;
        gControl->out_written_host = host;
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStream) {
        // Frames the service has not captured yet read as silence, not as what the ring held one turn earlier; such reads are counted.
        const int32_t behind = (int32_t)(nowFrames - (int64_t)info->mInputTime.mSampleTime);
        if (!diag->behind_max || behind < diag->behind_min) diag->behind_min = behind;
        if (behind > diag->behind_max) diag->behind_max = behind;
        Float32 *dst = main;
        uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        const uint64_t written = gControl->in_written;
        if (t + frames > written) diag->in_misses += 1;
        for (UInt32 i = 0; i < frames; ++i, ++t) dst[i] = t < written ? gIn[t % PAGANI_AUDIO_RING_FRAMES] / 32768.0f : 0.0f;
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *PaganiAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    return &gInterfacePtr;
}
