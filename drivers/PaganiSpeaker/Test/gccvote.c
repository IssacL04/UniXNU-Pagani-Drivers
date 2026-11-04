/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The GCC branch vote register 0x52008 under its writers, interleaved. Four writers run as threads that a deterministic scheduler
 * lets run one at a time, switching at every register access and every delay (the points where the hardware would let another CPU in),
 * in an order a seeded generator picks:
 *   touch    Sm8750Platform sm8750_touch_clock (PeripheralIO.h, as compiled into PaganiInput), bits 18-21 and 26, then its branch polls
 *   speaker  PaganiSpeaker speaker_clocks_on (SpeakerHw.c), bits 7, 8 and 14, then its RCG and branch polls
 *   gauge    PaganiBattery's own protocol, written out here as BatteryI2c.c vote_bits under PaganiBattery.cpp voteLock: compare-and-swap
 *            0 -> 'BATT' every 10 us for 50 ms, read, write the missing bits 7, 8, 11, read back, store 0 (PaganiBattery is not changed)
 *   pcie     PaganiWifi's protocol (PaganiWifi.cpp hwVoteLock: compare-and-swap in, compare-and-swap out), bits 0-6
 * The register model: a branch runs while its vote bits are set (a dropped vote stops it again, as on the SoC).
 * Checked over thousands of schedules: with every writer on the lock word no vote is ever lost, every writer succeeds, the word ends
 * free and nothing was written without it; the same schedules with the touch and the speaker's updates as they were before (a bare
 * read-modify-write) do lose votes, so the schedules reach the race the lock closes; a holder that never gives the word back costs each
 * writer 50 ms and one counted unlocked update, never a hang; a vote already in place takes neither a write nor the lock. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PeripheralIO.h>
#include "SpeakerBoard.h"
#include "SpeakerHw.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {Touch, Speaker, Gauge, Pcie, Writers};
#define GAUGE_BITS ((1u << 7) | (1u << 8) | (1u << 11))
#define PCIE_BITS 0x7fu
#define TAG_BATT 0x42415454u
#define TAG_WIFI 0x57494649u

/* ---- the scheduler: one runner at a time; every yield lets the generator pick who goes on ---- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int turn, alive[Writers];
    uint64_t rng;
    unsigned switches;
} S = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, -1, {0}, 0, 0};
static __thread int me;

static uint32_t next_random(void)
{
    S.rng ^= S.rng << 13; S.rng ^= S.rng >> 7; S.rng ^= S.rng << 17;
    return (uint32_t)(S.rng >> 11);
}
/* With the mutex held: the next runner among the living (-1 when none is left). */
static void pick(void)
{
    int living[Writers], n = 0;
    for (int i = 0; i < Writers; ++i) if (S.alive[i]) living[n++] = i;
    S.turn = n ? living[next_random() % (uint32_t)n] : -1;
    if (n) S.switches++;
    pthread_cond_broadcast(&S.cv);
}
static void wait_turn(void) {while (S.turn != me) pthread_cond_wait(&S.cv, &S.mu);}
static void yield(void)
{
    pthread_mutex_lock(&S.mu);
    pick();
    wait_turn();
    pthread_mutex_unlock(&S.mu);
}

/* ---- the GCC: the vote register, the touch's branches and DFS table, the I2C hub's branches and RCG ---- */
static struct {
    uint32_t vote;
    uint64_t us;            /* every delay of every writer advances it */
    unsigned writes, unlocked_writes;
} G;
static int on(uint32_t bits) {return (G.vote & bits) == bits;}
static uint32_t gcc_value(uint32_t reg)
{
    switch (reg) {
    case SM8750_GCC_VOTE: return G.vote;
    case 0x2315c: case 0x23148: case 0x23140: case 0x23144: case 0x183cc: return on(SM8750_GCC_VOTE_TOUCH) ? 0u : 0x80000000u;
    case SpeakerGccHaltSahb: return on(1u << 7) ? 0u : 0x80000000u;
    case SpeakerGccHaltCore: return on(1u << 8) ? 0u : 0x80000000u;
    case SpeakerGccHaltS4: return on(1u << 14) ? 0u : 0x80000000u;
    default: return 0;      /* the DFS table off and its one level on the XO; the SE4 RCG on the XO */
    }
}
static uint32_t gcc_read(void *context, uint32_t reg) {(void)context; yield(); return gcc_value(reg);}
static const uint32_t kTag[Writers] = {SM8750_GCC_OWNER_TOUCH, SPEAKER_GCC_OWNER, TAG_BATT, TAG_WIFI};
static void gcc_write(void *context, uint32_t reg, uint32_t value)
{
    (void)context;
    yield();
    CHECK(reg == SM8750_GCC_VOTE);            /* nobody writes anything else here */
    G.writes++;
    if (sm8750_gcc_vote_owner != kTag[me]) G.unlocked_writes++;   /* written by a writer that does not hold the lock word */
    G.vote = value;
}
static void gcc_delay(void *context, unsigned us) {(void)context; G.us += us; yield();}
static const struct peripheral_io kGcc = {0, gcc_read, gcc_write, gcc_delay};

