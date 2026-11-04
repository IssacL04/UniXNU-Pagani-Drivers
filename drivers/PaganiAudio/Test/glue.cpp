// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the PaganiAudio kernel glue, which the kernel archive only compiles: PaganiAudio.cpp (boot arguments, the start and each
// of its failures, the entry points before and after serving, the service thread's round, the boot log's rationing) and
// PaganiAudioService.cpp (the user client: open, memory, requests, close; the host clock correction and its guards). Both files are
// included here, so their internal functions and state are reachable, with the kernel interfaces of Test/kstubs and the real engine.
// Checked: without pagani-audio (or with 0) one boot argument is read and nothing else happens (no lock, memory, class, thread, service,
// log line); each failing step is recorded, logged once and stops there, and later entry points refuse without touching the lock; the
// started service maps the right memory with the right options, publishes the grant and the user client class, serves requests per
// client and refuses those of a closed client; the host clock correction writes only a 24 MHz (or the board's) value on managed RAM
// inside the shared region, never calls phystokv for a device page, and balances every prepare; pagani-audio-log=2 writes rounds 10 s
// apart only for its first 16, then 10 min apart; the plug-in's route probe is on by default (pagani-audio-probe=0 leaves it out) and
// becomes an "audio route" row when it changes, rationed, never from a half-written probe, with 'vcsp' also as MediaExperience takes it.
// Build: c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -I kstubs -I ..
//        glue.cpp glue_log.c ../PaganiAudioEngine.c -o glue
#include <setjmp.h>
#include <stdlib.h>
#include <initializer_list>
#include "kstubs/kstub.h"
#include "../PaganiAudioService.cpp"
#include "../PaganiAudio.cpp"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

// ---- the kernel, as the glue sees it ------------------------------------------------------------------------------------------------
extern "C" {
extern char kstub_lines[256][256];   // glue_log.c: BootProgress's boot log
extern unsigned kstub_line_count;
kmod_info_t g_kernel_kmod_info;
// glue_haptics.c: the vibrator's stream API as PaganiHaptics would answer it, recorded.
extern int kstub_vib_begins, kstub_vib_ends, kstub_vib_writes, kstub_vib_begin_rc, kstub_vib_last_drain, kstub_vib_peak;
extern uint32_t kstub_vib_rate, kstub_vib_mv, kstub_vib_samples;
}

static uint64_t gNow = 1000000000ULL;                    // host ticks
static mach_timebase_info_data_t gTimebase = {625, 12};   // the 19.2 MHz timer
static uint32_t gLastWaitMs;
uint64_t kstub_mach_absolute_time(void) {return gNow;}
void kstub_clock_timebase_info(mach_timebase_info_data_t *info) {*info = gTimebase;}
void kstub_absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) {*ns = gTimebase.numer ? t * gTimebase.numer / gTimebase.denom : 0;}
void kstub_clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *result)
{
    gLastWaitMs = scale == kMillisecondScale ? interval : 0;
    *result = gNow + (uint64_t)interval * scale * gTimebase.denom / gTimebase.numer;
}

struct BootArg {const char *name; uint32_t value;};
static BootArg gArgs[12];
static unsigned gArgCount, gArgReads;
static void bootArgs(std::initializer_list<BootArg> list)
{
    gArgCount = 0;
    for (const BootArg &a : list) gArgs[gArgCount++] = a;
}
boolean_t PE_parse_boot_argn(const char *name, void *ptr, int size)
{
    gArgReads++;
    for (unsigned i = 0; i < gArgCount; ++i) {
        if (strcmp(gArgs[i].name, name)) continue;
        CHECK(size == sizeof(uint32_t));
        memcpy(ptr, &gArgs[i].value, sizeof(uint32_t));
        return 1;
    }
    return 0;
}

static unsigned gIOLogs;
void IOLog(const char *, ...) {gIOLogs++;}

struct KStubLock {int held, wakeups;};
static KStubLock gLockStorage;
static int gLockAllocs, gLockFail, gSleeps, gSleepsLeft;
static jmp_buf gSleepEnd;
IOLock *IOLockAlloc(void)
{
    if (gLockFail) return nullptr;
    gLockAllocs++;
    gLockStorage = KStubLock{};
    return &gLockStorage;
}
void IOLockLock(IOLock *lock) {CHECK(lock == &gLockStorage && !lock->held); lock->held = 1;}
void IOLockUnlock(IOLock *lock) {CHECK(lock == &gLockStorage && lock->held); lock->held = 0;}
void IOLockWakeup(IOLock *lock, void *event, bool oneThread)
{
    CHECK(lock == &gLockStorage && lock->held && event == (void *)&gEvents && !oneThread);
    lock->wakeups++;
}
// The service thread's sleep: the test ends the thread's loop here once it has slept as often as asked.
int IOLockSleepDeadline(IOLock *lock, void *event, uint64_t deadline, UInt32 type)
{
    CHECK(lock == &gLockStorage && lock->held && event == (void *)&gEvents && type == THREAD_UNINT && deadline >= gNow);
    gSleeps++;
    if (--gSleepsLeft <= 0) {
        lock->held = 0;
        longjmp(gSleepEnd, 1);
    }
    return 0;
}

task_t kstub_kernel_task = (task_t)0x55;
static thread_continue_t gThreadFn;
static int gThreadStarts, gThreadFail, gThreadDeallocs;
static char gThreadName[32];
kern_return_t kstub_kernel_thread_start(thread_continue_t fn, void *, thread_t *thread)
{
    if (gThreadFail) return KERN_FAILURE;
    gThreadFn = fn;
    gThreadStarts++;
    *thread = (thread_t)0x77;
    return KERN_SUCCESS;
}
void kstub_thread_deallocate(thread_t thread) {CHECK(thread == (thread_t)0x77); gThreadDeallocs++;}
void kstub_thread_set_thread_name(thread_t thread, const char *name) {CHECK(thread == (thread_t)0x77); strlcpy(gThreadName, name, sizeof gThreadName);}
thread_t kstub_current_thread(void) {return (thread_t)0x77;}

static int gPre, gPost, gPreFail, gPostFail, gStaticInits;
static char gKext[64];
void *OSMetaClass::preModLoad(const char *kext)
{
    gPre++;
    strlcpy(gKext, kext, sizeof gKext);
    return gPreFail ? nullptr : (void *)0x99;
}
IOReturn OSMetaClass::postModLoad(void *handle)
{
    CHECK(handle == (void *)0x99);
    gPost++;
    return gPostFail ? kIOReturnError : kIOReturnSuccess;
}
// The class initializer of PaganiAudioService.cpp, which the kernel build takes out of __mod_init_func: it may only run between
// preModLoad and postModLoad.
extern "C" void pagani_audio_service_static_init(void)
{
    CHECK(gPre == gPost + 1);
    gStaticInits++;
}

static int gBufferAllocs, gBufferFailAt = -1, gBuffersLive;
IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithOptions(task_t task, IOOptionBits options, vm_size_t capacity,
                                                                      vm_offset_t alignment)
{
    CHECK(task == kstub_kernel_task && alignment == 4096 && capacity % 4096 == 0);
    if (gBufferAllocs++ == gBufferFailAt) return nullptr;
    IOBufferMemoryDescriptor *m = new IOBufferMemoryDescriptor;
    m->kstub_bytes = aligned_alloc(4096, capacity);
    memset(m->kstub_bytes, 0xa5, capacity);   // what the allocator leaves: the engine clears it
    m->kstub_size = capacity;
    m->kstub_options = options;
    gBuffersLive++;
    return m;
}
IOBufferMemoryDescriptor::~IOBufferMemoryDescriptor()
{
    free(kstub_bytes);
    gBuffersLive--;
}

