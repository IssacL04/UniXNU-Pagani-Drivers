/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The partition reader (one-block cache over NativeUfs 4 KiB blocks) follows PaganiAdsp AdspFirmware.c, itself after UniXNU-sheng
 * package/AdspFirmware.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e); the rest is new. */
/* The peach firmware files, found by the names an ath12k-style stack asks for (basenames: "amss.bin", "board.bin", ...) and served from
 * where the phone keeps them, never from the boot image and never written anywhere:
 *   1 in place from the firmware partition (modem_a, then modem_b: NativeUfs native_ufs_part_find/read, a FAT volume), in the
 *     directories image/peach then image (Android mounts modem_a at /vendor/firmware_mnt; every file below is there, read back from the
 *     phone's real volume with this code on 2026-11-01, sha256 equal);
 *   2 the RAM Data seed: SEED/<Android name> then SEED/<basename> (SEED = /private/var/ngpu/firmware/wlan; local files put there by
 *     build-userspace/a830-inputs/wifi/make-wifi-firmware-dir.py). The seed is read only after the Data volume is mounted: until
 *     then its reader answers PwFwNotYet and nothing is cached.
 * board.bin and regdb.bin take the opposite order, the seed first: Android loads those two from /odm/etc/wifi/peach (dmesg of 2026-10-31),
 * different bytes from modem_a's, and /odm is erofs in super, out of this driver's reach, so the seed is where they can be. Their lookup
 * answers PwFwNotYet until the volume is mounted, and the owner, which waits for it, calls pw_fw_seed_gone when it gives up: from then on
 * the partition's copies are taken. A seed without them means the partition's copies at once.
 * Android names (cnss2 on this phone, facts.md section 8): amss.bin = peach/amss20.bin (fallback amss20.bin), board.bin = the BDF
 * (peach/bdwlan.elf, or the pagani-wifi-bdf override), regdb.bin = peach/regdb.bin, m3.bin = peach/phy_ucode20.elf (only with
 * pagani-wifi-m3=1), aux_ucode.bin = peach/aux_ucode20.elf, tmel_peach_20.elf (no prefix), the QDSS trace configuration
 * (peach/qdss_trace_config_perf_v2.cfg, fallback peach/qdss_trace_config_v2.cfg). join.txt (the unattended join's network name and
 * key) comes from the seed only. Every other request (firmware-2.bin, board-2.bin, caldata.bin, ...) is answered "absent", which the
 * stack takes as its cue for the single-file names.
 *
 * Portable C over callbacks (the kernel: NativeUfs and the VFS; the host tests: FAT images made by mtools and a directory). A file
 * is read whole, once, and stays cached for the life of the driver (the stack keeps pointers into it). */
#ifndef PAGANI_WIFI_FIRMWARE_H
#define PAGANI_WIFI_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#include "WifiFat.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PwFwOk = 0,
    PwFwNoPartition = -1,   /* none of the partitions was found (or storage is not up yet) */
    PwFwNotFat = -2,        /* the partition holds no FAT volume */
    PwFwAbsent = -3,        /* no source has the file */
    PwFwIoError = -5,            /* a read failed (storage, a damaged volume, the VFS) */
    PwFwTooBig = -6,        /* larger than PW_FW_MAX_BYTES */
    PwFwArgument = -8,
    PwFwNoMemory = -9,
    PwFwNotYet = -10,       /* the seed is not mounted yet (the VFS answered "no such path" before the Data volume is up) */
    PwFwFull = -11,         /* every cache slot is taken */
};
enum {PwFwSrcNone = 0, PwFwSrcFat = 1, PwFwSrcSeed = 2};

#define PW_FW_BLOCK 4096u
#define PW_FW_RUN_BLOCKS 256u           /* 1 MiB per disk read */
#define PW_FW_SLOTS 14u
#define PW_FW_PATH 256u                 /* the longest path kept (seed directory + Android name) */
#define PW_FW_MAX_BYTES (32u << 20)
#define PW_FW_SEED_DIR "/private/var/ngpu/firmware/wlan"
#define PW_FW_DEFAULT_BDF "bdwlan.elf"

