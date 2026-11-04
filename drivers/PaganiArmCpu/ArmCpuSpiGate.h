// SPDX-License-Identifier: BSD-2-Clause-Patent
// The enable gate behind armcpu_spi_disable/armcpu_spi_enable (ArmCpu.cpp), kept apart so the host test
// (Test/spi-gate.cpp) runs the same code. A driver may only switch a line it attached: an INTID without a slot is refused
// before any distributor write. Disable clears the line in GICD_ICENABLER<n> and waits for GICD_CTLR.RWP; enable sets it in
// GICD_ISENABLER<n> after a barrier. The slot, the handler and the line's configuration stay as attach left them, so enable
// restores exactly the attached state. Both may run in the primary interrupt context (a storm guard disabling its own line
// before the EOI): no lock, no sleep, RWP is a bounded spin.
#ifndef ARMCPU_SPI_GATE_H
#define ARMCPU_SPI_GATE_H
#include <stdint.h>

enum ArmCpuSpiGateResult {ArmCpuSpiGateOk, ArmCpuSpiGateNotReady, ArmCpuSpiGateNotAttached, ArmCpuSpiGateTimeout};

// Whether intid has one of the first `used` slots (Slot has a member `id`).
template <typename Slot>
static inline bool armCpuSpiAttached (const Slot *slots, uint32_t used, uint32_t intid)
{
  for (uint32_t slot = 0; slot < used; slot++) {
    if (slots[slot].id == intid) { return true; }
  }
  return false;
}

// dist: the mapped distributor (32-bit registers); ready: the GIC is initialized; rwpClear: waits for GICD_CTLR.RWP == 0.
static inline ArmCpuSpiGateResult armCpuSpiGate (volatile uint32_t *dist, bool ready, bool attached, uint32_t intid, bool enable,
                                                 bool (*rwpClear) (volatile uint32_t *))
{
  if (dist == nullptr || !ready) { return ArmCpuSpiGateNotReady; }
  if (intid < 32 || intid >= 1020 || !attached) { return ArmCpuSpiGateNotAttached; }
  const uint32_t word = intid / 32, bit = 1U << (intid % 32);
  if (enable) {
#if defined(__aarch64__)
    __asm__ volatile ("dsb sy" ::: "memory");
#else
    __atomic_thread_fence (__ATOMIC_SEQ_CST);
#endif
    dist[(0x100 / 4) + word] = bit;   // GICD_ISENABLER<n>
    return ArmCpuSpiGateOk;
  }
  dist[(0x180 / 4) + word] = bit;     // GICD_ICENABLER<n>
  return rwpClear (dist) ? ArmCpuSpiGateOk : ArmCpuSpiGateTimeout;
}
#endif