// The caller's memory for the host clock correction: one physical page of RAM at kRam, mapped in the caller at kUser.
alignas(4096) static uint8_t gPage[4096];
static const uint64_t kUser = 0x1a0000000ULL, kRam = 0x880000000ULL, kDevice = 0x0f100000ULL;
static task_t gOwner = (task_t)0x66;
static int gMapDevice, gUnmapped, gPhysToKv, gRangeFail, gRanges, gRangesLive, gPrepared, gCompleted;
static IOReturn gPrepareResult = kIOReturnSuccess;
static uint64_t gRangeAddress, gRangeLength;
static IOOptionBits gRangeOptions;
class KStubRange : public IOMemoryDescriptor {
public:
    ~KStubRange() override {gRangesLive--;}
};
IOMemoryDescriptor *IOMemoryDescriptor::withAddressRange(uint64_t address, uint64_t length, IOOptionBits options, task_t task)
{
    CHECK(task == gOwner);
    gRanges++;
    gRangeAddress = address;
    gRangeLength = length;
    gRangeOptions = options;
    if (gRangeFail) return nullptr;
    gRangesLive++;
    return new KStubRange;
}
IOReturn IOMemoryDescriptor::prepare(IODirection) {gPrepared++; return gPrepareResult;}
IOReturn IOMemoryDescriptor::complete(IODirection) {gCompleted++; return kIOReturnSuccess;}
int copyin(uint64_t uaddr, void *kaddr, size_t len)
{
    if (uaddr < kUser || uaddr + len > kUser + sizeof gPage) return 14;   // EFAULT
    memcpy(kaddr, gPage + (uaddr - kUser), len);
    return 0;
}
void *get_task_pmap(task_t task) {CHECK(task == gOwner); return (void *)0x5000;}
extern "C" uint64_t pmap_find_pa(void *pmap, uint64_t va)
{
    CHECK(pmap == (void *)0x5000);
    if (gUnmapped || va < kUser || va >= kUser + sizeof gPage) return 0;
    return (gMapDevice ? kDevice : kRam) + (va - kUser);
}
extern "C" boolean_t pmap_valid_address(uint64_t pa) {return pa >= kRam && pa < kRam + sizeof gPage;}
extern "C" vm_offset_t phystokv(uint64_t pa)
{
    gPhysToKv++;
    if (!pmap_valid_address(pa)) {
        fprintf(stderr, "phystokv(%#llx): not RAM, the kernel panics here\n", (unsigned long long)pa);
        abort();
    }
    return (vm_offset_t)(gPage + (pa - kRam));
}

static char gSelfName[17] = "audiomxd";
static int gSelfPid = 123;
extern "C" void proc_selfname(char *buf, int size) {strlcpy(buf, gSelfName, (size_t)size);}
extern "C" int proc_selfpid(void) {return gSelfPid;}

static IOService gResources;
static IOService *gPublished;
static int gNoResources, gAttachFail, gStartFail;
IOService *IOService::getResourceService() {return gNoResources ? nullptr : &gResources;}
bool IOService::init(OSDictionary *) {return true;}
bool IOService::attach(IOService *provider)
{
    if (gAttachFail) return false;
    kstub_provider = provider;
    retain();   // the registry keeps it
    return true;
}
void IOService::detach(IOService *provider) {CHECK(kstub_provider == provider); kstub_provider = nullptr; release();}
bool IOService::start(IOService *provider) {return provider && !(gStartFail && !dynamic_cast<IOUserClient *>(this));}
void IOService::registerService(IOOptionBits) {kstub_registered++; gPublished = this;}
bool IOService::terminate(IOOptionBits) {kstub_terminated++; return true;}
bool IOService::setProperty(const char *key, const char *value)
{
    if (!strcmp(key, "NativeSandboxGrantProcess")) strlcpy(kstub_grant, value, sizeof kstub_grant);
    else if (!strcmp(key, "IOUserClientClass")) strlcpy(kstub_user_client, value, sizeof kstub_user_client);
    else if (!strcmp(key, "PaganiAudioBackend")) strlcpy(kstub_backend, value, sizeof kstub_backend);
    else CHECK(!"unexpected string property");
    return true;
}
bool IOService::setProperty(const char *key, unsigned long long value, unsigned int bits)
{
    CHECK(!strcmp(key, "PaganiAudioABI") && bits == 32);
    kstub_abi = value;
    return true;
}
bool IOUserClient::initWithTask(task_t, void *, UInt32, OSDictionary *) {return true;}
IOReturn IOUserClient::clientClose() {return kIOReturnUnsupported;}
IOReturn IOUserClient::clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **) {return kIOReturnUnsupported;}
IOReturn IOUserClient::externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *)
{
    return kIOReturnUnsupported;
}

// ---- helpers ------------------------------------------------------------------------------------------------------------------------
// A fresh boot: every state of PaganiAudio.cpp and of the stand-ins back to what the kernel starts with.
static void reboot(void)
{
    for (IOBufferMemoryDescriptor *&m : gMemory) {
        if (m) m->release();
        m = nullptr;
    }
    if (gPublished && gPublished->kstub_provider) gPublished->detach(gPublished->kstub_provider);   // the registry lets it go
    gPublished = nullptr;
    memset((void *)&gEngine, 0, sizeof gEngine);
    gLock = nullptr;
    gStarted = gServing = gEvents = 0;
    gHostClock = 1;
    gOptions = gRounds = 0;
    gLogLevel = 1;
    memset(gLastOpener, 0, sizeof gLastOpener);
    gSummary = Summary{};
    gRoute = RouteLog{};
    gActuator = 0;
    gHapticsSustainMv = 0;
    gHapticsRate = PAGANI_AUDIO_HAPTICS_RATE;
    gHapticsCapMv = PAGANI_AUDIO_HAPTICS_CAP_MV;
    memset((void *)&gHaptics, 0, sizeof gHaptics);
    gMx = MxLog{};
    gVa = VaLog{};
    gHapticsLog = HapticsLog{};
    kstub_vib_begins = kstub_vib_ends = kstub_vib_writes = kstub_vib_begin_rc = kstub_vib_last_drain = kstub_vib_peak = 0;
    kstub_vib_rate = kstub_vib_mv = kstub_vib_samples = 0;
    memset((void *)pagani_audio_status, 0, sizeof pagani_audio_status);
    memset((void *)pagani_audio_clock, 0, sizeof pagani_audio_clock);
    kstub_line_count = 0;
    gTimebase = {625, 12};
    gNow = 1000000000ULL;
    gArgCount = gArgReads = gIOLogs = 0;
    gLockFail = gLockAllocs = gSleeps = gSleepsLeft = 0;
    gThreadFn = nullptr;
    gThreadStarts = gThreadFail = gThreadDeallocs = 0;
    gThreadName[0] = 0;
    gPre = gPost = gPreFail = gPostFail = gStaticInits = 0;
    gBufferAllocs = 0;
    gBufferFailAt = -1;
    gNoResources = gAttachFail = gStartFail = 0;
    CHECK(gBuffersLive == 0);
}

static int has_line(const char *prefix)
{
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i)
        if (!strncmp(kstub_lines[i], prefix, strlen(prefix))) return 1;
    return 0;
}

static int has_text(const char *text)
{
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i)
        if (strstr(kstub_lines[i], text)) return 1;
    return 0;
}

static unsigned lines_with(const char *prefix)
{
    unsigned n = 0;
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i) n += !strncmp(kstub_lines[i], prefix, strlen(prefix));
    return n;
}