typedef struct {
    void *context;
    /* NativeUfs: 1 when the partition was found (its LUN, first 4 KiB block and block count), 0 otherwise. */
    int (*find)(void *context, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
    /* 1 when `blocks` 4 KiB blocks from lba of the LUN were read into dst. */
    int (*read)(void *context, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
    /* The seed: 0 and the size when the file exists; PwFwAbsent when it does not; PwFwNotYet before the volume is mounted. May be null
     * (no seed). */
    int (*file_size)(void *context, const char *path, uint64_t *size);
    /* 0 when length bytes at offset were read. */
    int (*file_read)(void *context, const char *path, uint64_t offset, void *dst, size_t length);
    void *(*alloc)(void *context, size_t bytes);
    void (*free)(void *context, void *memory, size_t bytes);
} PwFwIo;

typedef struct {
    const char *partitions[4];          /* GPT names, in order (default modem_a, modem_b) */
    unsigned partition_count;           /* 0: no partition is read */
    const char *dirs[4];                /* FAT directories, in order (default image/peach, image) */
    unsigned dir_count;
    const char *seed_dir;               /* null: PW_FW_SEED_DIR; "" : no seed */
    const char *bdf;                    /* board data basename; null: PW_FW_DEFAULT_BDF */
    uint32_t m3;                        /* 1: m3.bin is peach/phy_ucode20.elf; 0: m3.bin is absent (the null-M3 mode) */
} PwFwConfig;

typedef struct {
    char request[32];                   /* what the stack asked for */
    uint8_t *data;
    uint32_t size, allocated;
    uint8_t source;                     /* PwFwSrc* */
    char where[PW_FW_PATH];             /* the name it was found under, "modem_a:image/peach/amss20.bin" or the seed path */
} PwFwFile;

typedef struct {
    PwFwIo io;
    PwFwConfig config;
    char seed_dir[PW_FW_PATH - 64], bdf[32];
    /* the partition */
    int mount_rc;                       /* 1 not tried, else the last pw_fw_mount result */
    uint32_t lun;
    uint64_t first, blocks;
    char partition[24];
    uint64_t cached;                    /* the block in `block`, UINT64_MAX when none */
    uint8_t block[PW_FW_BLOCK];
    FatVolume fat;
    int disk_failed;
    /* the cache */
    PwFwFile files[PW_FW_SLOTS];
    unsigned file_count;
    int seed_gone;                      /* pw_fw_seed_gone: nothing waits for the seed any more */
    /* counters for the log */
    uint32_t disk_reads, seed_reads, lookups, misses;
    int last_error;
    char last_path[PW_FW_PATH];
} PwFw;

void pw_fw_init(PwFw *fw, const PwFwIo *io, const PwFwConfig *config);
/* Finds the first configured partition and mounts its FAT volume (again after a PwFwNoPartition: storage may come up later). */
int pw_fw_mount(PwFw *fw);
/* The candidate names of a request, in the order they are tried (Android names; the FAT directories take their basenames). Returns
 * their count (0: the request is always answered absent). seed_only is set for files that come from the seed alone. */
unsigned pw_fw_candidates(const PwFw *fw, const char *request, const char *out[4], int *seed_only);
/* A file by the name the stack asks for (its last path component is used): PwFwOk with the cached bytes, or an error. Never caches
 * a failure (a later call looks again: the seed may have been mounted in between). */
int pw_fw_get(PwFw *fw, const char *request, const uint8_t **data, uint32_t *size);
/* The Data volume did not come in the time the owner gives it: board.bin and regdb.bin stop waiting for the seed. */
void pw_fw_seed_gone(PwFw *fw);
/* The cache entry of a loaded file (null when not loaded), for the log. */
const PwFwFile *pw_fw_loaded(const PwFw *fw, const char *request);
/* A byte range of the mounted partition (offset from its start); exposed for the tests. */
int pw_fw_partition_read(void *fw, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
