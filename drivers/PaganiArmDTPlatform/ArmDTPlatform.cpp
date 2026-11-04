// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Generic AArch64 Darwin platform expert for a supplied Apple-format device
// tree. It derives from XNU's real IODTPlatformExpert and relies on the
// platform's own DT root name ("device-tree") for matching. No board name,
// SoC address or vendor identity is compiled into this source; the default-off
// calendar of the OnePlus 13T (pagani-rtc) keeps its addresses in PlatformRtc.h,
// and its restart and power off (pagani-halt, PlatformHalt.h) go through
// BootProgress.
//
// The DT must contain the nodes XNU's IODT platform path consumes:
//   /chosen with a CHRP NVRAM proxy (nvram-bank-size, nvram-proxy-data)
//   /options, /cpus, /arm-io and /defaults
// The NVRAM proxy is required so the real IODTNVRAM is published before any
// security initialization. This driver does not fabricate NVRAM or suppress
// a missing one; absent inputs make configure() fail.

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>

#include "PlatformHalt.h"
#include "PlatformRtc.h"

// Observation counters describe actual published nubs, not started drivers.
extern "C" {
volatile uint32_t armdt_platform_configured = 0;
volatile uint32_t armdt_platform_child_count = 0;
volatile uint32_t armdt_platform_cpu_count = 0;
volatile uint32_t armdt_platform_bus_count = 0;
volatile uint32_t armdt_platform_defaults_count = 0;
volatile uint32_t armdt_platform_nvram_proxy = 0;
volatile uint32_t armdt_platform_device_count = 0;
}

// BootProgress's log and reset path. The defaults stand in on a kernel whose
// BootProgress lacks them: nothing is logged, and haltRestart falls back on
// the plain PSCI reset.
extern "C" {
void boot_progress_trace (const char *text);
void ramoops_log_restart (const char *reason);
void ramoops_log_power_off (const char *reason);
__attribute__((weak)) void boot_progress_trace (const char *) {}
__attribute__((weak)) void ramoops_log_restart (const char *) {}
__attribute__((weak)) void ramoops_log_power_off (const char *) {}
}

static_assert (unsigned (PHALT_TYPE_HALT) == kPEHaltCPU && unsigned (PHALT_TYPE_RESTART) == kPERestartCPU &&
               unsigned (PHALT_TYPE_UPS_HALT) == kPEUPSDelayHaltCPU &&
               unsigned (PHALT_TYPE_PANIC_RESTART) == kPEPanicRestartCPU, "PlatformHalt.h follows IOPlatformExpert.h");

class ArmDTPlatform : public IODTPlatformExpert {
  OSDeclareDefaultStructors (ArmDTPlatform)
public:
  bool start (IOService *provider) override;
  bool configure (IOService *provider) override;
  const char *deleteList (void) override;
  const char *excludeList (void) override;
  int haltRestart (unsigned int type) override;
  long getGMTTimeOfDay (void) override;
  void setGMTTimeOfDay (long secs) override;
private:
  unsigned haltMode;
  bool hasNVRAMProxy (IOService *provider);
  bool createDeviceNubs (IOService *provider);
};

OSDefineMetaClassAndStructors (ArmDTPlatform, IODTPlatformExpert)

// Serialized empty name lists: XNU's processTopLevel() treats a null pointer
// as "every entry" (remove every child), not as "nothing". An empty list
// genuinely selects no entry, so the supplied device tree is preserved.
const char *ArmDTPlatform::deleteList (void)
{
  return "()";
}

const char *ArmDTPlatform::excludeList (void)
{
  return "()";
}

bool ArmDTPlatform::hasNVRAMProxy (IOService *provider)
{
  IORegistryEntry *chosen = provider->childFromPath ("chosen", gIODTPlane);
  if (chosen == nullptr) {
    return false;
  }
  OSData *bankSize = OSDynamicCast (OSData, chosen->getProperty ("nvram-bank-size"));
  OSData *proxyData = OSDynamicCast (OSData, chosen->getProperty ("nvram-proxy-data"));
  bool valid = (bankSize != nullptr) && (bankSize->getLength () == sizeof (uint32_t)) &&
               (proxyData != nullptr) && (proxyData->getLength () != 0);
  chosen->release ();
  return valid;
}