static void entry_points_refuse(void)
{
    PaganiAudioClientState c = {0};
    for (uint32_t t = 0; t <= 3; ++t) CHECK(pagani_audio_memory(t) == nullptr);
    CHECK(pagani_audio_request(&c, PaganiAudioStartOutput) == -1 && c.bits == 0);
    CHECK(pagani_audio_set_backend(&pagani_audio_silent_backend) == -1 && pagani_audio_set_backend(nullptr) == -1);
    pagani_audio_wake();   // the lock may not exist: it must not be touched
    CHECK(gLockStorage.held == 0);
}

static PaganiAudioUserClient *open_client(const char *process, int pid, task_t task)
{
    strlcpy(gSelfName, process, sizeof gSelfName);
    gSelfPid = pid;
    PaganiAudioUserClient *client = OSTypeAlloc(PaganiAudioUserClient);
    CHECK(client->initWithTask(task, nullptr, 0, nullptr));
    CHECK(client->start(gPublished));
    return client;
}

static IOReturn call(PaganiAudioUserClient *client, uint32_t selector, uint32_t scalars = 0, uint64_t scalar = 0, uint32_t structure = 0)
{
    IOExternalMethodArguments args = {};
    args.selector = selector;
    args.scalarInput = &scalar;
    args.scalarInputCount = scalars;
    args.structureInputSize = structure;
    return client->externalMethod(selector, &args, nullptr, nullptr, nullptr);
}

// ---- tests ------------------------------------------------------------------------------------------------------------------------
static void test_disabled(void)
{
    // No pagani-audio, pagani-audio=0, or only the other pagani-audio-* arguments: one boot argument read, nothing else.
    const std::initializer_list<BootArg> cases[] = {{}, {{"pagani-audio", 0}}, {{"pagani-audio-log", 2}, {"pagani-audio-trace", 1}}};
    for (const auto &boot : cases) {
        reboot();
        bootArgs(boot);
        pagani_audio_start();
        CHECK(gArgReads == 1 && gLockAllocs == 0 && gBufferAllocs == 0 && gPre == 0 && gStaticInits == 0 && gThreadStarts == 0);
        CHECK(gPublished == nullptr && kstub_line_count == 0 && gIOLogs == 0 && gStarted == 0 && gServing == 0);
        int clean = 1;
        for (int32_t s : pagani_audio_status) clean &= s == 0;
        CHECK(clean && gEngine.ctl == nullptr);
        entry_points_refuse();
    }
}

static void expect_failure(int32_t step, const char *line)
{
    pagani_audio_start();
    CHECK(pagani_audio_status[0] == step && gServing == 0 && has_line(line) && gIOLogs == 1);
    entry_points_refuse();
    const unsigned lines = kstub_line_count;
    pagani_audio_start();   // once per boot: a second call changes nothing
    CHECK(pagani_audio_status[0] == step && kstub_line_count == lines);
}

static void test_failures(void)
{
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gLockFail = 1;
    expect_failure(-2, "audio: failed at memory (step 2)");
    CHECK(gBufferAllocs == 0 && gThreadStarts == 0 && gPre == 0);
    for (int at = 0; at < 3; ++at) {
        reboot();
        bootArgs({{"pagani-audio", 1}});
        gBufferFailAt = at;
        expect_failure(-2, "audio: failed at memory (step 2)");
        CHECK(gBufferAllocs == at + 1 && gThreadStarts == 0 && gPre == 0);
    }
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gTimebase = {0, 1};
    expect_failure(-3, "audio: failed at timebase (step 3)");
    CHECK(gPre == 0 && gThreadStarts == 0);
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gPreFail = 1;
    expect_failure(-4, "audio: failed at class registration (step 4)");
    CHECK(gStaticInits == 0 && gThreadStarts == 0);
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gPostFail = 1;
    expect_failure(-4, "audio: failed at class registration (step 4)");
    CHECK(gStaticInits == 1 && gThreadStarts == 0);
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gThreadFail = 1;
    expect_failure(-5, "audio: failed at thread (step 5)");
    CHECK(gPublished == nullptr);
    // The service cannot be published: the thread runs on (idle), every entry point refuses.
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gNoResources = 1;
    expect_failure(-6, "audio: failed at service attach (step 6)");
    CHECK(gThreadStarts == 1 && gPublished == nullptr);
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gAttachFail = 1;
    expect_failure(-6, "audio: failed at service attach (step 6)");
    reboot();
    bootArgs({{"pagani-audio", 1}});
    gStartFail = 1;
    expect_failure(-6, "audio: failed at service start (step 6)");
    CHECK(gPublished == nullptr);
}

static void start_service(std::initializer_list<BootArg> boot)
{
    reboot();
    bootArgs(boot);
    pagani_audio_start();
    CHECK(pagani_audio_status[0] == 6 && gServing == 1 && gPublished != nullptr);
}

static void test_start(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-audio-trace", 1}, {"pagani-audio-log", 2}});
    CHECK(pagani_audio_status[5] == PaganiAudioBackendSilent && pagani_audio_status[6] == 1 &&
          pagani_audio_status[7] == (PaganiAudioOptionTraceRouting | PaganiAudioOptionProbeRoute));
    // The control page and the rings: shared with user space, cleared, the timeline of the 19.2 MHz timer.
    const size_t sizes[3] = {4096, 65536 * 2 * 2, 65536 * 2};
    for (int i = 0; i < 3; ++i) {
        CHECK(gMemory[i] && gMemory[i]->kstub_size == sizes[i] && gMemory[i]->kstub_refs == 1);
        CHECK(gMemory[i]->kstub_options == (IOOptionBits)(kIODirectionInOut | kIOMemoryKernelUserShared));
    }
    const PaganiAudioControl *c = (const PaganiAudioControl *)gMemory[0]->getBytesNoCopy();
    CHECK(c->magic == PAGANI_AUDIO_MAGIC && c->ticks_per_frame_q32 == 400ULL << 32 &&
          c->options == (PaganiAudioOptionTraceRouting | PaganiAudioOptionProbeRoute));
    CHECK((c->state & (PaganiAudioOutputReady | PaganiAudioInputReady)) == (PaganiAudioOutputReady | PaganiAudioInputReady));
    CHECK(((const int16_t *)gMemory[1]->getBytesNoCopy())[0] == 0 && ((const int16_t *)gMemory[2]->getBytesNoCopy())[65535] == 0);
    // The classes are registered under the kernel's own name, the thread runs, the service carries the grant for the audio server.
    CHECK(gPre == 1 && gPost == 1 && gStaticInits == 1 && !strcmp(gKext, g_kernel_kmod_info.name));
    CHECK(gThreadStarts == 1 && gThreadDeallocs == 1 && gThreadFn == serviceThread);
    CHECK(!strcmp(gPublished->kstub_grant, "audiomxd,mediaserverd") && !strcmp(gPublished->kstub_user_client, "PaganiAudioUserClient"));
    CHECK(!strcmp(gPublished->kstub_backend, "silent") && gPublished->kstub_abi == PAGANI_AUDIO_VERSION);
    CHECK(gPublished->kstub_registered == 1 && gPublished->kstub_provider == &gResources && gPublished->kstub_refs == 1);
    CHECK(kstub_line_count == 1 && gIOLogs == 1 && !strcmp(kstub_lines[0], "audio: up mode 1 backend silent frame 400/1 ticks (q32 "
                                                           "19000000000) ring 65536 period 480 lead 14400 hostclock 1 options 3 log 2"));
    // Once per boot.
    pagani_audio_start();
    CHECK(gBufferAllocs == 3 && gThreadStarts == 1 && gPre == 1 && kstub_line_count == 1);
    // The arguments as given: no trace option, host clock off, log 0 (start line only); the route probe stays on.
    start_service({{"pagani-audio", 1}, {"pagani-audio-hostclock", 0}, {"pagani-audio-log", 0}});
    CHECK(gOptions == PaganiAudioOptionProbeRoute && gHostClock == 0 && gLogLevel == 0 && !pagani_audio_host_clock_allowed() &&
          pagani_audio_status[7] == PaganiAudioOptionProbeRoute);
    CHECK(has_line("audio: up mode 1 backend silent frame 400/1") && strstr(kstub_lines[0], "hostclock 0 options 2 log 0"));
    // pagani-audio-probe=0: no option at all (and any other value keeps it).
    start_service({{"pagani-audio", 1}, {"pagani-audio-probe", 0}});
    CHECK(gOptions == 0 && pagani_audio_status[7] == 0 && ((const PaganiAudioControl *)gMemory[0]->getBytesNoCopy())->options == 0);
    CHECK(strstr(kstub_lines[0], "options 0 log 1"));
    start_service({{"pagani-audio", 1}, {"pagani-audio-probe", 7}, {"pagani-audio-trace", 1}});
    CHECK(gOptions == (PaganiAudioOptionTraceRouting | PaganiAudioOptionProbeRoute));
}

