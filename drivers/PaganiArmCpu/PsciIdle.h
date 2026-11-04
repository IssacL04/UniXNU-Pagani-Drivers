// SPDX-License-Identifier: BSD-2-Clause-Patent
// pagani-idle: XNU's CPU idle through PSCI CPU_SUSPEND retention states on the OnePlus 13T (SM8750 under Gunyah). The choice of
// state, the accounting, the safety switches and the log text live here, free of kernel headers: Test/psci-idle.cpp runs them on
// the host against a fake PSCI. ArmCpu.cpp registers the processor_idle hook that runs them and makes the PSCI calls.
//
// What this rests on:
// - XNU's cpu_idle (osfmk/arm64/cpu.c) calls the processor_idle hook given to ml_processor_register with IRQ and FIQ masked,
//   right before its own WFI and after SetIdlePop programmed the next timer deadline (CNTV_CVAL_EL0). ArmCpu registered no hook
//   so far: an idle CPU only executes WFI, the shallowest state.
// - PSCI CPU_SUSPEND (0xc4000001) of a standby/retention power_state returns to the caller once a wake-up event is pending (an
//   interrupt, masked or not); that interrupt stays pending, so XNU's WFI right after returns at once and the interrupt is taken
//   as usual. An error return leaves XNU's ordinary WFI. Linux calls these states the same way (psci_cpu_suspend_enter: no
//   context lost, entry point 0).
// - The PKX110 runtime DT (/cpus/idle-states; Android cpuidle "psci_idle" with the "qcom-cpu-lpm" governor; on 2026-10-30 CPU0
//   had 108.7 s in "ret" against 61.2 s in WFI, CPU6 193.1 s against 4.6 s) describes three states, all of retention type
//   (bits 30 and 16 clear in either power_state format), none with local-timer-stop (the CPU's own timer keeps waking it):
//     ret          0x00000004  CPU0-5: entry 93 us, exit 129 us, residency 560 us; CPU6-7: 172 / 130 / 686 us
//     ret-pll-off  0x01000054  the cluster (CLUSTER_PD0, PD1): entry 2150 us, exit 1983 us, residency 9144 us
//     apps-pc      0x0200c354  the APSS (CLUSTER_PD2): entry 2800 us, exit 4400 us, residency 10150 us
//   apps-pc is never requested here: Android enters it only after flushing the apps RSC sleep/wake sets (apps_rsc has
//   power-domains = CLUSTER_PD2), which XNU does not have, and its wake sources then go through the PDC.
// - XNU sees PSCI 1.1 over SMC (BootProgress 'T' record of the 13T loop runs: "psci=1.1 conduit=smc(/psci)").
//
// Modes (pagani-idle): 0 or absent: no hook, no PSCI call (as before). 1: "ret" (named c4 here) when the next timer is at least
// pagani-idle-c4-us away (default residency + exit latency: 689 us on CPUs 0-5, 816 us on CPUs 6-7). 2: also "ret-pll-off" (cl5)
// when it is at least pagani-idle-cl-us away (default 11127 us). A CPU's request is a vote: in platform-coordinated mode (the PSCI
// default; mode 2 asks for it with SET_SUSPEND_MODE when the firmware also offers OS-initiated mode) the cluster leaves its
// clocks running until every CPU of the cluster votes cl5.
// History: the next timer only bounds an idle from above. A CPU whose last kShortRun idles each ended by another interrupt (before
// its timer) and lasted less than the c4 threshold, as during disk I/O, stays in WFI until an idle lasts longer again: such waits
// would otherwise pay c4's exit latency every time (Linux's cpuidle governors use their own wake-up history the same way).
// Safety: a state that returns an error kFailLimit times in a row on one CPU, or whose CPU wakes more than kLateMs after its timer
// deadline kLateLimit times in a row, is switched off for the boot (XNU's WFI remains); c4 switched off ends PSCI idle altogether.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace psciidle {

