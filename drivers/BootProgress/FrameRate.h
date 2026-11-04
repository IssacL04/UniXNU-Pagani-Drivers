// SPDX-License-Identifier: BSD-2-Clause-Patent
// Frame rate of the display path (pagani-fps; BootProgress.c, boot_progress_frame in PeripheralStatus.h). Every frame that reached the
// panel, from the CPU copy or from the GPU presenter, goes into a ring with the mach time it got there: two atomic adds and one 64-bit
// store, no lock, any thread. The 1 Hz observer reads the ring without stopping the writers and derives: the frames of the last second
// and of the nine seconds before it (FPS and its range), the frame times of the last second (average, 99th percentile, largest), the
// last second's split between the two paths, and every frame interval above FRAME_JITTER_MS since its previous pass (the jitter records
// of the ramoops log, rate-limited by frame_limit_take). The row it prints is at most 65 characters.
// Where the time went (boot_progress_frame_timed): beside its slot every frame keeps its breakdown (frame_breakdown: gap, off, take,
// copy, done in microseconds and its flags), stored before the slot and checked by generation like it; the scan names the frame of the
// last second's largest interval (max_n) and of every jitter (frame_jitter n), and frame_slow_row prints the SLOW row from them, from
// the newest frame's stage (frame_stage_word) and from this second's fallbacks and empty client waits.
// Pure integer arithmetic on tick values: no floating point (the kernel builds this with -mgeneral-regs-only) and no aggregate copies
// or zeroing initializers (the early console must not call memset or memcpy). C and C++; host tested by BootProgress/Test/console.c
// (through BootProgress.c) and NativeMobileDisplay/Test/frame-rate.cpp and frame-timing.cpp (directly).
#ifndef PAGANI_FRAME_RATE_H
#define PAGANI_FRAME_RATE_H
#include <stdint.h>
#include "PeripheralStatus.h"

enum {
    FRAME_RING = 2048,              // frames kept: ten seconds up to 204 frames a second (a power of two)
    FRAME_WINDOWS = 10,             // the FPS range: the last ten one-second windows
    FRAME_TOP = 24,                 // largest frame times kept for the 99th percentile (enough for up to FRAME_RING of them)
    FRAME_JITTER_MS = 50,           // a frame interval above this is a jitter...
    FRAME_IDLE_MS = 1000,           // ...below this: from here on the compositor had nothing to show (idle); no frame time either
    FRAME_JITTER_EVENTS = 16,       // jitters one pass keeps for the log (the newest)
    FRAME_JITTER_BURST = 4,         // jitter records in the log: at most this many at once,
    FRAME_JITTER_PERIOD_S = 5,      // then one per this many seconds
    FRAME_CPU = 0,                  // paths (PAGANI_FRAME_CPU, PAGANI_FRAME_GPU)
    FRAME_GPU = 1,
};

// A slot: bits [52:0] the stamp (mach ticks; 2^53 ticks at 19.2 MHz are 14 years), bit 53 the path, bits [63:54] the generation of
// the frame number n that wrote it, (n / FRAME_RING + 1) mod 1024, so a slot not yet written for frame n (still zero, or still holding
// frame n - FRAME_RING) never passes for it.
#define FRAME_STAMP_MASK ((1ULL << 53) - 1)
static inline uint64_t frame_generation(uint64_t n) {return (n / FRAME_RING + 1) & 0x3ff;}

// A frame's breakdown, microseconds (FRAME_US_NONE: that stage did not happen; longer than FRAME_US_MAX saturates there, 67 s): gap
// from the previous frame's arrival, off arrival -> offer, take offer -> taken, copy taken -> copied, done copied -> presented; flags
// the PAGANI_TIMING_* bits and FRAME_TIMED (a breakdown was given). In the ring as three words beside the slot, each carrying the
// frame's generation in bits [63:54] like the slot and two 26-bit values, a in [25:0] and b in [51:26]: (gap, off), (take, copy),
// (done, flags).
enum { FRAME_TIMING_WORDS = 3, FRAME_US_BITS = 26 };
#define FRAME_US_NONE ((1u << FRAME_US_BITS) - 1)
#define FRAME_US_MAX (FRAME_US_NONE - 1)
#define FRAME_TIMED (1u << 25)

