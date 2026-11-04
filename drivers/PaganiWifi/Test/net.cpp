// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// PaganiWifiNet.cpp itself, compiled against Test/net_stub.h and a model of the module's station interface (nw_abi.h): what wifid does in Settings,
// request by request, as IO80211.framework of 21H461 sends it (README.md, Settings): the radio on, a scan and its results to EIO, a join with the PMK
// wifid derived, the link's events and CURRENT_NETWORK, a leave, a join from a joined network, a wrong key, the radio off; the channel list wifid
// filters a scan with; a request the driver does not answer. The request numbers and layouts are WifiApple80211.h's.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "net_stub.h"
extern "C" int kev_post_msg(struct kev_msg *m) {return kev_post_msg_stub(m);}
StubWorld world;
#include "../PaganiWifiNet.cpp"

using namespace pagani_wifi_net;

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)

// ---- the module, as far as the service sees it. The requests only queue: run() is the Wi-Fi thread's next step ----
struct Station {
    nw_wifi_status status = {};
    std::vector<nw_bss_info> list;
    const nw_wifi_events *ev = nullptr;
    int scans = 0, connects = 0, leaves = 0;
    std::string ssid, key;
    bool hasBssid = false, leavePending = false, connectPending = false, failConnect = false;
} S;
extern "C" {
void nw_wifi_set_events(const struct nw_wifi_events *ev) {S.ev = ev;}
int nw_wifi_scan(const uint8_t *, size_t) {++S.scans; return 0;}
int nw_wifi_bss_count(void) {return int(S.list.size());}
int nw_wifi_bss_get(int i, struct nw_bss_info *out)
{
    if (i < 0 || size_t(i) >= S.list.size()) return -1;
    *out = S.list[size_t(i)];
    return 0;
}
int nw_wifi_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen)
{
    if (S.failConnect) return -1;
    ++S.connects;
    S.ssid.assign(reinterpret_cast<const char *>(ssid), len);
    S.key.assign(pass ? pass : "", plen);
    S.hasBssid = bssid != nullptr;
    S.connectPending = true;
    return 0;
}
void nw_wifi_disconnect(void) {++S.leaves; S.leavePending = true;}
int nw_wifi_tx(const uint8_t *, size_t) {return 0;}
void nw_wifi_status_get(struct nw_wifi_status *st) {*st = S.status;}
void nw_wifi_tx_reasons(char *text, int size) {if (size > 0) text[0] = 0;}
void pagani_wifi_wake(void) {}
int pagani_wifi_file(const char *, const uint8_t **, size_t *) {return -1;}
}
static std::vector<std::string> gLog, gSteps;   // every line; the ones that always reach the boot log (WifiUtil.h PwLogGate: level 3 and below)
extern "C" void pagani_wifi_logf(int level, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    gLog.push_back(line);
    if (level <= 3) gSteps.push_back(line);
}
static bool logged(const char *text) {for (const std::string &l : gLog) if (l.find(text) != std::string::npos) return true; return false;}
static bool stepped(const char *text) {for (const std::string &l : gSteps) if (l.find(text) != std::string::npos) return true; return false;}

