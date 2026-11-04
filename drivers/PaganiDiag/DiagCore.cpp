// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// PaganiDiag core (DiagCore.h): line decisions without device APIs.
#include "DiagCore.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pagani_diag {

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool parse_unsigned(const std::string &text, unsigned low, unsigned high, unsigned &out) {
    if (text.empty() || text.size() > 9) return false;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + unsigned(c - '0');
    }
    if (value < low || value > high) return false;
    out = value;
    return true;
}

bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

bool migration_process(const std::string &p) {
    return p == "com.apple.migrationpluginwrapper" || p == "com.apple.datamigrator" || p == "datamigrator";
}

bool install_process(const std::string &p) { return p == "installd" || p == "installcoordinationd"; }

// "SystemApp" (MISystemAppMigrator, SystemAppState, LookupSystemAppState, ...) but not "SystemAppearance", which every UIKit/AppKit
// process logs (NSRequiresAquaSystemAppearance).
bool mentions_system_app(const std::string &message) {
    for (size_t at = message.find("SystemApp"); at != std::string::npos; at = message.find("SystemApp", at + 1))
        if (message.compare(at + 9, 7, "earance") != 0) return true;
    return false;
}

// Control characters as spaces, each UTF-8 sequence (or stray non-ASCII byte) as one '?', runs of blanks as one space, no blank at
// either end.
std::string ascii(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool skipping = false;      // inside a multi-byte sequence already shown as '?'
    for (unsigned char c : s) {
        if (c >= 0x80) {
            if ((c & 0xc0) == 0x80 && skipping) continue;
            skipping = c >= 0xc0;
            out += '?';
            continue;
        }
        skipping = false;
        const char ch = c < 0x20 || c == 0x7f ? ' ' : char(c);
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// At most max characters, the last one '~' when cut.
std::string cut(std::string s, size_t max) {
    if (s.size() <= max) return s;
    if (max == 0) return std::string();
    s.resize(max - 1);
    s += '~';
    return s;
}

std::string clip(const std::string &s, size_t max) { return cut(ascii(s), max); }

std::string field(const std::string &s, size_t max) {
    std::string out = s.substr(0, std::min(s.size(), max));
    for (char &c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string state_counts(const std::vector<std::pair<std::string, long>> &states) {
    std::map<long, unsigned> counts;
    for (const auto &entry : states) counts[entry.second]++;
    std::string out;
    for (const auto &count : counts) out += ' ' + std::to_string(count.first) + ':' + std::to_string(count.second);
    return out;
}

}  // namespace

// "60,180,600": ascending seconds (1..3600), at most kMaxRounds; "none": no round.
bool parse_rounds(const std::string &text, std::vector<unsigned> &out) {
    std::vector<unsigned> rounds;
    if (text != "none") {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            unsigned value = 0;
            if (!parse_unsigned(text.substr(start, end - start), 1, 3600, value) || (!rounds.empty() && value <= rounds.back()) ||
                rounds.size() == kMaxRounds)
                return false;
            rounds.push_back(value);
            start = end + 1;
        }
    }
    out = rounds;
    return true;
}

// "kc,aud": names of round_probes(); "none": no probe left out.
bool parse_skip(const std::string &text, std::vector<std::string> &out) {
    std::vector<std::string> skip;
    if (text != "none") {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            const std::string name = text.substr(start, end - start);
            if (std::find(round_probes().begin(), round_probes().end(), name) == round_probes().end()) return false;
            skip.push_back(name);
            start = end + 1;
        }
    }
    out = skip;
    return true;
}

void parse_config(const std::string &text, Config &config, std::string *bad) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t equals = line.find('=');
        const std::string key = trim(line.substr(0, equals)), value = equals == std::string::npos ? "" : trim(line.substr(equals + 1));
        bool ok = equals != std::string::npos;
        unsigned number = 0;
        if (!ok) {
            // a key without "=value": named in *bad below
        } else if (key == "rounds") {
            ok = parse_rounds(value, config.rounds);
        } else if (key == "skip") {
            ok = parse_skip(value, config.skip);
        } else if (key == "budget_lines") {
            ok = parse_unsigned(value, 2, 300, config.budget_lines);
        } else if (key == "budget_bytes") {
            ok = parse_unsigned(value, 512, 16384, config.budget_bytes);
        } else if (key == "migration") {
            ok = parse_unsigned(value, 0, 1, number);
            if (ok) config.migration = number != 0;
        } else if (key == "minutes") {
            ok = parse_unsigned(value, 0, 60, config.minutes);
        } else if (key == "period") {
            ok = parse_unsigned(value, 5, 600, config.period);
        } else if (key == "pass_lines") {
            ok = parse_unsigned(value, 0, 200, config.pass_lines);
        } else if (key == "boot_lines") {
            ok = parse_unsigned(value, 0, 1000, config.boot_lines);
        } else if (key == "notice_lines") {
            ok = parse_unsigned(value, 0, 1000, config.notice_lines);
        } else if (key == "info_lines") {
            ok = parse_unsigned(value, 0, 1000, config.info_lines);
        } else if (key == "facts_every") {
            ok = parse_unsigned(value, 0, 100, config.facts_every);
        } else if (key == "gap_ms") {
            ok = parse_unsigned(value, 100, 10000, config.gap_ms);
        } else if (key == "log" || key == "logpref") {
            ok = parse_unsigned(value, 0, 1, number);
            if (ok) (key == "log" ? config.log : config.log_preference) = number != 0;
        } else {
            ok = false;
        }
        if (!ok && bad) {
            if (!bad->empty()) *bad += ' ';
            *bad += key.empty() ? std::string("?") : clip(key, 24);
        }
    }
}

