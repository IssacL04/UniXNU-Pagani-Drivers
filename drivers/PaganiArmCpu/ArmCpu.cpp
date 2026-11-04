// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <mach/processor_info.h>
#include "ArmCpuSpiGate.h"
#include "CpuFreqGovernor.h"
#include "PsciIdle.h"
#include "../BootProgress/PeripheralStatus.h"

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr (unsigned int);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop (vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start (processor_t);
extern "C" kern_return_t kernel_thread_start (void (*) (void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate (thread_t);
extern "C" thread_t current_thread (void);
extern "C" kern_return_t thread_terminate (thread_t);
// pagani-cpufreq: the scheduler's per-processor idle ticks (osfmk/kern/processor.c processor_info, PROCESSOR_CPU_LOAD_INFO counts the
// recount idle time in hz_tick_interval units of mach time) and the thread's name.
extern "C" kern_return_t processor_info (processor_t, processor_flavor_t, host_t *, processor_info_t, mach_msg_type_number_t *);
extern "C" processor_t cpu_to_processor (int);
extern "C" uint32_t hz_tick_interval;
extern "C" void thread_set_thread_name (thread_t, const char *);
// pagani-cpufreq's launch boost: the execs of an app or extension bundle (phone-app-exec-count.patch), what begins one, and XNU's process
// identity version, one more at every fork (bsd/kern/kern_fork.c forkproc) and every exec (bsd/kern/kern_exec.c exec_handle_sugid), what
// renews one. Lock-free reads.
extern "C" int pagani_app_execs;
extern "C" int nextpidversion;
extern "C" void pagani_cpufreq_boost (void);
// Weak defaults for builds without the other drivers (the statically linked kernel refuses undefined weak references): BootProgress's
// CPU status row, its persistent log and stage history, PaganiInput's touch frame counter (also defined weak by the A830 DCVS) and the
// GPU driver's two governor inputs (Adreno830's A830Dcvs.cpp defines both): adreno830_cpu_hint, a free-running count of accepted GPU
// submissions, and adreno830_thermal_state, the hottest GPU sensor in deci-C with its flags (cpufreq::kGpuHeat*: a usable reading,
// the GPU held below its top by its thermal loop, its 95 C emergency). Both are lock-free reads.
extern "C" {
void boot_progress_stage (const char *);
uint32_t adreno830_cpu_hint (void);
int32_t adreno830_thermal_state (uint32_t *flags);
__attribute__((weak)) void boot_progress_cpu_row (const char *) {}
__attribute__((weak)) void boot_progress_trace (const char *) {}
__attribute__((weak)) void boot_progress_stage (const char *) {}
__attribute__((weak)) volatile uint32_t pagani_input_touch_events;
// pagani-screenoff bit 0 (CpuFreqGovernor.h, screen off): the display's power as PaganiPanel publishes it from the display's power
// callback (bit 0 off, the count of changes above it) and PaganiInput's key report counter. Weak zero words like the touch counter:
// the kernel link keeps one word each, and without those drivers the screen reads as on and no key is ever pressed.
__attribute__((weak)) volatile uint32_t pagani_screen_state;
__attribute__((weak)) volatile uint32_t pagani_input_key_events;
__attribute__((weak)) uint32_t adreno830_cpu_hint (void) { return 0; }
__attribute__((weak)) int32_t adreno830_thermal_state (uint32_t *flags)
{
  if (flags != nullptr) { *flags = 0; }
  return 0;
}
}

extern "C" {
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_timer_interrupts = 0;
volatile uint64_t armcpu_ipi_interrupts = 0;
volatile uint64_t armcpu_ipi_sent = 0;       // SGIs the kernel asked ArmCpu::signalCPU to send
volatile uint64_t armcpu_ipi_selftest_sent = 0;
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];   // per logical core: architectural timer interrupts and SGI 0 taken
static bool gSpiIpi = false;                // boot-arg armcpu-spi-ipi=1: IPIs are software-pended SPIs 1008+core routed to that core (ICC_SGI1R_EL1 generates nothing on this board)
static bool gSelfIpi = false;               // boot-arg armcpu-self-ipi=1: every core sends itself SGI 0 from initCPU
volatile uint32_t armcpu_irq_phase = 0,armcpu_irq_id = 1023;
volatile uint32_t armcpu_quiesce_calls = 0,armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence (sheng-smp=1). Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 GIC redistributor ready, 7 running.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8], armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
}

#define ARMCPU_SMP_SLOTS 8

class ArmCpu : public IOCPU {
  OSDeclareDefaultStructors (ArmCpu)
  OSSharedPtr<IOMemoryMap> distributor;
  OSSharedPtr<IOMemoryMap> redistributors;
  IOCPUInterruptController *controller = nullptr;
  const OSSymbol *cpuName = nullptr;
  uintptr_t dist = 0;
  uintptr_t redist = 0;
  uint64_t affinity = 0;
  uintptr_t redistBase = 0;
  uint64_t redistSize = 0;
  bool skipGic = false;
  bool diagnosePower = false;
  bool secondary = false;
  uint32_t logicalId = 0;
  bool mapGic (void);
  bool initDistributor (void);
  bool wakeRedistributor (bool local);
  bool finishSecondary (void);
  static void drainPending (void);
  bool prepareGic (void);
  void startSecondary (void);
  static void startThread (void *argument, wait_result_t);
  void enableGic (void);
  void interrupt (void);
  static void dispatch (void *target, void *, void *, int);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void initCPU (bool boot) override;
  void quiesceCPU (void) override;
  void haltCPU (void) override;
  kern_return_t startCPU (vm_offset_t, vm_offset_t) override;
  void signalCPU (IOCPU *target) override;
  uint64_t signalTarget (void) const { return affinity; }
  void enableCPUTimeBase (bool enabled) override;
  const OSSymbol *getCPUName (void) override;
};

OSDefineMetaClassAndStructors (ArmCpu, IOCPU)

// Redistributor ISENABLER0: SGIs 0..15 (only SGI 0 is used by the kernel; the others let the self-SGI probe tell which INTIDs the GIC accepts) and PPI 27.
static const uint32_t kSgiPpiEnable = 0xffffU | (1U << 27);

static const uint32_t kIpiSpiBase = 1008;   // INTIDs 1008..1015 stand for 'IPI to logical core 0..7'

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

// Distributor side of the software IPI of logical core `core`: group 1, edge-triggered, priority 0x80, routed to affinity `aff`, enabled.
static void spiIpiConfigure (uintptr_t dist, uint32_t core, uint64_t aff)
{
  const uint32_t id = kIpiSpiBase + core;
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) | (2U << ((id % 16) * 2)));
  write32 (dist, 0x400 + (id / 4) * 4, 0x80808080);
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void spiIpiSend (uintptr_t dist, uint32_t core)
{
  const uint32_t id = kIpiSpiBase + core;
  __asm__ volatile ("dsb ishst" ::: "memory");
  write32 (dist, 0x200 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t counter (void)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static bool waitClear (uintptr_t base, unsigned offset, uint32_t mask)
{
  uint64_t frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
  uint64_t started = counter ();
  while ((read32 (base, offset) & mask) != 0) {
    if (counter () - started > frequency / 10) {
      return false;
    }
  }
  return true;
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uint64_t gFramesSize = 0;
static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0; // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gSmpWanted = false;
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
static bool gRemoteGic = false; // sheng-smp-gic=1: the boot CPU prepares a new core's redistributor before CPU_ON

// Shared peripheral interrupts (GICv3 SPIs, INTID 32..1019). A driver attaches a handler for one INTID; the line is set to
// Group 1 non-secure, priority 0xa0, the given trigger and routed to the boot CPU (GICD_IROUTER, affinity routing is on),
// then enabled. The handler runs in the primary interrupt context, before the EOI, and must clear a level source.
// An SPI without a handler is counted and disabled instead of being acknowledged forever.
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};
static ArmCpuSpi gSpi[ARMCPU_SPI_SLOTS];
static volatile uint32_t gSpiUsed = 0;
static uint64_t gBootAffinity = 0;
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
}

// CPU performance levels over SCMI (boot-arg pagani-cpufreq; CpuFreqGovernor.h says what each mode does, ScmiPerf.h how the messages
// travel). Absent or 0: nothing here runs, no window is mapped. Otherwise one kernel thread does everything: it maps the CPUCP mailbox
// windows and the SCMI shared memory, describes both performance domains, then holds the top level (1) or runs the governor (2), and
// shows the CPU row (BootProgress row 14). All SCMI use holds gCpuFreqLock and happens in that thread; a caller in interrupt context
// is refused before any access. A transient SCMI failure (BUSY, no answer) is retried after a backoff (CpuFreqGovernor.h, Retry; a start
// that failed this way probes again after it, the thread sleeping meanwhile); a final failure, or a retry budget run out, switches the
// feature off for the boot and says why on the row; it never panics.
// armcpu_cpufreq_state: 0 not started, 1 probing (also while a start waits for its retry), 2 running, 3 off after a failure.
extern "C" {
volatile uint32_t armcpu_cpufreq_state = 0;
}
namespace {
cpufreq::Control gCpuFreq;
IOLock *gCpuFreqLock;
IOMemoryMap *gCpuFreqTx, *gCpuFreqRx, *gCpuFreqShmem;
uintptr_t gCpuFreqTxVa, gCpuFreqShmemVa;
uint32_t gCpuFreqMode, gCpuFreqMin;
bool gCpuFreqScreen;                        // pagani-screenoff bit 0: the governor's screen-off policy (CpuFreqGovernor.h)
bool gCpuFreqNoRetry;                       // pagani-cpufreq-retry=0: every SCMI failure final (CpuFreqGovernor.h, Retry)
uint32_t gCpuFreqTouchMs, gCpuFreqLaunchMs; // pagani-cpufreq-touch-ms, pagani-cpufreq-launch-ms (CpuFreqGovernor.h)
uint32_t gCpuFreqAsks;                      // pagani_cpufreq_boost calls
uint64_t gCpuFreqSpin;                      // counter ticks a poll spins before it sleeps between reads
char gCpuFreqShown[cpufreq::kRowWidth + 1];
// The performance domain of each logical CPU from its DT affinity (MPIDR Aff2: CPUs 0x0-0x500 domain 0, 0x10000 and 0x10100 domain 1),
// noted as each CPU nub starts; 0 unknown, else domain + 1.
volatile uint32_t gCpuFreqDomain[ARMCPU_SMP_SLOTS];

void cpufreqNoteCpu (uint32_t logical, uint64_t mpidr)
{
  if (logical < ARMCPU_SMP_SLOTS) { gCpuFreqDomain[logical] = ((mpidr >> 16) & 0xff) != 0 ? 2 : 1; }
}

uint32_t cpufreqRead32 (void *, uint32_t offset)
{
  return *reinterpret_cast<volatile uint32_t *> (gCpuFreqShmemVa + offset);
}

void cpufreqWrite32 (void *, uint32_t offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gCpuFreqShmemVa + offset) = value;
}

// The message is in the shared memory before the doorbell reaches the CPUCP (two Device regions: a full barrier between them).
void cpufreqRing (void *, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *> (gCpuFreqTxVa + scmi::kDoorbell) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

uint64_t cpufreqNow (void *)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value) :: "memory");
  return value;
}

