// SPDX-License-Identifier: BSD-2-Clause-Patent
// The stall probe (boot-arg pagani-diag not 0; the M and Q records of the ramoops log, RamoopsLog.h). One run has to answer what the key
// threads were doing during a severe hitch or a key that took seconds, and whether a thread could run and did not. This header is the part
// free of kernel headers, host tested by Test/stall.c and, with threads under TSan, Test/stall-threads.c; BootProgress.c holds the glue and
// the device repository's patches/xnu/phone-stall-probe.patch the hooks and readers. Off (no pagani-diag): nothing is allocated or started,
// the hooks are one load of pagani_stall_on, a frame costs one load of stall_mode.
//
// What runs where, and on what it relies:
//   Every dispatch: thread_dispatch -> boot_progress_stall_run -> stall_note_run, at splsched on the CPU itself with the new thread locked.
//     No lock, no read-modify-write: each stall_cpu has one writer, that CPU, so it loads and stores (relaxed, for the readers). The
//     per-band counters only grow. The largest latency of a band is kept per second, in the epoch's half of max_*: the observer clears the
//     half that is not in use, turns the epoch and reads the half it left; a CPU that loaded the old epoch a moment before still lands in
//     the half being read. What is on core is a small seqlock of the same writer, so a reader never mixes two threads' fields.
//   Idle and IPIs: stall_note_idle on the CPU itself from cpu_idle. stall_note_ipi_send from any CPU in cpu_signal_internal with interrupts
//     masked, before the SGI goes out: an atomic add and, while none is pending, the target's pending stamp. stall_note_ipi_recv on the
//     target in cpu_signal_handler_internal takes the stamp with an exchange: a stamp left there says an IPI has not landed. Two senders at
//     once keep either stamp; a handler that eats the stamp of a later SGI only loses one latency sample. Neither can leave a stamp behind
//     unless an SGI really did not arrive.
//     Kernel threads count too, in a band of their own (STALL_KERNEL), so "a thread could run and did not" has no blind spot left in the kernel.
//   The sampler: a thread call of its own at THREAD_CALL_PRIORITY_HIGH, every STALL_TICK_MS while the display updates or a touch or a kbt pair
//     is recent, not at all otherwise. A thread call in thread context, never with a lock of ours held across anything but the kbt table.
//     stall_watch, stall_gap, stall_limit and stall_busy belong to it alone. It reads other CPUs' scheduler state without locks (a racy
//     word is only a wrong sample: stall_watch_cpu asks for two ticks in a row) and other threads only through references it holds:
//       - a task reference (task_reference, taken under tasks_threads_lock, try-lock: a contended list is left for the next tick) keeps the
//         task and its map pointer's holder, a thread reference keeps the thread structure, also after it exited: `active` turns 0, the
//         sampler drops both and looks them up again at most once a second;
//       - the thread's state comes under its thread lock at splsched (boot_progress_stall_thread_read), kernel return addresses too: with the
//         lock held the thread cannot be dispatched, so a thread off core keeps its kernel stack, and the walk stays inside that stack. A
//         thread on core is never walked. A blocked thread's user registers sit in its PCB, stable while it is in the kernel;
//       - a thread on core: PCB values are stale, so an immediate cross-call (SIGPxcallImm: fails at once when one is pending, cannot time
//         out into a panic) makes its CPU copy the interrupted state into a static slot. The slot is static because the sampler stops waiting
//         after STALL_XCALL_US (a CPU with interrupts masked) and the handler may run later: it only compares the current thread's pointer and
//         writes the slot. The slot stays busy until that write came, so a late answer never lands in a new request;
//       - user memory: boot_progress_stall_map takes a map reference, and boot_progress_stall_map_read reads through pmap_find_phys (a spin
//         lock of a few instructions) and ml_phys_read, managed pages only (this kernel has no physical aperture: a per-CPU copy window, preemption
//         off and a TLB invalidate each way for each word, at most 32 words a sample). No fault, no sleep, no vm_map lock: the sampler must not
//         queue behind the map lock or the page fault the stalled thread itself is stuck in, and vm_map_read_user would. A page that is not
//         resident ends the walk with end=read; a page that changes under the read gives a chain crash_walk's checks stop.
//       - file mappings (Q img) take the map lock and vnode locks, so a thread call of their own at a low priority does them: if that one
//         blocks, nothing else waits for it.
//     Panic or reset: stall_halted is checked at the top of a tick, before every target and before a cross-call. boot_progress_panic also sets
//     it first thing and turns the hooks off. A halted sampler does not reschedule and leaves stall_armed set, so nothing arms it again; it
//     drops no references, as a panic must not take locks.
//   The App on screen (a gap's third target): RunningBoard gives it the focal role (setpriority(PRIO_DARWIN_ROLE, PRIO_DARWIN_ROLE_UI_FOCAL), which is
//     TASK_FOREGROUND_APPLICATION), and every App switch moves it, so each gap sample asks again: boot_progress_stall_foreground walks the task list
//     under the same try-lock as the lookups, reads the role word and takes no reference. Only a pid that changed costs a lookup and a Q map record.
//   Records: ramoops_log_line, its own bounded lock, from thread context. Every Q record counts against a stall_limit, STALL_LINES_PER_S a second
//     each: one for the keyboard (STALL_LINES_KBT for a run), so a run of gaps cannot starve a slow key, one for gaps only the frames before tell
//     (STALL_LINES_TAIL), so stalls that have a sign cannot starve them or they those, and one for all the rest (STALL_LINES_MAX), of which the less
//     a stall is sure to be one, the less it may use (stall_level_lines): what is left of the run always reaches the long gaps. A Q record is
//     130 bytes on average (run 20261101-110201): all three limits at their rate are 60 lines a second, 8 KiB/s, the 32 KiB events ring turns over
//     in 4 s, and the UFS log's writer looks every 500 ms and takes a round at half of UFS_LOG_ADVANCE, so no two rounds miss a turn. All three used
//     up are 4800 lines, 620 KiB, of a window that keeps 11 MiB of any ring. M takes 0.6 KiB/s of the console ring, its other tenants 0.6 KiB/s:
//     without the UFS log that ring (28 KiB) holds the last 25 s.
//
// Trigger for a gap (stall_gap_step). The end of an animation looks like a hitch: the last frame came on time and no frame follows. What tells a
// stall is that something waits for a frame: a touch seen after the newest frame was, a kbt pair still open (both STALL_SIGN_SURE: they stay until
// the next frame), or a GPU submission seen after that frame (STALL_SIGN_GPU: it could be work that is never shown, so it counts while recent and
// spends STALL_LEVEL_LOW). Seen after: a change of the two against their value at that moment, so a frame's own submission or touch does not count.
// From the first sign the stall's clock runs (that is how a tap on a screen idle for a minute starts a stall at all) and stall_gap_ms are thresholds
// of it, each one sample, up to those of a multi-second hitch, so its middle and end are sampled too. The sampler stays armed for a sure sign.
// Without any sign only the frames before tell: with STALL_CADENCE_FRAMES of them in the STALL_ACTIVE_MS before the newest and a touch or a
// submission in the last STALL_ACTIVE_MS, it may be a stalled animation or the end of one; one sample at the first threshold, as STALL_LEVEL_TAIL.
// A blinking cursor has neither sign nor cadence: no sample, however long. When all thresholds are used up, a touch after the last of
// them starts a new stall, still without a frame. The first threshold is 110 ms and not 100: SpringBoard's clock icon runs a 10 Hz display link, and
// on the home screen a frame every 100 ms is all there is, with a finger on the glass too (21H461, +[SBHClockApplicationIconImageView initialize]).
// Trigger for kbt (stall_kbt_*): a dn, up or in pair still open STALL_KBT_FIRST_MS after its begin, then every STALL_KBT_NEXT_MS, less and less
// often the longer it stays open (stall_kbt_wait_ms), until it ends. The pairs are what phone-stall-probe.patch's keyboard trace tells: the
// begins in its queue, and the ends of those; a repeated touchUp: has a begin and an end, neither reaches this table.
//
// Records (kind M, console ring: once a second until the first frame, whatever else is quiet: the boot's long waits have no frame and no touch to
// keep them in view; then while the sampler is awake and for STALL_M_TAIL_S after the last frame or touch: a hitch nothing
// waits on, an app opened and left alone, outlives the sampler. In such a second nobody watched: tick, idleq and stuck read 0, free is the moment's
// and not a minimum. A hitch longer than that shows no M after it. Kind Q, events ring). Times in ms, ticks nowhere:
//   M lat fg <dispatches>/<largest ms>/<over 10>/<over 50>/<over 200>[ <name>[<pid>]/<tid hex>/<pri>] def ... bg ... k ...[ <tid hex>/<pri>]
//                                    fg: sched pri >= 47, def: >= 20, bg: below, user threads; k: kernel threads; runnable to on core. All four
//                                    at their longest do not fit with their threads: the numbers always do, a thread only with the room left
//   M cpu idle a,b,c,.. ipis a,b,.. ipir a,b,.. tmr a,b,..            per CPU: idle entries, IPIs sent to it, IPIs taken, timer interrupts
//   M ipi max a,b,.. idleq a,b,.. stuck a,b,.. tick <longest sampler interval> img <mappings found>
//                                    the slowest IPI to each CPU; ticks that CPU stayed idle over a non-empty queue; IPIs pending too long
//   M io rd <reads> <ms> vfy <ms> free <min> avail <n> flt <n> pin <n> cmp <n> dcmp <n> pw <n> t1 <n> t2 <n> t3 <n>
//                                    q <used>/<over rate>/<over budget> kq <the same of the keyboard's limit> tq <of the frames-only gaps'>
//                                    pw: vm_page_wait calls that went to sleep
//   M mem wire <p> int <p> ext <p> cmp <resident p>/<held p> thr <p> pur <p> zone <p> ram <bytes> gpu <MiB>/<peak MiB>
//                                    every STALL_MEM_EVERY_S of a second that has records, about 3% of the console ring: the account changes
//                                    slowly, free and avail have their line every second. Pages of 4 KiB: wired, pageable anonymous, pageable file,
//                                    the compressor's resident pages and the pages held in them, throttled, purgeable, zones; ram: the Data
//                                    disk's resident bytes (0 without NativeRamBlock); gpu: the GPU pool in use and its peak
//   Q s=<id> why=gapN|dn|up|in|idleq|ipi st=<stall> n=<sample of it> age=<ms> tch=<ms|-> gpu=<ms|-> fb=<frames before> [since=<ms> ask=<0|1|2> fg=<n>]
//                                    [pid=<pid> <name> open=<n>] [cpu=<n>]
//                                    age: since the stall began. gaps: since=ms since the newest frame; ask=the stall's sign: 0 only the frames
//                                    before tell, 1 a GPU submission, 2 a touch or an open kbt pair; fg=tasks in the focal role but SpringBoard
//                                    and backboardd, -1 the list was busy: 0 is the home screen, and a gap sample then has no fg target
//   Q s=<id> th <tag> pid=<pid> <name> tid=0x<tid> st=<C|R|W|U|T|?> pri=<sched>/<base> cpu=<n|-> wt=<ms> [x=ok|to|mv|no] hint=<n>[ ev=0x<event>]
//                                    [ sc=<nr>][ to=<ms>][ o=0x<x1>][ cont=k:<addr>][ k=k:<addr>,..]
//   Q s=<id> bt <tag> pid=<pid> pc=<token> lr=<token>[ esr=0x.. far=0x..] <token>.. end=<why>[@0x<fp>]
//   Q s=<id> no <tag>                 the target has no process or thread now
//   Q s=<id> cpu <n>:<name>/<tid hex>/<pri>/<ms on core> ... rq=<n> rt=<n>   four CPUs a record; -/<ms> idle, ~/<ms> idle and chosen, x off
//   Q map pid=<pid> <name> slide=0x.. main=0x..            once for each target looked up
//   Q img pid=<pid> start=0x.. end=0x.. off=0x.. <file>    the file mapping of a frame outside the shared cache
// Tags: rs backboardd's render server thread, sb SpringBoard's main thread, fg the main thread of the App on screen (gaps: the newest task of
// the focal role), app the main thread of the App of a kbt pair, kbd kbd's busiest thread.
// st: C on core, R runnable, W waiting, U waiting uninterruptibly, T terminating. wt: since it went off core (W, U), since it became
// runnable (R), since it went on core (C). hint: XNU's block_hint_t. Address tokens are CrashTrace.h's; k: the low 32 bits of an unslid
// kernel address (all of them start 0xfffffff0): cont= the continuation of a thread blocked without a stack, k= the return addresses of one
// blocked with its stack (or preempted), kpc= where a thread on core was in the kernel.
//
// Pure C: no platform calls, no floating point, no aggregate copies, no zeroing of large locals (CrashTrace.h says why).
#ifndef PAGANI_STALL_PROBE_H
#define PAGANI_STALL_PROBE_H
#include <stdint.h>
#include "CrashTrace.h"