// PSCI 1.x function ids. SMC32 calls (VERSION, FEATURES, SET_SUSPEND_MODE) and CPU_SUSPEND report a 32-bit result.
constexpr uint64_t kVersion = 0x84000000u;
constexpr uint64_t kFeatures = 0x8400000au;
constexpr uint64_t kSetSuspendMode = 0x8400000fu;
constexpr uint64_t kCpuSuspend64 = 0xc4000001u;
constexpr uint64_t kCpuDefaultSuspend64 = 0xc400000cu;
constexpr uint64_t kSystemSuspend64 = 0xc400000eu;
constexpr uint64_t kStatResidency64 = 0xc4000010u;
constexpr uint64_t kStatCount64 = 0xc4000011u;
constexpr int32_t kSuccess = 0, kNotSupported = -1, kInvalidParameters = -2, kDenied = -3;
constexpr uint64_t kPlatformCoordinated = 0;          // SET_SUSPEND_MODE argument
constexpr uint64_t kAffinityMask = 0xff00ffffffULL;   // MPIDR Aff3..Aff0
constexpr int32_t kNotCalled = 1;                     // Facts::setModeResult when SET_SUSPEND_MODE was not called

inline int32_t result32 (int64_t raw)
{
  return static_cast<int32_t> (static_cast<uint32_t> (static_cast<uint64_t> (raw)));
}

enum : unsigned { kC4 = 0, kCl5 = 1, kStates = 2 };
struct StateInfo {
  const char *name;
  uint32_t param;
  uint32_t residencyUs[2];   // [cluster]: 0 CPUs 0-5 (MPIDR Aff2 0), 1 CPUs 6-7 (Aff2 1)
  uint32_t exitUs[2];
};
constexpr StateInfo kState[kStates] = {
  {"c4", 0x00000004u, {560, 686}, {129, 130}},      // medium-cluster0-c4 / large-cluster1-c4, idle-state-name "ret"
  {"cl5", 0x01000054u, {9144, 9144}, {1983, 1983}},  // medium-cluster-cl5 / large-cluster-cl5, "ret-pll-off"
};
constexpr uint32_t kAppsPcParam = 0x0200c354u;      // cluster-ss3 "apps-pc": never requested (see above)

enum : uint32_t { kModeOff = 0, kModeCore = 1, kModeCluster = 2 };
// Why a state is off: the mode does not use it, the probe, the boot self-test, errors or late wakes at run time.
enum : uint32_t { kCauseNone = 0, kCauseMode = 1, kCauseProbe = 2, kCauseSelfTest = 3, kCauseError = 4, kCauseLate = 5 };
constexpr unsigned kCpus = 8;
constexpr uint32_t kFailLimit = 4;          // error returns in a row on one CPU
constexpr uint32_t kLateLimit = 3;          // late wakes in a row on one CPU
constexpr uint32_t kLateMs = 50;            // a wake this long after the timer deadline is late (exit latencies are at most 2 ms)
constexpr uint32_t kShortRun = 2;           // short idles ended by another interrupt, in a row, before WFI is preferred
constexpr uint32_t kMinThresholdUs = 100, kMaxThresholdUs = 1000000;

// The platform: PSCI calls (function, x1..x3; the raw x0 comes back) and the virtual counter.
struct Io {
  void *context;
  int64_t (*call) (void *context, uint64_t function, uint64_t a, uint64_t b, uint64_t c);
  uint64_t (*now) (void *context);
};

struct Facts {
  bool probed;
  uint32_t version;
  int32_t suspend;          // PSCI_FEATURES(CPU_SUSPEND): bit 1 extended StateID format, bit 0 OS-initiated mode
  int32_t setMode, systemSuspend, defaultSuspend, statCount, statResidency;   // PSCI_FEATURES of those calls
  int32_t setModeResult;    // SET_SUSPEND_MODE(platform-coordinated), kNotCalled when not called
};

struct SelfTest {
  bool ran;
  int32_t rc;
  uint64_t start, deadline, end;
};