bool ArmDTPlatform::createDeviceNubs (IOService *provider)
{
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  if (bus == nullptr) {
    return false;
  }
  // IODTPlatformExpert's IOPlatformDevice nubs resolve resources through
  // their provider's IOPlatformExpert::getNubResources. They must therefore
  // be attached to this platform expert, not to the arm-io IOPlatformDevice
  // (ArmVirtPlatform uses the same parent relationship). The device-tree
  // parent remains arm-io, so reg/ranges resolution still follows the bus.
  // Publish every real arm-io child, not only the first pattern match. The
  // GIC lives here, and the UFS host/PHY nodes must be visible to their native
  // drivers too. processTopLevel() still uses deleteList()/excludeList().
  OSIterator *iterator = bus->getChildIterator (gIODTPlane);
  bus->release ();
  if (iterator == nullptr) {
    return false;
  }
  uint32_t count = 0;
  IORegistryEntry *entry;
  while ((entry = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    IOService *nub = createNub (entry);
    if (nub == nullptr) {
      continue;
    }
    if (!nub->attach (this)) {
      nub->release ();
      continue;
    }
    nub->registerService ();
    nub->release ();
    count++;
  }
  iterator->release ();
  armdt_platform_device_count = count;
  return true;
}

__attribute__((noreturn)) static void platformStageReset (void)
{
  // PSCI SYSTEM_RESET via the same SMC conduit as the entry reset probe.
  register uint64_t x0 __asm__ ("x0") = 0x84000009;
  register uint64_t x1 __asm__ ("x1") = 0;
  register uint64_t x2 __asm__ ("x2") = 0;
  register uint64_t x3 __asm__ ("x3") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "memory");
  for (;;) { __asm__ volatile ("wfe"); }
}

// ---- the calendar (pagani-rtc, PlatformRtc.h) ----
namespace {
struct RtcWindow {
  IOMemoryMap *map;
  volatile uint32_t *regs;
};
struct RtcState {
  IOLock *lock;
  PrtcClock clock;
  RtcWindow core, cnfg, obs;
  int apid;
  bool enabled, resolved;
};
}

static RtcState gRtc;

static bool rtcMap (RtcWindow &window, uint64_t base, uint32_t bytes)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (base, bytes);
  window.map = memory ? memory->map (kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  OSSafeReleaseNULL (memory);
  window.regs = window.map ? reinterpret_cast<volatile uint32_t *> (window.map->getVirtualAddress ()) : nullptr;
  return window.regs != nullptr;
}

static void rtcUnmap (RtcWindow &window)
{
  window.regs = nullptr;
  OSSafeReleaseNULL (window.map);
}

static uint32_t rtcRead32 (const RtcWindow &window, uint32_t offset)
{
  const uint32_t value = window.regs[offset / 4];
  __asm__ volatile ("dmb sy" ::: "memory");
  return value;
}

static uint32_t rtcCore (void *, uint32_t offset) { return rtcRead32 (gRtc.core, offset); }
static uint32_t rtcCnfg (void *, uint32_t offset) { return rtcRead32 (gRtc.cnfg, offset); }
static uint32_t rtcObsRead (void *, uint32_t offset) { return rtcRead32 (gRtc.obs, offset); }