static void test_clients(void)
{
    start_service({{"pagani-audio", 1}});
    PaganiAudioUserClient *a = open_client("audiomxd", 123, gOwner);
    CHECK(pagani_audio_status[1] == 1 && pagani_audio_status[4] == 123 && !strcmp(gLastOpener, "audiomxd"));
    CHECK(has_line("audio: open 1 by audiomxd pid 123"));
    // Only the PaganiAudio service is a provider (the open is counted at initWithTask, before start).
    PaganiAudioUserClient *stray = OSTypeAlloc(PaganiAudioUserClient);
    CHECK(stray->initWithTask(gOwner, nullptr, 0, nullptr) && !stray->start(&gResources) && pagani_audio_status[1] == 2);
    stray->release();
    // The three memories, retained for the caller, mapped read-write (options 0); nothing else.
    for (uint32_t t = 0; t < 3; ++t) {
        IOOptionBits options = 7;
        IOMemoryDescriptor *m = nullptr;
        CHECK(a->clientMemoryForType(t, &options, &m) == kIOReturnSuccess && options == 0 && m == gMemory[t] && m->kstub_refs == 2);
        m->release();
    }
    IOMemoryDescriptor *none = nullptr;
    IOOptionBits options = 0;
    CHECK(a->clientMemoryForType(3, &options, &none) == kIOReturnBadArgument && none == nullptr && pagani_audio_status[3] == 3);
    // Requests: per client, each wakes the service thread; malformed calls are refused.
    const int wakes = gLockStorage.wakeups;
    CHECK(call(a, PaganiAudioStartOutput) == kIOReturnSuccess && call(a, PaganiAudioStartInput) == kIOReturnSuccess);
    CHECK(gEngine.out_users == 1 && gEngine.in_users == 1 && gLockStorage.wakeups == wakes + 2 && !gLockStorage.held);
    CHECK(call(a, 5) == kIOReturnBadArgument && call(a, PaganiAudioStartOutput, 1, 0) == kIOReturnBadArgument);
    CHECK(call(a, PaganiAudioStopOutput, 0, 0, 4) == kIOReturnBadArgument && gEngine.out_users == 1);
    PaganiAudioUserClient *b = open_client("mediaserverd", 77, (task_t)0x67);
    CHECK(call(b, PaganiAudioStartOutput) == kIOReturnSuccess && gEngine.out_users == 2 && has_line("audio: open 3 by mediaserverd pid 77"));
    // a closes: only its own requests go; a request that still reaches it (another thread of its dying process) is refused.
    CHECK(a->clientClose() == kIOReturnSuccess && a->kstub_terminated == 1);
    CHECK(gEngine.out_users == 1 && gEngine.in_users == 0 && pagani_audio_status[2] == 1 && has_line("audio: close 1 by audiomxd"));
    CHECK(call(a, PaganiAudioStartOutput) == kIOReturnNotReady && call(a, PaganiAudioStartInput) == kIOReturnNotReady);
    CHECK(gEngine.out_users == 1 && gEngine.in_users == 0);
    CHECK(b->clientClose() == kIOReturnSuccess && gEngine.out_users == 0);
    a->release();
    b->release();
    // Open and close lines stop after 16 each (the boot log is shared with the crash records); the counts go on.
    const unsigned before = lines_with("audio: open ");
    for (int i = 0; i < 20; ++i) {
        PaganiAudioUserClient *c = open_client("audiomxd", 200 + i, gOwner);
        c->clientClose();
        c->release();
    }
    CHECK(pagani_audio_status[1] == 23 && pagani_audio_status[2] == 22 && lines_with("audio: open ") == 16 && before == 3);
    CHECK(lines_with("audio: close ") == 16);
}

// One or more rounds of the service thread, ended at its sleep.
static void rounds(int sleeps)
{
    gSleepsLeft = sleeps;
    if (!setjmp(gSleepEnd)) {
        serviceThread(nullptr, 0);
        CHECK(!"the service thread returned");
    }
}

static void test_service_thread(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-audio-log", 2}});
    PaganiAudioUserClient *a = open_client("audiomxd", 123, gOwner);
    // A request woke the thread before its first round: it runs a round, does not sleep (an event came), runs again, then sleeps until
    // the next period (at most 10 ms with output running).
    CHECK(call(a, PaganiAudioStartOutput) == kIOReturnSuccess);
    const unsigned lines = kstub_line_count;
    rounds(1);
    CHECK(!strcmp(gThreadName, "pagani-audio") && gEngine.running && gEngine.stats.out_starts == 1 && gSleeps == 1);
    CHECK(gLastWaitMs >= 1 && gLastWaitMs <= 10 && gEngine.stats.taken == 4);
    // pagani-audio-log=2: the round's summary.
    CHECK(kstub_line_count == lines + 2 && has_line("audio svc st=") && has_line("audio plug loads=0 opens=1/0 maps=0 by audiomxd"));
    // 25 ms later: the thread takes the periods that came due, sleeps again.
    gNow += 25 * 19200;
    rounds(1);
    CHECK(gEngine.stats.taken == 4 + 2 && gSleeps == 2);
    // The client goes: output keeps 5 s, then stops; idle the thread sleeps a second.
    CHECK(a->clientClose() == kIOReturnSuccess);
    rounds(1);
    CHECK(gEngine.running && gEngine.stop_pending);
    gNow += 6000ULL * 19200;
    rounds(1);
    CHECK(!gEngine.running && gEngine.stats.out_stops == 1);
    rounds(1);
    CHECK(gLastWaitMs == 1000);
    a->release();
}

