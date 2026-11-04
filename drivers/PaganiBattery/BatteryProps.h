/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What iOS reads of the battery: the IOPMPowerSource properties of a snapshot, built in one portable place that the kernel service
 * (PaganiBatteryService.cpp, which sets each of them) and the host checks (Test/props.c, and check-pagani-battery-host.py against the
 * system volume's powerd when it is at hand) share. The facts, from the 21H461 system volume (no Apple code is used):
 *
 *   powerd (System/Library/CoreServices/powerd.bundle/powerd), the battery's properties as it unpacks them (function at 0x10002e790):
 *   CurrentCapacity and MaxCapacity must both be there or the source is ignored; ExternalConnected, ExternalChargeCapable,
 *   BatteryInstalled, IsCharging, AppleRawExternalConnected, FullyCharged and AtCriticalLevel count only as kCFBooleanTrue; Voltage,
 *   DesignCapacity, TimeRemaining, InstantAmperage, Amperage, CycleCount numbers; AdapterDetails a dictionary (FamilyCode). The power
 *   source dictionary it publishes (0x100027ea4): "Power Source State" is "AC Power" exactly when ExternalConnected, "Raw External
 *   Connected" and "Show Charging UI" follow AppleRawExternalConnected, "Is Present" BatteryInstalled, "Is Charging" IsCharging (with a
 *   battery present), "Current Capacity" CurrentCapacity * 100 / MaxCapacity.
 *   SpringBoard's status bar (BatteryCenter.framework, -[BCPowerSourceController ...] at 0x1fca91dc0): the charge from "Current
 *   Capacity"/"Max Capacity"; the bolt is "Is Charging" ? "Power Source State" == "AC Power" : "Show Charging UI". So a phone held full
 *   by its charger (not charging) shows the bolt only through AppleRawExternalConnected.
 *   Settings > Battery > Battery Health (PreferenceBundles/BatteryUsageUI.bundle, standalone): "Maximum Capacity Percent", "Battery
 *   Service State" and "Battery Service Flags" of IOPSCopyPowerSourcesByType(1)[0] (0x320a4, 0x321e0, 0x32368), the capacity shown as
 *   clamp(value / 100, 0, 1) in percent style (0x15e20). powerd computes them in its battery health loop (0x10002f818):
 *   ceil(100 * NominalChargeCapacity / DesignCapacity) within 1..150 (104 while CycleCount <= 20 and nothing is stored yet), which needs
 *   NominalChargeCapacity, DesignCapacity >= 1 and CycleCount (0x10002b118; without CycleCount no figure, and the page shows 0 %).
 *
 * Not published, on purpose (each one changes what iOS concludes, and nothing true can be put there):
 *   Serial            powerd keys its health state on it; with a serial and no thermalmonitord UPO stepper on this device
 *                     (com.apple.system.batteryHealth.UPOAware never posted) the loop sets flag 0x80000 and state 5, which the page
 *                     shows as "unable to determine"; without a serial the state is -1, which the page shows as normal (0x100029618).
 *   BatteryData       read only for the gauge algorithm's inputs (WeightedRa, LifetimeData, AlgoChemID, Qmax, DateOfFirstUse:
 *                     0x10003311c, 0x1000315b8, 0x100035410), which steer calibration and service flags: no NFG8011B value is one.
 *   AppleAuthCPRelay  a child service whose isTrustedForUI claims an authenticated Apple battery (powerd 0x10001ac3c, BatteryUsageUI
 *                     genuineBatteryStatus). Battery module authentication is answered "not supported" instead, by the layout's
 *                     MobileGestalt fix-up (IN/battery/integrate): without it powerd waits for this service forever, skips the health
 *                     loop ("missing auth", Maximum Capacity Percent -1) and the page shows a spinner and 0 %.
 *   MaximumCapacityPercent is published for completeness (powerlog's PLBatteryAgent reads that name); 21H461 powerd computes its own
 *   figure from NominalChargeCapacity and DesignCapacity and reads this name only from powerlog's preferences. */
#ifndef PAGANI_BATTERY_PROPS_H
#define PAGANI_BATTERY_PROPS_H
#include <stdint.h>
#include "BatteryCore.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {PbpBool = 1, PbpNumber = 2, PbpString = 3, PbpAdapter = 4};

typedef struct {
    const char *key;
    uint32_t type;            /* Pbp* */
    int32_t value;            /* PbpBool 0/1; PbpNumber (a 32-bit OSNumber; negative values two's complement, as IOPMPowerSource's own) */
    const char *text;         /* PbpString */
} PbatProp;

/* AdapterDetails (PbpAdapter: the service builds the dictionary from the snapshot): FamilyCode always (0 without an adapter), Watts,
 * AdapterVoltage (mV) and Current (mA) with a known adapter. IOPM.h kIOPMPSAdapterDetails* names. */
#define PBAT_ADAPTER_FAMILY "FamilyCode"
#define PBAT_ADAPTER_WATTS "Watts"
#define PBAT_ADAPTER_MV "AdapterVoltage"
#define PBAT_ADAPTER_MA "Current"

#define PBAT_PROPS_MAX 32
/* The properties of a published snapshot (its percent known), in setting order; returns the count. */
unsigned pagani_battery_props(const PbatSnapshot *s, PbatProp out[PBAT_PROPS_MAX]);
/* The diagnostics string of PaganiBatterySource (not read by iOS). */
const char *pagani_battery_source_name(uint32_t sources);

#ifdef __cplusplus
}
#endif
#endif
