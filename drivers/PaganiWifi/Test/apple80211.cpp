// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of WifiApple80211.h, the request layouts PaganiWifiNet.cpp answers the wireless framework with (sheng's, iPadOS 21H461):
// the request header and the two ioctl numbers, the scan result, the association request (as the daemon fills it: name, BSSID,
// key, cipher 6 with a derived PMK), the channel list and the link event, field by field at the offsets the framework uses; the
// frequency-to-channel records of every channel the service lists; the PMK in hex. The structures are filled through raw bytes at
// the framework's offsets and read back through the fields, so a moved field fails here.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../WifiApple80211.h"

using namespace pagani_wifi_net;

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)

template <class T> static void put(void *base, size_t at, T v) {std::memcpy(static_cast<uint8_t *>(base) + at, &v, sizeof v);}

int main()
{
    // the request header: name 0, type 16, value 20, length 24, user pointer 32
    apple80211req rq;
    std::memset(&rq, 0, sizeof rq);
    std::memcpy(&rq, "en0", 4);
    put<int32_t>(&rq, 16, kAssociate);
    put<int32_t>(&rq, 20, -7);
    put<uint32_t>(&rq, 24, sizeof(assoc_request));
    put<uint64_t>(&rq, 32, 0x16fdf8000ull);
    CHECK(!std::strcmp(rq.req_if_name, "en0") && rq.req_type == kAssociate && rq.req_val == -7 && rq.req_len == 592 &&
          rq.req_data == 0x16fdf8000ull);
    CHECK(SIOCSA80211 == 0x802869c8ul && SIOCGA80211 == 0xc02869c9ul);

    // an association request as the daemon writes it: WPA2-PSK with the PMK it derived (cipher 6, 32 bytes)
    alignas(8) uint8_t raw[sizeof(assoc_request)] = {};
    put<uint32_t>(raw, 0x00, 1);
    put<uint32_t>(raw, 0x0c, 8);
    std::memcpy(raw + 0x10, "HomeWiFi", 8);
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(raw + 0x30, bssid, 6);
    put<uint32_t>(raw, 0x3c, 32);
    put<uint32_t>(raw, 0x40, 6);
    for (unsigned i = 0; i < 32; ++i) raw[0x48 + i] = uint8_t(i * 7 + 1);
    assoc_request a;
    std::memcpy(&a, raw, sizeof a);
    CHECK(a.version == 1 && a.ssid_length == 8 && !std::memcmp(a.ssid, "HomeWiFi", 8) && !std::memcmp(a.bssid, bssid, 6));
    CHECK(a.key_length == 32 && a.key_cipher == kKeyCipherPmk && a.key[0] == 1 && a.key[31] == uint8_t(31 * 7 + 1));
    // the private address, as _performAssociation leaves it: a flag and the address, read through the raw bytes of the same structure
    const uint8_t privateMac[6] = {0x2a, 0x11, 0x22, 0x33, 0x44, 0x55};
    raw[0x1d0] = 0x08;
    std::memcpy(raw + 0x1fc, privateMac, 6);
    std::memcpy(&a, raw, sizeof a);
    const uint8_t *back = reinterpret_cast<const uint8_t *>(&a);
    CHECK((back[kAssocFlags] & kAssocFlagPrivateMac) && !std::memcmp(back + kAssocPrivateMac, privateMac, 6));
    char pmk[65];
    pmkHex(a.key, pmk);
    CHECK(std::strlen(pmk) == 64 && !std::strncmp(pmk, "01080f161d", 10) && pmk[62] == 'd' && pmk[63] == 'a');

    // a scan result as the framework reads it: channel record at 4, RSSI at 22, BSSID 28, SSID length/bytes 0x60/0x61, age 0x84,
    // information elements 0x8c (their length at 0x8a)
    scan_result r;
    std::memset(&r, 0, sizeof r);
    r.version = 1;
    r.ch = channelOf(5180);
    r.rssi = -48;
    std::memcpy(r.bssid, bssid, 6);
    r.ssid_length = 8;
    std::memcpy(r.ssid, "HomeWiFi", 8);
    r.age_ms = 1234;
    r.elements_length = 3;
    r.elements[0] = 7;
    uint8_t out[sizeof r];
    std::memcpy(out, &r, sizeof r);
    uint32_t chan;
    std::memcpy(&chan, out + 8, 4);
    int16_t rssi;
    std::memcpy(&rssi, out + 22, 2);
    uint32_t age;
    std::memcpy(&age, out + 0x84, 4);
    uint16_t ielen;
    std::memcpy(&ielen, out + 0x8a, 2);
    CHECK(chan == 36 && rssi == -48 && !std::memcmp(out + 28, bssid, 6) && out[0x60] == 8 && !std::memcmp(out + 0x61, "HomeWiFi", 8));
    CHECK(age == 1234 && ielen == 3 && out[0x8c] == 7);

    // the channel list: version, count, then 12-byte records
    static channels_list list;
    list.version = 1;
    list.count = 2;
    list.list[0] = channelOf(2412);
    list.list[1] = channelOf(5745);
    uint32_t words[6];
    std::memcpy(words, &list, sizeof words);
    CHECK(words[0] == 1 && words[1] == 2 && words[2] == 1 && words[3] == 1 && words[4] == 0xa && words[5] == 1);
    // every channel the service lists maps to its 802.11 number and band; the DFS ones (52-64, 100-144) are marked and not scanned actively
    const uint32_t numbers[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136,
                                140, 144, 149, 153, 157, 161, 165};
    CHECK(sizeof kListedMhz / sizeof kListedMhz[0] == sizeof numbers / sizeof numbers[0]);
    for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        const channel c = listedChannel(kListedMhz[i]);
        const bool dfs = numbers[i] >= 52 && numbers[i] <= 144;
        CHECK(c.version == 1 && c.channel == numbers[i]);
        CHECK(c.flags == (kListedMhz[i] > 3000 ? 0x12u : 0xau) + (dfs ? 0x100u : 0x80u));
        CHECK(channelOf(kListedMhz[i]).flags == (kListedMhz[i] > 3000 ? 0x12u : 0xau));    // a scan result's channel carries no such marks
    }
    // the networks a scan offers: everything from the cache, from a real scan what it can have heard
    CHECK(bssOffered(0, true) && bssOffered(kBssStaleMs, true) && !bssOffered(kBssStaleMs + 1, true) && bssOffered(10 * kBssStaleMs, false));
    // the link event the service posts on a loss: down 0, RSSI 4, reason 20
    link_event e;
    std::memset(&e, 0, sizeof e);
    e.down = 1;
    e.rssi = -70;
    e.involuntary = 1;
    e.reason = 4;
    uint8_t ev[sizeof e];
    std::memcpy(ev, &e, sizeof e);
    int32_t erssi;
    uint32_t reason;
    std::memcpy(&erssi, ev + 4, 4);
    std::memcpy(&reason, ev + 20, 4);
    CHECK(ev[0] == 1 && erssi == -70 && ev[16] == 1 && reason == 4);
    // the two ways a link ends: lost, with its reason; given up, voluntary, no reason (wifid fills in the reason it noted)
    const link_event lost = linkDown(false, 7), left = linkDown(true, 7);
    CHECK(lost.down == 1 && lost.involuntary == 1 && lost.reason == 7);
    CHECK(left.down == 1 && left.involuntary == 0 && left.reason == 0);
    std::printf("PASS Apple80211 layouts: %u checks\n", checks);
    return 0;
}
