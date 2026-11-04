// SPDX-License-Identifier: BSD-2-Clause-Patent
// StallProbe.h with threads, for ThreadSanitizer: eight writers, each the only one of its CPU's dispatch counters and on-core fields as in
// thread_dispatch, user and kernel threads, send and take IPIs to each other, while the observer turns the second and the sampler reads what is on core. Every count must add
// up (a reader may see a second late, never lose or invent one), an on-core read never mixes two threads, and every kbt note under a lock survives.
//   cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread stall-threads.c -o stall-threads && ./stall-threads
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../StallProbe.h"

enum { HZ = 19200000, EVENTS = 200000 };
static struct stall_state s;
static struct stall_kbt open_pairs;
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t writers_done;

// A thread's fields are all derived from its number, so a mix shows.
static void *writer(void *argument)
{
    const unsigned cpu = (unsigned)(uintptr_t)argument;
    char name[STALL_NAME + 1];
    for (unsigned i = 0; i < EVENTS; i++) {
        const unsigned who = (cpu * 16 + i % 16) + 1;
        snprintf(name, sizeof(name), "p%u", who);
        struct boot_progress_stall_run r;
        r.stamp = who * 1000ULL;
        r.latency = (i % 4 == 0) ? HZ / 50 : 100;
        r.tid = who;
        r.name = name;
        r.pid = (int32_t)who;
        r.pri = i % 2 ? 47 : 31;
        r.cpu = cpu;
        r.user = 1;
        if (i % 8 == 3) {                       // a kernel thread: the band of its own, no name
            r.pid = 0;
            r.name = NULL;
            r.user = 0;
        }
        stall_note_run(&s, &r);
        stall_note_idle(&s, cpu);
        stall_note_ipi_send(&s, (cpu + 1) % STALL_CPUS, i + 1);
        stall_note_ipi_recv(&s, cpu, i + 2);
        if (!(i & 0xff)) {
            pthread_mutex_lock(&open_lock);
            stall_kbt_note(&open_pairs, (int32_t)cpu + 1, STALL_KBT_IN, (int)((i >> 8) & 1), i, HZ);
            pthread_mutex_unlock(&open_lock);
        }
    }
    __atomic_fetch_add(&writers_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *observer(void *argument)
{
    static struct stall_seen seen;
    static struct stall_second second;
    uint64_t *total = argument;
    for (;;) {
        const int last = __atomic_load_n(&writers_done, __ATOMIC_ACQUIRE) == STALL_CPUS;
        stall_second_take(&s, &seen, &second);
        for (unsigned b = 0; b < STALL_BANDS; b++) {
            total[0] += second.band[b].n;
            total[1] += second.band[b].over10;
            assert(second.band[b].over10 <= second.band[b].n + 2 * EVENTS);
        }
        total[5] += second.band[STALL_KERNEL].n;
        for (unsigned c = 0; c < STALL_CPUS; c++) {
            total[2] += second.idle[c];
            total[3] += second.sent[c];
            total[4] += second.recv[c];
        }
        if (last) return NULL;
    }
}

static void *sampler(void *argument)
{
    (void)argument;
    struct stall_oncore on;
    struct stall_kbt_due due;
    unsigned good = 0;
    while (__atomic_load_n(&writers_done, __ATOMIC_ACQUIRE) != STALL_CPUS) {
        for (unsigned c = 0; c < STALL_CPUS; c++) {
            if (!stall_oncore_read(&s.cpu[c], &on) || !on.pid) continue;
            assert(on.tid == (uint64_t)on.pid && on.since == (uint64_t)on.pid * 1000);
            char name[STALL_NAME + 1];
            snprintf(name, sizeof(name), "p%d", on.pid);
            assert(!on.name[0] || !strcmp(on.name, name));
            good++;
        }
        pthread_mutex_lock(&open_lock);
        (void)stall_kbt_take(&open_pairs, 1000000, HZ, &due);
        pthread_mutex_unlock(&open_lock);
    }
    printf("  %u on-core reads\n", good);
    return NULL;
}

// A late handler: the asker gives up on most questions, the handler answers whenever it gets there. A word only ever belongs to one question: the
// answer always carries the thread that was asked for at that moment.
static struct stall_slot slot;
static uint32_t asking_done;

static void *handler(void *argument)
{
    (void)argument;
    unsigned answered = 0;
    while (!__atomic_load_n(&asking_done, __ATOMIC_ACQUIRE) || __atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == STALL_SLOT_ASKED) {
        if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != STALL_SLOT_ASKED) continue;
        const uint64_t want = (uint64_t)(uintptr_t)stall_slot_wanted(&slot);
        slot.regs.pc = want;
        slot.regs.lr = ~want;
        stall_slot_answer(&slot, 1);
        answered++;
    }
    printf("  %u answers\n", answered);
    return NULL;
}

static void test_late_answers(void)
{
    pthread_t answering;
    pthread_create(&answering, NULL, handler, NULL);
    unsigned checked = 0;
    for (uintptr_t i = 1; i <= 200000; i++) {
        if (!stall_slot_ask(&slot, (void *)i)) continue;
        for (unsigned spin = 0; spin < 4 && !stall_slot_answered(&slot); spin++) {}
        if (!stall_slot_answered(&slot)) continue;      // "to": this one is left, the next ask finds out
        assert(stall_slot_found(&slot) == 1 && slot.regs.pc == i && slot.regs.lr == ~(uint64_t)i);
        checked++;
    }
    __atomic_store_n(&asking_done, 1, __ATOMIC_RELEASE);
    pthread_join(answering, NULL);
    printf("  %u answers checked\n", checked);
}

int main(void)
{
    static uint64_t total[6];
    test_late_answers();
    pthread_t threads[STALL_CPUS], watcher, reader;
    stall_state_init(&s, HZ);
    pthread_create(&watcher, NULL, observer, total);
    pthread_create(&reader, NULL, sampler, NULL);
    for (uintptr_t c = 0; c < STALL_CPUS; c++) pthread_create(&threads[c], NULL, writer, (void *)c);
    for (unsigned c = 0; c < STALL_CPUS; c++) pthread_join(threads[c], NULL);
    pthread_join(watcher, NULL);
    pthread_join(reader, NULL);
    const uint64_t all = (uint64_t)STALL_CPUS * EVENTS;
    assert(total[0] == all && total[1] == all / 4 && total[2] == all && total[3] == all && total[4] == all && total[5] == all / 8);
    printf("stall threads: %llu events, every count adds up\n", (unsigned long long)all);
    return 0;
}