static void test_summary(void)
{
    start_service({{"pagani-audio", 1}});
    PaganiAudioDiag *d = (PaganiAudioDiag *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_DIAG_OFFSET);
    unsigned n = kstub_line_count;
    summarize(1000);                       // nothing happened: no row
    CHECK(kstub_line_count == n);
    gEngine.stats.out_starts = 1;
    summarize(2000);                       // the first change: rows at once
    CHECK(kstub_line_count == n + 2 && gRounds == 1);
    gEngine.stats.out_starts = 2;
    summarize(20000);                      // within 30 s of the last: none
    CHECK(kstub_line_count == n + 2);
    summarize(32000);
    CHECK(kstub_line_count == n + 4 && gRounds == 2);
    gEngine.stats.taken = 1000;            // moving counters do not count at level 1
    summarize(70000);
    CHECK(kstub_line_count == n + 4);
    // After 12 rounds the spacing is 10 minutes.
    uint64_t ms = 70000;
    while (gRounds < kEarlyRounds) {
        gEngine.stats.out_starts++;
        ms += 30000;
        summarize(ms);
    }
    gEngine.stats.out_starts++;
    n = kstub_line_count;
    summarize(ms + 300000);
    CHECK(kstub_line_count == n);
    summarize(ms + 600000);
    CHECK(kstub_line_count == n + 2);
    // The plug-in's rows: IO timing once it writes, its unanswered properties, the host clock request (once per address).
    d->writes = 5;
    d->count = 3;
    d->selector[0] = 'zzzz';
    d->where[0] = 2u << 24 | 'o';
    d->selector[1] = 'accs';
    d->where[1] = 2u << 24 | 'g';
    d->selector[2] = 0x01020304;
    d->where[2] = 3u << 24 | 0x05;
    pagani_audio_clock[1] = 0x1a0000040ULL;
    n = kstub_line_count;
    gLogLevel = 2;
    summarize(ms + 610000);
    CHECK(kstub_line_count == n + 5 && has_line("audio io frames=0 ahead=0..0") && has_line("audio clock step=0 va=1a0000040"));
    CHECK(has_line("audio unk n=3 zzzz@2o accs@2g ....@3."));
    gEngine.stats.taken++;                 // level 2: moving counters count, every 10 s
    n = kstub_line_count;
    summarize(ms + 615000);
    CHECK(kstub_line_count == n);
    summarize(ms + 620000);
    CHECK(kstub_line_count == n + 4);      // the clock row only once per address
    // Whatever the plug-in writes into its half of the page, the rows stay within their buffers.
    memset(d, 0xff, sizeof *d);
    gEngine.stats.out_starts++;
    summarize(ms + 640000);
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i) CHECK(strlen(kstub_lines[i]) < 192);
    CHECK(has_line("audio unk n=4294967295 "));
    // Level 0: no rows at all.
    gLogLevel = 0;
    n = kstub_line_count;
    gEngine.stats.out_starts++;
    summarize(ms + 700000);
    CHECK(kstub_line_count == n);
}

// pagani-audio-log=2 while audio IO runs: the moving counters differ in every round, so level 2 writes a round every 10 s, but only for
// its first 16 rounds; then level 1's spacing holds (10 min), so the rows of a diagnosis run stay a small part of the 32 KiB events ring
// that BootProgress shares with the boot lines (unbounded: 360 rounds an hour, the ring overwritten within minutes of IO).
static void test_summary_bound(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-audio-log", 2}});
    PaganiAudioDiag *d = (PaganiAudioDiag *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_DIAG_OFFSET);
    // Rows as long as on the device: counters of a long run, three unanswered properties, the host clock row once.
    gEngine.stats.taken = gEngine.stats.played = gEngine.stats.captured = 1000000;
    d->count = 3;
    d->selector[0] = 'zzzz';
    d->selector[1] = 'accs';
    d->selector[2] = 'vcsp';
    d->where[0] = d->where[1] = d->where[2] = 2u << 24 | 'o';
    d->ahead_min = -1024;
    d->ahead_max = 15360;
    d->skew_min = -480;
    d->skew_max = 480;
    d->io_tc = 1;
    pagani_audio_clock[1] = 0x1a0000040ULL;
    const unsigned first = kstub_line_count;
    uint64_t at[32] = {}, bytes = 0, run = 0;
    uint32_t rounds = 0;
    for (uint64_t ms = 1000; ms <= 3600000; ms += 1000) {   // an hour of IO, the service thread's round every second
        gEngine.stats.taken += 100;
        gEngine.stats.played += 100;
        gEngine.stats.captured += 100;
        d->writes += 47;
        d->frames_total += 48000;
        const unsigned lines = kstub_line_count;
        summarize(ms);
        // What a row takes in the ring: its text, BootProgress's 33-byte record header and the newline (RamoopsLog.h).
        for (unsigned i = lines; i < kstub_line_count && i < 256; ++i) bytes += strlen(kstub_lines[i]) + 34;
        if (gRounds != rounds) {
            CHECK(gRounds == rounds + 1 && rounds < 32);
            at[rounds++] = ms;
        }
        if (ms == 600000) run = bytes;                     // a diagnosis run of 10 minutes
    }
    // 16 rounds 10 s apart, then one every 10 min: 21 in the hour, each its svc, plug, io and unk rows (and the clock row once). Over
    // 500 bytes a round: a 10-minute run takes under a third of the ring, the hour under half (unbounded, 10 minutes nearly filled it).
    for (uint32_t i = 1; i < rounds; ++i) CHECK(at[i] - at[i - 1] == (i < 16 ? 10000u : 600000u));
    CHECK(rounds == 16 + 5 && at[0] == 1000 && kstub_line_count - first == 4 * rounds + 1 && kstub_line_count < 256);
    CHECK(bytes / rounds > 500 && run < 32768 / 3 && bytes < 32768 / 2);
    // The early rounds of level 1 (12, 30 s apart) are spent too: a change for the observer waits for the 10 min, then comes.
    gEngine.stats.out_starts++;
    summarize(3600000 + 30000);
    CHECK(gRounds == rounds);
    summarize(at[rounds - 1] + 600000);
    CHECK(gRounds == rounds + 1);
}

// The route probe's row: written by the plug-in into its section of the page, printed when it changed, 5 s apart at least, 24 per boot.
static void test_route(void)
{
    start_service({{"pagani-audio", 1}});
    PaganiAudioProbe *p = (PaganiAudioProbe *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_PROBE_OFFSET);
    unsigned n = kstub_line_count;
    summarize(1000);                        // no probe yet (seq 0): nothing
    CHECK(kstub_line_count == n && lines_with("audio route") == 0);
    p->seq = 1;                             // being written: nothing either
    p->devices = 4;
    summarize(2000);
    CHECK(kstub_line_count == n);
    p->reason = PaganiAudioProbeTimer;
    p->va_plugin = 37;
    p->vad = 58;
    p->ours = 44;
    p->ours_vold = 1;
    p->ours_running = 0;
    p->vcsp_status = 0;
    p->vcsp = 1;
    p->category = 'cmed';
    p->mode = 'imdf';
    p->ports_status = 0;
    p->out_ports = 1;
    p->port_type[0] = 'ptbo';
    p->vad_volume_milli = 500;
    p->vol_queries = 12;
    p->seq = 2;
    summarize(3000);
    CHECK(kstub_line_count == n + 1 && !strcmp(kstub_lines[n], "audio route #1 r1 dev=4 va=37 vad=58 ours=44 vold=1 run=0 vcsp=1/0 vc=1 "
                                                                 "cat=cmed mode=imdf out=1 ptbo .... ports=0 vol=500 q=12"));
    // The same answers again (another probe, more reads of the volume control): no row.
    p->seq = 4;
    p->reason = PaganiAudioProbeVolume;
    p->vol_queries = 40;
    summarize(10000);
    CHECK(kstub_line_count == n + 1);
    // A change: a row, but not within 5 s of the last one.
    p->seq = 6;
    p->ours_vold = 0;
    p->vcsp = 0;
    p->out_ports = 0;
    p->port_type[0] = 0;
    summarize(7000);
    CHECK(kstub_line_count == n + 1);
    summarize(8000);
    CHECK(kstub_line_count == n + 2 && has_line("audio route #3 r2 dev=4 va=37 vad=58 ours=44 vold=0 run=0 vcsp=0/0 vc=0 cat=cmed "
                                                  "mode=imdf out=0 .... .... ports=0 vol=500 q=40"));
    // vc is 'vcsp' as MediaExperience takes it, (status | value) != 0: a failed read ('who?') counts as a volume control, the raw pair
    // stays in the row. The rationing still compares the raw fields: a new raw pair is a row even when vc stays, the same pair is none.
    p->seq = 8;
    p->vcsp_status = (int32_t)'who?';
    summarize(14000);
    CHECK(kstub_line_count == n + 3 && has_line("audio route #4 r2 dev=4 va=37 vad=58 ours=44 vold=0 run=0 vcsp=0/2003332927 vc=1 "
                                                  "cat=cmed mode=imdf out=0 .... .... ports=0 vol=500 q=40"));
    p->seq = 10;
    p->vcsp_status = (int32_t)'!obj';       // another failure: still vc=1, but the raw status changed, so a row as before
    summarize(20000);
    CHECK(kstub_line_count == n + 4 && has_line("audio route #5 r2 dev=4 va=37 vad=58 ours=44 vold=0 run=0 vcsp=0/560947818 vc=1 "));
    p->seq = 12;
    p->vcsp_status = 0;
    p->vcsp = 2;                            // any value but 0 read without error
    summarize(26000);
    CHECK(kstub_line_count == n + 5 && has_line("audio route #6 r2 dev=4 va=37 vad=58 ours=44 vold=0 run=0 vcsp=2/0 vc=1 "));
    p->seq = 14;
    summarize(32000);                       // the same raw pair again: no row
    CHECK(kstub_line_count == n + 5);
    // 24 rows per boot at most.
    uint64_t ms = 32000;
    for (uint32_t i = 0; i < 40; ++i) {
        p->seq += 2;
        p->devices = 100 + i;
        ms += 6000;
        summarize(ms);
    }
    CHECK(lines_with("audio route") == kRouteRows && gRoute.rows == kRouteRows);
    // Whatever the plug-in writes, the row stays within its buffer.
    reboot();
    start_service({{"pagani-audio", 1}});
    p = (PaganiAudioProbe *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_PROBE_OFFSET);
    memset(p, 0xff, sizeof *p);
    p->seq = 0xfffffffe;
    summarize(1000);
    CHECK(lines_with("audio route #2147483647 r4294967295 ") == 1);
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i) CHECK(strlen(kstub_lines[i]) < 192);
    // Log level 0: not even the route row.
    start_service({{"pagani-audio", 1}, {"pagani-audio-log", 0}});
    p = (PaganiAudioProbe *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_PROBE_OFFSET);
    p->seq = 2;
    p->vcsp = 1;
    n = kstub_line_count;
    summarize(5000);
    CHECK(kstub_line_count == n);
}

