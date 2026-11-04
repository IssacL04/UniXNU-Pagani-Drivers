/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * pw_fw_partition_read and pw_fw_mount follow PaganiAdsp AdspFirmware.c (pagani_fw_partition_read, pagani_fw_mount), which follows
 * UniXNU-sheng package/AdspFirmware.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). WifiFirmware.h describes the rest. */
#include "WifiFirmware.h"
#include "WifiUtil.h"

static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}

/* ---- the partition, through the one-block cache ---- */
int pw_fw_partition_read(void *context, uint64_t offset, void *to, size_t length)
{
    PwFw *fw = (PwFw *)context;
    uint8_t *out = (uint8_t *)to;
    if (!fw || !to || !fits(offset, length, fw->blocks * PW_FW_BLOCK)) return -1;
    while (length) {
        const uint64_t lba = offset / PW_FW_BLOCK;
        const size_t within = (size_t)(offset % PW_FW_BLOCK);
        if (!within && length >= PW_FW_BLOCK) {
            uint64_t n = length / PW_FW_BLOCK;
            if (n > PW_FW_RUN_BLOCKS) n = PW_FW_RUN_BLOCKS;
            if (!fw->io.read(fw->io.context, fw->lun, fw->first + lba, (uint32_t)n, out)) {fw->disk_failed = 1; return -1;}
            fw->disk_reads++;
            out += n * PW_FW_BLOCK;
            offset += n * PW_FW_BLOCK;
            length -= (size_t)(n * PW_FW_BLOCK);
            continue;
        }
        if (fw->cached != lba) {
            fw->cached = UINT64_MAX;
            if (!fw->io.read(fw->io.context, fw->lun, fw->first + lba, 1, fw->block)) {fw->disk_failed = 1; return -1;}
            fw->disk_reads++;
            fw->cached = lba;
        }
        const size_t take = PW_FW_BLOCK - within < length ? PW_FW_BLOCK - within : length;
        pw_copy(out, fw->block + within, take);
        out += take;
        offset += take;
        length -= take;
    }
    return 0;
}

void pw_fw_init(PwFw *fw, const PwFwIo *io, const PwFwConfig *config)
{
    pw_zero(fw, sizeof *fw);
    if (io) fw->io = *io;
    if (config) fw->config = *config;
    fw->mount_rc = 1;
    fw->cached = UINT64_MAX;
    fw->last_error = PwFwOk;
    pw_strcpy(fw->seed_dir, sizeof fw->seed_dir, config && config->seed_dir ? config->seed_dir : PW_FW_SEED_DIR);
    PwText t;
    pw_text_start(&t, fw->bdf, sizeof fw->bdf);
    pw_text_str(&t, "peach/");
    pw_text_str(&t, config && config->bdf && config->bdf[0] ? config->bdf : PW_FW_DEFAULT_BDF);
    if (fw->config.partition_count > 4) fw->config.partition_count = 4;
    if (fw->config.dir_count > 4) fw->config.dir_count = 4;
}

int pw_fw_mount(PwFw *fw)
{
    if (!fw || !fw->io.find || !fw->io.read) return PwFwArgument;
    if (fw->mount_rc == PwFwOk) return PwFwOk;
    if (fw->mount_rc == PwFwNotFat) return PwFwNotFat;   /* a volume that is not FAT stays so */
    int found = 0;
    for (unsigned i = 0; i < fw->config.partition_count && !found; ++i) {
        const char *name = fw->config.partitions[i];
        uint32_t lun = 0;
        uint64_t first = 0, blocks = 0;
        if (!name || !fw->io.find(fw->io.context, name, &lun, &first, &blocks)) continue;
        found = 1;
        fw->lun = lun;
        fw->first = first;
        fw->blocks = blocks;
        pw_strcpy(fw->partition, sizeof fw->partition, name);
    }
    if (!found) return fw->mount_rc = PwFwNoPartition;
    fw->cached = UINT64_MAX;
    fw->disk_failed = 0;
    if (!fw->blocks || fw->blocks > UINT64_MAX / PW_FW_BLOCK) return fw->mount_rc = PwFwNotFat;
    const int rc = fat_mount(&fw->fat, fw->blocks * PW_FW_BLOCK, pw_fw_partition_read, fw);
    if (rc == FatReadError || fw->disk_failed) return fw->mount_rc = PwFwIoError;
    return fw->mount_rc = rc ? PwFwNotFat : PwFwOk;
}