// ---- what XNU fills in (phone-stall-probe.patch keeps an identical copy in osfmk/kern/bsd_kern.c; boot_progress_stall_abi catches a mismatch) ---
#define BOOT_PROGRESS_STALL_VERSION 2u
enum { BOOT_PROGRESS_STALL_NAME = 17, BOOT_PROGRESS_STALL_KFRAMES = 5 };

// A thread going on core. In thread_dispatch: at splsched, the thread locked.
struct boot_progress_stall_run {
    uint64_t stamp;                         // processor->last_dispatch
    uint64_t latency;                       // ticks from runnable to on core
    uint64_t tid;                           // thread_id
    const char *name;                       // the process name; NULL: a kernel or idle thread
    int32_t pid;                            // -1 the idle thread, 0 a kernel thread
    int32_t pri;                            // sched_pri
    uint32_t cpu;
    uint32_t user;                          // a user thread: its band goes by pri; pid 0 without it: the kernel's band
};

// A thread's user state. The caller says which: its PCB, or the interrupted state on the CPU it runs on.
struct boot_progress_stall_regs {
    uint64_t pc, lr, fp, sp, far;
    uint64_t x[8];
    uint64_t x16;                           // the system call number while it is in one
    uint32_t esr, cpsr;
    uint32_t valid;                         // 0: no 64-bit user state
    uint32_t kernel;                        // the interrupted context was the kernel's: kpc holds its pc, the rest is the PCB
    uint64_t kpc;
};

struct boot_progress_stall_thread {
    uint64_t tid, wait_event, last_run, made_runnable;
    uint64_t continuation;                  // unslid
    uint64_t kframe[BOOT_PROGRESS_STALL_KFRAMES];   // unslid return addresses of a thread off core with a kernel stack, past the scheduler's own
    uint32_t kframes;
    uint32_t state;                         // thread->state (TH_* bits)
    uint32_t hint;                          // block_hint
    uint32_t active;                        // 0: it exited, drop it
    int32_t sched_pri, base_pri;
    int32_t cpu;                            // the CPU it is on, -1: off core
    int32_t reserved;
    struct boot_progress_stall_regs user;
};

struct boot_progress_stall_task {
    int32_t pid;                            // without a task: 0 it is not on the list, -1 the list was busy
    char name[BOOT_PROGRESS_STALL_NAME];
    uint64_t cache_start, cache_end, cache_slide;   // the shared region; end 0: none
    uint64_t main_header, user_limit;
};

enum { STALL_CPU_OFF, STALL_CPU_IDLE, STALL_CPU_DISPATCHING, STALL_CPU_RUNNING };

struct boot_progress_stall_cpu {
    uint32_t kind;                          // STALL_CPU_*
    uint32_t on_idle;                       // the idle thread is its active thread
    int32_t runq;                           // its bound queue and the pset's clutch root
    int32_t rt;                             // the pset's realtime queue
};

struct boot_progress_stall_vm {
    uint64_t faults, pageins, compressions, decompressions;
    uint64_t throttled[4];                  // [tier]: I/O throttle sleeps
    uint64_t page_waits;                    // vm_page_wait calls that went to sleep
};

// The memory account (M mem), pages: racy words, a picture of the moment.
struct boot_progress_stall_mem {
    uint32_t wired, internal, external;     // pageable anonymous, pageable file
    uint32_t compressor, compressed;        // the compressor's resident pages, the pages held in them
    uint32_t throttled;                     // anonymous pages pageout may not look at while there is no compressor
    uint32_t purgeable;
    uint32_t zone;
};

#define BOOT_PROGRESS_STALL_ABI ((uint32_t)(BOOT_PROGRESS_STALL_VERSION << 24 | sizeof(struct boot_progress_stall_thread) << 12 | \
    (sizeof(struct boot_progress_stall_run) + sizeof(struct boot_progress_stall_task) + sizeof(struct boot_progress_stall_cpu) + \
     sizeof(struct boot_progress_stall_vm) + sizeof(struct boot_progress_stall_mem))))

// ---- a sample's question to the CPU a thread runs on ----------------------------------------------------------------------------------------
// One sampler asks, the handler on that CPU answers, maybe after the sampler stopped waiting. Static, one per CPU: the answer needs somewhere to land
// whenever it comes, and a slot asked once takes no new question before its answer came, so a late answer never lands in a newer question.
enum { STALL_SLOT_FREE, STALL_SLOT_ASKED, STALL_SLOT_ANSWERED };

struct stall_slot {
    void *want;
    uint32_t state, found;
    struct boot_progress_stall_regs regs;
};

// 0 while the last question is unanswered.
static inline int stall_slot_ask(struct stall_slot *s, void *thread)
{
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == STALL_SLOT_ASKED) return 0;
    __atomic_store_n(&s->want, thread, __ATOMIC_RELAXED);
    __atomic_store_n(&s->found, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s->state, STALL_SLOT_ASKED, __ATOMIC_RELEASE);
    return 1;
}

// The call did not go out: no answer will come.
static inline void stall_slot_cancel(struct stall_slot *s) {__atomic_store_n(&s->state, STALL_SLOT_FREE, __ATOMIC_RELEASE);}

static inline void *stall_slot_wanted(const struct stall_slot *s) {return __atomic_load_n(&s->want, __ATOMIC_RELAXED);}

// The handler, after it filled s->regs: `found` 1 when it was that thread.
static inline void stall_slot_answer(struct stall_slot *s, uint32_t found)
{
    __atomic_store_n(&s->found, found, __ATOMIC_RELAXED);
    __atomic_store_n(&s->state, STALL_SLOT_ANSWERED, __ATOMIC_RELEASE);
}