static void put_double(uint64_t offset, double value) {memcpy(gPage + offset, &value, sizeof value);}
static double get_double(uint64_t offset)
{
    double value;
    memcpy(&value, gPage + offset, sizeof value);
    return value;
}

static void test_host_clock(void)
{
    start_service({{"pagani-audio", 1}});
    PaganiAudioUserClient *a = open_client("audiomxd", 123, gOwner);
    const uint64_t at = 0x40, address = kUser + at;
    // CoreAudio's 24 MHz becomes the board's 19.2 MHz, on the shared page through the physical aperture.
    put_double(at, 24000000.0);
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnSuccess && get_double(at) == 19200000.0);
    CHECK(gRangeAddress == address && gRangeLength == 8 && gRangeOptions == kIODirectionOut && gPrepared == 1 && gCompleted == 1);
    CHECK(gRangesLive == 0 && pagani_audio_clock[0] == 0 && pagani_audio_clock[1] == address && pagani_audio_clock[2] == kRam + at);
    CHECK(pagani_audio_clock[3] == 0x4176e36000000000ULL);
    // Asked again (a new process of the audio server): the board's value stays.
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnSuccess && get_double(at) == 19200000.0);
    // Any other value is no host clock constant: left alone.
    put_double(at, 48000.0);
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnUnsupported && get_double(at) == 48000.0 && pagani_audio_clock[0] == 4);
    // Outside the shared region or misaligned: refused before any memory is touched.
    const int ranges = gRanges;
    const uint64_t bad[] = {address + 4, SHARED_REGION_BASE_ARM64 - 8, SHARED_REGION_BASE_ARM64 + SHARED_REGION_SIZE_ARM64,
                            0x3fffffff8ULL, 0};
    for (uint64_t b : bad) CHECK(call(a, PaganiAudioSetHostClock, 1, b) == kIOReturnBadArgument);
    CHECK(gRanges == ranges && pagani_audio_clock[0] == 1);
    // The region's last word passes the range check (then the caller cannot read it here).
    CHECK(call(a, PaganiAudioSetHostClock, 1, SHARED_REGION_BASE_ARM64 + SHARED_REGION_SIZE_ARM64 - 8) == kIOReturnNotReadable);
    CHECK(gRanges == ranges + 1 && pagani_audio_clock[0] == 3 && gRangesLive == 0);
    // A device page mapped into the caller that holds the value: never through phystokv (it panics outside the RAM), never written.
    put_double(at, 24000000.0);
    gMapDevice = 1;
    const int kv = gPhysToKv;
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnNotWritable && gPhysToKv == kv && get_double(at) == 24000000.0);
    CHECK(pagani_audio_clock[0] == 4 && pagani_audio_clock[2] == kDevice + at && gRangesLive == 0 && gCompleted == gPrepared);
    gMapDevice = 0;
    // Not mapped, not prepared, no descriptor: each recorded, nothing written, every prepare completed.
    gUnmapped = 1;
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnNoResources && get_double(at) == 24000000.0);
    gUnmapped = 0;
    gPrepareResult = kIOReturnNoMemory;
    const int completed = gCompleted;
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnNoMemory && gCompleted == completed && gRangesLive == 0);
    gPrepareResult = kIOReturnSuccess;
    gRangeFail = 1;
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnNoMemory && pagani_audio_clock[0] == 1);
    gRangeFail = 0;
    // The selector takes exactly one scalar.
    CHECK(call(a, PaganiAudioSetHostClock) == kIOReturnBadArgument && call(a, PaganiAudioSetHostClock, 1, address, 8) == kIOReturnBadArgument);
    CHECK(get_double(at) == 24000000.0);
    // pagani-audio-hostclock=0: nothing is looked at.
    a->clientClose();
    a->release();
    start_service({{"pagani-audio", 1}, {"pagani-audio-hostclock", 0}});
    a = open_client("audiomxd", 123, gOwner);
    const int before = gRanges;
    CHECK(call(a, PaganiAudioSetHostClock, 1, address) == kIOReturnUnsupported && gRanges == before && get_double(at) == 24000000.0);
    a->clientClose();
    a->release();
}