struct frame_breakdown {
    uint32_t gap_us, off_us, take_us, copy_us, done_us;
    uint32_t flags;
};

struct frame_ring {
    uint64_t slot[FRAME_RING];      // frame n in slot n % FRAME_RING
    uint64_t timing[FRAME_RING][FRAME_TIMING_WORDS];    // frame n's breakdown, in timing[n % FRAME_RING]
    uint64_t count;                 // frames recorded (the next frame's number)
    uint64_t paths[2];              // frames recorded per path
};

// Microseconds of `ticks` at `hz`, saturated at FRAME_US_MAX; FRAME_US_NONE without a clock.
static inline uint32_t frame_us(uint64_t ticks, uint64_t hz)
{
    if (!hz) return FRAME_US_NONE;
    const uint64_t whole = ticks / hz;
    if (whole >= (uint64_t)FRAME_US_MAX / 1000000 + 1) return FRAME_US_MAX;
    const uint64_t us = whole * 1000000 + ticks % hz * 1000000 / hz;
    return us > FRAME_US_MAX ? FRAME_US_MAX : (uint32_t)us;
}

// From stamp `from` to stamp `to` (ticks; 0: that stage did not happen), never negative (stamps of two CPUs).
static inline uint32_t frame_span_us(uint64_t from, uint64_t to, uint64_t hz)
{
    if (!from || !to) return FRAME_US_NONE;
    return frame_us(to > from ? to - from : 0, hz);
}

// The breakdown of a frame presented at `presented` with the stages in `t` (PeripheralStatus.h pagani_frame_timing).
static inline void frame_breakdown_of(const struct pagani_frame_timing *t, uint64_t presented, uint64_t hz, struct frame_breakdown *b)
{
    b->gap_us = t->gap ? frame_us(t->gap, hz) : FRAME_US_NONE;
    b->off_us = frame_span_us(t->arrival, t->offer, hz);
    b->take_us = frame_span_us(t->offer, t->taken, hz);
    b->copy_us = frame_span_us(t->taken, t->copied, hz);
    b->done_us = frame_span_us(t->copied, presented, hz);
    b->flags = FRAME_TIMED | (t->flags & PAGANI_TIMING_FLAGS);
}

static inline uint64_t frame_pair(uint32_t a, uint32_t b)
{
    return (uint64_t)(b & FRAME_US_NONE) << FRAME_US_BITS | (a & FRAME_US_NONE);
}

// One frame on `path` at `stamp` with its breakdown (NULL: none, flags 0): its number, then the breakdown words, then the slot (release:
// a reader that sees the slot sees the words), then the path's count.
static inline void frame_ring_record_breakdown(struct frame_ring *ring, unsigned path, uint64_t stamp, const struct frame_breakdown *b)
{
    path &= 1;
    const uint64_t n = __atomic_fetch_add(&ring->count, 1, __ATOMIC_RELAXED);
    const uint64_t generation = frame_generation(n) << 54;
    uint64_t *words = ring->timing[n % FRAME_RING];
    __atomic_store_n(&words[0], generation | frame_pair(b ? b->gap_us : FRAME_US_NONE, b ? b->off_us : FRAME_US_NONE), __ATOMIC_RELAXED);
    __atomic_store_n(&words[1], generation | frame_pair(b ? b->take_us : FRAME_US_NONE, b ? b->copy_us : FRAME_US_NONE), __ATOMIC_RELAXED);
    __atomic_store_n(&words[2], generation | frame_pair(b ? b->done_us : FRAME_US_NONE, b ? b->flags : 0), __ATOMIC_RELAXED);
    __atomic_store_n(&ring->slot[n % FRAME_RING], (stamp & FRAME_STAMP_MASK) | (uint64_t)path << 53 | generation, __ATOMIC_RELEASE);
    __atomic_fetch_add(&ring->paths[path], 1, __ATOMIC_RELAXED);
}