// One CPU. The idle hook of that CPU is its only writer (noteCpu aside); the log thread reads the counters.
struct Cpu {
  uint32_t known;           // set (release) after affinity and cluster
  uint32_t cluster;
  uint64_t affinity;
  uint64_t entries[kStates], ticks[kStates], fails[kStates], late[kStates];
  uint32_t failRun[kStates], lateRun[kStates];
  uint64_t shortIdle;       // idle entries left to XNU's WFI: the timer was too close
  uint64_t predicted;       // idle entries left to XNU's WFI: the recent idles were short (history)
  uint64_t enteredAt, enteredDeadline;   // the idle in progress: counter at entry and its timer deadline (armed or not)
  uint32_t enteredArmed, shortRun;       // ... whether that timer was armed; short idles ended early, in a row
  uint64_t resumed;         // counter when the last CPU_SUSPEND returned, until the exit hook measured the path after it
  uint64_t postTicks, postCount, postMax;
  int32_t lastError;
};

struct Control {
  uint32_t mode;
  uint32_t on;              // 1 while the hook may suspend (acquire/release)
  uint32_t disabled;        // bit per state that is off
  uint32_t cause[kStates];
  uint64_t hz, lateTicks;
  uint64_t threshold[kStates][2];
  const char *reason;       // why PSCI idle is off before it ran (probe, self-test), nullptr otherwise
  Facts facts;
  SelfTest test[kStates];
  Cpu cpu[kCpus];
};

template <typename T> inline T load (const T *p) { return __atomic_load_n (p, __ATOMIC_RELAXED); }
template <typename T> inline void store (T *p, T v) { __atomic_store_n (p, v, __ATOMIC_RELAXED); }
// Single-writer counters: a plain read-modify-write is enough, the stores stay whole for the reader.
inline void bump (uint64_t *p) { store (p, load (p) + 1); }
inline void add (uint64_t *p, uint64_t v) { store (p, load (p) + v); }

inline uint64_t ticks (uint64_t hz, uint64_t us)
{
  return us / 1000000u * hz + ((us % 1000000u) * hz + 999999u) / 1000000u;
}

inline uint64_t microseconds (uint64_t hz, uint64_t t)
{
  return hz == 0 ? 0 : t / hz * 1000000u + (t % hz) * 1000000u / hz;
}

inline bool running (const Control *c)
{
  return __atomic_load_n (&c->on, __ATOMIC_ACQUIRE) != 0;
}

inline bool usable (const Control *c, unsigned state)
{
  return (load (&c->disabled) & (1u << state)) == 0;
}

// Switches a state off for the boot; c4 off ends PSCI idle (the hook then returns at once and XNU's WFI is all that remains).
inline void disable (Control *c, unsigned state, uint32_t cause)
{
  if (state >= kStates) { return; }
  if ((__atomic_fetch_or (&c->disabled, 1u << state, __ATOMIC_RELAXED) & (1u << state)) == 0) {
    store (&c->cause[state], cause);
  }
  if (state == kC4) { __atomic_store_n (&c->on, 0u, __ATOMIC_RELEASE); }
}

inline bool fail (Control *c, const char *reason)
{
  c->reason = reason;
  __atomic_store_n (&c->on, 0u, __ATOMIC_RELEASE);
  return false;
}

inline uint32_t clampUs (uint32_t us)
{
  return us < kMinThresholdUs ? kMinThresholdUs : (us > kMaxThresholdUs ? kMaxThresholdUs : us);
}

// pagani-idle, pagani-idle-c4-us and pagani-idle-cl-us (0: the defaults), the counter frequency. False (and the mode off) for a
// mode other than 1 or 2 or no counter frequency: nothing must then run.
inline bool configure (Control *c, uint32_t mode, uint64_t hz, uint32_t c4Us, uint32_t clUs)
{
  c->mode = kModeOff;
  c->hz = hz;
  if ((mode != kModeCore && mode != kModeCluster) || hz == 0) { return false; }
  c->mode = mode;
  for (unsigned cluster = 0; cluster < 2; cluster++) {
    const uint32_t core = c4Us != 0 ? clampUs (c4Us) : kState[kC4].residencyUs[cluster] + kState[kC4].exitUs[cluster];
    uint32_t deep = clUs != 0 ? clampUs (clUs) : kState[kCl5].residencyUs[cluster] + kState[kCl5].exitUs[cluster];
    if (deep < core) { deep = core; }     // the cluster state is never preferred for a shorter idle than the core state
    c->threshold[kC4][cluster] = ticks (hz, core);
    c->threshold[kCl5][cluster] = ticks (hz, deep);
  }
  c->lateTicks = ticks (hz, uint64_t (kLateMs) * 1000u);
  if (mode == kModeCore) { disable (c, kCl5, kCauseMode); }
  return true;
}