// ---- revision 2 -------------------------------------------------------------------------------------------------------------------
static const PaganiAudioRevision *revision(void)
{
    return (const PaganiAudioRevision *)((const uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_REVISION_OFFSET);
}

static void test_revision2_options(void)
{
    // Every service of revision 2 says so; without the actuator it offers no actuator geometry, memory or selectors.
    start_service({{"pagani-audio", 1}});
    CHECK(revision()->magic == PAGANI_AUDIO_REVISION_MAGIC && revision()->revision == 2 && revision()->actuator_frames == 0);
    CHECK(gBufferAllocs == 3 && gMemory[3] == nullptr && !pagani_audio_actuator_enabled());
    // pagani-audio-mx=1, pagani-audio-srzl=1: their option bits, nothing else changes (no line beyond the start line).
    start_service({{"pagani-audio", 1}, {"pagani-audio-mx", 1}, {"pagani-audio-srzl", 1}});
    CHECK(gOptions == (PaganiAudioOptionProbeRoute | PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair) && gBufferAllocs == 3);
    CHECK(((const PaganiAudioControl *)gMemory[0]->getBytesNoCopy())->options == gOptions && kstub_line_count == 1);
    CHECK(strstr(kstub_lines[0], "options e log 1"));
    // pagani-audio-srzl by its bits: 1 the repair, as ever; 2 VirtualAudio's actuator settings alone; 3 both; nothing else moves.
    start_service({{"pagani-audio", 1}, {"pagani-audio-srzl", 2}});
    CHECK(gOptions == (PaganiAudioOptionProbeRoute | PaganiAudioOptionVaSettings) && gBufferAllocs == 3);
    start_service({{"pagani-audio", 1}, {"pagani-audio-srzl", 3}});
    CHECK(gOptions == (PaganiAudioOptionProbeRoute | PaganiAudioOptionSrzlRepair | PaganiAudioOptionVaSettings) && gBufferAllocs == 3);
    start_service({{"pagani-audio", 1}, {"pagani-audio-srzl", 1}});
    CHECK(gOptions == (PaganiAudioOptionProbeRoute | PaganiAudioOptionSrzlRepair));
    start_service({{"pagani-audio", 1}, {"pagani-audio-srzl", 4}});
    CHECK(gOptions == PaganiAudioOptionProbeRoute);
    start_service({{"pagani-audio", 1}, {"pagani-audio-mx", 1}, {"pagani-audio-srzl", 1}});
    PaganiAudioUserClient *a = open_client("audiomxd", 123, gOwner);
    IOMemoryDescriptor *m = nullptr;
    IOOptionBits options = 0;
    CHECK(a->clientMemoryForType(PaganiAudioMemoryActuator, &options, &m) == kIOReturnBadArgument && m == nullptr);
    CHECK(call(a, PaganiAudioStartActuator) == kIOReturnBadArgument && call(a, PaganiAudioStopActuator) == kIOReturnBadArgument);
    PaganiAudioClientState c = {0};
    CHECK(pagani_audio_request(&c, PaganiAudioStartActuator) == -1 && gEngine.act_users == 0);
    a->clientClose();
    a->release();
    // pagani-haptics-ios without the vibrator running (absent, or its dry run 2): not offered, said once in the log.
    for (unsigned haptics : {0u, 2u}) {
        if (haptics) start_service({{"pagani-audio", 1}, {"pagani-haptics-ios", 1}, {"pagani-haptics", haptics}});
        else start_service({{"pagani-audio", 1}, {"pagani-haptics-ios", 1}});
        CHECK(!gActuator && !(gOptions & PaganiAudioOptionActuator) && gBufferAllocs == 3 && revision()->actuator_frames == 0);
        CHECK(has_line("audio: haptics actuator not offered: pagani-haptics-ios=1 needs 1 or 2 with pagani-haptics=1"));
        CHECK(!pagani_audio_actuator_enabled());
    }
    // A stage PaganiHaptics refuses (HapticsIos.c, pgh_ios_plan): no node, so no device either.
    start_service({{"pagani-audio", 1}, {"pagani-haptics-ios", 3}, {"pagani-haptics", 1}});
    CHECK(!gActuator && !(gOptions & PaganiAudioOptionActuator) && gBufferAllocs == 3 && revision()->actuator_frames == 0);
    CHECK(has_line("audio: haptics actuator not offered: pagani-haptics-ios=3 needs 1 or 2 with pagani-haptics=1"));
}

static void put_actuator_sound(uint64_t from, uint32_t frames)
{
    PaganiAudioActuatorControl *a = (PaganiAudioActuatorControl *)gMemory[3]->getBytesNoCopy();
    int16_t *ring = (int16_t *)((uint8_t *)a + PAGANI_AUDIO_ACT_HEADER_BYTES);
    for (uint32_t i = 0; i < frames; ++i) ring[(from + i) & (PAGANI_AUDIO_ACT_RING_FRAMES - 1)] = (int16_t)((i / 141) % 2 ? 16384 : -16384);
    a->written = from + frames;
    a->written_host = gNow;
    a->sound_frame = from;
    a->sound_host = gNow;
}

static void test_actuator(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-haptics-ios", 2}, {"pagani-haptics", 1}, {"pagani-haptics-vmax-mv", 3000},
                   {"pagani-audio-haptics-rate", 16000}, {"pagani-audio-haptics-sustain-mv", 1500}});
    CHECK(pagani_audio_actuator_enabled() && gHaptics.sustain_mv == 1500);   // what PaganiHaptics asks before it makes /product/haptics
    // The fourth memory: the actuator's header and ring, shared like the others, on the main timeline; the revision block says so.
    CHECK(gActuator && (gOptions & PaganiAudioOptionActuator) && gBufferAllocs == 4 && gMemory[3]->kstub_size == PAGANI_AUDIO_ACT_BYTES);
    CHECK(gMemory[3]->kstub_options == (IOOptionBits)(kIODirectionInOut | kIOMemoryKernelUserShared));
    const PaganiAudioActuatorControl *h = (const PaganiAudioActuatorControl *)gMemory[3]->getBytesNoCopy();
    CHECK(h->magic == PAGANI_AUDIO_ACT_MAGIC && h->ring_frames == PAGANI_AUDIO_ACT_RING_FRAMES && h->ticks_per_frame_q32 == 400ULL << 32);
    CHECK(h->anchor_host == ((const PaganiAudioControl *)gMemory[0]->getBytesNoCopy())->out_anchor_host);
    CHECK(revision()->actuator_frames == PAGANI_AUDIO_ACT_RING_FRAMES && revision()->actuator_rate == 48000);
    CHECK(kstub_line_count == 2 && has_line("audio: haptics actuator offered: ring 16384 frames, fifo 16000 Hz, full scale 7450 mV, cap 3000 mV, "
                                            "sustained 1500 mV"));
    // The user client: memory type 3, the actuator's selectors, released at close.
    PaganiAudioUserClient *a = open_client("audiomxd", 123, gOwner);
    IOMemoryDescriptor *m = nullptr;
    IOOptionBits options = 7;
    CHECK(a->clientMemoryForType(PaganiAudioMemoryActuator, &options, &m) == kIOReturnSuccess && m == gMemory[3] && options == 0);
    m->release();
    CHECK(call(a, PaganiAudioStartActuator) == kIOReturnSuccess && gEngine.act_users == 1 && gEngine.out_users == 0);
    // The service thread's round runs the bridge: 2 ms rounds while the actuator runs.
    rounds(1);
    CHECK(gHaptics.running && gLastWaitMs == PAGANI_AUDIO_HAPTICS_POLL_MS);
    // A sound written ahead of now comes due: the vibrator gets a stream at the asked rate and the vibrator's cap.
    const uint64_t frame = pagani_audio_engine_frame_at(&gEngine, gNow);
    put_actuator_sound(frame + 200, 4800);
    for (int i = 0; i < 60; ++i) {
        gNow += 2 * 19200;
        rounds(1);
    }
    CHECK(kstub_vib_begins == 1 && kstub_vib_rate == 16000 && kstub_vib_mv == 3000 && kstub_vib_samples > 1000);
    // 0.5 * 127 * 7450 / 3000 = 157.7: clipped at 127.
    CHECK(kstub_vib_peak == 127 && gHaptics.stats.clips > 0 && gHaptics.stats.latency_us >= 0);
    // Its rows: the bridge's and the plug-in's counters.
    CHECK(has_line("audio haptics runs=1/0 io=0/0 frames=") && has_line("audio haptics plug loads=0 vad=0 hog=0 writes=0"));
    CHECK(has_text(" fifo=16000 cap=3000 lim=0"));
    // The client closes: its wish goes, the run ends after the keep time, the stream drained.
    CHECK(a->clientClose() == kIOReturnSuccess && gEngine.act_users == 0);
    for (int i = 0; i < 10; ++i) {
        gNow += 2 * 19200;
        rounds(1);
    }
    CHECK(!gHaptics.running && gHaptics.stats.run_ends == 1 && kstub_vib_ends >= 1 && kstub_vib_last_drain == 1);
    CHECK(call(a, PaganiAudioStartActuator) == kIOReturnNotReady);   // a closed client's request
    a->release();
    rounds(1);
    CHECK(gLastWaitMs == 1000);
    // The vibrator refuses (not running): counted, the audio service goes on.
    start_service({{"pagani-audio", 1}, {"pagani-haptics-ios", 1}, {"pagani-haptics", 1}});
    CHECK(gHapticsCapMv == PAGANI_AUDIO_HAPTICS_CAP_MV && gHaptics.rate == 8000 && gHaptics.sustain_mv == PAGANI_AUDIO_HAPTICS_SUSTAIN_MV);
    a = open_client("audiomxd", 123, gOwner);
    kstub_vib_begin_rc = -19;
    CHECK(call(a, PaganiAudioStartActuator) == kIOReturnSuccess);
    rounds(1);
    put_actuator_sound(pagani_audio_engine_frame_at(&gEngine, gNow) + 200, 4800);
    for (int i = 0; i < 60; ++i) {
        gNow += 2 * 19200;
        rounds(1);
    }
    CHECK(kstub_vib_begins == 1 && gHaptics.stats.begin_failed == 1 && gHaptics.stats.last_rc == -19 && kstub_vib_writes == 0);
    a->clientClose();
    a->release();
}

