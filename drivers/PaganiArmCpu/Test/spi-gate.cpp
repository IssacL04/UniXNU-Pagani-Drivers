// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the armcpu_spi_disable/armcpu_spi_enable gate (ArmCpuSpiGate.h): only an attached INTID is switched, an
// unknown or out-of-range one is refused before any distributor write, disable writes GICD_ICENABLER<n> and waits for RWP,
// enable writes GICD_ISENABLER<n>, a GIC that is not ready refuses both.
// Build: xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined Test/spi-gate.cpp -o /tmp/spi-gate
#include "../ArmCpuSpiGate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct Slot {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};

static uint32_t dist[0x10000 / 4];
static unsigned rwpPolls;
static bool rwpResult = true;
static bool rwp (volatile uint32_t *d)
{
  assert (d == dist);
  rwpPolls++;
  return rwpResult;
}

static unsigned written ()
{
  unsigned n = 0;
  for (uint32_t w : dist) { n += w != 0; }
  return n;
}

int main ()
{
  Slot slots[16] = {};
  slots[0].id = 332;
  slots[1].id = 710;
  slots[2].id = 994;
  const uint32_t used = 3;
  unsigned checks = 0;
#define CHECK(x) do {assert (x); checks++;} while (0)
  CHECK (armCpuSpiAttached (slots, used, 332) && armCpuSpiAttached (slots, used, 994));
  CHECK (!armCpuSpiAttached (slots, used, 336) && !armCpuSpiAttached (slots, 2, 994) && !armCpuSpiAttached (slots, 0, 332));
  // disable an attached line: ICENABLER10 (332 / 32 = 10) bit 12, then RWP
  CHECK (armCpuSpiGate (dist, true, true, 332, false, rwp) == ArmCpuSpiGateOk);
  CHECK (dist[(0x180 / 4) + 10] == (1U << 12) && rwpPolls == 1 && written () == 1);
  memset (dist, 0, sizeof dist);
  // enable it again: ISENABLER10 bit 12, no RWP wait
  CHECK (armCpuSpiGate (dist, true, true, 332, true, rwp) == ArmCpuSpiGateOk);
  CHECK (dist[(0x100 / 4) + 10] == (1U << 12) && rwpPolls == 1 && written () == 1);
  memset (dist, 0, sizeof dist);
  // 710: ICENABLER22 bit 6
  CHECK (armCpuSpiGate (dist, true, true, 710, false, rwp) == ArmCpuSpiGateOk && dist[(0x180 / 4) + 22] == (1U << 6));
  memset (dist, 0, sizeof dist);
  // a line nobody attached, or out of the SPI range: refused, nothing written
  CHECK (armCpuSpiGate (dist, true, armCpuSpiAttached (slots, used, 336), 336, false, rwp) == ArmCpuSpiGateNotAttached);
  CHECK (armCpuSpiGate (dist, true, armCpuSpiAttached (slots, used, 336), 336, true, rwp) == ArmCpuSpiGateNotAttached);
  CHECK (armCpuSpiGate (dist, true, true, 31, false, rwp) == ArmCpuSpiGateNotAttached);
  CHECK (armCpuSpiGate (dist, true, true, 1020, true, rwp) == ArmCpuSpiGateNotAttached);
  CHECK (written () == 0 && rwpPolls == 2);
  // the GIC is not ready (or not mapped): refused
  CHECK (armCpuSpiGate (dist, false, true, 332, false, rwp) == ArmCpuSpiGateNotReady);
  CHECK (armCpuSpiGate (nullptr, true, true, 332, true, rwp) == ArmCpuSpiGateNotReady && written () == 0);
  // RWP never clears: the disable reports a timeout (the write itself was made)
  rwpResult = false;
  CHECK (armCpuSpiGate (dist, true, true, 994, false, rwp) == ArmCpuSpiGateTimeout && dist[(0x180 / 4) + 31] == (1U << 2));
  printf ("PASS: ArmCpu SPI gate %u checks: disable/enable only for attached INTIDs, ICENABLER + RWP, ISENABLER\n", checks);
  return 0;
}
