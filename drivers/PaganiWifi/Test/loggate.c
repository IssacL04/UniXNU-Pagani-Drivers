/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host test of the filter between the stack's log and the boot log (WifiUtil.h PwLogGate, pagani-wifi-log). */
#include <stdio.h>
#include <stdlib.h>
#include "../WifiUtil.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

int main(void)
{
    PwLogGate g;
    /* the default, level 6: what pLog did before the switch existed */
    pw_log_gate_init(&g, 6, 1000);
    CHECK(pw_log_gate(&g, 3, 1000) && pw_log_gate(&g, 6, 1000));
    CHECK(!pw_log_gate(&g, 7, 1000) && g.held_back == 0 && g.debug_traced == 0);
    unsigned sent = 2;
    while (pw_log_gate(&g, 6, 1000)) ++sent;
    CHECK(sent == PW_LOG_LINES && g.traced == PW_LOG_LINES);
    CHECK(!pw_log_gate(&g, 4, 1000));           /* after the budget only errors */
    CHECK(pw_log_gate(&g, 3, 1000) && pw_log_gate(&g, 1, 1000) && pw_log_gate(&g, 3, 5000000));
    CHECK(!pw_log_gate(&g, 7, 5000000));

    /* a lower level: only what asks for it */
    pw_log_gate_init(&g, 3, 0);
    CHECK(pw_log_gate(&g, 3, 0) && !pw_log_gate(&g, 4, 0) && !pw_log_gate(&g, 6, 0) && !pw_log_gate(&g, 7, 0));

    /* level 7: a QMI session's burst goes through, then the bucket's rate, and a held line is counted */
    pw_log_gate_init(&g, 7, 0);
    unsigned burst = 0;
    while (pw_log_gate(&g, 7, 0)) ++burst;
    CHECK(burst == PW_LOG_DEBUG_BURST && g.held_back == 1 && g.traced == 0);
    CHECK(pw_log_gate(&g, 6, 0) && pw_log_gate(&g, 3, 0));   /* debug lines do not hold back the others */
    CHECK(!pw_log_gate(&g, 7, PW_LOG_DEBUG_MS - 1));   /* less than a line's time earns nothing */
    CHECK(pw_log_gate(&g, 7, PW_LOG_DEBUG_MS) && !pw_log_gate(&g, 7, PW_LOG_DEBUG_MS));
    /* ten seconds earn their lines, nothing lost or gained to rounding over a long run */
    unsigned long long now = PW_LOG_DEBUG_MS;
    unsigned got = 0;
    for (unsigned ms = 0; ms < 10000; ++ms) {
        ++now;
        if (pw_log_gate(&g, 7, now)) ++got;
    }
    CHECK(got == 10000 / PW_LOG_DEBUG_MS);
    /* a quiet minute fills the bucket and no more */
    now += 60000;
    burst = 0;
    while (pw_log_gate(&g, 7, now)) ++burst;
    CHECK(burst == PW_LOG_DEBUG_BURST);
    /* the total */
    pw_log_gate_init(&g, 7, 0);
    unsigned total = 0;
    for (unsigned long long t = 0; t < 3600000; t += 5) if (pw_log_gate(&g, 7, t)) ++total;
    CHECK(total == PW_LOG_DEBUG_LINES && g.debug_traced == PW_LOG_DEBUG_LINES);
    printf("PASS log gate: %u checks\n", checks);
    return 0;
}