static inline int stall_slot_answered(const struct stall_slot *s) {return __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == STALL_SLOT_ANSWERED;}
static inline uint32_t stall_slot_found(const struct stall_slot *s) {return __atomic_load_n(&s->found, __ATOMIC_RELAXED);}

// ---- host-side limits and thresholds ------------------------------------------------------------------------------------------------------
enum {
    STALL_CPUS = 8,
    STALL_BANDS = 4,
    STALL_FG = 0, STALL_DEF = 1, STALL_BG = 2, STALL_KERNEL = 3,
    STALL_PRI_FG = 47, STALL_PRI_DEF = 20,
    STALL_NAME = 16,                        // characters of a process name kept
    STALL_TICK_MS = 10,                     // the sampler's period while armed
    STALL_ARMED_MS = 2000,                  // armed this long after the last frame, touch or open pair
    STALL_M_TAIL_S = 10,                    // the M records go on this long after the last frame or touch, with the sampler asleep
    STALL_MEM_EVERY_S = 5,                  // M mem: 150 bytes each, 30 bytes/s against M's 560
    STALL_XCALL_US = 500,                   // how long a sample waits for a running thread's CPU
    STALL_FRAMES = 8,                       // return addresses of a sample
    STALL_LOOKUP_MS = 1000,                 // a lost target is looked up at most this often
    STALL_LINES_PER_S = 20,                 // of each limit
    STALL_BURST_S = 2,                      // what a limit saves up: a stall's first three samples, 27 lines in 290 ms, after a quiet
    STALL_LINES_MAX = 2000,
    STALL_LINES_TAIL = 800,                 // a gap only the frames before tell: all of the limit of its own
    STALL_LINES_LOW = 1200,                 // a stall at the first threshold
    STALL_LINES_MID = 1600,                 // a stall at 200 ms
    STALL_LINES_KBT = 2000,                 // the keyboard's own limit
    STALL_LEVEL_TAIL = 0, STALL_LEVEL_LOW = 1, STALL_LEVEL_MID = 2, STALL_LEVEL_HIGH = 3, STALL_LEVEL_KBT = 4, STALL_LEVELS = 5,
    STALL_GAP_LEVELS = 6,
    STALL_ACTIVE_MS = 500,
    STALL_CADENCE_FRAMES = 3,
    STALL_KBT_DN = 0, STALL_KBT_UP = 1, STALL_KBT_IN = 2, STALL_KBT_GROUPS = 3,
    STALL_KBT_SLOTS = 8,
    STALL_KBT_BEGINS = 4,
    STALL_KBT_FIRST_MS = 300,
    STALL_KBT_NEXT_MS = 500,                // the first STALL_KBT_STEP samples, then twice that, then four times
    STALL_KBT_STEP = 4,
    STALL_KBT_SAMPLES = 64,                 // a pair whose end never comes is given up after this many samples, about two minutes
    STALL_HUNGRY_COUNT = 2,                 // ticks in a row an idle CPU sees a non-empty queue before they count
    STALL_HUNGRY_TICKS = 5,                 // ... before a sample
    STALL_IPI_STUCK_MS = 20,                // an IPI pending this long counts as stuck, and is sampled
    STALL_BUSY_THREADS = 32,
    STALL_KNOWN = 24,                       // file mappings already reported
    STALL_LINE = CRASH_LINE,
};
static const uint16_t stall_gap_ms[STALL_GAP_LEVELS] = {110, 200, 400, 1000, 3000, 8000};
static const uint16_t stall_level_lines[STALL_LEVELS] = {STALL_LINES_TAIL, STALL_LINES_LOW, STALL_LINES_MID, STALL_LINES_MAX, STALL_LINES_KBT};
static const char *const stall_kbt_names[STALL_KBT_GROUPS] = {"dn", "up", "in"};

// ---- per CPU: dispatch, idle, IPIs --------------------------------------------------------------------------------------------------------
#define STALL_LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define STALL_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

struct stall_band {
    uint64_t n, over10, over50, over200;    // only grow
    uint64_t max_ticks[2], max_tid[2], max_name[2][2];  // [epoch & 1]
    int32_t max_pid[2], max_pri[2];
};

struct stall_cpu {
    struct stall_band band[STALL_BANDS];
    uint32_t seq;                           // odd while the on-core fields change
    int32_t pid, pri, name_pid;             // name_pid: whose name is in name[]
    uint64_t tid, since;
    uint64_t name[2];
    uint64_t idle;                          // idle entries
    uint64_t sent, recv;                    // IPIs to this CPU, taken by it
    uint64_t pending;                       // the stamp of the oldest IPI not taken
    uint64_t ipi_max[2];                    // [epoch & 1]: ticks
} __attribute__((aligned(128)));

struct stall_state {
    struct stall_cpu cpu[STALL_CPUS];
    uint32_t epoch;
    uint64_t t10, t50, t200;                // ticks
};

static inline void stall_state_init(struct stall_state *s, uint64_t hz)
{
    STALL_STORE(&s->t10, hz / 100);
    STALL_STORE(&s->t50, hz / 20);
    STALL_STORE(&s->t200, hz / 5);
}

static inline unsigned stall_band_of(int32_t pri) {return pri >= STALL_PRI_FG ? STALL_FG : pri >= STALL_PRI_DEF ? STALL_DEF : STALL_BG;}

// The first STALL_NAME characters as two words (zero-padded), so a reader takes them with atomic loads.
static inline void stall_name_words(const char *name, uint64_t *a, uint64_t *b)
{
    uint64_t w[2] = {0, 0};
    for (unsigned i = 0; name && i < STALL_NAME && name[i]; i++) w[i / 8] |= (uint64_t)(uint8_t)name[i] << (i % 8 * 8);
    *a = w[0];
    *b = w[1];
}

static inline void stall_words_name(uint64_t a, uint64_t b, char *out)
{
    for (unsigned i = 0; i < STALL_NAME; i++) out[i] = (char)(uint8_t)((i < 8 ? a : b) >> (i % 8 * 8));
    out[STALL_NAME] = 0;
}

