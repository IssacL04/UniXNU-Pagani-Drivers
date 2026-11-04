// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The weak defaults a kernel without PaganiAdsp (or without PaganiSensors) links: PaganiAdsp.h's IPC router client API reports the
// driver as off (no port, PaganiAdspOff, no server), PaganiSensors.h's status -19. PaganiSensors.cpp links the first set; its start
// then logs "PaganiAdsp is off ... services without samples" and runs as pagani-sensors=1.
#define PAGANI_ADSP_WEAK_DEFAULTS 1
#define PAGANI_SENSORS_WEAK_DEFAULTS 1
#include "check.h"
#include "../../PaganiAdsp/PaganiAdsp.h"
#include "../PaganiSensors.h"

static void server(void *c, int up, uint32_t node, uint32_t port, uint32_t instance) {(void)c; (void)up; (void)node; (void)port; (void)instance;}
static void receive(void *c, uint32_t node, uint32_t port, const uint8_t *d, uint32_t n) {(void)c; (void)node; (void)port; (void)d; (void)n;}

int main(void)
{
    const PaganiAdspQrtrClient client = {server, receive};
    const PaganiAdspQrtrConfig config = {0x190, PAGANI_ADSP_QRTR_ANY, 0, 0};
    PaganiAdspQrtrServer s = {1, 2, 3, 4};
    PaganiAdspQrtrPort *port = pagani_adsp_qrtr_open(&config, &client, 0);
    CHECK(!port && pagani_adsp_qrtr_port_number(port) == 0);
    CHECK(pagani_adsp_qrtr_server(port, &s) == 0 && !s.node && !s.port && !s.instance && !s.generation);
    s.node = 9;
    CHECK(pagani_adsp_qrtr_lookup(0x190, PAGANI_ADSP_QRTR_ANY, &s) == 0 && !s.node);
    CHECK(pagani_adsp_qrtr_send(port, 5, 0x1a, "x", 1) == PaganiAdspOff);
    pagani_adsp_qrtr_close(port);
    PaganiAdspStatus st;
    CHECK(pagani_adsp_status(&st) == PaganiAdspOff && pagani_adsp_send(0, "x", 1) == PaganiAdspOff);
    PaganiSensorsStatus ss;
    pagani_sensors_start();
    CHECK(pagani_sensors_status(&ss) == -19);
    printf("PASS weak defaults: PaganiAdsp's router client API and PaganiSensors report off: %u checks\n", checks);
    return 0;
}
