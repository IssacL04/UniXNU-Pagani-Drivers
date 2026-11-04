// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the kernel side's pure C (kernel/pagani_diag.h), compiled as C with the kernel's strict conversions (Test/run.sh):
// the U line of a write and the limits (the budget of a boot in lines and ring bytes, 20 lines in any 10 s, refusals counted), the
// spool's line splitter, the summary line the kernel keeps, the watched names, and the kernel's own lines (status, process table,
// clock) within one 200-byte record.
#include "../kernel/pagani_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;
#define CHECK(condition)                                                                        \
	do {                                                                                        \
		++checks;                                                                               \
		if (!(condition)) {                                                                     \
			++failures;                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);       \
		}                                                                                       \
	} while (0)

static int
printable(const char *line)
{
	for (; *line; line++) {
		if (*line < 0x20 || *line >= 0x7f) {
			return 0;
		}
	}
	return 1;
}

static void
test_line_and_limits(void)
{
	char line[PAGANI_DIAG_TEXT + 1];
	CHECK(pagani_diag_line(line, "start v1 pid 87", 15) == 21 && strcmp(line, "diag: start v1 pid 87") == 0);
	CHECK(pagani_diag_line(line, "diag: x", 7) == 7 && strcmp(line, "diag: x") == 0);
	CHECK(pagani_diag_line(line, "diag: diag: x", 13) == 13 && strcmp(line, "diag: diag: x") == 0);
	CHECK(pagani_diag_line(line, "diag:", 5) == 11 && strcmp(line, "diag: diag:") == 0);
	CHECK(pagani_diag_line(line, "\r", 1) == 0);
	CHECK(pagani_diag_line(line, "x\ry", 3) == 7 && strcmp(line, "diag: x") == 0);
	CHECK(pagani_diag_line(line, "a\tb\x80" "c", 5) == 11 && strcmp(line, "diag: a b c") == 0);
	char big[400];
	memset(big, 'k', sizeof(big));
	CHECK(pagani_diag_line(line, big, PAGANI_DIAG_TEXT) == PAGANI_DIAG_TEXT && line[PAGANI_DIAG_TEXT] == '\0');
	CHECK(printable(line));

	struct pagani_diag_limit limit;
	memset(&limit, 0, sizeof(limit));
	pagani_diag_budget(&limit, 2);          /* the long budget: lines run out first with these short lines */
	CHECK(limit.boot_lines == PAGANI_DIAG_LONG_LINES && limit.boot_bytes == PAGANI_DIAG_LONG_BYTES);
	uint64_t now = 1000, window = 10;
	for (int i = 0; i < PAGANI_DIAG_WINDOW_LINES; i++) {
		CHECK(pagani_diag_take(&limit, now, window, 10) == PAGANI_DIAG_TAKEN);
	}
	CHECK(pagani_diag_take(&limit, now + 9, window, 10) == PAGANI_DIAG_OVER_WINDOW);
	CHECK(limit.dropped == 1 && limit.lines == PAGANI_DIAG_WINDOW_LINES);
	CHECK(pagani_diag_take(&limit, now + 10, window, 10) == PAGANI_DIAG_TAKEN);     /* the oldest left the window */
	/* A steady 1 a unit (10 a window) is always taken, up to the lines of the budget. */
	now = 100;
	while (limit.lines < limit.boot_lines) {
		CHECK(pagani_diag_take(&limit, now, window, 10) == PAGANI_DIAG_TAKEN);
		now += 1;
	}
	CHECK(pagani_diag_take(&limit, now + 1000, window, 10) == PAGANI_DIAG_OVER_BOOT);
	CHECK(limit.dropped == 2 && limit.lines == limit.boot_lines);
	/* The default budget: the lines of a boot, and the bytes as the events ring counts them. */
	memset(&limit, 0, sizeof(limit));
	pagani_diag_budget(&limit, 1);
	CHECK(limit.boot_lines == PAGANI_DIAG_BOOT_LINES && limit.boot_bytes == PAGANI_DIAG_BOOT_BYTES);
	now = 0;
	while (pagani_diag_take(&limit, now, window, 10) == PAGANI_DIAG_TAKEN) {
		now += 100;
	}
	CHECK(limit.lines == PAGANI_DIAG_BOOT_LINES && limit.bytes == PAGANI_DIAG_BOOT_LINES * (10 + PAGANI_DIAG_RECORD_BYTES));
	memset(&limit, 0, sizeof(limit));
	pagani_diag_budget(&limit, 1);
	now = 0;
	uint32_t spent = 0;
	while (pagani_diag_take(&limit, now, window, PAGANI_DIAG_TEXT) == PAGANI_DIAG_TAKEN) {
		spent += PAGANI_DIAG_TEXT + PAGANI_DIAG_RECORD_BYTES;
		now += 100;
	}
	CHECK(limit.bytes == spent && spent <= PAGANI_DIAG_BOOT_BYTES && spent + PAGANI_DIAG_TEXT + PAGANI_DIAG_RECORD_BYTES > PAGANI_DIAG_BOOT_BYTES);
	CHECK(limit.lines < PAGANI_DIAG_BOOT_LINES && limit.dropped == 1);
	CHECK(pagani_diag_take(&limit, now + 100, window, 1) == PAGANI_DIAG_TAKEN || limit.bytes + 1 + PAGANI_DIAG_RECORD_BYTES > PAGANI_DIAG_BOOT_BYTES);
	/* Model check: random write times; never more than 20 taken within any window. */
	memset(&limit, 0, sizeof(limit));
	pagani_diag_budget(&limit, 2);
	uint64_t taken_at[PAGANI_DIAG_LONG_LINES];
	size_t taken = 0;
	unsigned seed = 7;
	now = 0;
	for (int i = 0; i < 5000 && taken < PAGANI_DIAG_LONG_LINES; i++) {
		seed = seed * 1103515245u + 12345u;
		now += (seed >> 16) % 3;
		if (pagani_diag_take(&limit, now, window, 10) == PAGANI_DIAG_TAKEN) {
			taken_at[taken++] = now;
		}
	}
	for (size_t i = PAGANI_DIAG_WINDOW_LINES; i < taken; i++) {
		CHECK(taken_at[i] - taken_at[i - PAGANI_DIAG_WINDOW_LINES] >= window);
	}
}

