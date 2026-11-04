// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the firmware reader (AdspFirmware.c over FatVolume.c and QcomMdt.c), after UniXNU-sheng package/tests/modem_fw_host.c:
//   firmware VOLUME.img CASES_DIR
// VOLUME.img is a FAT volume made by mtools (check-pagani-adsp-host.py), presented as partition modem_a on LUN 4 from 4 KiB block 4870
// as on the PKX110. CASES_DIR/cases lists "name rc base size" (rc: the expected PaganiFw code of open+check+load) and, for each image
// that loads, NAME.meta and NAME.region: the metadata and the carveout the script computed on its own (0xa5 where no segment goes).
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AdspFirmware.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size ? *size : 1);
    CHECK(data && fread(data, 1, *size, f) == *size);
    fclose(f);
    return data;
}

enum {kLun = 4, kFirst = 4870};
typedef struct {
    const uint8_t *data;
    size_t size;
    unsigned finds, not_ready, reads, fail_after;   /* fail_after: refuse every read after this many (0: never) */
    unsigned total_reads;
    uint64_t blocks_read;
    size_t allocated;
} Disk;

static int disk_find(void *context, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    Disk *d = context;
    d->finds++;
    if (d->not_ready) {d->not_ready--; return 0;}
    if (strcmp(name, "modem_a")) return 0;
    *lun = kLun;
    *first = kFirst;
    *blocks = d->size / 4096;
    return 1;
}
static int disk_read(void *context, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
    Disk *d = context;
    CHECK(lun == kLun && lba >= kFirst && blocks && blocks <= PAGANI_FW_RUN_BLOCKS);
    if (d->fail_after && d->reads >= d->fail_after) return 0;
    d->reads++;
    d->total_reads++;
    d->blocks_read += blocks;
    const uint64_t at = (lba - kFirst) * 4096;
    CHECK(at + (uint64_t)blocks * 4096 <= d->size);
    memcpy(dst, d->data + at, (size_t)blocks * 4096);
    return 1;
}
static void *disk_alloc(void *context, size_t bytes)
{
    Disk *d = context;
    d->allocated += bytes;
    return calloc(1, bytes);
}
static void disk_free(void *context, void *memory, size_t bytes)
{
    Disk *d = context;
    CHECK(d->allocated >= bytes);
    d->allocated -= bytes;
    free(memory);
}

static Disk disk;
static const PaganiFwDisk ops = {&disk, disk_find, disk_read, disk_alloc, disk_free};
static const char *const names[] = {"modem_a", "modem_b"};

static void partition_reads(PaganiFwVolume *v)
{
    /* Any range, through the block cache and the whole-block path, equals the volume's bytes. */
    static uint8_t got[300000];
    const uint64_t offsets[] = {0, 1, 4095, 4096, 4097, 8191, 12345, 65536 + 7};
    const size_t lengths[] = {1, 7, 4095, 4096, 4097, 8192, 65537, 270000};
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; ++i)
        for (unsigned k = 0; k < sizeof lengths / sizeof lengths[0]; ++k) {
            if (offsets[i] + lengths[k] > disk.size) continue;
            memset(got, 0x5a, lengths[k]);
            CHECK(pagani_fw_partition_read(v, offsets[i], got, lengths[k]) == 0);
            CHECK(!memcmp(got, disk.data + offsets[i], lengths[k]));
        }
    CHECK(pagani_fw_partition_read(v, disk.size - 1, got, 2) == -1);
}