static nw_bss_info bss(uint8_t last, const char *ssid, uint32_t mhz, int signal, uint32_t age = 500)
{
    nw_bss_info b;
    std::memset(&b, 0, sizeof b);
    const uint8_t id[6] = {2, 0x11, 0x22, 0x33, 0x44, last};
    std::memcpy(b.bssid, id, 6);
    b.ssid_len = uint8_t(std::strlen(ssid));
    std::memcpy(b.ssid, ssid, b.ssid_len);
    b.freq_mhz = mhz;
    b.signal_dbm = int8_t(signal);
    b.age_ms = age;
    b.caps = 0x11;
    b.beacon_int = 100;
    return b;
}
// The Wi-Fi thread's next step: what the real nw_sta_run does with the requests, by the events it gives (tests/sta_sim.c checks the module itself).
static void linkDown(int reason) {S.status.state = NW_STA_IDLE; std::memset(S.status.bssid, 0, 6); S.status.ssid_len = 0; S.ev->link_lost(reason);}
static void linkUp(const nw_bss_info &b)
{
    S.status.state = NW_STA_UP;
    std::memcpy(S.status.bssid, b.bssid, 6);
    S.status.ssid_len = b.ssid_len;
    std::memcpy(S.status.ssid, b.ssid, 32);
    S.status.freq_mhz = b.freq_mhz;
    S.status.signal_dbm = b.signal_dbm;
    S.ev->connect_done(NW_RES_OK);
}
static void run(int joinResult = NW_RES_OK)
{
    if (S.leavePending) {
        S.leavePending = false;
        if (S.status.state == NW_STA_UP) linkDown(0);
        else if (S.status.state >= NW_STA_AUTH) {S.status.state = NW_STA_IDLE; S.ev->connect_done(NW_RES_FAILED);}
    }
    if (S.connectPending) {
        S.connectPending = false;
        if (S.status.state == NW_STA_UP) linkDown(0);
        const nw_bss_info *found = nullptr;
        for (const nw_bss_info &b : S.list) if (S.ssid == std::string(reinterpret_cast<const char *>(b.ssid), b.ssid_len)) found = &b;
        if (joinResult == NW_RES_OK && found) linkUp(*found);
        else S.ev->connect_done(joinResult == NW_RES_OK ? NW_RES_NOT_FOUND : joinResult);
    }
}

// ---- the requests, the way the framework makes them ----
static int request(bool set, apple80211req &rq)
{
    std::strcpy(rq.req_if_name, "en0");
    return if_ioctl(gIf, set ? SIOCSA80211 : SIOCGA80211, &rq);
}
template <class T> static apple80211req with(int32_t type, T *data, uint32_t length = sizeof(T))
{
    apple80211req rq;
    std::memset(&rq, 0, sizeof rq);
    rq.req_type = type;
    rq.req_len = length;
    rq.req_data = reinterpret_cast<uint64_t>(data);
    return rq;
}
static int value(int32_t type, int32_t *out)
{
    apple80211req rq;
    std::memset(&rq, 0, sizeof rq);
    rq.req_type = type;
    const int e = request(false, rq);
    *out = rq.req_val;
    return e;
}
static int get(int32_t type, void *data, uint32_t length)
{
    apple80211req rq = with(type, static_cast<uint8_t *>(data), length);
    return request(false, rq);
}
// _performAssociation with a password on a WPA2 network: 856 bytes, cipher 6, the PMK
static int join(const char *ssid, bool privateMac = false)
{
    static uint8_t raw[856];
    std::memset(raw, 0, sizeof raw);
    auto put = [](size_t at, uint32_t v) {std::memcpy(raw + at, &v, 4);};
    put(0, 1);
    put(0xc, uint32_t(std::strlen(ssid)));
    std::memcpy(raw + 0x10, ssid, std::strlen(ssid));
    put(0x3c, 32);
    put(0x40, kKeyCipherPmk);
    for (unsigned i = 0; i < 32; ++i) raw[0x48 + i] = uint8_t(i + 0xa0);
    if (privateMac) raw[kAssocFlags] |= kAssocFlagPrivateMac;
    apple80211req rq = with(kAssociate, raw, 856);
    return request(true, rq);
}
static int plain(int32_t type)
{
    apple80211req rq;
    std::memset(&rq, 0, sizeof rq);
    rq.req_type = type;
    return request(true, rq);
}
static int power(uint32_t on)
{
    uint32_t in[6] = {1, 1, on, 0, 0, 0};
    apple80211req rq = with(kPower, in, sizeof in);
    return request(true, rq);
}
static int scan()
{
    static uint8_t data[5520];
    std::memset(data, 0, sizeof data);
    data[0] = 1;
    apple80211req rq = with(kScanReq, data, sizeof data);
    return request(true, rq);
}
static std::vector<scan_result> results()
{
    std::vector<scan_result> out;
    for (unsigned guard = 0; guard < 100; ++guard) {
        static uint8_t record[1228];
        std::memset(record, 0, sizeof record);
        const int e = get(kScanResult, record, sizeof record);
        if (e) {CHECK(e == EIO); return out;}                  // the framework's end of the list
        scan_result r;
        std::memcpy(&r, record, sizeof r);
        out.push_back(r);
    }
    CHECK(false);
    return out;
}
// the 802.11 events of class 6 since the last call, by code; the link's kernel events are counted on their own
static std::vector<uint32_t> events(unsigned *linkOn = nullptr, unsigned *linkOff = nullptr, std::vector<link_event> *links = nullptr)
{
    std::vector<uint32_t> codes;
    for (const StubEvent &e : world.events) {
        if (e.cls == kEventClass80211) {
            CHECK(e.sub == kEventSubclass80211 && e.data.size() >= 16 && !std::strcmp(reinterpret_cast<const char *>(e.data.data()), "en0"));
            codes.push_back(e.code);
            if (e.code == kEvLinkChanged && links) {
                link_event l;
                CHECK(e.data.size() == 16 + sizeof l);                // _dispatchLinkChangedEvent: at least 32 bytes
                std::memcpy(&l, e.data.data() + 16, sizeof l);
                links->push_back(l);
            }
        } else if (e.cls == KEV_NETWORK_CLASS) {
            if (e.code == KEV_DL_LINK_ON && linkOn) ++*linkOn;
            if (e.code == KEV_DL_LINK_OFF && linkOff) ++*linkOff;
        }
    }
    world.events.clear();
    return codes;
}
static const std::vector<uint32_t> kJoined = {kEvAssocDone, kEvSsidChanged, kEvBssidChanged, kEvLinkChanged};
static const std::vector<uint32_t> kLeft = {kEvLinkChanged, kEvBssidChanged, kEvSsidChanged};

