// SPDX-License-Identifier: BSD-2-Clause-Patent
// Crash trace (pagani-crash; the X records of the ramoops log, RamoopsLog.h). When a process dies of a fatal signal (SIGILL,
// SIGTRAP, SIGABRT, SIGEMT, SIGFPE, SIGBUS, SIGSEGV, SIGSYS), XNU (the device repository's patches/xnu/crash-trace.patch:
// postsig_locked -> boot_progress_crash_signal in osfmk/kern/bsd_kern.c) hands the user state of the thread taking the signal to
// boot_progress_crash (BootProgress.c), on that thread, with no lock held and before the process starts to exit. For a fault
// that thread is the faulting one and its state is the fault's (pc at the faulting instruction, far and esr of the abort); for
// abort() it is the thread that called pthread_kill (pc in __pthread_kill, lr in pthread_kill). This header then:
//   - walks the user frame chain from fp: at most CRASH_FRAMES return addresses. Every frame record is checked before it is
//     read (8-byte aligned, inside the user address space) and read with XNU's copyin; a read that fails ends the walk, and so
//     does a chain that does not climb (a caller's record lies above its callee's, at most CRASH_STEP_MAX above). Bit 60 of a
//     saved fp (a Swift async frame, osfmk/kern/backtrace.c) and bits above the user address space (pointer authentication)
//     are dropped;
//   - names every code address: inside the shared region as the dyld shared cache's unslid address (address - slide); in the
//     main executable's mapping as the offset from its Mach-O header; in another file mapping as the offset from that image's
//     header, or as the file offset when the mapping does not start with a Mach-O header; otherwise as the raw address;
//   - writes a handful of X records, every one shorter than RAMOOPS_LOG_TEXT_MAX (no '+' records), and limits them: a process
//     name reports at most once in CRASH_QUIET_MS (the crashes left out are counted and the next report says how many), and a
//     report whose trace equals that name's last full report of the last CRASH_REPEAT_MS is a single "same" record.
// Records (lowercase hex; address tokens: c:<unslid cache address>, m:<offset from the main executable's header>, <a-d>:<offset
// from the header of image a-d>, <a-d>@<file offset in image a-d (m@: in the main executable)>, ?:<raw address>):
//   crash pid=<pid> <name> sig=<n> exc=<n> code=0x.. sub=0x.. esr=0x.. far=0x.. tid=0x..
//   regs pid=<pid> pc=<token> lr=<token> fp=0x.. sp=0x.. x0=0x.. x1=<token or 0x..> cpsr=0x..
//   map pid=<pid> slide=0x.. main=0x.. hash=<8 hex> skipped=<n>[ async=<n>][ all]
//   img pid=<pid> m=<main executable's file> a=<file> ...      (the file mappings named, only when there are any)
//   bt pid=<pid> <index of its first frame> <token> ...         (at most 8 return addresses; the last one adds end=<why>[@0x<fp>])
//   same pid=<pid> <name> sig=<n> hash=<8 hex> as=<pid of the full report> skipped=<n>
// exc, code and sub are the uthread's last Mach exception (uu_exception, uu_code, uu_subcode): the fault that raised the signal
// for SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGFPE, stale for a signal sent by kill or pthread_kill (esr then shows the SVC).
// Pure C: no locks (crash_report takes the sink's lock only around the rate-limit table, never across a read), no platform
// calls, no floating point, no aggregate copies and no zeroing of large locals (the kernel builds BootProgress with
// -mgeneral-regs-only and -ftrivial-auto-var-init=zero and must not call memset or memcpy). BootProgress.c includes it; host
// tested by BootProgress/Test/crash.c (simulated user memory, mappings, rate limit, the records through the real RamoopsLog.c).
#ifndef PAGANI_CRASH_TRACE_H
#define PAGANI_CRASH_TRACE_H
#include <stdint.h>

// The interface XNU fills in (crash-trace.patch keeps an identical copy in osfmk/kern/bsd_kern.c; size and version catch a
// mismatch).
#define BOOT_PROGRESS_CRASH_VERSION 1u
enum { BOOT_PROGRESS_CRASH_NAME = 40 };

struct boot_progress_crash_image {
    uint64_t start, end;                    // the file mapping that holds the address: [start, end)
    uint64_t offset;                        // its offset in the file at start
    char name[BOOT_PROGRESS_CRASH_NAME];    // the file's name (vnode_getname), NUL-terminated, cut to fit
};

