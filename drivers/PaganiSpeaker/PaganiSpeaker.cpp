// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The thread, the waits, the low buffer mapping and the DSP client callbacks follow UniXNU-sheng package/AudioProbe.cpp
// (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e: audioThread, dspState/dspReceive, nextAnswer, setupBuffer, mapRegion).
//
// Kernel side of PaganiSpeaker (README.md). Stage7Probe calls pagani_speaker_start() after pagani_audio_start() (weak default there).
// Without pagani-speaker=1 it returns at once: nothing is allocated, mapped or started (pagani-audio-tone alone only logs that it was
// ignored). With it, one kernel thread, "pagani-speaker", runs the bring-up of SpeakerCore.c (pins, L15B, the I2C hub, both TFA9873,
// the DSP's adsp_apps channel through PaganiAdsp, APM readiness, SMMU pass-through, the period buffer, the AudioReach graphs), plays
// the boot tone with pagani-audio-tone=1 (=2: the walk over both speakers) without anything of iOS, then hands the "adsp" backend
// (SpeakerBackend.c) to the PaganiAudio service (pagani-audio=1) and ends; from then on the service's thread drives it. Every period
// passes the digital ceiling (SpeakerLimit.h, pagani-speaker-ceiling). Every wait is bounded; nothing panics; failures are "[spk]"
// lines in the boot log (BootProgress's ramoops U records; weak default when BootProgress is absent).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "SpeakerBackend.h"
#include "SpeakerCore.h"
#include "SpeakerSmmu.h"
#define PAGANI_ADSP_WEAK_DEFAULTS
#include "PaganiAdsp.h"
}
#include <Rpmh.h>          // SoC kit (Sm8750Platform): rpmh_vrm_address, sm8750_rpmh_vrm_enable (the touch rails' sequence)
#include "Drv2Lock.h"      // drivers/PaganiInput: the DRV-2 lock word shared with the touch and GPU votes

// The statically linked kernel refuses undefined weak references: providers that may be absent get weak defaults here (the real
// definitions in BootProgress, PaganiAudio and PaganiAdsp win whenever they are linked; PAGANI_ADSP_WEAK_DEFAULTS above).
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // the word UfsLog.c's writer stops on while it is not 0, bit by bit
__attribute__((weak)) int pagani_audio_set_backend(const PaganiAudioBackend *) {return -1;}
__attribute__((weak)) void pagani_audio_wake(void) {}
__attribute__((weak)) uint32_t pagani_audio_engine_take(PaganiAudioEngine *, uint64_t, int16_t *, uint64_t) {return 0;}
__attribute__((weak)) void pagani_audio_engine_played(PaganiAudioEngine *, uint64_t) {}
__attribute__((weak)) void pagani_audio_engine_captured(PaganiAudioEngine *, const int16_t *, uint32_t, uint64_t) {}
__attribute__((weak)) void pagani_audio_engine_align_output(PaganiAudioEngine *, uint64_t) {}
__attribute__((weak)) uint64_t pagani_audio_engine_frame_at(const PaganiAudioEngine *, uint64_t) {return 0;}
__attribute__((weak)) const PaganiAudioBackend pagani_audio_silent_backend = {};
extern uint64_t gPhysBase, gPhysSize;
}