// The CPUCP answers within microseconds when it is well: spin the first 100 us, then sleep 1 ms between reads up to the timeout.
void cpufreqPause (void *, uint64_t waited)
{
  if (waited < gCpuFreqSpin) { __asm__ volatile ("yield" ::: "memory"); return; }
  IOSleep (1);
}

bool cpufreqAllowed (void *)
{
  return !ml_at_interrupt_context ();
}

void cpufreqLog (void *, const char *text)
{
  kprintf ("ArmCpu: %s\n", text);
  boot_progress_trace (text);
}

// The row when its text changed (BootProgress redraws it only then as well).
void cpufreqPublish (void)
{
  char text[cpufreq::kRowWidth + 1];
  cpufreq::controlRow (&gCpuFreq, text, sizeof (text));
  unsigned i = 0;
  for (; text[i] != 0 && text[i] == gCpuFreqShown[i]; i++) {}
  if (text[i] == gCpuFreqShown[i]) { return; }
  for (i = 0; i < sizeof (text); i++) { gCpuFreqShown[i] = text[i]; }
  boot_progress_cpu_row (text);
}

void cpufreqOff (const char *reason)
{
  gCpuFreq.on = false;
  unsigned i = 0;
  for (; reason[i] != 0 && i + 1 < sizeof (gCpuFreq.reason); i++) { gCpuFreq.reason[i] = reason[i]; }
  gCpuFreq.reason[i] = 0;
  char line[64];
  scmi::Text t = {line, 0, sizeof (line)};
  scmi::put (&t, "cpufreq: off: ");
  scmi::put (&t, gCpuFreq.reason);
  cpufreqLog (nullptr, line);
  armcpu_cpufreq_state = 3;
  cpufreqPublish ();
}

// Sleeps until counter tick `at` (a retry's backoff; at least 1 ms, at most 2 s: the backoff is at most cpufreq::kRetryMaxMs).
void cpufreqSleepUntil (uint64_t at, uint64_t hz)
{
  const uint64_t now = cpufreqNow (nullptr), perMs = hz / 1000 != 0 ? hz / 1000 : 1;
  uint64_t ms = at > now ? (at - now + perMs - 1) / perMs : 1;
  if (ms == 0) { ms = 1; }
  if (ms > 2000) { ms = 2000; }
  IOSleep (static_cast<unsigned> (ms));
}

// Device memory (uncached) for one register window; the map keeps its descriptor.
IOMemoryMap *cpufreqMap (uint64_t pa, uint64_t bytes)
{
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress (pa, bytes, kIODirectionInOut);
  if (memory == nullptr) { return nullptr; }
  IOMemoryMap *map = memory->map (kIOMapAnywhere | kIOMapInhibitCache);
  memory->release ();
  return map;
}

// Each known CPU's idle ticks and whether it runs.
void cpufreqSample (cpufreq::CpuTicks *out)
{
  const int last = ml_get_max_cpu_number ();
  for (unsigned c = 0; c < cpufreq::kCpus; c++) {
    out[c].known = false; out[c].running = false; out[c].domain = 0; out[c].idle = 0;
    const uint32_t domain = c < ARMCPU_SMP_SLOTS ? gCpuFreqDomain[c] : 0;
    if (domain == 0 || static_cast<int> (c) > last) { continue; }
    processor_t processor = cpu_to_processor (static_cast<int> (c));
    if (processor == nullptr) { continue; }
    processor_basic_info_data_t basic = {};
    processor_cpu_load_info_data_t load = {};
    host_t host = nullptr;
    mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
    if (processor_info (processor, PROCESSOR_BASIC_INFO, &host, reinterpret_cast<processor_info_t> (&basic), &count) != KERN_SUCCESS) {
      continue;
    }
    count = PROCESSOR_CPU_LOAD_INFO_COUNT;
    if (processor_info (processor, PROCESSOR_CPU_LOAD_INFO, &host, reinterpret_cast<processor_info_t> (&load), &count) != KERN_SUCCESS) {
      continue;
    }
    out[c].known = true;
    out[c].running = basic.running != 0;
    out[c].domain = domain - 1;
    out[c].idle = load.cpu_ticks[CPU_STATE_IDLE];
  }
}