/* The spool through pagani_diag_spool_take in pieces of every size: the same lines as a plain split, overlong lines cut. */
static size_t
spool_lines(const char *data, size_t n, size_t piece, char lines[][PAGANI_DIAG_TEXT + 1], size_t max)
{
	struct pagani_diag_spool spool;
	memset(&spool, 0, sizeof(spool));
	size_t count = 0;
	for (size_t start = 0; start < n; start += piece) {
		size_t end = start + piece < n ? start + piece : n;
		for (size_t at = start; at < end;) {
			size_t used = 0;
			char line[PAGANI_DIAG_TEXT + 1];
			int got = pagani_diag_spool_take(&spool, data + at, end - at, line, &used);
			CHECK(used > 0 && used <= end - at);
			if (got && count < max) {
				strcpy(lines[count++], line);
			}
			at += used;
		}
	}
	return count;
}

static void
test_spool(void)
{
	char lines[16][PAGANI_DIAG_TEXT + 1];
	const char *text = "start v2 pid 9\nr1@61 mg bat 1\n\nr1 sum ok\npartial";
	for (size_t piece = 1; piece <= strlen(text); piece++) {
		size_t count = spool_lines(text, strlen(text), piece, lines, 16);
		CHECK(count == 4);
		CHECK(strcmp(lines[0], "start v2 pid 9") == 0 && strcmp(lines[1], "r1@61 mg bat 1") == 0 && lines[2][0] == '\0' &&
		    strcmp(lines[3], "r1 sum ok") == 0);
	}
	/* 450 bytes then a break: the first 200 bytes are a line, the rest is dropped; the next line is whole. */
	char data[600];
	memset(data, 'a', 450);
	data[450] = '\n';
	memcpy(data + 451, "next\n", 5);
	for (size_t piece = 1; piece <= 456; piece += 37) {
		size_t count = spool_lines(data, 456, piece, lines, 16);
		CHECK(count == 2 && strlen(lines[0]) == PAGANI_DIAG_TEXT && strcmp(lines[1], "next") == 0);
	}
	/* exactly 200 bytes and a break: one line, nothing dropped */
	memset(data, 'b', 200);
	data[200] = '\n';
	memcpy(data + 201, "z\n", 2);
	CHECK(spool_lines(data, 203, 203, lines, 16) == 2 && strlen(lines[0]) == 200 && strcmp(lines[1], "z") == 0);
}

