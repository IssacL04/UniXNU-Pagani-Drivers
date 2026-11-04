/*
 * PaganiDiag (phone-diag-sysctl.patch; UniXNU-Pagani-Drivers drivers/PaganiDiag): the morning checks of the OnePlus 13T images in
 * BootProgress's boot log. Off unless boot-arg pagani-diag is not 0, read once at the boot's first exec (launchd's); off, every exec
 * costs one atomic load here and nothing else exists (no sysctl, no thread, no line).
 *
 * On, three things (pagani_diag.h):
 * 1. The daemon's start. launchd of 21H461 takes every job below /System/Library from its signed cache /System/Library/xpc/launchd.plist
 *    on a normal boot, so an overlay adds no job. The first exec of /usr/libexec/DumpPanic by xpcproxy or launchd (job
 *    com.apple.DumpPanic: root, RunAtLoad, no MachServices, restarted only after a crash; it files the previous boot's panic report,
 *    which this board does not keep) runs PAGANI_DIAG_DAEMON in its place when the Data seed has that file. A later exec (the restart
 *    after a crash) runs DumpPanic. That exec has to pass two gates the 2026-10-31 A package did not: the Sandbox platform policy
 *    refuses xpcproxy any program below /private/var outside its short list (the daemon was in /private/var/ngpu/bin; it is on the list
 *    now), and the original AMFI's system-service launch policy refuses an owner-trusted binary on the Data volume (launchd makes every
 *    cached job a system service); pagani_diag_launch_constraints passes the daemon's own image alone, so a diag image needs no global
 *    sheng-no-lc=1. The verdicts are kept: exec_activate_image's result and process_signature's.
 * 2. The daemon's lines: kern.pagani_diag_line (root, write only), or the spool file PAGANI_DIAG_SPOOL when that write is refused,
 *    read every PAGANI_DIAG_TICK_SECONDS; the limits of pagani_diag_take; kern.pagani_diag_dropped counts the refused ones.
 * 3. The kernel's own lines ("diag: k<s> ..."), from a thread at the lowest priority: at 60, 180 and 600 s the daemon's start and
 *    state, PaganiInput's key and touch counters, the app extension execs and bindfs_vget's counts, the process table of the watched
 *    names (only when it changed) and the calendar clock; at 900, 1200, 1500 and 1800 s the start, state and counters and the daemon's
 *    last round summary again. These need no user space, so the start, the keys and the process table are in the log even when the
 *    daemon never ran, and the repeats keep them in the 32 KiB events ring until a late reset.
 * Failures are U lines; nothing here panics. The sysctls are registered at run time (SYSCTL_STRUCT_INIT and sysctl_register_oid
 * under the extensible kern node, as a kext does) and the lock is allocated then, so the patch adds no startup declarations.
 */
#include <kern/clock.h>
#include <sys/reason.h>
#include "pagani_diag.h"
extern void boot_progress_trace(const char *text) __attribute__((weak_import));
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
#define PAGANI_DIAG_PRIORITY        4       /* MAXPRI_THROTTLE (osfmk/kern/sched.h, not exported to BSD): below every user band */
/* PaganiInput's counters. Weak zero definitions as in PaganiArmCpu: the kernel link keeps one word. */
__attribute__((weak)) volatile uint32_t pagani_input_key_events;
__attribute__((weak)) volatile uint32_t pagani_input_touch_events;
/* bindfs_vget's counters: weak here as well, phone-bindfs-vget.patch comes later in the series. */
__attribute__((weak)) volatile uint32_t native_bindfs_vget_calls;
__attribute__((weak)) volatile uint32_t native_bindfs_vget_resolved;

enum {
	PAGANI_DIAG_UNREAD = 0,             /* the boot-arg is read at the first exec */
	PAGANI_DIAG_OFF = 1,
	PAGANI_DIAG_ON = 2,
	PAGANI_DIAG_READING = 3,
};
static uint32_t pagani_diag_state;
static uint32_t pagani_diag_mode;       /* the boot-arg's value */
static lck_grp_t *pagani_diag_lck_grp;
static lck_mtx_t *pagani_diag_lock;     /* the limits, the counters below and the kept summary */
static struct pagani_diag_limit pagani_diag_limit;
static uint32_t pagani_diag_sysctl_lines, pagani_diag_spool_lines;
static char pagani_diag_summary[PAGANI_DIAG_TEXT + 1];
static uint32_t pagani_diag_launched;   /* the first exec of the host has been seen */
static uint32_t pagani_diag_host_execs;
static uint32_t pagani_diag_execs[PAGANI_DIAG_WATCHED];
static uint32_t pagani_diag_extension_execs, pagani_diag_widget_execs;
/* The start, written once by the redirecting exec (and its two verdicts), read by the watch thread under the lock. */
static struct {
	int launch, launch_error, activated, signature;
	int32_t pid;
	uint32_t launch_uptime, signature_namespace;
	uint64_t signature_code;
} pagani_diag_start = {.activated = -1, .signature = -1};
/* The redirected exec's image_params, an identity for the two verdict hooks (never dereferenced): they run for every exec and look at
 * nothing else until it matches. */
