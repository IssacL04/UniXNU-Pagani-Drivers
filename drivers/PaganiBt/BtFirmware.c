/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * BtFirmware.h 说明了来源。 */
#include "BtFirmware.h"
#include "BtUtil.h"
#include "PaganiBtBoard.h"

typedef struct {const char *name; uint8_t modem;} PbFwEntry;
static const PbFwEntry kFiles[PbFwCount] = {
    {PB_FW_DIR PB_FW_PERI, 0}, {PB_FW_DIR PB_FW_PERI_NVM, 0}, {PB_FW_DIR PB_FW_TMEL, 1}, {PB_FW_DIR PB_FW_BT, 0}, {PB_FW_DIR PB_FW_BT_NVM, 0},
};
static const char *const kPartitions[2][2] = {{"bluetooth_a", "bluetooth_b"}, {"modem_a", "modem_b"}};

static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}

static int partition_read(void *context, uint64_t offset, void *to, size_t length)
{
    PbFw *fw = (PbFw *)context;
    uint8_t *out = (uint8_t *)to;
    if (!fw || !to || !fits(offset, length, fw->blocks * PB_FW_BLOCK)) return -1;
    while (length) {
        const uint64_t lba = offset / PB_FW_BLOCK;
        const size_t within = (size_t)(offset % PB_FW_BLOCK);
        if (!within && length >= PB_FW_BLOCK) {
            uint64_t n = length / PB_FW_BLOCK;
            if (n > PB_FW_RUN_BLOCKS) n = PB_FW_RUN_BLOCKS;
            if (!fw->io.read(fw->io.context, fw->lun, fw->first + lba, (uint32_t)n, out)) return -1;
            fw->disk_reads++;
            out += n * PB_FW_BLOCK;
            offset += n * PB_FW_BLOCK;
            length -= (size_t)(n * PB_FW_BLOCK);
            continue;
        }
        if (fw->cached != lba) {
            fw->cached = UINT64_MAX;
            if (!fw->io.read(fw->io.context, fw->lun, fw->first + lba, 1, fw->block)) return -1;
            fw->disk_reads++;
            fw->cached = lba;
        }
        const size_t take = PB_FW_BLOCK - within < length ? PB_FW_BLOCK - within : length;
        pb_copy(out, fw->block + within, take);
        out += take;
        offset += take;
        length -= take;
    }
    return 0;
}

void pb_fw_init(PbFw *fw, const PbFwIo *io)
{
    pb_zero(fw, sizeof *fw);
    if (io) fw->io = *io;
    fw->cached = UINT64_MAX;
}

uint32_t pb_fw_hash(const uint8_t *data, uint32_t bytes)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

/* 在这组分区里打开文件。分区不存在、不是 FAT、没有这个文件都接着试下一个，都不行时给最靠后的错误。 */
static int open_file(PbFw *fw, const PbFwEntry *entry, FatFile *file, const char **where)
{
    int rc = PbFwNoPartition;
    for (unsigned i = 0; i < 2; ++i) {
        const char *name = kPartitions[entry->modem][i];
        uint32_t lun = 0;
        uint64_t first = 0, blocks = 0;
        if (!fw->io.find(fw->io.context, name, &lun, &first, &blocks)) continue;
        fw->lun = lun;
        fw->first = first;
        fw->blocks = blocks;
        fw->cached = UINT64_MAX;
        const int mounted = fat_mount(&fw->fat, blocks * PB_FW_BLOCK, partition_read, fw);
        if (mounted) {
            rc = mounted == FatReadError ? PbFwIoError : PbFwNotFat;
            continue;
        }
        const int opened = fat_open(&fw->fat, entry->name, file);
        if (opened == FatOk) {
            *where = name;
            return PbFwOk;
        }
        rc = opened == FatReadError || opened == FatDamaged ? PbFwIoError : PbFwAbsent;
    }
    return rc;
}

int pb_fw_load(PbFw *fw, unsigned *failed)
{
    if (!fw || !fw->io.find || !fw->io.read || !fw->io.alloc) return PbFwArgument;
    for (unsigned i = 0; i < PbFwCount; ++i) {
        PbFwFile *f = &fw->files[i];
        if (f->data) continue;
        if (failed) *failed = i;
        FatFile file;
        const char *where = NULL;
        int rc = open_file(fw, &kFiles[i], &file, &where);
        if (rc) return rc;
        if (file.directory || !file.size) return PbFwAbsent;
        if (file.size > PB_FW_MAX_BYTES) return PbFwTooBig;
        uint8_t *data = (uint8_t *)fw->io.alloc(fw->io.context, file.size);
        if (!data) return PbFwNoMemory;
        if (fat_read(&fw->fat, &file, 0, data, file.size)) {
            if (fw->io.free) fw->io.free(fw->io.context, data, file.size);
            return PbFwIoError;
        }
        f->data = data;
        f->size = f->allocated = file.size;
        f->hash = pb_fw_hash(data, file.size);
        f->partition = where;
    }
    return PbFwOk;
}

void pb_fw_release(PbFw *fw)
{
    for (unsigned i = 0; i < PbFwCount; ++i) {
        PbFwFile *f = &fw->files[i];
        if (f->data && fw->io.free) fw->io.free(fw->io.context, f->data, f->allocated);
        f->data = NULL;
        f->size = f->allocated = 0;
    }
}