void cpufreqRun (void)
{
  cpufreq::Control *c = &gCpuFreq;
  c->log = cpufreqLog;
  c->logContext = nullptr;
  uint64_t hz;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (hz));
  gCpuFreqSpin = hz / 10000;
  gCpuFreqLock = IOLockAlloc ();
  if (gCpuFreqLock == nullptr || hz == 0) { cpufreqOff ("no lock"); return; }
  gCpuFreqTx = cpufreqMap (scmi::kTxBase, scmi::kTxBytes);
  gCpuFreqRx = cpufreqMap (scmi::kRxBase, scmi::kRxBytes);
  gCpuFreqShmem = cpufreqMap (scmi::kShmemBase, scmi::kShmemBytes);
  if (gCpuFreqTx == nullptr) { cpufreqOff ("map tx"); return; }
  if (gCpuFreqShmem == nullptr) { cpufreqOff ("map shmem"); return; }
  // The RX window is only for the CPUCP's completion interrupts, which a polled transfer never asks for: nothing reads or writes it, so
  // a failure to map it is only noted.
  if (gCpuFreqRx == nullptr) { cpufreqLog (nullptr, "cpufreq: rx window not mapped (not used: completion is polled)"); }
  gCpuFreqTxVa = static_cast<uintptr_t> (gCpuFreqTx->getVirtualAddress ());
  gCpuFreqShmemVa = static_cast<uintptr_t> (gCpuFreqShmem->getVirtualAddress ());
  scmi::Io io = {};
  io.context = nullptr;
  io.read32 = cpufreqRead32;
  io.write32 = cpufreqWrite32;
  io.ring = cpufreqRing;
  io.now = cpufreqNow;
  io.pause = cpufreqPause;
  io.allowed = cpufreqAllowed;
  io.hz = hz;
  io.timeoutMs = scmi::kTimeoutMs;
  c->perf.channel.io = io;
  armcpu_cpufreq_state = 1;
  // Logged before the first access: a stage-2 fault or a hang right here is then the CPUCP windows (Gunyah not mapping them).
  cpufreqLog (nullptr, "cpufreq: probing CPUCP SCMI (tx 0x17830000 doorbell 0x104, shmem 0x17b4e000)");
  IOLockLock (gCpuFreqLock);
  c->screen = gCpuFreqScreen && gCpuFreqMode == cpufreq::kModeGovernor;
  c->noRetry = gCpuFreqNoRetry;
  c->touchMs = gCpuFreqTouchMs;
  c->launchMs = gCpuFreqLaunchMs;
  bool alive = cpufreq::controlStart (c, gCpuFreqMode, gCpuFreqMin, cpufreqNow (nullptr));
  IOLockUnlock (gCpuFreqLock);
  // A start that failed transiently (BUSY, no answer: c->retry.waiting) probes again once its backoff passed, the thread asleep meanwhile;
  // the retry budgets end it ("CPU scmi off: ... x6"). Each attempt sends what one start sends.
  while (!alive && c->retry.waiting) {
    cpufreqPublish ();
    cpufreqSleepUntil (c->retry.at, hz);
    IOLockLock (gCpuFreqLock);
    alive = cpufreq::controlProbe (c, cpufreqNow (nullptr));
    IOLockUnlock (gCpuFreqLock);
  }
  cpufreqPublish ();
  {
    char text[cpufreq::kRowWidth + 1];
    cpufreq::controlRow (c, text, sizeof (text));
    boot_progress_stage (text);
  }
  if (!alive) { armcpu_cpufreq_state = 3; return; }
  armcpu_cpufreq_state = 2;
  const uint64_t tick = hz_tick_interval > 1 ? hz_tick_interval : hz / 100;
  const uint32_t window = gCpuFreqMode == cpufreq::kModeGovernor ? cpufreq::kWindowMs : cpufreq::kRowMs;
  cpufreq::CpuTicks before[cpufreq::kCpus], now[cpufreq::kCpus];
  cpufreqSample (before);
  uint64_t last = cpufreqNow (nullptr), rowAt = last;
  uint32_t online[scmi::kDomains] = {0, 0};
  for (;;) {
    IOSleep (window);
    const uint64_t at = cpufreqNow (nullptr);
    cpufreqSample (now);
    // Each CPU's busy ticks over the window: the governor averages every CPU on its own before it takes a domain's busiest.
    cpufreq::CpuWindow cpus[cpufreq::kCpus];
    uint32_t running[scmi::kDomains];
    cpufreq::cpuWindows (now, before, cpufreq::kCpus, at - last, tick, cpus, running);
    for (unsigned d = 0; d < scmi::kDomains; d++) {
      if (running[d] > online[d]) { online[d] = running[d]; }
    }
    for (unsigned i = 0; i < cpufreq::kCpus; i++) { before[i] = now[i]; }
    last = at;
    const uint32_t touches = pagani_input_touch_events;
    const uint32_t hint = adreno830_cpu_hint ();
    cpufreq::GpuHeat heat = {0, 0};
    heat.deci = adreno830_thermal_state (&heat.flags);
    cpufreq::ScreenInput screen = {0, 0};
    if (c->screen) {
      screen.state = __atomic_load_n (&pagani_screen_state, __ATOMIC_ACQUIRE);
      screen.keys = __atomic_load_n (&pagani_input_key_events, __ATOMIC_RELAXED);
    }
    const cpufreq::LaunchInput launch = {static_cast<uint32_t> (__atomic_load_n (&pagani_app_execs, __ATOMIC_RELAXED)),
                                         __atomic_load_n (&gCpuFreqAsks, __ATOMIC_RELAXED),
                                         __atomic_load_n (&pagani_screen_state, __ATOMIC_ACQUIRE),
                                         static_cast<uint32_t> (__atomic_load_n (&nextpidversion, __ATOMIC_RELAXED))};
    IOLockLock (gCpuFreqLock);
    // Both modes: the governor window (2) or a top level still owed (1), and the 5 s check; nothing while a retry's backoff lasts.
    alive = cpufreq::controlWindowCpus (c, at, cpus, cpufreq::kCpus, touches, hint, heat, screen, launch);
    IOLockUnlock (gCpuFreqLock);
    if (!alive) { armcpu_cpufreq_state = 3; cpufreqPublish (); return; }
    if (at - rowAt >= cpufreq::ticks (hz, cpufreq::kRowMs)) {
      cpufreq::controlLoads (c, online);
      online[0] = online[1] = 0;
      rowAt = at;
      cpufreqPublish ();
    }
  }
}

void cpufreqThread (void *, wait_result_t)
{
  thread_set_thread_name (current_thread (), "pagani-cpufreq");
  cpufreqRun ();
  thread_terminate (current_thread ());
}

