/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 分区读取（NativeUfs 4 KiB 块上的单块缓存）沿用 PaganiWifi WifiFirmware.c，它又沿用 PaganiAdsp AdspFirmware.c。 */
/* 蓝牙的五个固件文件，从手机自己的分区原地读，绝不进启动镜像，也绝不写回：
 *   bluetooth_a（再试 bluetooth_b）的 image/ 下四个，modem_a（再试 modem_b）的 image/ 下一个 tmel_peach_20.elf。
 * NVM 读进可写内存，改地址和波特率只改这份内存（BtDownload.c）。 */
#ifndef PAGANI_BT_FIRMWARE_H
#define PAGANI_BT_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#include "BtFat.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {PbFwOk = 0, PbFwNoPartition = -1, PbFwNotFat = -2, PbFwAbsent = -3, PbFwIoError = -5, PbFwTooBig = -6, PbFwArgument = -8, PbFwNoMemory = -9};
enum {PbFwPeri, PbFwPeriNvm, PbFwTmel, PbFwBt, PbFwBtNvm, PbFwCount};
#define PB_FW_BLOCK 4096u
#define PB_FW_RUN_BLOCKS 256u           /* 每次磁盘读最多 1 MiB */
#define PB_FW_MAX_BYTES (1u << 20)

typedef struct {
    void *context;
    /* NativeUfs：找到分区返回 1（LUN、首块、块数，都是 4 KiB 块），否则 0。 */
    int (*find)(void *context, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
    /* 从 lba 读 blocks 个 4 KiB 块，成功返回 1。 */
    int (*read)(void *context, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
    void *(*alloc)(void *context, size_t bytes);
    void (*free)(void *context, void *memory, size_t bytes);
} PbFwIo;

typedef struct {
    uint8_t *data;
    uint32_t size, allocated, hash;     /* hash：FNV-1a 32，日志里和主机算的比 */
    const char *partition;
} PbFwFile;

typedef struct {
    PbFwIo io;
    uint32_t lun;
    uint64_t first, blocks, cached;
    uint8_t block[PB_FW_BLOCK];
    FatVolume fat;
    PbFwFile files[PbFwCount];
    uint32_t disk_reads;
} PbFw;

void pb_fw_init(PbFw *fw, const PbFwIo *io);
/* 把五个文件都读进内存。成功返回 0；失败返回负数，*failed 是失败文件的序号，已经读到的留着，再调一次从缺的继续：分区可能还没就绪。 */
int pb_fw_load(PbFw *fw, unsigned *failed);
void pb_fw_release(PbFw *fw);
uint32_t pb_fw_hash(const uint8_t *data, uint32_t bytes);

#ifdef __cplusplus
}
#endif
#endif
