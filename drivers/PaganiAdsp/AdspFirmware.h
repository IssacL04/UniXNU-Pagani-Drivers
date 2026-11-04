/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The partition reader follows UniXNU-sheng package/AdspFirmware.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). */
/* Remote processor firmware taken in place from the device's own firmware partition (modem_a on the OnePlus 13T: LUN 4, FAT): never
 * part of the build, never copied into the boot image. An image NAME is read the way the Linux MDT loader reads it (facts only, no
 * code): DIR/NAME.mdt holds the ELF and program headers and the hash segment (the metadata the secure world authenticates), and each
 * loadable segment i whose bytes are not inside the .mdt comes from DIR/NAME.bNN, which must be exactly the segment's file size.
 * When NAME.mdt is missing, NAME.b00 (the headers alone) stands in for it and the hash comes from its own .bNN.
 *
 * Portable C over two callbacks (find a partition by its GPT name, read 4 KiB blocks of a LUN) and an allocator: the kernel passes
 * NativeUfs native_ufs_part_find/read, the host tests a FAT image file. Nothing here waits, and nothing is ever written to storage. */
#ifndef PAGANI_ADSP_FIRMWARE_H
#define PAGANI_ADSP_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#include "FatVolume.h"
#include "QcomMdt.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *context;
    /* 1 when the partition was found: its LUN, first 4 KiB block and block count; 0 otherwise (also while storage is not up). */
    int (*find)(void *context, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
    /* 1 when `blocks` 4 KiB blocks from lba of the LUN were read into dst. */
    int (*read)(void *context, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
    void *(*alloc)(void *context, size_t bytes);
    void (*free)(void *context, void *memory, size_t bytes);
} PaganiFwDisk;

enum {
    PaganiFwOk = 0,
    PaganiFwNoPartition = -1,   /* none of the partition names was found (or storage is not up yet: try again later) */
    PaganiFwNotFat = -2,        /* the partition holds no FAT16/FAT32 volume */
    PaganiFwNoFile = -3,        /* NAME.mdt and NAME.b00, or a segment's NAME.bNN, is missing */
    PaganiFwBadImage = -4,      /* not a signed firmware ELF (detail: the QcomMdt code) */
    PaganiFwIo = -5,            /* a read failed (storage, or a damaged volume: detail is the Fat code) */
    PaganiFwSize = -6,          /* a NAME.bNN is not exactly its segment's size, or the headers file is too small or too large */
    PaganiFwOutside = -7,       /* a segment does not fit the carveout */
    PaganiFwArgument = -8,
    PaganiFwNoMemory = -9,
};

#define PAGANI_FW_HEADERS_MAX (256u * 1024u)   /* NAME.mdt: headers, hash and signature (a few KiB in practice) */
#define PAGANI_FW_BLOCK 4096u
#define PAGANI_FW_RUN_BLOCKS 256u              /* blocks per disk read */

typedef struct {
    PaganiFwDisk disk;
    uint32_t lun;
    uint64_t first, blocks;                 /* the partition, in 4 KiB blocks */
    char partition[24];                     /* the name that mounted */
    uint64_t cached;                        /* the block in `block`, UINT64_MAX when none */
    uint8_t block[PAGANI_FW_BLOCK];
    FatVolume fat;
    uint32_t reads;                         /* disk read calls, for the log */
    uint64_t read_blocks;
    int disk_failed;                        /* a disk read refused: an I/O error, not a damaged volume */
} PaganiFwVolume;

typedef struct {
    PaganiFwVolume *volume;
    char name[24];
    char path[64];                          /* the last file looked at, for the log */
    uint8_t *headers;                       /* NAME.mdt (or NAME.b00), whole */
    uint32_t headers_bytes;
    uint8_t from_b00;
    QcomMdtImage elf;
    FatFile piece[QCOM_MDT_MAX_SEGMENTS];   /* NAME.bNN by program header index */
    uint8_t has_piece[QCOM_MDT_MAX_SEGMENTS];
    uint32_t loadable;                      /* loadable segments */
    uint32_t pieces;                        /* .bNN files the image uses */
    uint64_t file_bytes;                    /* the loadable segments' file bytes (from the .bNN files or the .mdt) */
    int detail;                             /* the Fat/QcomMdt code or the segment index of the last failure, -1 none */
} PaganiFwImage;

/* Finds the first of `names` (GPT partition names) and mounts its FAT volume. */
int pagani_fw_mount(PaganiFwVolume *v, const PaganiFwDisk *disk, const char *const *names, unsigned count);
/* Reads DIR/NAME.mdt (else DIR/NAME.b00), parses it, and finds every .bNN the image needs (sizes checked). */
int pagani_fw_open(PaganiFwImage *image, PaganiFwVolume *v, const char *directory, const char *name);
void pagani_fw_close(PaganiFwImage *image);

/* The metadata for the peripheral authentication service: ELF and program headers, then the hash segment. */
uint64_t pagani_fw_metadata_size(const PaganiFwImage *image);
int pagani_fw_metadata(PaganiFwImage *image, uint8_t *to, uint64_t capacity);
/* Bytes from the lowest loadable address to the highest end rounded up to 4 KiB: the size of the memory setup call. */
uint64_t pagani_fw_span(const PaganiFwImage *image);
/* PaganiFwOk when every loadable segment fits a carveout of region_size bytes at region_base (checked before any secure call). */
int pagani_fw_check(const PaganiFwImage *image, uint64_t region_base, uint64_t region_size);
/* Writes every loadable segment into the mapped carveout (region: its first byte) and zeroes each segment's tail; *written gets the
 * segments' file bytes written (file_bytes when it succeeds). */
int pagani_fw_load(PaganiFwImage *image, uint8_t *region, uint64_t region_base, uint64_t region_size, uint64_t *written);

/* A byte range of the partition (offset from its start), through the one-block cache; exposed for the tests. */
int pagani_fw_partition_read(void *volume, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