struct boot_progress_crash {
    uint32_t size, version;                 // sizeof(struct boot_progress_crash), BOOT_PROGRESS_CRASH_VERSION
    int32_t pid, signal;                    // the signal number (no core flag)
    const char *name;                       // the process name (p_name)
    uint64_t time_ms;                       // mach time in milliseconds (the rate limit's clock)
    uint64_t thread_id;                     // thread_tid of the thread taking the signal
    uint64_t pc, lr, fp, sp, x0, x1, far;   // its user state
    uint32_t esr, cpsr;
    int32_t exception, reserved;            // uthread uu_exception, uu_code, uu_subcode
    uint64_t code, subcode;
    uint64_t user_limit;                    // the end of the user address space (vm_map_max)
    uint64_t cache_start, cache_end;        // the task's shared region [start, end); end 0: none
    uint64_t cache_slide;                   // the dyld shared cache's slide in it
    uint64_t main_header;                   // the main executable's Mach-O header (task mach_header_vm_address), 0: unknown
    void *context;
    // copyin: 0 when every byte was read. Called with interrupts enabled and no lock held; may fault pages in.
    int (*read)(void *context, uint64_t address, void *buffer, uint32_t bytes);
    // 1 when a file mapping holds the address (image filled in), else 0. May take the map and vnode locks.
    int (*image)(void *context, uint64_t address, struct boot_progress_crash_image *image);
};

// BootProgress.c: the strong definition XNU's weak default gives way to.
void boot_progress_crash(const struct boot_progress_crash *crash);

#if defined(__clang__)
#define CRASH_UNINIT __attribute__((uninitialized))     // filled before use; no -ftrivial-auto-var-init memset
#else
#define CRASH_UNINIT
#endif

enum {
    CRASH_FRAMES = 16,                      // return addresses taken from the frame chain
    CRASH_STEP_MAX = 0x1000000,             // a caller's frame record lies at most 16 MiB above its callee's
    CRASH_LOW = 0x4000,                     // no frame record below this (page zero and the null guard)
    CRASH_IMAGES = 4,                       // file mappings named per report (a-d); further ones show as ?:<address>
    CRASH_LOOKUPS = 8,                      // image lookups per report, found or not
    CRASH_NAMES = 32,                       // process names the rate limit tracks. A name pushed out reports in full at its next
                                            // crash, so more names crashing than slots defeat the limit: the phone identity has
                                            // about a dozen poster extensions (48 bytes a slot)
    CRASH_NAME = 16,                        // name characters compared and printed (the S exit record prints %.16s)
    CRASH_QUIET_MS = 10000,                 // a name reports at most once in this long
    CRASH_REPEAT_MS = 30000,                // the same trace again within this long: one "same" record. The events ring holds
                                            // only 55-66 s (measured 2026-10-30 with the GPU rows, less with X records), and the
                                            // newest full report is up to CRASH_REPEAT_MS + one crash interval old: 30 s keeps it
                                            // in the ring for loops up to ~20 s apart (60 s let it be overwritten, "same" left)
    CRASH_LINE = 190,                       // longest record text (RAMOOPS_LOG_TEXT_MAX is 200)
    CRASH_PER_LINE = 8,                     // return addresses per "bt" record
    CRASH_MODE_OFF = 0,                     // pagani-crash=0
    CRASH_MODE_ON = 1,                      // the default: rate-limited
    CRASH_MODE_ALL = 2,                     // pagani-crash=2: every crash in full
};
#define CRASH_SWIFT_ASYNC_BIT (1ULL << 60)
#define CRASH_MH_MAGIC_64 0xfeedfacfu

enum { CRASH_STOP_END, CRASH_STOP_MAX, CRASH_STOP_READ, CRASH_STOP_ORDER, CRASH_STOP_GAP, CRASH_STOP_ALIGN, CRASH_STOP_RANGE };
static const char *const crash_stop_names[] = {"end", "max", "read", "order", "gap", "align", "range"};

// What an address is: raw, in the shared cache (value: the unslid address), in the main executable (the offset from its header),
// in image `image` (the offset from its header, or the file offset).
enum { CRASH_RAW, CRASH_CACHE, CRASH_MAIN, CRASH_IMAGE, CRASH_FILE };

