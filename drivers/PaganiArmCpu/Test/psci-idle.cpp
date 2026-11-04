// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of pagani-idle (PsciIdle.h) against a fake PSCI firmware: the boot-arg configuration and its thresholds (the PKX110
// runtime DT's residencies and exit latencies), the CPU table, the probe (PSCI version, PSCI_FEATURES, SET_SUSPEND_MODE only in
// mode 2 and only when OS-initiated mode is offered), the self-test verdicts, the choice of state against the next timer
// deadline (both clusters, the timer disabled or masked, a deadline already passed, counter wrap), CPU_SUSPEND accounting, the
// safety switches (errors in a row, late wakes in a row, c4 off ends PSCI idle), the post-wake measurement, the wake-up history
// (short idles ended by another interrupt keep a CPU in WFI until a longer one), the log lines (exact
// text, every truncation), random sequences against the invariants (only 0x4 and 0x1000054 are ever requested, never apps-pc,
// never a state that is off, never with the timer closer than the c4 threshold, never against the history, counts match the
// calls), eight CPU threads with
// a log reader (run it under TSan too), and an idle-pattern model of the 13T's periodic wakeups (what share of idle time each
// mode reaches).
// Build: xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
//        Test/psci-idle.cpp -o /tmp/psci-idle
//        xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=thread Test/psci-idle.cpp -o /tmp/psci-idle-tsan
#include "../PsciIdle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort (); } checks++; } while (0)
#define CHECK_STR(a, b) do { if (strcmp ((a), (b)) != 0) { \
    fprintf (stderr, "FAIL %s:%d:\n  got  \"%s\"\n  want \"%s\"\n", __FILE__, __LINE__, (a), (b)); abort (); } checks++; } while (0)

using namespace psciidle;

static const uint64_t kHz = 19200000;
static const uint64_t kAff[kCpus] = {0x0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100};

// ---- Fake PSCI firmware.
struct Call {
  uint64_t function, a, b, c;
};

struct Fake {
  // Probe answers.
  int64_t version = 0x10001;
  std::map<uint64_t, int64_t> features = {{kCpuSuspend64, 0x2}};
  int64_t setModeResult = 0;
  // CPU_SUSPEND: the result for each parameter (default SUCCESS) and how the wake comes: at the deadline plus `exitTicks`,
  // `lateTicks` after it, or at once.
  std::map<uint64_t, std::vector<int64_t>> script;   // per parameter: results in order (then SUCCESS)
  enum Wake { kAtDeadline, kLate, kAtOnce } wake = kAtDeadline;
  uint64_t exitTicks = 100, lateBy = 0;
  uint64_t unarmedTicks = 20;                        // a CPU_SUSPEND without an armed timer lasts this long
  bool wfiEarly = false;                             // XNU's WFI (no CPU_SUSPEND) ends 30 ticks in, else at the deadline
  bool lastFailed = false;                           // the last CPU_SUSPEND returned an error (XNU's WFI follows)
  uint64_t upperGarbage = 0;                         // ORed into x0's upper half of every result
  // Clock and the deadline the next CPU_SUSPEND waits for.
  uint64_t now = 1000000, deadline = 0;
  bool armed = true;
  std::vector<Call> calls;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> statCount;   // (affinity, param) -> count

  int64_t call (uint64_t function, uint64_t a, uint64_t b, uint64_t c)
  {
    calls.push_back ({function, a, b, c});
    if (function == kVersion) { return version; }
    if (function == kFeatures) {
      auto f = features.find (a);
      return garbage (f == features.end () ? kNotSupported : f->second);
    }
    if (function == kSetSuspendMode) { return garbage (setModeResult); }
    if (function == kStatCount64) {
      auto f = statCount.find ({a, b});
      return f == statCount.end () ? 0 : static_cast<int64_t> (f->second);
    }
    if (function == kCpuSuspend64) {
      auto &results = script[a];
      int64_t rc = kSuccess;
      if (!results.empty ()) { rc = results.front (); results.erase (results.begin ()); }
      lastFailed = rc != kSuccess;
      if (rc != kSuccess) { now += 5; return garbage (rc); }
      if (!armed) { now += unarmedTicks; }
      else if (wake == kAtOnce) { now += 20; }
      else if (wake == kAtDeadline) { now = (deadline > now ? deadline : now) + exitTicks; }
      else { now = (deadline > now ? deadline : now) + lateBy; }
      return garbage (kSuccess);
    }
    return garbage (kNotSupported);
  }
  int64_t garbage (int64_t rc) const
  {
    return static_cast<int64_t> ((static_cast<uint64_t> (rc) & 0xffffffffULL) | upperGarbage);
  }
  unsigned count (uint64_t function, uint64_t a = ~0ULL) const
  {
    unsigned n = 0;
    for (const Call &x : calls) { n += x.function == function && (a == ~0ULL || x.a == a); }
    return n;
  }
};

static int64_t fakeCall (void *context, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  return static_cast<Fake *> (context)->call (function, a, b, c);
}

static uint64_t fakeNow (void *context)
{
  return static_cast<Fake *> (context)->now;
}

static Io ioOf (Fake *fake)
{
  return Io {fake, fakeCall, fakeNow};
}

static void addCpus (Control *c)
{
  for (unsigned i = 0; i < kCpus; i++) { noteCpu (c, i, kAff[i] | 0x81000000ULL); }
}

// A ready control: configured, CPUs noted, probed against `fake`, self-tests passed, started.
static void ready (Control *c, Fake *fake, uint32_t mode, uint32_t c4Us = 0, uint32_t clUs = 0)
{
  *c = Control {};
  CHECK (configure (c, mode, kHz, c4Us, clUs));
  addCpus (c);
  Io io = ioOf (fake);
  CHECK (probe (c, io));
  CHECK (selfTest (c, kC4, kSuccess, 0, 100, 200));
  CHECK (start (c));
}