std::string entry_line(const Entry &entry) {
    char head[96];
    snprintf(head, sizeof head, "L\t%.6f\t%d\t%d\t", entry.time, entry.level, entry.pid);
    return head + field(entry.process, 64) + '\t' + field(entry.subsystem, 64) + '\t' + field(entry.message, 600);
}

bool parse_entry(const std::string &line, Entry &entry) {
    if (line.size() < 2 || line[0] != 'L' || line[1] != '\t') return false;
    std::vector<std::string> parts;
    size_t start = 2;
    for (int i = 0; i < 5; ++i) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) return false;
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    char *end = nullptr;
    const double time = strtod(parts[0].c_str(), &end);
    if (parts[0].empty() || *end) return false;
    const long level = strtol(parts[1].c_str(), &end, 10);
    if (parts[1].empty() || *end || level < 0 || level > 5) return false;
    const long pid = strtol(parts[2].c_str(), &end, 10);
    if (parts[2].empty() || *end) return false;
    entry.time = time;
    entry.level = int(level);
    entry.pid = int(pid);
    entry.process = parts[3];
    entry.subsystem = parts[4];
    entry.message = line.substr(start);
    return true;
}

int classify(const Entry &entry, const std::string &self) {
    if (entry.level < kInfo || entry.level > kFault) return -1;
    if (!self.empty() && contains(entry.message, self.c_str())) return -1;
    const bool migration = migration_process(entry.process), install = install_process(entry.process);
    const bool migration_subsystem = entry.subsystem == "com.apple.migration";
    const bool system_app = mentions_system_app(entry.message);
    if (system_app && (migration || install || migration_subsystem)) return 0;
    if (!migration && !install) return (system_app && entry.level >= kNotice) || migration_subsystem ? 2 : -1;
    if (entry.level >= kError) return 0;
    if (install) return entry.level >= kNotice ? 1 : -1;
    return entry.level >= kNotice || migration_subsystem ? 2 : -1;
}

std::string process_tag(const std::string &process) {
    if (process == "com.apple.migrationpluginwrapper") return "mpw";
    if (process == "com.apple.datamigrator" || process == "datamigrator") return "dm";
    if (process == "installd") return "ind";
    if (process == "installcoordinationd") return "icd";
    std::string name = process.compare(0, 10, "com.apple.") == 0 ? process.substr(10) : process;
    name = ascii(name);
    for (char &c : name)
        if (c == ' ') c = '_';
    if (name.empty()) name = "?";
    return name.substr(0, 12);
}