// Each CPU as its nub starts: its logical number and its DT affinity (MPIDR value).
inline void noteCpu (Control *c, uint32_t logical, uint64_t mpidr)
{
  if (logical >= kCpus) { return; }
  Cpu *p = &c->cpu[logical];
  p->affinity = mpidr & kAffinityMask;
  p->cluster = ((mpidr >> 16) & 0xff) != 0 ? 1 : 0;
  __atomic_store_n (&p->known, 1u, __ATOMIC_RELEASE);
}

inline int slotOf (const Control *c, uint64_t mpidr)
{
  const uint64_t affinity = mpidr & kAffinityMask;
  for (unsigned i = 0; i < kCpus; i++) {
    if (__atomic_load_n (&c->cpu[i].known, __ATOMIC_ACQUIRE) != 0 && c->cpu[i].affinity == affinity) { return static_cast<int> (i); }
  }
  return -1;
}

// The PSCI facts (boot CPU, before any secondary CPU runs). False and the reason when CPU_SUSPEND cannot be used. Mode 2 with
// OS-initiated mode on offer asks for platform-coordinated mode; a refusal leaves only c4.
inline bool probe (Control *c, const Io &io)
{
  Facts *f = &c->facts;
  f->probed = true;
  f->suspend = f->setMode = f->systemSuspend = f->defaultSuspend = f->statCount = f->statResidency = kNotSupported;
  f->setModeResult = kNotCalled;
  const int64_t version = io.call (io.context, kVersion, 0, 0, 0);
  f->version = static_cast<uint32_t> (static_cast<uint64_t> (version));
  if (result32 (version) < 0 || f->version < 0x10000u) { return fail (c, "PSCI older than 1.0"); }
  f->suspend = result32 (io.call (io.context, kFeatures, kCpuSuspend64, 0, 0));
  f->setMode = result32 (io.call (io.context, kFeatures, kSetSuspendMode, 0, 0));
  f->systemSuspend = result32 (io.call (io.context, kFeatures, kSystemSuspend64, 0, 0));
  f->defaultSuspend = result32 (io.call (io.context, kFeatures, kCpuDefaultSuspend64, 0, 0));
  f->statCount = result32 (io.call (io.context, kFeatures, kStatCount64, 0, 0));
  f->statResidency = result32 (io.call (io.context, kFeatures, kStatResidency64, 0, 0));
  if (f->suspend < 0) { return fail (c, "no PSCI CPU_SUSPEND"); }
  if (c->mode == kModeCluster && (f->suspend & 1) != 0) {
    if (f->setMode < 0) {
      disable (c, kCl5, kCauseProbe);
    } else {
      f->setModeResult = result32 (io.call (io.context, kSetSuspendMode, kPlatformCoordinated, 0, 0));
      if (f->setModeResult != kSuccess) { disable (c, kCl5, kCauseProbe); }
    }
  }
  return true;
}

// The boot CPU's self-test entry of a state (ArmCpu.cpp idleSelfTest: interrupts masked, its timer brought forward to
// `deadline`, CPU_SUSPEND, returned at `end`): passes on SUCCESS and a return no later than kLateMs after the deadline.
inline bool selfTest (Control *c, unsigned state, int32_t rc, uint64_t start, uint64_t deadline, uint64_t end)
{
  if (state >= kStates) { return false; }
  c->test[state] = {true, rc, start, deadline, end};
  const bool ok = rc == kSuccess && end >= start && !(end > deadline && end - deadline > c->lateTicks);
  if (!ok) {
    disable (c, state, kCauseSelfTest);
    if (state == kC4) { return fail (c, "self-test"); }
  }
  return ok;
}

// Switches the hook on: only after a successful probe and a passing c4 self-test; false otherwise.
inline bool start (Control *c)
{
  if (c->mode == kModeOff || c->reason != nullptr || !c->facts.probed || !c->test[kC4].ran || !usable (c, kC4)) { return false; }
  __atomic_store_n (&c->on, 1u, __ATOMIC_RELEASE);
  return true;
}