static const void *pagani_diag_token;
/* The running daemon's image, for its probes (pagani_diag_launch_constraints): the running daemon holds this vnode. */
static vnode_t pagani_diag_daemon_vp;
static uint32_t pagani_diag_daemon_vid;

static uint32_t
pagani_diag_uptime(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_SEC);
}

/* A line of the daemon (sysctl or spool) through the limits into the log. */
static int
pagani_diag_accept(const char *text, size_t length, int spool)
{
	char line[PAGANI_DIAG_TEXT + 1];
	size_t used = pagani_diag_line(line, text, length);
	if (!used) {
		return EINVAL;
	}
	uint64_t now = mach_absolute_time(), window = 0;
	nanoseconds_to_absolutetime((uint64_t)PAGANI_DIAG_WINDOW_SECONDS * NSEC_PER_SEC, &window);
	lck_mtx_lock(pagani_diag_lock);
	int taken = pagani_diag_take(&pagani_diag_limit, now, window, used);
	bool note = taken != PAGANI_DIAG_TAKEN && !pagani_diag_limit.noted;
	if (note) {
		pagani_diag_limit.noted = 1;
	}
	if (taken == PAGANI_DIAG_TAKEN) {
		if (spool) {
			pagani_diag_spool_lines++;
		} else {
			pagani_diag_sysctl_lines++;
		}
		size_t prefix = sizeof(PAGANI_DIAG_PREFIX) - 1;
		if (pagani_diag_is_summary(line + prefix, used - prefix)) {
			strlcpy(pagani_diag_summary, line + prefix, sizeof(pagani_diag_summary));
		}
	}
	lck_mtx_unlock(pagani_diag_lock);
	if (boot_progress_trace) {
		if (taken == PAGANI_DIAG_TAKEN) {
			boot_progress_trace(line);
		} else if (note) {
			boot_progress_trace(taken == PAGANI_DIAG_OVER_BOOT ? "diag: kernel budget of this boot used, later lines dropped" :
			    "diag: kernel limit of 20 lines in 10 s reached, lines dropped");
		}
	}
	return taken == PAGANI_DIAG_TAKEN ? 0 : taken == PAGANI_DIAG_OVER_BOOT ? ENOSPC : EAGAIN;
}

static int
sysctl_pagani_diag_line SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[PAGANI_DIAG_TEXT];
	if (req->newptr == USER_ADDR_NULL || !kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	size_t length = req->newlen;
	if (length > sizeof(text)) {
		length = sizeof(text);
	}
	int error = SYSCTL_IN(req, text, length);
	if (error) {
		return error;
	}
	return pagani_diag_accept(text, length, 0);
}
static struct sysctl_oid pagani_diag_line_oid = SYSCTL_STRUCT_INIT(_kern, OID_AUTO, pagani_diag_line,
    CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED, NULL, 0, sysctl_pagani_diag_line, "A", "");
static struct sysctl_oid pagani_diag_dropped_oid = SYSCTL_STRUCT_INIT(_kern, OID_AUTO, pagani_diag_dropped,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, &pagani_diag_limit.dropped, 0, sysctl_handle_int, "IU", "");

/* ---- the spool ------------------------------------------------------------------------------------------------------------------ */

static off_t pagani_diag_spool_offset;
static struct pagani_diag_spool pagani_diag_spool_lines_in;
static char pagani_diag_spool_chunk[PAGANI_DIAG_SPOOL_CHUNK];  /* the watch thread's only */
static int pagani_diag_spool_error, pagani_diag_spool_noted;

