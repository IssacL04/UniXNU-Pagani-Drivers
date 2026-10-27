// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_PERIPHERAL_STATUS_H
#define PAGANI_PERIPHERAL_STATUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PAGANI_KEYS, PAGANI_TOUCH, PAGANI_PANEL, PAGANI_SPI, PAGANI_SPI_FIFO, PAGANI_PERIPHERALS };
void boot_progress_peripheral_enable(int enabled);
void boot_progress_peripheral(unsigned device, const char *stage, int32_t result, uint32_t value, uint64_t reports);
void boot_progress_touch_spi(uint32_t firmware, uint32_t disabled, uint32_t state, uint32_t irq, uint32_t tx, uint32_t rx);
void boot_progress_touch_dma(uint32_t tx, uint32_t rx, uint32_t txin, uint32_t rxin);
void boot_progress_touch_read(uint32_t length, uint32_t head, uint32_t pins, uint32_t dma,
                              uint32_t state, uint32_t txin, uint32_t rxin);
void boot_progress_touch_smmu(uint32_t slot, uint32_t bank, uint32_t smr, uint32_t s2cr);
void boot_progress_peripheral_frame(void);
#ifdef __cplusplus
}
#endif
#endif