// The deepest usable state for an idle whose next timer interrupt is at `cval` (ctl: CNTV_CTL_EL0), -1 for XNU's WFI.
inline int choose (const Control *c, uint32_t cluster, uint64_t ctl, uint64_t cval, uint64_t now)
{
  const uint32_t off = load (&c->disabled);
  if ((off & (1u << kC4)) != 0 || cluster > 1) { return -1; }
  if ((ctl & 3) != 1) { return kC4; }          // timer disabled or masked: only an interrupt can end the idle, core state only
  const int64_t left = static_cast<int64_t> (cval - now);
  if (left <= 0 || static_cast<uint64_t> (left) < c->threshold[kC4][cluster]) { return -1; }
  if ((off & (1u << kCl5)) == 0 && static_cast<uint64_t> (left) >= c->threshold[kCl5][cluster]) { return kCl5; }
  return kC4;
}

inline void account (Control *c, Cpu *p, unsigned state, int32_t rc, bool armed, uint64_t deadline, uint64_t before, uint64_t after)
{
  if (rc != kSuccess) {
    bump (&p->fails[state]);
    store (&p->lastError, rc);
    p->lateRun[state] = 0;
    if (++p->failRun[state] >= kFailLimit) { disable (c, state, kCauseError); }
    return;
  }
  p->failRun[state] = 0;
  bump (&p->entries[state]);
  add (&p->ticks[state], after >= before ? after - before : 0);
  store (&p->resumed, after);
  if (armed && after > deadline && after - deadline > c->lateTicks) {
    bump (&p->late[state]);
    if (++p->lateRun[state] >= kLateLimit) { disable (c, state, kCauseLate); }
  } else {
    p->lateRun[state] = 0;
  }
}

// The hook entering idle (IRQ/FIQ masked, on the idle CPU): CPU_SUSPEND of the chosen state, or nothing. Returns the state
// requested, -1 when the CPU was left to XNU's WFI.
inline int enterIdle (Control *c, const Io &io, uint64_t mpidr, uint64_t ctl, uint64_t cval, uint64_t now)
{
  if (!running (c)) { return -1; }
  const int slot = slotOf (c, mpidr);
  if (slot < 0) { return -1; }
  Cpu *p = &c->cpu[slot];
  const bool armed = (ctl & 3) == 1;
  p->enteredAt = now;
  p->enteredDeadline = cval;
  p->enteredArmed = armed ? 1 : 0;
  const int state = choose (c, p->cluster, ctl, cval, now);
  if (state < 0) { bump (&p->shortIdle); return -1; }
  if (p->shortRun >= kShortRun) { bump (&p->predicted); return -1; }
  const int32_t rc = result32 (io.call (io.context, kCpuSuspend64, kState[state].param, 0, 0));
  const uint64_t after = io.now (io.context);
  account (c, p, static_cast<unsigned> (state), rc, armed, cval, now, after);
  return state;
}

// The hook as the CPU leaves cpu_idle (after every idle the entry hook saw): the history (was this idle shorter than the c4
// threshold and ended by another interrupt than its timer?) and, after a CPU_SUSPEND, the time XNU took from its return to here
// (its idle-entry cache flush platform_cache_idle_enter, then a WFI the pending interrupt ends at once).
inline void exitIdle (Control *c, uint64_t mpidr, uint64_t now)
{
  const int slot = slotOf (c, mpidr);
  if (slot < 0) { return; }
  Cpu *p = &c->cpu[slot];
  if (p->enteredAt != 0 && now >= p->enteredAt) {
    const bool early = p->enteredArmed == 0 || static_cast<int64_t> (p->enteredDeadline - now) > 0;
    if (early && now - p->enteredAt < c->threshold[kC4][p->cluster]) {
      if (p->shortRun < 255) { p->shortRun++; }
    } else {
      p->shortRun = 0;
    }
  }
  p->enteredAt = 0;
  const uint64_t resumed = load (&p->resumed);
  if (resumed == 0) { return; }
  store (&p->resumed, uint64_t (0));
  if (now < resumed) { return; }
  const uint64_t post = now - resumed;
  add (&p->postTicks, post);
  bump (&p->postCount);
  if (post > load (&p->postMax)) { store (&p->postMax, post); }
}

