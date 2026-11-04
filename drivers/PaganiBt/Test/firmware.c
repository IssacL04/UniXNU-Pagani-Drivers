/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtFirmware.c 对 mtools 做的 FAT 卷（4 KiB 扇区，手机分区就是这样）：五个文件逐字节相等，缺分区、缺文件、磁盘错误，分区后到时再调一次接着读。
 * 用法：firmware <bluetooth 卷> <modem 卷> <文件目录> */
#include "../BtFirmware.h"
#include "../PaganiBtBoard.h"
#include "check.h"

typedef struct {const char *name; const uint8_t *data; uint64_t size;} Disk;
static Disk disks[4];
static unsigned disk_count;
static int read_errors_after = -1, reads, allocs, frees;

static uint8_t *load(const char *path, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s missing\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (uint64_t)n;
    return data;
}
static int disk_find(void *c, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    (void)c;
    for (unsigned i = 0; i < disk_count; ++i) {
        if (strcmp(disks[i].name, name)) continue;
        *lun = i;
        *first = 3;                                    /* 分区不从 0 开始 */
        *blocks = disks[i].size / PB_FW_BLOCK;
        return 1;
    }
    return 0;
}
static int disk_read(void *c, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
    (void)c;
    ++reads;
    if (read_errors_after == 0) return 0;
    if (read_errors_after > 0) --read_errors_after;
    CHECK(lun < disk_count && lba >= 3);
    const uint64_t at = (lba - 3) * PB_FW_BLOCK, bytes = (uint64_t)blocks * PB_FW_BLOCK;
    CHECK(at + bytes <= disks[lun].size);
    memcpy(dst, disks[lun].data + at, (size_t)bytes);
    return 1;
}
static void *fw_alloc(void *c, size_t bytes) {(void)c; ++allocs; return malloc(bytes);}
static void fw_free(void *c, void *memory, size_t bytes) {(void)c; (void)bytes; ++frees; free(memory);}
static const PbFwIo kIo = {NULL, disk_find, disk_read, fw_alloc, fw_free};

static PbFw fw;
static const char *const kNames[PbFwCount] = {PB_FW_PERI, PB_FW_PERI_NVM, PB_FW_TMEL, PB_FW_BT, PB_FW_BT_NVM};

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    uint64_t bt_size, modem_size;
    const uint8_t *bt = load(argv[1], &bt_size), *modem = load(argv[2], &modem_size);
    uint8_t *want[PbFwCount];
    uint64_t want_size[PbFwCount];
    for (unsigned i = 0; i < PbFwCount; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[3], kNames[i]);
        want[i] = load(path, &want_size[i]);
    }

    /* 分区都还没有：报 PbFwNoPartition，一次都不读 */
    unsigned failed = 99;
    pb_fw_init(&fw, &kIo);
    CHECK(pb_fw_load(&fw, &failed) == PbFwNoPartition && failed == 0 && reads == 0);

    /* bluetooth_a 先到：读到 modem 那个文件停下，已读的留着；modem_a 到了再调一次接着读 */
    disks[0] = (Disk){"bluetooth_a", bt, bt_size};
    disk_count = 1;
    CHECK(pb_fw_load(&fw, &failed) == PbFwNoPartition && failed == PbFwTmel);
    CHECK(fw.files[PbFwPeri].data && fw.files[PbFwPeriNvm].data && !fw.files[PbFwBt].data);
    const int before = allocs;
    disks[1] = (Disk){"modem_a", modem, modem_size};
    disk_count = 2;
    CHECK(pb_fw_load(&fw, &failed) == PbFwOk);
    CHECK(allocs == before + 3);
    for (unsigned i = 0; i < PbFwCount; ++i) {
        CHECK(fw.files[i].size == want_size[i]);
        CHECK(!memcmp(fw.files[i].data, want[i], (size_t)want_size[i]));
        CHECK(fw.files[i].hash == pb_fw_hash(want[i], (uint32_t)want_size[i]));
    }
    CHECK(!strcmp(fw.files[PbFwTmel].partition, "modem_a") && !strcmp(fw.files[PbFwBt].partition, "bluetooth_a"));
    pb_fw_release(&fw);
    CHECK(frees == allocs && !fw.files[0].data);

    /* b 槽回退：只有 _b */
    disks[0].name = "bluetooth_b";
    disks[1].name = "modem_b";
    pb_fw_init(&fw, &kIo);
    CHECK(pb_fw_load(&fw, &failed) == PbFwOk);
    CHECK(!strcmp(fw.files[PbFwPeri].partition, "bluetooth_b"));
    pb_fw_release(&fw);

    /* 卷里没有这个文件：bluetooth 卷里找 tmel 就是没有 */
    disks[0].name = "bluetooth_a";
    disks[1] = (Disk){"modem_a", bt, bt_size};
    pb_fw_init(&fw, &kIo);
    CHECK(pb_fw_load(&fw, &failed) == PbFwAbsent && failed == PbFwTmel);
    pb_fw_release(&fw);

    /* 不是 FAT */
    static uint8_t blank[1 << 20];
    disks[0].data = blank;
    disks[0].size = sizeof blank;
    pb_fw_init(&fw, &kIo);
    CHECK(pb_fw_load(&fw, &failed) == PbFwNotFat);
    disks[0].data = bt;
    disks[0].size = bt_size;

    /* 磁盘读错：不留半截文件，分配的内存还回去 */
    for (int after = 0; after < 30; ++after) {
        disks[1] = (Disk){"modem_a", modem, modem_size};
        pb_fw_init(&fw, &kIo);
        read_errors_after = after;
        const int rc = pb_fw_load(&fw, &failed);
        read_errors_after = -1;
        if (rc) {
            CHECK(rc == PbFwIoError);
            CHECK(!fw.files[failed].data);
        }
        pb_fw_release(&fw);
    }
    CHECK(frees == allocs);

    /* 参数 */
    PbFwIo no_alloc = kIo;
    no_alloc.alloc = NULL;
    pb_fw_init(&fw, &no_alloc);
    CHECK(pb_fw_load(&fw, &failed) == PbFwArgument);
    printf("PASS firmware: %u checks, %d disk reads\n", checks, reads);
    return 0;
}