static void rtcObsWrite (void *, uint32_t offset, uint32_t value)
{
  gRtc.obs.regs[offset / 4] = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void rtcDelay (void *, unsigned us) { IODelay (us); }

static const PrtcIo gRtcIo = {nullptr, rtcCore, rtcCnfg, rtcObsRead, rtcObsWrite, rtcDelay};

// The channel, once: the two table windows go again, the observer stays.
// Under gRtc.lock.
static void rtcResolve (void)
{
  gRtc.resolved = true;
  gRtc.apid = PRTC_ENODEV;
  if (rtcMap (gRtc.core, PRTC_CORE_BASE, PRTC_CORE_BYTES) && rtcMap (gRtc.cnfg, PRTC_CNFG_BASE, PRTC_CNFG_BYTES) &&
      rtcMap (gRtc.obs, PRTC_OBS_BASE, PRTC_OBS_BYTES)) {
    gRtc.apid = prtc_find (&gRtcIo, PRTC_PPID);
  }
  rtcUnmap (gRtc.core);
  rtcUnmap (gRtc.cnfg);
  if (gRtc.apid < 0) {
    rtcUnmap (gRtc.obs);
  }
}

// Under gRtc.lock.
static int rtcRead (uint32_t *raw, uint8_t *control)
{
  if (!gRtc.resolved) {
    rtcResolve ();
  }
  return gRtc.apid < 0 ? gRtc.apid : prtc_read_raw (&gRtcIo, gRtc.apid, raw, control);
}

static uint64_t rtcUptime (void)
{
  clock_sec_t seconds = 0;
  clock_usec_t micros = 0;
  clock_get_system_microtime (&seconds, &micros);
  return seconds;
}

static void rtcLog (const char *what, int rc, uint32_t raw, uint8_t control, const PrtcClock &clock, int64_t utc)
{
  static const char *const from[] = {"none", "raw", "boot-arg", "floor", "set"};
  char line[128];
  snprintf (line, sizeof (line), "[rtc] %s rc %d raw %u ctl 0x%x apid %d offset %lld (%s) utc %lld", what, rc, raw, control,
            gRtc.apid, clock.offset, from[clock.from], utc);
  boot_progress_trace (line);
}

static int64_t rtcSeconds (void)
{
  uint32_t raw = 0;
  uint8_t control = 0;
  IOLockLock (gRtc.lock);
  const int rc = rtcRead (&raw, &control);
  const int64_t utc = prtc_clock_get (&gRtc.clock, rc == PRTC_OK, raw, rtcUptime ());
  const PrtcClock clock = gRtc.clock;
  IOLockUnlock (gRtc.lock);
  rtcLog ("get", rc, raw, control, clock, utc);
  return utc;
}

// A BCM2837 XNU does not wait for the platform expert before it starts the
// calendar (IOKitInitializeTime compiles the IORTC wait out), and a read that
// finds gIOPlatform unset gives 0. IOBSD is published after that read, so
// from here on the calendar has either this clock's time or the uptime.
static void rtcLateCheck (void *, wait_result_t)
{
  thread_set_thread_name (current_thread (), "pagani-rtc");
  OSDictionary *matching = IOService::resourceMatching ("IOBSD");
  if (matching == nullptr) {
    return;
  }
  IOService *published = IOService::waitForMatchingService (matching);
  OSSafeReleaseNULL (published);
  OSSafeReleaseNULL (matching);
  clock_sec_t seconds = 0;
  clock_usec_t micros = 0;
  clock_get_calendar_microtime (&seconds, &micros);
  const int64_t utc = rtcSeconds ();
  if (prtc_calendar_stale (seconds, utc)) {
    clock_set_calendar_microtime (static_cast<clock_sec_t> (utc), 0);
  }
}

// Runs before IOPlatformExpert::start makes this object gIOPlatform: from
// then on the calendar can ask for the time.
static void rtcStart (IOService *provider)
{
  unsigned enabled = 0;
  PE_parse_boot_argn ("pagani-rtc", &enabled, sizeof (enabled));
  if (enabled != 1) {
    return;
  }
  gRtc.lock = IOLockAlloc ();
  if (gRtc.lock == nullptr) {
    return;
  }
  int64_t floor = 0;
  IORegistryEntry *chosen = provider->childFromPath ("chosen", gIODTPlane);
  if (chosen != nullptr) {
    OSData *built = OSDynamicCast (OSData, chosen->getProperty ("pagani-build-time"));
    if (built != nullptr) {
      floor = prtc_floor (built->getBytesNoCopy (), built->getLength ());
    }
    chosen->release ();
  }
  int64_t offset = 0;
  const bool haveOffset = PE_parse_boot_argn ("pagani-rtc-offset", &offset, sizeof (offset));
  prtc_clock_start (&gRtc.clock, haveOffset, offset, floor);
  gRtc.enabled = true;
}

long ArmDTPlatform::getGMTTimeOfDay (void)
{
  return gRtc.enabled ? rtcSeconds () : IODTPlatformExpert::getGMTTimeOfDay ();
}

// The offset in memory only: the RTC itself is never written.
void ArmDTPlatform::setGMTTimeOfDay (long secs)
{
  if (!gRtc.enabled) {
    IODTPlatformExpert::setGMTTimeOfDay (secs);
    return;
  }
  uint32_t raw = 0;
  uint8_t control = 0;
  IOLockLock (gRtc.lock);
  const int rc = rtcRead (&raw, &control);
  prtc_clock_set (&gRtc.clock, rc == PRTC_OK, raw, rtcUptime (), secs);
  const PrtcClock clock = gRtc.clock;
  IOLockUnlock (gRtc.lock);
  rtcLog ("set", rc, raw, control, clock, secs);
}

// ---- restart and power off (pagani-halt, PlatformHalt.h) ----
int ArmDTPlatform::haltRestart (unsigned int type)
{
  const char *reason = nullptr;
  const int action = pagani_halt_action (haltMode, type, &reason);
  if (action == PHALT_INHERIT) {
    return IODTPlatformExpert::haltRestart (type);
  }
  if (action == PHALT_POWER_OFF) {
    ramoops_log_power_off (reason);
  } else {
    ramoops_log_restart (reason);
  }
  // Those do not return on a kernel that has them.
  platformStageReset ();
}

bool ArmDTPlatform::start (IOService *provider)
{
  PE_parse_boot_argn ("pagani-halt", &haltMode, sizeof (haltMode));
  rtcStart (provider);
  if (!IODTPlatformExpert::start (provider)) {
    return false;
  }
  if (gRtc.enabled) {
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start (rtcLateCheck, nullptr, &thread) == KERN_SUCCESS) {
      thread_deallocate (thread);
    }
  }
  return true;
}