/* the speaker's register windows: only its GCC window is used by speaker_clocks_on */
static uint32_t spk_read(void *context, unsigned window, uint32_t offset)
{
    CHECK(window == SpeakerWinGcc);
    return gcc_read(context, offset);
}
static void spk_write(void *context, unsigned window, uint32_t offset, uint32_t value)
{
    CHECK(window == SpeakerWinGcc);
    gcc_write(context, offset, value);
}
static void spk_delay(void *context, uint32_t us) {gcc_delay(context, us);}
static const SpeakerIo kSpeakerIo = {0, spk_read, spk_write, spk_delay};

/* ---- the writers ---- */
static int legacy;          /* 1: the touch and the speaker update the register as before the lock (bare read-modify-write) */
static int rc[Writers];

static int touch_writer(void)
{
    if (!legacy) {
        unsigned hz = 0, select = 9;
        const int r = sm8750_touch_clock(&kGcc, &hz, &select);
        return r ? r : (hz == 19200000 && select == 0 ? 0 : -1);
    }
    const uint32_t vote = peripheral_read(&kGcc, SM8750_GCC_VOTE);   /* sm8750_touch_clock before the shared update */
    peripheral_write(&kGcc, SM8750_GCC_VOTE, vote | SM8750_GCC_VOTE_TOUCH);
    return 0;
}
static int speaker_writer(void)
{
    if (!legacy) {
        SpeakerClocks s;
        return speaker_clocks_on(&kSpeakerIo, &s);
    }
    const uint32_t vote = peripheral_read(&kGcc, SpeakerGccVote);   /* SpeakerHw.c before the shared update */
    if ((vote & SpeakerGccVoteBits) != SpeakerGccVoteBits) peripheral_write(&kGcc, SpeakerGccVote, vote | SpeakerGccVoteBits);
    return (peripheral_read(&kGcc, SpeakerGccVote) & SpeakerGccVoteBits) == SpeakerGccVoteBits ? 0 : -5;
}
/* A writer with its own copy of the lock protocol (the gauge: release by a store; PCIe: release by a compare-and-swap). */
static int protocol_writer(uint32_t tag, uint32_t bits, int cas_release)
{
    if ((peripheral_read(&kGcc, SM8750_GCC_VOTE) & bits) == bits) return 0;
    uint32_t step = 0;
    for (;; ++step) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (step >= 5000) return -16;
        gcc_delay(0, 10);
    }
    const uint32_t v = peripheral_read(&kGcc, SM8750_GCC_VOTE);
    if ((v & bits) != bits) peripheral_write(&kGcc, SM8750_GCC_VOTE, v | bits);
    const uint32_t after = peripheral_read(&kGcc, SM8750_GCC_VOTE);
    if (cas_release) {
        uint32_t expected = tag;
        __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&sm8750_gcc_vote_owner, 0u, __ATOMIC_RELEASE);
    }
    return (after & bits) == bits ? 0 : -5;
}

static void *runner(void *arg)
{
    me = (int)(intptr_t)arg;
    pthread_mutex_lock(&S.mu);
    wait_turn();
    pthread_mutex_unlock(&S.mu);
    switch (me) {
    case Touch: rc[me] = touch_writer(); break;
    case Speaker: rc[me] = speaker_writer(); break;
    case Gauge: rc[me] = protocol_writer(TAG_BATT, GAUGE_BITS, 0); break;
    default: rc[me] = protocol_writer(TAG_WIFI, PCIE_BITS, 1); break;
    }
    pthread_mutex_lock(&S.mu);
    S.alive[me] = 0;
    pick();
    pthread_mutex_unlock(&S.mu);
    return 0;
}

/* One schedule from seed with the writers in mask; returns the vote register at the end. */
static uint32_t schedule(uint64_t seed, unsigned mask, uint32_t initial)
{
    pthread_t threads[Writers];
    memset(&G, 0, sizeof G);
    G.vote = initial;
    S.rng = seed * 0x9e3779b97f4a7c15ull + 1;
    S.switches = 0;
    pthread_mutex_lock(&S.mu);
    for (int i = 0; i < Writers; ++i) {S.alive[i] = (int)((mask >> i) & 1u); rc[i] = 0;}
    pick();
    pthread_mutex_unlock(&S.mu);
    /* nothing here touches what the runners touch (the check counter included) until they all ended */
    int created[Writers] = {0}, joined[Writers] = {0};
    for (int i = 0; i < Writers; ++i) if (mask & (1u << i)) created[i] = pthread_create(&threads[i], 0, runner, (void *)(intptr_t)i);
    for (int i = 0; i < Writers; ++i) if ((mask & (1u << i)) && !created[i]) joined[i] = pthread_join(threads[i], 0);
    for (int i = 0; i < Writers; ++i) CHECK(created[i] == 0 && joined[i] == 0);
    CHECK(S.turn == -1);
    return G.vote;
}

#define ALL_BITS (SM8750_GCC_VOTE_TOUCH | SpeakerGccVoteBits | GAUGE_BITS | PCIE_BITS)

