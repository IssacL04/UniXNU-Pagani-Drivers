/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* FatVolume (ported unchanged from UniXNU-sheng package/FatVolume.c, commit 08cde20e, the same copy PaganiAdsp carries) under names of
 * this driver: PaganiAdsp's objects define fat_mount/fat_open/fat_read in the same force-loaded archive, so this driver's copy is built
 * as pagani_wifi_fat_* (WifiFat.c includes FatVolume.c after these renames). Every user of the FAT reader in PaganiWifi includes this
 * header instead of FatVolume.h. */
#ifndef PAGANI_WIFI_FAT_H
#define PAGANI_WIFI_FAT_H
#define fat_mount pagani_wifi_fat_mount
#define fat_open pagani_wifi_fat_open
#define fat_read pagani_wifi_fat_read
#include "FatVolume.h"
#endif
