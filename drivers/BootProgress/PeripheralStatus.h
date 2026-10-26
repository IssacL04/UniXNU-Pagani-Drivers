// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_PERIPHERAL_STATUS_H
#define PAGANI_PERIPHERAL_STATUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PAGANI_KEYS, PAGANI_TOUCH, PAGANI_PANEL, PAGANI_PERIPHERALS };
void boot_progress_peripheral_enable(int enabled);
void boot_progress_peripheral(unsigned device, const char *stage, int32_t result, uint32_t value, uint64_t reports);
void boot_progress_peripheral_frame(void);
#ifdef __cplusplus
}
#endif
#endif