char level_letter(int level) {
    switch (level) {
    case kInfo: return 'I';
    case kNotice: return 'N';
    case kError: return 'E';
    case kFault: return 'F';
    default: return '?';
    }
}

std::string compact(const std::string &message, size_t max) {
    std::string s = ascii(message);
    // MobileInstallation's thread address ("%p %s: %s" in MOLogWriteV).
    if (s.size() > 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        size_t i = 2;
        while (i < s.size() && is_hex(s[i])) ++i;
        if (i - 2 >= 4 && i - 2 <= 16 && i < s.size() && s[i] == ' ') s.erase(0, i + 1);
    }
    // "-[Class selector:with:parts:]: text" -> "Class.selector: text".
    if (s.size() > 3 && (s[0] == '-' || s[0] == '+') && s[1] == '[') {
        const size_t close = s.find(']');
        const size_t space = s.find(' ');
        if (close != std::string::npos && space != std::string::npos && space < close) {
            const std::string cls = s.substr(2, std::min<size_t>(space - 2, 32));
            std::string selector = s.substr(space + 1, close - space - 1);
            selector = selector.substr(0, std::min(selector.find(':'), size_t(40)));
            size_t rest = close + 1;
            if (rest < s.size() && s[rest] == ':') ++rest;
            while (rest < s.size() && s[rest] == ' ') ++rest;
            s = cls + '.' + selector + (rest < s.size() ? ": " + s.substr(rest) : std::string());
        }
    }
    static const std::string migrator = "MISystemAppMigrator: ";
    if (s.compare(0, migrator.size(), migrator) == 0) s = "SAM: " + s.substr(migrator.size());
    static const std::string apple = "com.apple.";
    for (size_t at; (at = s.find(apple)) != std::string::npos;) s.erase(at, apple.size());
    return cut(s, max);
}

std::string format_entry(const Entry &entry, double boot_time) {
    const long seconds = entry.time > boot_time ? long(entry.time - boot_time) : 0;
    const std::string head = process_tag(entry.process) + ' ' + level_letter(entry.level) + '@' + std::to_string(seconds) + ' ';
    return head + compact(entry.message, kLineMax > head.size() ? kLineMax - head.size() : 0);
}

std::string entry_key(const Entry &entry) {
    return process_tag(entry.process) + ' ' + level_letter(entry.level) + ' ' + compact(entry.message, kLineMax);
}

Rationer::Rationer(const Config &config, std::string self) : config_(config), self_(std::move(self)) {}

PassResult Rationer::pass(const std::vector<Entry> &entries, double boot_time) {
    struct Candidate {
        int cls;
        double time;
        size_t order;
        std::string key;
    };
    PassResult result;
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> this_pass;
    for (size_t i = 0; i < entries.size(); ++i) {
        const int cls = classify(entries[i], self_);
        if (cls < 0) continue;
        ++result.candidates;
        std::string key = entry_key(entries[i]);
        if (seen_.count(key) || !this_pass.insert(key).second) {
            ++result.duplicates;
            continue;
        }
        candidates.push_back({cls, entries[i].time, i, std::move(key)});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.cls != b.cls ? a.cls < b.cls : a.time < b.time;
    });
    const unsigned class_limit[3] = {UINT_MAX, config_.notice_lines, config_.info_lines};
    std::vector<const Candidate *> chosen;
    for (const Candidate &c : candidates) {
        if (chosen.size() < config_.pass_lines && logged_ < config_.boot_lines && class_logged_[c.cls] < class_limit[c.cls]) {
            chosen.push_back(&c);
            ++logged_;
            ++class_logged_[c.cls];
            seen_.insert(c.key);
        } else {
            ++result.suppressed;
        }
    }
    std::stable_sort(chosen.begin(), chosen.end(), [](const Candidate *a, const Candidate *b) {
        return a->time != b->time ? a->time < b->time : a->order < b->order;
    });
    for (const Candidate *c : chosen) result.lines.push_back(format_entry(entries[c->order], boot_time));
    suppressed_ += result.suppressed;
    duplicates_ += result.duplicates;
    return result;
}