/* New complete lines of the spool file into the log; the file must be root's regular file (it is in root's /private/var/db). */
static void
pagani_diag_spool_poll(void)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_open(PAGANI_DIAG_SPOOL, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
	if (error) {
		return;                 /* ENOENT: the daemon writes through the sysctl */
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_uid);
	VATTR_WANTED(&va, va_data_size);
	error = vnode_vtype(vp) != VREG ? EPERM : vnode_getattr(vp, &va, ctx);
	if (!error && (!VATTR_IS_SUPPORTED(&va, va_uid) || va.va_uid != 0)) {
		error = EPERM;
	}
	off_t size = !error && VATTR_IS_SUPPORTED(&va, va_data_size) ? (off_t)va.va_data_size : 0;
	if (!error && size < pagani_diag_spool_offset) {
		pagani_diag_spool_offset = 0;   /* truncated: a new start */
		pagani_diag_spool_lines_in.length = 0;
		pagani_diag_spool_lines_in.skipping = 0;
	}
	if (!error && size > PAGANI_DIAG_SPOOL_MAX) {
		size = PAGANI_DIAG_SPOOL_MAX;
	}
	if (!error && size > pagani_diag_spool_offset) {
		off_t want = size - pagani_diag_spool_offset;
		int length = want > PAGANI_DIAG_SPOOL_CHUNK ? PAGANI_DIAG_SPOOL_CHUNK : (int)want;
		int resid = 0;
		error = vn_rdwr(UIO_READ, vp, (caddr_t)pagani_diag_spool_chunk, length, pagani_diag_spool_offset, UIO_SYSSPACE, IO_NOAUTH,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		size_t got = error || resid > length ? 0 : (size_t)(length - resid);
		pagani_diag_spool_offset += (off_t)got;
		for (size_t at = 0; at < got;) {
			char line[PAGANI_DIAG_TEXT + 1];
			size_t used = 0;
			if (pagani_diag_spool_take(&pagani_diag_spool_lines_in, pagani_diag_spool_chunk + at, got - at, line, &used) && line[0]) {
				(void)pagani_diag_accept(line, strlen(line), 1);
			}
			at += used;
		}
	}
	vnode_close(vp, FREAD, ctx);
	if (error && !pagani_diag_spool_noted) {
		pagani_diag_spool_noted = 1;
		pagani_diag_spool_error = error;
		if (boot_progress_trace) {
			char line[96];
			snprintf(line, sizeof(line), "diag: spool %s refused: error %d", PAGANI_DIAG_SPOOL, error);
			boot_progress_trace(line);
		}
	}
}

/* ---- the kernel's own lines ----------------------------------------------------------------------------------------------------- */

static int
pagani_diag_ps_filter(proc_t p, __unused void *arg)
{
	return pagani_diag_watch_index(proc_best_name(p)) >= 0;
}

static int
pagani_diag_ps_callout(proc_t p, void *arg)
{
	struct pagani_diag_ps *ps = arg;
	int index = pagani_diag_watch_index(proc_best_name(p));
	if (index >= 0) {
		if (!ps[index].live++) {
			ps[index].pid = proc_getpid(p);
		}
	}
	return PROC_RETURNED;
}

static void
pagani_diag_status_now(struct pagani_diag_status *s, char *name, size_t name_size)
{
	memset(s, 0, sizeof(*s));
	s->uptime = pagani_diag_uptime();
	lck_mtx_lock(pagani_diag_lock);
	s->launch = pagani_diag_start.launch;
	s->launch_error = pagani_diag_start.launch_error;
	s->pid = pagani_diag_start.pid;
	s->launch_uptime = pagani_diag_start.launch_uptime;
	s->activated = pagani_diag_start.activated;
	s->signature = pagani_diag_start.signature;
	s->signature_namespace = pagani_diag_start.signature_namespace;
	s->signature_code = pagani_diag_start.signature_code;
	s->sysctl_lines = pagani_diag_sysctl_lines;
	s->spool_lines = pagani_diag_spool_lines;
	s->dropped = pagani_diag_limit.dropped;
	lck_mtx_unlock(pagani_diag_lock);
	s->host_execs = os_atomic_load(&pagani_diag_host_execs, relaxed);
	s->spool_error = pagani_diag_spool_error;
	s->key_events = pagani_input_key_events;
	s->touch_events = pagani_input_touch_events;
	s->extension_execs = os_atomic_load(&pagani_diag_extension_execs, relaxed);
	s->widget_execs = os_atomic_load(&pagani_diag_widget_execs, relaxed);
	s->vget_calls = native_bindfs_vget_calls;
	s->vget_resolved = native_bindfs_vget_resolved;
	if (s->launch == PAGANI_DIAG_LAUNCH_REDIRECTED && s->pid > 0) {
		proc_t p = proc_find(s->pid);
		if (p != PROC_NULL) {
			strlcpy(name, proc_best_name(p), name_size);
			s->now = name;
			s->csflags = proc_getcsflags(p);
			proc_rele(p);
		}
	}
}

static void
pagani_diag_round(size_t round)
{
	char label[16], line[PAGANI_DIAG_TEXT + 1], name[64];
	snprintf(label, sizeof(label), "diag: k%u", (unsigned)pagani_diag_rounds[round]);
	struct pagani_diag_status status;
	pagani_diag_status_now(&status, name, sizeof(name));
	pagani_diag_status_line(line, sizeof(line), label, &status);
	boot_progress_trace(line);
	if (round < PAGANI_DIAG_FULL_ROUNDS) {
		/* the watch thread's only; a table like the last round's is not written again */
		static struct pagani_diag_ps ps[PAGANI_DIAG_WATCHED], last_ps[PAGANI_DIAG_WATCHED];
		memset(ps, 0, sizeof(ps));
		proc_iterate(PROC_ALLPROCLIST | PROC_NOWAITTRANS, pagani_diag_ps_callout, ps, pagani_diag_ps_filter, NULL);
		for (size_t i = 0; i < PAGANI_DIAG_WATCHED; i++) {
			ps[i].execs = os_atomic_load(&pagani_diag_execs[i], relaxed);
		}
		for (size_t first = 0; (round == 0 || memcmp(ps, last_ps, sizeof(ps)) != 0) && first < PAGANI_DIAG_WATCHED;) {
			size_t next = pagani_diag_ps_line(line, sizeof(line), PAGANI_DIAG_TEXT, label, ps, PAGANI_DIAG_WATCHED, first);
			if (next == first) {
				break;
			}
			boot_progress_trace(line);
			first = next;
		}
		memcpy(last_ps, ps, sizeof(ps));
		clock_sec_t now = 0, boot = 0;
		clock_usec_t now_us = 0, boot_us = 0;
		clock_get_calendar_microtime(&now, &now_us);
		clock_get_boottime_microtime(&boot, &boot_us);
		pagani_diag_time_line(line, sizeof(line), label, (int64_t)now, (int64_t)boot);
		boot_progress_trace(line);
	} else {
		char summary[PAGANI_DIAG_TEXT + 1];
		lck_mtx_lock(pagani_diag_lock);
		strlcpy(summary, pagani_diag_summary, sizeof(summary));
		lck_mtx_unlock(pagani_diag_lock);
		if (summary[0]) {
			snprintf(line, sizeof(line), "%s again %s", label, summary);
			boot_progress_trace(line);
		}
	}
}

/* The watch: the spool, the silence after the launch and the rounds, at the lowest priority until the last round. */
static void
pagani_diag_watch(__unused void *parameter, __unused wait_result_t result)
{
	size_t round = 0;
	int silence_checked = 0;
	while (round < PAGANI_DIAG_ROUND_COUNT) {
		pagani_diag_spool_poll();
		uint32_t up = pagani_diag_uptime();
		if (!silence_checked) {
			lck_mtx_lock(pagani_diag_lock);
			int redirected = pagani_diag_start.launch == PAGANI_DIAG_LAUNCH_REDIRECTED;
			uint32_t since = pagani_diag_start.launch_uptime, lines = pagani_diag_sysctl_lines + pagani_diag_spool_lines;
			int32_t pid = pagani_diag_start.pid;
			lck_mtx_unlock(pagani_diag_lock);
			if (redirected && up >= since + PAGANI_DIAG_SILENCE_SECONDS) {
				silence_checked = 1;
				if (!lines) {
					char line[64];
					snprintf(line, sizeof(line), "diag: no line from pid %d in %d s", (int)pid, PAGANI_DIAG_SILENCE_SECONDS);
					boot_progress_trace(line);
				}
			}
		}
		while (round < PAGANI_DIAG_ROUND_COUNT && up >= pagani_diag_rounds[round]) {
			pagani_diag_round(round++);
		}
		if (round < PAGANI_DIAG_ROUND_COUNT) {
			delay_for_interval(PAGANI_DIAG_TICK_SECONDS * 1000, 1000 * 1000);
		}
	}
	boot_progress_trace("diag: kernel watch done");
}

/* The boot-arg, once; on, the lock, the sysctls and the watch thread. */
static void
pagani_diag_setup(void)
{
	uint32_t mode = 0;
	if (!boot_progress_trace || !PE_parse_boot_argn("pagani-diag", &mode, sizeof(mode)) || !mode) {
		os_atomic_store(&pagani_diag_state, PAGANI_DIAG_OFF, release);
		return;
	}
	pagani_diag_lck_grp = lck_grp_alloc_init("pagani-diag", LCK_GRP_ATTR_NULL);
	pagani_diag_lock = pagani_diag_lck_grp ? lck_mtx_alloc_init(pagani_diag_lck_grp, LCK_ATTR_NULL) : NULL;
	if (!pagani_diag_lock) {
		boot_progress_trace("diag: off: no lock");
		os_atomic_store(&pagani_diag_state, PAGANI_DIAG_OFF, release);
		return;
	}
	pagani_diag_mode = mode;
	pagani_diag_budget(&pagani_diag_limit, mode);
	sysctl_register_oid(&pagani_diag_line_oid);
	sysctl_register_oid(&pagani_diag_dropped_oid);
	os_atomic_store(&pagani_diag_state, PAGANI_DIAG_ON, release);
	thread_t thread = THREAD_NULL;
	kern_return_t kr = kernel_thread_start_priority(pagani_diag_watch, NULL, PAGANI_DIAG_PRIORITY, &thread);
	char line[160];
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
		snprintf(line, sizeof(line), "diag: on (pagani-diag=%u): daemon %s in place of DumpPanic, checks at 60 180 600 s", mode,
		    PAGANI_DIAG_DAEMON);
	} else {
		snprintf(line, sizeof(line), "diag: on (pagani-diag=%u) without the watch thread: error %d", mode, kr);
	}
	boot_progress_trace(line);
}