static void test_mx_rows(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-audio-mx", 1}});
    PaganiAudioMx *m = (PaganiAudioMx *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_MX_OFFSET);
    unsigned n = kstub_line_count;
    summarize(1000);                        // nothing written yet
    CHECK(kstub_line_count == n);
    m->progress = 0x3ff | 2u << PaganiAudioMxFilterShift;
    m->acq_ms = 3;
    m->srzl_state = PaganiAudioSrzlWaiting;
    m->gen = 1;
    summarize(2000);                        // just written: wait one round
    CHECK(kstub_line_count == n);
    summarize(3000);
    CHECK(kstub_line_count == n + 1 && has_line("audio mx p=0x23ff filt=2 acq=3 srzl=2 st=0/0 after=- filt=0>0 n=0"));
    summarize(9000);                        // unchanged: no row
    CHECK(kstub_line_count == n + 1);
    m->srzl_state = PaganiAudioSrzlDone;
    m->serial_after = 'S';
    m->filter_before = 2;
    m->filter_after = 3;
    m->attempts = 1;
    m->gen = 2;
    summarize(10000);
    summarize(11000);
    CHECK(kstub_line_count == n + 2 && has_line("audio mx p=0x23ff filt=2 acq=3 srzl=3 st=0/0 after=S filt=2>3 n=1"));
    // A value in the block without the option changes nothing in the row's text; with the option (bit 1 alone also opens the row) it is shown.
    start_service({{"pagani-audio", 1}, {"pagani-audio-srzl", 2}});
    PaganiAudioMx *s = (PaganiAudioMx *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_MX_OFFSET);
    n = kstub_line_count;
    s->va_settings = PaganiAudioVaSettingsGiven;
    s->gen = 1;
    summarize(40000);
    summarize(41000);
    CHECK(kstub_line_count == n + 1 && has_line("audio mx p=0x0000 filt=0 acq=0 srzl=0 st=0/0 after=- filt=0>0 n=0 va=1"));
    start_service({{"pagani-audio", 1}, {"pagani-audio-mx", 1}});
    m = (PaganiAudioMx *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_MX_OFFSET);
    n = kstub_line_count;
    m->va_settings = PaganiAudioVaSettingsGiven;
    m->gen = 1;
    summarize(50000);
    summarize(51000);
    CHECK(kstub_line_count == n + 1 && has_line("audio mx p=0x0000 filt=0 acq=0 srzl=0 st=0/0 after=- filt=0>0 n=0"));
    CHECK(!strstr(kstub_lines[n], " va="));
    // Whatever the plug-in writes, the row stays within its buffer; without the options no row.
    memset((void *)m, 0xff, sizeof *m);
    summarize(20000);
    summarize(30000);
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i) CHECK(strlen(kstub_lines[i]) < 192);
    start_service({{"pagani-audio", 1}});
    m = (PaganiAudioMx *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_MX_OFFSET);
    m->gen = 5;
    n = kstub_line_count;
    summarize(1000);
    summarize(2000);
    CHECK(kstub_line_count == n);
}

static void test_vaerr_rows(void)
{
    start_service({{"pagani-audio", 1}, {"pagani-audio-trace", 1}});
    PaganiAudioTrace *tr = (PaganiAudioTrace *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_TRACE_OFFSET);
    // The plug-in's own property accesses (kinds 0 and 1) are not the routing layer's failures: no row.
    tr->entry[0][0] = 'nsrt';
    tr->entry[0][1] = 2u << 24 | 'g' << 16 | 1;
    tr->count = 1;
    unsigned n = kstub_line_count;
    summarize(1000);
    CHECK(kstub_line_count == n);
    // A failing call of the routing layer (kind 4) and a throw (kind 8, its offset in VirtualAudio).
    tr->entry[1][0] = 'vcsp';
    tr->entry[1][1] = 58u << 24 | 'o' << 16 | 4 | 2;
    tr->entry[2][0] = 0x232ac;
    tr->entry[2][1] = 'g' << 16 | 8 | 2;
    tr->count = 3;
    summarize(2000);
    CHECK(kstub_line_count == n + 1 && has_line("audio vaerr n=2 vcsp@58o T+0x232ac"));
    summarize(20000);                        // no more: no row
    CHECK(kstub_line_count == n + 1);
    // 8 rows per boot at most, 10 s apart; garbage stays within the buffer.
    for (uint32_t i = 3; i < 40; ++i) {
        tr->entry[i][0] = 0x9f530 + i;
        tr->entry[i][1] = 8 | 2;
        tr->count = i + 1;
        summarize(20000 + 11000ull * i);
    }
    CHECK(lines_with("audio vaerr") == 8);
    memset(tr, 0xff, sizeof *tr);
    gVa = VaLog{};
    summarize(900000);
    for (unsigned i = 0; i < kstub_line_count && i < 256; ++i) CHECK(strlen(kstub_lines[i]) < 192);
    // Without pagani-audio-trace no row.
    start_service({{"pagani-audio", 1}});
    tr = (PaganiAudioTrace *)((uint8_t *)gMemory[0]->getBytesNoCopy() + PAGANI_AUDIO_TRACE_OFFSET);
    tr->entry[0][0] = 0x232ac;
    tr->entry[0][1] = 8 | 2;
    tr->count = 1;
    n = kstub_line_count;
    summarize(1000);
    CHECK(kstub_line_count == n);
}

int main(void)
{
    strlcpy(g_kernel_kmod_info.name, "__kernel__", sizeof g_kernel_kmod_info.name);
    static_assert(SHARED_REGION_BASE_ARM64 == 0x180000000ULL && SHARED_REGION_SIZE_ARM64 == 0x180000000ULL, "xnu-10063 arm64 region");
    test_disabled();
    test_failures();
    test_start();
    test_clients();
    test_service_thread();
    test_summary();
    test_summary_bound();
    test_route();
    test_host_clock();
    test_revision2_options();
    test_actuator();
    test_mx_rows();
    test_vaerr_rows();
    reboot();
    CHECK(gBuffersLive == 0);
    printf("PASS glue: %u checks (start, failures, clients, service thread, boot log, host clock; revision 2: options, actuator memory, "
           "requests and bridge rounds, mx, vaerr and haptics rows)\n", checks);
    return 0;
}