int main()
{
    const uint8_t mac[6] = {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    pagani_wifi_net_start(mac);
    CHECK(gIf && S.ev && world.ioctl);
    S.status.state = NW_STA_IDLE;
    std::memcpy(S.status.mac, mac, 6);

    // ---- the radio on, the state a daemon starts from ----
    CHECK(power(1) == 0 && !gUserJoin && S.leaves == 0);
    CHECK(events() == std::vector<uint32_t>{kEvPowerChanged});
    uint32_t pw[6] = {};
    CHECK(get(kPower, pw, 24) == 0 && pw[0] == 1 && pw[1] == 1 && pw[2] == 1);
    int32_t v = -1;
    CHECK(value(kState, &v) == 0 && v == 0 && value(kOpMode, &v) == 0 && v == 1);
    uint8_t text[64];
    CHECK(get(kSsid, text, 32) == ENOENT && get(kBssid, text, 6) == ENOENT && get(kRssi, text, sizeof text) == ENOENT && get(kCurrentNetwork, text, sizeof text) == ENOENT);

    // ---- the channels wifid filters a scan with: the DFS ones are there, marked, and it gets the same list from both requests ----
    static channels_list chans, hw;
    CHECK(get(kSupportedChannels, &chans, sizeof chans) == 0 && get(kHwSupportedChannels, &hw, sizeof hw) == 0);
    CHECK(chans.version == 1 && chans.count == sizeof kListedMhz / sizeof kListedMhz[0] && !std::memcmp(&chans, &hw, sizeof chans));
    bool dfs52 = false;
    for (unsigned i = 0; i < chans.count; ++i) if (chans.list[i].channel == 52) dfs52 = (chans.list[i].flags & kChanDfs) && !(chans.list[i].flags & kChanActive);
    CHECK(dfs52);

    // ---- a scan: the radio is asked, SCAN_DONE, the records, EIO. What this scan did not hear is not offered ----
    S.list = {bss(1, "HomeWiFi", 5745, -48), bss(2, "Other", 2437, -60), bss(3, "GoneAway", 5180, -50, kBssStaleMs + 1), bss(4, "Radar", 5260, -55)};
    int scans = S.scans;
    CHECK(scan() == 0 && S.scans == scans + 1);
    S.ev->scan_done();
    CHECK(events() == std::vector<uint32_t>{kEvScanDone});
    std::vector<scan_result> list = results();
    CHECK(list.size() == 3 && !std::memcmp(list[0].ssid, "HomeWiFi", 8) && list[0].ch.channel == 149 && list[0].rssi == -48 && list[2].ch.channel == 52);
    CHECK(results().size() == 3);                                 // the cursor starts over after the end, as a second read of the same scan does

    // ---- a wrong password: the framework reads 21 (Invalid PMK, WiFiKit: Incorrect Password), no link event ----
    CHECK(join("HomeWiFi") == 0 && S.connects == 1 && S.key.size() == 64 && S.key.substr(0, 8) == "a0a1a2a3" && !S.hasBssid);
    CHECK(gUserJoin && !logged("private address"));
    run(NW_RES_BAD_KEY);
    unsigned on = 0, off = 0;
    CHECK(events(&on, &off) == std::vector<uint32_t>{kEvAssocDone} && !on && !off);
    CHECK(value(kAssociateResult, &v) == 0 && v == kAssocBadKey);

    // ---- the right one, with a private address asked for: en0 keeps the board's, the log says so ----
    CHECK(join("HomeWiFi", true) == 0 && logged("private address asked"));
    CHECK(value(kAssociateResult, &v) == 0 && v == kAssocFailed);   // until the module answers
    run();
    std::vector<link_event> links;
    CHECK(events(&on, &off, &links) == kJoined && on == 1 && !off && world.running);
    CHECK(links.size() == 1 && !links[0].down && links[0].rssi == -48);
    CHECK(value(kAssociateResult, &v) == 0 && v == kAssocOk && value(kState, &v) == 0 && v == 4);
    std::memset(text, 0, sizeof text);
    CHECK(get(kSsid, text, 32) == 0 && !std::memcmp(text, "HomeWiFi", 8) && get(kBssid, text, 6) == 0 && text[5] == 1);
    // wifid drops a link-up it cannot put a network to: CURRENT_NETWORK answers, also when the module's list has lost the entry
    static uint8_t record[1228];
    CHECK(get(kCurrentNetwork, record, sizeof record) == 0);
    scan_result current;
    std::memcpy(&current, record, sizeof current);
    CHECK(!std::memcmp(current.ssid, "HomeWiFi", 8) && current.ch.channel == 149);
    const std::vector<nw_bss_info> before = S.list;
    S.list.erase(S.list.begin());
    std::memset(record, 0, sizeof record);
    CHECK(get(kCurrentNetwork, record, sizeof record) == 0);
    std::memcpy(&current, record, sizeof current);
    CHECK(!std::memcmp(current.ssid, "HomeWiFi", 8) && current.rssi == -48);
    S.list = before;
    // the signal as the module has it now
    S.status.signal_dbm = -71;
    int32_t chains[16] = {};
    CHECK(get(kRssi, chains, sizeof chains) == 0 && chains[3] == -71 && chains[7] == -71);
    // joined: no scan goes to the radio, the results come from the cache after a while, all of them
    scans = S.scans;
    CHECK(scan() == 0 && S.scans == scans && world.delayed);
    world.delayed->function(nullptr, nullptr);
    CHECK(events() == std::vector<uint32_t>{kEvScanDone});
    CHECK(results().size() == 4);

    // ---- a join from the joined network, no leave first ----
    int leaves = S.leaves;
    CHECK(join("Other") == 0 && logged("from a joined network") && S.leaves == leaves);
    links.clear();
    on = off = 0;
    run();
    std::vector<uint32_t> joinedAgain = kLeft;
    joinedAgain.insert(joinedAgain.end(), kJoined.begin(), kJoined.end());
    CHECK(events(&on, &off, &links) == joinedAgain && on == 1 && off == 1 && world.running);
    CHECK(links.size() == 2 && links[0].down && !links[0].involuntary && !links[0].reason && !links[1].down);
    CHECK(value(kState, &v) == 0 && v == 4 && get(kSsid, text, 32) == 0 && !std::memcmp(text, "Other", 5));

    // ---- the access point ends it: involuntary, with its reason ----
    links.clear();
    on = off = 0;
    S.status.state = NW_STA_IDLE;
    S.ev->link_lost(15);
    CHECK(events(&on, &off, &links) == kLeft && off == 1 && !world.running);
    CHECK(links.size() == 1 && links[0].down && links[0].involuntary && links[0].reason == 15);

    // ---- a leave: wifid's state moves on the link's end, which comes with the step; the kernel's link event once ----
    CHECK(join("HomeWiFi") == 0);
    run();
    events();
    links.clear();
    on = off = 0;
    CHECK(plain(kDisassociate) == 0 && S.leaves == leaves + 1 && S.leavePending);
    CHECK(events(&on, &off).empty() && !off);                     // nothing yet: the module has not left
    run();
    CHECK(events(&on, &off, &links) == kLeft && off == 1);
    CHECK(links.size() == 1 && links[0].down && !links[0].involuntary && !links[0].reason);
    CHECK(value(kState, &v) == 0 && v == 0 && get(kSsid, text, 32) == ENOENT && get(kBssid, text, 6) == ENOENT);
    // a leave with no link: no link event, the module is asked anyway
    on = off = 0;
    CHECK(plain(kDisassociate) == 0);
    run();
    CHECK(events(&on, &off).empty() && off == 1);
    // a leave during a join: the join's result, no link to lose, and no voluntary mark left for the next loss
    S.status.state = NW_STA_AUTH;
    CHECK(plain(kDeauth) == 0);
    run();
    CHECK(events() == std::vector<uint32_t>{kEvAssocDone} && value(kAssociateResult, &v) == 0 && v == kAssocFailed);
    links.clear();
    S.status.state = NW_STA_IDLE;
    S.ev->link_lost(4);
    events(nullptr, nullptr, &links);
    CHECK(links.size() == 1 && links[0].involuntary);

    // ---- the radio off, joined: the link's end, voluntary, and the power event; off the state reads 0, on it works again ----
    CHECK(join("HomeWiFi") == 0);
    run();
    events();
    links.clear();
    CHECK(power(0) == 0 && logged("power set 0") && S.leavePending);
    CHECK(events() == std::vector<uint32_t>{kEvPowerChanged});
    run();
    CHECK(events(nullptr, nullptr, &links) == kLeft && links.size() == 1 && !links[0].involuntary);
    CHECK(get(kPower, pw, 24) == 0 && pw[2] == 0 && value(kState, &v) == 0 && v == 0);
    CHECK(power(1) == 0 && S.leaves >= leaves && get(kPower, pw, 24) == 0 && pw[2] == 1);
    events();

    // ---- what the module refuses, and a request nobody answers ----
    S.failConnect = true;
    CHECK(join("HomeWiFi") == EINVAL);
    S.failConnect = false;
    CHECK(plain(kRadioInfo) == EOPNOTSUPP && get(kRadioInfo, text, 8) == EOPNOTSUPP && get(kChannelsInfo, text, 8) == EOPNOTSUPP);
    CHECK(logged("type 54 set") && logged("-> 102 "));
    // what the walk through Settings is judged by (wifi-rounds.py WALK) is never held back
    for (const char *line : {"wifi-net: join: ", "join finished: module result 4", "join finished: module result 0", "wifi-net: leave", "link lost: reason 0, as asked",
                             "from a joined network", "power set 0", "power set 1"})
        CHECK(stepped(line));
    std::printf("PASS the network service: %u checks\n", checks);
    return 0;
}
