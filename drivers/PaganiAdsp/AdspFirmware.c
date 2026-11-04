/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The partition reader follows UniXNU-sheng package/AdspFirmware.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e); the
 * .mdt/.bNN layout is described in AdspFirmware.h. */
#include "AdspFirmware.h"
#include "AdspUtil.h"

static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}

/* ---- the partition, through the one-block cache ---- */
int pagani_fw_partition_read(void *context, uint64_t offset, void *to, size_t length)
{
    PaganiFwVolume *v = (PaganiFwVolume *)context;
    uint8_t *out = (uint8_t *)to;
    if (!v || !to || !fits(offset, length, v->blocks * PAGANI_FW_BLOCK)) return -1;
    while (length) {
        const uint64_t lba = offset / PAGANI_FW_BLOCK;
        const size_t within = (size_t)(offset % PAGANI_FW_BLOCK);
        if (!within && length >= PAGANI_FW_BLOCK) {
            /* Whole blocks go straight to the caller. */
            uint64_t n = length / PAGANI_FW_BLOCK;
            if (n > PAGANI_FW_RUN_BLOCKS) n = PAGANI_FW_RUN_BLOCKS;
            if (!v->disk.read(v->disk.context, v->lun, v->first + lba, (uint32_t)n, out)) {v->disk_failed = 1; return -1;}
            v->reads++;
            v->read_blocks += n;
            out += n * PAGANI_FW_BLOCK;
            offset += n * PAGANI_FW_BLOCK;
            length -= (size_t)(n * PAGANI_FW_BLOCK);
            continue;
        }
        if (v->cached != lba) {
            v->cached = UINT64_MAX;
            if (!v->disk.read(v->disk.context, v->lun, v->first + lba, 1, v->block)) {v->disk_failed = 1; return -1;}
            v->reads++;
            v->read_blocks++;
            v->cached = lba;
        }
        const size_t take = PAGANI_FW_BLOCK - within < length ? PAGANI_FW_BLOCK - within : length;
        pagani_copy(out, v->block + within, take);
        out += take;
        offset += take;
        length -= take;
    }
    return 0;
}

int pagani_fw_mount(PaganiFwVolume *v, const PaganiFwDisk *disk, const char *const *names, unsigned count)
{
    if (!v || !disk || !disk->find || !disk->read || !disk->alloc || !disk->free || !names || !count) return PaganiFwArgument;
    pagani_zero(v, sizeof *v);
    v->disk = *disk;
    v->cached = UINT64_MAX;
    int found = 0;
    for (unsigned i = 0; i < count && !found; ++i) {
        if (!names[i]) continue;
        uint32_t lun = 0;
        uint64_t first = 0, blocks = 0;
        if (!disk->find(disk->context, names[i], &lun, &first, &blocks)) continue;
        found = 1;
        v->lun = lun;
        v->first = first;
        v->blocks = blocks;
        pagani_strcpy(v->partition, sizeof v->partition, names[i]);
    }
    if (!found) return PaganiFwNoPartition;
    if (!v->blocks || v->blocks > UINT64_MAX / PAGANI_FW_BLOCK) return PaganiFwNotFat;
    const int rc = fat_mount(&v->fat, v->blocks * PAGANI_FW_BLOCK, pagani_fw_partition_read, v);
    if (rc == FatReadError || v->disk_failed) return PaganiFwIo;
    return rc ? PaganiFwNotFat : PaganiFwOk;
}

/* ---- the image ---- */
static int headers_read(void *context, uint64_t offset, void *to, size_t length)
{
    const PaganiFwImage *image = (const PaganiFwImage *)context;
    if (!fits(offset, length, image->headers_bytes)) return -1;
    pagani_copy(to, image->headers + offset, length);
    return 0;
}

