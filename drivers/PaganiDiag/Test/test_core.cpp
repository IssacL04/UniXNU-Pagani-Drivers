// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the PaganiDiag core (DiagCore.cpp): configuration, the probe's entry lines, the filter, compaction and line format,
// de-duplication and rationing over a boot, the pacing against the kernel's own limit (kernel/pagani_diag.h), and the fact and pass
// lines. Run by Test/run.sh with ASan and UBSan.
#include "DiagCore.h"

extern "C" {
#include "../kernel/pagani_diag.h"
}

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace pagani_diag;

static int failures = 0, checks = 0;
#define CHECK(condition)                                                                                \
    do {                                                                                                \
        ++checks;                                                                                       \
        if (!(condition)) {                                                                             \
            ++failures;                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);               \
        }                                                                                               \
    } while (0)
#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        ++checks;                                                                                       \
        const std::string left_ = (a), right_ = (b);                                                   \
        if (left_ != right_) {                                                                          \
            ++failures;                                                                                 \
            fprintf(stderr, "%s:%d: CHECK_EQ failed:\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__,   \
                    left_.c_str(), right_.c_str());                                                     \
        }                                                                                               \
    } while (0)

static bool clean(const std::string &line, size_t max) {
    if (line.size() > max) return false;
    for (unsigned char c : line)
        if (c < 0x20 || c >= 0x7f) return false;
    return true;
}

static Entry entry(double time, int level, const char *process, const char *subsystem, const std::string &message, int pid = 100) {
    Entry e;
    e.time = time;
    e.level = level;
    e.pid = pid;
    e.process = process;
    e.subsystem = subsystem;
    e.message = message;
    return e;
}

static void test_config() {
    Config c;
    std::string bad;
    parse_config("", c, &bad);
    CHECK(bad.empty());
    CHECK(c.minutes == 10 && c.period == 30 && c.pass_lines == 40 && c.boot_lines == 200 && c.gap_ms == 550 && c.log &&
          c.log_preference && c.facts_every == 8);
    CHECK(c.rounds == (std::vector<unsigned>{60, 180, 600}) && !c.migration && c.budget_lines == kBudgetLines && c.budget_bytes == kBudgetBytes);
    parse_config("# comment\n minutes = 3 \nperiod=60\npass_lines=10 # trailing\nboot_lines=120\nlog=0\nlogpref=0\n"
                 "notice_lines=5\ninfo_lines=6\nfacts_every=0\ngap_ms=1000\n",
                 c, &bad);
    CHECK(bad.empty());
    CHECK(c.minutes == 3 && c.period == 60 && c.pass_lines == 10 && c.boot_lines == 120 && !c.log && !c.log_preference &&
          c.notice_lines == 5 && c.info_lines == 6 && c.facts_every == 0 && c.gap_ms == 1000);
    Config d;
    bad.clear();
    parse_config("period=1\nminutes=-1\nunknown=4\ngap_ms=50\nlog=2\nnoequals\nboot_lines=12x\n", d, &bad);
    CHECK_EQ(bad, "period minutes unknown gap_ms log noequals boot_lines");
    CHECK(d.period == 30 && d.minutes == 10 && d.gap_ms == 550 && d.log && d.boot_lines == 200);
    Config r;
    bad.clear();
    parse_config("rounds=30,90\nbudget_lines=20\nbudget_bytes=2048\nmigration=1\n", r, &bad);
    CHECK(bad.empty());
    CHECK(r.rounds == (std::vector<unsigned>{30, 90}) && r.budget_lines == 20 && r.budget_bytes == 2048 && r.migration);
    parse_config("rounds=none\nskip=kc,aud,wid\n", r, &bad);
    CHECK(bad.empty() && r.rounds.empty() && r.skip == (std::vector<std::string>{"kc", "aud", "wid"}));
    parse_config("skip=none\n", r, &bad);
    CHECK(bad.empty() && r.skip.empty());
    Config e;
    bad.clear();
    parse_config("rounds=90,30\nrounds=60,\nrounds=0\nrounds=1,2,3,4,5,6,7,8,9\nrounds=abc\nrounds=\nrounds=3601\nbudget_lines=1\nbudget_bytes=99\nmigration=2\nskip=kc,x\nskip=\n", e, &bad);
    CHECK_EQ(bad, "rounds rounds rounds rounds rounds rounds rounds budget_lines budget_bytes migration skip skip");
    CHECK(e.rounds == (std::vector<unsigned>{60, 180, 600}) && e.budget_lines == kBudgetLines && !e.migration && e.skip.empty());
}

static void test_entry_lines() {
    Entry e = entry(1730000123.25, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration",
                    "MISystemAppMigrator: a\tb\nc", 321);
    const std::string line = entry_line(e);
    CHECK(line.find('\n') == std::string::npos);
    Entry back;
    CHECK(parse_entry(line, back));
    CHECK(back.level == kInfo && back.pid == 321 && back.process == e.process && back.subsystem == e.subsystem);
    CHECK(back.time > 1730000123.249 && back.time < 1730000123.251);
    CHECK_EQ(back.message, "MISystemAppMigrator: a b c");
    Entry empty = entry(5, kError, "installd", "", "");
    CHECK(parse_entry(entry_line(empty), back) && back.subsystem.empty() && back.message.empty());
    const std::string long_message(2000, 'x');
    CHECK(parse_entry(entry_line(entry(5, kError, "installd", "", long_message)), back) && back.message.size() == 600);
    for (const char *bad : {"", "L", "P\t1", "L\tx\t3\t1\tp\ts\tm", "L\t1\t9\t1\tp\ts\tm", "L\t1\t3\t1\tp", "L\t\t3\t1\tp\ts\tm"})
        CHECK(!parse_entry(bad, back));
}

static void test_classify() {
    const char *mig = "com.apple.migration";
    CHECK(classify(entry(1, kInfo, "com.apple.migrationpluginwrapper", mig, "MISystemAppMigrator: x is currently NOT installed")) == 0);
    CHECK(classify(entry(1, kInfo, "com.apple.datamigrator", mig, "Data migrator completed with success: YES!")) == 2);
    CHECK(classify(entry(1, kInfo, "com.apple.migrationpluginwrapper", "", "plain info")) == -1);
    CHECK(classify(entry(1, kNotice, "com.apple.migrationpluginwrapper", "", "plain notice")) == 2);
    CHECK(classify(entry(1, kError, "com.apple.datamigrator", "", "an error")) == 0);
    CHECK(classify(entry(1, kFault, "installcoordinationd", "com.apple.installcoordination", "a fault")) == 0);
    CHECK(classify(entry(1, kNotice, "installd", "", "0x16f8e3000 -[MIInstaller performInstallationWithError:]: done")) == 1);
    CHECK(classify(entry(1, kInfo, "installd", "", "an info")) == -1);
    CHECK(classify(entry(1, kInfo, "installd", "", "UpdateSystemAppState info")) == 0);
    CHECK(classify(entry(1, kDebug, "com.apple.migrationpluginwrapper", mig, "MISystemAppMigrator: debug")) == -1);
    CHECK(classify(entry(1, kUndefined, "installd", "", "x")) == -1);
    CHECK(classify(entry(1, kNotice, "SpringBoard", "", "SystemAppPlaceholder missing")) == 2);
    CHECK(classify(entry(1, kInfo, "SpringBoard", "", "SystemApp info elsewhere")) == -1);
    CHECK(classify(entry(1, kError, "SpringBoard", "", "unrelated error")) == -1);
    CHECK(classify(entry(1, kInfo, "lsd", mig, "SystemApp in the migration subsystem")) == 0);
    // "SystemAppearance" (NSRequiresAquaSystemAppearance, logged by every app) is not a system app line; a later real one still is.
    CHECK(classify(entry(1, kNotice, "Shottr", "", "NSApp cache appearance: -NSRequiresAquaSystemAppearance: 0")) == -1);
    CHECK(classify(entry(1, kNotice, "Shottr", "", "SystemAppearance then SystemAppState")) == 2);
    CHECK(classify(entry(1, kNotice, "Shottr", "", "ends with SystemApp")) == 2);
    CHECK(classify(entry(1, kInfo, "SpringBoard", mig, "DMIsMigrationNeeded called by process: SpringBoard[60]")) == 2);
    CHECK(classify(entry(1, kInfo, "SpringBoard", "com.apple.springboard", "plain info")) == -1);
    CHECK(classify(entry(1, kNotice, "installd", "", "UseSystemAppearance")) == 1);
    // installd's notes about this daemon's own requests.
    CHECK(classify(entry(1, kNotice, "installd", "",
                         "0x16f8e3000 -[MIClientConnection lookupSystemAppStateWithOptions:completion:]: Lookup system app state "
                         "requested by paganidiag (pid 87) with options (null)")) == -1);
    CHECK(classify(entry(1, kNotice, "installd", "", "requested by someone"), "") == 1);
}

static void test_compact_and_format() {
    CHECK_EQ(compact("0x16f8e3000 -[MIInstaller performInstallationWithError:]: Install Successful for (Placeholder:com.apple.tv)", 170),
             "MIInstaller.performInstallationWithError: Install Successful for (Placeholder:tv)");
    CHECK_EQ(compact("MISystemAppMigrator: com.apple.mobilecal is currently NOT installed. Desired app state is 2", 170),
             "SAM: mobilecal is currently NOT installed. Desired app state is 2");
    CHECK_EQ(compact("+[MISystemAppState _systemAppStateFromURL:]: Failed", 170), "MISystemAppState._systemAppStateFromURL: Failed");
    CHECK_EQ(compact("-[A b]", 170), "A.b");
    CHECK_EQ(compact("-[NoClose sel: never", 170), "-[NoClose sel: never");
    CHECK_EQ(compact("  tabs\tand\n\nlines  \r", 170), "tabs and lines");
    CHECK_EQ(compact("caf\xc3\xa9 \xe2\x9c\x93 \x80x \x01\x7f", 170), "caf? ? ?x");
    CHECK_EQ(compact("0x12 short pointer", 170), "0x12 short pointer");
    CHECK_EQ(compact("abcdefghij", 5), "abcd~");
    CHECK_EQ(compact("abcde", 5), "abcde");
    CHECK_EQ(compact("abc", 0), "");
    const std::string very_long = "-[" + std::string(80, 'C') + " " + std::string(80, 's') + ":x:]: tail";
    const std::string shortened = compact(very_long, 170);
    CHECK(shortened.size() == 32 + 1 + 40 + 6 && shortened.find("tail") != std::string::npos);

    CHECK_EQ(process_tag("com.apple.migrationpluginwrapper"), "mpw");
    CHECK_EQ(process_tag("com.apple.datamigrator"), "dm");
    CHECK_EQ(process_tag("datamigrator"), "dm");
    CHECK_EQ(process_tag("installd"), "ind");
    CHECK_EQ(process_tag("installcoordinationd"), "icd");
    CHECK_EQ(process_tag("com.apple.WebKit.Networking"), "WebKit.Netwo");
    CHECK_EQ(process_tag("my proc"), "my_proc");
    CHECK_EQ(process_tag(""), "?");
    CHECK(level_letter(kInfo) == 'I' && level_letter(kNotice) == 'N' && level_letter(kError) == 'E' && level_letter(kFault) == 'F' &&
          level_letter(kDebug) == '?');

    const double boot = 1730000000;
    CHECK_EQ(format_entry(entry(boot + 312.9, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration",
                                "MISystemAppMigrator: Skipping installation of com.apple.weather because it is already uninstalled"),
                          boot),
             "mpw I@312 SAM: Skipping installation of weather because it is already uninstalled");
    CHECK_EQ(format_entry(entry(boot - 5, kError, "installd", "", "x"), boot), "ind E@0 x");
    const std::string huge = format_entry(entry(boot + 1, kNotice, "installd", "", std::string(1000, 'm')), boot);
    CHECK(huge.size() == kLineMax && huge.back() == '~' && clean(huge, kLineMax));
    CHECK_EQ(entry_key(entry(boot + 1, kNotice, "installd", "", "0xabcdef12 same")), entry_key(entry(boot + 99, kNotice, "installd", "",
                                                                                                    "0x12345678 same")));
}

static std::vector<Entry> many(int count, int level, const char *process, const char *subsystem, const char *text, double at) {
    std::vector<Entry> out;
    for (int i = 0; i < count; ++i) out.push_back(entry(at + i, level, process, subsystem, std::string(text) + " " + std::to_string(i)));
    return out;
}

static void test_rationer() {
    const double boot = 1000;
    Config config;
    Rationer r(config);
    // Pass 1: 30 migration info lines (class 2), 3 installd notices (class 1), 2 SystemApp lines (class 0), noise, a repeat.
    std::vector<Entry> one = many(30, kInfo, "com.apple.datamigrator", "com.apple.migration", "plugin step", 1100);
    for (const Entry &e : many(3, kNotice, "installd", "", "0x16f8e3000 -[MIClientConnection installURL:]: note", 1200)) one.push_back(e);
    one.push_back(entry(1300, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration", "MISystemAppMigrator: A"));
    one.push_back(entry(1050, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration", "MISystemAppMigrator: B"));
    one.push_back(entry(1301, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration", "MISystemAppMigrator: A"));
    one.push_back(entry(1302, kDebug, "installd", "", "debug noise"));
    one.push_back(entry(1303, kNotice, "backboardd", "", "unrelated"));
    PassResult p1 = r.pass(one, boot);
    CHECK(p1.candidates == 36 && p1.duplicates == 1);
    CHECK(p1.lines.size() == 35 && p1.suppressed == 0);
    // Time order in the output, whatever the class.
    CHECK_EQ(p1.lines.front(), "mpw I@50 SAM: B");
    CHECK_EQ(p1.lines.back(), "mpw I@300 SAM: A");
    for (size_t i = 1; i < p1.lines.size(); ++i) CHECK(clean(p1.lines[i], kLineMax));
    CHECK(suppressed_line(p1).empty());
    // Pass 2: the same SystemApp line again is a duplicate; class 2 is capped at 40 a boot (30 used): 10 of 20 new ones get in.
    std::vector<Entry> two = many(20, kInfo, "com.apple.datamigrator", "com.apple.migration", "later step", 1400);
    two.push_back(entry(1500, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration", "MISystemAppMigrator: A"));
    PassResult p2 = r.pass(two, boot);
    CHECK(p2.lines.size() == 10 && p2.suppressed == 10 && p2.duplicates == 1);
    CHECK_EQ(suppressed_line(p2), "suppressed 10 dup 1");
    // Pass 3: 60 class 0 lines and 10 class 1 notices: 40 a pass, class 0 first.
    std::vector<Entry> three = many(60, kInfo, "com.apple.migrationpluginwrapper", "com.apple.migration", "MISystemAppMigrator: app", 1600);
    for (const Entry &e : many(10, kNotice, "installcoordinationd", "", "coordinator", 1590)) three.push_back(e);
    PassResult p3 = r.pass(three, boot);
    CHECK(p3.lines.size() == 40 && p3.suppressed == 30);
    for (const std::string &line : p3.lines) CHECK(line.find("SAM: app") != std::string::npos);
    CHECK(r.logged() == 35 + 10 + 40);
    // Suppressed lines were not marked seen: they get in on a later pass.
    PassResult p4 = r.pass(three, boot);
    CHECK(p4.duplicates == 40 && p4.lines.size() == 30);
    // The boot limit: 200 lines in all.
    for (int pass = 0; pass < 10; ++pass) {
        PassResult more = r.pass(many(40, kError, "installd", "", ("error " + std::to_string(pass)).c_str(), 2000 + pass * 100), boot);
        CHECK(more.lines.size() + more.suppressed == 40);
    }
    CHECK(r.logged() == 200);
    PassResult after = r.pass(many(5, kFault, "installd", "", "late fault", 9000), boot);
    CHECK(after.lines.empty() && after.suppressed == 5);
    // Limits from the configuration.
    Config small;
    small.pass_lines = 3;
    small.boot_lines = 4;
    Rationer s(small);
    CHECK(s.pass(many(10, kError, "installd", "", "e", 100), 0).lines.size() == 3);
    CHECK(s.pass(many(10, kError, "installd", "", "f", 200), 0).lines.size() == 1);
    CHECK(s.pass(many(10, kError, "installd", "", "g", 300), 0).lines.empty());
}

// The daemon's pacing never trips the kernel's window: every line of the long budget (pagani-diag=2) is taken, and the next is refused
// for the boot.
static void test_pacing_against_kernel() {
    Config config;
    Pacer pacer(config.gap_ms);
    struct pagani_diag_limit limit = {};
    pagani_diag_budget(&limit, 2);
    const uint64_t window = uint64_t(PAGANI_DIAG_WINDOW_SECONDS) * 1000000;    // microseconds
    double now = 0;
    for (int i = 0; i < PAGANI_DIAG_LONG_LINES; ++i) {
        now = pacer.next(now) + (i % 7 == 0 ? 0.0001 : 0);     // sleep jitter only makes gaps longer
        CHECK(pagani_diag_take(&limit, uint64_t(now * 1e6), window, 12) == PAGANI_DIAG_TAKEN);
        pacer.sent(now);
        if (i % 40 == 39) now += 3;     // the next pass
    }
    CHECK(limit.dropped == 0 && limit.lines == PAGANI_DIAG_LONG_LINES);
    CHECK(pagani_diag_take(&limit, uint64_t((now + 100) * 1e6), window, 12) == PAGANI_DIAG_OVER_BOOT && limit.dropped == 1);
    // Faster than the gap: the 21st line within 10 s is refused, the line 10 s after the first is taken again.
    struct pagani_diag_limit fast = {};
    pagani_diag_budget(&fast, 2);
    for (int i = 0; i < 20; ++i) CHECK(pagani_diag_take(&fast, uint64_t(i) * 100000, window, 12) == PAGANI_DIAG_TAKEN);
    CHECK(pagani_diag_take(&fast, 2000000, window, 12) == PAGANI_DIAG_OVER_WINDOW);
    CHECK(pagani_diag_take(&fast, window - 1, window, 12) == PAGANI_DIAG_OVER_WINDOW);
    CHECK(pagani_diag_take(&fast, window, window, 12) == PAGANI_DIAG_TAKEN);
    CHECK(pagani_diag_take(&fast, window + 1, window, 12) == PAGANI_DIAG_OVER_WINDOW);
    CHECK(pagani_diag_take(&fast, window + 100000, window, 12) == PAGANI_DIAG_TAKEN);
    CHECK(fast.dropped == 3 && fast.lines == 22);
    // At most 20 accepted lines in any window, whatever the arrival pattern.
    struct pagani_diag_limit random = {};
    pagani_diag_budget(&random, 2);
    std::vector<uint64_t> taken;
    uint64_t t = 0, seed = 12345;
    for (int i = 0; i < 2000 && random.lines < PAGANI_DIAG_BOOT_LINES; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        t += (seed >> 33) % 900000;
        if (pagani_diag_take(&random, t, window, 12) == PAGANI_DIAG_TAKEN) taken.push_back(t);
    }
    for (size_t i = PAGANI_DIAG_WINDOW_LINES; i < taken.size(); ++i) CHECK(taken[i] - taken[i - PAGANI_DIAG_WINDOW_LINES] >= window);
    CHECK(taken.size() == PAGANI_DIAG_BOOT_LINES);
}

static void test_kernel_text() {
    CHECK_EQ(kernel_text("abc\ndef"), "abc");
    CHECK_EQ(kernel_text("a\tb\x01" "c\xff"), "a b c ");
    CHECK(kernel_text(std::string(500, 'z')).size() == kKernelText);
    char line[PAGANI_DIAG_TEXT + 1];
    // The kernel's own line: prefix once, cut to 200, control bytes as spaces, an empty text refused.
    CHECK(pagani_diag_line(line, "hello", 5) == 11 && !strcmp(line, "diag: hello"));
    CHECK(pagani_diag_line(line, "diag: hello", 11) == 11 && !strcmp(line, "diag: hello"));
    CHECK(pagani_diag_line(line, "diag: ", 6) == 0 && line[0] == 0);
    CHECK(pagani_diag_line(line, "", 0) == 0);
    CHECK(pagani_diag_line(line, "\nx", 2) == 0);
    CHECK(pagani_diag_line(line, "a\tb\nc", 5) == 9 && !strcmp(line, "diag: a b"));
    CHECK(pagani_diag_line(line, "ab\0cd", 5) == 8 && !strcmp(line, "diag: ab"));
    const char unterminated[3] = {'x', 'y', 'z'};
    CHECK(pagani_diag_line(line, unterminated, 2) == 8 && !strcmp(line, "diag: xy"));
    std::string big(PAGANI_DIAG_TEXT, 'q');
    CHECK(pagani_diag_line(line, big.data(), big.size()) == PAGANI_DIAG_TEXT && strlen(line) == PAGANI_DIAG_TEXT);
    const std::string daemon_line = "diag: " + kernel_text(std::string(400, 'w'));
    CHECK(daemon_line.size() == PAGANI_DIAG_TEXT);
    std::string high = "\x80\xfe ok";
    CHECK(pagani_diag_line(line, high.data(), high.size()) == 11 && !strcmp(line, "diag:    ok"));
}

static void test_facts_lines() {
    std::map<std::string, std::string> values = {
        {"mg.SupportedDeviceFamilies", "[1]"}, {"mg.DeviceClassNumber", "1"}, {"mg.DeviceClass", "iPhone"},
        {"mg.ProductType", "iPhone16,1"}, {"mg.HWModelStr", "D83AP"}, {"mg.arm64", "1"}, {"hw.product", "iPhone16,1"},
        {"hw.target", "D83"}, {"hw.machine", "iPhone16,1"}, {"hw.model", "D83AP"}, {"arch", "100000c/1"}, {"caps", "match=1"},
        {"fam.skip", "0"}, {"fam.sam", "1"}, {"force", "arr 28 str 28"}, {"force.stat", "501:20 644"}, {"skip.mobile", "1"},
        {"skip.root", "-"}, {"logpref", "seed"}};
    std::vector<std::string> lines = facts_lines(values);
    CHECK(lines.size() == 4);
    CHECK_EQ(lines[0], "mg fam [1] dcn 1 dc iPhone pt iPhone16,1 hw D83AP arm64 1");
    CHECK_EQ(lines[1], "hw product iPhone16,1 target D83 machine iPhone16,1 model D83AP arch 100000c/1");
    CHECK_EQ(lines[2], "caps match=1 skip 0 fam 1");
    CHECK_EQ(lines[3], "force arr 28 str 28 own 501:20 644 skipfam mob 1 root - logpref seed");
    values["fam.sam"] = "0 e10 MIInstaller This app was not built to support this device family; app is compatible with ( 1, 2 ) but "
                        "this device supports ( ) and a lot more text that runs past the end of the line for sure";
    values.erase("caps");
    lines = facts_lines(values);
    CHECK(lines[2].compare(0, 24, "caps - skip 0 fam 0 e10 ") == 0 && lines[2].size() == kLineMax && lines[2].back() == '~');
    lines = facts_lines({});
    CHECK_EQ(lines[0], "mg fam - dcn - dc - pt - hw - arm64 -");
    for (const std::string &line : lines) CHECK(clean(line, kLineMax));
}

// The answers of a healthy round and of a phone that answers nothing.
static Values full_values() {
    Values v;
    for (const auto &key : mg_keys()) v["mg." + key.second] = "1";
    v["mg.clh"] = "0";
    v["pwr.n"] = "1";
    v["pwr.type"] = "InternalBattery";
    v["pwr.cap"] = "83";
    v["pwr.max"] = "100";
    v["pwr.chg"] = "0";
    v["pwr.state"] = "Battery Power";
    v["pwr.src"] = "Battery Power";
    for (const auto &pair : hid_pairs()) v["hid." + hid_key(pair.first, pair.second)] = "0";
    v["hid.b/2e"] = "1";
    v["hid.ff00/3"] = "1";
    v["hid.all"] = "5";
    for (const auto &item : io_classes()) v["io." + item.second] = "1";
    v["io.in"] = "2";
    v["io.wl"] = "0";
    v["kc.add"] = "0";
    v["kc.get"] = "0";
    v["kc.del"] = "0";
    v["kc.match"] = "1";
    v["aud.out"] = "Speaker";
    v["aud.in"] = "MicrophoneBuiltIn";
    v["aud.vol"] = "0.50";
    v["sen.acc"] = "1";
    v["sen.gyr"] = "1";
    v["sen.mag"] = "0";
    v["sen.dm"] = "1";
    v["hap.hw"] = "1";
    v["hap.aud"] = "1";
    v["net.up"] = "2";
    v["net.list"] = "lo0+ en0-";
    v["data.free"] = "1422";
    v["data.total"] = "2047";
    v["time.local"] = "2026-10-31 19:02:03 +0800";
    v["time.tz"] = "Asia/Shanghai";
    v["wk.n"] = "35";
    v["wk.fs"] = "25/10";
    v["wk.reg"] = "25/10";
    v["wk.ok"] = "0/10";
    v["wk.okn"] = "10";
    v["wk.ep"] = "0/10";
    v["wk.pf"] = "0:25 2:10";
    v["wk.oth"] = "0";
    v["wk.err"] = "-10814x25";
    v["wk.ls"] = "10";
    v["wk.pt"] = "2";
    v["wk.cap"] = "4001";
    v["wk.fg"] = "0";
    v["wk.fgr"] = "0";
    v["wk.fid"] = "1";
    v["wk.chr"] = "1";
    v["wk.ext"] = "3";
    v["wk.wid"] = "1";
    return v;
}

static void test_feature_lines() {
    CHECK_EQ(hid_key(0xff00, 0x0a), "ff00/a");
    CHECK_EQ(hid_key(0x0b, 0x2e), "b/2e");
    CHECK(mg_keys().size() == 14 && hid_pairs().size() == 7 && round_probes().size() == 8);
    const Values full = full_values();
    const std::string summary = summary_line(1, 61, full);
    CHECK_EQ(summary, "r1 sum @61 mg 13/14 pwr 83 c0 hid 2/7 io 8/9 kc 0/0/0 aud Speaker sen 3/4 hap 1 net 2 data 1422/2047M wk 10/35");
    CHECK(clean(summary, kLineMax));
    CHECK(pagani_diag_is_summary(summary.c_str(), summary.size()));     // the kernel keeps it and logs it again on its later rounds
    const std::string none = summary_line(2, 180, Values());
    CHECK_EQ(none, "r2 sum @180 mg - pwr - c- hid - io - kc -/-/- aud - sen - hap - net - data -/-M wk -/-");
    CHECK(pagani_diag_is_summary(none.c_str(), none.size()));
    const std::vector<DetailLine> details = detail_lines(full);
    std::map<std::string, std::string> text;
    for (const DetailLine &d : details) text[d.key] = d.text;
    CHECK(details.front().key == "wk" && details[1].key == "wkv" && details[2].key == "kc" && details.back().key == "time");
    CHECK_EQ(text["wk"], "wk n35 fs 25/10 reg 25/10 ok 0/10 ep 0/10 pf 0:25 2:10 oth 0 err -10814x25 ls 10 pt 2");
    CHECK_EQ(text["wkv"], "wkv cap 4001 fg 0/0 fid 1 chr 1 ext 3 wid 1");
    CHECK_EQ(text["kc"], "kc add 0 get 0 del 0 match 1");
    CHECK_EQ(text["pwr"], "pwr n1 InternalBattery 83/100 chg0 Battery_Power src Battery_Power");
    CHECK_EQ(text["hid"], "hid ff00/3:1 ff00/4:0 ff00/8:0 ff00/9:0 ff00/a:0 ff0c/3:0 b/2e:1 all 5");
    CHECK_EQ(text["io"], "io in:2 pan:1 aud:1 bat:1 sen:1 pwr:1 wl:0 aks:1 hid:1");
    CHECK_EQ(text["mg"], "mg bat1 hap1 clh0 wifi1 bt1 acc1 gyr1 mag1 als1 prx1 mic1 fla1 cam1 gps1");
    CHECK_EQ(text["aud"], "aud out Speaker in MicrophoneBuiltIn vol 0.50");
    CHECK_EQ(text["sen"], "sen acc1 gyr1 mag0 dm1 hap 1/1");
    CHECK_EQ(text["net"], "net lo0+ en0-");
    CHECK_EQ(text["time"], "time 2026-10-31 19:02:03 +0800 tz Asia/Shanghai");
    Values troubled = full;
    troubled["t.kc"] = "probe kc timeout";
    for (const char *key : {"kc.add", "kc.get", "kc.del", "kc.match", "sen.acc", "sen.gyr", "sen.mag", "sen.dm"}) troubled.erase(key);
    const std::vector<DetailLine> left = detail_lines(troubled);
    CHECK_EQ(left.front().text, "probe kc timeout");
    std::map<std::string, std::string> kept;
    for (const DetailLine &d : left) kept[d.key] = d.text;
    CHECK(kept.count("kc") == 0);                               // no answers, no line: its trouble says why
    CHECK_EQ(kept["sen"], "sen acc- gyr- mag- dm- hap 1/1");    // a line with one of its two probes
    CHECK(detail_lines(Values()).empty());
    // Long, hostile values stay within a line.
    Values wild = full;
    for (const char *key : {"pwr.type", "pwr.state", "pwr.src", "aud.out", "aud.in", "net.list", "time.tz", "time.local", "wk.pf", "wk.err", "wk.fg"})
        wild[key] = std::string(400, 'x') + "\t\n\xc3\xa9";
    wild["pwr.cap"] = wild["kc.add"] = wild["data.free"] = std::string(300, '9');
    CHECK(clean(summary_line(3, 4000000, wild), kLineMax));
    for (const DetailLine &d : detail_lines(wild)) CHECK(clean(d.text, kLineMax - 16));
}

static void test_widget_paths() {
    CHECK(widget_extension_path("/Applications/Weather.app/PlugIns/WeatherWidget.appex/WeatherWidget"));
    CHECK(widget_extension_path("/Applications/Music.app/PlugIns/MusicWidgets.appex/MusicWidgets"));
    CHECK(!widget_extension_path("/Applications/FindMy.app/PlugIns/FindMyWidgetIntentsItems.appex/FindMyWidgetIntentsItems"));
    CHECK(!widget_extension_path("/Applications/Home.app/PlugIns/HomeWidgetSingleAccessoryIntent.appex/HomeWidgetSingleAccessoryIntent"));
    CHECK(!widget_extension_path("/Applications/News.app/PlugIns/NewsToday2.appex/NewsToday2"));
    CHECK(!widget_extension_path("/Applications/Widgets.app/PlugIns/Other.appex/Other"));
    CHECK(!widget_extension_path("/Applications/WidgetRenderer_Default.app/WidgetRenderer_Default"));
    CHECK(!widget_extension_path("/x/PlugIns/Widget.appex"));
    CHECK(widget_extension_path("/Applications/IntentDemo.app/PlugIns/DemoWidget.appex/DemoWidget"));
    CHECK(!widget_extension_path(""));
}

static void test_widget_census() {
    // The state the upstream note describes: the view's 25 extensions registered with no platform and no extension point, the root's fine.
    std::vector<WidgetEntry> entries;
    for (int i = 0; i < 35; ++i) {
        WidgetEntry entry;
        entry.in_view = i < 25;
        entry.registered = true;
        entry.valid = !entry.in_view;
        entry.point = !entry.in_view;
        entry.platform = entry.in_view ? 0 : 2;
        entry.error = entry.in_view ? -10814 : 0;
        entries.push_back(entry);
    }
    Values values;
    census_values(entries, values);
    CHECK_EQ(values["wk.n"], "35");
    CHECK_EQ(values["wk.fs"], "25/10");
    CHECK_EQ(values["wk.reg"], "25/10");
    CHECK_EQ(values["wk.ok"], "0/10");
    CHECK_EQ(values["wk.okn"], "10");
    CHECK_EQ(values["wk.ep"], "0/10");
    CHECK_EQ(values["wk.pf"], "0:25 2:10");
    CHECK_EQ(values["wk.oth"], "0");
    CHECK_EQ(values["wk.err"], "-10814x25");
    // Fixed, except two that LaunchServices took from the placeholder, and one it does not have.
    for (WidgetEntry &entry : entries) {
        entry.valid = entry.point = true;
        entry.platform = 2;
        entry.error = 0;
    }
    entries[0].elsewhere = entries[1].elsewhere = true;
    entries[2] = WidgetEntry();
    entries[2].in_view = true;
    entries[2].error = -10814;
    Values fixed;
    census_values(entries, fixed);
    CHECK_EQ(fixed["wk.reg"], "24/10");
    CHECK_EQ(fixed["wk.ok"], "24/10");
    CHECK_EQ(fixed["wk.pf"], "2:34");
    CHECK_EQ(fixed["wk.oth"], "2");
    CHECK_EQ(fixed["wk.err"], "-10814x1");
    // Nothing found: a Mac, or a boot without the view.
    Values empty;
    census_values({}, empty);
    CHECK_EQ(empty["wk.n"], "0");
    CHECK_EQ(empty["wk.fs"], "0/0");
    CHECK_EQ(empty["wk.pf"], "-");
    CHECK_EQ(empty["wk.err"], "0");
    Values line;
    line["wk.n"] = "35";
    line["wk.okn"] = "34";
    CHECK(summary_line(1, 61, line).find(" wk 34/35") != std::string::npos);
}

static void test_rounds() {
    Config config;
    // A steady phone: the details once, then only the summaries, and well inside the budget.
    {
        Rounds rounds(config);
        Budget used(config.budget_lines, config.budget_bytes);
        const Values values = full_values();
        std::vector<std::string> first = rounds.lines(1, 61, values);
        CHECK(first.size() == 12 && first[0].compare(0, 7, "r1 sum ") == 0);
        CHECK(first[1].compare(0, 12, "r1@61 wk n35") == 0 && first[2].compare(0, 13, "r1@61 wkv cap") == 0);
        CHECK_EQ(first[3], "r1@61 kc add 0 get 0 del 0 match 1");
        for (const std::string &line : first) CHECK(clean(line, kLineMax));
        Values later = values;
        later["time.local"] = "2026-10-31 19:04:03 +0800";     // the clock moved: no new line
        later["data.free"] = "1400";
        std::vector<std::string> second = rounds.lines(2, 181, later);
        CHECK(second.size() == 1 && second[0].find("data 1400/2047M") != std::string::npos);
        later["kc.add"] = "-34018";                            // a change comes through
        later["time.tz"] = "UTC";
        std::vector<std::string> third = rounds.lines(3, 601, later);
        CHECK(third.size() == 3);
        CHECK_EQ(third[1], "r3@601 kc add -34018 get 0 del 0 match 1");
        CHECK(third[2].find("tz UTC") != std::string::npos && rounds.cut() == 0);
    }
    // The worst case: every line at its longest and every line changing each round. The summaries survive, the budget holds, and
    // the kernel, with its own stricter arithmetic, takes every line the daemon writes.
    {
        Rounds rounds(config);
        struct pagani_diag_limit limit;
        memset(&limit, 0, sizeof limit);
        pagani_diag_budget(&limit, 1);
        unsigned written = 0, summaries = 0;
        size_t bytes = 0;
        uint64_t now = 0;
        auto write = [&](const std::string &line) {
            const size_t text = kPrefixBytes + line.size();
            bytes += text + kRecordBytes;
            ++written;
            CHECK(pagani_diag_take(&limit, now, 10, text) == PAGANI_DIAG_TAKEN);
            now += 100;
        };
        const std::string start(kLineMax, 's');
        rounds.spend(start);
        write(start);
        const unsigned times[] = {60, 180, 600};
        for (unsigned round = 1; round <= 3; ++round) {
            Values wild = full_values();
            for (auto &item : wild) item.second = std::string(300, char('a' + round));
            wild["t.mg"] = std::string(300, 'm');
            wild["t.kc"] = std::string(300, 'k');
            std::vector<std::string> lines = rounds.lines(round, times[round - 1], wild);
            CHECK(!lines.empty() && lines[0].compare(0, 2 + std::to_string(round).size() - 1 + 4, "r" + std::to_string(round) + " sum") == 0);
            summaries += !lines.empty() && lines[0].find(" sum ") != std::string::npos;
            for (const std::string &line : lines) write(line);
        }
        const std::string end(kLineMax, 'e');
        CHECK(rounds.budget().fits(end.size()));
        write(end);
        CHECK(summaries == 3);
        CHECK(bytes <= config.budget_bytes && written <= config.budget_lines);
        CHECK(rounds.cut() > 0);
        CHECK(limit.dropped == 0);
    }
    // A tiny budget: nothing is overrun, the summaries go first.
    {
        Config tiny;
        tiny.budget_lines = 4;
        tiny.budget_bytes = 800;
        Rounds rounds(tiny);
        unsigned written = 0;
        size_t bytes = 0;
        const Values values = full_values();
        for (unsigned round = 1; round <= 3; ++round)
            for (const std::string &line : rounds.lines(round, round * 100, values)) {
                ++written;
                bytes += Budget::cost(line.size());
            }
        CHECK(written <= 4 && bytes <= 800);
        CHECK(rounds.cut() > 0);
    }
    // No rounds at all: nothing to write, nothing to reserve.
    {
        Config none;
        none.rounds.clear();
        Rounds rounds(none);
        CHECK(rounds.budget().fits(kLineMax));
    }
    // The daemon's budget lies inside the kernel's, and a full record fits one U line.
    CHECK(kBudgetLines <= PAGANI_DIAG_BOOT_LINES && kBudgetBytes <= PAGANI_DIAG_BOOT_BYTES);
    CHECK(kRecordBytes == PAGANI_DIAG_RECORD_BYTES && kPrefixBytes == sizeof(PAGANI_DIAG_PREFIX) - 1);
    CHECK(kLineMax + kPrefixBytes <= PAGANI_DIAG_TEXT);
}

static void test_pass_lines() {
    PassFacts f;
    f.pass = 3;
    f.uptime = 95;
    CHECK_EQ(pass_line(f), "p3 @95 st - lk - mig - bun - stg - sen - lg -");
    CHECK(not1_line(f).empty());
    f.file_bytes = 734;
    const char *apps[] = {"com.apple.tv", "com.apple.iBooks", "com.apple.MobileAddressBook", "com.apple.facetime", "com.apple.DocumentsApp",
                          "com.apple.findmy", "com.apple.Fitness", "com.apple.freeform", "com.apple.Health", "com.apple.Home",
                          "com.apple.Magnifier", "com.apple.Maps", "com.apple.measure", "com.apple.mobilecal", "com.apple.mobilemail",
                          "com.apple.mobilenotes", "com.apple.MobileStore", "com.apple.mobiletimer", "com.apple.Music", "com.apple.news",
                          "com.apple.podcasts", "com.apple.reminders", "com.apple.Translate", "com.apple.shortcuts", "com.apple.stocks",
                          "com.apple.tips", "com.apple.VoiceMemos", "com.apple.weather", "com.apple.Photo-Booth"};
    for (const char *app : apps) f.file.emplace_back(app, 2);
    f.file[0].second = 1;
    f.file[1].second = 6;
    f.lookup = 1;
    f.lookup_states = f.file;
    f.migration = 1;
    f.bundles = 0;
    f.staged = 29;
    f.sentinel = 1;
    f.log = "15234/52/1200ms";
    f.kernel_dropped = 2;
    CHECK_EQ(pass_line(f), "p3 @95 st 734b n29 1:1 2:27 6:1 lk n29 1:1 2:27 6:1 mig 1 bun 0 stg 29 sen 1 lg 15234/52/1200ms kd 2");
    const std::string not1 = not1_line(f);
    CHECK(not1.compare(0, 31, "p3 not1 2:DocumentsApp,Fitness,") == 0);
    CHECK(not1.find(" 6:iBooks") == std::string::npos);       // cut: the 6 group comes after the 27 of state 2
    CHECK(not1.size() <= kLineMax && clean(not1, kLineMax));
    CHECK(not1.find(" +") != std::string::npos);
    // Everything that was left out is counted: shown IDs + the "+N" number = 28.
    size_t shown = 0;
    for (char c : not1.substr(8, not1.find(" +") - 8)) shown += c == ',';
    shown += 1;
    CHECK(shown + std::stoul(not1.substr(not1.find(" +") + 2)) == 28);
    PassFacts few;
    few.pass = 1;
    few.file = {{"com.apple.weather", 2}, {"com.apple.tips", 6}, {"com.apple.Maps", 1}};
    CHECK_EQ(not1_line(few), "p1 not1 2:weather 6:tips");
    few.file_bytes = -2;
    few.lookup = -2;
    few.migration = -2;
    CHECK_EQ(pass_line(few), "p1 @0 st unreadable lk fail mig e bun - stg - sen - lg -");
    few.file_bytes = -3;
    few.lookup = -1;
    CHECK(pass_line(few).find("st bad lk nil") != std::string::npos);
    CHECK_EQ(abbreviate_id("com.apple.MobileAddressBook"), "MobileAddres");
    CHECK_EQ(abbreviate_id("org.example,app x"), "org.example_");
}

int main() {
    test_config();
    test_entry_lines();
    test_classify();
    test_compact_and_format();
    test_rationer();
    test_pacing_against_kernel();
    test_kernel_text();
    test_facts_lines();
    test_feature_lines();
    test_widget_paths();
    test_widget_census();
    test_rounds();
    test_pass_lines();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
