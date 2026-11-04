/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtBridge.c 的线程约定，用真线程和 ThreadSanitizer 跑：一个工作线程，一个客户端线程写，一个客户端线程读（bluetoothd 的 RxLoop），加上反复断开重连。
 * bridge.c 是单线程的，时间可控；这里只证明锁用对了、帧不丢不乱，逻辑判据都在 bridge.c。 */
#include <pthread.h>
#include <time.h>
#include "../BtBridge.h"
#include "../BtStandIn.h"
#include "../BtTlv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* check.h 的计数器不带锁，这里不计数。 */
#define VERIFY(x) do {if (!(x)) {fprintf(stderr, "%s:%d: VERIFY failed: %s\n", __FILE__, __LINE__, #x); exit(1);}} while (0)

static const uint8_t kAddress[6] = {1, 2, 3, 4, 5, 6};
enum {kRounds = 40, kAclPerRound = 400, kPayload = 300, kSockBytes = 4096};

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gKick = PTHREAD_COND_INITIALIZER, gRoom = PTHREAD_COND_INITIALIZER;
static int gKicked, gQuit;
static PbBridge *b;

/* 控制器：工作线程独占，ACL 回环，命令答替身的表 */
static uint8_t gCtrl[1u << 20];
static uint32_t gCtrlHead, gCtrlTail;

/* 套接字：client_put 在桥的锁里写，读线程读，有自己的锁，和真的套接字锁一样在里层 */
static pthread_mutex_t gSockLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gSockCond = PTHREAD_COND_INITIALIZER;
static uint8_t gSock[kSockBytes];
static uint32_t gSockHead, gSockTail;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void io_lock(void *c, int take)
{
    (void)c;
    if (take) pthread_mutex_lock(&gLock);
    else pthread_mutex_unlock(&gLock);
}
static uint64_t io_now(void *c) {(void)c; return now_ms();}
static void ctrl_put(const uint8_t *data, uint32_t bytes)
{
    VERIFY(gCtrlTail - gCtrlHead + bytes <= sizeof gCtrl);
    for (uint32_t i = 0; i < bytes; ++i) gCtrl[gCtrlTail++ % sizeof gCtrl] = data[i];
}
static int io_write(void *c, const uint8_t *data, uint32_t bytes, unsigned timeout_ms)
{
    (void)c;
    (void)timeout_ms;
    if (data[0] == PbH4Cmd) {
        uint8_t reply[PbStandInMax];
        ctrl_put(reply, pb_standin_reply(data, bytes, kAddress, reply));
    } else if (data[0] == PbH4Acl) {
        ctrl_put(data, bytes);
    }
    return 0;
}
static uint32_t io_read(void *c, uint8_t *data, uint32_t max)
{
    (void)c;
    uint32_t n = 0;
    while (n < max && gCtrlHead != gCtrlTail) data[n++] = gCtrl[gCtrlHead++ % sizeof gCtrl];
    return n;
}
static int io_put(void *c, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    (void)c;
    (void)unit;
    pthread_mutex_lock(&gSockLock);
    int rc = 1;
    if (kSockBytes - (gSockTail - gSockHead) >= bytes) {
        for (uint32_t i = 0; i < bytes; ++i) gSock[gSockTail++ % kSockBytes] = data[i];
        pthread_cond_signal(&gSockCond);
        rc = 0;
    }
    pthread_mutex_unlock(&gSockLock);
    return rc;
}
static void io_kick(void *c) {(void)c; gKicked = 1; pthread_cond_signal(&gKick);}
static void timed(pthread_cond_t *cond, unsigned ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)ms * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(cond, &gLock, &t);
}
static void io_wait(void *c, unsigned ms) {(void)c; timed(&gRoom, ms);}
static void io_wake(void *c) {(void)c; pthread_cond_broadcast(&gRoom);}
static void io_log(void *c, const char *line) {(void)c; (void)line;}
static const PbBridgeIo kIo = {NULL, io_lock, io_now, io_write, io_read, io_put, io_kick, io_wait, io_wake, io_log};

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        const int ms = pb_bridge_step(b);
        VERIFY(ms >= 0);
        pthread_mutex_lock(&gLock);
        if (gQuit) {
            pthread_mutex_unlock(&gLock);
            return NULL;
        }
        if (ms && !gKicked) timed(&gKick, (unsigned)ms);
        gKicked = 0;
        pthread_mutex_unlock(&gLock);
    }
}