/* DIR/NAME.SUFFIX into image->path. */
static void make_path(PaganiFwImage *image, const char *directory, const char *suffix)
{
    PaganiText t;
    pagani_text_start(&t, image->path, sizeof image->path);
    if (directory && directory[0]) {
        pagani_text_str(&t, directory);
        pagani_text_char(&t, '/');
    }
    pagani_text_str(&t, image->name);
    pagani_text_char(&t, '.');
    pagani_text_str(&t, suffix);
}
static void piece_suffix(char out[4], unsigned index)
{
    out[0] = 'b';
    out[1] = (char)('0' + index / 10 % 10);
    out[2] = (char)('0' + index % 10);
    out[3] = 0;
}
/* A Fat code as the image error: a refused disk read is an I/O error, anything else a damaged volume (also reported as I/O). */
static int fat_error(PaganiFwImage *image, int rc)
{
    image->detail = rc;
    return rc == FatNotFound ? PaganiFwNoFile : PaganiFwIo;
}
static int segment_in_headers(const PaganiFwImage *image, const QcomMdtSegment *s)
{
    return fits(s->file_offset, s->file_size, image->headers_bytes);
}
static int hash_in_headers(const PaganiFwImage *image)
{
    const QcomMdtSegment *hash = &image->elf.segment[image->elf.hash];
    return image->elf.header_size + hash->file_size == image->headers_bytes || segment_in_headers(image, hash);
}

int pagani_fw_open(PaganiFwImage *image, PaganiFwVolume *v, const char *directory, const char *name)
{
    if (!image || !v || !name || !name[0] || pagani_strlen(name, sizeof image->name) >= sizeof image->name) return PaganiFwArgument;
    pagani_zero(image, sizeof *image);
    image->volume = v;
    image->detail = -1;
    pagani_strcpy(image->name, sizeof image->name, name);
    FatFile headers;
    make_path(image, directory, "mdt");
    int rc = fat_open(&v->fat, image->path, &headers);
    if (rc == FatNotFound) {
        image->from_b00 = 1;
        make_path(image, directory, "b00");
        rc = fat_open(&v->fat, image->path, &headers);
    }
    if (rc) return fat_error(image, rc);
    if (headers.directory) return fat_error(image, FatNotFound);
    if (headers.size < 52 || headers.size > PAGANI_FW_HEADERS_MAX) {image->detail = (int)headers.size; return PaganiFwSize;}
    image->headers = (uint8_t *)v->disk.alloc(v->disk.context, headers.size);
    if (!image->headers) return PaganiFwNoMemory;
    image->headers_bytes = headers.size;
    if ((rc = fat_read(&v->fat, &headers, 0, image->headers, headers.size))) return fat_error(image, rc);
    if ((rc = qcom_mdt_parse(&image->elf, headers.size, headers_read, image))) {image->detail = rc; return PaganiFwBadImage;}
    const int hash_outside = !hash_in_headers(image);
    for (unsigned i = 0; i < image->elf.count; ++i) {
        const QcomMdtSegment *s = &image->elf.segment[i];
        const int load = s->load && s->file_size && !segment_in_headers(image, s);
        if (s->load) {
            image->loadable++;
            image->file_bytes += s->file_size;
        }
        if (!load && !(hash_outside && (int)i == image->elf.hash)) continue;
        char suffix[4];
        piece_suffix(suffix, i);
        make_path(image, directory, suffix);
        if ((rc = fat_open(&v->fat, image->path, &image->piece[i]))) {
            fat_error(image, rc);
            image->detail = (int)i;
            return rc == FatNotFound ? PaganiFwNoFile : PaganiFwIo;
        }
        /* The Linux loader refuses a segment file of any other size ("truncated file"). */
        if (image->piece[i].directory || image->piece[i].size != s->file_size) {image->detail = (int)i; return PaganiFwSize;}
        image->has_piece[i] = 1;
        image->pieces++;
    }
    if (!image->loadable) {image->detail = QcomMdtNoSegments; return PaganiFwBadImage;}
    return PaganiFwOk;
}