static inline void frame_ring_record(struct frame_ring *ring, unsigned path, uint64_t stamp)
{
    frame_ring_record_breakdown(ring, path, stamp, 0);
}

// Frame n's breakdown; 0 when its words do not hold it (reused by a later frame) or it was recorded without one.
static inline int frame_ring_breakdown(const struct frame_ring *ring, uint64_t n, struct frame_breakdown *b)
{
    uint64_t words[FRAME_TIMING_WORDS];
    for (unsigned i = 0; i < FRAME_TIMING_WORDS; i++) {
        words[i] = __atomic_load_n(&ring->timing[n % FRAME_RING][i], __ATOMIC_ACQUIRE);
        if (words[i] >> 54 != frame_generation(n)) return 0;
    }
    b->gap_us = (uint32_t)words[0] & FRAME_US_NONE;
    b->off_us = (uint32_t)(words[0] >> FRAME_US_BITS) & FRAME_US_NONE;
    b->take_us = (uint32_t)words[1] & FRAME_US_NONE;
    b->copy_us = (uint32_t)(words[1] >> FRAME_US_BITS) & FRAME_US_NONE;
    b->done_us = (uint32_t)words[2] & FRAME_US_NONE;
    b->flags = (uint32_t)(words[2] >> FRAME_US_BITS) & FRAME_US_NONE;
    return (b->flags & FRAME_TIMED) != 0;
}

// The newest frame's stage (boot_progress_frame_stage) as one word: the stamp in bits [52:0], PAGANI_STAGE_* in bits [58:56].
static inline uint64_t frame_stage_word(unsigned stage, uint64_t stamp)
{
    return (stamp & FRAME_STAMP_MASK) | (uint64_t)(stage & 7) << 56;
}
static inline unsigned frame_stage_of(uint64_t word) {return (unsigned)(word >> 56) & 7;}
static inline uint64_t frame_stage_stamp(uint64_t word) {return word & FRAME_STAMP_MASK;}

// Frame n's stamp and path; 0 when its slot does not hold it (not stored yet, or reused by a later frame).
static inline int frame_ring_get(const struct frame_ring *ring, uint64_t n, uint64_t *stamp, unsigned *path)
{
    const uint64_t value = __atomic_load_n(&ring->slot[n % FRAME_RING], __ATOMIC_ACQUIRE);
    if (value >> 54 != frame_generation(n)) return 0;
    *stamp = value & FRAME_STAMP_MASK;
    *path = (unsigned)(value >> 53) & 1;
    return 1;
}

// The largest values so far in descending order (at most FRAME_TOP).
static inline void frame_top_add(uint64_t *top, unsigned *tops, uint64_t value)
{
    unsigned i = *tops;
    if (i == FRAME_TOP) {
        if (value <= top[FRAME_TOP - 1]) return;
        i = FRAME_TOP - 1;
    } else {
        *tops = i + 1;
    }
    for (; i && top[i - 1] < value; i--) top[i] = top[i - 1];
    top[i] = value;
}

// The 99th percentile of n values, linear between the closest ranks (numpy's default): with v sorted ascending and h = (n - 1) * 0.99,
// v[floor(h)] + (h - floor(h)) * (v[floor(h) + 1] - v[floor(h)]). `top` holds the largest of the values in descending order; for
// n <= FRAME_RING the two needed are among the 22 largest (v[floor(h)] is top[n - 1 - floor(h)]).
static inline uint64_t frame_p99(const uint64_t *top, unsigned tops, uint64_t n)
{
    if (!n || !tops) return 0;
    const uint64_t h = (n - 1) * 99, low = h / 100, fraction = h % 100;
    uint64_t i = n - 1 - low;
    if (i >= tops) i = tops - 1;
    const uint64_t a = top[i], b = i ? top[i - 1] : a;
    return a + (b - a) * fraction / 100;
}

