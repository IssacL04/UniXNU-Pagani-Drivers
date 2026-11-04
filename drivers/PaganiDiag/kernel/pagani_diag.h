/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * PaganiDiag, kernel side (phone-diag-sysctl.patch; UniXNU-Pagani-Drivers drivers/PaganiDiag).
 *
 * Everything here is off unless boot-arg pagani-diag is not 0 (read once, at the boot's first exec). Then:
 * - kern.pagani_diag_line is a write-only, root-only string sysctl: each write is one line, logged as the BootProgress U line
 *   "diag: <text>". The text is the written bytes up to the first NUL or line break, at most PAGANI_DIAG_TEXT of them; a leading
 *   "diag: " is not doubled, control and non-ASCII bytes become spaces, and the whole line stays within one 200-character U record.
 *   The same lines can come through the spool file PAGANI_DIAG_SPOOL (root's, read by the kernel every PAGANI_DIAG_TICK_SECONDS):
 *   the daemon's fallback when the sysctl write is refused. Either way the kernel logs at most PAGANI_DIAG_WINDOW_LINES in any
 *   PAGANI_DIAG_WINDOW_SECONDS and, because the 32 KiB events ring is shared with every driver, stops at a budget a boot:
 *   PAGANI_DIAG_BOOT_LINES lines and PAGANI_DIAG_BOOT_BYTES bytes as the ring counts them (pagani-diag=2, for the migration passes:
 *   the LONG limits). A write over a limit fails (EAGAIN for the window, ENOSPC for the boot), is counted (kern.pagani_diag_dropped)
 *   and the first such write logs one line saying so.
 * - The kernel's own checks, which need no user space: at pagani_diag_rounds seconds after boot one "k<s>" line on the daemon's
 *   start (pid, exec and signature verdicts, alive or gone, lines so far) and PaganiInput's key and touch counters, and on the first
 *   rounds the process table of the watched names (pid, instances, execs since boot; left out when it did not change) and the
 *   calendar clock; on the later rounds the daemon's last summary line again, so the verdict is still in the ring at a late reset.
 *   Every status line ends with the app extension execs since boot and bindfs_vget's counts (the widget census, README.md).
 *
 * Pure C without kernel dependencies so the host test compiles the same code (drivers/PaganiDiag/Test/test_kernel.c).
 */
#ifndef PAGANI_DIAG_H
#define PAGANI_DIAG_H

#include <stddef.h>
#include <stdint.h>

#define PAGANI_DIAG_TEXT            200     /* one U record (BootProgress RAMOOPS_LOG_TEXT_MAX); a longer line continues in '+' records */
#define PAGANI_DIAG_PREFIX          "diag: "
#define PAGANI_DIAG_BOOT_LINES      48      /* the daemon's lines a boot: start, the rounds' lines, end (README.md "Budget") */
#define PAGANI_DIAG_BOOT_BYTES      5632    /* ... and their cost in the 32 KiB events ring */
#define PAGANI_DIAG_LONG_LINES      300     /* pagani-diag=2: the migration passes as well */
#define PAGANI_DIAG_LONG_BYTES      16384
#define PAGANI_DIAG_RECORD_BYTES    34      /* a record beside its text: 33 bytes of header and the line break (RamoopsLog.h) */
#define PAGANI_DIAG_WINDOW_LINES    20
#define PAGANI_DIAG_WINDOW_SECONDS  10
#define PAGANI_DIAG_SILENCE_SECONDS 90      /* after the launch: one line when the daemon has written nothing by then */
/* The first exec of PAGANI_DIAG_HOST runs PAGANI_DIAG_DAEMON. The directory is not free to choose: the Sandbox platform policy's
 * process-exec* (21H461) refuses xpcproxy any program below /private/var outside a short list of trees, and this tree is on the list
 * while no program of the system refers to it (README.md "How it starts"; Test/check-exec-policy.py walks the policy). */
#define PAGANI_DIAG_DAEMON          "/private/var/factory_mount/paganidiag"
#define PAGANI_DIAG_HOST            "/usr/libexec/DumpPanic"
#define PAGANI_DIAG_SPOOL           "/private/var/db/paganidiag.spool"   /* root's directory: only root can create the file */
#define PAGANI_DIAG_SPOOL_CHUNK     4096    /* read at most this much a tick */
#define PAGANI_DIAG_SPOOL_MAX       (256 * 1024)    /* and no further into the file than this */
#define PAGANI_DIAG_TICK_SECONDS    5
#define PAGANI_DIAG_FULL_ROUNDS     3       /* the first rounds (60, 180, 600 s) carry the process table and the clock */

/* When the kernel writes its own lines, in seconds since boot; the watch ends after the last. */
static const uint16_t pagani_diag_rounds[] = {60, 180, 600, 900, 1200, 1500, 1800};
#define PAGANI_DIAG_ROUND_COUNT     (sizeof(pagani_diag_rounds) / sizeof(pagani_diag_rounds[0]))

enum {
	PAGANI_DIAG_TAKEN = 0,
	PAGANI_DIAG_OVER_WINDOW = 1,
	PAGANI_DIAG_OVER_BOOT = 2,
};

/* The limits: when the last PAGANI_DIAG_WINDOW_LINES accepted lines came (a ring, oldest at lines % PAGANI_DIAG_WINDOW_LINES), how many
 * were accepted, what they cost and how many were refused; the budget of this boot. */
struct pagani_diag_limit {
	uint64_t times[PAGANI_DIAG_WINDOW_LINES];
	uint32_t lines;
	uint32_t bytes;
	uint32_t dropped;
	uint32_t noted;                     /* the first refusal has been logged */
	uint32_t boot_lines, boot_bytes;
};

/* The budget of a boot-arg value (not 0). */
static inline void
pagani_diag_budget(struct pagani_diag_limit *limit, uint32_t mode)
{
	limit->boot_lines = mode >= 2 ? PAGANI_DIAG_LONG_LINES : PAGANI_DIAG_BOOT_LINES;
	limit->boot_bytes = mode >= 2 ? PAGANI_DIAG_LONG_BYTES : PAGANI_DIAG_BOOT_BYTES;
}

/* Whether a line of `length` bytes written at `now` is logged; `window` is PAGANI_DIAG_WINDOW_SECONDS in the unit of `now`. Never more
 * than PAGANI_DIAG_WINDOW_LINES accepted lines lie within any `window`, nor more than the budget in a boot. */
static inline int
pagani_diag_take(struct pagani_diag_limit *limit, uint64_t now, uint64_t window, size_t length)
{
	uint32_t oldest = limit->lines % PAGANI_DIAG_WINDOW_LINES;
	uint32_t cost = (uint32_t)length + PAGANI_DIAG_RECORD_BYTES;
	if (limit->lines >= limit->boot_lines || limit->bytes + cost > limit->boot_bytes) {
		limit->dropped++;
		return PAGANI_DIAG_OVER_BOOT;
	}
	if (limit->lines >= PAGANI_DIAG_WINDOW_LINES && now - limit->times[oldest] < window) {
		limit->dropped++;
		return PAGANI_DIAG_OVER_WINDOW;
	}
	limit->times[oldest] = now;
	limit->lines++;
	limit->bytes += cost;
	return PAGANI_DIAG_TAKEN;
}

/* The U line of a written text (n bytes, not necessarily terminated): "diag: " and the text as described above. `line` holds
 * PAGANI_DIAG_TEXT + 1 bytes; returns the line's length, or 0 for an empty text. */
static inline size_t
pagani_diag_line(char *line, const char *text, size_t n)
{
	static const char prefix[] = PAGANI_DIAG_PREFIX;
	const size_t prefix_length = sizeof(prefix) - 1;
	size_t used = 0, at = 0;
	if (n >= prefix_length) {
		size_t i = 0;
		while (i < prefix_length && text[i] == prefix[i]) {
			i++;
		}
		if (i == prefix_length) {
			at = prefix_length;
		}
	}
	if (at >= n || text[at] == '\0' || text[at] == '\n' || text[at] == '\r') {
		line[0] = '\0';
		return 0;
	}
	while (used < prefix_length) {
		line[used] = prefix[used];
		used++;
	}
	for (; at < n && used < PAGANI_DIAG_TEXT; at++) {
		unsigned char c = (unsigned char)text[at];
		if (c == '\0' || c == '\n' || c == '\r') {
			break;
		}
		line[used++] = (c < 0x20 || c >= 0x7f) ? ' ' : (char)c;
	}
	line[used] = '\0';
	return used;
}

/* The daemon's round summary ("r<round> sum ..."), which the kernel keeps and logs again on its later rounds. `text` is a line as the
 * daemon wrote it (after "diag: "). */
static inline int
pagani_diag_is_summary(const char *text, size_t n)
{
	size_t at = 1;
	if (n < 7 || text[0] != 'r' || text[1] < '0' || text[1] > '9') {
		return 0;
	}
	while (at < n && text[at] >= '0' && text[at] <= '9') {
		at++;
	}
	return n - at >= 5 && text[at] == ' ' && text[at + 1] == 's' && text[at + 2] == 'u' && text[at + 3] == 'm' && text[at + 4] == ' ';
}

/* ---- the spool file: complete lines out of the bytes read so far ---------------------------------------------------------------- */

struct pagani_diag_spool {
	char partial[PAGANI_DIAG_TEXT + 1];
	size_t length;
	int skipping;                       /* inside a line longer than PAGANI_DIAG_TEXT: its first PAGANI_DIAG_TEXT bytes were taken */
};

/* Takes bytes of the spool (n > 0). Returns 1 when a line ended: `line` (PAGANI_DIAG_TEXT + 1 bytes) holds it without the line
 * break, NUL terminated, at most PAGANI_DIAG_TEXT bytes (a longer line is cut there and its rest dropped up to its line break).
 * *used is how many bytes were consumed: call again with the rest. Returns 0 with *used == n when the bytes ended inside a line. */
static inline int
pagani_diag_spool_take(struct pagani_diag_spool *s, const char *data, size_t n, char *line, size_t *used)
{
	size_t at = 0;
	while (at < n) {
		char c = data[at++];
		if (c == '\n') {
			size_t i;
			if (s->skipping) {
				s->skipping = 0;
				s->length = 0;
				continue;
			}
			for (i = 0; i < s->length; i++) {
				line[i] = s->partial[i];
			}
			line[s->length] = '\0';
			s->length = 0;
			*used = at;
			return 1;
		}
		if (s->skipping) {
			continue;
		}
		if (s->length == PAGANI_DIAG_TEXT) {
			size_t i;
			for (i = 0; i < s->length; i++) {
				line[i] = s->partial[i];
			}
			line[s->length] = '\0';
			s->length = 0;
			s->skipping = 1;
			*used = at;
			return 1;
		}
		s->partial[s->length++] = c;
	}
	*used = at;
	return 0;
}

/* ---- the kernel's own lines ----------------------------------------------------------------------------------------------------- */

/* The processes the kernel looks for (process name, as proc_best_name shows it; `prefix`: any name that starts with it). The plan of
 * 2026-10-31 (功能盘点.md 3.2 diag) and the crash loops seen so far (iconservicesagent's jetsam, FindMy). */
struct pagani_diag_watch {
	const char *name;
	uint8_t prefix;
};
static const struct pagani_diag_watch pagani_diag_watched[] = {
	{"wifid", 0}, {"bluetoothd", 0}, {"locationd", 0}, {"CommCenter", 0}, {"audiomxd", 0}, {"mediaserverd", 0},
	{"thermalmonitord", 0}, {"powerd", 0}, {"backboardd", 0}, {"kbd", 0}, {"WidgetRenderer", 1}, {"chronod", 0}, {"PosterBoard", 0},
	{"SpringBoard", 0}, {"iconservicesagent", 0}, {"securityd", 0}, {"PerfPowerServices", 0}, {"FindMy", 0},
};
#define PAGANI_DIAG_WATCHED         (sizeof(pagani_diag_watched) / sizeof(pagani_diag_watched[0]))

/* The last path component. */
static inline const char *
pagani_diag_basename(const char *path)
{
	const char *base = path;
	for (; *path; path++) {
		if (*path == '/' && path[1]) {
			base = path + 1;
		}
	}
	return base;
}

/* The index of a watched name, or -1. */
static inline int
pagani_diag_watch_index(const char *name)
{
	size_t i;
	for (i = 0; i < PAGANI_DIAG_WATCHED; i++) {
		const char *a = pagani_diag_watched[i].name, *b = name;
		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (!*a && (!*b || pagani_diag_watched[i].prefix)) {
			return (int)i;
		}
	}
	return -1;
}

/* The place after the first `needle` in the string `from`, or NULL. */
static inline const char *
pagani_diag_after(const char *from, const char *needle)
{
	for (; *from; from++) {
		const char *a = from, *b = needle;
		while (*b && *a == *b) {
			a++;
			b++;
		}
		if (!*b) {
			return a;
		}
	}
	return NULL;
}

/* What an exec path is to the widget census: 0 no app extension, 1 an extension (".appex/" in the path), 2 one with "Widget" and without
 * "Intent" in what follows the "PlugIns/" of its bundle: 33 of the 35 WidgetKit extensions of 21H461's /Applications and placeholders and
 * nothing else. NewsTag and NewsToday2 count as 1, and so do the six intents services named after a widget (FindMyWidgetIntentsItems and
 * the like), which the widget's configuration runs and which say nothing of the widget itself. Such a process leaves the table within
 * seconds and its exec record leaves the events ring soon after, so the counts are what stays. */
static inline int
pagani_diag_extension(const char *path)
{
	const char *end = pagani_diag_after(path, ".appex/");
	const char *name = end ? pagani_diag_after(path, "/PlugIns/") : NULL;
	if (!end) {
		return 0;
	}
	return name && pagani_diag_after(name, "Widget") && !pagani_diag_after(name, "Intent") ? 2 : 1;
}

/* What the kernel knows of one watched name. */
struct pagani_diag_ps {
	int32_t pid;                        /* a live process with the name (the first found), 0: none */
	uint32_t live;                      /* live processes with the name */
	uint32_t execs;                     /* execs of a program with the name since boot (the exec path's last component) */
};

/* Appends to line[used] (size bytes in all) as snprintf would; returns the new length, at most size - 1. */
static inline size_t
pagani_diag_put(char *line, size_t size, size_t used, const char *text)
{
	while (*text && used + 1 < size) {
		line[used++] = *text++;
	}
	line[used] = '\0';
	return used;
}

static inline size_t
pagani_diag_put_u64(char *line, size_t size, size_t used, uint64_t value, int hex)
{
	char digits[24];
	size_t n = 0;
	const unsigned base = hex ? 16u : 10u;
	do {
		unsigned digit = (unsigned)(value % base);
		digits[n++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
		value /= base;
	} while (value && n < sizeof(digits));
	while (n && used + 1 < size) {
		line[used++] = digits[--n];
	}
	line[used] = '\0';
	return used;
}

static inline size_t
pagani_diag_put_int(char *line, size_t size, size_t used, int64_t value)
{
	if (value < 0) {
		used = pagani_diag_put(line, size, used, "-");
		return pagani_diag_put_u64(line, size, used, (uint64_t)0 - (uint64_t)value, 0);
	}
	return pagani_diag_put_u64(line, size, used, (uint64_t)value, 0);
}

/* "<label> ps name:pid ..." with the entries [first, count) that fit within `limit` characters (limit < size): "name:pid", "name:-"
 * when no process has it, "*N" after the pid when N processes have it, "/N" when it was executed N times and that is not just once
 * for the one live process (a restart, or a start that did not stay). Returns the index of the first entry left out (count: all). */
static inline size_t
pagani_diag_ps_line(char *line, size_t size, size_t limit, const char *label, const struct pagani_diag_ps *ps, size_t count,
    size_t first)
{
	size_t used = 0, i;
	used = pagani_diag_put(line, size, used, label);
	used = pagani_diag_put(line, size, used, " ps");
	for (i = first; i < count; i++) {
		char entry[64];
		size_t n = 0;
		n = pagani_diag_put(entry, sizeof(entry), n, " ");
		n = pagani_diag_put(entry, sizeof(entry), n, pagani_diag_watched[i].name);
		n = pagani_diag_put(entry, sizeof(entry), n, ":");
		if (ps[i].pid > 0) {
			n = pagani_diag_put_int(entry, sizeof(entry), n, ps[i].pid);
		} else {
			n = pagani_diag_put(entry, sizeof(entry), n, "-");
		}
		if (ps[i].live > 1) {
			n = pagani_diag_put(entry, sizeof(entry), n, "*");
			n = pagani_diag_put_u64(entry, sizeof(entry), n, ps[i].live, 0);
		}
		if (ps[i].execs > 1 || (ps[i].execs == 1 && ps[i].live == 0)) {
			n = pagani_diag_put(entry, sizeof(entry), n, "/");
			n = pagani_diag_put_u64(entry, sizeof(entry), n, ps[i].execs, 0);
		}
		if (used + n > limit) {
			break;
		}
		used = pagani_diag_put(line, size, used, entry);
	}
	return i;
}

/* Seconds since 1970 as "YYYY-MM-DDTHH:MM:SSZ" (21 bytes with the NUL); before 1970 counts as 1970. */
static inline void
pagani_diag_utc(int64_t seconds, char out[21])
{
	int64_t days, rest, era, doe, yoe, y, doy, mp, d, m;
	unsigned i;
	char digits[21];
	if (seconds < 0) {
		seconds = 0;
	}
	days = seconds / 86400;
	rest = seconds % 86400;
	/* civil_from_days (H. Hinnant), days >= 0 */
	days += 719468;
	era = days / 146097;
	doe = days - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += m <= 2;
	if (y > 9999) {
		y = 9999;
	}
	{
		const int64_t parts[6] = {y, m, d, rest / 3600, rest / 60 % 60, rest % 60};
		const unsigned widths[6] = {4, 2, 2, 2, 2, 2};
		const char after[6] = {'-', '-', 'T', ':', ':', 'Z'};
		unsigned at = 0, p;
		for (p = 0; p < 6; p++) {
			int64_t v = parts[p];
			for (i = widths[p]; i > 0; i--) {
				digits[at + i - 1] = (char)('0' + v % 10);
				v /= 10;
			}
			at += widths[p];
			digits[at++] = after[p];
		}
		digits[at] = '\0';
	}
	for (i = 0; i < 21; i++) {
		out[i] = digits[i];
	}
}

/* "<label> time utc <UTC> boot <UTC of the boot>": the calendar clock as the kernel has it (1970 when nothing set it). */
static inline size_t
pagani_diag_time_line(char *line, size_t size, const char *label, int64_t now, int64_t boot)
{
	char text[21];
	size_t used = 0;
	used = pagani_diag_put(line, size, used, label);
	used = pagani_diag_put(line, size, used, " time utc ");
	pagani_diag_utc(now, text);
	used = pagani_diag_put(line, size, used, text);
	used = pagani_diag_put(line, size, used, " boot ");
	pagani_diag_utc(boot, text);
	return pagani_diag_put(line, size, used, text);
}

/* How the daemon's start went. */
enum {
	PAGANI_DIAG_LAUNCH_WAIT = 0,        /* no exec of PAGANI_DIAG_HOST yet */
	PAGANI_DIAG_LAUNCH_REDIRECTED = 1,  /* its first exec runs the daemon */
	PAGANI_DIAG_LAUNCH_NOFILE = 2,      /* the Data seed has no daemon: DumpPanic ran */
	PAGANI_DIAG_LAUNCH_ERROR = 3,       /* the daemon file could not be used (launch_error): DumpPanic ran */
	PAGANI_DIAG_LAUNCH_FAILED = 4,      /* the path could not be replaced (launch_error): that exec failed */
};

struct pagani_diag_status {
	uint32_t uptime;                    /* seconds since boot */
	int launch;                         /* PAGANI_DIAG_LAUNCH_* */
	int launch_error;
	int32_t pid;                        /* the redirected exec's pid */
	uint32_t launch_uptime;
	int activated;                      /* exec_activate_image's result for it: -1 not known */
	int signature;                      /* process_signature's verdict: -1 not known, 0 accepted, else the error */
	uint32_t signature_namespace;       /* its exit reason when refused */
	uint64_t signature_code;
	const char *now;                    /* the name the pid has now, NULL: gone */
	uint64_t csflags;                   /* its code signing flags when alive */
	uint32_t sysctl_lines, spool_lines, dropped, host_execs;
	int spool_error;                    /* 0, or why the spool file was refused (EPERM: not root's regular file) */
	uint32_t key_events, touch_events;  /* PaganiInput: key reports the HID parser accepted, touch frames */
	uint32_t extension_execs, widget_execs;     /* pagani_diag_extension: execs of app extensions, of widget extensions, since boot */
	uint32_t vget_calls, vget_resolved;         /* bindfs_vget (phone-bindfs-vget.patch): file IDs asked of the view, and resolved */
};

/* "<label> up <s> d <state> ..." (see README.md "What it logs"). */
static inline size_t
pagani_diag_status_line(char *line, size_t size, const char *label, const struct pagani_diag_status *s)
{
	size_t used = 0;
	used = pagani_diag_put(line, size, used, label);
	used = pagani_diag_put(line, size, used, " up ");
	used = pagani_diag_put_u64(line, size, used, s->uptime, 0);
	used = pagani_diag_put(line, size, used, " d ");
	switch (s->launch) {
	case PAGANI_DIAG_LAUNCH_WAIT:
		used = pagani_diag_put(line, size, used, "wait");
		break;
	case PAGANI_DIAG_LAUNCH_NOFILE:
		used = pagani_diag_put(line, size, used, "nofile");
		break;
	case PAGANI_DIAG_LAUNCH_ERROR:
	case PAGANI_DIAG_LAUNCH_FAILED:
		used = pagani_diag_put(line, size, used, s->launch == PAGANI_DIAG_LAUNCH_ERROR ? "err " : "fail ");
		used = pagani_diag_put_int(line, size, used, s->launch_error);
		break;
	default:
		used = pagani_diag_put(line, size, used, "pid ");
		used = pagani_diag_put_int(line, size, used, s->pid);
		used = pagani_diag_put(line, size, used, "@");
		used = pagani_diag_put_u64(line, size, used, s->launch_uptime, 0);
		used = pagani_diag_put(line, size, used, " act ");
		if (s->activated < 0) {
			used = pagani_diag_put(line, size, used, "-");
		} else {
			used = pagani_diag_put_int(line, size, used, s->activated);
		}
		used = pagani_diag_put(line, size, used, " sig ");
		if (s->signature < 0) {
			used = pagani_diag_put(line, size, used, "-");
		} else {
			used = pagani_diag_put_int(line, size, used, s->signature);
			if (s->signature) {
				used = pagani_diag_put(line, size, used, " ns ");
				used = pagani_diag_put_u64(line, size, used, s->signature_namespace, 0);
				used = pagani_diag_put(line, size, used, " c 0x");
				used = pagani_diag_put_u64(line, size, used, s->signature_code, 1);
			}
		}
		if (s->now) {
			used = pagani_diag_put(line, size, used, " now ");
			used = pagani_diag_put(line, size, used, s->now);
			used = pagani_diag_put(line, size, used, " cs 0x");
			used = pagani_diag_put_u64(line, size, used, s->csflags, 1);
		} else {
			used = pagani_diag_put(line, size, used, " gone");
		}
		break;
	}
	used = pagani_diag_put(line, size, used, " ln ");
	used = pagani_diag_put_u64(line, size, used, s->sysctl_lines, 0);
	used = pagani_diag_put(line, size, used, " sp ");
	used = pagani_diag_put_u64(line, size, used, s->spool_lines, 0);
	if (s->spool_error) {
		used = pagani_diag_put(line, size, used, "e");
		used = pagani_diag_put_int(line, size, used, s->spool_error);
	}
	used = pagani_diag_put(line, size, used, " dr ");
	used = pagani_diag_put_u64(line, size, used, s->dropped, 0);
	used = pagani_diag_put(line, size, used, " dp ");
	used = pagani_diag_put_u64(line, size, used, s->host_execs, 0);
	used = pagani_diag_put(line, size, used, " key ");
	used = pagani_diag_put_u64(line, size, used, s->key_events, 0);
	used = pagani_diag_put(line, size, used, " tch ");
	used = pagani_diag_put_u64(line, size, used, s->touch_events, 0);
	used = pagani_diag_put(line, size, used, " ax ");
	used = pagani_diag_put_u64(line, size, used, s->extension_execs, 0);
	used = pagani_diag_put(line, size, used, "/");
	used = pagani_diag_put_u64(line, size, used, s->widget_execs, 0);
	used = pagani_diag_put(line, size, used, " vg ");
	used = pagani_diag_put_u64(line, size, used, s->vget_calls, 0);
	used = pagani_diag_put(line, size, used, "/");
	return pagani_diag_put_u64(line, size, used, s->vget_resolved, 0);
}

#endif /* PAGANI_DIAG_H */
