/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The bring-up sequence and the client queues follow UniXNU-sheng package/AdspProbe.cpp (https://github.com/UniXNU/UniXNU-sheng,
 * commit 08cde20e), moved into portable C so that the host tests run the whole sequence against a simulated DSP. */
/* The OnePlus 13T ADSP bring-up and its run time, as portable C over a table of platform operations (PaganiAdsp.cpp gives the kernel
 * ones, Test/core.c a simulated SoC and DSP):
 *   1 Mapped     SMEM partition table (the only SMEM page read so far); the apps<->ADSP and global partitions located
 *   2 Firmware   modem_a: image/adsp_dtb.mdt + .bNN and image/adsp.mdt + .bNN found and parsed (storage reads only)
 *   3 Surveyed   the two partitions read (items 443/429/478-480, slave-kernel, crash reason 423), AOSS and RSC read
 *   4 Smp2p      item 429 allocated under the TCSR mutex: "master-kernel" and "sleepstate" (awake)
 *   5 Doorbells  IPCC: the LPASS SMP2P and GLINK signals and the interrupt (the AOP receive signal is never enabled)
 *   6 LoadState  AOSS QMP "{class: image, res: load_state, name: adsp, val: on}"
 *   7 Rails      lcx/lmx proxy votes (ARC, vlvl 384 by default) through apps RSC DRV-2
 *   8 Dtb        PAS 0x24: init_image, mem_setup 0x9ed80000, segments, auth_and_reset
 *   9 Loaded     PAS 1: init_image, mem_setup 0x9ee00000, segments
 *  10 Started    PAS 1 auth_and_reset
 *  11 Ready      SMP2P ready (5 s); handover drops the proxy votes whenever it comes
 *  12 Glink      items 478/479, the remote's 480, version agreed
 *  13 Channel    a client channel open
 * pagani-adsp-stop=N stops after stage N; stages 1 to 3 write nothing. A failure records the stage (negative) and its code, undoes what
 * the DSP had been given (PAS shutdown, votes, load_state off) and returns; nothing panics. What may still run keeps its power: a PAS
 * shutdown that failed, or a secure call that ran out of its budget, leaves the votes and load_state where they are (a rail pulled from
 * under a Q6 that did not stop is worse than a vote left on until reboot). Single threaded: everything but the client registry (its own
 * lock) belongs to the DSP thread.
 *
 * No UFS write is on the wire while something is touched for the first time (the ops' quiet, optional): a reset nothing sees coming is a
 * power cut in the middle of a write, and this flash can lose a whole logical unit to that (UfsLog.h). Three stretches, each opened by a line
 * that has time to reach the UFS log ("[adsp] guard smem-table", "smem", "lpass"; crumb_ms), then the writer held and a round under way given
 * PAGANI_ADSP_QUIET_MS to end: the SMEM table; the partitions, the TCSR mutex and the IPCC; AOSS, the rails, PAS and the Q6 until ready. A
 * stretch that hangs keeps the writer held and its line is the last one read back; a failure and a stop let go. */
