// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 两个驱动投的是同一组轨道：资源名、PPID、电压、顺序必须和 PaganiWifiTables.h 一致，Wi-Fi 改了这里要跟着改，否则两边互相抵消。
// 引脚和窗口同理。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../PaganiWifi/PaganiWifiTables.h"
extern "C" {
#include "../PaganiBtBoard.h"
}

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)

int main()
{
    using namespace pagani_wifi;
    CHECK(sizeof kRails / sizeof kRails[0] == PB_RAIL_COUNT);
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        CHECK(!std::strcmp(kRails[i].resource, kPbRails[i].resource));
        CHECK(kRails[i].ppid == kPbRails[i].ppid);
        CHECK(kRails[i].chosenMv == kPbRails[i].chosenMv);
        CHECK(kRails[i].maxMv == kPbRails[i].maxMv);
        CHECK(kRails[i].order == kPbRails[i].order);
    }
    CHECK(kGcc == PB_GCC_BASE && kGccMapBytes == PB_GCC_BYTES);
    CHECK(kTlmmPins == PB_TLMM_BASE && kTlmmPinStride == PbPinStride);
    CHECK(kRscDrv2 == PB_RSC_DRV2 && kCmdDb == PB_CMD_DB && kCmdDbBytes == PB_CMD_DB_BYTES);
    CHECK(!std::strcmp(kRefClock, "RPMH_RF_CLK1"));
    for (const Pin &p : kPins) if (!std::strcmp(p.name, "wlan_en")) CHECK(p.gpio == PbPinWlanEn);
    for (const Pin &p : kPins) if (!std::strcmp(p.name, "sw_ctrl")) CHECK(p.gpio == PbPinSwCtrl);
    std::printf("PASS tables: %u checks\n", checks);
    return 0;
}
