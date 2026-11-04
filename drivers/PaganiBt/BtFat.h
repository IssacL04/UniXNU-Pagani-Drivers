/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* FatVolume（原样取自 UniXNU-sheng package/FatVolume.c，commit 08cde20e，和 PaganiAdsp、PaganiWifi 带的是同一份）用本驱动的名字编译：
 * 这几个驱动的目标文件都进同一个强制加载的归档，这份编成 pagani_bt_fat_*。PaganiBt 里用 FAT 的地方都包含这个头。 */
#ifndef PAGANI_BT_FAT_H
#define PAGANI_BT_FAT_H
#define fat_mount pagani_bt_fat_mount
#define fat_open pagani_bt_fat_open
#define fat_read pagani_bt_fat_read
#include "FatVolume.h"
#endif