// From the boot CPU's start: pagani-cpufreq=1 or 2 starts the thread; absent or 0 does nothing; another value only says so on the row.
// pagani-screenoff bit 0 adds the governor's screen-off policy (pagani-cpufreq=2 only; the other bits are the GPU's and the panel's).
// pagani-cpufreq-retry=0 makes every SCMI failure final again (default 1: BUSY and missing answers are retried, CpuFreqGovernor.h).
// pagani-cpufreq-touch-ms=<ms> and pagani-cpufreq-launch-ms=<ms> (0: no launch boost) set how long the governor holds the touch and launch
// levels (defaults cpufreq::kTouchMs, cpufreq::kLaunchMs; pagani-cpufreq=2 only).
void cpufreqStart (void)
{
  unsigned mode = 0, mhz = 0, screen = 0, retry = 1, touchMs = cpufreq::kTouchMs, launchMs = cpufreq::kLaunchMs;
  if (!PE_parse_boot_argn ("pagani-cpufreq", &mode, sizeof (mode)) || mode == cpufreq::kModeOff) { return; }
  PE_parse_boot_argn ("pagani-cpufreq-min", &mhz, sizeof (mhz));
  PE_parse_boot_argn ("pagani-screenoff", &screen, sizeof (screen));
  PE_parse_boot_argn ("pagani-cpufreq-retry", &retry, sizeof (retry));
  PE_parse_boot_argn ("pagani-cpufreq-touch-ms", &touchMs, sizeof (touchMs));
  PE_parse_boot_argn ("pagani-cpufreq-launch-ms", &launchMs, sizeof (launchMs));
  gCpuFreqScreen = (screen & 1u) != 0;
  gCpuFreqNoRetry = retry == 0;
  gCpuFreqTouchMs = touchMs;
  gCpuFreqLaunchMs = launchMs;
  gCpuFreq.mode = mode;
  if (mode != cpufreq::kModeMax && mode != cpufreq::kModeGovernor) { cpufreqOff ("bad pagani-cpufreq"); return; }
  gCpuFreqMode = mode;
  gCpuFreqMin = mhz;
  thread_t thread;
  if (kernel_thread_start (&cpufreqThread, nullptr, &thread) != KERN_SUCCESS) { cpufreqOff ("no thread"); return; }
  thread_deallocate (thread);
}
}  // namespace

// A driver that knows an app is starting asks for the launch boost (pagani-cpufreq-launch-ms, CpuFreqGovernor.h), past the governor's own
// app-start rule and its touch gate. Any thread, any context: this only counts, the governor's thread takes it at its next window and
// no SCMI message leaves the caller. The heat and screen-off caps hold as for any boost; without the governor it does nothing. Callers
// define an empty weak default (the statically linked kernel refuses undefined weak references).
extern "C" void pagani_cpufreq_boost (void)
{
  __atomic_fetch_add (&gCpuFreqAsks, 1u, __ATOMIC_RELAXED);
}

// pagani-idle: XNU's CPU idle through PSCI CPU_SUSPEND retention states (PsciIdle.h: which states, why, the safety switches; README
// "CPU idle"). Absent or 0: no processor_idle hook is registered and nothing below runs. Otherwise every CPU registers idleNotify;
// the boot CPU's start reads the PSCI facts, runs one self-test entry per state with its timer brought forward and only then
// switches the hook on; a thread logs the counts every pagani-idle-log seconds (default 30, at most 3600; 0: the start lines only).
// Nothing here panics: whatever fails leaves XNU's own WFI. armcpu_idle_state: 0 not configured, 1 probing, 2 on, 3 off.
extern "C" {
volatile uint32_t armcpu_idle_state = 0;
}
static bool psciConduit (bool *hvc);
namespace {
psciidle::Control gIdle;
bool gIdleHvc = false;
uint32_t gIdleLogSeconds = 30;

// The PSCI call of the idle path: psciCall's conduit, but x1-x3 are outputs too (SMCCC result registers: the firmware may change
// them, and this call is inlined into the idle hook), x4-x17 clobbered as before SMCCC 1.1.
int64_t idleCall (void *, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (gIdleHvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) :
                      : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) :
                      : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

uint64_t idleNow (void *)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value) :: "memory");
  return value;
}

const psciidle::Io gIdleIo = {nullptr, idleCall, idleNow};

void idleLog (const char *text)
{
  kprintf ("ArmCpu: %s\n", text);
  boot_progress_trace (text);
}

// XNU's processor_idle hook: osfmk/arm64/cpu.c cpu_idle (enter, before its WFI) and cpu_idle_exit (leave), IRQ/FIQ masked, on the
// idle CPU itself. One atomic load while PSCI idle is off.
void idleNotify (cpu_id_t, boolean_t enter, uint64_t *)
{
  if (!psciidle::running (&gIdle)) { return; }
  uint64_t mpidr;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  if (!enter) {
    psciidle::exitIdle (&gIdle, mpidr, idleNow (nullptr));
    return;
  }
  uint64_t ctl, cval;
  __asm__ volatile ("mrs %0, cntv_ctl_el0\n\tmrs %1, cntv_cval_el0" : "=r" (ctl), "=r" (cval));
  psciidle::enterIdle (&gIdle, gIdleIo, mpidr, ctl, cval, idleNow (nullptr));
}

void idleNoteCpu (uint32_t logical, uint64_t mpidr)
{
  psciidle::noteCpu (&gIdle, logical, mpidr);
}

// Boot CPU, before its ml_processor_register: the boot-args. The hook every CPU registers, nullptr while pagani-idle is off, wrong,
// or the GIC is skipped (armcpu-skip-gic: no timer interrupt, so a CPU must never suspend).
processor_idle_t idleConfigure (bool skipGic)
{
  unsigned mode = 0, c4Us = 0, clUs = 0, logSeconds = 30;
  if (!PE_parse_boot_argn ("pagani-idle", &mode, sizeof (mode)) || mode == 0) { return nullptr; }
  PE_parse_boot_argn ("pagani-idle-c4-us", &c4Us, sizeof (c4Us));
  PE_parse_boot_argn ("pagani-idle-cl-us", &clUs, sizeof (clUs));
  PE_parse_boot_argn ("pagani-idle-log", &logSeconds, sizeof (logSeconds));
  gIdleLogSeconds = logSeconds > 3600 ? 3600 : logSeconds;
  uint64_t hz;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (hz));
  if (skipGic || !psciidle::configure (&gIdle, mode, hz, c4Us, clUs)) {
    gIdle.mode = psciidle::kModeOff;
    armcpu_idle_state = 3;
    idleLog (skipGic ? "idle: off: armcpu-skip-gic" : "idle: off: pagani-idle must be 1 or 2");
    return nullptr;
  }
  return &idleNotify;
}

// Secondary CPUs: the same hook, nullptr while pagani-idle is off.
processor_idle_t idleHook (void)
{
  return gIdle.mode != psciidle::kModeOff ? &idleNotify : nullptr;
}

void idleOff (void)
{
  char line[96];
  psciidle::Text t = psciidle::text (line, sizeof (line));
  psciidle::put (&t, "idle: off: ");
  psciidle::put (&t, gIdle.reason != nullptr ? gIdle.reason : "c4 not usable");
  idleLog (line);
  armcpu_idle_state = 3;
}

// One real entry of `state` on the boot CPU before the hook goes on: IRQ/FIQ masked, the virtual timer brought forward to `us` from
// now (or XNU's own deadline when that is earlier), CPU_SUSPEND, then XNU's deadline put back as it was (an early pop only makes
// rtclock_intr resync). Logged before the call, so a hang right there is identified.
bool idleSelfTest (unsigned state, uint32_t us)
{
  char line[160];
  psciidle::Text t = psciidle::text (line, sizeof (line));
  psciidle::put (&t, "idle: self-test ");
  psciidle::put (&t, psciidle::kState[state].name);
  psciidle::put (&t, ' ');
  psciidle::putHex (&t, psciidle::kState[state].param);
  psciidle::put (&t, " with the timer in ");
  psciidle::putDec (&t, us);
  psciidle::put (&t, " us");
  idleLog (line);
  const boolean_t enabled = ml_set_interrupts_enabled (FALSE);
  uint64_t ctl, cval;
  __asm__ volatile ("mrs %0, cntv_ctl_el0\n\tmrs %1, cntv_cval_el0" : "=r" (ctl), "=r" (cval));
  const uint64_t start = idleNow (nullptr);
  uint64_t deadline = start + psciidle::ticks (gIdle.hz, us);
  if ((ctl & 3) == 1 && cval < deadline) { deadline = cval; }
  const uint64_t on = 1;
  __asm__ volatile ("msr cntv_cval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb" :: "r" (deadline), "r" (on) : "memory");
  const int32_t rc = psciidle::result32 (idleCall (nullptr, psciidle::kCpuSuspend64, psciidle::kState[state].param, 0, 0));
  const uint64_t end = idleNow (nullptr);
  __asm__ volatile ("msr cntv_cval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb" :: "r" (cval), "r" (ctl) : "memory");
  ml_set_interrupts_enabled (enabled);
  const bool ok = psciidle::selfTest (&gIdle, state, rc, start, deadline, end);
  psciidle::selfTestLine (&gIdle, state, line, sizeof (line));
  idleLog (line);
  return ok;
}