std::string suppressed_line(const PassResult &result) {
    if (!result.suppressed) return std::string();
    return "suppressed " + std::to_string(result.suppressed) + " dup " + std::to_string(result.duplicates);
}

std::string kernel_text(const std::string &line) {
    std::string out;
    for (unsigned char c : line) {
        if (c == '\n' || c == '\r' || c == 0 || out.size() >= kKernelText) break;
        out += c < 0x20 || c >= 0x7f ? ' ' : char(c);
    }
    return out;
}

std::vector<std::string> facts_lines(const std::map<std::string, std::string> &values) {
    auto get = [&](const char *key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() || it->second.empty() ? std::string("-") : it->second;
    };
    std::vector<std::string> lines;
    lines.push_back("mg fam " + get("mg.SupportedDeviceFamilies") + " dcn " + get("mg.DeviceClassNumber") + " dc " +
                    get("mg.DeviceClass") + " pt " + get("mg.ProductType") + " hw " + get("mg.HWModelStr") + " arm64 " + get("mg.arm64"));
    lines.push_back("hw product " + get("hw.product") + " target " + get("hw.target") + " machine " + get("hw.machine") + " model " +
                    get("hw.model") + " arch " + get("arch"));
    lines.push_back("caps " + get("caps") + " skip " + get("fam.skip") + " fam " + get("fam.sam"));
    lines.push_back("force " + get("force") + " own " + get("force.stat") + " skipfam mob " + get("skip.mobile") + " root " +
                    get("skip.root") + " logpref " + get("logpref"));
    for (std::string &line : lines) line = clip(line, kLineMax);
    return lines;
}

const std::vector<std::pair<std::string, std::string>> &mg_keys() {
    static const std::vector<std::pair<std::string, std::string>> keys = {
        {"HasBattery", "bat"}, {"DeviceSupportsHaptics", "hap"}, {"DeviceSupportsClosedLoopHaptics", "clh"}, {"wifi", "wifi"},
        {"bluetooth", "bt"}, {"accelerometer", "acc"}, {"gyroscope", "gyr"}, {"magnetometer", "mag"},
        {"ambient-light-sensor", "als"}, {"proximity-sensor", "prx"}, {"microphone", "mic"}, {"camera-flash", "fla"},
        {"still-camera", "cam"}, {"gps", "gps"},
    };
    return keys;
}

const std::vector<std::pair<std::string, std::string>> &io_classes() {
    static const std::vector<std::pair<std::string, std::string>> classes = {
        {"PaganiInput", "in"}, {"PaganiPanel", "pan"}, {"PaganiAudio", "aud"}, {"PaganiBattery", "bat"}, {"PaganiSensorHid", "sen"},
        {"IOPMPowerSource", "pwr"}, {"IO80211Interface", "wl"}, {"AppleKeyStore", "aks"}, {"IOHIDEventService", "hid"},
    };
    return classes;
}

const std::vector<std::pair<unsigned, unsigned>> &hid_pairs() {
    static const std::vector<std::pair<unsigned, unsigned>> pairs = {
        {0xff00, 3}, {0xff00, 4}, {0xff00, 8}, {0xff00, 9}, {0xff00, 0x0a}, {0xff0c, 3}, {0x0b, 0x2e},
    };
    return pairs;
}

std::string hid_key(unsigned page, unsigned usage) {
    char text[24];
    snprintf(text, sizeof text, "%x/%x", page, usage);
    return text;
}

const std::vector<std::string> &round_probes() {
    static const std::vector<std::string> probes = {"mg", "pwr", "hid", "kc", "aud", "sen", "hap", "wid"};
    return probes;
}