#ifndef PAGANI_ADSP_CORE_H
#define PAGANI_ADSP_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "AdspFirmware.h"
#include "PaganiAdsp.h"
#include "QcomGlink.h"
#include "QcomQrtr.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#ifdef __cplusplus
extern "C" {
#endif

/* SM8750 / PKX110 facts (the phone's running devicetree, Android 15: remoteproc-adsp@03000000 "qcom,sun-adsp-pas", qcom,smp2p-adsp,
 * glink-edge "lpass"; the reserved-memory nodes; upstream sm8550_adsp_resource for the PAS ids). */
#define PAGANI_ADSP_SMEM_BASE 0x81d00000ull
#define PAGANI_ADSP_SMEM_SIZE 0x200000u
#define PAGANI_ADSP_TCSR_MUTEX 0x1f43000ull     /* tcsr_mutex_block 0x1f40000, hwlock 3, stride 0x1000 */
#define PAGANI_ADSP_IPCC_BASE 0x406000ull       /* ipcc_mproc; its interrupt: SPI 229, level */
#define PAGANI_ADSP_IPCC_SPI 229u
#define PAGANI_ADSP_AOSS_BASE 0xc300000ull      /* aoss_qmp message RAM, 0x400 bytes */
#define PAGANI_ADSP_AOSS_SIZE 0x400u
#define PAGANI_ADSP_RSC_DRV2 0x16520000ull      /* apps_rsc drv-2 */
#define PAGANI_ADSP_RSC_SIZE 0x10000u
#define PAGANI_ADSP_CMD_DB 0x81c60000ull
#define PAGANI_ADSP_CMD_DB_SIZE 0x20000u
#define PAGANI_ADSP_REGION_BASE 0x9ee00000ull   /* adspslpi_region */
#define PAGANI_ADSP_REGION_SIZE 0x5080000ull
#define PAGANI_ADSP_DTB_BASE 0x9ed80000ull      /* q6_adsp_dtb_region */
#define PAGANI_ADSP_DTB_SIZE 0x80000ull
#define PAGANI_ADSP_QUIET_BIT 64u               /* pagani_panel_probe_quiet: 1 panel, 2 USB, 4 Bluetooth, 8 Wi-Fi, 16 camera, 32 speaker, this */
#define PAGANI_ADSP_QUIET_MS 1000u              /* the writer looks every 500 ms at the longest, a round takes a few ms (PanelProbe.h) */
enum {
    PaganiAdspPasAdsp = 1, PaganiAdspPasDtb = 0x24, PaganiAdspRemotePid = 2,
    PaganiAdspItemSmp2pIn = 443, PaganiAdspItemSmp2pOut = 429, PaganiAdspItemCrash = 423,
    PaganiAdspItemGlinkDesc = 478, PaganiAdspItemGlinkTx = 479, PaganiAdspItemGlinkRx = 480,
    PaganiAdspGlinkFifo = 0x4000,
    PaganiAdspIpccClientLpass = 3, PaganiAdspSignalGlink = 0, PaganiAdspSignalSmp2p = 2,
    PaganiAdspBitFatal = 1u << 0, PaganiAdspBitReady = 1u << 1, PaganiAdspBitHandover = 1u << 2,
    PaganiAdspMaxChannels = 6,          /* IPCRTR and five clients (GLINK has 8 slots) */
};
/* Events the wait operation reports. */
enum {
    PaganiAdspEvSmp2p = 1u << 0,        /* IPCC LPASS signal 2 */
    PaganiAdspEvGlink = 1u << 1,        /* IPCC LPASS signal 0 */
    PaganiAdspEvSend = 1u << 2,         /* a client queued a message or registered a channel */
    PaganiAdspEvPoll = 1u << 3,         /* no interrupt: look at everything */
};
/* Error codes of the core's own steps (the operations' codes pass through unchanged). */
enum {
    PaganiAdspErrRunning = -114,        /* the DSP already runs (slave-kernel bits or GLINK items present) */
    PaganiAdspErrTimeout = -60,         /* no ready within the deadline */
    PaganiAdspErrFatal = -5,            /* the DSP reported a fatal error */
    PaganiAdspErrNoMemory = -12,
    PaganiAdspErrLock = -16,            /* the TCSR mutex stayed taken */
    PaganiAdspErrGuard = -13,           /* an SMEM access outside the readable ranges was refused */
    PaganiAdspErrItem = -34,            /* an SMEM item is too small for its structure */
    PaganiAdspErrStopped = -125,        /* stopped by pagani-adsp-stop (not a failure; stage stays positive) */
    PaganiAdspErrScmParked = -1005,     /* a secure call ran out of its budget (PaganiAdspPlatScm - Sm8750ScmTimeout, checked in
                                           PaganiAdsp.cpp): its session stays parked in the secure world, nothing more is asked of it */
};

typedef struct {
    uint32_t aoss_magic, aoss_version;  /* the QMP descriptor: "LIAM" and 1 */
    uint32_t aoss_links;                /* bit 0..3: our link, its ack, our channel, the AOP's channel up (0xffff each) */
    uint32_t rsc_id;                    /* DRV-2 word 0: version */
} PaganiAdspSurvey;

typedef struct PaganiAdspOps {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* Sleeps until an event or `ms` pass; returns PaganiAdspEv* bits (0: the time passed). */
    uint32_t (*wait)(void *context, uint32_t ms);
    void (*wake)(void *context);                         /* a client queued work (any thread) */
    void (*sleep_ms)(void *context, uint32_t ms);
    void (*trace)(void *context, const char *line);      /* one log line (BootProgress ramoops U record) */
    void (*lock)(void *context);                         /* the client registry; never held across a callback */
    void (*unlock)(void *context);
    void *(*alloc)(void *context, size_t bytes);         /* zeroed */
    void (*free)(void *context, void *memory, size_t bytes);
    int (*hwlock)(void *context, int take);              /* TCSR mutex 3: take (1 when taken) or release */
    void (*ring)(void *context, uint32_t signal);        /* IPCC doorbell to LPASS */
    int (*doorbells)(void *context);                     /* interrupt and receive signals; 0, else the core polls */
    void (*survey)(void *context, PaganiAdspSurvey *out);
    int (*qmp)(void *context, const char *message);      /* AOSS QMP, 0 when the AOP took it */
    int (*rails)(void *context, int on);                 /* lcx/lmx proxy votes */
    int (*pas_init)(void *context, uint32_t pas, const uint8_t *metadata, uint64_t bytes);
    int (*pas_mem_setup)(void *context, uint32_t pas, uint64_t base, uint64_t bytes);
    int (*pas_auth)(void *context, uint32_t pas);
    int (*pas_shutdown)(void *context, uint32_t pas);
    void (*pas_release)(void *context, uint32_t pas, int authenticated);
    void (*pas_result)(void *context, int64_t *x0, uint64_t *x1);
    uint8_t *(*map)(void *context, uint64_t base, uint64_t bytes);   /* write-combined carveout mapping */
    void (*unmap)(void *context, uint8_t *va, uint64_t bytes);
    PaganiFwDisk disk;
    /* Holds the UFS log's writer (1) or lets it go (0), by this driver's bit of pagani_panel_probe_quiet. Optional (null: no stretches). */
    void (*quiet)(void *context, unsigned on);
} PaganiAdspOps;

typedef struct {
    uint32_t stop_after;                /* 0: everything; 1..12: stop after that stage */
    uint32_t ready_ms;                  /* 0: 5000 */
    const char *partitions[2];          /* firmware partitions, tried in order */
    unsigned partition_count;
    const char *directory;              /* "image" */
    const uint32_t *retry_ms;           /* pauses while the partition is not found (storage may not be up yet) */
    unsigned retries;
    uint32_t crumb_ms;                  /* how long a line takes to reach the UFS log; 0: no UFS log, so nothing to hold */
} PaganiAdspConfig;

struct PaganiAdspCore;
struct PaganiAdspChannel {
    char name[PAGANI_ADSP_NAME];
    uint32_t intent_size, intent_count, max_message, depth;
    int initiator;
    PaganiAdspClient client;
    void *context;
    struct PaganiAdspCore *core;
    uint8_t *storage;                   /* intent_count buffers of intent_size */
    uint8_t *queue;                     /* depth slots: length word, then max_message bytes */
    uint32_t head, count;               /* registry lock */
    QcomGlinkChannel *glink;            /* DSP thread */
    volatile uint32_t up;               /* last state reported to the client */
    uint32_t sent, deferred, dropped;
};

typedef struct {
    QcomQrtr router;
    PaganiAdspChannel *channel;
    uint32_t link_ups, link_downs, requests, answers, traced_requests, traced_services;
    uint32_t last_services;
    uint8_t hello_traced;
} PaganiAdspQrtr;

typedef struct PaganiAdspCore {
    const PaganiAdspOps *ops;
    PaganiAdspConfig config;
    PaganiAdspStatus status;
    QcomSmem smem;
    uint32_t refused;                   /* SMEM accesses the guard refused */
    QcomSmemPartition part, global;
    uint8_t have_global, irq_ok, booted, glink_started, glink_stopped, dead;   /* dead: the bring-up failed or stopped */
    uint8_t votes_on, load_state_on, dtb_running, adsp_running, handover, fatal;
    uint8_t scm_parked;                 /* a secure call came back PaganiAdspErrScmParked */
    uint8_t held;                       /* the UFS log's writer is held */
    QcomSmp2p p2p;
    int stop_entry, sleep_entry;
    uint32_t slave;
    QcomGlink glink;
    uint64_t auth_ms, glink_check_ms;
    uint32_t unknown_traced, version_traced;
    PaganiFwVolume *volume;
    PaganiFwImage *dtb, *adsp;
    PaganiAdspChannel channel[PaganiAdspMaxChannels];
    uint32_t channel_count;             /* published with a release store */
    PaganiAdspQrtr qrtr;
    char line[256];
} PaganiAdspCore;

/* Sets the core up on the mapped SMEM window and registers the IPC router channel. Nothing is read yet. */
void pagani_adsp_core_init(PaganiAdspCore *c, const PaganiAdspOps *ops, const PaganiAdspConfig *config, volatile uint8_t *smem,
                           uint32_t smem_size);
/* The bring-up. 1: the DSP runs (enter the service loop); 0: stopped as configured or failed (see the status). */
int pagani_adsp_core_boot(PaganiAdspCore *c);
/* One round of the run time for the events seen. */
void pagani_adsp_core_service(PaganiAdspCore *c, uint32_t events);
/* How long the service loop may sleep without an event. */
uint32_t pagani_adsp_core_idle_ms(PaganiAdspCore *c);

PaganiAdspChannel *pagani_adsp_core_channel(PaganiAdspCore *c, const PaganiAdspChannelConfig *config, const PaganiAdspClient *client,
                                            void *context);
int pagani_adsp_core_send(PaganiAdspCore *c, PaganiAdspChannel *channel, const void *data, uint32_t length);
void pagani_adsp_core_status(PaganiAdspCore *c, PaganiAdspStatus *out);

/* The IPC router and its service locator (AdspQrtr.c). */
void pagani_adsp_qrtr_init(PaganiAdspCore *c);
/* The SM8750 protection domain table the locator answers from (exposed for the tests). */
const QcomPdDomain *pagani_adsp_qrtr_domains(unsigned *count);

/* For the IPC router module: one "[adsp] ..." line from the DSP thread. */
void pagani_adsp_core_trace(PaganiAdspCore *c, const char *text);

#ifdef __cplusplus
}
#endif
#endif