void pagani_fw_close(PaganiFwImage *image)
{
    if (!image || !image->volume) return;
    if (image->headers) image->volume->disk.free(image->volume->disk.context, image->headers, image->headers_bytes);
    image->headers = 0;
    image->headers_bytes = 0;
}

uint64_t pagani_fw_metadata_size(const PaganiFwImage *image)
{
    return image && image->headers ? qcom_mdt_metadata_size(&image->elf) : 0;
}

int pagani_fw_metadata(PaganiFwImage *image, uint8_t *to, uint64_t capacity)
{
    if (!image || !image->headers || !to) return PaganiFwArgument;
    const uint64_t bytes = qcom_mdt_metadata_size(&image->elf);
    if (capacity < bytes) return PaganiFwArgument;
    if (hash_in_headers(image)) {
        const int rc = qcom_mdt_metadata(&image->elf, headers_read, image, to, capacity);
        if (rc) image->detail = rc;
        return rc ? PaganiFwBadImage : PaganiFwOk;
    }
    const int hash = image->elf.hash;
    if (!image->has_piece[hash]) {image->detail = hash; return PaganiFwNoFile;}
    pagani_copy(to, image->headers, (size_t)image->elf.header_size);
    const int rc = fat_read(&image->volume->fat, &image->piece[hash], 0, to + image->elf.header_size,
                            (size_t)image->elf.segment[hash].file_size);
    if (rc) {image->detail = rc; return PaganiFwIo;}
    return PaganiFwOk;
}

uint64_t pagani_fw_span(const PaganiFwImage *image)
{
    return image && image->headers ? qcom_mdt_span(&image->elf) : 0;
}

int pagani_fw_check(const PaganiFwImage *image, uint64_t region_base, uint64_t region_size)
{
    if (!image || !image->headers) return PaganiFwArgument;
    for (unsigned i = 0; i < image->elf.count; ++i) {
        uint64_t at = 0;
        if (!image->elf.segment[i].load) continue;
        if (qcom_mdt_place(&image->elf, i, region_base, region_size, &at)) return PaganiFwOutside;
    }
    if (image->elf.relocatable && pagani_fw_span(image) > region_size) return PaganiFwOutside;
    return PaganiFwOk;
}

/* Zeroes device-like memory with aligned 64-bit stores where it can. */
static void zero_region(uint8_t *to, uint64_t n)
{
    while (n && ((uintptr_t)to & 7)) {*to++ = 0; --n;}
    volatile uint64_t *w = (volatile uint64_t *)(void *)to;
    for (; n >= 8; n -= 8) *w++ = 0;
    to = (uint8_t *)(uintptr_t)w;
    while (n--) *to++ = 0;
}

int pagani_fw_load(PaganiFwImage *image, uint8_t *region, uint64_t region_base, uint64_t region_size, uint64_t *written)
{
    uint64_t done = 0;
    if (written) *written = 0;
    if (!image || !image->headers || !region) return PaganiFwArgument;
    for (unsigned i = 0; i < image->elf.count; ++i) {
        const QcomMdtSegment *s = &image->elf.segment[i];
        uint64_t at = 0;
        if (!s->load) continue;
        if (qcom_mdt_place(&image->elf, i, region_base, region_size, &at)) {image->detail = (int)i; return PaganiFwOutside;}
        if (s->file_size) {
            if (segment_in_headers(image, s)) {
                pagani_copy(region + at, image->headers + s->file_offset, (size_t)s->file_size);
            } else {
                if (!image->has_piece[i]) {image->detail = (int)i; return PaganiFwNoFile;}
                const int rc = fat_read(&image->volume->fat, &image->piece[i], 0, region + at, (size_t)s->file_size);
                if (rc) {image->detail = (int)i; return PaganiFwIo;}
            }
            done += s->file_size;
        }
        if (s->memory_size > s->file_size) zero_region(region + at + s->file_size, s->memory_size - s->file_size);
    }
    if (written) *written = done;
    return PaganiFwOk;
}