namespace {

std::string value_of(const Values &values, const std::string &key) {
    const auto it = values.find(key);
    return it == values.end() || it->second.empty() ? std::string("-") : it->second;
}

// "<found>/<all>" of the answers that are `found` for the keys; "-" when none answered.
template <typename Found>
std::string tally(const std::vector<std::string> &keys, const Values &values, Found found) {
    size_t answered = 0, yes = 0;
    for (const std::string &key : keys) {
        const std::string value = value_of(values, key);
        if (value == "-") continue;
        ++answered;
        yes += found(value);
    }
    return answered ? std::to_string(yes) + "/" + std::to_string(keys.size()) : std::string("-");
}

std::vector<std::string> mg_value_keys() {
    std::vector<std::string> keys;
    for (const auto &key : mg_keys()) keys.push_back("mg." + key.second);
    return keys;
}

std::vector<std::string> hid_value_keys() {
    std::vector<std::string> keys;
    for (const auto &pair : hid_pairs()) keys.push_back("hid." + hid_key(pair.first, pair.second));
    return keys;
}

std::vector<std::string> io_value_keys() {
    std::vector<std::string> keys;
    for (const auto &item : io_classes()) keys.push_back("io." + item.second);
    return keys;
}

bool is_one(const std::string &value) { return value == "1"; }
bool not_zero(const std::string &value) { return value != "0"; }

// One word of a value for the lines: its first `max` characters, blanks as '_'.
std::string word(const std::string &value, size_t max) {
    std::string out = ascii(value).substr(0, max);
    for (char &c : out)
        if (c == ' ') c = '_';
    return out.empty() ? std::string("-") : out;
}

}  // namespace

bool widget_extension_path(const char *path) {
    if (!strstr(path, ".appex/")) return false;
    const char *plugins = strstr(path, "/PlugIns/");
    return plugins && strstr(plugins, "Widget") && !strstr(plugins, "Intent");
}

void census_values(const std::vector<WidgetEntry> &entries, Values &values) {
    struct Pair {
        unsigned view = 0, root = 0;
        void add(bool yes, bool in_view) {
            if (yes) ++(in_view ? view : root);
        }
        std::string text() const { return std::to_string(view) + '/' + std::to_string(root); }
    } disk, registered, valid, point;
    std::map<long, unsigned> platforms, errors;
    unsigned elsewhere = 0;
    for (const WidgetEntry &entry : entries) {
        disk.add(true, entry.in_view);
        registered.add(entry.registered, entry.in_view);
        valid.add(entry.valid, entry.in_view);
        point.add(entry.point, entry.in_view);
        elsewhere += entry.registered && entry.elsewhere;
        if (entry.registered) ++platforms[entry.platform];
        if (!entry.valid && entry.error) ++errors[entry.error];
    }
    std::string platform_text;
    for (const auto &item : platforms) platform_text += (platform_text.empty() ? "" : " ") + std::to_string(item.first) + ':' + std::to_string(item.second);
    std::string error_text = "0";
    unsigned most = 0;
    for (const auto &item : errors)
        if (item.second > most) {
            most = item.second;
            error_text = std::to_string(item.first) + 'x' + std::to_string(item.second);
        }
    values["wk.n"] = std::to_string(entries.size());
    values["wk.fs"] = disk.text();
    values["wk.reg"] = registered.text();
    values["wk.ok"] = valid.text();
    values["wk.okn"] = std::to_string(valid.view + valid.root);
    values["wk.ep"] = point.text();
    values["wk.pf"] = platform_text.empty() ? std::string("-") : platform_text;
    values["wk.oth"] = std::to_string(elsewhere);
    values["wk.err"] = error_text;
}

std::string summary_line(unsigned round, long uptime, const Values &values) {
    auto get = [&](const char *key) { return value_of(values, key); };
    const std::string line = 'r' + std::to_string(round) + " sum @" + std::to_string(uptime) + " mg " + tally(mg_value_keys(), values, is_one) +
                             " pwr " + word(get("pwr.cap"), 5) + " c" + word(get("pwr.chg"), 2) + " hid " +
                             tally(hid_value_keys(), values, not_zero) + " io " + tally(io_value_keys(), values, not_zero) + " kc " +
                             get("kc.add") + "/" + get("kc.get") + "/" + get("kc.del") + " aud " + word(get("aud.out"), 16) + " sen " +
                             tally({"sen.acc", "sen.gyr", "sen.mag", "sen.dm"}, values, is_one) + " hap " + get("hap.hw") + " net " +
                             get("net.up") + " data " + get("data.free") + "/" + get("data.total") + "M wk " + get("wk.okn") + "/" + get("wk.n");
    return clip(line, kLineMax);
}