namespace {
enum : uint32_t {
    kDrv2Owner = SPEAKER_GCC_OWNER,          // 'SPKR', as in the GCC vote word
    kHandTries = 60,                         // the PaganiAudio service not serving yet: asked again every 500 ms for 30 s
    kHandPauseMs = 500,
};

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t base;
    uint32_t size;
};
bool mapWindow(Window &w, uint64_t base, uint32_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.base = base;
    w.size = size;
    return w.va != nullptr;
}
uint32_t rd(const Window &w, uint32_t at)
{
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr(const Window &w, uint32_t at, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

constexpr uint16_t kPins[] = {SpeakerPinSda, SpeakerPinScl, SpeakerPinSck, SpeakerPinData0, SpeakerPinWs, SpeakerPinData1,
                              SpeakerPinResetTop, SpeakerPinResetBottom};
constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];

volatile uint32_t gStarted, gPending, gHanded;
SpeakerCore *gCore;
SpeakerConfig gConfig;
SpeakerBackend gBackendState;
PaganiAudioBackend gBackend;
PaganiAdspChannel *gChannel;
Window gGcc, gSe, gPin[kPinCount], gSmmu, gRsc, gCmdDb, gBuffer;
uint32_t gTone, gToneHz = 440, gToneMs = 500, gToneAtten = 30, gAudio;

uint64_t nowNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

// ---- events: the DSP thread queues and wakes; the owner of the core waits ----
void wake()
{
    __atomic_store_n(&gPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
    if (__atomic_load_n(&gHanded, __ATOMIC_ACQUIRE)) pagani_audio_wake();   // the service thread runs the backend's pump
}
void dspState(void *, int up)
{
    speaker_core_state(gCore, up);
    wake();
}
void dspReceive(void *, const uint8_t *data, uint32_t length)
{
    speaker_core_receive(gCore, data, length);
    wake();
}

uint64_t opNow(void *) {return nowNs() / 1000000ull;}
uint64_t opTicks(void *) {return mach_absolute_time();}
void opWait(void *, uint32_t ms)
{
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&gPending, 0u, __ATOMIC_RELEASE);
}
void opTrace(void *, const char *line) {trace(line);}
int opChannel(void *)
{
    static const PaganiAdspClient client = {dspState, dspReceive};
    // Android's adsp_apps: 20 intents of 512 bytes, the DSP opens it; our largest packet (the stream graph's open) fits 512.
    const PaganiAdspChannelConfig config = {"adsp_apps", 512, 20, 0, 512, PAGANI_ADSP_MAX_QUEUE};
    gChannel = pagani_adsp_channel_config(&config, &client, nullptr);
    return gChannel ? 0 : PaganiAdspOff;
}
int opSend(void *, const uint8_t *packet, uint32_t length) {return pagani_adsp_send(gChannel, packet, length);}
int opDspState(void *, int32_t *stage, uint32_t *crashed)
{
    PaganiAdspStatus status;
    const int rc = pagani_adsp_status(&status);
    if (rc != PaganiAdspOk) return rc;
    *stage = status.stage;
    *crashed = status.crashed;
    return 0;
}

// ---- L15B through apps RSC DRV-2: the command database names the VRM, the touch driver's three-command request votes it ----
uint32_t kitRead(void *context, uint32_t offset) {return rd(*static_cast<Window *>(context), offset);}
void kitWrite(void *context, uint32_t offset, uint32_t value) {wr(*static_cast<Window *>(context), offset, value);}
void kitDelay(void *, unsigned microseconds)
{
    if (microseconds >= 1000) IOSleep(microseconds / 1000);
    else IODelay(microseconds);
}
void lockDelay(unsigned microseconds) {IODelay(microseconds);}
int opRail(void *, uint32_t millivolts, uint32_t *detail)
{
    if (!mapWindow(gRsc, SPEAKER_RSC_DRV2, SPEAKER_RSC_SIZE) || !mapWindow(gCmdDb, SPEAKER_CMD_DB, SPEAKER_CMD_DB_SIZE)) return -12;
    const peripheral_io db = {&gCmdDb, kitRead, kitWrite, kitDelay}, rsc = {&gRsc, kitRead, kitWrite, kitDelay};
    uint32_t address = 0;
    int rc = rpmh_vrm_address(&db, SPEAKER_CMD_DB_SIZE, SPEAKER_RAIL, &address);
    *detail = address;
    if (rc) return rc;
    uint32_t waits = 0;
    rc = pagani_drv2_acquire(kDrv2Owner, lockDelay, &waits);
    if (rc) return rc;
    rpmh_result result = {};
    rc = sm8750_rpmh_vrm_enable(&rsc, address, millivolts, &result);
    pagani_drv2_release();
    *detail = (address & 0xfffffu) | (result.tcs & 0xfu) << 20 | (waits > 0xff ? 0xffu : waits) << 24;
    return rc;
}

// ---- the audio streams in pass-through, on the bank the storage stream runs on (SpeakerSmmu.h: one S2CR and one SMR, nothing else) ----
uint32_t smmuRead(void *, uint32_t offset) {return rd(gSmmu, offset);}
void smmuWrite(void *, uint32_t offset, uint32_t value) {wr(gSmmu, offset, value);}
int opSmmu(void *, uint16_t sid, uint64_t pa, uint32_t bytes, uint32_t *detail)
{
    if (!mapWindow(gSmmu, SPEAKER_SMMU_BASE, SPEAKER_SMMU_SIZE)) return -12;
    const SpeakerSmmuIo io = {nullptr, smmuRead, smmuWrite};
    const SpeakerSmmuPlan plan = speaker_smmu_plan(&io, sid, pa, bytes, SPEAKER_SMMU_SIZE);
    *detail = (plan.slot & 0xffu) | (plan.bank & 0xffu) << 8 | (plan.reuse ? 1u << 16 : 0u) | (plan.error ? (plan.s2cr & 0xffu) << 24 : 0u);
    return speaker_smmu_configure(&io, &plan, sid);
}

// ---- the period buffer: low memory outside XNU's window (the DSP takes 32-bit addresses), write-combined ----
uint8_t *opBuffer(void *, uint64_t pa, uint32_t bytes)
{
    const uint64_t end = pa + bytes;
    if ((pa & 0xfff) || !bytes || end > 0x100000000ull || (pa < gPhysBase + gPhysSize && end > gPhysBase)) return nullptr;
    if (!mapWindow(gBuffer, pa, bytes, kIOMapWriteCombineCache)) return nullptr;
    return const_cast<uint8_t *>(gBuffer.va);
}
void opPublish(void *) {__asm__ volatile("dsb sy" ::: "memory");}
// The word is shared bit by bit: never a plain store.
void opQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, SPEAKER_QUIET_BIT, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~uint32_t(SPEAKER_QUIET_BIT), __ATOMIC_RELEASE);
}