// On the CPU itself, at splsched: what went on core, and for any thread but the idle thread how long it waited to.
static inline void stall_note_run(struct stall_state *s, const struct boot_progress_stall_run *r)
{
    if (r->cpu >= STALL_CPUS) return;
    struct stall_cpu *c = &s->cpu[r->cpu];
    const uint32_t seq = STALL_LOAD(&c->seq);
    __atomic_store_n(&c->seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    STALL_STORE(&c->pid, r->pid);
    STALL_STORE(&c->pri, r->pri);
    STALL_STORE(&c->tid, r->tid);
    STALL_STORE(&c->since, r->stamp);
    uint64_t a, b;
    if (STALL_LOAD(&c->name_pid) != r->pid) {   // a CPU mostly switches between a few threads of one process and its idle thread
        stall_name_words(r->name, &a, &b);
        STALL_STORE(&c->name[0], a);
        STALL_STORE(&c->name[1], b);
        STALL_STORE(&c->name_pid, r->pid);
    }
    __atomic_store_n(&c->seq, seq + 2, __ATOMIC_RELEASE);
    if (!r->user && r->pid < 0) return;         // the idle thread
    struct stall_band *band = &c->band[r->user ? stall_band_of(r->pri) : STALL_KERNEL];
    const uint64_t latency = r->latency;
    STALL_STORE(&band->n, STALL_LOAD(&band->n) + 1);
    if (latency >= STALL_LOAD(&s->t10)) {
        STALL_STORE(&band->over10, STALL_LOAD(&band->over10) + 1);
        if (latency >= STALL_LOAD(&s->t50)) {
            STALL_STORE(&band->over50, STALL_LOAD(&band->over50) + 1);
            if (latency >= STALL_LOAD(&s->t200)) STALL_STORE(&band->over200, STALL_LOAD(&band->over200) + 1);
        }
    }
    const unsigned e = STALL_LOAD(&s->epoch) & 1;
    if (latency > STALL_LOAD(&band->max_ticks[e])) {
        STALL_STORE(&band->max_ticks[e], latency);
        STALL_STORE(&band->max_tid[e], r->tid);
        STALL_STORE(&band->max_pid[e], r->pid);
        STALL_STORE(&band->max_pri[e], r->pri);
        STALL_STORE(&band->max_name[e][0], STALL_LOAD(&c->name[0]));
        STALL_STORE(&band->max_name[e][1], STALL_LOAD(&c->name[1]));
    }
}

static inline void stall_note_idle(struct stall_state *s, unsigned cpu)
{
    if (cpu < STALL_CPUS) STALL_STORE(&s->cpu[cpu].idle, STALL_LOAD(&s->cpu[cpu].idle) + 1);
}

// Before the SGI to `cpu` goes out; any CPU, interrupts masked.
static inline void stall_note_ipi_send(struct stall_state *s, unsigned cpu, uint64_t now)
{
    if (cpu >= STALL_CPUS) return;
    struct stall_cpu *c = &s->cpu[cpu];
    __atomic_fetch_add(&c->sent, 1, __ATOMIC_RELAXED);
    if (!STALL_LOAD(&c->pending)) STALL_STORE(&c->pending, now ? now : 1);
}

// On `cpu`, in its IPI handler.
static inline void stall_note_ipi_recv(struct stall_state *s, unsigned cpu, uint64_t now)
{
    if (cpu >= STALL_CPUS) return;
    struct stall_cpu *c = &s->cpu[cpu];
    STALL_STORE(&c->recv, STALL_LOAD(&c->recv) + 1);
    const uint64_t since = __atomic_exchange_n(&c->pending, 0, __ATOMIC_RELAXED);
    const unsigned e = STALL_LOAD(&s->epoch) & 1;
    if (since && now > since && now - since > STALL_LOAD(&c->ipi_max[e])) STALL_STORE(&c->ipi_max[e], now - since);
}

// What is on core now; 0 when four tries met the writer (the caller shows '?').
struct stall_oncore {
    int32_t pid, pri;
    uint64_t tid, since;
    char name[STALL_NAME + 1];
};

static inline int stall_oncore_read(const struct stall_cpu *c, struct stall_oncore *out)
{
    for (unsigned tries = 0; tries < 4; tries++) {
        const uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        const int32_t pid = STALL_LOAD(&c->pid), pri = STALL_LOAD(&c->pri), name_pid = STALL_LOAD(&c->name_pid);
        const uint64_t tid = STALL_LOAD(&c->tid), since = STALL_LOAD(&c->since);
        const uint64_t a = STALL_LOAD(&c->name[0]), b = STALL_LOAD(&c->name[1]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (STALL_LOAD(&c->seq) != seq) continue;
        out->pid = pid;
        out->pri = pri;
        out->tid = tid;
        out->since = since;
        stall_words_name(name_pid == pid ? a : 0, name_pid == pid ? b : 0, out->name);
        return 1;
    }
    return 0;
}

// ---- one second of it (the 1 Hz observer alone) --------------------------------------------------------------------------------------------
struct stall_band_view {
    uint64_t n, over10, over50, over200;
    uint64_t max_ticks, tid;
    int32_t pid, pri;
    char name[STALL_NAME + 1];
};

struct stall_second {
    struct stall_band_view band[STALL_BANDS];
    uint32_t idle[STALL_CPUS], sent[STALL_CPUS], recv[STALL_CPUS];
    uint64_t ipi_max[STALL_CPUS];           // ticks
};

// The totals of the previous pass.
struct stall_seen {
    uint64_t band[STALL_BANDS][4];
    uint64_t idle[STALL_CPUS], sent[STALL_CPUS], recv[STALL_CPUS];
};

static inline uint32_t stall_delta32(uint64_t value, uint64_t *seen)
{
    const uint64_t delta = value - *seen;
    *seen = value;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

static inline void stall_second_take(struct stall_state *s, struct stall_seen *seen, struct stall_second *out)
{
    const uint32_t epoch = STALL_LOAD(&s->epoch);
    const unsigned cur = epoch & 1, other = cur ^ 1;
    // The half about to come into use: its last writers are a second old.
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        struct stall_cpu *cpu = &s->cpu[c];
        STALL_STORE(&cpu->ipi_max[other], 0);
        for (unsigned b = 0; b < STALL_BANDS; b++) {
            struct stall_band *band = &cpu->band[b];
            STALL_STORE(&band->max_ticks[other], 0);
            STALL_STORE(&band->max_tid[other], 0);
            STALL_STORE(&band->max_pid[other], 0);
            STALL_STORE(&band->max_pri[other], 0);
            STALL_STORE(&band->max_name[other][0], 0);
            STALL_STORE(&band->max_name[other][1], 0);
        }
    }
    __atomic_store_n(&s->epoch, epoch + 1, __ATOMIC_RELEASE);
    for (unsigned b = 0; b < STALL_BANDS; b++) {
        struct stall_band_view *v = &out->band[b];
        uint64_t totals[4] = {0, 0, 0, 0}, a = 0, c2 = 0;
        v->max_ticks = 0;
        v->tid = 0;
        v->pid = v->pri = 0;
        for (unsigned c = 0; c < STALL_CPUS; c++) {
            const struct stall_band *band = &s->cpu[c].band[b];
            totals[0] += STALL_LOAD(&band->n);
            totals[1] += STALL_LOAD(&band->over10);
            totals[2] += STALL_LOAD(&band->over50);
            totals[3] += STALL_LOAD(&band->over200);
            const uint64_t ticks = STALL_LOAD(&band->max_ticks[cur]);
            if (ticks > v->max_ticks) {
                v->max_ticks = ticks;
                v->tid = STALL_LOAD(&band->max_tid[cur]);
                v->pid = STALL_LOAD(&band->max_pid[cur]);
                v->pri = STALL_LOAD(&band->max_pri[cur]);
                a = STALL_LOAD(&band->max_name[cur][0]);
                c2 = STALL_LOAD(&band->max_name[cur][1]);
            }
        }
        v->n = stall_delta32(totals[0], &seen->band[b][0]);
        v->over10 = stall_delta32(totals[1], &seen->band[b][1]);
        v->over50 = stall_delta32(totals[2], &seen->band[b][2]);
        v->over200 = stall_delta32(totals[3], &seen->band[b][3]);
        stall_words_name(a, c2, v->name);
    }
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        struct stall_cpu *cpu = &s->cpu[c];
        out->idle[c] = stall_delta32(STALL_LOAD(&cpu->idle), &seen->idle[c]);
        out->sent[c] = stall_delta32(STALL_LOAD(&cpu->sent), &seen->sent[c]);
        out->recv[c] = stall_delta32(STALL_LOAD(&cpu->recv), &seen->recv[c]);
        out->ipi_max[c] = STALL_LOAD(&cpu->ipi_max[cur]);
    }
}

// ---- the sampler's watch over idle CPUs and pending IPIs -----------------------------------------------------------------------------------
struct stall_watch {
    uint8_t streak[STALL_CPUS];             // ticks in a row idle over a non-empty queue
    uint8_t ipi_seen[STALL_CPUS];           // this pending IPI has been counted
    uint64_t hungry[STALL_CPUS];            // ticks of such a streak from its second on; only grow, the observer reads them
    uint64_t stuck[STALL_CPUS];             // IPIs pending STALL_IPI_STUCK_MS; only grow
};

// One CPU at one tick; 1 when its streak reaches STALL_HUNGRY_TICKS (once a streak). A CPU the scheduler chose but that still runs its idle
// thread is as hungry as an idle one: that is what a lost wake-up IPI looks like.
static inline int stall_watch_cpu(struct stall_watch *w, unsigned cpu, const struct boot_progress_stall_cpu *c)
{
    if (cpu >= STALL_CPUS) return 0;
    const int idle = c->kind == STALL_CPU_IDLE || (c->kind == STALL_CPU_DISPATCHING && c->on_idle);
    if (!idle || c->runq + c->rt <= 0) {
        w->streak[cpu] = 0;
        return 0;
    }
    if (w->streak[cpu] != 255) w->streak[cpu]++;
    if (w->streak[cpu] >= STALL_HUNGRY_COUNT) STALL_STORE(&w->hungry[cpu], w->hungry[cpu] + 1);
    return w->streak[cpu] == STALL_HUNGRY_TICKS;
}

// 1 when the IPI pending at `pending` (0: none) just became stuck.
static inline int stall_watch_ipi(struct stall_watch *w, unsigned cpu, uint64_t pending, uint64_t now, uint64_t hz)
{
    if (cpu >= STALL_CPUS) return 0;
    if (!pending || now < pending || now - pending < hz / 1000 * STALL_IPI_STUCK_MS) {
        w->ipi_seen[cpu] = 0;
        return 0;
    }
    if (w->ipi_seen[cpu]) return 0;
    w->ipi_seen[cpu] = 1;
    STALL_STORE(&w->stuck[cpu], w->stuck[cpu] + 1);
    return 1;
}

// ---- the Q records' limit ------------------------------------------------------------------------------------------------------------------
struct stall_limit {
    uint64_t credit, stamp;                 // ticks: a line costs hz / STALL_LINES_PER_S
    uint32_t used;                          // lines written
    uint32_t over_rate, over_budget;        // samples left out
    uint32_t primed;
};

static inline int stall_limit_take(struct stall_limit *l, uint64_t now, uint64_t hz, unsigned lines, unsigned level)
{
    const uint64_t burst = hz * STALL_BURST_S, cost = lines * (hz / STALL_LINES_PER_S);
    if (!l->primed) {
        l->primed = 1;
        l->credit = burst;
        l->stamp = now;
    }
    if (now > l->stamp) {
        const uint64_t gained = now - l->stamp;
        l->credit = gained >= burst - l->credit ? burst : l->credit + gained;
        l->stamp = now;
    }
    if (l->used + lines > stall_level_lines[level < STALL_LEVELS ? level : STALL_LEVEL_TAIL]) {
        STALL_STORE(&l->over_budget, l->over_budget + 1);
        return 0;
    }
    if (l->credit < cost) {
        STALL_STORE(&l->over_rate, l->over_rate + 1);
        return 0;
    }
    l->credit -= cost;
    STALL_STORE(&l->used, l->used + lines);
    return 1;
}

// ---- gaps ----------------------------------------------------------------------------------------------------------------------------------
struct stall_gap {
    uint64_t frames, newest;                // the frame ring's count and newest stamp
    uint64_t asked;                         // when something first waited for a frame after that one, 0: nothing has
    uint64_t touch_at_frame;                // the last touch when that frame was seen
    uint32_t gpu_at_frame;                  // the submission count then
    uint32_t stalls;                        // stalls sampled so far: the current one's number while samples
    uint32_t quiet, capped;                 // crossings left out: nothing tells a stall, its one sample was taken
    uint8_t level, samples;
    uint8_t primed;
    uint8_t sign;                           // the strongest sign since asked (STALL_SIGN_*)
};

struct stall_gap_in {
    uint64_t now, hz;
    uint64_t frames, newest;                // the ring's count, the newest frame's stamp
    uint32_t frames_before;                 // frames in the STALL_ACTIVE_MS before the newest
    uint32_t gpu;                           // submissions so far
    uint64_t gpu_at, touch_at;              // when the last submission and the last touch frame were seen, 0: none yet
    uint32_t kbt_open;                      // a kbt pair is open
};

static inline uint64_t stall_ms(uint64_t ticks, uint64_t hz)
{
    return hz ? ticks / hz * 1000 + ticks % hz * 1000 / hz : 0;
}

static inline int stall_recent(uint64_t now, uint64_t at, uint64_t hz)
{
    return at && (now < at || stall_ms(now - at, hz) <= STALL_ACTIVE_MS);
}

enum { STALL_SIGN_NONE, STALL_SIGN_GPU, STALL_SIGN_SURE };

// What says something waits for a frame now: seen after the newest frame was, as the frame's own touch and submission were seen no later.
static inline unsigned stall_gap_sign(const struct stall_gap *g, const struct stall_gap_in *in)
{
    if (in->kbt_open) return STALL_SIGN_SURE;
    if (in->touch_at != g->touch_at_frame && in->touch_at > g->newest && stall_recent(in->now, in->touch_at, in->hz)) return STALL_SIGN_SURE;
    return in->gpu != g->gpu_at_frame && stall_recent(in->now, in->gpu_at, in->hz) ? STALL_SIGN_GPU : STALL_SIGN_NONE;
}

// Nothing says so, but the frames came in a run that may have stalled: this much is worth one sample.
static inline int stall_gap_cadence(const struct stall_gap_in *in)
{
    return in->frames_before >= STALL_CADENCE_FRAMES &&
           (stall_recent(in->now, in->gpu_at, in->hz) || stall_recent(in->now, in->touch_at, in->hz));
}

// When the stall's clock started.
static inline uint64_t stall_gap_from(const struct stall_gap *g) {return g->asked ? g->asked : g->newest;}

// A stall with a sure sign and thresholds left: the sampler stays awake for them, whatever else is quiet.
static inline int stall_gap_waiting(const struct stall_gap *g) {return g->sign == STALL_SIGN_SURE && g->level < STALL_GAP_LEVELS;}

// One tick. Returns the threshold (ms) to sample at now, 0 for none. A new frame ends the stall. A tick that comes late crosses several
// thresholds as one sample.
static inline uint32_t stall_gap_step(struct stall_gap *g, const struct stall_gap_in *in)
{
    if (!g->primed || in->frames != g->frames) {
        g->primed = 1;
        g->frames = in->frames;
        g->newest = in->newest;
        g->gpu_at_frame = in->gpu;
        g->touch_at_frame = in->touch_at;
        g->asked = 0;
        g->sign = STALL_SIGN_NONE;
        g->level = 0;
        g->samples = 0;
        return 0;
    }
    if (!g->frames || in->now <= g->newest) return 0;
    if (g->asked && g->level == STALL_GAP_LEVELS && in->touch_at > g->asked &&
        stall_ms(in->touch_at - g->asked, in->hz) > stall_gap_ms[STALL_GAP_LEVELS - 1]) {
        g->asked = 0;
        g->sign = STALL_SIGN_NONE;
        g->samples = 0;
    }
    const unsigned sign = stall_gap_sign(g, in);
    if (!g->asked && sign) {
        g->asked = in->now;
        g->level = 0;
    }
    if (sign > g->sign) g->sign = (uint8_t)sign;
    const uint64_t from = stall_gap_from(g);
    const uint64_t age = in->now > from ? stall_ms(in->now - from, in->hz) : 0;
    unsigned level = g->level;
    while (level < STALL_GAP_LEVELS && age >= stall_gap_ms[level]) level++;
    if (level == g->level) return 0;
    g->level = (uint8_t)level;
    if (!g->asked) {
        if (g->samples) {
            g->capped++;
            return 0;
        }
        if (!stall_gap_cadence(in)) {
            g->quiet++;
            return 0;
        }
    } else if (g->sign == STALL_SIGN_GPU && !sign) {
        g->quiet++;
        return 0;
    }
    if (!g->samples) g->stalls++;
    g->samples++;
    return stall_gap_ms[level - 1];
}

// The limit's level of the sample stall_gap_step just asked for.
static inline unsigned stall_gap_level(const struct stall_gap *g, uint32_t ms)
{
    if (!g->asked) return STALL_LEVEL_TAIL;
    if (g->sign != STALL_SIGN_SURE) return STALL_LEVEL_LOW;
    return ms <= stall_gap_ms[0] ? STALL_LEVEL_LOW : ms <= 200 ? STALL_LEVEL_MID : STALL_LEVEL_HIGH;
}

// ---- kbt pairs left open (under the glue's lock: any thread notes, the sampler asks) -------------------------------------------------------
struct stall_kbt_open {
    uint64_t begin[STALL_KBT_BEGINS];       // open begins, oldest at head; more than fit are only counted in `open`
    uint64_t next;                          // the next sample
    int32_t pid;
    uint32_t open;
    uint32_t id;                            // st= of the samples
    uint16_t samples;
    uint8_t group, used, head, queued;
};

struct stall_kbt {
    struct stall_kbt_open slot[STALL_KBT_SLOTS];
    uint32_t ids;
    uint32_t recycled, given_up;
};

struct stall_kbt_due {
    uint64_t age_ms;
    int32_t pid;
    uint32_t id, open, sample;
    uint32_t group;
};

static inline struct stall_kbt_open *stall_kbt_find(struct stall_kbt *t, int32_t pid, unsigned group)
{
    for (unsigned i = 0; i < STALL_KBT_SLOTS; i++) {
        struct stall_kbt_open *s = &t->slot[i];
        if (s->used && s->pid == pid && s->group == group) return s;
    }
    return 0;
}

// A begin or an end of pid's pair in `group`. An end without a begin (the probe started in between) changes nothing.
static inline void stall_kbt_note(struct stall_kbt *t, int32_t pid, unsigned group, int end, uint64_t now, uint64_t hz)
{
    if (group >= STALL_KBT_GROUPS) return;
    struct stall_kbt_open *s = stall_kbt_find(t, pid, group);
    if (end) {
        if (!s) return;
        s->open--;
        if (s->queued) {
            s->head = (uint8_t)((s->head + 1) % STALL_KBT_BEGINS);
            s->queued--;
        }
        if (!s->open) {
            s->used = 0;
        } else if (s->queued) {                 // the next open begin's clock
            s->next = s->begin[s->head] + hz / 1000 * STALL_KBT_FIRST_MS;
            s->samples = 0;
            s->id = ++t->ids;
        }
        return;
    }
    if (!s) {
        struct stall_kbt_open *oldest = &t->slot[0];
        for (unsigned i = 0; i < STALL_KBT_SLOTS; i++) {
            struct stall_kbt_open *c = &t->slot[i];
            if (!c->used) { oldest = c; break; }
            if (c->begin[c->head] < oldest->begin[oldest->head]) oldest = c;
        }
        s = oldest;
        if (s->used) t->recycled++;
        s->used = 1;
        s->pid = pid;
        s->group = (uint8_t)group;
        s->head = s->queued = 0;
        s->open = 0;
    }
    if (!s->open) {
        s->next = now + hz / 1000 * STALL_KBT_FIRST_MS;
        s->samples = 0;
        s->id = ++t->ids;
    }
    if (s->open != UINT32_MAX) s->open++;
    if (s->queued < STALL_KBT_BEGINS) {
        s->begin[(s->head + s->queued) % STALL_KBT_BEGINS] = now;
        s->queued++;
    }
}

// How long a pair that had `samples` samples waits for the next: the first ones tell the most, and a pair may stay open for most of a minute.
static inline uint32_t stall_kbt_wait_ms(uint32_t samples)
{
    return samples <= STALL_KBT_STEP ? STALL_KBT_NEXT_MS : samples <= 2 * STALL_KBT_STEP ? 2 * STALL_KBT_NEXT_MS : 4 * STALL_KBT_NEXT_MS;
}

// 1 and *due for the oldest pair whose sample is due, which then waits for stall_kbt_wait_ms. A pair past STALL_KBT_SAMPLES is given up.
static inline int stall_kbt_take(struct stall_kbt *t, uint64_t now, uint64_t hz, struct stall_kbt_due *due)
{
    struct stall_kbt_open *pick = 0;
    for (unsigned i = 0; i < STALL_KBT_SLOTS; i++) {
        struct stall_kbt_open *s = &t->slot[i];
        if (!s->used || now < s->next) continue;
        if (!pick || s->next < pick->next) pick = s;
    }
    if (!pick) return 0;
    if (pick->samples >= STALL_KBT_SAMPLES) {
        pick->used = 0;
        t->given_up++;
        return 0;
    }
    pick->samples++;
    pick->next = now + hz / 1000 * stall_kbt_wait_ms(pick->samples);
    const uint64_t first = pick->queued ? pick->begin[pick->head] : now;
    due->age_ms = stall_ms(now > first ? now - first : 0, hz);
    due->pid = pick->pid;
    due->id = pick->id;
    due->open = pick->open;
    due->sample = pick->samples;
    due->group = pick->group;
    return 1;
}

// The process is gone: its pairs will not end.
static inline void stall_kbt_forget(struct stall_kbt *t, int32_t pid)
{
    for (unsigned i = 0; i < STALL_KBT_SLOTS; i++) if (t->slot[i].used && t->slot[i].pid == pid) t->slot[i].used = 0;
}

// Anything open: the sampler stays armed for it.
static inline int stall_kbt_any(const struct stall_kbt *t)
{
    for (unsigned i = 0; i < STALL_KBT_SLOTS; i++) if (t->slot[i].used) return 1;
    return 0;
}

// ---- kbd's busiest thread --------------------------------------------------------------------------------------------------------------------
struct stall_busy {
    uint64_t tid[STALL_BUSY_THREADS], time[STALL_BUSY_THREADS];
    uint32_t count;
};

// The thread with the most CPU time since the previous call (a thread new since then: all of its time), 0 without threads. Keeps the times.
static inline uint64_t stall_busiest(struct stall_busy *seen, const uint64_t *tids, const uint64_t *times, unsigned n)
{
    uint64_t best = 0, best_delta = 0;
    int have = 0;
    if (n > STALL_BUSY_THREADS) n = STALL_BUSY_THREADS;
    for (unsigned i = 0; i < n; i++) {
        uint64_t before = 0;
        for (unsigned k = 0; k < seen->count; k++) if (seen->tid[k] == tids[i]) { before = seen->time[k]; break; }
        const uint64_t delta = times[i] > before ? times[i] - before : 0;
        if (!have || delta > best_delta) {
            have = 1;
            best = tids[i];
            best_delta = delta;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        seen->tid[i] = tids[i];
        seen->time[i] = times[i];
    }
    seen->count = n;
    return best;
}

// ---- system calls with a timeout -------------------------------------------------------------------------------------------------------------
// The number a thread entered the kernel with, as a signed value: BSD calls positive, Mach traps negative (x16, 32 bits). 0 unless it is in the kernel
// by way of an SVC (ESR class 0x15): a page fault leaves x16 as user code had it, an interrupt leaves the esr of the last exception.
static inline int32_t stall_syscall_of(const struct boot_progress_stall_regs *r, char letter)
{
    if ((letter != 'W' && letter != 'U' && !(letter == 'C' && r->kernel)) || (r->esr >> 26 & 0x3f) != 0x15) return 0;
    return (int32_t)(uint32_t)r->x16;
}

// 1 and *ms when `sc` is a wait with a timeout in the registers. What it tells: mach_msg with MACH_RCV_TIMEOUT (0x100 in x1), a semaphore's timed
// wait, a condition variable's, __semwait_signal, the ulock waits, poll, thread_switch.
static inline int stall_wait_timeout(int32_t sc, const struct boot_progress_stall_regs *r, uint64_t *ms)
{
    uint64_t sec = 0, nsec = 0;
    switch (sc) {
    case -31:                               // mach_msg_trap(msg, option, send_size, rcv_size, rcv_name, timeout, notify)
    case -32:                               // mach_msg_overwrite_trap
        if (!(r->x[1] & 0x100)) return 0;
        *ms = (uint32_t)r->x[5];
        return 1;
    case -47:                               // mach_msg2_trap(data, options, .., timeout)
        if (!(r->x[1] & 0x100)) return 0;
        *ms = (uint32_t)r->x[7];
        return 1;
    case -38: sec = (uint32_t)r->x[1]; nsec = (uint32_t)r->x[2]; break;     // semaphore_timedwait_trap(name, sec, nsec)
    case -39: sec = (uint32_t)r->x[2]; nsec = (uint32_t)r->x[3]; break;     // semaphore_timedwait_signal_trap(wait, signal, sec, nsec)
    case -61: *ms = (uint32_t)r->x[2]; return 1;                            // syscall_thread_switch(thread, option, ms)
    case 230:                                                               // poll(fds, nfds, timeout)
        if ((int32_t)r->x[2] < 0) return 0;
        *ms = (uint32_t)r->x[2];
        return 1;
    case 305: sec = r->x[6]; nsec = (uint32_t)r->x[7]; break;               // psynch_cvwait(cv, cvlsgen, cvugen, mutex, mugen, flags, sec, nsec)
    case 334:                                                               // __semwait_signal(cond, mutex, timeout, relative, sec, nsec)
    case 423:
        if (!(uint32_t)r->x[2]) return 0;
        sec = r->x[4];
        nsec = (uint32_t)r->x[5];
        break;
    case 515: *ms = (uint32_t)r->x[3] / 1000; return (uint32_t)r->x[3] != 0;    // ulock_wait(op, addr, value, timeout us)
    case 544: *ms = r->x[3] / 1000000; return r->x[3] != 0;                     // ulock_wait2(op, addr, value, timeout ns, value2)
    default: return 0;
    }
    if (!sec && !nsec) return 0;
    *ms = sec * 1000 + nsec / 1000000;
    return 1;
}

// ---- file mappings already reported ----------------------------------------------------------------------------------------------------------
struct stall_known {
    int32_t pid[STALL_KNOWN];
    uint64_t start[STALL_KNOWN], end[STALL_KNOWN];
    uint32_t next;
};

static inline int stall_known_has(const struct stall_known *k, int32_t pid, uint64_t address)
{
    for (unsigned i = 0; i < STALL_KNOWN; i++) if (k->end[i] && k->pid[i] == pid && address >= k->start[i] && address < k->end[i]) return 1;
    return 0;
}

static inline void stall_known_add(struct stall_known *k, int32_t pid, uint64_t start, uint64_t end)
{
    const unsigned i = k->next++ % STALL_KNOWN;
    k->pid[i] = pid;
    k->start[i] = start;
    k->end[i] = end;
}

// ---- text -------------------------------------------------------------------------------------------------------------------------------------
static inline void stall_put_u(struct crash_text *x, uint64_t value) {crash_put_dec(x, (int64_t)(value & INT64_MAX));}

// Milliseconds of `ticks`, one decimal below 100 ("3.2"), whole ones from there.
static inline void stall_put_ms(struct crash_text *x, uint64_t ticks, uint64_t hz)
{
    const uint64_t tenths = hz ? ticks / hz * 10000 + ticks % hz * 10000 / hz : 0;
    if (tenths < 1000) {
        stall_put_u(x, tenths / 10);
        crash_put_char(x, '.');
        stall_put_u(x, tenths % 10);
    } else {
        stall_put_u(x, tenths / 10);
    }
}

static inline void stall_put_list32(struct crash_text *x, const uint32_t *values, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (i) crash_put_char(x, ',');
        stall_put_u(x, values[i]);
    }
}

static inline void stall_head(struct crash_text *x, char *line, uint32_t id, const char *what)
{
    crash_text_start(x, line, STALL_LINE + 1);
    crash_put_str(x, "s=");
    stall_put_u(x, id);
    crash_put_char(x, ' ');
    crash_put_str(x, what);
}

// The first Q record of a sample. A gap: "gap110"; kbt: the group's name, with the process.
struct stall_sample_head {
    uint32_t id, stall, sample;
    uint32_t gap_ms;                        // not 0: a gap
    const char *why;                        // otherwise: dn, up, in, idleq, ipi
    uint64_t age_ms;
    uint64_t touch_ms, gpu_ms;              // UINT64_MAX: never
    uint64_t since_ms;                      // a gap: since the newest frame
    uint32_t sign;                          // a gap: the stall's sign (STALL_SIGN_*)
    int32_t focal;                          // a gap: tasks in the focal role but SpringBoard and backboardd, -1 the list was busy
    uint32_t frames_before;
    int32_t pid;                            // not 0: kbt
    const char *name;
    uint32_t open;
    int32_t cpu;                            // not negative: idleq, ipi
};

static inline void stall_put_age(struct crash_text *x, const char *key, uint64_t ms)
{
    crash_put_str(x, key);
    if (ms == UINT64_MAX) crash_put_char(x, '-');
    else stall_put_u(x, ms);
}

static inline void stall_line_head(char *line, const struct stall_sample_head *h)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "s=");
    stall_put_u(&x, h->id);
    crash_put_str(&x, " why=");
    if (h->gap_ms) {
        crash_put_str(&x, "gap");
        stall_put_u(&x, h->gap_ms);
    } else {
        crash_put_str(&x, h->why);
    }
    crash_put_str(&x, " st=");
    stall_put_u(&x, h->stall);
    crash_put_str(&x, " n=");
    stall_put_u(&x, h->sample);
    crash_put_str(&x, " age=");
    stall_put_u(&x, h->age_ms);
    stall_put_age(&x, " tch=", h->touch_ms);
    stall_put_age(&x, " gpu=", h->gpu_ms);
    crash_put_str(&x, " fb=");
    stall_put_u(&x, h->frames_before);
    if (h->gap_ms) {
        crash_put_str(&x, " since=");
        stall_put_u(&x, h->since_ms);
        crash_put_str(&x, " ask=");
        stall_put_u(&x, h->sign);
        crash_put_str(&x, " fg=");
        crash_put_dec(&x, h->focal);
    }
    if (h->pid) {
        crash_put_str(&x, " pid=");
        crash_put_dec(&x, h->pid);
        crash_put_char(&x, ' ');
        crash_put_name(&x, h->name, STALL_NAME);
        crash_put_str(&x, " open=");
        stall_put_u(&x, h->open);
    }
    if (h->cpu >= 0) {
        crash_put_str(&x, " cpu=");
        crash_put_dec(&x, h->cpu);
    }
}