bool ArmDTPlatform::configure (IOService *provider)
{
  kprintf ("ArmDTPlatform: configure\n");
  IORegistryEntry *cpus = provider->childFromPath ("cpus", gIODTPlane);
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  IORegistryEntry *defaults = provider->childFromPath ("defaults", gIODTPlane);
  IORegistryEntry *options = provider->childFromPath ("options", gIODTPlane);
  bool structure = (cpus != nullptr) && (bus != nullptr) &&
                   (defaults != nullptr) && (options != nullptr);
  if (cpus != nullptr) { cpus->release (); }
  if (bus != nullptr) { bus->release (); }
  if (defaults != nullptr) { defaults->release (); }
  if (options != nullptr) { options->release (); }
  if (!structure) {
    IOLog ("ArmDTPlatform: required /cpus, /arm-io, /defaults or /options node is absent\n");
    return false;
  }
  if (!hasNVRAMProxy (provider)) {
    IOLog ("ArmDTPlatform: /chosen does not supply a usable CHRP NVRAM proxy\n");
    return false;
  }
  unsigned platformStage = 0;
  PE_parse_boot_argn ("sheng-platform-stage", &platformStage, sizeof (platformStage));
  if (platformStage == 1) {
    platformStageReset ();
  }

  // The inherited implementation publishes real IODTNVRAM from /chosen,
  // then attaches the CPU and top-level nubs. No identity or resource is
  // invented here; failure is returned instead of replaced with a stub.
  if (!IODTPlatformExpert::configure (provider)) {
    return false;
  }
  if (platformStage == 2) {
    platformStageReset ();
  }

  OSIterator *iterator = getChildIterator (gIOServicePlane);
  if (iterator == nullptr) {
    return false;
  }
  uint32_t count = 0;
  uint32_t cpuCount = 0;
  uint32_t busCount = 0;
  uint32_t defaultsCount = 0;
  IORegistryEntry *child;
  while ((child = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    count++;
    OSData *type = OSDynamicCast (OSData, child->getProperty ("device_type"));
    if ((type != nullptr) && (type->getLength () == 4) &&
        (memcmp (type->getBytesNoCopy (), "cpu", 4) == 0)) {
      cpuCount++;
    }
    if (strcmp (child->getName (), "arm-io") == 0) {
      busCount++;
    }
    if (strcmp (child->getName (), "defaults") == 0) {
      defaultsCount++;
    }
  }
  iterator->release ();
  armdt_platform_child_count = count;
  armdt_platform_cpu_count = cpuCount;
  armdt_platform_bus_count = busCount;
  armdt_platform_defaults_count = defaultsCount;
  armdt_platform_nvram_proxy = 1;
  armdt_platform_configured = 1;
  kprintf ("ArmDTPlatform: CPU nubs=%u bus nubs=%u, NVRAM proxy ready\n", cpuCount, busCount);
  // Publish the bus child devices (interrupt controller, VirtIO MMIO, ...)
  // as real IOPlatformDevice nubs. No driver is started here.
  if (!createDeviceNubs (provider)) {
    return false;
  }
  if (platformStage == 3) {
    platformStageReset ();
  }
  // Explicit hardware A/B diagnostic: reach the end of the real platform
  // configuration and panic before publishing the service. The payload's
  // panic-screen probe makes this observable without a serial console.
  unsigned platformProbe = 0;
  PE_parse_boot_argn ("sheng-platform-panic", &platformProbe, sizeof (platformProbe));
  if (platformProbe != 0 || platformStage == 4) {
    platformStageReset ();
  }
  registerService ();
  if (platformStage == 5) {
    platformStageReset ();
  }
  return true;
}