void idleThread (void *, wait_result_t)
{
  thread_set_thread_name (current_thread (), "pagani-idle");
  psciidle::Snapshot then, now;
  psciidle::snapshot (&gIdle, &gIdleIo, idleNow (nullptr), &then);
  for (;;) {
    IOSleep (gIdleLogSeconds * 1000);
    psciidle::snapshot (&gIdle, &gIdleIo, idleNow (nullptr), &now);
    char line[256];
    psciidle::periodLine (&gIdle, &then, &now, line, sizeof (line));
    idleLog (line);
    then = now;
    if (!psciidle::running (&gIdle)) { armcpu_idle_state = 3; break; }
  }
  thread_terminate (current_thread ());
}

// Boot CPU, after initCPU (its GIC interface and timer interrupt are on) and before any secondary CPU starts (SET_SUSPEND_MODE
// needs the others off): probe, self-tests, on, the log thread.
void idleStart (void)
{
  if (gIdle.mode == psciidle::kModeOff) { return; }
  armcpu_idle_state = 1;
  bool hvc = false;
  if (!psciConduit (&hvc)) {
    psciidle::fail (&gIdle, "no /psci method");
    idleOff ();
    return;
  }
  gIdleHvc = hvc;
  idleLog (hvc ? "idle: probing PSCI over HVC" : "idle: probing PSCI over SMC");
  char line[256];
  const bool probed = psciidle::probe (&gIdle, gIdleIo);
  psciidle::factsLine (&gIdle, line, sizeof (line));
  idleLog (line);
  if (!probed) { idleOff (); return; }
  if (!idleSelfTest (psciidle::kC4, 2000)) { idleOff (); return; }
  if (psciidle::usable (&gIdle, psciidle::kCl5)) { idleSelfTest (psciidle::kCl5, 12000); }
  if (!psciidle::start (&gIdle)) { idleOff (); return; }
  armcpu_idle_state = 2;
  psciidle::configLine (&gIdle, line, sizeof (line));
  idleLog (line);
  if (gIdleLogSeconds == 0) { return; }
  thread_t thread;
  if (kernel_thread_start (&idleThread, nullptr, &thread) != KERN_SUCCESS) { idleLog ("idle: no log thread"); return; }
  thread_deallocate (thread);
}
}  // namespace

IOService *ArmCpu::probe (IOService *provider, SInt32 *score)
{
  OSData *type = OSDynamicCast (OSData, provider->getProperty ("device_type"));
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  uint32_t id;
  if ((type == nullptr) || (type->getLength () != 4) ||
      (memcmp (type->getBytesNoCopy (), "cpu", 4) != 0) ||
      (reg == nullptr) || (reg->getLength () < sizeof (id))) {
    return nullptr;
  }
  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));
  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {
    unsigned smp = 0;
    PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
    if (smp == 0 || ml_get_cpu_number (id) < 0 || ml_get_cpu_number (id) >= ARMCPU_SMP_SLOTS) {
      return nullptr; // Secondary startup is opt-in (sheng-smp=1).
    }
  }
  return IOCPU::probe (provider, score);
}