// TH_WAIT 0x01, TH_SUSP 0x02, TH_RUN 0x04, TH_UNINT 0x08, TH_TERMINATE 0x10, TH_TERMINATE2 0x20 (osfmk/kern/thread.h). st= in the header above.
static inline char stall_state_letter(uint32_t state, int32_t cpu)
{
    if (state & 0x30) return 'T';
    if (cpu >= 0) return 'C';
    if (state & 0x04) return 'R';
    if (state & 0x08) return 'U';
    if (state & 0x01) return 'W';
    return '?';
}

// How long it has been in that state.
static inline uint64_t stall_state_ticks(const struct boot_progress_stall_thread *t, char letter, uint64_t now, uint64_t since)
{
    const uint64_t from = letter == 'R' ? t->made_runnable : letter == 'C' ? since : t->last_run;
    return from && from != UINT64_MAX && now > from ? now - from : 0;
}

struct stall_thread_view {
    uint32_t id;
    const char *tag;
    int32_t pid;
    const char *name;
    const struct boot_progress_stall_thread *t;
    const struct boot_progress_stall_regs *regs;    // the state to read the system call from
    uint64_t now, hz, since;                // since: when it went on core (the CPU's record), for st=C
    const char *xcall;                      // ok, to, mv, no, or NULL: no cross-call
};