struct crash_addr {
    uint64_t value;
    uint8_t kind, image;                    // image: the slot in crash_images
    char letter;                            // CRASH_IMAGE and CRASH_FILE: the image's letter (m: the main executable)
};

struct crash_images {
    struct boot_progress_crash_image map[CRASH_IMAGES];
    uint8_t header[CRASH_IMAGES];           // 1: map[i] starts with a Mach-O header
    uint8_t main[CRASH_IMAGES];             // 1: map[i] holds the main executable's header
    char letter[CRASH_IMAGES];              // m for the main executable, a, b, ... for the others in the order they were met
    unsigned count, lookups, named;
};

struct crash_trace {
    struct crash_images images;
    struct crash_addr pc, lr, x1, frame[CRASH_FRAMES];
    unsigned frames, stop, async;
    uint64_t stop_at;                       // the frame record address the walk stopped at (CRASH_STOP_END: unused)
    uint32_t hash;
};

// The rate limit, per process name (under the sink's lock).
struct crash_slot {
    char name[CRASH_NAME + 1];
    uint8_t used, reported, full;
    uint32_t skipped, hash;
    int32_t full_pid;
    uint64_t last_ms, full_ms;
};

struct crash_table {
    struct crash_slot slot[CRASH_NAMES];
};

// Where the records go. lock returns UINT64_MAX when it could not be taken (then the report goes on without the table: full).
struct crash_sink {
    void (*emit)(void *context, const char *line);
    uint64_t (*lock)(void);
    void (*unlock)(uint64_t flags);
    void *context;
};

static inline int crash_signal_traced(int32_t signal)
{
    // SIGILL 4, SIGTRAP 5, SIGABRT 6, SIGEMT 7, SIGFPE 8, SIGBUS 10, SIGSEGV 11, SIGSYS 12
    return signal == 4 || signal == 5 || signal == 6 || signal == 7 || signal == 8 || signal == 10 || signal == 11 ||
           signal == 12;
}

// All bits of a user address: the smallest 2^k - 1 that holds user_limit - 1 (2^47 - 1 without a limit).
static inline uint64_t crash_user_mask(const struct boot_progress_crash *c)
{
    uint64_t top = c->user_limit > 1 ? c->user_limit - 1 : (1ULL << 47) - 1, mask = 0xffff;
    while (mask < top && mask < (1ULL << 56) - 1) mask = (mask << 1) | 1;
    return mask;
}

static inline uint64_t crash_user_limit(const struct boot_progress_crash *c, uint64_t mask)
{
    return c->user_limit > 1 && c->user_limit - 1 <= mask ? c->user_limit : mask + 1;
}

// The frame chain from c->fp into t->frame (values: the masked return addresses, classified later), at most `frames` of them.
static inline void crash_walk_some(const struct boot_progress_crash *c, uint64_t mask, struct crash_trace *t, unsigned frames)
{
    const uint64_t limit = crash_user_limit(c, mask);
    uint64_t fp = c->fp & mask;
    t->frames = 0;
    t->async = 0;
    t->stop = CRASH_STOP_END;
    t->stop_at = 0;
    if (!fp) return;
    for (;;) {
        unsigned stop = CRASH_STOP_END;
        uint64_t record[2] = {0, 0};
        if (fp & 7) stop = CRASH_STOP_ALIGN;
        else if (fp < CRASH_LOW || fp >= limit || limit - fp < sizeof(record)) stop = CRASH_STOP_RANGE;
        else if (t->frames == frames) stop = CRASH_STOP_MAX;
        else if (!c->read || c->read(c->context, fp, record, (uint32_t)sizeof(record))) stop = CRASH_STOP_READ;
        if (stop != CRASH_STOP_END) {
            t->stop = stop;
            t->stop_at = fp;
            return;
        }
        const uint64_t ret = record[1] & mask;
        if (record[0] & CRASH_SWIFT_ASYNC_BIT) t->async++;
        const uint64_t next = record[0] & mask;
        if (!ret) return;                   // the thread's first frame
        t->frame[t->frames].value = ret;
        t->frame[t->frames].kind = CRASH_RAW;
        t->frame[t->frames].image = 0;
        t->frame[t->frames].letter = '?';
        t->frames++;
        if (!next) return;
        if (next <= fp || next - fp > CRASH_STEP_MAX) {
            t->stop = next <= fp ? CRASH_STOP_ORDER : CRASH_STOP_GAP;
            t->stop_at = next;
            return;
        }
        fp = next;
    }
}