bool ArmCpu::mapGic (void)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath ("/arm-io", gIODTPlane);
  IORegistryEntry *gic = IORegistryEntry::fromPath ("/arm-io/intc", gIODTPlane);
  if ((soc == nullptr) || (gic == nullptr)) {
    if (soc != nullptr) { soc->release (); }
    if (gic != nullptr) { gic->release (); }
    return false;
  }
  OSData *ranges = OSDynamicCast (OSData, soc->getProperty ("ranges"));
  OSData *reg = OSDynamicCast (OSData, gic->getProperty ("reg"));
  OSData *compatible = OSDynamicCast (OSData, gic->getProperty ("compatible"));
  uint64_t bus[3];
  uint64_t regions[4];
  bool valid = (ranges != nullptr) && (ranges->getLength () == sizeof (bus)) &&
               (reg != nullptr) && (reg->getLength () == sizeof (regions)) &&
               (compatible != nullptr) && (compatible->getLength () == 11) &&
               (memcmp (compatible->getBytesNoCopy (), "arm,gic-v3", 11) == 0);
  if (valid) {
    memcpy (bus, ranges->getBytesNoCopy (), sizeof (bus));
    memcpy (regions, reg->getBytesNoCopy (), sizeof (regions));
  }
  OSData *strideProperty = OSDynamicCast (OSData, gic->getProperty ("redistributor-stride"));
  if (strideProperty != nullptr && strideProperty->getLength () == sizeof (uint64_t)) {
    memcpy (&gRedistStride, strideProperty->getBytesNoCopy (), sizeof (uint64_t));
    if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) { gRedistStride = 0; }
  }
  armcpu_gic_stride = static_cast<uint32_t> (gRedistStride);
  soc->release ();
  gic->release ();
  if (!valid || (regions[1] < 0x10000) || (regions[3] < 0x20000)) {
    return false;
  }
  for (unsigned index = 0; index < 4; index += 2) {
    if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
        (regions[index + 1] > bus[2] - (regions[index] - bus[0])) ||
        (bus[1] + regions[index] - bus[0] < bus[1])) {
      return false;
    }
  }
  OSSharedPtr<IOMemoryDescriptor> distMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut), OSNoRetain);
  OSSharedPtr<IOMemoryDescriptor> rdMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut), OSNoRetain);
  if (!distMemory || !rdMemory) {
    return false;
  }
  distributor = OSSharedPtr<IOMemoryMap> (distMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  redistributors = OSSharedPtr<IOMemoryMap> (rdMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  if (!distributor || !redistributors) {
    return false;
  }
  dist = static_cast<uintptr_t> (distributor->getVirtualAddress ());
  if (((read32 (dist, 0xffe8) >> 4) & 15) != 3) {
    return false;
  }
  redistBase = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
  redistSize = regions[3];
  gDistMap = distributor;
  gRedistMap = redistributors;
  gDist = dist;
  gFrames = redistBase;
  gFramesSize = redistSize;
  return true;
}

bool ArmCpu::initDistributor (void)
{
  uint32_t ctl = read32 (dist, 0);
  write32 (dist, 0, ctl & ~3U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  write32 (dist, 0, (ctl & ~3U) | 16U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
  for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
    write32 (dist, 0x180 + id / 8, 0xffffffff);
  }
  return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor (bool local)
{
  uintptr_t frames = redistBase;
  // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
  // from the DT affinity, so the new core never needs GIC MMIO of its own.
  if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
  uint32_t target = static_cast<uint32_t> ((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
  redist = 0;
  for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
    armcpu_gic_typer_probe[0] = static_cast<uint32_t> (offset);
    uint64_t typer = *reinterpret_cast<volatile uint64_t *> (frames + offset + 8);
    armcpu_gic_typer_probe[1] = static_cast<uint32_t> (typer);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      redist = frames + offset;
      break;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  if (redist == 0) { return false; }
  write32 (redist, 0x14, read32 (redist, 0x14) & ~2U);
  if (!waitClear (redist, 0x14, 4)) { return false; }
  uintptr_t sgi = redist + 0x10000;
  write32 (sgi, 0x180, 0xffffffff);
  write32 (sgi, 0x80, 0xffffffff);
  write32 (sgi, 0xd00, 0);
  write32 (sgi, 0x280, 0xffffffff);
  for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
    write32 (sgi, offset, 0x80808080);
  }
  if (!local) { write32 (sgi, 0x100, kSgiPpiEnable); }
  __asm__ volatile ("dsb sy" ::: "memory");
  return waitClear (redist, 0, 8);
}

bool ArmCpu::prepareGic (void)
{
  kprintf ("ArmCpu: map GICv3\n");
  if (!mapGic ()) { kprintf ("ArmCpu: GIC mapping failed\n"); return false; }
  kprintf ("ArmCpu: initialize distributor\n");
  if (!initDistributor ()) { kprintf ("ArmCpu: distributor initialization failed\n"); return false; }
  kprintf ("ArmCpu: wake boot redistributor\n");
  if (!wakeRedistributor (true)) { kprintf ("ArmCpu: redistributor wake failed\n"); return false; }
  gGicShared = 1;
  return true;
}

void ArmCpu::enableGic (void)
{
  kprintf ("ArmCpu: enable CPU GIC interface\n");
  uint64_t value;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  value |= 1;
  __asm__ volatile ("msr icc_sre_el1, %0\n\tisb" :: "r" (value) : "memory");
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  if ((value & 1) == 0) { panic ("ArmCpu: GICv3 system register interface unavailable"); }
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (value));
  value &= ~2ULL; // EOImode 0: EOIR also deactivates the interrupt.
  __asm__ volatile ("msr icc_ctlr_el1, %0" :: "r" (value) : "memory");
  value = 0xff;
  __asm__ volatile ("msr icc_pmr_el1, %0" :: "r" (value) : "memory");
  value = 0;
  __asm__ volatile ("msr icc_bpr1_el1, %0" :: "r" (value) : "memory");
  if (!secondary) { gBootAffinity = affinity & 0xff00ffffffULL; }
  if (!secondary || !gRemoteGic) {
    write32 (redist + 0x10000, 0x100, kSgiPpiEnable);
    write32 (dist, 0, read32 (dist, 0) | 0x12U);
  }
  value = 1;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
  if (!secondary) {
    uint64_t pmr, ig, sre;
    __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
    __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (ig));
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
    armcpu_gic_boot[0] = read32 (redist + 0x10000, 0x80);
    armcpu_gic_boot[1] = read32 (redist + 0x10000, 0x100);
    armcpu_gic_boot[2] = read32 (dist, 0);
    armcpu_gic_boot[3] = read32 (dist, 4);
    armcpu_gic_boot[4] = read32 (redist + 0x10000, 0xd00);
    armcpu_gic_boot[5] = static_cast<uint32_t> (pmr);
    armcpu_gic_boot[6] = static_cast<uint32_t> (ig);
    armcpu_gic_boot[7] = static_cast<uint32_t> (sre);
  }
  armcpu_gic_ready = 1;
  if (!secondary) {
    kprintf ("ArmCpu: GIC ready PPI=%08x PMR=%x IGRPEN1=%x SRE=%x\n",
             armcpu_gic_boot[1], armcpu_gic_boot[5], armcpu_gic_boot[6], armcpu_gic_boot[7]);
  }
}


bool ArmCpu::start (IOService *provider)
{
  kprintf ("ArmCpu: start\n");
  unsigned skip = 0;
  PE_parse_boot_argn ("armcpu-skip-gic", &skip, sizeof (skip));
  skipGic = skip != 0;
  unsigned selfIpi = 0;
  PE_parse_boot_argn ("armcpu-self-ipi", &selfIpi, sizeof (selfIpi));
  gSelfIpi = selfIpi == 1;
  unsigned spiIpi = 0;
  PE_parse_boot_argn ("armcpu-spi-ipi", &spiIpi, sizeof (spiIpi));
  gSpiIpi = spiIpi == 1;
  unsigned diagnostic = 0;
  PE_parse_boot_argn("armcpu-diagnostics", &diagnostic, sizeof(diagnostic));
  diagnosePower=diagnostic==1;
  uint32_t physical;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  memcpy (&physical, reg->getBytesNoCopy (), sizeof (physical));
  logicalId = static_cast<uint32_t> (ml_get_cpu_number (physical));
  secondary = logicalId != static_cast<uint32_t> (ml_get_boot_cpu_number ());
  cpufreqNoteCpu (logicalId, physical);
  idleNoteCpu (logicalId, physical);
  if (secondary) {
    if (!IOCPU::start (provider)) { return false; }
    armcpu_smp_step[logicalId] = 1;
    affinity = physical;
    armcpu_smp_mpidr[logicalId] = physical;
    cpuName = OSSymbol::withCStringNoCopy ("ARM architectural secondary CPU");
    if (cpuName == nullptr) { return false; }
    retain ();
    __atomic_store_n (&gPending[logicalId], this, __ATOMIC_RELEASE);
    if (gBootRegistered != 0) { drainPending (); }
    return true;
  } else {
    uint64_t bootMpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (bootMpidr));
    armcpu_boot_mpidr = static_cast<uint32_t> (bootMpidr);
    if ((!skipGic && !prepareGic ()) || !IOCPU::start (provider)) { return false; }
  }
  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");
  if (cpuName == nullptr) { return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = physical;
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (physical));
  info.cluster_type = ml_get_boot_cluster_type ();
  info.processor_idle = idleConfigure (skipGic);   // pagani-idle; nullptr (XNU's plain WFI) when off
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { panic ("ArmCpu: CPU %u registration failed: %d", logicalId, status); }
  setCPUNumber (info.log_id);
  armcpu_registered = 1;
  unsigned smp = 0;
  PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
  gSmpWanted = smp != 0;
  unsigned remote = 0;
  PE_parse_boot_argn ("sheng-smp-gic", &remote, sizeof (remote));
  gRemoteGic = remote != 0;
  armcpu_smp_requested = gSmpWanted ? 1 : 0;
  unsigned cpuCount = static_cast<unsigned> (ml_get_max_cpu_number ()) + 1;
  controller = new IOCPUInterruptController;
  if ((controller == nullptr) || (controller->initCPUInterruptController (cpuCount, 1) != kIOReturnSuccess)) {
    panic ("ArmCpu: CPU interrupt controller allocation failed");
  }
  controller->registerCPUInterruptController ();
  controller->enableCPUInterrupt (this);
  if (controller->registerInterrupt (provider, logicalId, this, &ArmCpu::dispatch, nullptr) != kIOReturnSuccess) {
    panic ("ArmCpu: CPU IRQ action registration failed");
  }
  gController = controller;
  initCPU (true);
  idleStart ();   // pagani-idle: probe and self-test on this CPU while the secondaries are still off
  registerService ();
  __atomic_store_n (&gBootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending ();
  cpufreqStart ();
  return true;
}

void ArmCpu::drainPending (void)
{
  for (unsigned id = 1; id < ARMCPU_SMP_SLOTS; id++) {
    ArmCpu *cpu = __atomic_exchange_n (&gPending[id], static_cast<ArmCpu *> (nullptr), __ATOMIC_ACQ_REL);
    if (cpu != nullptr) { cpu->finishSecondary (); }
  }
}

bool ArmCpu::finishSecondary (void)
{
  dist = gDist;
  redistBase = gFrames;
  redistSize = gFramesSize;
  if (gRemoteGic && !wakeRedistributor (false)) { armcpu_smp_fail[logicalId] = 3; return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = static_cast<uint32_t> (affinity);
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (static_cast<uint32_t> (affinity)));
  info.cluster_type = ml_get_boot_cluster_type ();
  info.processor_idle = idleHook ();   // pagani-idle; nullptr when off
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }
  setCPUNumber (info.log_id);
  armcpu_smp_step[logicalId] = 2;
  setCPUState (kIOCPUStateUninitalized);
  registerService ();
  startSecondary ();
  return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit (bool *hvc)
{
  IORegistryEntry *psci = IORegistryEntry::fromPath ("/psci", gIODTPlane);
  if (psci == nullptr) { return false; }
  OSData *method = OSDynamicCast (OSData, psci->getProperty ("method"));
  bool ok = false;
  if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "smc", 4) == 0) {
    *hvc = false; ok = true;
  } else if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "hvc", 4) == 0) {
    *hvc = true; ok = true;
  }
  psci->release ();
  return ok;
}