// ---- register windows ----
const Window *window(unsigned id)
{
    if (id == SpeakerWinGcc) return &gGcc;
    if (id == SpeakerWinSe) return &gSe;
    for (unsigned i = 0; i < kPinCount; ++i) if (id == SPEAKER_WIN_PIN(kPins[i])) return &gPin[i];
    return nullptr;
}
uint32_t ioRead(void *, unsigned id, uint32_t offset)
{
    const Window *w = window(id);
    return w && offset + 4 <= w->size ? rd(*w, offset) : 0xffffffffu;
}
void ioWrite(void *, unsigned id, uint32_t offset, uint32_t value)
{
    const Window *w = window(id);
    if (w && offset + 4 <= w->size) wr(*w, offset, value);
}
void ioDelay(void *, uint32_t microseconds) {kitDelay(nullptr, microseconds);}
const SpeakerIo kIo = {nullptr, ioRead, ioWrite, ioDelay};
const SpeakerOps kOps = {nullptr, opNow, opWait, opTrace, opChannel, opSend, opDspState, opRail, opSmmu, opBuffer, opPublish, &kIo, opQuiet};

bool mapAll()
{
    if (!mapWindow(gGcc, SPEAKER_GCC_BASE, SPEAKER_GCC_SIZE) || !mapWindow(gSe, SPEAKER_SE_BASE, SPEAKER_SE_SIZE)) return false;
    for (unsigned i = 0; i < kPinCount; ++i) if (!mapWindow(gPin[i], SPEAKER_TLMM_PIN(kPins[i]), 0x1000)) return false;
    return true;
}

uint32_t argument(const char *name, uint32_t fallback)
{
    uint32_t value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}

// How long a line takes to reach the UFS log: its period (pagani-ufslog-ms, default 3 s, UfsLog.h) and a look of the writer, 500 ms at the
// longest. No log, nothing writes: the holds and their waits are left out.
uint32_t crumbMs()
{
    if (argument("sheng-log", 0) != 1) return 0;
    const uint32_t period = argument("pagani-ufslog-ms", 3000);
    return (period >= 1000 && period <= 3000 ? period : 3000) + 500;
}

void speakerThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-speaker");
    if (!mapAll()) {
        trace("[spk] FAIL st=-1 err=-12 register windows");
        return;
    }
    speaker_core_init(gCore, &kOps, &gConfig);
    if (speaker_core_bringup(gCore) != 1) return;   // the core logged where it stopped
    // The kernel's own test tone: the DSP and the amplifiers only, nothing of iOS (no plug-in, no audio server, no route).
    if (gTone) speaker_core_tones(gCore, gTone, gToneHz, gToneMs, gToneAtten);
    if (!gAudio) {
        trace("[spk] pagani-audio is not set: no iOS output, the speaker path stays idle (st=11 after a tone)");
        return;
    }
    speaker_backend_init(&gBackendState, &gBackend, gCore, opTicks, nullptr);
    __atomic_store_n(&gHanded, 1u, __ATOMIC_RELEASE);
    // From here the service's thread owns the core: this thread touches it no more (its line buffer included). A service that is
    // not serving yet (it starts with Stage7Probe, so it should be long before the DSP) is asked again for 30 s.
    for (uint32_t tries = 1;; ++tries) {
        if (pagani_audio_set_backend(&gBackend) == 0) {
            speaker_core_reached(gCore, SpeakerStServing);   // only the stage word: the service never writes it
            trace("[spk] backend adsp handed to the PaganiAudio service (st=12)");
            return;
        }
        if (tries >= kHandTries) break;
        IOSleep(kHandPauseMs);
    }
    __atomic_store_n(&gHanded, 0u, __ATOMIC_RELEASE);
    trace("[spk] FAIL st=-12 err=-19 the PaganiAudio service never took the adsp backend (30 s): the speaker path stays idle");
}
}  // namespace

// Stage7Probe's call (its weak default does nothing). Once per boot; returns at once without pagani-speaker=1 (default 0): nothing
// is allocated, mapped or started then, and pagani-audio-tone alone does nothing either (one line says so).
extern "C" void pagani_speaker_start(void)
{
    const uint32_t speaker = argument("pagani-speaker", 0), tone = argument("pagani-audio-tone", 0);
    if (speaker != 1) {
        if (speaker || tone) trace("[spk] off: pagani-speaker=1 is not set (pagani-audio-tone needs it); nothing started");
        return;
    }
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    gTone = tone ? (tone == 2 ? 2u : 1u) : 0u;
    gAudio = argument("pagani-audio", 0) != 0;
    gToneHz = argument("pagani-audio-tone-hz", 440);
    gToneMs = argument("pagani-audio-tone-ms", 500);
    gToneAtten = argument("pagani-audio-tone-atten", 30);
    gConfig.stop_after = argument("pagani-speaker-stop", 0);
    gConfig.amps = argument("pagani-speaker-amps", 3);
    gConfig.sd_line = argument("pagani-speaker-sd", AR_I2S_SD0);
    gConfig.device_bits = argument("pagani-speaker-bits", 32);
    gConfig.prm_port = argument("pagani-speaker-prm-port", AR_PRM_IID);
    gConfig.prm_clock = argument("pagani-speaker-clock", 1);
    gConfig.feedback = argument("pagani-speaker-feedback", 0);
    gConfig.i2c_pull = argument("pagani-speaker-i2c-pull", 1);
    gConfig.rail = argument("pagani-speaker-rail", 1);
    gConfig.sids = argument("pagani-speaker-sids", 0xf);
    gConfig.dsp_wait_ms = argument("pagani-speaker-dsp-ms", 120000);
    gConfig.ceiling_db = argument("pagani-speaker-ceiling", SPEAKER_LIMIT_DEFAULT_DB);   // dB below full scale; 3..40, never off
    gConfig.crumb_ms = crumbMs();
    uint64_t pa = SPEAKER_BUFFER_PA;
    PE_parse_boot_argn("pagani-speaker-pa", &pa, sizeof pa);
    gConfig.buffer_pa = pa;
    gCore = static_cast<SpeakerCore *>(IOMallocZeroData(sizeof(SpeakerCore)));
    if (!gCore) {
        trace("[spk] FAIL st=-1 err=-12 no memory for the core");
        return;
    }
    thread_t thread;
    if (kernel_thread_start(speakerThread, nullptr, &thread) != KERN_SUCCESS) {
        trace("[spk] FAIL st=-1 err=-12 thread");
        return;
    }
    thread_deallocate(thread);
}