static inline void crash_walk(const struct boot_progress_crash *c, uint64_t mask, struct crash_trace *t)
{
    crash_walk_some(c, mask, t, CRASH_FRAMES);
}

// Names an address (see the header comment); a new file mapping takes the next image slot while there is one.
static inline void crash_where(const struct boot_progress_crash *c, struct crash_images *im, uint64_t address,
                               struct crash_addr *where)
{
    where->value = address;
    where->kind = CRASH_RAW;
    where->image = 0;
    where->letter = '?';
    if (!address) return;
    if (c->cache_end > c->cache_start && address >= c->cache_start && address < c->cache_end) {
        where->value = address - c->cache_slide;
        where->kind = CRASH_CACHE;
        return;
    }
    unsigned i = 0;
    while (i < im->count && !(address >= im->map[i].start && address < im->map[i].end)) i++;
    if (i == im->count) {
        if (im->count == CRASH_IMAGES || im->lookups == CRASH_LOOKUPS || !c->image) return;
        im->lookups++;
        struct boot_progress_crash_image *m = &im->map[i];
        m->start = m->end = m->offset = 0;
        m->name[0] = 0;
        if (c->image(c->context, address, m) != 1 || address < m->start || address >= m->end) return;
        m->name[BOOT_PROGRESS_CRASH_NAME - 1] = 0;
        uint32_t magic = 0;
        im->header[i] = (uint8_t)(c->read && !c->read(c->context, m->start, &magic, (uint32_t)sizeof(magic)) &&
                                  magic == CRASH_MH_MAGIC_64);
        im->main[i] = (uint8_t)(c->main_header && c->main_header >= m->start && c->main_header < m->end);
        im->letter[i] = im->main[i] ? 'm' : (char)('a' + im->named++);
        im->count++;
    }
    where->image = (uint8_t)i;
    where->letter = im->letter[i];
    if (im->main[i] && address >= c->main_header) {
        where->value = address - c->main_header;
        where->kind = CRASH_MAIN;
    } else if (im->header[i]) {
        where->value = address - im->map[i].start;
        where->kind = CRASH_IMAGE;
    } else {
        where->value = im->map[i].offset + (address - im->map[i].start);
        where->kind = CRASH_FILE;
    }
}

// FNV-1a over a 64-bit value.
static inline uint32_t crash_mix(uint32_t hash, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        hash ^= (uint8_t)(value >> (8 * i));
        hash *= 16777619u;
    }
    return hash;
}

static inline uint32_t crash_mix_addr(uint32_t hash, const struct crash_addr *a)
{
    // Raw addresses move with ASLR: only their kind counts.
    hash = crash_mix(hash, (uint64_t)a->kind << 8 | a->image);
    return a->kind == CRASH_RAW ? hash : crash_mix(hash, a->value);
}

// Walks the chain and names pc, lr, x1 (only as a shared cache address: a selector) and every return address.
static inline void crash_take(const struct boot_progress_crash *c, struct crash_trace *t)
{
    const uint64_t mask = crash_user_mask(c);
    t->images.count = 0;
    t->images.lookups = 0;
    t->images.named = 0;
    crash_walk(c, mask, t);
    crash_where(c, &t->images, c->pc & mask, &t->pc);
    crash_where(c, &t->images, c->lr & mask, &t->lr);
    for (unsigned i = 0; i < t->frames; i++) crash_where(c, &t->images, t->frame[i].value, &t->frame[i]);
    t->x1.value = c->x1;
    t->x1.kind = CRASH_RAW;
    t->x1.image = 0;
    t->x1.letter = '?';
    if (c->cache_end > c->cache_start && c->x1 >= c->cache_start && c->x1 < c->cache_end) {
        t->x1.value = c->x1 - c->cache_slide;
        t->x1.kind = CRASH_CACHE;
    }
    uint32_t hash = crash_mix(2166136261u, (uint64_t)(uint32_t)c->signal);
    hash = crash_mix_addr(crash_mix_addr(hash, &t->pc), &t->lr);
    for (unsigned i = 0; i < t->frames; i++) hash = crash_mix_addr(hash, &t->frame[i]);
    t->hash = crash_mix(hash, t->stop);
}