std::vector<DetailLine> detail_lines(const Values &values) {
    auto get = [&](const char *key) { return value_of(values, key); };
    std::vector<DetailLine> lines;
    // A group whose probe gave no answer has no line (its trouble is one, or it was left out): `answer` is one of its keys.
    auto add = [&](const std::string &key, const char *answer, const std::string &text, const std::string &same = std::string()) {
        if (value_of(values, answer) == "-") return;
        const std::string line = clip(text, kLineMax - 16);     // room for "r<round>@<uptime> "
        lines.push_back({key, line, same.empty() ? line : same});
    };
    for (const std::string &probe : round_probes()) {
        const std::string trouble = value_of(values, "t." + probe);
        if (trouble != "-") lines.push_back({"t." + probe, clip(trouble, kLineMax - 16), clip(trouble, kLineMax - 16)});
    }
    add("wk", "wk.n", "wk n" + get("wk.n") + " fs " + get("wk.fs") + " reg " + get("wk.reg") + " ok " + get("wk.ok") + " ep " + get("wk.ep") + " pf " +
                  get("wk.pf") + " oth " + get("wk.oth") + " err " + get("wk.err") + " ls " + get("wk.ls") + " pt " + get("wk.pt"));
    add("wkv", "wk.cap", "wkv cap " + get("wk.cap") + " fg " + get("wk.fg") + "/" + get("wk.fgr") + " fid " + get("wk.fid") + " chr " + get("wk.chr") +
                  " ext " + get("wk.ext") + " wid " + get("wk.wid"));
    add("kc", "kc.add", "kc add " + get("kc.add") + " get " + get("kc.get") + " del " + get("kc.del") + " match " + get("kc.match"));
    add("pwr", "pwr.n", "pwr n" + get("pwr.n") + " " + word(get("pwr.type"), 20) + " " + get("pwr.cap") + "/" + get("pwr.max") + " chg" +
                   get("pwr.chg") + " " + word(get("pwr.state"), 16) + " src " + word(get("pwr.src"), 16));
    std::string hid = "hid";
    for (const auto &pair : hid_pairs()) {
        const std::string key = hid_key(pair.first, pair.second);
        hid += ' ' + key + ':' + value_of(values, "hid." + key);
    }
    add("hid", "hid.all", hid + " all " + get("hid.all"));
    std::string io = "io";
    for (const auto &item : io_classes()) io += ' ' + item.second + ':' + value_of(values, "io." + item.second);
    add("io", ("io." + io_classes().front().second).c_str(), io);
    std::string mg = "mg";
    for (const auto &key : mg_keys()) mg += ' ' + key.second + value_of(values, "mg." + key.second);
    add("mg", ("mg." + mg_keys().back().second).c_str(), mg);
    add("aud", "aud.out", "aud out " + word(get("aud.out"), 40) + " in " + word(get("aud.in"), 40) + " vol " + get("aud.vol"));
    add("sen", value_of(values, "sen.acc") == "-" ? "hap.hw" : "sen.acc", "sen acc" + get("sen.acc") + " gyr" + get("sen.gyr") + " mag" + get("sen.mag") + " dm" + get("sen.dm") + " hap " +
                   get("hap.hw") + "/" + get("hap.aud"));
    add("net", "net.list", "net " + get("net.list"));
    add("time", "time.local", "time " + get("time.local") + " tz " + word(get("time.tz"), 40), get("time.tz"));   // the kernel logs the clock itself
    return lines;
}

void Budget::spend(size_t text) {
    if (lines_left_) --lines_left_;
    const size_t bytes = cost(text);
    bytes_left_ = bytes >= bytes_left_ ? 0 : unsigned(bytes_left_ - bytes);
}

Rounds::Rounds(const Config &config) : budget_(config.budget_lines, config.budget_bytes), rounds_(config.rounds.size()) {}

// What the rest of the boot needs after this round's details: the later summaries and the end line, at their longest.
size_t Rounds::reserve(unsigned round) const {
    const size_t later = rounds_ > round ? rounds_ - round : 0;
    return (later + 1) * Budget::cost(kLineMax);
}