static void
test_summary_and_names(void)
{
	CHECK(pagani_diag_is_summary("r1 sum bat ok", 13));
	CHECK(pagani_diag_is_summary("r12 sum x", 9));
	CHECK(!pagani_diag_is_summary("r1@61 sum x", 11));
	CHECK(!pagani_diag_is_summary("r1 summary", 10));
	CHECK(!pagani_diag_is_summary("rx sum a", 8));
	CHECK(!pagani_diag_is_summary("r1 sum", 6));
	CHECK(!pagani_diag_is_summary("start v2", 8));
	CHECK(strcmp(pagani_diag_basename("/usr/libexec/DumpPanic"), "DumpPanic") == 0);
	CHECK(strcmp(pagani_diag_basename("wifid"), "wifid") == 0);
	CHECK(strcmp(pagani_diag_basename("/a/b/"), "b/") == 0);
	CHECK(pagani_diag_watch_index("wifid") == 0);
	CHECK(pagani_diag_watch_index("wifid2") == -1);
	CHECK(pagani_diag_watch_index("wifi") == -1);
	CHECK(pagani_diag_watch_index("WidgetRenderer_Default") == 10 && pagani_diag_watch_index("WidgetRenderer") == 10);
	CHECK(pagani_diag_watch_index("chronod") == 11 && pagani_diag_watch_index("chronod2") == -1);
	CHECK(pagani_diag_watch_index("paganidiag") == -1);     /* its probes come and go: its state is in the status line */
	CHECK(pagani_diag_watch_index("FindMy") == (int)PAGANI_DIAG_WATCHED - 1);
	CHECK(pagani_diag_watch_index("") == -1);
	for (size_t i = 0; i < PAGANI_DIAG_WATCHED; i++) {
		CHECK(pagani_diag_watch_index(pagani_diag_watched[i].name) == (int)i);
	}
	/* the widget census */
	CHECK(pagani_diag_extension("/Applications/Weather.app/PlugIns/WeatherWidget.appex/WeatherWidget") == 2);
	CHECK(pagani_diag_extension("/Applications/Music.app/PlugIns/MusicWidgets.appex/MusicWidgets") == 2);
	CHECK(pagani_diag_extension("/Applications/News.app/PlugIns/NewsToday2.appex/NewsToday2") == 1);
	/* The six intents services with "Widget" in their names: a widget's configuration, no widget. */
	CHECK(pagani_diag_extension("/Applications/Batteries.app/PlugIns/BatteriesWidgetIntentsExtension.appex/BatteriesWidgetIntentsExtension") == 1);
	CHECK(pagani_diag_extension("/Applications/MobileSlideShow.app/PlugIns/PhotosReliveWidgetIntents.appex/PhotosReliveWidgetIntents") == 1);
	CHECK(pagani_diag_extension("/Applications/Preferences.app/PlugIns/ScreenTimeWidgetIntentsExtension.appex/ScreenTimeWidgetIntentsExtension") == 1);
	CHECK(pagani_diag_extension("/Applications/FindMy.app/PlugIns/FindMyWidgetIntentsItems.appex/FindMyWidgetIntentsItems") == 1);
	CHECK(pagani_diag_extension("/Applications/FindMy.app/PlugIns/FindMyWidgetIntentsPeople.appex/FindMyWidgetIntentsPeople") == 1);
	CHECK(pagani_diag_extension("/Applications/Home.app/PlugIns/HomeWidgetSingleAccessoryIntent.appex/HomeWidgetSingleAccessoryIntent") == 1);
	/* "Intent" in the app's name alone is no reason. */
	CHECK(pagani_diag_extension("/Applications/IntentDemo.app/PlugIns/DemoWidget.appex/DemoWidget") == 2);
	CHECK(pagani_diag_extension("/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod") == 0);
	CHECK(pagani_diag_extension("/Applications/WidgetRenderer_Default.app/WidgetRenderer_Default") == 0);
	CHECK(pagani_diag_extension("/Applications/Widgets.app/PlugIns/Other.appex/Other") == 1);   /* "Widget" in the app's name alone */
	CHECK(pagani_diag_extension("/usr/libexec/xpcproxy") == 0 && pagani_diag_extension("") == 0);
	CHECK(pagani_diag_extension("/x/.appex") == 0 && pagani_diag_extension("/x/a.appex/a") == 1);
	CHECK(PAGANI_DIAG_ROUND_COUNT == 7 && pagani_diag_rounds[0] == 60 && pagani_diag_rounds[1] == 180 && pagani_diag_rounds[2] == 600 &&
	    pagani_diag_rounds[PAGANI_DIAG_ROUND_COUNT - 1] == 1800);
	for (size_t i = 1; i < PAGANI_DIAG_ROUND_COUNT; i++) {
		CHECK(pagani_diag_rounds[i] > pagani_diag_rounds[i - 1]);
	}
}