// ---- text ----------------------------------------------------------------------------------------------------------------
struct crash_text {
    char *at;
    unsigned used, room;                    // room: the buffer's size (text at most room - 1)
};

static inline void crash_text_start(struct crash_text *x, char *buffer, unsigned room)
{
    x->at = buffer;
    x->used = 0;
    x->room = room;
    buffer[0] = 0;
}

static inline void crash_put_char(struct crash_text *x, char c)
{
    if (x->used + 1 >= x->room) return;
    x->at[x->used++] = c;
    x->at[x->used] = 0;
}

static inline void crash_put_str(struct crash_text *x, const char *s)
{
    while (s && *s) crash_put_char(x, *s++);
}

// A name as one token: at most `limit` characters, spaces as '_', anything unprintable as '?'.
static inline void crash_put_name(struct crash_text *x, const char *s, unsigned limit)
{
    unsigned n = 0;
    if (!s || !s[0]) { crash_put_char(x, '?'); return; }
    for (; n < limit && s[n]; n++) {
        const unsigned char u = (unsigned char)s[n];
        crash_put_char(x, u == ' ' ? '_' : u > 0x20 && u < 0x7f ? (char)u : '?');
    }
}

static inline void crash_put_hex(struct crash_text *x, uint64_t value)
{
    char digits[16];
    unsigned n = 0;
    do { digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4; } while (value);
    while (n) crash_put_char(x, digits[--n]);
}

static inline void crash_put_hex8(struct crash_text *x, uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4) crash_put_char(x, "0123456789abcdef"[(value >> shift) & 15]);
}

static inline void crash_put_dec(struct crash_text *x, int64_t value)
{
    char digits[20];
    unsigned n = 0;
    uint64_t v = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    if (value < 0) crash_put_char(x, '-');
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) crash_put_char(x, digits[--n]);
}

static inline void crash_put_key_hex(struct crash_text *x, const char *key, uint64_t value)
{
    crash_put_str(x, key);
    crash_put_str(x, "0x");
    crash_put_hex(x, value);
}

static inline void crash_put_addr(struct crash_text *x, const struct crash_addr *a)
{
    switch (a->kind) {
    case CRASH_CACHE: crash_put_str(x, "c:"); break;
    case CRASH_MAIN: crash_put_str(x, "m:"); break;
    case CRASH_IMAGE: crash_put_char(x, a->letter); crash_put_char(x, ':'); break;
    case CRASH_FILE: crash_put_char(x, a->letter); crash_put_char(x, '@'); break;
    default: crash_put_str(x, "?:"); break;
    }
    crash_put_hex(x, a->value);
}

// The length crash_put_addr writes.
static inline unsigned crash_addr_length(const struct crash_addr *a)
{
    unsigned n = 3;
    for (uint64_t v = a->value >> 4; v; v >>= 4) n++;
    return n;
}

static inline void crash_put_pid(struct crash_text *x, const char *tag, int32_t pid)
{
    crash_put_str(x, tag);
    crash_put_str(x, " pid=");
    crash_put_dec(x, pid);
}

// ---- the report ----------------------------------------------------------------------------------------------------------
static inline int crash_name_equal(const char *slot, const char *name)
{
    unsigned n = 0;
    if (!name) name = "";
    for (; n < CRASH_NAME && name[n]; n++) if (slot[n] != name[n]) return 0;
    return slot[n] == 0;
}

static inline struct crash_slot *crash_find(struct crash_table *table, const char *name)
{
    for (unsigned i = 0; i < CRASH_NAMES; i++)
        if (table->slot[i].used && crash_name_equal(table->slot[i].name, name)) return &table->slot[i];
    return 0;
}

// The name's slot; a new name takes a free slot or the one that reported longest ago.
static inline struct crash_slot *crash_slot_for(struct crash_table *table, const char *name)
{
    struct crash_slot *s = crash_find(table, name), *oldest = &table->slot[0];
    if (s) return s;
    for (unsigned i = 0; i < CRASH_NAMES; i++) {
        struct crash_slot *candidate = &table->slot[i];
        if (!candidate->used) { oldest = candidate; break; }
        if (candidate->last_ms < oldest->last_ms) oldest = candidate;
    }
    s = oldest;
    unsigned n = 0;
    if (!name) name = "";
    for (; n < CRASH_NAME && name[n]; n++) s->name[n] = name[n];
    for (; n <= CRASH_NAME; n++) s->name[n] = 0;
    s->used = 1;
    s->reported = s->full = 0;
    s->skipped = s->hash = 0;
    s->full_pid = 0;
    s->last_ms = s->full_ms = 0;
    return s;
}