std::vector<std::string> Rounds::lines(unsigned round, long uptime, const Values &values) {
    std::vector<std::string> out;
    const std::string summary = summary_line(round, uptime, values);
    // A summary always fits: the budget keeps room for it. With a budget set too small it is left out rather than the kernel refusing it.
    if (budget_.fits(summary.size())) {
        budget_.spend(summary.size());
        out.push_back(summary);
    } else {
        ++cut_;
    }
    const std::string head = 'r' + std::to_string(round) + '@' + std::to_string(uptime) + ' ';
    for (const DetailLine &detail : detail_lines(values)) {
        const auto seen = last_.find(detail.key);
        if (seen != last_.end() && seen->second == detail.same) continue;
        const std::string line = clip(head + detail.text, kLineMax);
        // Lines outside the budget are written on a later round if they still differ.
        if (budget_.lines_left() <= rounds_ - std::min<size_t>(rounds_, round) + 1 || !budget_.fits(line.size(), reserve(round))) {
            ++cut_;
            continue;
        }
        budget_.spend(line.size());
        last_[detail.key] = detail.same;
        out.push_back(line);
    }
    return out;
}

std::string abbreviate_id(const std::string &id) {
    std::string name = id.compare(0, 10, "com.apple.") == 0 ? id.substr(10) : id;
    name = ascii(name);
    for (char &c : name)
        if (c == ' ' || c == ',') c = '_';
    return name.substr(0, 12);
}

std::string pass_line(const PassFacts &f) {
    std::string line = 'p' + std::to_string(f.pass) + " @" + std::to_string(f.uptime) + " st ";
    if (f.file_bytes == -1) line += '-';
    else if (f.file_bytes == -2) line += "unreadable";
    else if (f.file_bytes < 0) line += "bad";
    else line += std::to_string(f.file_bytes) + "b n" + std::to_string(f.file.size()) + state_counts(f.file);
    line += " lk ";
    if (f.lookup == 1) line += 'n' + std::to_string(f.lookup_states.size()) + state_counts(f.lookup_states);
    else line += f.lookup == -1 ? "nil" : f.lookup == -2 ? "fail" : "-";
    line += " mig ";
    line += f.migration == -2 ? std::string("e") : f.migration < 0 ? std::string("-") : std::to_string(f.migration);
    line += " bun " + (f.bundles < 0 ? std::string("-") : std::to_string(f.bundles));
    line += " stg " + (f.staged < 0 ? std::string("-") : std::to_string(f.staged));
    line += " sen " + (f.sentinel < 0 ? std::string("-") : std::to_string(f.sentinel));
    line += " lg " + (f.log.empty() ? std::string("-") : f.log);
    if (f.kernel_dropped) line += " kd " + std::to_string(f.kernel_dropped);
    return clip(line, kLineMax);
}

std::string not1_line(const PassFacts &f) {
    std::map<long, std::vector<std::string>> groups;
    for (const auto &entry : f.file)
        if (entry.second != 1) groups[entry.second].push_back(entry.first);
    if (groups.empty()) return std::string();
    std::vector<std::string> tokens;     // "<state>:<id>" for a group's first app, "<id>" for the others
    for (auto &group : groups) {
        std::sort(group.second.begin(), group.second.end());
        for (size_t i = 0; i < group.second.size(); ++i)
            tokens.push_back((i ? std::string() : std::to_string(group.first) + ':') + abbreviate_id(group.second[i]));
    }
    std::string line = 'p' + std::to_string(f.pass) + " not1";
    size_t used = 0;
    for (; used < tokens.size(); ++used) {
        const bool first_of_group = tokens[used].find(':') != std::string::npos;
        const std::string piece = (first_of_group ? " " : ",") + tokens[used];
        const size_t reserve = used + 1 < tokens.size() ? 5 : 0;      // "+NNN"
        if (line.size() + piece.size() + reserve > kLineMax) break;
        line += piece;
    }
    if (used < tokens.size()) line += " +" + std::to_string(tokens.size() - used);
    return clip(line, kLineMax);
}

}  // namespace pagani_diag
