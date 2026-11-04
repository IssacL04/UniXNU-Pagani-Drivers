// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of WifiFirmware.c (the firmware sources): FAT volumes made by mtools (check-pagani-wifi-host.py) standing for modem_a,
// read through a model of NativeUfs's 4 KiB block calls, and a seed directory standing for /private/var/ngpu/firmware/wlan.
//   firmware FAT16_IMAGE FAT32_IMAGE STAGING_DIR SEED_DIR
// FAT16 (4096-byte sectors, like the phone's modem_a): image/peach/{amss20.bin, bdwlan.elf, bdwlan.b0e, regdb.bin, phy_ucode20.elf,
// aux_ucode20.elf, qdss_trace_config_v2.cfg, join.txt} and image/tmel_peach_20.elf. FAT32: image/amss20.bin only. The seed:
// peach/qdss_trace_config_perf_v2.cfg, join.txt, and in odm/ a seed that carries Android's /odm files, peach/regdb.bin and
// peach/bdwlan.elf (different bytes from the partition's); big/ holds a 33 MiB file. STAGING_DIR has the bytes each file must read back as.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../WifiFirmware.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

typedef struct {const char *path; uint8_t *data; size_t size;} Blob;
static Blob slurp(const char *path)
{
    Blob b = {path, NULL, 0};
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    b.size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(b.size ? b.size : 1);
    CHECK(b.data && fread(b.data, 1, b.size, f) == b.size);
    fclose(f);
    return b;
}

/* ---- NativeUfs model: partitions by name over image files ---- */
typedef struct {
    const Blob *modem_a, *modem_b;   /* null: no such partition */
    unsigned finds, reads, fail_reads;
    const char *seed;
    int seed_mounted, seed_fail;
    unsigned seed_lookups;
    size_t allocated, freed;
} Model;
static Model m;
static const Blob *part(const char *name) {return !strcmp(name, "modem_a") ? m.modem_a : !strcmp(name, "modem_b") ? m.modem_b : NULL;}
static int find(void *ctx, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    (void)ctx;
    m.finds++;
    const Blob *b = part(name);
    if (!b) return 0;
    *lun = 4;
    *first = !strcmp(name, "modem_a") ? 4870 : 94470;
    *blocks = b->size / PW_FW_BLOCK;
    return 1;
}
static int read_blocks(void *ctx, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
    (void)ctx;
    m.reads++;
    if (m.fail_reads) return 0;
    CHECK(lun == 4 && blocks >= 1 && blocks <= PW_FW_RUN_BLOCKS);
    const Blob *b = lba >= 94470 && m.modem_b ? m.modem_b : m.modem_a;
    const uint64_t first = b == m.modem_b ? 94470 : 4870;
    CHECK(lba >= first && (lba - first + blocks) * PW_FW_BLOCK <= b->size);
    memcpy(dst, b->data + (lba - first) * PW_FW_BLOCK, (size_t)blocks * PW_FW_BLOCK);
    return 1;
}
/* ---- the VFS model ---- */
static int file_size(void *ctx, const char *path, uint64_t *size)
{
    (void)ctx;
    m.seed_lookups++;
    if (!m.seed_mounted) return PwFwNotYet;
    if (m.seed_fail) return PwFwIoError;
    CHECK(!strncmp(path, m.seed, strlen(m.seed)));
    struct stat s;
    if (stat(path, &s)) return errno == ENOENT ? PwFwAbsent : PwFwIoError;
    *size = (uint64_t)s.st_size;
    return 0;
}
static int file_read(void *ctx, const char *path, uint64_t offset, void *dst, size_t length)
{
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return PwFwIoError;
    fseek(f, (long)offset, SEEK_SET);
    const size_t got = fread(dst, 1, length, f);
    fclose(f);
    return got == length ? 0 : PwFwIoError;
}
static void *alloc(void *ctx, size_t n) {(void)ctx; m.allocated += n; return malloc(n);}
static void release(void *ctx, void *p, size_t n) {(void)ctx; m.freed += n; free(p);}
static const PwFwIo io = {NULL, find, read_blocks, file_size, file_read, alloc, release};

static char staging[512];
static void same(const uint8_t *data, uint32_t size, const char *relative)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s", staging, relative);
    Blob b = slurp(path);
    CHECK(size == b.size && !memcmp(data, b.data, b.size));
    free(b.data);
}
static PwFw fw;
static PwFwConfig config(int m3, const char *bdf)
{
    PwFwConfig c;
    memset(&c, 0, sizeof c);
    c.partitions[0] = "modem_a";
    c.partitions[1] = "modem_b";
    c.partition_count = 2;
    c.dirs[0] = "image/peach";
    c.dirs[1] = "image";
    c.dir_count = 2;
    c.seed_dir = m.seed;
    c.bdf = bdf;
    c.m3 = (uint32_t)m3;
    return c;
}