static void
pagani_diag_set_start(int launch, int error, int32_t pid, const void *imgp)
{
	lck_mtx_lock(pagani_diag_lock);
	pagani_diag_start.launch = launch;
	pagani_diag_start.launch_error = error;
	pagani_diag_start.pid = pid;
	pagani_diag_start.launch_uptime = pagani_diag_uptime();
	os_atomic_store(&pagani_diag_token, imgp, release);
	lck_mtx_unlock(pagani_diag_lock);
}

/* exec_activate_image, with the saved path: the first exec of PAGANI_DIAG_HOST by xpcproxy or launchd saves PAGANI_DIAG_DAEMON in its
 * place (see above); every exec counts towards the watched names. An error fails the exec. */
static int
pagani_diag_launch(struct image_params *imgp, proc_t p, const char **excpath)
{
	uint32_t state = os_atomic_load(&pagani_diag_state, acquire);
	if (state == PAGANI_DIAG_UNREAD && os_atomic_cmpxchg(&pagani_diag_state, PAGANI_DIAG_UNREAD, PAGANI_DIAG_READING, acq_rel)) {
		pagani_diag_setup();
		state = os_atomic_load(&pagani_diag_state, acquire);
	}
	if (state != PAGANI_DIAG_ON || p == PROC_NULL) {
		return 0;
	}
	int watched = pagani_diag_watch_index(pagani_diag_basename(*excpath));
	if (watched >= 0) {
		os_atomic_inc(&pagani_diag_execs[watched], relaxed);
	}
	int extension = pagani_diag_extension(*excpath);
	if (extension) {
		os_atomic_inc(&pagani_diag_extension_execs, relaxed);
		if (extension > 1) {
			os_atomic_inc(&pagani_diag_widget_execs, relaxed);
		}
	}
	if (strcmp(*excpath, PAGANI_DIAG_HOST) != 0) {
		return 0;
	}
	os_atomic_inc(&pagani_diag_host_execs, relaxed);
	if (os_atomic_load(&pagani_diag_launched, relaxed)) {
		return 0;
	}
	const char *name = proc_best_name(p);
	if (proc_getpid(p) != 1 && strcmp(name, "xpcproxy") != 0 && strcmp(name, "launchd") != 0) {
		return 0;
	}
	if (!os_atomic_cmpxchg(&pagani_diag_launched, 0, 1, relaxed)) {
		return 0;
	}
	vnode_t vp = NULLVP;
	int error = vnode_lookup(PAGANI_DIAG_DAEMON, 0, &vp, vfs_context_current());
	if (!error) {
		if (vnode_vtype(vp) != VREG) {
			error = ENOEXEC;
		}
		vnode_put(vp);
	}
	char line[96];
	if (error) {
		pagani_diag_set_start(error == ENOENT ? PAGANI_DIAG_LAUNCH_NOFILE : PAGANI_DIAG_LAUNCH_ERROR, error, proc_getpid(p), NULL);
		snprintf(line, sizeof(line), "diag: launch pid %d: daemon %s, DumpPanic runs", proc_getpid(p),
		    error == ENOENT ? "absent" : "unusable");
		boot_progress_trace(line);
		return 0;
	}
	exec_reset_save_path(imgp);
	error = exec_save_path(imgp, CAST_USER_ADDR_T(PAGANI_DIAG_DAEMON), UIO_SYSSPACE, excpath);
	pagani_diag_set_start(error ? PAGANI_DIAG_LAUNCH_FAILED : PAGANI_DIAG_LAUNCH_REDIRECTED, error, proc_getpid(p), error ? NULL : imgp);
	if (error) {
		snprintf(line, sizeof(line), "diag: launch pid %d failed %d", proc_getpid(p), error);
	} else {
		snprintf(line, sizeof(line), "diag: launch pid %d DumpPanic -> paganidiag", proc_getpid(p));
	}
	boot_progress_trace(line);
	return error;
}