static int64_t psciCall (bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (hvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

void ArmCpu::startThread (void *argument, wait_result_t)
{
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  armcpu_smp_step[self->logicalId] = 3;
  // Blocks until the new core reaches the scheduler; a core that never arrives only stalls this helper thread.
  processor_start (self->machProcessor);
  thread_terminate (current_thread ());
}

void ArmCpu::startSecondary (void)
{
  thread_t thread;
  if (kernel_thread_start (&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
    armcpu_smp_psci_rc[logicalId] = -1000;
    return;
  }
  thread_deallocate (thread);
}

void ArmCpu::initCPU (bool boot)
{
  if (secondary) {
    if (!boot) { panic ("ArmCpu: secondary resume is unsupported"); }
    armcpu_smp_step[logicalId] = 5;
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t> (mpidr);
    // A failure is recorded and the core parked (rather than a panic that hides the other cores' state).
    if ((mpidr & 0xff00ffffffULL) != (affinity & 0xff00ffffffULL)) { armcpu_smp_fail[logicalId] = 1; for (;;) { __asm__ volatile ("wfe"); } }
    if (!gRemoteGic && !wakeRedistributor (true)) { armcpu_smp_fail[logicalId] = 2; for (;;) { __asm__ volatile ("wfe"); } }
    armcpu_smp_step[logicalId] = 6;
    enableGic ();
    if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
    // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
    // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
    ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
    enableCPUTimeBase (true);
    setCPUState (kIOCPUStateRunning);
    armcpu_smp_step[logicalId] = 7;
    __atomic_add_fetch (&armcpu_smp_running, 1, __ATOMIC_RELAXED);
    // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
    // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
    if (gSelfIpi) { signalCPU (this); }
    return;
  }
  if (!boot) { panic ("ArmCpu: secondary/resume CPU startup is unsupported"); }
  if (skipGic) {
    // Explicit A/B hardware diagnostic: do not touch GIC MMIO or enable an
    // interrupt source. CPU registration still happens; the timer is left off.
    setCPUState (kIOCPUStateRunning);
    return;
  }
  enableGic ();
  if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
  enableCPUTimeBase (true);
  setCPUState (kIOCPUStateRunning);
  if (gSelfIpi) { signalCPU (this); }
}

void ArmCpu::dispatch (void *target, void *, void *, int)
{
  static_cast<ArmCpu *> (target)->interrupt ();
}

void ArmCpu::interrupt (void)
{
  unsigned drained=0;
  for (;;) {
    armcpu_irq_phase=1;
    uint64_t id;
    __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));
    armcpu_irq_id=static_cast<uint32_t>(id & 0xffffff);
    if ((id & 0xffffff) >= 1020) { armcpu_irq_phase=0;break; }
    if(diagnosePower && ++drained>128)panic("ArmCpu: IRQ drain exceeds 128 id=%llx timer=%llu IPI=%llu",id,armcpu_timer_interrupts,armcpu_ipi_interrupts);
    if ((id & 0xffffff) == 27) {
      __atomic_add_fetch (&armcpu_timer_interrupts, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch (&armcpu_core_timer[logicalId & 7], 1, __ATOMIC_RELAXED);
      armcpu_irq_phase=2;
      rtclock_intr (0);
      armcpu_irq_phase=3;
    } else if ((id & 0xffffff) == 0) {
      __atomic_add_fetch (&armcpu_ipi_interrupts, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch (&armcpu_core_sgi[logicalId & 7], 1, __ATOMIC_RELAXED);
      armcpu_irq_phase=4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase=5;
    } else if (gSpiIpi && (id & 0xffffff) >= kIpiSpiBase && (id & 0xffffff) < kIpiSpiBase + 8) {
      __atomic_add_fetch (&armcpu_ipi_interrupts, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch (&armcpu_core_sgi[logicalId & 7], 1, __ATOMIC_RELAXED);
      armcpu_irq_phase = 4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase = 5;
    } else {
      const uint32_t intid = static_cast<uint32_t> (id & 0xffffff);
      ArmCpuSpi *spi = nullptr;
      const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
      for (uint32_t slot = 0; intid >= 32 && slot < used && spi == nullptr; slot++) {
        if (gSpi[slot].id == intid) { spi = &gSpi[slot]; }
      }
      if (spi != nullptr) {
        armcpu_irq_phase = 7;
        spi->count = spi->count + 1;
        spi->handler (spi->context);
      } else if (intid >= 32 && gDist != 0) {
        __atomic_add_fetch (&armcpu_spi_unhandled, 1, __ATOMIC_RELAXED);
        armcpu_spi_unhandled_id = intid;
        write32 (gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
      } else {
        panic ("ArmCpu: unexpected architectural IRQ %llu", id);
      }
    }
    armcpu_irq_phase=6;
    __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (id) : "memory");
  }
}

void ArmCpu::signalCPU (IOCPU *target)
{
  ArmCpu *peer = OSDynamicCast (ArmCpu, target);
  if (peer == nullptr || (peer->signalTarget () & 0xff) >= 16) {
    panic ("ArmCpu: unsupported CPU signal target");
  }
  uint64_t aff = peer->signalTarget ();
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) |
                 ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  __atomic_add_fetch (&armcpu_ipi_sent, 1, __ATOMIC_RELAXED);
  if (gSpiIpi) { spiIpiSend (gDist, peer->logicalId & 7); return; }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_ipi_interrupts before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;
extern "C" void armcpu_gic_diag (uint64_t aff, uint32_t *out)
{
  for (unsigned i = 0; i < 8; i++) { out[i] = 0xdeadbeef; }
  if (gDist == 0 || gFrames == 0) { armcpu_gic_diag_stage = 0xe; return; }
  const uint32_t target = static_cast<uint32_t> ((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
  for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
    armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t> (offset >> 16);
    const uint64_t typer = *reinterpret_cast<volatile uint64_t *> (gFrames + offset + 8);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
      armcpu_gic_diag_stage = 2; out[0] = read32 (sgi, 0x80);
      armcpu_gic_diag_stage = 3; out[1] = read32 (sgi, 0x100);
      armcpu_gic_diag_stage = 4; out[2] = read32 (sgi, 0x200);
      armcpu_gic_diag_stage = 5; out[3] = read32 (sgi, 0xd00);
      armcpu_gic_diag_stage = 6; out[4] = read32 (redist, 0x14);
      armcpu_gic_diag_stage = 7; out[5] = read32 (gDist, 0);
      armcpu_gic_diag_stage = 8; out[6] = read32 (sgi, 0x400);
      out[7] = 1;
      armcpu_gic_diag_stage = 9;
      return;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe (uint64_t *out)
{
  uint64_t mpidr, daif, v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  __asm__ volatile ("mrs %0, daif\n\tmsr daifset, #2\n\tisb" : "=r" (daif) :: "memory");
  for (unsigned i = 0; i < 10; i++) { out[i] = 0; }
  out[0] = mpidr;
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (v)); out[3] = v;
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (v)); out[4] = v;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (v)); out[5] = v;
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (v)); out[6] = v;
  __asm__ volatile ("mrs %0, icc_rpr_el1" : "=r" (v)); out[7] = v;
  if ((mpidr & 0xff) < 16) {
    const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) | (1ULL << (mpidr & 0xff));
    for (uint64_t intid = 0; intid < 16; intid++) {
      const uint64_t sgi = base | (intid << 24);
      uint64_t h = 1023, ta, tb;
      __asm__ volatile ("dsb ishst\n\tisb\n\tmrs %0, cntvct_el0\n\tisb" : "=r" (ta) :: "memory");
      __asm__ volatile ("msr icc_sgi1r_el1, %1\n\tisb\n\tmrs %0, cntvct_el0" : "=r" (tb) : "r" (sgi) : "memory");
      if (intid == 0) { out[8] = tb - ta; }
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[1] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (v) : "memory");
        continue;
      }
      __asm__ volatile ("dsb ishst\n\tmsr icc_sgi0r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir0_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[2] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar0_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir0_el1, %0\n\tisb" :: "r" (v) : "memory");
      }
    }
  }
  {
    uint64_t ta, tb;
    __asm__ volatile ("isb\n\tmrs %0, cntvct_el0\n\tisb\n\tmrs %2, icc_pmr_el1\n\tisb\n\tmrs %1, cntvct_el0" : "=r" (ta), "=r" (tb), "=r" (v) :: "memory");
    out[9] = tb - ta;
  }
  __asm__ volatile ("msr daif, %0\n\tisb" :: "r" (daif) : "memory");
}