struct frame_jitter {
    uint64_t age;                   // ticks from the late frame to the pass
    uint64_t n;                     // the late frame's number (its breakdown: frame_ring_breakdown)
    uint32_t interval_us;           // its interval to the frame before it
    uint32_t path;
};

struct frame_scan {
    uint32_t fps, fps_low, fps_high;    // frames in the last second; their range over the one-second windows of the last ten seconds
    uint32_t windows;                   // that the ring covered (1 to FRAME_WINDOWS; the last second always counts)
    uint32_t paths[2];                  // the last second's frames per path
    uint32_t intervals;                 // frame times among them: intervals to the frame before below FRAME_IDLE_MS
    uint64_t avg_us, p99_us, max_us;
    uint64_t max_n;                     // the frame that ended the largest of them (the newest of equal ones); with intervals only
    uint64_t next;                      // the first frame of the next pass's jitter scan
    uint64_t lost;                      // frames whose interval this pass could not see (their slots were reused first)
    uint32_t jitters, kept;             // jitter intervals among frames [from, next), and how many `events` keeps (newest first)
    struct frame_jitter events[FRAME_JITTER_EVENTS];
};

// One pass at `now` (ticks, `hz` a second) over frames [0, count) (count loaded before now was read), newest first: back through the
// ten windows and at least to frame `from` - 1 (the previous pass's `next`), as far as the ring still holds them. A newest frame whose
// slot is not stored yet is left to the next pass; out-of-order stamps (writers on two CPUs) count as interval 0.
static inline void frame_ring_scan(const struct frame_ring *ring, uint64_t count, uint64_t now, uint64_t hz, uint64_t from,
                                   struct frame_scan *out)
{
    uint32_t window[FRAME_WINDOWS];
    uint64_t top[FRAME_TOP];
    unsigned tops = 0;
    for (unsigned w = 0; w < FRAME_WINDOWS; w++) window[w] = 0;
    out->fps = out->fps_low = out->fps_high = 0;
    out->windows = 1;
    out->paths[0] = out->paths[1] = 0;
    out->intervals = 0;
    out->avg_us = out->p99_us = out->max_us = 0;
    out->max_n = 0;
    out->next = count;
    out->lost = 0;
    out->jitters = out->kept = 0;
    if (!hz) return;
    if (from > count) from = count;
    const uint64_t span = (uint64_t)FRAME_WINDOWS * hz, jitter = hz * FRAME_JITTER_MS / 1000, idle = hz * FRAME_IDLE_MS / 1000;
    const uint64_t oldest = count > FRAME_RING ? count - FRAME_RING : 0;
    uint64_t end = count, checked = count, sum = 0, newer = 0, newer_n = 0, seen = 0;
    unsigned newer_path = 0;
    int have = 0, complete = 0;
    for (uint64_t n = count;;) {
        if (n == oldest) {
            complete = !oldest;             // every frame since the first was read
            break;
        }
        n--;
        uint64_t stamp = 0;
        unsigned path = 0;
        if (!frame_ring_get(ring, n, &stamp, &path)) {
            if (!have && n + 1 == count) {  // the newest frame is still being stored: the next pass takes it
                end = checked = n;
                continue;
            }
            break;                          // reused while this pass read it: the older frames are gone as well
        }
        if (stamp > now) stamp = now;
        const uint64_t age = now - stamp;
        if (have) {                         // the newer frame's interval
            const uint64_t interval = newer > stamp ? newer - stamp : 0;
            checked = newer_n;
            if (now - newer < hz && interval < idle) {
                sum += interval;
                if (!out->intervals || interval > top[0]) out->max_n = newer_n;
                out->intervals++;
                frame_top_add(top, &tops, interval);
            }
            if (newer_n >= from && interval > jitter && interval < idle) {
                if (out->kept < FRAME_JITTER_EVENTS) {
                    struct frame_jitter *j = &out->events[out->kept++];
                    j->age = now - newer;
                    j->n = newer_n;
                    j->interval_us = (uint32_t)(interval * 1000000 / hz);
                    j->path = newer_path;
                }
                out->jitters++;
            }
        }
        if (age < span) {
            const uint64_t w = age / hz;
            window[w]++;
            if (!w) out->paths[path]++;
        }
        seen = age;
        have = 1;
        newer = stamp;
        newer_n = n;
        newer_path = path;
        if (age >= span && n < from) {      // older than every window, and frame `from`'s interval is seen
            complete = 1;
            break;
        }
    }
    out->fps = out->fps_low = out->fps_high = window[0];
    for (unsigned w = 1; w < FRAME_WINDOWS; w++) {
        if (!complete && seen < (uint64_t)(w + 1) * hz) break;      // older frames of this window may be gone
        if (window[w] < out->fps_low) out->fps_low = window[w];
        if (window[w] > out->fps_high) out->fps_high = window[w];
        out->windows++;
    }
    if (out->intervals) {
        out->avg_us = sum / out->intervals * 1000000 / hz;
        out->p99_us = frame_p99(top, tops, out->intervals) * 1000000 / hz;
        out->max_us = top[0] * 1000000 / hz;
    }
    out->next = end;
    const uint64_t first = from ? from : 1;     // frame 0 has no interval
    out->lost = checked > first ? checked - first : 0;
}