int main(int argc, char **argv)
{
    CHECK(argc == 5);
    Blob fat16 = slurp(argv[1]), fat32 = slurp(argv[2]);
    snprintf(staging, sizeof staging, "%s", argv[3]);
    m.seed = argv[4];
    const uint8_t *data;
    uint32_t size;

    /* 1. modem_a FAT16 (4 KiB sectors) and a mounted seed without the /odm files: the partition comes first, and answers for these two. */
    m.modem_a = &fat16;
    m.seed_mounted = 1;
    PwFwConfig c = config(1, NULL);
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/amss20.bin");
    const PwFwFile *f = pw_fw_loaded(&fw, "amss.bin");
    CHECK(f && f->source == PwFwSrcFat && !strcmp(f->where, "modem_a:image/peach/amss20.bin"));
    /* the stack's own path forms ("QCC2072/hw1.0/amss.bin") come to the same cached file, with no new disk read */
    const unsigned reads = m.reads;
    const uint8_t *again;
    CHECK(pw_fw_get(&fw, "QCC2072/hw1.0/amss.bin", &again, &size) == PwFwOk && again == data && m.reads == reads);
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/bdwlan.elf");
    CHECK(pw_fw_get(&fw, "m3.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/phy_ucode20.elf");
    CHECK(pw_fw_get(&fw, "aux_ucode.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/aux_ucode20.elf");
    /* tmel_peach_20.elf has no peach/ prefix: found in image/ */
    CHECK(pw_fw_get(&fw, "tmel_peach_20.elf", &data, &size) == PwFwOk);
    same(data, size, "tmel_peach_20.elf");
    CHECK(!strcmp(pw_fw_loaded(&fw, "tmel_peach_20.elf")->where, "modem_a:image/tmel_peach_20.elf"));
    /* no regdb in this seed: the partition's */
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/regdb.bin");
    CHECK(!strcmp(pw_fw_loaded(&fw, "regdb.bin")->where, "modem_a:image/peach/regdb.bin"));
    /* the QDSS configuration: the partition's v2 before the seed's perf_v2 (partition first, then the candidates' order) */
    CHECK(pw_fw_get(&fw, "qdss_trace_config_v2.cfg", &data, &size) == PwFwOk);
    same(data, size, "peach/qdss_trace_config_v2.cfg");
    /* join.txt only from the seed, though the partition has one */
    CHECK(pw_fw_get(&fw, "join.txt", &data, &size) == PwFwOk);
    same(data, size, "seed-join.txt");
    CHECK(pw_fw_loaded(&fw, "join.txt")->source == PwFwSrcSeed);
    /* what the stack asks for first and must not find */
    const unsigned before = m.reads, lookups = m.seed_lookups;
    CHECK(pw_fw_get(&fw, "firmware-2.bin", &data, &size) == PwFwAbsent);
    CHECK(pw_fw_get(&fw, "board-2.bin", &data, &size) == PwFwAbsent);
    CHECK(pw_fw_get(&fw, "caldata.bin", &data, &size) == PwFwAbsent && m.reads == before && m.seed_lookups == lookups);
    CHECK(pw_fw_get(&fw, "", &data, &size) == PwFwArgument && pw_fw_get(&fw, NULL, &data, &size) == PwFwArgument);

    /* 2. the null-M3 mode and a BDF override */
    c = config(0, "bdwlan.b0e");
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "m3.bin", &data, &size) == PwFwAbsent);
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/bdwlan.b0e");
    const char *names[4];
    int seed_only;
    CHECK(pw_fw_candidates(&fw, "amss.bin", names, &seed_only) == 2 && !strcmp(names[0], "peach/amss20.bin") &&
          !strcmp(names[1], "amss20.bin") && !seed_only);
    CHECK(pw_fw_candidates(&fw, "join.txt", names, &seed_only) == 1 && seed_only);

    /* 2b. a seed with Android's /odm files: those two come from it, everything else from the partition as before */
    char odm[600];
    snprintf(odm, sizeof odm, "%s/odm", m.seed);
    c = config(1, NULL);
    c.seed_dir = odm;
    pw_fw_init(&fw, &io, &c);
    m.reads = 0;
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwOk);
    same(data, size, "seed-bdwlan.elf");
    CHECK(pw_fw_loaded(&fw, "board.bin")->source == PwFwSrcSeed && m.reads == 0);   /* the partition was not even mounted */
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk);
    same(data, size, "seed-regdb.bin");
    CHECK(pw_fw_loaded(&fw, "regdb.bin")->source == PwFwSrcSeed);
    CHECK(strstr(pw_fw_loaded(&fw, "regdb.bin")->where, "/odm/peach/regdb.bin"));
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwOk && pw_fw_loaded(&fw, "amss.bin")->source == PwFwSrcFat);
    CHECK(pw_fw_get(&fw, "aux_ucode.bin", &data, &size) == PwFwOk && pw_fw_loaded(&fw, "aux_ucode.bin")->source == PwFwSrcFat);
    /* the override of the BDF's name holds in the seed too: it has no such file, so the partition's */
    c = config(1, "bdwlan.b0e");
    c.seed_dir = odm;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/bdwlan.b0e");
    /* before the Data volume: "not yet", though the partition has them, and nothing is cached */
    m.seed_mounted = 0;
    c = config(1, NULL);
    c.seed_dir = odm;
    pw_fw_init(&fw, &io, &c);
    m.reads = 0;
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwNotYet && !pw_fw_loaded(&fw, "board.bin") && m.reads == 0);
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwNotYet && fw.last_error == PwFwNotYet);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwOk);        /* the others do not wait for it */
    /* it comes: the seed's */
    m.seed_mounted = 1;
    CHECK(pw_fw_get(&fw, "board.bin", &data, &size) == PwFwOk);
    same(data, size, "seed-bdwlan.elf");
    /* it never comes: after pw_fw_seed_gone the partition's copies, and later requests stay with what they got */
    m.seed_mounted = 0;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwNotYet);
    pw_fw_seed_gone(&fw);
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/regdb.bin");
    CHECK(pw_fw_loaded(&fw, "regdb.bin")->source == PwFwSrcFat);
    m.seed_mounted = 1;
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk && pw_fw_loaded(&fw, "regdb.bin")->source == PwFwSrcFat);
    /* a seed file that cannot be read does not cost the partition's copy */
    pw_fw_init(&fw, &io, &c);
    m.seed_fail = 1;
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk);
    same(data, size, "peach/regdb.bin");
    m.seed_fail = 0;
    /* the volume gone and no partition: nothing to give, and it says what is wrong */
    m.seed_mounted = 0;
    const Blob *keep = m.modem_a;
    m.modem_a = NULL;
    pw_fw_init(&fw, &io, &c);
    pw_fw_seed_gone(&fw);
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwNotYet);
    m.modem_a = keep;
    m.seed_mounted = 1;
    /* a failure of pw_fw_seed_gone's argument changes nothing */
    pw_fw_seed_gone(NULL);

    /* 3. the seed before the Data volume is mounted: "not yet", nothing cached; once mounted the file is found */
    m.seed_mounted = 0;
    c = config(1, NULL);
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "join.txt", &data, &size) == PwFwNotYet && !pw_fw_loaded(&fw, "join.txt"));
    m.seed_mounted = 1;
    CHECK(pw_fw_get(&fw, "join.txt", &data, &size) == PwFwOk);
    /* a seed file over 32 MiB is refused */
    m.modem_a = NULL;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwAbsent);   /* no partition, nothing in the seed */
    CHECK(fw.mount_rc == PwFwNoPartition);

    /* 4. modem_a absent: modem_b (FAT32, 512-byte sectors), its amss20.bin in image/ */
    m.modem_b = &fat32;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwOk && !strcmp(fw.partition, "modem_b"));
    same(data, size, "fat32-amss20.bin");
    CHECK(!strcmp(pw_fw_loaded(&fw, "amss.bin")->where, "modem_b:image/amss20.bin"));
    /* the partition has no regdb: the seed's */
    c.seed_dir = odm;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "regdb.bin", &data, &size) == PwFwOk && pw_fw_loaded(&fw, "regdb.bin")->source == PwFwSrcSeed);
    same(data, size, "seed-regdb.bin");
    c.seed_dir = m.seed;
    m.modem_b = NULL;

    /* 5. a partition that holds no FAT volume: the seed still answers; a failing disk is an I/O error and nothing is cached */
    Blob zero = {"zero", calloc(1, 1 << 20), 1 << 20};
    m.modem_a = &zero;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "qdss_trace_config_v2.cfg", &data, &size) == PwFwOk && fw.mount_rc == PwFwNotFat);
    same(data, size, "seed-qdss_trace_config_perf_v2.cfg");
    m.modem_a = &fat16;
    m.fail_reads = 1;
    pw_fw_init(&fw, &io, &c);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwIoError && !pw_fw_loaded(&fw, "amss.bin"));
    m.fail_reads = 0;
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwOk);   /* an I/O failure is retried at the next request */
    same(data, size, "peach/amss20.bin");

    /* 6. no partition configured and no seed: absent; the seed's big file is refused */
    PwFwConfig none = config(1, NULL);
    none.partition_count = 0;
    none.seed_dir = "";
    pw_fw_init(&fw, &io, &none);
    CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwAbsent);
    {
        /* big.bin through the join.txt alias would need a name; use the candidates of a request mapped to it: the seed directory
         * itself holds peach/amss20.bin as a 33 MiB file in the "big" subdirectory */
        char bigdir[600];
        snprintf(bigdir, sizeof bigdir, "%s/big", m.seed);
        PwFwConfig big = config(1, NULL);
        big.partition_count = 0;
        big.seed_dir = bigdir;
        pw_fw_init(&fw, &io, &big);
        CHECK(pw_fw_get(&fw, "amss.bin", &data, &size) == PwFwTooBig);
    }
    /* every byte allocated for a failed read was given back; the cached ones stay */
    CHECK(m.freed <= m.allocated);
    free(fat16.data); free(fat32.data); free(zero.data);
    printf("PASS firmware sources: %u checks\n", checks);
    return 0;
}