/* ---- names ---- */
typedef struct {const char *request; const char *names[3]; uint8_t seed_only;} Alias;
static const Alias kAliases[] = {
    {"amss.bin", {"peach/amss20.bin", "amss20.bin", 0}, 0},
    {"regdb.bin", {"peach/regdb.bin", 0, 0}, 0},
    {"aux_ucode.bin", {"peach/aux_ucode20.elf", 0, 0}, 0},
    {"tmel_peach_20.elf", {"tmel_peach_20.elf", "peach/tmel_peach_20.elf", 0}, 0},
    {"qdss_trace_config_v2.cfg", {"peach/qdss_trace_config_perf_v2.cfg", "peach/qdss_trace_config_v2.cfg", 0}, 0},
    {"join.txt", {"join.txt", 0, 0}, 1},
};
static const char kM3[] = "peach/phy_ucode20.elf";

static const char *basename_of(const char *path)
{
    const char *base = path;
    for (const char *c = path; c && *c; ++c) if (*c == '/') base = c + 1;
    return base;
}

unsigned pw_fw_candidates(const PwFw *fw, const char *request, const char *out[4], int *seed_only)
{
    unsigned n = 0;
    if (seed_only) *seed_only = 0;
    if (!fw || !request) return 0;
    const char *base = basename_of(request);
    if (pw_streq(base, "board.bin")) {
        out[n++] = fw->bdf;
        return n;
    }
    if (pw_streq(base, "m3.bin")) {
        if (fw->config.m3) out[n++] = kM3;
        return n;
    }
    for (unsigned i = 0; i < sizeof kAliases / sizeof kAliases[0]; ++i) {
        if (!pw_streq(base, kAliases[i].request)) continue;
        for (unsigned k = 0; k < 3 && kAliases[i].names[k]; ++k) out[n++] = kAliases[i].names[k];
        if (seed_only) *seed_only = kAliases[i].seed_only;
        return n;
    }
    return 0;
}

static PwFwFile *slot_of(PwFw *fw, const char *request)
{
    for (unsigned i = 0; i < fw->file_count; ++i) if (pw_streq(fw->files[i].request, request)) return &fw->files[i];
    return 0;
}

const PwFwFile *pw_fw_loaded(const PwFw *fw, const char *request)
{
    if (!fw || !request) return 0;
    const char *base = basename_of(request);
    for (unsigned i = 0; i < fw->file_count; ++i) if (pw_streq(fw->files[i].request, base)) return &fw->files[i];
    return 0;
}

/* Keeps bytes the caller allocated in a new slot. */
static int keep(PwFw *fw, const char *request, uint8_t *data, uint32_t size, uint32_t allocated, uint8_t source, const char *where)
{
    if (fw->file_count >= PW_FW_SLOTS) {
        fw->io.free(fw->io.context, data, allocated);
        return PwFwFull;
    }
    PwFwFile *f = &fw->files[fw->file_count++];
    pw_strcpy(f->request, sizeof f->request, request);
    f->data = data;
    f->size = size;
    f->allocated = allocated;
    f->source = source;
    pw_strcpy(f->where, sizeof f->where, where);
    return PwFwOk;
}

/* One candidate on the FAT volume: PwFwOk (kept), PwFwAbsent, or an error. */
static int from_fat(PwFw *fw, const char *request, const char *path)
{
    FatFile file;
    pw_strcpy(fw->last_path, sizeof fw->last_path, path);
    int rc = fat_open(&fw->fat, path, &file);
    if (rc == FatNotFound || rc == FatNotDirectory) return PwFwAbsent;
    if (rc) return PwFwIoError;
    if (file.directory) return PwFwAbsent;
    if (file.size > PW_FW_MAX_BYTES) return PwFwTooBig;
    const uint32_t allocated = file.size ? file.size : 1;
    uint8_t *data = (uint8_t *)fw->io.alloc(fw->io.context, allocated);
    if (!data) return PwFwNoMemory;
    if (file.size && fat_read(&fw->fat, &file, 0, data, file.size)) {
        fw->io.free(fw->io.context, data, allocated);
        return PwFwIoError;
    }
    char where[PW_FW_PATH];
    PwText t;
    pw_text_start(&t, where, sizeof where);
    pw_text_str(&t, fw->partition);
    pw_text_char(&t, ':');
    pw_text_str(&t, path);
    return keep(fw, request, data, file.size, allocated, PwFwSrcFat, where);
}