/* 读线程按 H4 拆帧，查 ACL 的序号。 */
static uint32_t gAclSeen, gEvents;
static uint32_t gReaderQuit;
static uint8_t gParse[4096];
static uint32_t gParsed;
static void parse(uint32_t round)
{
    for (;;) {
        if (!gParsed) return;
        VERIFY(gParse[0] == PbH4Acl || gParse[0] == PbH4Evt);
        uint32_t total;
        if (gParse[0] == PbH4Evt) {
            if (gParsed < 3) return;
            total = 3u + gParse[2];
        } else {
            if (gParsed < 5) return;
            total = 5u + (uint32_t)(gParse[3] | (uint32_t)gParse[4] << 8);
        }
        if (gParsed < total) return;
        if (gParse[0] == PbH4Evt) {
            __atomic_fetch_add(&gEvents, 1u, __ATOMIC_RELAXED);
        } else {
            VERIFY(total == 5u + kPayload);
            VERIFY(gParse[5] == (uint8_t)__atomic_load_n(&gAclSeen, __ATOMIC_RELAXED) && gParse[6] == (uint8_t)round);
            for (uint32_t i = 7; i < total; ++i) VERIFY(gParse[i] == 0x5a);
            __atomic_fetch_add(&gAclSeen, 1u, __ATOMIC_RELAXED);
        }
        memmove(gParse, gParse + total, gParsed - total);
        gParsed -= total;
    }
}
static uint32_t gRound;
static void *reader(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&gSockLock);
        while (gSockHead == gSockTail && !__atomic_load_n(&gReaderQuit, __ATOMIC_RELAXED)) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 5000000L;
            t.tv_sec += t.tv_nsec / 1000000000L;
            t.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&gSockCond, &gSockLock, &t);
        }
        if (gSockHead == gSockTail) {
            pthread_mutex_unlock(&gSockLock);
            return NULL;
        }
        uint32_t n = 0;
        while (gSockHead != gSockTail && gParsed + n < sizeof gParse) gParse[gParsed + n++] = gSock[gSockHead++ % kSockBytes];
        pthread_mutex_unlock(&gSockLock);
        gParsed += n;
        parse(__atomic_load_n(&gRound, __ATOMIC_RELAXED));
    }
}

static void wait_for(uint32_t *counter, uint32_t want)
{
    const uint64_t until = now_ms() + 20000;
    while (__atomic_load_n(counter, __ATOMIC_RELAXED) < want) {
        VERIFY(now_ms() < until);
        struct timespec t = {0, 100000};
        nanosleep(&t, NULL);
    }
}

int main(void)
{
    b = calloc(1, sizeof *b);
    VERIFY(b != NULL);
    pb_bridge_init(b, &kIo, kAddress, 0);
    pthread_t work, read_thread;
    pb_bridge_up(b, NULL, NULL, 0);
    VERIFY(pthread_create(&work, NULL, worker, NULL) == 0);
    VERIFY(pthread_create(&read_thread, NULL, reader, NULL) == 0);
    uint32_t sent_events = 0;
    for (uint32_t round = 0; round < kRounds; ++round) {
        __atomic_store_n(&gRound, round, __ATOMIC_RELAXED);
        __atomic_store_n(&gAclSeen, 0, __ATOMIC_RELAXED);
        VERIFY(pb_bridge_connect(b, round + 1, 100) == PbBridgeOk);
        uint8_t p[PbPacketMax];
        pb_bridge_write(b, round + 1, p, pb_hci_command(p, PbOpErroneousData, (const uint8_t *)"\x01", 1));   /* 走完初始化 */
        pb_bridge_write(b, round + 1, p, pb_hci_command(p, 0xfc4d, NULL, 0));                                /* 当场回 */
        sent_events += 2;
        for (uint32_t i = 0; i < kAclPerRound; ++i) {
            uint8_t acl[5 + kPayload];
            memset(acl, 0x5a, sizeof acl);
            acl[0] = PbH4Acl;
            acl[1] = 0x01;
            acl[2] = 0x20;
            acl[3] = (uint8_t)kPayload;
            acl[4] = (uint8_t)(kPayload >> 8);
            acl[5] = (uint8_t)i;
            acl[6] = (uint8_t)round;
            pb_bridge_write(b, round + 1, acl, sizeof acl);
        }
        wait_for(&gAclSeen, kAclPerRound);
        wait_for(&gEvents, sent_events);
        pb_bridge_disconnect(b, round + 1);
    }
    pthread_mutex_lock(&gLock);
    gQuit = 1;
    pthread_cond_signal(&gKick);
    pthread_mutex_unlock(&gLock);
    VERIFY(pthread_join(work, NULL) == 0);
    __atomic_store_n(&gReaderQuit, 1, __ATOMIC_RELAXED);
    VERIFY(pthread_join(read_thread, NULL) == 0);
    VERIFY(!b->guard && !b->tx_dropped && !b->up_full && !b->put_lost);
    printf("PASS bridge-threads: %u rounds, %u ACL frames each way\n", kRounds, kRounds * kAclPerRound);
    return 0;
}
