/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The read-only file tree the ADSP's sensor protection domain reads through FastRPC (SensorsRpc.h): the phone's /vendor/etc/sensors,
 * /mnt/vendor/persist/sensors and /sys/devices/soc0, pulled once from Android and packed on the host by scripts/make-sensor-pack.py
 * into one file of the RAM Data seed (/private/var/ngpu/sensors/pack.bin). Reading vendor's EROFS and persist's ext4 in place would need
 * two file systems and super's partition table in the kernel for what one pack does; a pack also holds exactly what Android showed the
 * DSP, the phone's own calibration included. Portable C without a C library.
 *
 * The file (little endian):
 *   +0   "SSCPACK1"
 *   +8   u32 entries, u32 names (bytes of the name area)
 *   +16  u64 total (the size of the file)
 *   +24  u32 sum (of the bytes of the table and the names), u32 zero
 *   +32  entries: u64 offset, u64 size, u32 name (offset into the name area), u32 flags (1: a directory); sorted by name, byte by byte
 *   then the names (NUL terminated absolute paths), then the files' bytes. */
#ifndef SENSORS_PACK_H
#define SENSORS_PACK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {SSP_HEADER = 32, SSP_ENTRY = 24, SSP_MAX_ENTRIES = 8192, SSP_MAX_NAMES = 0x100000, SSP_PATH = 256, SSP_DIRECTORY = 1};

typedef struct {
    uint32_t count, names, sum;
    uint64_t total;
} SspHeader;
/* 0 and the header's fields, -1 when it is not a pack header. */
int ssp_header(const uint8_t *bytes, uint32_t length, SspHeader *out);
/* The bytes of the table and the names that follow the header. */
uint32_t ssp_table_bytes(const SspHeader *h);

typedef struct {
    const uint8_t *table;              /* the entries, then the names */
    uint32_t count, names;
    uint64_t total;
} SspPack;
/* Checks the whole table (sum, sorted names, every name terminated, every file inside total): 0, or -1. A zeroed SspPack is the empty
 * pack: every lookup misses. */
int ssp_open(SspPack *p, const SspHeader *h, const uint8_t *table);

typedef struct {
    uint64_t offset, size;
    const char *name;
    uint32_t flags;
} SspEntry;
void ssp_entry(const SspPack *p, uint32_t index, SspEntry *out);
/* The index of a canonical path, -1 when it is not there. */
int ssp_find(const SspPack *p, const char *path);
/* A DSP path (length bytes, NUL terminated or not) made canonical: absolute, single slashes, no "." segment, no trailing slash, and the
 * aliases the DSP may use for the same files: /system/vendor/... is /vendor/..., /persist/... is /mnt/vendor/persist/.... The length, or
 * -1 for a relative path, "..", or one that does not fit. */
int ssp_path(const char *in, uint32_t length, char *out, uint32_t capacity);
/* The next direct child of directory dir: *cursor starts at 0; 1 and the child (leaf: its last segment), 0 at the end. */
int ssp_child(const SspPack *p, uint32_t dir, uint32_t *cursor, SspEntry *child, const char **leaf);

#ifdef __cplusplus
}
#endif
#endif