// Under the lock: 1 to report this crash (*skipped: the crashes of this name left out since its last report), else 0 (counted).
static inline int crash_admit(struct crash_table *table, const char *name, uint64_t now, unsigned mode, uint32_t *skipped)
{
    struct crash_slot *s = crash_slot_for(table, name);
    if (!(mode & CRASH_MODE_ALL) && s->reported && now - s->last_ms < CRASH_QUIET_MS) {
        if (s->skipped != UINT32_MAX) s->skipped++;
        return 0;
    }
    *skipped = s->skipped;
    s->skipped = 0;
    s->reported = 1;
    s->last_ms = now;
    return 1;
}

// Under the lock, after the walk: 1 for a full report, 0 for a "same" record (*as: the pid of the full report it repeats).
static inline int crash_full(struct crash_table *table, const char *name, uint32_t hash, uint64_t now, int32_t pid,
                             unsigned mode, int32_t *as)
{
    struct crash_slot *s = crash_find(table, name);
    if (!s) return 1;
    if (!(mode & CRASH_MODE_ALL) && s->full && s->hash == hash && now - s->full_ms < CRASH_REPEAT_MS) {
        *as = s->full_pid;
        return 0;
    }
    s->full = 1;
    s->hash = hash;
    s->full_ms = now;
    s->full_pid = pid;
    return 1;
}

static inline void crash_emit_line(const struct crash_sink *sink, struct crash_text *x)
{
    if (sink->emit && x->used) sink->emit(sink->context, x->at);
}