struct frame_text {
    char *at;
    unsigned used, room;
};

static inline void frame_put(struct frame_text *t, char c)
{
    if (t->used + 1 >= t->room) return;
    t->at[t->used++] = c;
    t->at[t->used] = 0;
}

static inline void frame_puts(struct frame_text *t, const char *s)
{
    while (*s) frame_put(t, *s++);
}

static inline void frame_putu(struct frame_text *t, uint64_t value)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) frame_put(t, digits[--n]);
}

// Milliseconds from microseconds: one decimal below 100 ms ("17.2"), whole ones from there ("412"); at most four characters below
// FRAME_IDLE_MS.
static inline void frame_putms(struct frame_text *t, uint64_t us)
{
    const uint64_t tenths = (us + 50) / 100;
    if (tenths < 1000) {
        frame_putu(t, tenths / 10);
        frame_put(t, '.');
        frame_putu(t, tenths % 10);
    } else {
        frame_putu(t, (us + 500) / 1000);
    }
}

static inline uint64_t frame_count_shown(uint64_t value) {return value > 999 ? 999 : value;}

// The FPS row, e.g. "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13": the frames of the last second with their range
// over the last ten one-second windows, the last second's frame times (average, 99th percentile, largest; "ft -" without any) and its
// frames per path. Counts above 999 print as 999, so the row is at most 65 characters. Returns its length (cut to size - 1).
static inline unsigned frame_row(char *row, unsigned size, const struct frame_scan *s)
{
    struct frame_text t;
    t.at = row;
    t.used = 0;
    t.room = size;
    if (!size) return 0;
    row[0] = 0;
    frame_puts(&t, "FPS ");
    frame_putu(&t, frame_count_shown(s->fps));
    frame_puts(&t, " (");
    frame_putu(&t, frame_count_shown(s->fps_low));
    frame_put(&t, '-');
    frame_putu(&t, frame_count_shown(s->fps_high));
    frame_puts(&t, ") | ft ");
    if (s->intervals) {
        frame_putms(&t, s->avg_us);
        frame_puts(&t, " p99 ");
        frame_putms(&t, s->p99_us);
        frame_puts(&t, " max ");
        frame_putms(&t, s->max_us);
        frame_puts(&t, "ms");
    } else {
        frame_put(&t, '-');
    }
    frame_puts(&t, " | gpu ");
    frame_putu(&t, frame_count_shown(s->paths[FRAME_GPU]));
    frame_puts(&t, " cpu ");
    frame_putu(&t, frame_count_shown(s->paths[FRAME_CPU]));
    return t.used;
}