int main(void)
{
    const unsigned all = (1u << Writers) - 1u;
    /* 1. Every writer on the lock word: no vote lost in any schedule, each writer succeeds, the word ends free, no unlocked write. */
    unsigned switches = 0;
    for (uint64_t seed = 1; seed <= 3000; ++seed) {
        const uint32_t initial = seed % 3 == 0 ? (1u << 31) : seed % 3 == 1 ? 0u : (1u << 7);   /* foreign bits stay */
        const uint32_t vote = schedule(seed, all, initial);
        CHECK((vote & (ALL_BITS | initial)) == (ALL_BITS | initial) && (vote & ~(ALL_BITS | initial)) == 0);
        for (int i = 0; i < Writers; ++i) CHECK(rc[i] == 0);
        CHECK(sm8750_gcc_vote_owner == 0 && sm8750_gcc_vote_unlocked == 0 && G.unlocked_writes == 0);
        switches += S.switches;
    }
    CHECK(switches > 3000 * 20);
    /* Pairs and triples too (each subset of writers, many schedules). */
    for (unsigned mask = 3; mask <= all; ++mask) {
        if (!(mask & (mask - 1))) continue;
        uint32_t want = 0;
        const uint32_t bits[Writers] = {SM8750_GCC_VOTE_TOUCH, SpeakerGccVoteBits, GAUGE_BITS, PCIE_BITS};
        for (int i = 0; i < Writers; ++i) if (mask & (1u << i)) want |= bits[i];
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            CHECK(schedule(seed * 7919 + mask, mask, 0) == want);
            for (int i = 0; i < Writers; ++i) CHECK(rc[i] == 0);
            CHECK(sm8750_gcc_vote_owner == 0 && G.unlocked_writes == 0);
        }
    }
    /* 2. The same schedules with the touch and the speaker as they were: votes are lost (the race the lock closes is reachable). */
    legacy = 1;
    unsigned lost = 0, lost_speaker = 0, lost_touch = 0;
    for (uint64_t seed = 1; seed <= 3000; ++seed) {
        const uint32_t vote = schedule(seed, all, 0);
        if ((vote & ALL_BITS) != ALL_BITS) lost++;
        if ((vote & SpeakerGccVoteBits) != SpeakerGccVoteBits) lost_speaker++;
        if ((vote & SM8750_GCC_VOTE_TOUCH) != SM8750_GCC_VOTE_TOUCH) lost_touch++;
        sm8750_gcc_vote_owner = 0;
    }
    CHECK(lost > 30 && lost_speaker > 0 && lost_touch > 0);
    legacy = 0;
    /* 3. A holder that never gives the word back: each writer waits 50 ms once, writes without it (counted) and finishes. */
    for (int w = Touch; w <= Speaker; ++w) {
        sm8750_gcc_vote_owner = 0x44454144u;                     /* 'DEAD' */
        const uint32_t vote = schedule(5, 1u << w, 0);
        CHECK(rc[w] == 0 && vote == (w == Touch ? SM8750_GCC_VOTE_TOUCH : SpeakerGccVoteBits));
        CHECK(G.us >= 50000 && G.us < 50000 + 30000 && G.unlocked_writes == 1 && G.writes == 1);
        CHECK(sm8750_gcc_vote_owner == 0x44454144u && sm8750_gcc_vote_unlocked == (uint32_t)(w - Touch + 1));
    }
    sm8750_gcc_vote_owner = 0;
    sm8750_gcc_vote_unlocked = 0;
    /* 4. Votes already in place: one read, no write, the lock word not taken (a held word is no wait). */
    sm8750_gcc_vote_owner = TAG_BATT;
    CHECK(schedule(9, (1u << Touch) | (1u << Speaker), ALL_BITS) == ALL_BITS && G.writes == 0 && G.us < 100);
    CHECK(rc[Touch] == 0 && rc[Speaker] == 0 && sm8750_gcc_vote_owner == TAG_BATT && sm8750_gcc_vote_unlocked == 0);
    sm8750_gcc_vote_owner = 0;
    /* 5. The helper's own contract: bad arguments touch nothing; a register that drops the write is -5 with the word given back. */
    struct sm8750_gcc_vote_report r;
    me = Touch;
    S.alive[Touch] = 1;
    S.turn = Touch;
    memset(&G, 0, sizeof G);
    CHECK(sm8750_gcc_vote_set(&kGcc, 0, 1, &r) == -22 && sm8750_gcc_vote_set(&kGcc, 1, 0, &r) == -22 && !r.wrote && G.writes == 0);
    CHECK(sm8750_gcc_vote_set(0, 1, 1, &r) == -22);
    CHECK(sm8750_gcc_vote_set(&kGcc, 1u << 20, SM8750_GCC_OWNER_TOUCH, &r) == 0 && r.wrote && r.locked && r.before == 0 &&
          r.after == (1u << 20) && sm8750_gcc_vote_owner == 0);
    S.alive[Touch] = 0;
    S.turn = -1;
    printf("PASS gccvote: %u checks (3000 + 3300 locked schedules lose nothing; the old updates lose votes in %u of 3000; a stuck holder "
           "costs 50 ms, counted)\n", checks, lost);
    return 0;
}