// ---- Log text (bounded, always terminated).
struct Text {
  char *out;
  size_t used, size;
};

inline void put (Text *t, char ch)
{
  if (t->size == 0) { return; }
  if (t->used + 1 < t->size) { t->out[t->used++] = ch; }
  t->out[t->used] = 0;
}

inline void put (Text *t, const char *s)
{
  while (s != nullptr && *s != 0) { put (t, *s++); }
}

inline void putDec (Text *t, uint64_t v)
{
  char digits[24];
  unsigned n = 0;
  do { digits[n++] = static_cast<char> ('0' + v % 10); v /= 10; } while (v != 0);
  while (n != 0) { put (t, digits[--n]); }
}

inline void putSigned (Text *t, int64_t v)
{
  if (v < 0) { put (t, '-'); putDec (t, static_cast<uint64_t> (-(v + 1)) + 1); } else { putDec (t, static_cast<uint64_t> (v)); }
}

inline void putHex (Text *t, uint64_t v)
{
  char digits[20];
  unsigned n = 0;
  do { digits[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v != 0);
  put (t, "0x");
  while (n != 0) { put (t, digits[--n]); }
}

// A PSCI_FEATURES result: the flags in hex, an error as a negative number.
inline void putFeature (Text *t, int32_t v)
{
  if (v < 0) { putSigned (t, v); } else { putHex (t, static_cast<uint32_t> (v)); }
}

// A value with `decimals` digits after the point (value given in 10^-decimals units).
inline void putFixed (Text *t, uint64_t value, unsigned decimals)
{
  uint64_t scale = 1;
  for (unsigned i = 0; i < decimals; i++) { scale *= 10; }
  putDec (t, value / scale);
  if (decimals == 0) { return; }
  put (t, '.');
  uint64_t rest = value % scale;
  for (unsigned i = 0; i < decimals; i++) { scale /= 10; put (t, static_cast<char> ('0' + rest / scale)); rest %= scale; }
}

// Counter ticks in units of `unitUs` microseconds (1000: ms, 1000000: s) with `decimals` digits after the point (at most 3).
inline void putTime (Text *t, uint64_t hz, uint64_t counterTicks, uint64_t unitUs, unsigned decimals)
{
  uint64_t scale = 1;
  for (unsigned i = 0; i < decimals && i < 3; i++) { scale *= 10; }
  const uint64_t us = microseconds (hz, counterTicks);
  putFixed (t, us / unitUs * scale + (us % unitUs) * scale / unitUs, decimals < 3 ? decimals : 3);
}

inline void putMs (Text *t, uint64_t hz, uint64_t counterTicks, unsigned decimals)
{
  putTime (t, hz, counterTicks, 1000u, decimals);
}

inline void putSeconds (Text *t, uint64_t hz, uint64_t counterTicks, unsigned decimals)
{
  putTime (t, hz, counterTicks, 1000000u, decimals);
}

inline Text text (char *out, size_t size)
{
  Text t = {out, 0, size};
  if (size != 0) { out[0] = 0; }
  return t;
}

inline const char *causeName (uint32_t cause)
{
  switch (cause) {
  case kCauseMode: return "mode";
  case kCauseProbe: return "probe";
  case kCauseSelfTest: return "self-test";
  case kCauseError: return "errors";
  case kCauseLate: return "late wakes";
  default: return "-";
  }
}

// "idle: psci 1.1 CPU_SUSPEND 0x2 (ext-id, no osi) SET_SUSPEND_MODE -1 SYSTEM_SUSPEND -1 DEFAULT_SUSPEND -1 STAT_COUNT -1
// STAT_RESIDENCY -1[ set PC <rc>]"
inline void factsLine (const Control *c, char *out, size_t size)
{
  Text t = text (out, size);
  const Facts *f = &c->facts;
  put (&t, "idle: psci ");
  if (result32 (static_cast<int64_t> (f->version)) < 0) { putSigned (&t, result32 (static_cast<int64_t> (f->version))); }
  else { putDec (&t, f->version >> 16); put (&t, '.'); putDec (&t, f->version & 0xffff); }
  put (&t, " CPU_SUSPEND "); putFeature (&t, f->suspend);
  if (f->suspend >= 0) {
    put (&t, (f->suspend & 2) != 0 ? " (ext-id, " : " (orig-id, ");
    put (&t, (f->suspend & 1) != 0 ? "osi)" : "no osi)");
  }
  put (&t, " SET_SUSPEND_MODE "); putFeature (&t, f->setMode);
  put (&t, " SYSTEM_SUSPEND "); putFeature (&t, f->systemSuspend);
  put (&t, " DEFAULT_SUSPEND "); putFeature (&t, f->defaultSuspend);
  put (&t, " STAT_COUNT "); putFeature (&t, f->statCount);
  put (&t, " STAT_RESIDENCY "); putFeature (&t, f->statResidency);
  if (f->setModeResult != kNotCalled) { put (&t, " set PC "); putSigned (&t, f->setModeResult); }
}

// "idle: mode 2 c4 0x4 from 689/816us cl5 0x1000054 from 11127us, late 50ms"
inline void configLine (const Control *c, char *out, size_t size)
{
  Text t = text (out, size);
  put (&t, "idle: mode "); putDec (&t, c->mode);
  for (unsigned s = 0; s < kStates; s++) {
    if (load (&c->cause[s]) == kCauseMode) { continue; }
    put (&t, ' '); put (&t, kState[s].name); put (&t, ' '); putHex (&t, kState[s].param); put (&t, " from ");
    putDec (&t, microseconds (c->hz, c->threshold[s][0]));
    if (c->threshold[s][1] != c->threshold[s][0]) { put (&t, '/'); putDec (&t, microseconds (c->hz, c->threshold[s][1])); }
    put (&t, "us");
    if (!usable (c, s)) { put (&t, " (off: "); put (&t, causeName (load (&c->cause[s]))); put (&t, ')'); }
  }
  put (&t, ", late "); putDec (&t, kLateMs); put (&t, "ms");
}

// "idle: self-test c4 rc 0 in 2.03ms (timer 2.00ms) ok"
inline void selfTestLine (const Control *c, unsigned state, char *out, size_t size)
{
  Text t = text (out, size);
  if (state >= kStates || !c->test[state].ran) { put (&t, "idle: no self-test"); return; }
  const SelfTest *s = &c->test[state];
  put (&t, "idle: self-test "); put (&t, kState[state].name); put (&t, " rc "); putSigned (&t, s->rc);
  put (&t, " in "); putMs (&t, c->hz, s->end >= s->start ? s->end - s->start : 0, 2);
  put (&t, "ms (timer "); putMs (&t, c->hz, s->deadline >= s->start ? s->deadline - s->start : 0, 2); put (&t, "ms) ");
  put (&t, usable (c, state) ? "ok" : (s->rc != kSuccess ? "error" : "late"));
}

// Counters at one moment, for the period lines.
struct Snapshot {
  uint64_t at;
  uint64_t entries[kStates], ticks[kStates], fails[kStates], late[kStates];
  uint64_t shortIdle, predicted, postTicks, postCount, postMax;
  uint64_t cpuTicks[kCpus];
  bool cpuKnown[kCpus];
  uint64_t platform[kStates];
  bool platformValid;
};

// Reads the counters; with PSCI_STAT_COUNT available also the platform's own entry counts of the states in use (summed over the
// known CPUs; one call per CPU and state).
inline void snapshot (const Control *c, const Io *io, uint64_t now, Snapshot *s)
{
  *s = Snapshot {};
  s->at = now;
  s->platformValid = io != nullptr && c->facts.statCount >= 0;
  for (unsigned i = 0; i < kCpus; i++) {
    const Cpu *p = &c->cpu[i];
    s->cpuKnown[i] = __atomic_load_n (&p->known, __ATOMIC_ACQUIRE) != 0;
    if (!s->cpuKnown[i]) { continue; }
    for (unsigned st = 0; st < kStates; st++) {
      s->entries[st] += load (&p->entries[st]);
      s->ticks[st] += load (&p->ticks[st]);
      s->fails[st] += load (&p->fails[st]);
      s->late[st] += load (&p->late[st]);
      s->cpuTicks[i] += load (&p->ticks[st]);
      if (s->platformValid && load (&c->cause[st]) != kCauseMode) {
        s->platform[st] += static_cast<uint64_t> (io->call (io->context, kStatCount64, p->affinity, kState[st].param, 0));
      }
    }
    s->shortIdle += load (&p->shortIdle);
    s->predicted += load (&p->predicted);
    s->postTicks += load (&p->postTicks);
    s->postCount += load (&p->postCount);
    const uint64_t max = load (&p->postMax);
    if (max > s->postMax) { s->postMax = max; }
  }
}

inline uint64_t delta (uint64_t now, uint64_t then)
{
  return now >= then ? now - then : 0;
}

// "idle 30.0s c4 2345x 2.91s cl5 12x 0.30s short 1234 hist 56 fail 0/0 late 0/0 cpu% 95 93 97 90 99 98 99 99 post 3us max 41us
// plat 4567/12[ off cl5 late wakes]": the period, then per state the CPU_SUSPEND entries and the time spent in them (summed
// over the CPUs), idles left to WFI because the timer was too close or because of the history, errors and late wakes since
// boot, each CPU's share of the period in CPU_SUSPEND, the path after a wake (average, maximum since boot), the platform's own
// counts (PSCI_STAT_COUNT, when offered) and states switched off.
inline void periodLine (const Control *c, const Snapshot *then, const Snapshot *now, char *out, size_t size)
{
  Text t = text (out, size);
  const uint64_t span = delta (now->at, then->at);
  put (&t, "idle "); putSeconds (&t, c->hz, span, 1); put (&t, 's');
  for (unsigned s = 0; s < kStates; s++) {
    if (load (&c->cause[s]) == kCauseMode) { continue; }
    put (&t, ' '); put (&t, kState[s].name); put (&t, ' ');
    putDec (&t, delta (now->entries[s], then->entries[s])); put (&t, "x ");
    putSeconds (&t, c->hz, delta (now->ticks[s], then->ticks[s]), 2); put (&t, 's');
  }
  put (&t, " short "); putDec (&t, delta (now->shortIdle, then->shortIdle));
  put (&t, " hist "); putDec (&t, delta (now->predicted, then->predicted));
  const bool cluster = load (&c->cause[kCl5]) != kCauseMode;
  put (&t, " fail "); putDec (&t, now->fails[kC4]);
  if (cluster) { put (&t, '/'); putDec (&t, now->fails[kCl5]); }
  put (&t, " late "); putDec (&t, now->late[kC4]);
  if (cluster) { put (&t, '/'); putDec (&t, now->late[kCl5]); }
  put (&t, " cpu%");
  for (unsigned i = 0; i < kCpus; i++) {
    put (&t, ' ');
    if (!now->cpuKnown[i] || span == 0) { put (&t, '-'); continue; }
    uint64_t share = delta (now->cpuTicks[i], then->cpuTicks[i]) * 100u / span;
    putDec (&t, share > 100 ? 100 : share);
  }
  const uint64_t posts = delta (now->postCount, then->postCount);
  put (&t, " post ");
  putDec (&t, posts != 0 ? microseconds (c->hz, delta (now->postTicks, then->postTicks) / posts) : 0);
  put (&t, "us max "); putDec (&t, microseconds (c->hz, now->postMax)); put (&t, "us");
  if (now->platformValid) {
    put (&t, " plat "); putDec (&t, delta (now->platform[kC4], then->platform[kC4]));
    if (cluster) { put (&t, '/'); putDec (&t, delta (now->platform[kCl5], then->platform[kCl5])); }
  }
  bool first = true;
  for (unsigned s = 0; s < kStates; s++) {
    const uint32_t cause = load (&c->cause[s]);
    if (usable (c, s) || cause == kCauseMode) { continue; }
    put (&t, first ? " off " : ", "); first = false;
    put (&t, kState[s].name); put (&t, ' '); put (&t, causeName (cause));
  }
  if (!running (c) && c->mode != kModeOff) { put (&t, " (PSCI idle off)"); }
}

}  // namespace psciidle