static inline void stall_line_thread(char *line, const struct stall_thread_view *v)
{
    struct crash_text x;
    const struct boot_progress_stall_thread *t = v->t;
    const char letter = stall_state_letter(t->state, t->cpu);
    stall_head(&x, line, v->id, "th ");
    crash_put_str(&x, v->tag);
    crash_put_str(&x, " pid=");
    crash_put_dec(&x, v->pid);
    crash_put_char(&x, ' ');
    crash_put_name(&x, v->name, STALL_NAME);
    crash_put_key_hex(&x, " tid=", t->tid);
    crash_put_str(&x, " st=");
    crash_put_char(&x, letter);
    crash_put_str(&x, " pri=");
    crash_put_dec(&x, t->sched_pri);
    crash_put_char(&x, '/');
    crash_put_dec(&x, t->base_pri);
    crash_put_str(&x, " cpu=");
    if (t->cpu >= 0) crash_put_dec(&x, t->cpu);
    else crash_put_char(&x, '-');
    crash_put_str(&x, " wt=");
    stall_put_ms(&x, stall_state_ticks(t, letter, v->now, v->since), v->hz);
    if (v->xcall) {
        crash_put_str(&x, " x=");
        crash_put_str(&x, v->xcall);
    }
    crash_put_str(&x, " hint=");
    stall_put_u(&x, t->hint);
    if (t->wait_event) crash_put_key_hex(&x, " ev=", t->wait_event);
    if (v->regs->valid) {
        const int32_t sc = stall_syscall_of(v->regs, letter);
        uint64_t ms = 0;
        if (sc) {
            crash_put_str(&x, " sc=");
            crash_put_dec(&x, sc);
        }
        if (stall_wait_timeout(sc, v->regs, &ms)) {
            crash_put_str(&x, " to=");
            stall_put_u(&x, ms);
        }
        if (sc == -31 || sc == -32 || sc == -47) crash_put_key_hex(&x, " o=", v->regs->x[1]);
    }
    if (t->continuation) {
        crash_put_str(&x, " cont=k:");
        crash_put_hex(&x, (uint32_t)t->continuation);
    }
    if (t->kframes) {
        crash_put_str(&x, " k=");
        for (unsigned i = 0; i < t->kframes && i < BOOT_PROGRESS_STALL_KFRAMES; i++) {
            if (i) crash_put_char(&x, ',');
            crash_put_str(&x, "k:");
            crash_put_hex(&x, (uint32_t)t->kframe[i]);
        }
    }
}

