// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The Apple80211 request layouts of UniXNU-sheng package/WifiNet.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e),
// moved into a header so that the host test (Test/apple80211.cpp) checks the same definitions the kernel builds. The layouts are
// those iPadOS 17.7.x's wireless framework reads and writes on an "en" interface (sheng: same 21H461 build, WPA2 join, DHCP and
// speed test on hardware); where a field's meaning is not known it stays zero. Plain C++ (no kernel header), little endian, LP64.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pagani_wifi_net {

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    uint64_t req_data __attribute__((aligned(8)));   // user_addr_t
};

// _IOW('i', 200, struct apple80211req) and _IOWR('i', 201, ...): IOC_IN 0x80000000 (| IOC_OUT 0x40000000), length << 16, group << 8.
constexpr unsigned long kIocIn = 0x80000000ul, kIocOut = 0x40000000ul;
constexpr unsigned long ioc(unsigned long inout, unsigned long group, unsigned long num, unsigned long len)
{
    return inout | (len & 0x1fff) << 16 | group << 8 | num;
}
constexpr unsigned long SIOCSA80211 = 0x802869c8ul;
constexpr unsigned long SIOCGA80211 = 0xc02869c9ul;
static_assert(sizeof(apple80211req) == 0x28 && offsetof(apple80211req, req_data) == 0x20, "apple80211req layout");
static_assert(SIOCSA80211 == ioc(kIocIn, 'i', 200, sizeof(apple80211req)), "SIOCSA80211 encoding");
static_assert(SIOCGA80211 == ioc(kIocIn | kIocOut, 'i', 201, sizeof(apple80211req)), "SIOCGA80211 encoding");

// request types
enum : int32_t {
    kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10,
    kScanResult = 11, kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20,
    kAssociateResult = 21, kDisassociate = 22, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43,
    kHardwareVersion = 44, kCountryCode = 51, kRadioInfo = 54, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103,
    kChannelsInfo = 207, kHwSupportedChannels = 254,
};
// events (class IEEE 802.11 = 6, subclass 1)
enum : uint32_t {kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10};
constexpr uint32_t kEventClass80211 = 6, kEventSubclass80211 = 1;
// results of an association, as the framework numbers them
enum : int {kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21};

struct channel {uint32_t version, channel, flags;};
struct channels_list {uint32_t version, count; channel list[401]; uint32_t unused;};              // 4824 bytes
struct scan_result {                                                                            // 1164 bytes
    uint32_t version; channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability;
    uint8_t bssid[6]; uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms;
    uint8_t flags, unused2; uint16_t elements_length; uint8_t elements[1024];
};
// (IO80211.framework _performAssociation, 21H461: the request is 856 bytes, the kernel reads the first 592. The daemon's private address for
// the network is at kAssocPrivateMac when bit 3 of the byte at kAssocFlags is set. The firmware's station keeps the board's address and en0 its
// lladdr, which is consistent for the network and the address configuration; the private address is only what Settings shows.)
constexpr size_t kAssocFlags = 0x1d0, kAssocPrivateMac = 0x1fc;
constexpr uint8_t kAssocFlagPrivateMac = 1u << 3;
constexpr uint32_t kKeyCipherPmk = 6, kKeyCipherPassphrase = 10;
struct assoc_request {                                                                          // 592 bytes
    uint32_t version; uint16_t mode, auth_lower, auth_upper, unused0; uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6],
    unused1[2]; uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64];
    uint8_t rest[0x250 - 0x88];
};
struct link_event {
    uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3];
    uint32_t reason, subreason; uint8_t unused3[4];
};
static_assert(sizeof(scan_result) == 1164 && offsetof(scan_result, ssid_length) == 0x60 && offsetof(scan_result, age_ms) == 0x84 &&
              offsetof(scan_result, elements) == 0x8c, "scan result layout");
static_assert(sizeof(assoc_request) == 592 && offsetof(assoc_request, key) == 0x48 && offsetof(assoc_request, ssid) == 0x10 &&
              offsetof(assoc_request, bssid) == 0x30 && offsetof(assoc_request, key_cipher) == 0x40, "association layout");
static_assert(sizeof(channels_list) == 4824 && offsetof(channels_list, list) == 8, "channel list layout");
static_assert(sizeof(link_event) == 32 && offsetof(link_event, reason) == 20, "link event layout");

// The channel record of a frequency: 5 GHz (flags 0x2 | 0x10) or 2.4 GHz (0x2 | 0x8); channel 14 is 2484 MHz.
constexpr channel channelOf(uint32_t mhz)
{
    return mhz >= 4900 ? channel{1, (mhz - 5000) / 5, 0x2 | 0x10} : channel{1, mhz == 2484 ? 14u : (mhz - 2407) / 5, 0x2 | 0x8};
}
static_assert(channelOf(2412).channel == 1 && channelOf(2484).channel == 14 && channelOf(5180).channel == 36 &&
              channelOf(5825).channel == 165 && channelOf(5180).flags == 0x12 && channelOf(2437).flags == 0xa, "channel numbers");

// The channels SUPPORTED_CHANNELS and HW_SUPPORTED_CHANNELS list (wifid's _wifi_device_scan_async keeps only these of what a scan asks for; the
// module scans every channel of its bands anyway): 2.4 GHz 1-11, UNII-1, UNII-2A and -2C (DFS: 52-64, 100-144, passive), UNII-3. A router of this
// country on a DFS channel is otherwise a network the system was told it cannot reach.
constexpr uint16_t kListedMhz[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
                                   5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825};
constexpr uint32_t kChanActive = 0x80, kChanDfs = 0x100;    // scanned actively; radar rules: listened to only
constexpr bool isDfs(uint32_t mhz) {return mhz >= 5250 && mhz <= 5730;}
constexpr channel listedChannel(uint32_t mhz)
{
    const channel c = channelOf(mhz);
    return channel{c.version, c.channel, c.flags | (isDfs(mhz) ? kChanDfs : kChanActive)};
}
static_assert(sizeof kListedMhz / sizeof kListedMhz[0] == 36 && sizeof kListedMhz / sizeof kListedMhz[0] <= 401, "channel list");
static_assert(listedChannel(5260).channel == 52 && listedChannel(5260).flags == 0x112 && listedChannel(5720).channel == 144 &&
              listedChannel(5240).flags == 0x92 && listedChannel(5745).flags == 0x92 && listedChannel(2412).flags == 0x8a, "listed channels");

// A network a real scan did not hear stays in the module's list: it stops being offered once the scan is over. A scan takes seconds, so anything
// older than this was not heard by it. Results answered from the cache (joined: no scan runs) are all offered, as they all are old.
constexpr uint32_t kBssStaleMs = 30000;
constexpr bool bssOffered(uint32_t ageMs, bool fromRealScan) {return !fromRealScan || ageMs <= kBssStaleMs;}

// The link's end: whoever asked for it (a leave, the radio off, a join from another network) gets a voluntary one, wifid then keeps the reason it
// noted itself; anything else is the access point's or the radio's doing, an involuntary one with its reason.
inline link_event linkDown(bool asked, uint32_t reason)
{
    link_event e = {};
    e.down = 1;
    e.involuntary = asked ? 0 : 1;
    e.reason = asked ? 0 : reason;
    return e;
}

// The pairwise master key the daemon derived (cipher 6, 32 bytes) as the 64 hex digits the module takes; out holds 65 bytes.
inline void pmkHex(const uint8_t key[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[key[i] >> 4];
        out[2 * i + 1] = hex[key[i] & 15];
    }
    out[64] = 0;
}

}  // namespace pagani_wifi_net
