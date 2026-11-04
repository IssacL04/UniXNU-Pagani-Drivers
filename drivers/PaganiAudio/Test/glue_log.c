// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The boot log for Test/glue.cpp: strong definitions of BootProgress's entry points, which replace the weak defaults of PaganiAudio.cpp at
// link time exactly as drivers/BootProgress RamoopsLog.c does in the kernel link (so this file must stay a translation unit of its own).
#include <stdio.h>

char kstub_lines[256][256];
unsigned kstub_line_count;

void boot_progress_trace(const char *text)
{
    if (kstub_line_count < 256) snprintf(kstub_lines[kstub_line_count], sizeof kstub_lines[0], "%s", text ? text : "");
    kstub_line_count++;
}

int boot_progress_trace_enabled(void) {return 1;}
