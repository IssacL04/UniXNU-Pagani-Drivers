/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The client model follows UniXNU-sheng package/ShengAdsp.h (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). */
/* PaganiAdsp: the OnePlus 13T audio DSP (ADSP, LPASS) for kernel clients. With boot argument pagani-adsp=1 a kernel thread loads the
 * DSP's own firmware from the modem_a partition, starts it through the secure world and brings up GLINK and the IPC router; a client
 * then talks to a DSP service over a GLINK channel:
 *   - the battery manager (PMIC GLINK): "PMIC_RTR_ADSP_APPS" (Android: no intents property, so 5 x 1 KiB);
 *   - the audio packet router (GPR, AudioReach APM/PRM): "adsp_apps" (Android: 20 intents of 512 bytes);
 *   - the IPC router "IPCRTR" is PaganiAdsp's own (service locator), not for clients.
 *   - a QMI service of the DSP on that router (e.g. the sensor client service 400 of the sensor PD): a router port, through the IPC
 *     router client API at the end of this header.
 *
 * Model. A client registers a channel by name at any time after boot (before or after the DSP is up or opened it); the DSP opens it, and the client
 * gets the channel state and every message through its callbacks. Callbacks run on the DSP thread: they must not block (copy what they
 * need, wake their own thread). pagani_adsp_send never waits: it queues a copy and the DSP thread sends it once the DSP offers a
 * receive buffer (intent) large enough. A message larger than every intent the DSP offered makes the thread ask the DSP for one.
 *
 * Linking. The kernel is linked statically and refuses an undefined weak reference. A client that must also build without PaganiAdsp
 * defines PAGANI_ADSP_WEAK_DEFAULTS in exactly one of its C or C++ files before including this header: that file then carries weak
 * definitions which report the driver as absent (null channel, PaganiAdspOff), and PaganiAdsp's own strong definitions replace them
 * whenever it is linked.
 *
 * Example (battery):
 *     static const PaganiAdspClient client = {batteryState, batteryReceive};
 *     channel = pagani_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 5, 0, &client, self);
 *     ...on the client's own thread: pagani_adsp_send(channel, request, length) == PaganiAdspOk */
#ifndef PAGANI_ADSP_H
#define PAGANI_ADSP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PAGANI_ADSP_MAX_MESSAGE 512u          /* default largest message a channel sends */
#define PAGANI_ADSP_MAX_MESSAGE_LIMIT 4096u   /* the most a channel may ask for (the DSP grants a 4 KiB intent on request) */
#define PAGANI_ADSP_DEFAULT_QUEUE 8u
#define PAGANI_ADSP_MAX_QUEUE 32u
#define PAGANI_ADSP_MAX_INTENTS 24u           /* receive buffers per channel */
#define PAGANI_ADSP_NAME 32u                  /* channel names, terminator included */

/* Results of pagani_adsp_send. */
enum {
    PaganiAdspOk = 0,
    PaganiAdspFull = -1,       /* the channel's queue is full: try again later */
    PaganiAdspArgument = -2,   /* null channel or data, zero length */
    PaganiAdspTooBig = -3,     /* longer than the channel's max_message */
    PaganiAdspDown = -4,       /* the DSP crashed or its link broke: nothing is sent any more this boot */
    PaganiAdspOff = -19,       /* PaganiAdsp is not linked, or pagani-adsp=1 is not set */
};

typedef struct PaganiAdspChannel PaganiAdspChannel;

typedef struct {
    /* up: 1 once the DSP opened the channel (sends go out from then on), 0 when it closed it or the DSP stopped. */
    void (*state)(void *context, int up);
    /* One complete message; data is valid during the call only. */
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} PaganiAdspClient;

typedef struct {
    const char *name;             /* GLINK channel name (at most 31 characters) */
    uint32_t intent_size;         /* receive buffers offered to the DSP: intent_count of intent_size bytes */
    uint32_t intent_count;        /* 1 .. PAGANI_ADSP_MAX_INTENTS */
    int initiator;                /* 1: we send OPEN first; 0 (the DSP's channels): we answer the DSP's OPEN */
    uint32_t max_message;         /* 0: PAGANI_ADSP_MAX_MESSAGE; at most PAGANI_ADSP_MAX_MESSAGE_LIMIT */
    uint32_t queue_depth;         /* 0: PAGANI_ADSP_DEFAULT_QUEUE; at most PAGANI_ADSP_MAX_QUEUE */
} PaganiAdspChannelConfig;

/* Registers a channel; null when the driver is off, out of channel slots or memory, or on a bad argument. A name may be registered once. */
PaganiAdspChannel *pagani_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                       const PaganiAdspClient *client, void *context);
PaganiAdspChannel *pagani_adsp_channel_config(const PaganiAdspChannelConfig *config, const PaganiAdspClient *client, void *context);
/* Queues one message (copied). PaganiAdspOk or one of the results above. Any thread except inside a callback holding a client lock the
 * callback also takes. */