// A duration of a breakdown: "-" without one, milliseconds with one decimal below 100 ms ("17.2"), whole ones below 10 s ("412",
// "1234"), then whole seconds ("67s"); at most four characters.
static inline void frame_putdur(struct frame_text *t, uint32_t us)
{
    if (us == FRAME_US_NONE) {
        frame_put(t, '-');
    } else if (us < 9999500) {
        frame_putms(t, us);
    } else {
        frame_putu(t, ((uint64_t)us + 500000) / 1000000);
        frame_put(t, 's');
    }
}

// How long the newest frame has been waiting: "412ms" below a second (one decimal below 100 ms), then seconds with one decimal below
// 100 s ("2.3s"), then whole seconds (at most 99999); at most six characters.
static inline void frame_putage(struct frame_text *t, uint64_t us)
{
    if (us < 999500) {
        frame_putms(t, us);
        frame_puts(t, "ms");
        return;
    }
    const uint64_t tenths = (us + 50000) / 100000;
    if (tenths < 1000) {
        frame_putu(t, tenths / 10);
        frame_put(t, '.');
        frame_putu(t, tenths % 10);
    } else {
        const uint64_t seconds = (us + 500000) / 1000000;
        frame_putu(t, seconds > 99999 ? 99999 : seconds);
    }
    frame_put(t, 's');
}

// The part of a frame's way the newest frame is in, by the stage it reached last: waiting to be offered ("off"), to be taken ("take"),
// for its copy ("copy"), to be presented ("done"); "idle" after the last frame (the compositor's next one has not come). 0: no frame yet.
static inline const char *frame_stage_name(unsigned stage)
{
    if (stage == PAGANI_STAGE_ARRIVED) return "off";
    if (stage == PAGANI_STAGE_OFFERED) return "take";
    if (stage == PAGANI_STAGE_TAKEN) return "copy";
    if (stage == PAGANI_STAGE_COPIED) return "done";
    if (stage == PAGANI_STAGE_IDLE) return "idle";
    return 0;
}

// The SLOW row's inputs: the last second's largest frame interval (have: there was one) with the breakdown of the frame that ended it
// (timed: the ring still held one), this second's CPU fallbacks (refused offers and failed GPU copies) and empty waits of the GPU client,
// and the stage the newest frame reached last with how long ago (PAGANI_STAGE_NONE: no frame yet).
struct frame_slow {
    int have, timed;
    uint64_t interval_us;
    struct frame_breakdown b;
    uint32_t fallbacks, timeouts;
    unsigned stage;
    uint64_t stage_us;
};

