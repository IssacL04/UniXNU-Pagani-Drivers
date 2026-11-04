// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// PaganiDiag core: what the diagnostic daemon logs and how much, as plain C++ without device APIs, so the host test (Test/test_core.cpp)
// runs the same code. The daemon (PaganiDiag.mm) collects; this decides the lines: the feature rounds (a summary and the details that
// changed, within a strict budget, because the 32 KiB events ring is shared with every driver) and, with migration=1, the passes of the
// system app migration (facts, one summary per pass, the unified-log entries: filter, compaction, de-duplication, rationing). Every
// line is ASCII without control characters and at most kLineMax characters; the kernel prefixes it with "diag: ".
#ifndef PAGANI_DIAG_CORE_H
#define PAGANI_DIAG_CORE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pagani_diag {

constexpr size_t kLineMax = 170;        // a line as the daemon writes it (a log line: tag, level, time, message)
constexpr size_t kKernelText = 194;     // what the kernel keeps after its "diag: " prefix (one 200-character BootProgress record)
constexpr const char *kSelf = "paganidiag";
constexpr size_t kRecordBytes = 34;     // a record beside its text in the events ring (PAGANI_DIAG_RECORD_BYTES)
constexpr size_t kPrefixBytes = 6;      // "diag: "
constexpr unsigned kBudgetLines = 44;   // the daemon's own budget, inside the kernel's (PAGANI_DIAG_BOOT_LINES, PAGANI_DIAG_BOOT_BYTES)
constexpr unsigned kBudgetBytes = 5120;
constexpr size_t kMaxRounds = 8;

struct Config {
    std::vector<unsigned> rounds = {60, 180, 600};      // the feature rounds, seconds since boot
    std::vector<std::string> skip;                      // probes of a round left out (round_probes)
    unsigned budget_lines = kBudgetLines;               // the lines of a boot, start and end included
    unsigned budget_bytes = kBudgetBytes;               // and what they may cost in the events ring
    bool migration = false;             // also the passes of the system app migration; they need boot-arg pagani-diag=2 (the long budget)
    // The migration passes:
    unsigned minutes = 10;              // how long they run
    unsigned period = 30;               // seconds from one pass to the next
    unsigned pass_lines = 40;           // unified-log lines per pass
    unsigned boot_lines = 200;          // unified-log lines per boot
    unsigned notice_lines = 80;         // of those at most this many installd/installcoordinationd notices (class 1)
    unsigned info_lines = 40;           // ... and this many other migration lines (class 2)
    unsigned facts_every = 8;           // the facts again every this many passes (passes 1, 9, 17); 0: pass 1 only
    unsigned gap_ms = 550;              // at least this long between two lines: never more than 19 in any 10 s (the kernel takes 20)
    bool log = true;                    // read the unified log
    bool log_preference = true;         // create the logging preference that keeps the migration's info lines when the seed has none
};

// "key=value" lines ('#' starts a comment, blanks around '=' are ignored). Unknown keys and bad values are named in *bad (space separated)
// and leave the default.
void parse_config(const std::string &text, Config &config, std::string *bad);

// OSLogEntryLogLevel.
enum Level { kUndefined = 0, kDebug = 1, kInfo = 2, kNotice = 3, kError = 4, kFault = 5 };

// One unified-log entry; time in seconds since 1970.
struct Entry {
    double time = 0;
    int level = kUndefined;
    int pid = 0;
    std::string process, subsystem, message;
};

// The log probe's line for an entry: "L<TAB>time<TAB>level<TAB>pid<TAB>process<TAB>subsystem<TAB>message", tabs and line breaks of the
// fields as spaces, the message cut at 600 bytes.
std::string entry_line(const Entry &entry);
// The reverse; false for any other line.
bool parse_entry(const std::string &line, Entry &entry);

// Which entries become lines, and their class. The four processes are com.apple.migrationpluginwrapper, com.apple.datamigrator,
// installd and installcoordinationd. 0: a "SystemApp" line (MISystemAppMigrator, SystemAppState, ...; not "SystemAppearance", which
// every UIKit process logs) of the four processes or of the com.apple.migration subsystem, and the four processes' errors and faults;
// 1: installd's and installcoordinationd's other notices; 2: the migration processes' other notices, every other com.apple.migration
// line (info included), and the "SystemApp" lines of any other process at notice or above. -1, not logged: debug and undefined levels,
// other processes otherwise, info outside com.apple.migration, and messages that name `self` (installd's notes about this daemon's own
// requests).
// Info lines are taken because DataMigration's _DMLogFunc logs every level as OS_LOG_TYPE_INFO (its __CPLog_os_trace_type_map is
// eight 0x01 bytes in 21H461), so a notice-or-above filter would see none of SystemAppMigrator's lines.
int classify(const Entry &entry, const std::string &self = kSelf);