int pagani_adsp_send(PaganiAdspChannel *channel, const void *data, uint32_t length);
/* 1 while the channel is open. */
int pagani_adsp_channel_up(const PaganiAdspChannel *channel);

/* Bring-up stages; PaganiAdspStatus.stage is the last one reached, or minus the one that failed. */
enum {
    PaganiAdspStOff = 0,
    PaganiAdspStMapped = 1,      /* windows mapped, SMEM partition table read */
    PaganiAdspStFirmware = 2,    /* adsp_dtb and adsp found and parsed on modem_a (storage reads only) */
    PaganiAdspStSurveyed = 3,    /* apps-ADSP and global SMEM partitions read; the DSP is not running */
    PaganiAdspStSmp2p = 4,       /* our SMP2P item 429 written */
    PaganiAdspStDoorbells = 5,   /* IPCC LPASS signals 0 and 2 enabled, interrupt attached */
    PaganiAdspStLoadState = 6,   /* AOSS "load_state adsp on" taken */
    PaganiAdspStRails = 7,       /* lcx/lmx proxy votes in place */
    PaganiAdspStDtb = 8,         /* adsp_dtb (PAS 0x24) authenticated and out of reset */
    PaganiAdspStLoaded = 9,      /* adsp (PAS 1) metadata accepted, segments written */
    PaganiAdspStStarted = 10,    /* adsp authenticated and out of reset */
    PaganiAdspStReady = 11,      /* SMP2P ready */
    PaganiAdspStGlink = 12,      /* GLINK version agreed */
    PaganiAdspStChannel = 13,    /* at least one channel open */
};

typedef struct {
    int32_t stage;               /* PaganiAdspSt*, negative: failed at that stage */
    int32_t error;               /* the failing step's code (see README "error codes") */
    int32_t slave;               /* the DSP's slave-kernel bits (fatal 0, ready 1, handover 2), -1 before the DSP made its item */
    uint32_t ready_ms;           /* from the authentication of adsp to ready */
    uint32_t firmware_kib;       /* adsp + adsp_dtb bytes loaded from files */
    uint32_t ipcc_interrupts;
    uint32_t glink_in, glink_out;
    uint32_t channels_up;        /* bit per registered channel, in registration order (bit 0: IPCRTR) */
    uint32_t qrtr_node;          /* the DSP's IPC router node, 0 before its hello */
    uint32_t qrtr_services;      /* services the DSP announced */
    uint32_t locator_requests, locator_answers;
    uint32_t crashed;            /* 1 after a fatal error report of a running DSP */
    char note[96];               /* crash reason from SMEM item 423, or the last unusual event */
} PaganiAdspStatus;

/* A copy of the current status; PaganiAdspOk, or PaganiAdspOff when the driver is not running. */
int pagani_adsp_status(PaganiAdspStatus *out);

/* ---- IPC router clients: QRTR ports of our node (1) on the DSP's router link (AdspQrtr.c) ----
 *
 * A kernel client (a QMI client of a DSP service, such as the sensor client service 400 in the sensor PD) opens a port, follows the
 * remote server of one service, and exchanges data messages (QMI requests, responses and indications) with it:
 *     static const PaganiAdspQrtrClient client = {sscServer, sscReceive};
 *     const PaganiAdspQrtrConfig config = {0x190, PAGANI_ADSP_QRTR_ANY, 0, 0};
 *     port = pagani_adsp_qrtr_open(&config, &client, self);
 *     ...on the client's own thread, once pagani_adsp_qrtr_server(port, &server) says the server is there:
 *     pagani_adsp_qrtr_send(port, server.node, server.port, request, length) == PaganiAdspOk
 * Ports are numbered from 0x4000 (QRTR's ephemeral range) and are not announced; a port with an announce_service is also our server of
 * that service, announced to the DSP (again after each hello of the DSP). Callbacks run on the DSP thread, without any PaganiAdsp lock
 * held: they must not block (copy what they need, wake their own thread). The server callback is an edge: read the level with
 * pagani_adsp_qrtr_server (a server announced before the open is never reported by a callback), and compare its generation, which
 * moves on every change (a service that restarted on the same node and port still has a new generation). Data larger than
 * PAGANI_ADSP_QRTR_MAX_DATA (the IPCRTR channel's 512-byte messages) is refused with PaganiAdspTooBig. Up to PAGANI_ADSP_QRTR_PORTS
 * ports are open at a time; closing one sends DEL_CLIENT (the server drops what the port asked for) and a callback already running
 * may still finish after the close returns, so a context lives as long as the driver (kernel clients live until reboot). */
#define PAGANI_ADSP_QRTR_ANY 0xffffffffu      /* any instance */
#define PAGANI_ADSP_QRTR_PORTS 12u            /* ports open at a time */
#define PAGANI_ADSP_QRTR_MAX_DATA 480u        /* one data message: the IPCRTR channel's 512 bytes less the 32-byte QRTR header */

typedef struct PaganiAdspQrtrPort PaganiAdspQrtrPort;