/* One path of the seed: PwFwOk (kept), PwFwAbsent, PwFwNotYet or an error. */
static int from_seed(PwFw *fw, const char *request, const char *path)
{
    uint64_t size = 0;
    pw_strcpy(fw->last_path, sizeof fw->last_path, path);
    const int rc = fw->io.file_size(fw->io.context, path, &size);
    if (rc) return rc == PwFwNotYet ? PwFwNotYet : rc == PwFwAbsent ? PwFwAbsent : PwFwIoError;
    if (size > PW_FW_MAX_BYTES) return PwFwTooBig;
    const uint32_t allocated = size ? (uint32_t)size : 1;
    uint8_t *data = (uint8_t *)fw->io.alloc(fw->io.context, allocated);
    if (!data) return PwFwNoMemory;
    if (size && (!fw->io.file_read || fw->io.file_read(fw->io.context, path, 0, data, (size_t)size))) {
        fw->io.free(fw->io.context, data, allocated);
        return PwFwIoError;
    }
    fw->seed_reads++;
    return keep(fw, request, data, (uint32_t)size, allocated, PwFwSrcSeed, path);
}

/* The worse of two outcomes for the report: an error beats "not yet", which beats "absent". */
static int worse(int a, int b)
{
    if (a == PwFwAbsent) return b;
    if (b == PwFwAbsent) return a;
    if (a == PwFwNotYet) return b;
    return a;
}

/* Every path of the candidates in the seed: PwFwOk (kept), or the worse of what they answered. */
static int seed_lookup(PwFw *fw, const char *base, const char *const *names, unsigned n)
{
    int result = PwFwAbsent;
    for (unsigned i = 0; i < n && result != PwFwOk; ++i) {
        const char *variants[2] = {names[i], basename_of(names[i])};
        for (unsigned v = 0; v < 2; ++v) {
            if (v == 1 && variants[1] == variants[0]) break;
            char path[PW_FW_PATH];
            PwText t;
            pw_text_start(&t, path, sizeof path);
            pw_text_str(&t, fw->seed_dir);
            pw_text_char(&t, '/');
            pw_text_str(&t, variants[v]);
            const int rc = from_seed(fw, base, path);
            if (rc == PwFwOk) {result = PwFwOk; break;}
            result = worse(result, rc);
        }
    }
    return result;
}

/* Android's /odm files: WifiFirmware.h says why these two look at the seed first. */
static int seed_preferred(const char *base) {return pw_streq(base, "board.bin") || pw_streq(base, "regdb.bin");}

void pw_fw_seed_gone(PwFw *fw) {if (fw) fw->seed_gone = 1;}

int pw_fw_get(PwFw *fw, const char *request, const uint8_t **data, uint32_t *size)
{
    if (!fw || !request || !data || !size || !fw->io.alloc || !fw->io.free) return PwFwArgument;
    const char *base = basename_of(request);
    if (!base[0] || pw_strlen(base, 32) >= 32) return PwFwArgument;
    fw->lookups++;
    PwFwFile *hit = slot_of(fw, base);
    if (hit) {
        *data = hit->data;
        *size = hit->size;
        return PwFwOk;
    }
    const char *names[4];
    int seed_only = 0;
    const unsigned n = pw_fw_candidates(fw, base, names, &seed_only);
    const int seed = n && fw->seed_dir[0] && fw->io.file_size;
    const int seed_first = seed && seed_preferred(base) && !fw->seed_gone;
    int result = PwFwAbsent;
    if (seed_first) {
        result = seed_lookup(fw, base, names, n);
        if (result == PwFwNotYet) {
            fw->misses++;
            fw->last_error = result;
            return result;
        }
    }
    if (result != PwFwOk && n && !seed_only && fw->config.partition_count && fw->io.find && fw->io.read) {
        int rc = pw_fw_mount(fw);
        if (rc == PwFwOk) {
            for (unsigned i = 0; i < n && result != PwFwOk; ++i) {
                for (unsigned d = 0; d < fw->config.dir_count; ++d) {
                    char path[PW_FW_PATH];
                    PwText t;
                    pw_text_start(&t, path, sizeof path);
                    if (fw->config.dirs[d] && fw->config.dirs[d][0]) {
                        pw_text_str(&t, fw->config.dirs[d]);
                        pw_text_char(&t, '/');
                    }
                    pw_text_str(&t, basename_of(names[i]));
                    rc = from_fat(fw, base, path);
                    if (rc == PwFwOk) {result = PwFwOk; break;}
                    result = worse(result, rc);
                }
            }
        } else if (rc != PwFwNoPartition) {
            result = worse(result, rc);
        }
    }
    if (result != PwFwOk && seed && !seed_first) {
        const int rc = seed_lookup(fw, base, names, n);
        result = rc == PwFwOk ? PwFwOk : worse(result, rc);
    }
    if (result != PwFwOk) {
        fw->misses++;
        fw->last_error = result;
        return result;
    }
    hit = slot_of(fw, base);
    if (!hit) return PwFwFull;
    *data = hit->data;
    *size = hit->size;
    return PwFwOk;
}