extern "C" void armcpu_ipi_selftest (uint64_t aff)
{
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  __atomic_add_fetch (&armcpu_ipi_selftest_sent, 1, __ATOMIC_RELAXED);
  if (gSpiIpi) {
    int cpu = ml_get_cpu_number (static_cast<uint32_t> (aff));
    if (cpu >= 0 && cpu < 8) { spiIpiSend (gDist, static_cast<uint32_t> (cpu)); }
    return;
  }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase (bool enabled)
{
  uint64_t value = enabled ? 1 : 2;
  __asm__ volatile ("msr cntv_ctl_el0, %0\n\tisb" :: "r" (value) : "memory");
  if (enabled && !secondary) {
    uint64_t frequency, control;
    __asm__ volatile ("mrs %0, cntfrq_el0\n\tmrs %1, cntv_ctl_el0" : "=r" (frequency), "=r" (control));
    kprintf ("ArmCpu: virtual timer Hz=%llu CTL=%llx, PPI 27\n", frequency, control);
  }
}

void ArmCpu::quiesceCPU (void)
{
  armcpu_quiesce_calls=armcpu_quiesce_calls+1;
  if(diagnosePower)panic("ArmCpu: unsupported quiesce requested before timer/GIC disable caller=%p halt=%u timer=%llu phase=%u",__builtin_return_address(0),armcpu_halt_calls,armcpu_timer_interrupts,armcpu_irq_phase);
  enableCPUTimeBase (false);
  uint64_t value = 0;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::haltCPU (void)
{
  armcpu_halt_calls=armcpu_halt_calls+1;
  quiesceCPU ();
  setCPUState (kIOCPUStateStopped);
  __asm__ volatile ("msr daifset, #15" ::: "memory");
  for (;;) { __asm__ volatile ("wfi"); }
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = static_cast<uintptr_t> (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1), end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) { __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory"); }
  __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
}

kern_return_t ArmCpu::startCPU (vm_offset_t, vm_offset_t)
{
  if (!secondary) { return KERN_NOT_SUPPORTED; }
  bool hvc = false;
  if (!psciConduit (&hvc)) { armcpu_smp_psci_rc[logicalId] = -1001; return KERN_NOT_SUPPORTED; }
  armcpu_smp_conduit = hvc ? 2 : 1;
  // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
  // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
  uint64_t entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (LowResetVectorBase)));
  cleanToMemory (LowResetVectorBase, 0x10000);   // reset vector, start_cpu and the code up to the MMU enable
  cleanToMemory (ResetHandlerData, 0x1000);
  cleanToMemory (const_boot_args, 0x1000);
  armcpu_smp_step[logicalId] = 4;
  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);
  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);
  return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName (void)
{
  return cpuName;
}

// Readback after configuration: bit 0 group 1, bit 1 enabled, bit 2 edge, bits 8..15 priority; IROUTER separately.
extern "C" IOReturn armcpu_spi_attach (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                       uint32_t *readback, uint64_t *router)
{
  if (gDist == 0 || armcpu_gic_ready == 0 || handler == nullptr) { return kIOReturnNotReady; }
  const unsigned interrupts = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (intid < 32 || intid >= 1020 || intid >= interrupts) { return kIOReturnBadArgument; }
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return kIOReturnExclusiveAccess; }
  }
  if (used >= ARMCPU_SPI_SLOTS) { return kIOReturnNoResources; }
  const uint32_t word = (intid / 32) * 4, bit = 1U << (intid % 32);
  write32 (gDist, 0x180 + word, bit); // ICENABLER while it is reconfigured
  if (!waitClear (gDist, 0, 1U << 31)) { return kIOReturnTimeout; }
  write32 (gDist, 0x80 + word, read32 (gDist, 0x80 + word) | bit); // IGROUPR: group 1 (RAZ/WI if secure owns it)
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  const unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1U << shift)) | ((edge ? 1U : 0U) << shift));
  *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid) = gBootAffinity;
  gSpi[used] = {intid, handler, context, 0};
  __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x100 + word, bit); // ISENABLER
  if (readback != nullptr) {
    *readback = ((read32 (gDist, 0x80 + word) & bit) ? 1U : 0U) | ((read32 (gDist, 0x100 + word) & bit) ? 2U : 0U) |
                (((read32 (gDist, cfg) >> shift) & 1U) << 2) |
                (static_cast<uint32_t> (*reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid)) << 8);
  }
  if (router != nullptr) { *router = *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid); }
  return kIOReturnSuccess;
}

extern "C" uint64_t armcpu_spi_count (uint32_t intid)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return gSpi[slot].count; }
  }
  return 0;
}

// Switch an attached SPI off (GICD_ICENABLER, then RWP) or on again (GICD_ISENABLER); the slot and the line's configuration
// stay. Only an INTID attached through armcpu_spi_attach is accepted (kIOReturnBadArgument otherwise). A driver's storm guard
// calls disable from its own handler, before the EOI, so neither takes a lock or sleeps.
extern "C" IOReturn armcpu_spi_disable (uint32_t intid);
extern "C" IOReturn armcpu_spi_enable (uint32_t intid);

static bool spiRwpClear (volatile uint32_t *)
{
  return waitClear (gDist, 0, 1U << 31);
}

static IOReturn spiGate (uint32_t intid, bool enable)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  switch (armCpuSpiGate (reinterpret_cast<volatile uint32_t *> (gDist), gDist != 0 && armcpu_gic_ready != 0,
                         armCpuSpiAttached (gSpi, used, intid), intid, enable, spiRwpClear)) {
  case ArmCpuSpiGateOk: return kIOReturnSuccess;
  case ArmCpuSpiGateNotReady: return kIOReturnNotReady;
  case ArmCpuSpiGateNotAttached: return kIOReturnBadArgument;
  case ArmCpuSpiGateTimeout: return kIOReturnTimeout;
  default: break;
  }
  return kIOReturnError;
}

extern "C" IOReturn armcpu_spi_disable (uint32_t intid)
{
  return spiGate (intid, false);
}

extern "C" IOReturn armcpu_spi_enable (uint32_t intid)
{
  return spiGate (intid, true);
}