// Short tag of a process: mpw com.apple.migrationpluginwrapper, dm com.apple.datamigrator, ind installd, icd installcoordinationd; else
// the name without "com.apple.", at most 12 characters.
std::string process_tag(const std::string &process);
// I, N, E or F; '?' for anything else.
char level_letter(int level);

// A message made short and safe: control characters as spaces, a UTF-8 sequence or other non-ASCII byte as one '?', blanks collapsed,
// a leading thread address ("0x16f8e3000 ", MobileInstallation's) dropped, a leading Objective-C method "-[Class selector:...]: "
// as "Class.selector: " (at most 32 and 40 characters), "MISystemAppMigrator: " as "SAM: ", every "com.apple." removed; at most
// max characters, the last one '~' when cut.
std::string compact(const std::string &message, size_t max);

// "<tag> <L>@<seconds since boot> <compact message>", at most kLineMax characters.
std::string format_entry(const Entry &entry, double boot_time);
// The de-duplication key of an entry: tag, level and compact message without the time.
std::string entry_key(const Entry &entry);

struct PassResult {
    std::vector<std::string> lines;     // in time order
    unsigned candidates = 0;            // entries that classify() took
    unsigned duplicates = 0;            // of those, already logged earlier this boot
    unsigned suppressed = 0;            // left out by the pass, boot or class limits
};

// The unified-log lines of a boot. Each pass takes the new entries; a line is logged once a boot; class 0 goes first, then class 1,
// then 2 (within a class the older first), until pass_lines this pass, boot_lines this boot or the class limit; the chosen lines are
// written in time order.
class Rationer {
public:
    explicit Rationer(const Config &config, std::string self = kSelf);
    PassResult pass(const std::vector<Entry> &entries, double boot_time);
    unsigned logged() const { return logged_; }
    unsigned suppressed() const { return suppressed_; }
    unsigned duplicates() const { return duplicates_; }

private:
    Config config_;
    std::string self_;
    std::unordered_set<std::string> seen_;
    unsigned logged_ = 0, suppressed_ = 0, duplicates_ = 0;
    unsigned class_logged_[3] = {0, 0, 0};
};

// "suppressed S dup D" for a pass that left lines out or met repeats; "" otherwise.
std::string suppressed_line(const PassResult &result);

// Spacing of the lines for the kernel's limit (at most 20 lines in any 10 s): the earliest time (seconds, any monotonic clock) the next
// line may be written.
class Pacer {
public:
    explicit Pacer(unsigned gap_ms) : gap_(gap_ms / 1000.0) {}
    double next(double now) const { return have_ && last_ + gap_ > now ? last_ + gap_ : now; }
    void sent(double now) { last_ = now; have_ = true; }

private:
    double gap_;
    double last_ = 0;
    bool have_ = false;
};

// What the kernel logs of a line: up to the first line break, at most kKernelText characters, control and non-ASCII bytes as spaces.
std::string kernel_text(const std::string &line);

// The facts probe's answers (key -> value, see PaganiDiag.mm probe_facts) as the facts lines, in a fixed order, each at most kLineMax
// characters; a missing answer shows as "-".
std::vector<std::string> facts_lines(const std::map<std::string, std::string> &values);

// ---- the feature rounds ---------------------------------------------------------------------------------------------------------

using Values = std::map<std::string, std::string>;  // the probes' answers (PaganiDiag.mm); a missing one shows as "-"

// The MobileGestalt answers of a round: (key, tag) in line order.
const std::vector<std::pair<std::string, std::string>> &mg_keys();
// The IOKit classes counted: (class, tag) in line order.
const std::vector<std::pair<std::string, std::string>> &io_classes();
// The HID services counted, by PrimaryUsagePage and PrimaryUsage, in line order.
const std::vector<std::pair<unsigned, unsigned>> &hid_pairs();
// "ff00/3": a usage pair in the keys of Values ("hid.<pair>") and in the lines.
std::string hid_key(unsigned page, unsigned usage);
// The child probes of a round, each `paganidiag --probe <name>`.
const std::vector<std::string> &round_probes();

// One WidgetKit extension in an app bundle and what LaunchServices has for it: the widget census (probe wid). Every link of the chain a
// widget needs is one count, so the log says where it breaks: on disk, registered at all, valid as chronod asks it (requireValid), with
// WidgetKit's extension point, on which platform, and at this bundle or at another copy of it (/System/Library/AppPlaceholders holds a
// placeholder with the same identifiers).
struct WidgetEntry {
    bool in_view = false;               // on the bind mount over /Applications (the system apps view), not on the root volume
    bool registered = false;            // LSApplicationExtensionRecord with requireValid NO
    bool valid = false;                 // ... with requireValid YES, what chronod asks before it lists an extension
    bool point = false;                 // the record's extension point is WidgetKit's
    bool elsewhere = false;             // the record's URL is not this bundle
    long platform = -1;                 // LSBundleRecord.platform of the record; -1: no record
    long error = 0;                     // the error code of the requireValid YES look-up
};
// The census as the wk.* answers of the round: wk.n, wk.fs, wk.reg, wk.ok, wk.ep ("<view>/<root>"; wk.ok also as wk.okn, all of them), wk.pf
// ("<platform>:<count>..."), wk.oth (records elsewhere), wk.err (the most common error, "<code>x<count>").
void census_values(const std::vector<WidgetEntry> &entries, Values &values);
// A process's path is a widget extension: "Widget" and no "Intent" below the "/PlugIns/" of an ".appex/" (kernel/pagani_diag.h pagani_diag_extension
// counts the execs the same way; the six intents services named after a widget are no widgets).
bool widget_extension_path(const char *path);