// One idle of CPU `cpu` whose timer fires `leftTicks` from now (armed), as the hook sees it: enter, CPU_SUSPEND or XNU's WFI
// (until the deadline when armed, unless wfiEarly), 7 ticks of XNU's path, exit.
static int idleOnce (Control *c, Fake *fake, unsigned cpu, uint64_t leftTicks, uint64_t ctl = 1)
{
  const uint64_t now = fake->now;
  fake->deadline = now + leftTicks;
  fake->armed = (ctl & 3) == 1;
  Io io = ioOf (fake);
  const size_t calls = fake->calls.size ();
  const int state = enterIdle (c, io, kAff[cpu] | 0x81000000ULL, ctl, now + leftTicks, now);
  if (fake->calls.size () == calls || fake->lastFailed) {      // XNU's own WFI: no CPU_SUSPEND, or it returned an error
    const uint64_t from = fake->now;
    fake->now = fake->wfiEarly || !fake->armed ? from + 30 : (now + leftTicks > from ? now + leftTicks : from);
  }
  fake->lastFailed = false;
  fake->now += 7;
  exitIdle (c, kAff[cpu] | 0x81000000ULL, fake->now);
  return state;
}

static void testConfigure ()
{
  Control c = {};
  CHECK (!configure (&c, 0, kHz, 0, 0) && c.mode == kModeOff);
  CHECK (!configure (&c, 3, kHz, 0, 0) && c.mode == kModeOff);
  CHECK (!configure (&c, 1, 0, 0, 0) && c.mode == kModeOff);
  // Defaults: residency + exit latency of the runtime DT, rounded up to whole counter ticks.
  c = Control {};
  CHECK (configure (&c, 1, kHz, 0, 0) && c.mode == kModeCore);
  CHECK (c.threshold[kC4][0] == 13229 && c.threshold[kC4][1] == 15668);   // 689 us, 816 us
  CHECK (c.threshold[kCl5][0] == 213639 && c.threshold[kCl5][1] == 213639);   // 11127 us
  CHECK (c.lateTicks == 960000);
  CHECK (!usable (&c, kCl5) && c.cause[kCl5] == kCauseMode && usable (&c, kC4));
  c = Control {};
  CHECK (configure (&c, 2, kHz, 0, 0) && usable (&c, kCl5) && usable (&c, kC4));
  // Overrides: clamped to 100 us .. 1 s; the cluster threshold never below the core one.
  c = Control {};
  CHECK (configure (&c, 2, kHz, 1000, 50));
  CHECK (c.threshold[kC4][0] == 19200 && c.threshold[kC4][1] == 19200 && c.threshold[kCl5][0] == 19200);
  c = Control {};
  CHECK (configure (&c, 2, kHz, 20, 5000000));
  CHECK (c.threshold[kC4][0] == 1920 && c.threshold[kCl5][1] == 19200000);
  // Conversions.
  CHECK (ticks (kHz, 1) == 20 && ticks (kHz, 1000000) == kHz && ticks (kHz, 0) == 0 && ticks (1000000000, 3) == 3000);
  CHECK (microseconds (kHz, 19200) == 1000 && microseconds (kHz, kHz * 86400 * 365) == 86400ULL * 365 * 1000000);
  CHECK (microseconds (0, 5) == 0);
  CHECK (result32 (static_cast<int64_t> (0xfffffffffffffffeULL)) == -2 && result32 (0x12345678fffffffdLL) == -3);
  CHECK (result32 (0x7fffffff00000000LL) == 0);
}

static void testCpus ()
{
  Control c = {};
  CHECK (configure (&c, 1, kHz, 0, 0));
  CHECK (slotOf (&c, 0) == -1);   // nothing noted yet: CPU0's affinity 0 must not match an empty slot
  addCpus (&c);
  noteCpu (&c, 8, 0x20000);       // out of range: ignored
  for (unsigned i = 0; i < kCpus; i++) {
    CHECK (slotOf (&c, kAff[i]) == static_cast<int> (i));
    CHECK (slotOf (&c, kAff[i] | 0x81000000ULL) == static_cast<int> (i));   // MPIDR RES1 bit 31 and MT bit 24
    CHECK (c.cpu[i].cluster == (i >= 6 ? 1u : 0u));
  }
  CHECK (slotOf (&c, 0x20000) == -1 && slotOf (&c, 0x600) == -1 && slotOf (&c, 0x100000000ULL) == -1);
  CHECK (slotOf (&c, 0x10100 | (0x1ULL << 32)) == -1);    // Aff3 (bits 39:32) is part of the affinity
  CHECK (slotOf (&c, 0x10100 | (0xffULL << 40)) == 7);    // bits above Aff3 are not
}