/* The end of exec_activate_image: its result for the redirected exec (EPERM: a MAC policy refused it, Test/check-exec-policy.py). */
static void
pagani_diag_activated(struct image_params *imgp, proc_t p, int error)
{
	if (os_atomic_load(&pagani_diag_token, acquire) != imgp || imgp == NULL || p == PROC_NULL) {
		return;
	}
	int noted = 0;
	lck_mtx_lock(pagani_diag_lock);
	if (pagani_diag_token == imgp && pagani_diag_start.pid == proc_getpid(p) && pagani_diag_start.activated < 0) {
		pagani_diag_start.activated = error;
		if (error) {
			os_atomic_store(&pagani_diag_token, NULL, release);
			noted = 1;
		}
	}
	lck_mtx_unlock(pagani_diag_lock);
	if (noted) {
		char line[64];
		snprintf(line, sizeof(line), "diag: launch pid %d refused: exec error %d", proc_getpid(p), error);
		boot_progress_trace(line);
	}
}

/* process_signature's launch constraint check: skipped for the daemon's own image alone, the redirected exec and the probes it starts
 * (the same vnode, held by the running daemon); everything else as ever, so a diag image differs from a daily one by the daemon. */
static int
pagani_diag_launch_constraints(proc_t p, struct image_params *imgp, os_reason_t *reason)
{
	if (imgp != NULL && imgp->ip_vp != NULLVP) {
		if (os_atomic_load(&pagani_diag_token, acquire) == imgp) {
			pagani_diag_daemon_vid = vnode_vid(imgp->ip_vp);
			os_atomic_store(&pagani_diag_daemon_vp, imgp->ip_vp, release);
			return 0;
		}
		if (os_atomic_load(&pagani_diag_daemon_vp, acquire) == imgp->ip_vp && pagani_diag_daemon_vid == vnode_vid(imgp->ip_vp)) {
			return 0;
		}
	}
	return mac_proc_check_launch_constraints(p, imgp, reason);
}