static inline void crash_write(const struct boot_progress_crash *c, const struct crash_trace *t, int full, int32_t as,
                               uint32_t skipped, unsigned mode, const struct crash_sink *sink)
{
    char line[CRASH_LINE + 1] CRASH_UNINIT;
    struct crash_text x;
    crash_text_start(&x, line, (unsigned)sizeof(line));
    if (!full) {
        crash_put_pid(&x, "same", c->pid);
        crash_put_char(&x, ' ');
        crash_put_name(&x, c->name, CRASH_NAME);
        crash_put_str(&x, " sig=");
        crash_put_dec(&x, c->signal);
        crash_put_str(&x, " hash=");
        crash_put_hex8(&x, t->hash);
        crash_put_str(&x, " as=");
        crash_put_dec(&x, as);
        crash_put_str(&x, " skipped=");
        crash_put_dec(&x, skipped);
        crash_emit_line(sink, &x);
        return;
    }
    crash_put_pid(&x, "crash", c->pid);
    crash_put_char(&x, ' ');
    crash_put_name(&x, c->name, CRASH_NAME);
    crash_put_str(&x, " sig=");
    crash_put_dec(&x, c->signal);
    crash_put_str(&x, " exc=");
    crash_put_dec(&x, c->exception);
    crash_put_key_hex(&x, " code=", c->code);
    crash_put_key_hex(&x, " sub=", c->subcode);
    crash_put_key_hex(&x, " esr=", c->esr);
    crash_put_key_hex(&x, " far=", c->far);
    crash_put_key_hex(&x, " tid=", c->thread_id);
    crash_emit_line(sink, &x);

    crash_text_start(&x, line, (unsigned)sizeof(line));
    crash_put_pid(&x, "regs", c->pid);
    crash_put_str(&x, " pc=");
    crash_put_addr(&x, &t->pc);
    crash_put_str(&x, " lr=");
    crash_put_addr(&x, &t->lr);
    crash_put_key_hex(&x, " fp=", c->fp);
    crash_put_key_hex(&x, " sp=", c->sp);
    crash_put_key_hex(&x, " x0=", c->x0);
    if (t->x1.kind == CRASH_CACHE) {
        crash_put_str(&x, " x1=");
        crash_put_addr(&x, &t->x1);
    } else {
        crash_put_key_hex(&x, " x1=", c->x1);
    }
    crash_put_key_hex(&x, " cpsr=", c->cpsr);
    crash_emit_line(sink, &x);

    crash_text_start(&x, line, (unsigned)sizeof(line));
    crash_put_pid(&x, "map", c->pid);
    crash_put_key_hex(&x, " slide=", c->cache_end > c->cache_start ? c->cache_slide : 0);
    crash_put_key_hex(&x, " main=", c->main_header);
    crash_put_str(&x, " hash=");
    crash_put_hex8(&x, t->hash);
    crash_put_str(&x, " skipped=");
    crash_put_dec(&x, skipped);
    if (t->async) {
        crash_put_str(&x, " async=");
        crash_put_dec(&x, t->async);
    }
    if (mode & CRASH_MODE_ALL) crash_put_str(&x, " all");
    crash_emit_line(sink, &x);

    if (t->images.count) {
        crash_text_start(&x, line, (unsigned)sizeof(line));
        crash_put_pid(&x, "img", c->pid);
        const unsigned prefix = x.used;
        for (unsigned i = 0; i < t->images.count; i++) {
            const char *name = t->images.map[i].name;
            unsigned length = 0;
            while (length < BOOT_PROGRESS_CRASH_NAME - 1 && name[length]) length++;
            if (x.used > prefix && x.used + 3 + (length ? length : 1) > CRASH_LINE) {
                crash_emit_line(sink, &x);
                crash_text_start(&x, line, (unsigned)sizeof(line));
                crash_put_pid(&x, "img", c->pid);
            }
            crash_put_char(&x, ' ');
            crash_put_char(&x, t->images.letter[i]);
            crash_put_char(&x, '=');
            crash_put_name(&x, name, BOOT_PROGRESS_CRASH_NAME - 1);
        }
        crash_emit_line(sink, &x);
    }

    // The return addresses, at most CRASH_PER_LINE a record, then end=<why>[@0x<fp>] on the last one.
    unsigned i = 0;
    do {
        crash_text_start(&x, line, (unsigned)sizeof(line));
        crash_put_pid(&x, "bt", c->pid);
        crash_put_char(&x, ' ');
        crash_put_dec(&x, i);
        for (unsigned on_line = 0; i < t->frames && on_line < CRASH_PER_LINE; on_line++, i++) {
            if (x.used + 1 + crash_addr_length(&t->frame[i]) > CRASH_LINE) break;
            crash_put_char(&x, ' ');
            crash_put_addr(&x, &t->frame[i]);
        }
        if (i == t->frames) {
            const unsigned need = 5 + 5 + (t->stop != CRASH_STOP_END ? 19 : 0);
            if (x.used + need > CRASH_LINE && i) {
                crash_emit_line(sink, &x);
                crash_text_start(&x, line, (unsigned)sizeof(line));
                crash_put_pid(&x, "bt", c->pid);
                crash_put_char(&x, ' ');
                crash_put_dec(&x, i);
            }
            crash_put_str(&x, " end=");
            crash_put_str(&x, crash_stop_names[t->stop < 7 ? t->stop : 0]);
            if (t->stop != CRASH_STOP_END) {
                crash_put_str(&x, "@0x");
                crash_put_hex(&x, t->stop_at);
            }
        }
        crash_emit_line(sink, &x);
    } while (i < t->frames);
}

// The whole report: signal filter, rate limit, walk (no lock held), records.
static inline void crash_report(struct crash_table *table, const struct boot_progress_crash *c, unsigned mode,
                                const struct crash_sink *sink)
{
    if (!c || c->size < sizeof(*c) || c->version != BOOT_PROGRESS_CRASH_VERSION || mode == CRASH_MODE_OFF ||
        !crash_signal_traced(c->signal))
        return;
    uint32_t skipped = 0;
    uint64_t flags = sink->lock ? sink->lock() : 0;
    int admitted = 1;
    if (flags != UINT64_MAX) {
        admitted = crash_admit(table, c->name, c->time_ms, mode, &skipped);
        if (sink->unlock) sink->unlock(flags);
    }
    if (!admitted) return;
    struct crash_trace trace CRASH_UNINIT;
    crash_take(c, &trace);
    int32_t as = 0;
    int full = 1;
    flags = sink->lock ? sink->lock() : 0;
    if (flags != UINT64_MAX) {
        full = crash_full(table, c->name, trace.hash, c->time_ms, c->pid, mode, &as);
        if (sink->unlock) sink->unlock(flags);
    }
    crash_write(c, &trace, full, as, skipped, mode, sink);
}

#endif