// The SLOW row: "SLOW 88.0ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0", "SLOW 88.0ms fb0 to0" without a breakdown,
// "SLOW - copy 2.3s fb0 to3" without a frame interval ("SLOW - fb0 to0" before the first frame). " done" only while the row stays
// within `width` characters; everything else stays within 65 (counts above 999 print as 999). Returns its length (cut to size - 1).
static inline unsigned frame_slow_row(char *row, unsigned size, unsigned width, const struct frame_slow *s)
{
    struct frame_text t, d, tail;
    char done[16], counts[24];
    t.at = row;
    t.used = 0;
    t.room = size;
    d.at = done;
    d.used = 0;
    d.room = sizeof(done);
    done[0] = 0;
    tail.at = counts;
    tail.used = 0;
    tail.room = sizeof(counts);
    counts[0] = 0;
    if (!size) return 0;
    row[0] = 0;
    frame_puts(&t, "SLOW ");
    if (s->have) {
        frame_putms(&t, s->interval_us);
        frame_puts(&t, "ms");
        if (s->timed) {
            frame_puts(&t, " gap ");
            frame_putdur(&t, s->b.gap_us);
            frame_puts(&t, " off ");
            frame_putdur(&t, s->b.off_us);
            frame_puts(&t, " take ");
            frame_putdur(&t, s->b.take_us);
            frame_puts(&t, " copy ");
            frame_putdur(&t, s->b.copy_us);
            if (s->b.done_us != FRAME_US_NONE) {
                frame_puts(&d, " done ");
                frame_putdur(&d, s->b.done_us);
            }
        }
    } else {
        frame_put(&t, '-');
        const char *name = frame_stage_name(s->stage);
        if (name) {
            frame_put(&t, ' ');
            frame_puts(&t, name);
            frame_put(&t, ' ');
            frame_putage(&t, s->stage_us);
        }
    }
    frame_puts(&tail, " fb");
    frame_putu(&tail, frame_count_shown(s->fallbacks));
    frame_puts(&tail, " to");
    frame_putu(&tail, frame_count_shown(s->timeouts));
    if (t.used + d.used + tail.used <= width) frame_puts(&t, done);
    frame_puts(&t, counts);
    return t.used;
}

// A late frame's breakdown for its J record: "gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1[ refused][ gpu-failed][ bypass][ deferred]
// [ flip] fb0 to0" (b NULL: the counts only), with this second's fallbacks and empty client waits. Returns its length (cut to size - 1).
static inline unsigned frame_jitter_detail(char *out, unsigned size, const struct frame_breakdown *b, uint32_t fallbacks,
                                           uint32_t timeouts)
{
    struct frame_text t;
    t.at = out;
    t.used = 0;
    t.room = size;
    if (!size) return 0;
    out[0] = 0;
    if (b) {
        frame_puts(&t, "gap ");
        frame_putdur(&t, b->gap_us);
        frame_puts(&t, " off ");
        frame_putdur(&t, b->off_us);
        frame_puts(&t, " take ");
        frame_putdur(&t, b->take_us);
        frame_puts(&t, " copy ");
        frame_putdur(&t, b->copy_us);
        frame_puts(&t, " done ");
        frame_putdur(&t, b->done_us);
        if (b->flags & PAGANI_TIMING_REFUSED) frame_puts(&t, " refused");
        if (b->flags & PAGANI_TIMING_FAILED) frame_puts(&t, " gpu-failed");
        if (b->flags & PAGANI_TIMING_BYPASS) frame_puts(&t, " bypass");
        if (b->flags & PAGANI_TIMING_DEFERRED) frame_puts(&t, " deferred");
        if (b->flags & PAGANI_TIMING_FLIP) frame_puts(&t, " flip");
        frame_put(&t, ' ');
    }
    frame_puts(&t, "fb");
    frame_putu(&t, frame_count_shown(fallbacks));
    frame_puts(&t, " to");
    frame_putu(&t, frame_count_shown(timeouts));
    return t.used;
}

// The jitter records' limit: FRAME_JITTER_BURST at once, then one per FRAME_JITTER_PERIOD_S (the credit is in ticks). `skipped` counts
// what was refused until the caller has reported it.
struct frame_limit {
    uint64_t credit, stamp;
    uint32_t skipped, primed;
};

static inline int frame_limit_take(struct frame_limit *l, uint64_t now, uint64_t hz)
{
    const uint64_t period = (uint64_t)FRAME_JITTER_PERIOD_S * hz, burst = (uint64_t)FRAME_JITTER_BURST * period;
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
    if (period && l->credit >= period) {
        l->credit -= period;
        return 1;
    }
    l->skipped++;
    return 0;
}
#endif