// pc=, lr= and the first return addresses. Written only with the room left, so end= always fits.
static inline void stall_line_bt(char *line, uint32_t id, const char *tag, int32_t pid, const struct boot_progress_stall_regs *r,
                                 const struct crash_trace *t)
{
    struct crash_text x;
    stall_head(&x, line, id, "bt ");
    crash_put_str(&x, tag);
    crash_put_str(&x, " pid=");
    crash_put_dec(&x, pid);
    crash_put_str(&x, " pc=");
    crash_put_addr(&x, &t->pc);
    crash_put_str(&x, " lr=");
    crash_put_addr(&x, &t->lr);
    if (r->kernel) {
        crash_put_str(&x, " kpc=k:");
        crash_put_hex(&x, (uint32_t)r->kpc);
    }
    if (r->esr) crash_put_key_hex(&x, " esr=", r->esr);
    if (r->far) crash_put_key_hex(&x, " far=", r->far);
    const unsigned tail = 5 + 5 + (t->stop != CRASH_STOP_END ? 19 : 0);
    for (unsigned i = 0; i < t->frames && i < STALL_FRAMES; i++) {
        if (x.used + 1 + crash_addr_length(&t->frame[i]) + tail > STALL_LINE) break;
        crash_put_char(&x, ' ');
        crash_put_addr(&x, &t->frame[i]);
    }
    crash_put_str(&x, " end=");
    crash_put_str(&x, crash_stop_names[t->stop < 7 ? t->stop : 0]);
    if (t->stop != CRASH_STOP_END) {
        crash_put_str(&x, "@0x");
        crash_put_hex(&x, t->stop_at);
    }
}

// The sample's trace of one thread: at most STALL_FRAMES return addresses from c->fp, named as CrashTrace.h does but without its image lookups
// (c->image is NULL here: they take the map lock). What lies outside the shared cache stays ?:<address>, the Q img records name it afterwards.
static inline void stall_trace_take(const struct boot_progress_crash *c, struct crash_trace *t)
{
    const uint64_t mask = crash_user_mask(c);
    t->images.count = 0;
    t->images.lookups = 0;
    t->images.named = 0;
    crash_walk_some(c, mask, t, STALL_FRAMES);
    crash_where(c, &t->images, c->pc & mask, &t->pc);
    crash_where(c, &t->images, c->lr & mask, &t->lr);
    for (unsigned i = 0; i < t->frames; i++) crash_where(c, &t->images, t->frame[i].value, &t->frame[i]);
}

// The first address of a trace that is neither in the shared cache nor in a mapping already reported, 0: none.
static inline uint64_t stall_trace_unknown(const struct crash_trace *t, const struct stall_known *k, int32_t pid)
{
    if (t->pc.kind == CRASH_RAW && t->pc.value && !stall_known_has(k, pid, t->pc.value)) return t->pc.value;
    for (unsigned i = 0; i < t->frames; i++)
        if (t->frame[i].kind == CRASH_RAW && t->frame[i].value && !stall_known_has(k, pid, t->frame[i].value)) return t->frame[i].value;
    return 0;
}

// One CPU of a sample, and the record of four of them.
struct stall_cpu_view {
    int ok;                                 // the on-core record could be read
    struct stall_oncore on;
    struct boot_progress_stall_cpu sched;
};

static inline void stall_line_cpus(char *line, uint32_t id, unsigned first, unsigned count, const struct stall_cpu_view *cpus, uint64_t now,
                                   uint64_t hz)
{
    struct crash_text x;
    int32_t rq = 0, rt = 0;
    stall_head(&x, line, id, "cpu");
    for (unsigned c = first; c < first + count && c < STALL_CPUS; c++) {
        const struct stall_cpu_view *v = &cpus[c];
        const uint64_t on_core = v->ok && now > v->on.since ? now - v->on.since : 0;
        crash_put_char(&x, ' ');
        stall_put_u(&x, c);
        crash_put_char(&x, ':');
        if (v->sched.runq > rq) rq = v->sched.runq;
        if (v->sched.rt > rt) rt = v->sched.rt;
        if (v->sched.kind == STALL_CPU_OFF) {
            crash_put_char(&x, 'x');
        } else if (!v->ok) {
            crash_put_char(&x, '?');
        } else if (v->on.pid < 0) {
            crash_put_char(&x, v->sched.kind == STALL_CPU_DISPATCHING ? '~' : '-');
            crash_put_char(&x, '/');
            stall_put_u(&x, stall_ms(on_core, hz));
        } else {
            if (v->on.pid) crash_put_name(&x, v->on.name, 12);
            else crash_put_char(&x, 'k');
            crash_put_char(&x, '/');
            crash_put_hex(&x, v->on.tid);
            crash_put_char(&x, '/');
            crash_put_dec(&x, v->on.pri);
            crash_put_char(&x, '/');
            stall_put_u(&x, stall_ms(on_core, hz));
        }
    }
    crash_put_str(&x, " rq=");
    crash_put_dec(&x, rq);
    crash_put_str(&x, " rt=");
    crash_put_dec(&x, rt);
}