typedef struct {
    /* The server this port follows appeared (up 1) or went away (up 0: DEL_SERVER, BYE, the link or the DSP down). */
    void (*server)(void *context, int up, uint32_t node, uint32_t port, uint32_t instance);
    /* One data message for this port from node:port; data is valid during the call only. */
    void (*receive)(void *context, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
} PaganiAdspQrtrClient;

typedef struct {
    uint32_t service;             /* the DSP's service this port follows (0: none) */
    uint32_t instance;            /* its instance as announced (version | instance << 8), or PAGANI_ADSP_QRTR_ANY */
    uint32_t announce_service;    /* nonzero: this port is our server of this service, announced to the DSP */
    uint32_t announce_instance;   /* as announced (version | instance << 8) */
} PaganiAdspQrtrConfig;

typedef struct {
    uint32_t node, port, instance;   /* the server, valid when pagani_adsp_qrtr_server returned 1 */
    uint32_t generation;             /* moves on every change of the port's server (also to none) */
} PaganiAdspQrtrServer;

/* Opens a port (any thread); null when PaganiAdsp is off or not linked, every port is taken, or on a bad argument. */
PaganiAdspQrtrPort *pagani_adsp_qrtr_open(const PaganiAdspQrtrConfig *config, const PaganiAdspQrtrClient *client, void *context);
/* Our port number (0 for null). */
uint32_t pagani_adsp_qrtr_port_number(const PaganiAdspQrtrPort *port);
/* The server the port follows: 1 and its address while it is announced, else 0 (the generation is filled either way). */
int pagani_adsp_qrtr_server(const PaganiAdspQrtrPort *port, PaganiAdspQrtrServer *out);
/* Any announced server of service (instance PAGANI_ADSP_QRTR_ANY: any), without a port: 1 and its address, else 0. */
int pagani_adsp_qrtr_lookup(uint32_t service, uint32_t instance, PaganiAdspQrtrServer *out);
/* Queues one data message from the port to node:port (copied, never waits; any thread, also inside the port's callbacks).
 * PaganiAdspOk or one of the results of pagani_adsp_send. */
int pagani_adsp_qrtr_send(PaganiAdspQrtrPort *port, uint32_t node, uint32_t to_port, const void *data, uint32_t length);
/* Closes the port: DEL_CLIENT (and DEL_SERVER for an announced server) to the DSP; the handle must not be used afterwards. */
void pagani_adsp_qrtr_close(PaganiAdspQrtrPort *port);

#ifdef PAGANI_ADSP_WEAK_DEFAULTS
__attribute__((weak)) PaganiAdspChannel *pagani_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count,
                                                             int initiator, const PaganiAdspClient *client, void *context)
{
    (void)name; (void)intent_size; (void)intent_count; (void)initiator; (void)client; (void)context;
    return 0;
}
__attribute__((weak)) PaganiAdspChannel *pagani_adsp_channel_config(const PaganiAdspChannelConfig *config,
                                                                    const PaganiAdspClient *client, void *context)
{
    (void)config; (void)client; (void)context;
    return 0;
}
__attribute__((weak)) int pagani_adsp_send(PaganiAdspChannel *channel, const void *data, uint32_t length)
{
    (void)channel; (void)data; (void)length;
    return PaganiAdspOff;
}
__attribute__((weak)) int pagani_adsp_channel_up(const PaganiAdspChannel *channel)
{
    (void)channel;
    return 0;
}
__attribute__((weak)) int pagani_adsp_status(PaganiAdspStatus *out)
{
    (void)out;
    return PaganiAdspOff;
}
__attribute__((weak)) PaganiAdspQrtrPort *pagani_adsp_qrtr_open(const PaganiAdspQrtrConfig *config, const PaganiAdspQrtrClient *client,
                                                               void *context)
{
    (void)config; (void)client; (void)context;
    return 0;
}
__attribute__((weak)) uint32_t pagani_adsp_qrtr_port_number(const PaganiAdspQrtrPort *port)
{
    (void)port;
    return 0;
}
__attribute__((weak)) int pagani_adsp_qrtr_server(const PaganiAdspQrtrPort *port, PaganiAdspQrtrServer *out)
{
    (void)port;
    if (out) {out->node = out->port = out->instance = out->generation = 0;}
    return 0;
}
__attribute__((weak)) int pagani_adsp_qrtr_lookup(uint32_t service, uint32_t instance, PaganiAdspQrtrServer *out)
{
    (void)service; (void)instance;
    if (out) {out->node = out->port = out->instance = out->generation = 0;}
    return 0;
}
__attribute__((weak)) int pagani_adsp_qrtr_send(PaganiAdspQrtrPort *port, uint32_t node, uint32_t to_port, const void *data,
                                                uint32_t length)
{
    (void)port; (void)node; (void)to_port; (void)data; (void)length;
    return PaganiAdspOff;
}
__attribute__((weak)) void pagani_adsp_qrtr_close(PaganiAdspQrtrPort *port)
{
    (void)port;
}
#endif

#ifdef __cplusplus
}
#endif
#endif