// "r<round> sum @<uptime> mg <yes>/<all> pwr <percent> c<charging> hid <found>/<all> io <found>/<all> kc <add>/<get>/<del> aud <output>
// sen <yes>/<all> hap <hw> net <up> data <free>/<total>M wk <valid>/<all>": everything in one line; the kernel keeps the last one and
// logs it again on its later rounds, so it must stay short.
std::string summary_line(unsigned round, long uptime, const Values &values);

// A line of a round beside the summary. `text` has no "r<round>@<uptime> " head yet; it is written again only when `same` changed.
struct DetailLine {
    std::string key, text, same;
};
// In order of importance: a probe's trouble, the widget census, keychain, power, HID services, IOKit services, MobileGestalt, audio,
// sensors and haptics, interfaces, clock.
std::vector<DetailLine> detail_lines(const Values &values);

// The budget of a boot, in lines and in what they cost in the events ring. Stricter than the kernel's, which stops the daemon for
// certain (over it a write fails): this one decides what is left out.
class Budget {
public:
    Budget(unsigned lines, unsigned bytes) : lines_left_(lines), bytes_left_(bytes) {}
    static size_t cost(size_t text) { return kRecordBytes + kPrefixBytes + text; }
    // Whether a line of `text` characters fits with `reserve` bytes left over.
    bool fits(size_t text, size_t reserve = 0) const { return lines_left_ > 0 && cost(text) + reserve <= bytes_left_; }
    void spend(size_t text);
    unsigned lines_left() const { return lines_left_; }
    unsigned bytes_left() const { return bytes_left_; }

private:
    unsigned lines_left_, bytes_left_;
};

// The lines of the rounds of a boot: the summary always (the budget keeps room for every remaining summary and the end line), then the
// details of the first round and, on the later ones, those that changed, as far as the budget goes.
class Rounds {
public:
    explicit Rounds(const Config &config);
    std::vector<std::string> lines(unsigned round, long uptime, const Values &values);
    // A line outside the rounds (the start line) that comes out of the same budget.
    void spend(const std::string &line) { budget_.spend(line.size()); }
    unsigned cut() const { return cut_; }               // details left out for the budget
    const Budget &budget() const { return budget_; }

private:
    size_t reserve(unsigned round) const;

    Budget budget_;
    size_t rounds_;
    std::map<std::string, std::string> last_;
    unsigned cut_ = 0;
};

// ---- the migration passes ------------------------------------------------------------------------------------------------------

// One pass of the state summary.
struct PassFacts {
    unsigned pass = 0;
    long uptime = 0;                                    // seconds since boot
    long file_bytes = -1;                               // SystemAppInstallState.plist: -1 absent, -2 unreadable, -3 not a dictionary
    std::vector<std::pair<std::string, long>> file;     // its bundle ID -> state (a value that is no number: -1)
    int lookup = 0;                                     // MobileInstallationLookupSystemAppState: 1 answered, 0 not asked, -1 nil, -2 probe failed
    std::vector<std::pair<std::string, long>> lookup_states;
    int migration = -1;                                 // MobileInstallationGetSystemAppMigrationStatus: 1 complete, 0 not, -1 unknown, -2 error
    long bundles = -1;                                  // entries in /private/var/containers/Bundle/Application (-1 absent)
    long staged = -1;                                   // .app entries in /private/var/staged_system_apps (-1 absent)
    int sentinel = -1;                                  // /private/var/datamigrator/migrate exists
    std::string log;                                    // the previous log pass: "<entries>/<kept>/<ms>ms", "" before the first
    unsigned kernel_dropped = 0;                        // kern.pagani_diag_dropped
};

// "p<N> @<s> st <bytes>b n<count> <state>:<count>... lk ... mig <m> bun <n> stg <n> sen <s> lg <...>[ kd <n>]".
std::string pass_line(const PassFacts &facts);
// "p<N> not1 <state>:<id>,<id>...[+<more>]" for the apps of the state file in a state other than 1, "" when there are none.
std::string not1_line(const PassFacts &facts);
// A bundle ID as the lines show it: without "com.apple.", at most 12 characters.
std::string abbreviate_id(const std::string &id);

}  // namespace pagani_diag

#endif