static void
test_kernel_lines(void)
{
	char line[PAGANI_DIAG_TEXT + 1];
	struct pagani_diag_ps ps[PAGANI_DIAG_WATCHED];
	memset(ps, 0, sizeof(ps));
	/* every name live with a long pid, several instances and restarts: the widest table */
	for (size_t i = 0; i < PAGANI_DIAG_WATCHED; i++) {
		ps[i].pid = 99999;
		ps[i].live = 12;
		ps[i].execs = 345;
	}
	size_t first = 0, lines = 0;
	while (first < PAGANI_DIAG_WATCHED) {
		size_t next = pagani_diag_ps_line(line, sizeof(line), PAGANI_DIAG_TEXT, "diag: k1800", ps, PAGANI_DIAG_WATCHED, first);
		CHECK(next > first);
		CHECK(strlen(line) <= PAGANI_DIAG_TEXT && printable(line) && strncmp(line, "diag: k1800 ps ", 15) == 0);
		if (next <= first) {
			break;
		}
		first = next;
		lines++;
	}
	CHECK(lines >= 2 && lines <= 4);
	memset(ps, 0, sizeof(ps));
	ps[0].execs = 1;                    /* wifid started and is gone */
	ps[4].pid = 92;                     /* audiomxd */
	ps[4].live = 1;
	ps[4].execs = 1;
	ps[14].pid = 312;                   /* iconservicesagent restarted twice */
	ps[14].live = 1;
	ps[14].execs = 3;
	ps[10].pid = 297;                   /* two WidgetRenderer processes */
	ps[10].live = 2;
	size_t next = pagani_diag_ps_line(line, sizeof(line), PAGANI_DIAG_TEXT, "diag: k60", ps, PAGANI_DIAG_WATCHED, 0);
	CHECK(next == PAGANI_DIAG_WATCHED || strstr(line, "wifid:-/1") != NULL);
	CHECK(strstr(line, " wifid:-/1 ") != NULL && strstr(line, " bluetoothd:- ") != NULL && strstr(line, " audiomxd:92 ") != NULL);
	if (next < PAGANI_DIAG_WATCHED) {
		pagani_diag_ps_line(line, sizeof(line), PAGANI_DIAG_TEXT, "diag: k60", ps, PAGANI_DIAG_WATCHED, next);
	}
	char all[1024] = "";
	first = 0;
	while (first < PAGANI_DIAG_WATCHED) {
		size_t n = pagani_diag_ps_line(line, sizeof(line), PAGANI_DIAG_TEXT, "diag: k60", ps, PAGANI_DIAG_WATCHED, first);
		strcat(all, line + strlen("diag: k60 ps"));
		strcat(all, " ");
		first = n;
	}
	CHECK(strstr(all, " iconservicesagent:312/3 ") != NULL && strstr(all, " WidgetRenderer:297*2 ") != NULL &&
	    strstr(all, " FindMy:- ") != NULL);
	/* a too small limit: nothing fits, the caller stops */
	CHECK(pagani_diag_ps_line(line, sizeof(line), 12, "diag: k60", ps, PAGANI_DIAG_WATCHED, 0) == 0);

	/* the clock */
	char utc[21];
	pagani_diag_utc(0, utc);
	CHECK(strcmp(utc, "1970-01-01T00:00:00Z") == 0);
	pagani_diag_utc(-5, utc);
	CHECK(strcmp(utc, "1970-01-01T00:00:00Z") == 0);
	pagani_diag_utc(951782400, utc);    /* 2000-02-29 */
	CHECK(strcmp(utc, "2000-02-29T00:00:00Z") == 0);
	pagani_diag_utc(1793437445, utc);
	CHECK(strcmp(utc, "2026-10-31T09:04:05Z") == 0);
	/* against gmtime for many instants */
	unsigned seed = 11;
	for (int i = 0; i < 20000; i++) {
		seed = seed * 1103515245u + 12345u;
		time_t t = (time_t)(((uint64_t)seed << 3) % 4102444800ull);  /* to 2100 */
		struct tm tm;
		gmtime_r(&t, &tm);
		char want[32];
		snprintf(want, sizeof(want), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		    tm.tm_min, tm.tm_sec);
		pagani_diag_utc((int64_t)t, utc);
		CHECK(strcmp(utc, want) == 0);
		if (strcmp(utc, want) != 0) {
			break;
		}
	}
	pagani_diag_time_line(line, sizeof(line), "diag: k60", 61, 1);
	CHECK(strcmp(line, "diag: k60 time utc 1970-01-01T00:01:01Z boot 1970-01-01T00:00:01Z") == 0);

	/* the status line, every state, within one record */
	struct pagani_diag_status s;
	memset(&s, 0, sizeof(s));
	s.uptime = 61;
	s.launch = PAGANI_DIAG_LAUNCH_WAIT;
	s.activated = s.signature = -1;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d wait ln 0 sp 0 dr 0 dp 0 key 0 tch 0 ax 0/0 vg 0/0") == 0);
	s.launch = PAGANI_DIAG_LAUNCH_NOFILE;
	s.host_execs = 1;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d nofile ln 0 sp 0 dr 0 dp 1 key 0 tch 0 ax 0/0 vg 0/0") == 0);
	s.launch = PAGANI_DIAG_LAUNCH_ERROR;
	s.launch_error = 13;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d err 13 ln 0 sp 0 dr 0 dp 1 key 0 tch 0 ax 0/0 vg 0/0") == 0);
	s.launch = PAGANI_DIAG_LAUNCH_REDIRECTED;
	s.pid = 49;
	s.launch_uptime = 18;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d pid 49@18 act - sig - gone ln 0 sp 0 dr 0 dp 1 key 0 tch 0 ax 0/0 vg 0/0") == 0);
	s.activated = 0;
	s.signature = 0;
	s.now = "paganidiag";
	s.csflags = 0x24003301;
	s.sysctl_lines = 12;
	s.key_events = 7;
	s.touch_events = 1200;
	s.extension_execs = 41;
	s.widget_execs = 6;
	s.vget_calls = 1893;
	s.vget_resolved = 1890;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d pid 49@18 act 0 sig 0 now paganidiag cs 0x24003301 ln 12 sp 0 dr 0 dp 1 key 7 tch 1200 ax 41/6 vg 1893/1890") == 0);
	s.signature = 1;
	s.signature_namespace = 9;
	s.signature_code = 0x4;
	s.now = NULL;
	s.spool_error = 1;
	s.spool_lines = 3;
	pagani_diag_status_line(line, sizeof(line), "diag: k60", &s);
	CHECK(strcmp(line, "diag: k60 up 61 d pid 49@18 act 0 sig 1 ns 9 c 0x4 gone ln 12 sp 3e1 dr 0 dp 1 key 7 tch 1200 ax 41/6 vg 1893/1890") == 0);
	/* the widest values still fit one record */
	s.uptime = 4294967295u;
	s.pid = -2147483647 - 1;
	s.launch_uptime = 4294967295u;
	s.activated = 2147483647;
	s.signature = -2;                   /* a negative verdict prints as not known */
	s.now = "0123456789abcdef0123456789abcdef";
	s.csflags = ~0ull;
	s.sysctl_lines = s.spool_lines = s.dropped = s.host_execs = s.key_events = s.touch_events = 4294967295u;
	s.spool_error = -2147483647 - 1;
	pagani_diag_status_line(line, sizeof(line), "diag: k1800", &s);
	CHECK(strlen(line) <= PAGANI_DIAG_TEXT && printable(line));
	/* a tiny buffer is never overrun */
	char small[8];
	memset(small, 'x', sizeof(small));
	pagani_diag_status_line(small, 5, "diag: k60", &s);
	CHECK(strlen(small) == 4 && small[5] == 'x');
	pagani_diag_ps_line(small, 5, 4, "diag: k60", ps, PAGANI_DIAG_WATCHED, 0);
	CHECK(strlen(small) == 4);
	pagani_diag_time_line(small, 5, "diag: k60", 0, 0);
	CHECK(strlen(small) == 4);
}

int
main(void)
{
	test_line_and_limits();
	test_spool();
	test_summary_and_names();
	test_kernel_lines();
	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
