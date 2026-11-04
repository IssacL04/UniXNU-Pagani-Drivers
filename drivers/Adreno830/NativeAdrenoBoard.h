/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_BOARD_H
#define NATIVE_ADRENO_BOARD_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Host-only (never in the kernel archive, ARCHITECTURE 2.2): the driver reads no device tree at run time; the board's windows, SIDs
 * and interrupts are generated at build time from the DTS and the pinned live.dtb (ARCHITECTURE 3.2). This parser checks those
 * inputs in the host tests (tests/test_board.cpp: the SM8750 / OnePlus 13T GPU, GMU and GPU-SMMU cells).
 * Device-tree memory-window parsing for an Adreno platform node. Cells are
 * big-endian 32-bit as stored in the flattened device tree; the caller passes
 * the raw property bytes and the node's own #address-cells/#size-cells.
 * Only 1 or 2 cells per value are accepted, zero-length windows and any
 * address/size overflow are refused, and the output count is the exact number
 * of complete windows. This parses only; it does not map, claim, power or
 * validate that the addresses belong to a GPU. */
struct NativeAdrenoWindow {
    uint64_t base;
    uint64_t bytes;
};
enum { NativeAdrenoMaxWindows = 8 };
class NativeAdrenoBoard final {
    NativeAdrenoBoard()=delete;
public:
    /* Parse a device's `iommus` property. Each entry is one phandle cell
     * followed by #iommu-cells specifier cells; the first specifier cell is the
     * stream ID on this platform. Output keeps property order and never writes
     * on failure. Only the SID is extracted, never a guessed context bank. */
    static IOReturn parseIommus(const uint8_t *cells,size_t length,
        uint32_t cells_per_entry,uint16_t *stream_ids,size_t capacity,size_t *count);
    static IOReturn parseReg(const uint8_t *cells,size_t length,uint32_t address_cells,
        uint32_t size_cells,NativeAdrenoWindow *windows,size_t capacity,size_t *count);
};
#endif