static void testProbe ()
{
  // PSCI 1.1, CPU_SUSPEND in extended StateID format without OS-initiated mode (what Gunyah is expected to offer).
  {
    Fake fake;
    Control c = {};
    CHECK (configure (&c, 2, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && c.reason == nullptr);
    CHECK (c.facts.probed && c.facts.version == 0x10001 && c.facts.suspend == 2 && c.facts.setMode == kNotSupported);
    CHECK (c.facts.setModeResult == kNotCalled && fake.count (kSetSuspendMode) == 0 && usable (&c, kCl5));
    CHECK (fake.count (kFeatures) == 6 && fake.count (kCpuSuspend64) == 0);
  }
  // Upper halves of x0 are not part of the PSCI result.
  {
    Fake fake;
    fake.upperGarbage = 0xdead000000000000ULL;
    fake.features[kSystemSuspend64] = kNotSupported;
    Control c = {};
    CHECK (configure (&c, 1, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && c.facts.suspend == 2 && c.facts.systemSuspend == kNotSupported);
  }
  // Too old, or no answer.
  for (int64_t version : {int64_t (0x2), int64_t (0xffffffff), int64_t (-1), int64_t (0x0000ffff)}) {
    Fake fake;
    fake.version = version;
    Control c = {};
    CHECK (configure (&c, 1, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (!probe (&c, io) && strcmp (c.reason, "PSCI older than 1.0") == 0 && !running (&c));
    CHECK (fake.count (kFeatures) == 0);
  }
  // No CPU_SUSPEND.
  {
    Fake fake;
    fake.features.erase (kCpuSuspend64);
    Control c = {};
    CHECK (configure (&c, 1, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (!probe (&c, io) && strcmp (c.reason, "no PSCI CPU_SUSPEND") == 0);
    CHECK (!start (&c) || !running (&c));
  }
  // Mode 2 with OS-initiated mode on offer: platform-coordinated mode is asked for once, x1 = 0.
  {
    Fake fake;
    fake.features[kCpuSuspend64] = 0x3;
    fake.features[kSetSuspendMode] = 0;
    Control c = {};
    CHECK (configure (&c, 2, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && usable (&c, kCl5) && c.facts.setModeResult == kSuccess);
    CHECK (fake.count (kSetSuspendMode) == 1 && fake.calls.back ().function == kSetSuspendMode && fake.calls.back ().a == 0);
  }
  // ... refused: the cluster state is off, the core state stays.
  for (int64_t refusal : {int64_t (kDenied), int64_t (kInvalidParameters), int64_t (kNotSupported)}) {
    Fake fake;
    fake.features[kCpuSuspend64] = 0x3;
    fake.features[kSetSuspendMode] = 0;
    fake.setModeResult = refusal;
    Control c = {};
    CHECK (configure (&c, 2, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && !usable (&c, kCl5) && c.cause[kCl5] == kCauseProbe && usable (&c, kC4));
    CHECK (c.facts.setModeResult == refusal);
  }
  // OS-initiated mode without SET_SUSPEND_MODE (not PSCI conformant): cluster state off, nothing called.
  {
    Fake fake;
    fake.features[kCpuSuspend64] = 0x3;
    Control c = {};
    CHECK (configure (&c, 2, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && !usable (&c, kCl5) && fake.count (kSetSuspendMode) == 0);
  }
  // Mode 1 never asks for a mode.
  {
    Fake fake;
    fake.features[kCpuSuspend64] = 0x3;
    fake.features[kSetSuspendMode] = 0;
    Control c = {};
    CHECK (configure (&c, 1, kHz, 0, 0));
    Io io = ioOf (&fake);
    CHECK (probe (&c, io) && fake.count (kSetSuspendMode) == 0 && c.facts.setModeResult == kNotCalled);
  }
}

static void testSelfTest ()
{
  Fake fake;
  Io io = ioOf (&fake);
  Control c = {};
  CHECK (configure (&c, 2, kHz, 0, 0));
  CHECK (probe (&c, io));
  CHECK (!start (&c));     // no self-test yet
  // On time (2 ms timer, woken 0.1 ms after it), early, and up to 50 ms late pass.
  CHECK (selfTest (&c, kC4, kSuccess, 1000, 1000 + 38400, 1000 + 38400 + 1920));
  CHECK (selfTest (&c, kC4, kSuccess, 1000, 1000 + 38400, 1100));
  CHECK (selfTest (&c, kC4, kSuccess, 1000, 1000 + 38400, 1000 + 38400 + 960000));
  CHECK (start (&c) && running (&c));
  // A late cluster self-test switches only the cluster state off.
  CHECK (!selfTest (&c, kCl5, kSuccess, 1000, 2000, 2000 + 960001) && !usable (&c, kCl5) && c.cause[kCl5] == kCauseSelfTest);
  CHECK (running (&c) && usable (&c, kC4));
  // A failing c4 self-test keeps PSCI idle off.
  Control d = {};
  CHECK (configure (&d, 1, kHz, 0, 0) && probe (&d, io));
  CHECK (!selfTest (&d, kC4, kInvalidParameters, 0, 10, 20) && strcmp (d.reason, "self-test") == 0);
  CHECK (!start (&d) && !running (&d) && d.cause[kC4] == kCauseSelfTest);
  Control e = {};
  CHECK (configure (&e, 1, kHz, 0, 0) && probe (&e, io));
  CHECK (!selfTest (&e, kC4, kSuccess, 0, 100, 100 + 960001) && !start (&e));
  CHECK (!selfTest (&e, 7, kSuccess, 0, 0, 0));
  // Never configured, or not probed: never started.
  Control off = {};
  CHECK (!start (&off));
  Control unprobed = {};
  CHECK (configure (&unprobed, 1, kHz, 0, 0) && selfTest (&unprobed, kC4, kSuccess, 0, 1, 2) && !start (&unprobed));
  // A failed probe is final even with a passing self-test.
  Fake none;
  none.features.erase (kCpuSuspend64);
  Io noneIo = ioOf (&none);
  Control failed = {};
  CHECK (configure (&failed, 1, kHz, 0, 0) && !probe (&failed, noneIo));
  CHECK (selfTest (&failed, kC4, kSuccess, 0, 1, 2) && !start (&failed) && !running (&failed));
}

static void testChoose ()
{
  Control c = {};
  Fake fake;
  ready (&c, &fake, 2);
  const uint64_t now = 5000000;
  // Cluster 0: c4 from 13229 ticks, cl5 from 213639.
  CHECK (choose (&c, 0, 1, now + 13228, now) == -1);
  CHECK (choose (&c, 0, 1, now + 13229, now) == kC4);
  CHECK (choose (&c, 0, 1, now + 213638, now) == kC4);
  CHECK (choose (&c, 0, 1, now + 213639, now) == kCl5);
  // Cluster 1: c4 from 15668.
  CHECK (choose (&c, 1, 1, now + 15667, now) == -1 && choose (&c, 0, 1, now + 15667, now) == kC4);
  CHECK (choose (&c, 1, 1, now + 15668, now) == kC4);
  CHECK (choose (&c, 2, 1, now + 1000000, now) == -1);
  // Deadline passed or now: XNU's WFI (the interrupt is pending).
  CHECK (choose (&c, 0, 1, now, now) == -1 && choose (&c, 0, 1, now - 1, now) == -1 && choose (&c, 0, 1, 0, now) == -1);
  // Timer disabled (ENABLE 0) or masked (IMASK 1): the core state only.
  CHECK (choose (&c, 0, 0, now + 100000000, now) == kC4 && choose (&c, 0, 3, now + 1, now) == kC4);
  CHECK (choose (&c, 1, 2, now, now) == kC4);
  // ISTATUS (bit 2) does not matter.
  CHECK (choose (&c, 0, 5, now + 213639, now) == kCl5);
  // The counter wrapped between now and the deadline.
  CHECK (choose (&c, 0, 1, 213639 - 10, ~0ULL - 9) == kCl5);
  CHECK (choose (&c, 0, 1, 5, ~0ULL - 9) == -1);
  // States off.
  disable (&c, kCl5, kCauseError);
  CHECK (choose (&c, 0, 1, now + 100000000, now) == kC4 && running (&c));
  disable (&c, kC4, kCauseLate);
  CHECK (choose (&c, 0, 1, now + 100000000, now) == -1 && choose (&c, 0, 0, now, now) == -1 && !running (&c));
  CHECK (c.cause[kCl5] == kCauseError && c.cause[kC4] == kCauseLate);
  disable (&c, kC4, kCauseError);   // a second switch keeps the first cause
  CHECK (c.cause[kC4] == kCauseLate);
  disable (&c, 9, kCauseError);
  // Mode 1 never chooses the cluster state.
  Control d = {};
  ready (&d, &fake, 1);
  CHECK (choose (&d, 0, 1, now + 100000000, now) == kC4 && choose (&d, 1, 1, now + 100000000, now) == kC4);
  // A configured threshold.
  Control e = {};
  ready (&e, &fake, 2, 5000, 20000);
  CHECK (choose (&e, 0, 1, now + 95999, now) == -1 && choose (&e, 0, 1, now + 96000, now) == kC4);
  CHECK (choose (&e, 1, 1, now + 383999, now) == kC4 && choose (&e, 1, 1, now + 384000, now) == kCl5);
}

static void testIdle ()
{
  Fake fake;
  Control c = {};
  // Not started: the hook does nothing at all.
  CHECK (configure (&c, 2, kHz, 0, 0));
  addCpus (&c);
  {
    Io io = ioOf (&fake);
    CHECK (enterIdle (&c, io, 0, 1, fake.now + 1000000, fake.now) == -1 && fake.calls.empty ());
  }
  ready (&c, &fake, 2);
  fake.calls.clear ();
  // An unknown CPU is left alone.
  {
    Io io = ioOf (&fake);
    CHECK (enterIdle (&c, io, 0x20000, 1, fake.now + 1000000, fake.now) == -1 && fake.calls.empty ());
  }
  // Too close: WFI, counted.
  CHECK (idleOnce (&c, &fake, 0, 13228) == -1 && fake.calls.empty () && c.cpu[0].shortIdle == 1);
  // c4: one CPU_SUSPEND(0x4, 0, 0) that returns 100 ticks after the deadline.
  uint64_t before = fake.now;
  CHECK (idleOnce (&c, &fake, 0, 20000) == kC4);
  CHECK (fake.calls.size () == 1 && fake.calls[0].function == kCpuSuspend64 && fake.calls[0].a == 0x4);
  CHECK (fake.calls[0].b == 0 && fake.calls[0].c == 0);
  CHECK (c.cpu[0].entries[kC4] == 1 && c.cpu[0].ticks[kC4] == 20100 && c.cpu[0].fails[kC4] == 0);
  // The exit hook measured from the return (fake.now + 7 in idleOnce).
  CHECK (c.cpu[0].postCount == 1 && c.cpu[0].postTicks == 7 && c.cpu[0].postMax == 7 && c.cpu[0].resumed == 0);
  CHECK (fake.now == before + 20100 + 7);
  // An exit without a preceding CPU_SUSPEND measures nothing.
  exitIdle (&c, 0, fake.now + 1000);
  exitIdle (&c, 0x20000, fake.now + 1000);
  CHECK (c.cpu[0].postCount == 1);
  // cl5 on CPU 6.
  fake.calls.clear ();
  CHECK (idleOnce (&c, &fake, 6, 300000) == kCl5 && fake.calls.size () == 1 && fake.calls[0].a == 0x1000054);
  CHECK (c.cpu[6].entries[kCl5] == 1 && c.cpu[6].ticks[kCl5] == 300100);
  // cl5 refused four times in a row on CPU 6 (INVALID_PARAMETERS, as when the firmware does not know the state): cl5 off.
  fake.script[0x1000054] = {kInvalidParameters, kInvalidParameters, kInvalidParameters};
  for (int i = 0; i < 3; i++) { CHECK (idleOnce (&c, &fake, 6, 300000) == kCl5); }
  CHECK (usable (&c, kCl5) && c.cpu[6].fails[kCl5] == 3 && c.cpu[6].lastError == kInvalidParameters);
  // A success in between restarts the count.
  CHECK (idleOnce (&c, &fake, 6, 300000) == kCl5 && c.cpu[6].failRun[kCl5] == 0);
  fake.script[0x1000054] = {kDenied, kDenied, kDenied, kDenied};
  for (int i = 0; i < 4; i++) { CHECK (idleOnce (&c, &fake, 6, 300000) == kCl5); }
  CHECK (!usable (&c, kCl5) && c.cause[kCl5] == kCauseError && running (&c));
  CHECK (c.cpu[6].fails[kCl5] == 7 && c.cpu[6].lastError == kDenied);
  // Errors on another CPU are counted on their own (the switch is global).
  fake.calls.clear ();
  CHECK (idleOnce (&c, &fake, 7, 300000) == kC4 && fake.calls.size () == 1 && fake.calls[0].a == 0x4);
  // An error return leaves no post-wake measurement pending.
  fake.script[0x4] = {kInvalidParameters};
  const uint64_t posts = c.cpu[1].postCount;
  CHECK (idleOnce (&c, &fake, 1, 20000) == kC4 && c.cpu[1].postCount == posts && c.cpu[1].fails[kC4] == 1);
  // Late wakes: two late, one on time (restarts the count), three late: c4 off, PSCI idle off.
  fake.wake = Fake::kLate;
  fake.lateBy = 960001;
  CHECK (idleOnce (&c, &fake, 2, 20000) == kC4 && idleOnce (&c, &fake, 2, 20000) == kC4);
  CHECK (c.cpu[2].late[kC4] == 2 && c.cpu[2].lateRun[kC4] == 2 && running (&c));
  fake.lateBy = 960000;   // exactly 50 ms: not late
  CHECK (idleOnce (&c, &fake, 2, 20000) == kC4 && c.cpu[2].lateRun[kC4] == 0 && c.cpu[2].late[kC4] == 2);
  fake.lateBy = 2000000;
  for (int i = 0; i < 3; i++) { CHECK (idleOnce (&c, &fake, 2, 20000) == kC4); }
  CHECK (!running (&c) && !usable (&c, kC4) && c.cause[kC4] == kCauseLate && c.cpu[2].late[kC4] == 5);
  fake.calls.clear ();
  for (unsigned cpu = 0; cpu < kCpus; cpu++) { CHECK (idleOnce (&c, &fake, cpu, 1000000) == -1); }
  CHECK (fake.calls.empty ());
  // An unarmed timer is never late, however long the wait.
  Control d = {};
  Fake slow;
  ready (&d, &slow, 1);
  slow.wake = Fake::kLate;
  slow.lateBy = 100000000;
  slow.unarmedTicks = 100000000;
  for (int i = 0; i < 10; i++) { CHECK (idleOnce (&d, &slow, 3, 0, 0) == kC4); }
  CHECK (running (&d) && d.cpu[3].late[kC4] == 0 && d.cpu[3].entries[kC4] == 10);
  // Repeated c4 errors end PSCI idle.
  slow.script[0x4] = {kNotSupported, kNotSupported, kNotSupported, kNotSupported};
  for (int i = 0; i < 4; i++) { CHECK (idleOnce (&d, &slow, 4, 20000) == kC4); }
  CHECK (!running (&d) && d.cause[kC4] == kCauseError);
}

// The history: short idles ended by another interrupt (disk I/O, an IPI) make the next idles WFI until one lasts longer.
static void testHistory ()
{
  Fake fake;
  Control c = {};
  ready (&c, &fake, 2);
  fake.calls.clear ();
  // Two c4 entries that a device interrupt ends 20 ticks in (far before the timer, far below the c4 threshold).
  fake.wake = Fake::kAtOnce;
  CHECK (idleOnce (&c, &fake, 3, 500000) == kCl5 && c.cpu[3].shortRun == 1);
  CHECK (idleOnce (&c, &fake, 3, 20000) == kC4 && c.cpu[3].shortRun == 2 && fake.calls.size () == 2);
  // Now the history keeps the CPU in WFI, even with the timer far away; WFIs ended early keep it there.
  fake.wfiEarly = true;
  CHECK (idleOnce (&c, &fake, 3, 500000) == -1 && idleOnce (&c, &fake, 3, 20000) == -1);
  CHECK (fake.calls.size () == 2 && c.cpu[3].predicted == 2 && c.cpu[3].shortIdle == 0 && c.cpu[3].shortRun == 4);
  // Other CPUs are not affected.
  fake.wake = Fake::kAtDeadline;
  CHECK (idleOnce (&c, &fake, 4, 20000) == kC4 && fake.calls.size () == 3);
  // A WFI that lasts until its timer ends the run: the next idle may suspend again.
  fake.wfiEarly = false;
  CHECK (idleOnce (&c, &fake, 3, 500000) == -1 && c.cpu[3].shortRun == 0 && c.cpu[3].predicted == 3);
  CHECK (idleOnce (&c, &fake, 3, 500000) == kCl5 && fake.calls.size () == 4);
  // A short idle ended by its own timer is no evidence (the timer was the reason): c4 with an on-time wake keeps the run at 0.
  CHECK (idleOnce (&c, &fake, 3, 13229) == kC4 && c.cpu[3].shortRun == 0);
  // A long idle ended early (an interrupt after more than the threshold) also ends the run.
  fake.wake = Fake::kAtOnce;
  CHECK (idleOnce (&c, &fake, 5, 30000) == kC4 && idleOnce (&c, &fake, 5, 30000) == kC4 && c.cpu[5].shortRun == 2);
  fake.wfiEarly = true;
  CHECK (idleOnce (&c, &fake, 5, 30000) == -1 && c.cpu[5].shortRun == 3);
  fake.wfiEarly = false;
  CHECK (idleOnce (&c, &fake, 5, 30000) == -1 && c.cpu[5].shortRun == 0);
  // Timer unarmed: every end counts as early; a short one counts.
  fake.unarmedTicks = 10;
  CHECK (idleOnce (&c, &fake, 6, 0, 0) == kC4 && c.cpu[6].shortRun == 1);
  fake.unarmedTicks = 1000000;
  CHECK (idleOnce (&c, &fake, 6, 0, 0) == kC4 && c.cpu[6].shortRun == 0);
  // An exit without an entry (the hook switched on mid-idle) changes nothing.
  const uint32_t run = c.cpu[7].shortRun;
  exitIdle (&c, kAff[7], fake.now);
  CHECK (c.cpu[7].shortRun == run);
}

// Exact log text, and every truncation of it a terminated prefix written inside the buffer.
static void checkTruncation (const char *full, void (*render) (const Control *, char *, size_t), const Control *c)
{
  const size_t length = strlen (full);
  for (size_t size = 0; size <= length + 2; size++) {
    std::vector<char> buffer (size + 8, '#');
    render (c, buffer.data () + 4, size);
    for (size_t i = 0; i < 4; i++) { CHECK (buffer[i] == '#' && buffer[size + 4 + i] == '#'); }
    if (size == 0) { continue; }
    const char *out = buffer.data () + 4;
    const size_t n = strlen (out);
    CHECK (n == (length < size - 1 ? length : size - 1) && strncmp (out, full, n) == 0);
  }
}

static const Snapshot *gThen, *gNow;
static void renderPeriod (const Control *c, char *out, size_t size) { periodLine (c, gThen, gNow, out, size); }
static void renderSelfTest (const Control *c, char *out, size_t size) { selfTestLine (c, kC4, out, size); }

static void testLines ()
{
  char line[512];
  Fake fake;
  fake.features[kCpuSuspend64] = 0x3;
  fake.features[kSetSuspendMode] = 0;
  fake.features[kStatCount64] = 0;
  Control c = {};
  CHECK (configure (&c, 2, kHz, 0, 0));
  addCpus (&c);
  Io io = ioOf (&fake);
  CHECK (probe (&c, io));
  factsLine (&c, line, sizeof (line));
  CHECK_STR (line, "idle: psci 1.1 CPU_SUSPEND 0x3 (ext-id, osi) SET_SUSPEND_MODE 0x0 SYSTEM_SUSPEND -1 DEFAULT_SUSPEND -1 "
                   "STAT_COUNT 0x0 STAT_RESIDENCY -1 set PC 0");
  checkTruncation (line, factsLine, &c);
  configLine (&c, line, sizeof (line));
  CHECK_STR (line, "idle: mode 2 c4 0x4 from 689/816us cl5 0x1000054 from 11127us, late 50ms");
  checkTruncation (line, configLine, &c);
  CHECK (selfTest (&c, kC4, kSuccess, 1000, 1000 + 38400, 1000 + 38400 + 576));
  selfTestLine (&c, kC4, line, sizeof (line));
  CHECK_STR (line, "idle: self-test c4 rc 0 in 2.03ms (timer 2.00ms) ok");
  checkTruncation (line, renderSelfTest, &c);
  CHECK (!selfTest (&c, kCl5, kSuccess, 0, 288000, 288000 + 1920000));
  selfTestLine (&c, kCl5, line, sizeof (line));
  CHECK_STR (line, "idle: self-test cl5 rc 0 in 115.00ms (timer 15.00ms) late");
  configLine (&c, line, sizeof (line));
  CHECK_STR (line, "idle: mode 2 c4 0x4 from 689/816us cl5 0x1000054 from 11127us (off: self-test), late 50ms");
  selfTestLine (&c, 5, line, sizeof (line));
  CHECK_STR (line, "idle: no self-test");
  Control e = {};
  CHECK (configure (&e, 1, kHz, 0, 0));
  CHECK (!selfTest (&e, kC4, kDenied, 0, 10, 20));
  selfTestLine (&e, kC4, line, sizeof (line));
  CHECK_STR (line, "idle: self-test c4 rc -3 in 0.00ms (timer 0.00ms) error");
  // Older firmware.
  Fake old;
  old.version = 0x2;
  Control o = {};
  CHECK (configure (&o, 1, kHz, 0, 0));
  Io oldIo = ioOf (&old);
  CHECK (!probe (&o, oldIo));
  factsLine (&o, line, sizeof (line));
  CHECK_STR (line, "idle: psci 0.2 CPU_SUSPEND -1 SET_SUSPEND_MODE -1 SYSTEM_SUSPEND -1 DEFAULT_SUSPEND -1 STAT_COUNT -1 "
                   "STAT_RESIDENCY -1");
  old.version = kNotSupported;
  CHECK (!probe (&o, oldIo));
  factsLine (&o, line, sizeof (line));
  CHECK (strncmp (line, "idle: psci -1 CPU_SUSPEND -1", 28) == 0);
  configLine (&o, line, sizeof (line));
  CHECK_STR (line, "idle: mode 1 c4 0x4 from 689/816us, late 50ms");

  // Period line: 30 s, the platform's counts from PSCI_STAT_COUNT.
  Control p = {};
  Fake pf;
  pf.features[kStatCount64] = 0;
  ready (&p, &pf, 2);
  Io pio = ioOf (&pf);
  Snapshot then, now;
  snapshot (&p, &pio, 1000, &then);
  CHECK (then.platformValid && then.platform[kC4] == 0);
  for (unsigned i = 0; i < kCpus; i++) {
    p.cpu[i].entries[kC4] = 100 + i;
    p.cpu[i].ticks[kC4] = kHz * 3 * (i + 1);   // CPU i: (i + 1) * 3 s of 30 s in c4
    pf.statCount[{kAff[i], 0x4}] = 90 + i;
  }
  p.cpu[6].entries[kCl5] = 12;
  p.cpu[6].ticks[kCl5] = kHz / 10;            // + 0.1 s
  p.cpu[6].fails[kCl5] = 4;
  p.cpu[2].late[kC4] = 1;
  p.cpu[0].shortIdle = 1234;
  p.cpu[1].predicted = 56;
  p.cpu[0].postTicks = 19200 * 2;             // 2 ms over 4 wakes: 500 us average
  p.cpu[0].postCount = 4;
  p.cpu[0].postMax = 19200;                    // 1 ms
  pf.statCount[{kAff[6], 0x1000054}] = 11;
  disable (&p, kCl5, kCauseError);
  snapshot (&p, &pio, 1000 + kHz * 30, &now);
  gThen = &then;
  gNow = &now;
  periodLine (&p, &then, &now, line, sizeof (line));
  CHECK_STR (line, "idle 30.0s c4 828x 108.00s cl5 12x 0.10s short 1234 hist 56 fail 0/4 late 1/0 cpu% 10 20 30 40 50 60 70 80 "
                   "post 500us max 1000us plat 748/11 off cl5 errors");
  checkTruncation (line, renderPeriod, &p);
  // Unknown CPUs, an empty period, no STAT_COUNT, PSCI idle off, mode 1 (no cluster columns).
  Control q = {};
  Fake qf;
  ready (&q, &qf, 1);
  q.cpu[5].known = 0;
  Io qio = ioOf (&qf);
  Snapshot a, b;
  snapshot (&q, &qio, 500, &a);
  snapshot (&q, &qio, 500, &b);
  CHECK (!a.platformValid);
  disable (&q, kC4, kCauseError);
  periodLine (&q, &a, &b, line, sizeof (line));
  CHECK_STR (line, "idle 0.0s c4 0x 0.00s short 0 hist 0 fail 0 late 0 cpu% - - - - - - - - post 0us max 0us off c4 errors "
                   "(PSCI idle off)");
  q.cpu[0].ticks[kC4] = kHz * 1000;           // more than the period: capped at 100 %
  snapshot (&q, nullptr, 500 + kHz, &b);
  periodLine (&q, &a, &b, line, sizeof (line));
  CHECK (strstr (line, "cpu% 100 0 0 0 0 - 0 0 ") != nullptr && strstr (line, "plat") == nullptr);
  // Text helpers.
  char small[8];
  Text t = text (small, sizeof (small));
  putSigned (&t, INT64_MIN);
  CHECK_STR (small, "-922337");
  t = text (small, sizeof (small));
  putFixed (&t, 1234, 3);
  CHECK_STR (small, "1.234");
  t = text (small, sizeof (small));
  putTime (&t, kHz, kHz / 2, 1000000u, 5);   // decimals capped at 3
  CHECK_STR (small, "0.500");
  t = text (nullptr, 0);
  put (&t, "x");
  putDec (&t, 7);
  CHECK (t.used == 0);
}

// Random sequences against the invariants.
static void testRandom ()
{
  std::mt19937_64 random (20261030);
  for (unsigned round = 0; round < 300; round++) {
    Fake fake;
    const uint32_t mode = 1 + random () % 2;
    Control c = {};
    ready (&c, &fake, mode, random () % 3 == 0 ? static_cast<uint32_t> (random () % 3000) : 0,
           random () % 3 == 0 ? static_cast<uint32_t> (random () % 30000) : 0);
    fake.calls.clear ();
    unsigned suspends = 0;
    for (unsigned step = 0; step < 2000; step++) {
      const unsigned cpu = random () % kCpus;
      const uint64_t left = random () % 4 == 0 ? random () % 20000 : random () % 600000;
      const uint64_t ctl = random () % 16 == 0 ? random () % 8 : 1;
      if (random () % 50 == 0) {
        auto &s = fake.script[random () % 2 ? 0x4 : 0x1000054];
        for (unsigned n = random () % 6; n != 0; n--) { s.push_back (-1 - static_cast<int64_t> (random () % 3)); }
      }
      fake.wake = random () % 40 == 0 ? Fake::kLate : (random () % 5 == 0 ? Fake::kAtOnce : Fake::kAtDeadline);
      fake.lateBy = random () % 3000000;
      const bool wasRunning = running (&c);
      const uint32_t offBefore = c.disabled;
      const uint32_t runBefore = c.cpu[cpu].shortRun;
      fake.wfiEarly = random () % 3 == 0;
      const size_t callsBefore = fake.calls.size ();
      const int state = idleOnce (&c, &fake, cpu, left, ctl);
      if (fake.calls.size () != callsBefore) {
        CHECK (fake.calls.size () == callsBefore + 1);
        const Call &x = fake.calls.back ();
        CHECK (x.function == kCpuSuspend64 && (x.a == 0x4 || x.a == 0x1000054) && x.a != kAppsPcParam);
        CHECK (x.b == 0 && x.c == 0);
        CHECK (wasRunning && state >= 0 && (offBefore & (1u << state)) == 0 && runBefore < kShortRun);
        CHECK (mode == kModeCluster || x.a == 0x4);
        if ((ctl & 3) == 1) { CHECK (left >= c.threshold[kC4][cpu >= 6 ? 1 : 0]); }
        if (x.a == 0x1000054) { CHECK ((ctl & 3) == 1 && left >= c.threshold[kCl5][cpu >= 6 ? 1 : 0]); }
        suspends++;
      } else {
        CHECK (state == -1);
      }
    }
    uint64_t counted = 0;
    for (unsigned i = 0; i < kCpus; i++) {
      for (unsigned s = 0; s < kStates; s++) { counted += c.cpu[i].entries[s] + c.cpu[i].fails[s]; }
    }
    CHECK (counted == suspends && fake.count (kCpuSuspend64) == suspends);
    if (!usable (&c, kC4)) { CHECK (!running (&c)); }
  }
}

// Eight CPU threads entering and leaving idle while a log thread reads; cl5 starts failing on CPU 7 halfway through.
static std::atomic<uint64_t> gSuspendCalls {0}, gCl5Calls {0}, gCl5AfterOff {0};
static std::atomic<bool> gFailCl5 {false};
static Control gShared;
static thread_local uint64_t tNow = 0;
static thread_local unsigned tCpu = 0;

static int64_t threadCall (void *, uint64_t function, uint64_t a, uint64_t, uint64_t)
{
  if (function == kStatCount64) { return 3; }
  if (function != kCpuSuspend64) { return kNotSupported; }
  gSuspendCalls++;
  if (a == 0x1000054) {
    gCl5Calls++;
    if (!usable (&gShared, kCl5)) { gCl5AfterOff++; }
    if (gFailCl5 && tCpu == 7) { tNow += 3; return kDenied; }
  }
  tNow += a == 0x4 ? 15000 : 250000;
  return kSuccess;
}

static uint64_t threadNow (void *) { return tNow; }

static void testThreads ()
{
  gShared = Control {};
  CHECK (configure (&gShared, 2, kHz, 0, 0));
  addCpus (&gShared);
  Fake firmware;
  firmware.features[kStatCount64] = 0;
  Io probeIo = ioOf (&firmware);
  CHECK (probe (&gShared, probeIo));
  CHECK (selfTest (&gShared, kC4, kSuccess, 0, 1, 2) && start (&gShared));
  const Io io = {nullptr, threadCall, threadNow};
  std::atomic<bool> stop {false};
  std::atomic<unsigned> done {0};
  std::thread reader ([&] {
    Snapshot then, now;
    char line[512];
    snapshot (&gShared, &io, 0, &then);
    while (!stop) {
      snapshot (&gShared, &io, 1000000, &now);
      periodLine (&gShared, &then, &now, line, sizeof (line));
      if (strncmp (line, "idle ", 5) != 0) { fprintf (stderr, "FAIL reader: %s\n", line); abort (); }   // uncounted (own thread)
      then = now;
    }
  });
  std::vector<std::thread> cpus;
  for (unsigned cpu = 0; cpu < kCpus; cpu++) {
    cpus.emplace_back ([cpu, &io, &done] {
      tCpu = cpu;
      tNow = 1000;
      std::mt19937 random (cpu);
      for (unsigned i = 0; i < 40000; i++) {
        if (cpu == 0 && i == 20000) { gFailCl5 = true; }
        const uint64_t left = random () % 2 ? 300000 : 20000;
        const uint64_t entered = tNow;
        if (enterIdle (&gShared, io, kAff[cpu], 1, tNow + left, tNow) < 0) { tNow = entered + left; }   // XNU's WFI: the timer
        tNow += 11;
        exitIdle (&gShared, kAff[cpu], tNow);
      }
      done++;
    });
  }
  for (std::thread &t : cpus) { t.join (); }
  stop = true;
  reader.join ();
  CHECK (done == kCpus);
  CHECK (!usable (&gShared, kCl5) && gShared.cause[kCl5] == kCauseError && running (&gShared));
  // Once off, at most the requests already decided on each CPU can still name cl5.
  CHECK (gCl5AfterOff <= kCpus);
  uint64_t counted = 0;
  for (unsigned i = 0; i < kCpus; i++) {
    for (unsigned s = 0; s < kStates; s++) { counted += gShared.cpu[i].entries[s] + gShared.cpu[i].fails[s]; }
    CHECK (gShared.cpu[i].postCount == gShared.cpu[i].entries[kC4] + gShared.cpu[i].entries[kCl5]);
  }
  CHECK (counted == gSuspendCalls);
}

// Idle-pattern model of the 13T's periodic wakeups: per CPU a list of periodic timers (ms); between two consecutive timer
// interrupts the CPU idles (no other interrupts). Prints the share of idle time each mode spends in c4 / cl5 (a cluster state
// only counts while all CPUs of the cluster vote it, which platform-coordinated mode needs).
static void model (const char *name, const std::vector<std::vector<double>> &periods, uint32_t mode, double *c4Share,
                   double *clShare)
{
  Control c = {};
  Fake fake;
  ready (&c, &fake, mode);
  const uint64_t span = kHz * 10;   // 10 s
  double inC4 = 0, inCl = 0, idle = 0;
  // Per CPU: the sorted list of timer instants within the span.
  std::vector<std::vector<uint64_t>> pops (kCpus);
  for (unsigned cpu = 0; cpu < kCpus; cpu++) {
    for (double ms : periods[cpu]) {
      const uint64_t step = static_cast<uint64_t> (ms * kHz / 1000.0);
      for (uint64_t at = step / (cpu + 2); at < span; at += step) { pops[cpu].push_back (at); }
    }
    pops[cpu].push_back (span);
    std::sort (pops[cpu].begin (), pops[cpu].end ());
  }
  // Per CPU, the intervals it votes cl5 (for the cluster overlap).
  std::vector<std::vector<std::pair<uint64_t, uint64_t>>> votes (kCpus);
  for (unsigned cpu = 0; cpu < kCpus; cpu++) {
    uint64_t at = 0;
    for (uint64_t pop : pops[cpu]) {
      if (pop <= at) { continue; }
      const uint64_t left = pop - at;
      const int state = choose (&c, cpu >= 6 ? 1 : 0, 1, pop, at);
      idle += static_cast<double> (left);
      if (state == kC4) { inC4 += static_cast<double> (left); }
      if (state == kCl5) { votes[cpu].push_back ({at, pop}); inC4 += static_cast<double> (left); }
      at = pop + kHz / 10000;   // 100 us of work per wakeup
    }
  }
  // Cluster time: the overlap of all CPUs' cl5 votes in a cluster, sampled every 100 us.
  for (unsigned cluster = 0; cluster < 2; cluster++) {
    const unsigned first = cluster == 0 ? 0 : 6, last = cluster == 0 ? 6 : 8;
    std::vector<size_t> index (kCpus, 0);
    for (uint64_t t = 0; t < span; t += kHz / 10000) {
      bool all = true;
      for (unsigned cpu = first; cpu < last && all; cpu++) {
        auto &v = votes[cpu];
        while (index[cpu] < v.size () && v[index[cpu]].second <= t) { index[cpu]++; }
        all = index[cpu] < v.size () && v[index[cpu]].first <= t;
      }
      if (all) { inCl += static_cast<double> (kHz / 10000) * (last - first); }
    }
  }
  *c4Share = inC4 / idle;
  *clShare = inCl / idle;
  printf ("  model %-34s mode %u: c4-or-deeper %5.1f%% of idle time, cluster state %5.1f%%\n", name, mode, 100 * *c4Share,
          100 * *clShare);
}

static void testModel ()
{
  // Screen-off today: PaganiInput polls keys every 8 ms (on whichever CPU runs its thread, here CPU 0), the cpufreq governor
  // every 20 ms (CPU 1), BootProgress's observer every 1 s (CPU 2), the rest of XNU's housekeeping about every 100 ms.
  const std::vector<std::vector<double>> today = {{8}, {20}, {1000, 100}, {100}, {100}, {100}, {100}, {100}};
  // With the screen-off measures of the later stages: keys every 50 ms, the governor every 200 ms.
  const std::vector<std::vector<double>> quiet = {{50}, {200}, {1000}, {1000}, {1000}, {1000}, {500}, {500}};
  double c4, cl;
  model ("screen off, timers as today", today, 1, &c4, &cl);
  CHECK (c4 > 0.95 && cl == 0);
  model ("screen off, timers as today", today, 2, &c4, &cl);
  CHECK (c4 > 0.95 && cl < 0.40);   // the 8 ms key poll keeps cluster 0 from voting
  model ("screen off, slower polls (stage 2)", quiet, 2, &c4, &cl);
  CHECK (c4 > 0.98 && cl > 0.70);
}

int main ()
{
  testConfigure ();
  testCpus ();
  testProbe ();
  testSelfTest ();
  testChoose ();
  testIdle ();
  testHistory ();
  testLines ();
  testRandom ();
  testThreads ();
  testModel ();
  printf ("PASS: pagani-idle %u checks: configuration, CPU table, probe, self-test, state choice, accounting and safety "
          "switches, wake-up history, log text, random sequences, 8 CPU threads, idle model\n", checks);
  return 0;
}