static void run_case(PaganiFwVolume *v, const char *dir, const char *name, int want, uint64_t base, uint64_t size)
{
    static PaganiFwImage image;
    char path[512];
    int rc = pagani_fw_open(&image, v, "image", name);
    if (!rc) rc = pagani_fw_check(&image, base, size);
    uint8_t *region = 0;
    if (!rc) {
        region = malloc((size_t)size);
        memset(region, 0xa5, (size_t)size);
        uint64_t written = 0;
        rc = pagani_fw_load(&image, region, base, size, &written);
        if (!rc) CHECK(written == image.file_bytes);
    }
    if (rc != want) fprintf(stderr, "%s: rc %d, expected %d (detail %d, path %s)\n", name, rc, want, image.detail, image.path);
    CHECK(rc == want);
    if (!rc) {
        size_t n = 0;
        snprintf(path, sizeof path, "%s/%s.meta", dir, name);
        uint8_t *meta = slurp(path, &n);
        CHECK(meta && n == pagani_fw_metadata_size(&image));
        uint8_t *got = malloc(n);
        CHECK(pagani_fw_metadata(&image, got, n) == PaganiFwOk && !memcmp(got, meta, n));
        CHECK(pagani_fw_metadata(&image, got, n - 1) == PaganiFwArgument);
        snprintf(path, sizeof path, "%s/%s.region", dir, name);
        uint8_t *expect = slurp(path, &n);
        CHECK(expect && n == size && !memcmp(region, expect, n));
        free(meta); free(got); free(expect);
    }
    free(region);
    pagani_fw_close(&image);
    CHECK(disk.allocated == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    disk.data = slurp(argv[1], &disk.size);
    CHECK(disk.data && disk.size % 4096 == 0);
    static PaganiFwVolume v;
    /* Storage not up yet, then up; arguments; a missing partition name. */
    disk.not_ready = 1;
    CHECK(pagani_fw_mount(&v, &ops, names, 2) == PaganiFwNoPartition && disk.finds == 2);
    CHECK(pagani_fw_mount(&v, &ops, names, 0) == PaganiFwArgument);
    const char *const other[] = {"modem_b"};
    CHECK(pagani_fw_mount(&v, &ops, other, 1) == PaganiFwNoPartition);
    CHECK(pagani_fw_mount(&v, &ops, names, 2) == PaganiFwOk && v.lun == kLun && v.first == kFirst && !strcmp(v.partition, "modem_a"));
    partition_reads(&v);

    char path[512], name[64];
    snprintf(path, sizeof path, "%s/cases", argv[2]);
    FILE *cases = fopen(path, "r");
    CHECK(cases);
    int want;
    uint64_t base, size;
    unsigned ran = 0;
    while (fscanf(cases, "%63s %d %" SCNx64 " %" SCNx64, name, &want, &base, &size) == 4) {
        run_case(&v, argv[2], name, want, base, size);
        ran++;
    }
    fclose(cases);
    CHECK(ran >= 8);

    /* A disk that starts refusing reads: an I/O error, never a crash or a loop. */
    disk.reads = 0;
    disk.fail_after = 3;
    static PaganiFwVolume w;
    int rc = pagani_fw_mount(&w, &ops, names, 2);
    static PaganiFwImage image;
    if (!rc) rc = pagani_fw_open(&image, &w, "image", "adsp");
    if (!rc) {
        uint8_t *region = malloc(0x100000);
        rc = pagani_fw_load(&image, region, 0x9ee00000, 0x100000, 0);
        free(region);
    }
    CHECK(rc == PaganiFwIo);
    pagani_fw_close(&image);
    disk.fail_after = 0;
    /* Not a FAT volume. */
    uint8_t *copy = malloc(disk.size);
    memcpy(copy, disk.data, disk.size);
    memset(copy, 0, 512);
    const uint8_t *saved = disk.data;
    disk.data = copy;
    CHECK(pagani_fw_mount(&w, &ops, names, 2) == PaganiFwNotFat);
    disk.data = saved;
    free(copy);
    CHECK(disk.allocated == 0);
    printf("PASS firmware reader (%s, %u-byte sectors, %u cases): %u checks, %u disk reads, %" PRIu64 " blocks\n",
           v.fat.fat32 ? "FAT32" : "FAT16", v.fat.sector_bytes, ran, checks, disk.total_reads, disk.blocks_read);
    return 0;
}