static inline void stall_line_map(char *line, const struct boot_progress_stall_task *t)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "map pid=");
    crash_put_dec(&x, t->pid);
    crash_put_char(&x, ' ');
    crash_put_name(&x, t->name, STALL_NAME);
    crash_put_key_hex(&x, " slide=", t->cache_end > t->cache_start ? t->cache_slide : 0);
    crash_put_key_hex(&x, " main=", t->main_header);
}

static inline void stall_line_img(char *line, int32_t pid, const struct boot_progress_crash_image *image)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "img pid=");
    crash_put_dec(&x, pid);
    crash_put_key_hex(&x, " start=", image->start);
    crash_put_key_hex(&x, " end=", image->end);
    crash_put_key_hex(&x, " off=", image->offset);
    crash_put_char(&x, ' ');
    crash_put_name(&x, image->name, BOOT_PROGRESS_CRASH_NAME - 1);
}

// M lat: the bands that had a dispatch; the thread of a band's largest latency from 1 ms on, as long as the numbers of the bands after it still fit.
static inline void stall_put_band(struct crash_text *x, const char *name, const struct stall_band_view *v, uint64_t hz)
{
    crash_put_char(x, ' ');
    crash_put_str(x, name);
    crash_put_char(x, ' ');
    stall_put_u(x, v->n);
    crash_put_char(x, '/');
    stall_put_ms(x, v->max_ticks, hz);
    crash_put_char(x, '/');
    stall_put_u(x, v->over10);
    crash_put_char(x, '/');
    stall_put_u(x, v->over50);
    crash_put_char(x, '/');
    stall_put_u(x, v->over200);
}

static inline void stall_put_who(struct crash_text *x, unsigned band, const struct stall_band_view *v)
{
    crash_put_char(x, ' ');
    if (band != STALL_KERNEL) {                 // the kernel's threads have no process to name
        crash_put_name(x, v->name, 12);
        crash_put_char(x, '[');
        crash_put_dec(x, v->pid);
        crash_put_str(x, "]/");
    }
    crash_put_hex(x, v->tid);
    crash_put_char(x, '/');
    crash_put_dec(x, v->pri);
}

static inline void stall_line_lat(char *line, const struct stall_second *s, uint64_t hz)
{
    static const char *const names[STALL_BANDS] = {"fg", "def", "bg", "k"};
    struct crash_text x, m;
    char scratch[STALL_LINE + 1] CRASH_UNINIT;
    unsigned later[STALL_BANDS], need = 0;      // what the numbers of the bands after this one take
    for (unsigned b = STALL_BANDS; b-- > 0;) {
        later[b] = need;
        if (!s->band[b].n) continue;
        crash_text_start(&m, scratch, STALL_LINE + 1);
        stall_put_band(&m, names[b], &s->band[b], hz);
        need += m.used;
    }
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "lat");
    for (unsigned b = 0; b < STALL_BANDS; b++) {
        const struct stall_band_view *v = &s->band[b];
        if (!v->n) continue;
        stall_put_band(&x, names[b], v, hz);
        if (!hz || v->max_ticks < hz / 1000) continue;
        crash_text_start(&m, scratch, STALL_LINE + 1);
        stall_put_who(&m, b, v);
        if (x.used + m.used + later[b] <= STALL_LINE) stall_put_who(&x, b, v);
    }
}

static inline void stall_line_cpu(char *line, const struct stall_second *s, const uint32_t *timers)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "cpu idle ");
    stall_put_list32(&x, s->idle, STALL_CPUS);
    crash_put_str(&x, " ipis ");
    stall_put_list32(&x, s->sent, STALL_CPUS);
    crash_put_str(&x, " ipir ");
    stall_put_list32(&x, s->recv, STALL_CPUS);
    crash_put_str(&x, " tmr ");
    stall_put_list32(&x, timers, STALL_CPUS);
}

struct stall_ipi_line {
    uint32_t hungry[STALL_CPUS], stuck[STALL_CPUS];
    uint64_t tick_ticks;                    // the longest interval between two sampler ticks
    uint32_t images;
};

static inline void stall_line_ipi(char *line, const struct stall_second *s, const struct stall_ipi_line *i, uint64_t hz)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "ipi max ");
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        if (c) crash_put_char(&x, ',');
        stall_put_ms(&x, s->ipi_max[c], hz);
    }
    crash_put_str(&x, " idleq ");
    stall_put_list32(&x, i->hungry, STALL_CPUS);
    crash_put_str(&x, " stuck ");
    stall_put_list32(&x, i->stuck, STALL_CPUS);
    crash_put_str(&x, " tick ");
    stall_put_ms(&x, i->tick_ticks, hz);
    crash_put_str(&x, " img ");
    stall_put_u(&x, i->images);
}

struct stall_io_line {
    uint64_t reads, read_ticks, verify_ticks;
    uint32_t free_min, available;
    uint64_t faults, pageins, compressions, decompressions, page_waits;
    uint64_t throttled[3];                  // tiers 1 to 3
    uint32_t used, over_rate, over_budget;
    uint32_t kbt_used, kbt_over_rate, kbt_over_budget;
    uint32_t tail_used, tail_over_rate, tail_over_budget;
};

// "used/over rate/over budget" of a limit, after key.
static inline void stall_put_limit(struct crash_text *x, const char *key, uint32_t used, uint32_t over_rate, uint32_t over_budget)
{
    crash_put_str(x, key);
    stall_put_u(x, used);
    crash_put_char(x, '/');
    stall_put_u(x, over_rate);
    crash_put_char(x, '/');
    stall_put_u(x, over_budget);
}

static inline void stall_line_io(char *line, const struct stall_io_line *l, uint64_t hz)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "io rd ");
    stall_put_u(&x, l->reads);
    crash_put_char(&x, ' ');
    stall_put_ms(&x, l->read_ticks, hz);
    crash_put_str(&x, " vfy ");
    stall_put_ms(&x, l->verify_ticks, hz);
    crash_put_str(&x, " free ");
    stall_put_u(&x, l->free_min);
    crash_put_str(&x, " avail ");
    stall_put_u(&x, l->available);
    crash_put_str(&x, " flt ");
    stall_put_u(&x, l->faults);
    crash_put_str(&x, " pin ");
    stall_put_u(&x, l->pageins);
    crash_put_str(&x, " cmp ");
    stall_put_u(&x, l->compressions);
    crash_put_str(&x, " dcmp ");
    stall_put_u(&x, l->decompressions);
    crash_put_str(&x, " pw ");
    stall_put_u(&x, l->page_waits);
    crash_put_str(&x, " t1 ");
    stall_put_u(&x, l->throttled[0]);
    crash_put_str(&x, " t2 ");
    stall_put_u(&x, l->throttled[1]);
    crash_put_str(&x, " t3 ");
    stall_put_u(&x, l->throttled[2]);
    stall_put_limit(&x, " q ", l->used, l->over_rate, l->over_budget);
    stall_put_limit(&x, " kq ", l->kbt_used, l->kbt_over_rate, l->kbt_over_budget);
    stall_put_limit(&x, " tq ", l->tail_used, l->tail_over_rate, l->tail_over_budget);
}

struct stall_mem_line {
    struct boot_progress_stall_mem mem;
    uint64_t ram_bytes;                     // the Data disk's resident bytes
    uint32_t pool_mib, pool_peak_mib;       // the GPU pool
};

// 1 when M mem is due: the first pass, then STALL_MEM_EVERY_S after the last one. *next belongs to the observer.
static inline int stall_mem_due(uint64_t now, uint64_t hz, uint64_t *next)
{
    if (now < *next) return 0;
    *next = now + hz * STALL_MEM_EVERY_S;
    return 1;
}

static inline void stall_line_mem(char *line, const struct stall_mem_line *l)
{
    struct crash_text x;
    crash_text_start(&x, line, STALL_LINE + 1);
    crash_put_str(&x, "mem wire ");
    stall_put_u(&x, l->mem.wired);
    crash_put_str(&x, " int ");
    stall_put_u(&x, l->mem.internal);
    crash_put_str(&x, " ext ");
    stall_put_u(&x, l->mem.external);
    crash_put_str(&x, " cmp ");
    stall_put_u(&x, l->mem.compressor);
    crash_put_char(&x, '/');
    stall_put_u(&x, l->mem.compressed);
    crash_put_str(&x, " thr ");
    stall_put_u(&x, l->mem.throttled);
    crash_put_str(&x, " pur ");
    stall_put_u(&x, l->mem.purgeable);
    crash_put_str(&x, " zone ");
    stall_put_u(&x, l->mem.zone);
    crash_put_str(&x, " ram ");
    stall_put_u(&x, l->ram_bytes);
    crash_put_str(&x, " gpu ");
    stall_put_u(&x, l->pool_mib);
    crash_put_char(&x, '/');
    stall_put_u(&x, l->pool_peak_mib);
}

// The F record's tail: " | tch <touch frames this second> last <ms since the last one, - never> vl <late software vsync ticks>".
static inline unsigned stall_fps_tail(char *out, unsigned size, uint32_t touches, uint64_t last_ms, uint32_t vsync_late)
{
    struct crash_text x;
    if (!size) return 0;
    crash_text_start(&x, out, size);
    crash_put_str(&x, " | tch ");
    stall_put_u(&x, touches);
    stall_put_age(&x, " last ", last_ms);
    crash_put_str(&x, " vl ");
    stall_put_u(&x, vsync_late);
    return x.used;
}
#endif