/* process_signature, before it acts on its verdict: the verdict for the redirected exec (0, or the error and the exit reason that
 * will kill it: launch constraints, signature, MAC policy). */
static void
pagani_diag_signature(struct image_params *imgp, proc_t p, int error, os_reason_t reason)
{
	if (os_atomic_load(&pagani_diag_token, acquire) != imgp || imgp == NULL || p == PROC_NULL) {
		return;
	}
	int noted = 0;
	lck_mtx_lock(pagani_diag_lock);
	if (pagani_diag_token == imgp && pagani_diag_start.pid == proc_getpid(p) && pagani_diag_start.signature < 0) {
		pagani_diag_start.signature = error;
		pagani_diag_start.signature_namespace = reason != OS_REASON_NULL ? reason->osr_namespace : 0;
		pagani_diag_start.signature_code = reason != OS_REASON_NULL ? reason->osr_code : 0;
		os_atomic_store(&pagani_diag_token, NULL, release);
		noted = error != 0;
	}
	lck_mtx_unlock(pagani_diag_lock);
	if (noted) {
		char line[128];
		snprintf(line, sizeof(line), "diag: launch pid %d refused: signature error %d reason %u/0x%llx", proc_getpid(p), error,
		    reason != OS_REASON_NULL ? reason->osr_namespace : 0, reason != OS_REASON_NULL ? reason->osr_code : 0ULL);
		boot_progress_trace(line);
	}
}

