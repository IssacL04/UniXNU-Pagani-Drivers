/* SPDX-License-Identifier: MIT */
#ifndef SM8750_RPMH_H
#define SM8750_RPMH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host side of RPMh for the SM8750 (A830) GPU driver, ARCHITECTURE 4.1.1 and 4.3:
 *  - a snapshot of the AOP command DB (DT reserved memory cmd-db@81c60000, 0x20000 bytes),
 *    taken with aligned 32-bit reads, and name lookups of ARC (slave 3), VRM (slave 4) and
 *    BCM (slave 5) entries with their aux data;
 *  - ARC level selection and BCM vote encoding;
 *  - N commands through an idle ACTIVE TCS of the apps RSC DRV-2 (0x16520000), the protocol
 *    of SOC kits/Sm8750Platform/Rpmh.h sm8750_rpmh_vrm_enable() as run on PKX110, generalized
 *    from three VRM commands to 1..16 commands of any slave type;
 *  - the DRV-2 lock shared with the touch rail votes;
 *  - the votes this driver holds itself on apps DRV-2 (mx/cx, CN0, DDR), with HLOS semantics,
 *    never revoked (4.2.3 D5, 4.3.3).
 * Two BCM encoders exist on purpose and must not be mixed (4.3.2): the GMU bandwidth table
 * (HFI M2, voted by the GMU through the GPU RSC; Linux a8xx: valid 0 at 0 kBps, ACV BIT(2)) and
 * the host votes (Linux HLOS interconnect: SH0/MC0/CN0 keepalive, never valid 0; ACV BIT(0)).
 * Formats follow Linux drivers/soc/qcom/cmd-db.c, rpmh-rsc.c, include/soc/qcom/tcs.h,
 * interconnect/qcom/bcm-voter.c, sm8750.c and KGSL adreno_rpmh.c. No allocation and no logging;
 * hardware is reached only through the callbacks. Only the ACTIVE set is ever written;
 * sleep/wake TCSes are never touched. */

/* Same shape and meaning as struct peripheral_io of SOC kits/Sm8750Platform/PeripheralIO.h:
 * 32-bit access at a byte offset inside one mapped window (Device-nGnRE), and a delay. A
 * distinct name keeps this header includable next to the kit headers. */
typedef struct Sm8750RpmhIo {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void (*delay)(void *context, unsigned microseconds);
} Sm8750RpmhIo;

/* Return codes: 0 or a negative errno value, the same numbers the SoC kit uses. */
enum {
    Sm8750RpmhOk = 0,
    Sm8750RpmhIoError = -5,         /* a TCS register did not read back what was written */
    Sm8750RpmhBusy = -16,           /* no idle ACTIVE TCS, or the DRV-2 lock was not free within 2 s */
    Sm8750RpmhNotFound = -19,       /* bad cmd-db magic, or no entry of that name */
    Sm8750RpmhArgument = -22,
    Sm8750RpmhRange = -34,          /* requested level above every level of the ARC */
    Sm8750RpmhNoData = -61,         /* dummy ARC: no level above the first one */
    Sm8750RpmhCorrupt = -71,        /* malformed cmd-db, wrong slave type, duplicate name, bad aux */
    Sm8750RpmhUnsupported = -95,    /* RSC version or TCS/command counts outside the SM8750 ones */
    Sm8750RpmhTimeout = -110,       /* TCS not complete after 2000 x 1 ms; that TCS is retained */
};

enum {
    /* cmd-db layout */
    Sm8750CmdDbBytes = 0x20000,
    Sm8750CmdDbMagic = 0x0c0330db,          /* bytes db 30 03 0c at +4 */
    Sm8750CmdDbSlaves = 8,                  /* 16-byte slave headers from +8 */
    Sm8750CmdDbDataOffset = 144,            /* entry tables and aux data are relative to here */
    Sm8750CmdDbEntryBytes = 24,
    Sm8750CmdDbSlaveArc = 3,
    Sm8750CmdDbSlaveVrm = 4,
    Sm8750CmdDbSlaveBcm = 5,
    Sm8750RpmhArcMaxLevels = 16,
    Sm8750RpmhMaxCommands = 16,

    /* apps RSC DRV-2 (DT apps_rsc_drv2: tcs-offset 0xd00, tcs-distance 0x2a0, tcs-config <2 3> first) */
    Sm8750RpmhTcsBase = 0xd00,              /* IRQ_ENABLE +0, IRQ_STATUS +4, IRQ_CLEAR +8 */
    Sm8750RpmhTcsStride = 0x2a0,
    Sm8750RpmhActiveTcs = 3,                /* TCS 0..2 */
    Sm8750RpmhMsgIdWrite = 0x00010108,      /* write, response required, 4 bytes */
    Sm8750RpmhCmdComplete = 0x00010100,     /* CMD_STATUS issued | completed */
    Sm8750RpmhAmcEnable = 0x00010000,
    Sm8750RpmhAmcTrigger = 0x01000000,
    Sm8750RpmhPollUs = 1000,
    Sm8750RpmhPolls = 2000,
    Sm8750RpmhReadbackTries = 100,          /* 1 us apart */
    Sm8750RpmhLockStepUs = 10,
    Sm8750RpmhLockSteps = 200000,           /* 2 s */
    Sm8750RpmhLockOwnerGpu = 0x41383330,    /* 'A830' */

    /* votes, ARCHITECTURE 4.3.2-4.3.3 */
    Sm8750RpmhLowSvs = 0x40,                /* RPMH_REGULATOR_LEVEL_LOW_SVS: cx/mx with pagani-gpu-rails=0 */
    Sm8750RpmhCn0On = 0x60000001,           /* host CN0 (x 0, y enable_mask); GMU CNOC table level 1 */
    Sm8750RpmhCn0Off = 0x40000000,          /* GMU CNOC table level 0 only: never a host vote */
    Sm8750RpmhSh0BusWidth = 16,             /* Linux a8xx_catalog.c A830 bcms */
    Sm8750RpmhMc0BusWidth = 4,
    Sm8750RpmhAcvGpuPerfmode = 1 << 2,      /* GMU bandwidth table only */
    Sm8750RpmhAcvPerfmodeKbps = 10687500,   /* GMU table: DDR level 7 and above */
    Sm8750RpmhAcvHlosEnable = 1 << 0,       /* host ACV: Linux sm8750.c bcm_acv.enable_mask */
    Sm8750RpmhBcmVoteMask = 0x3fff,
    Sm8750RpmhDdrDefaultLevel = 11,         /* pagani-gpu-ddr default (author's measured vote); floor = same */

    Sm8750RpmhKernelStackBytes = 16384,     /* XNU kernel thread stack */
};

/* Snapshot of the whole reserved region; only the snapshot is parsed. 128 KiB: a static object
 * or a one-time heap allocation at S0, never a local variable (the static assertion below says
 * why; build with -Wframe-larger-than to catch a stack copy). */
typedef struct Sm8750CmdDb {
    uint8_t snapshot[Sm8750CmdDbBytes];
    uint32_t bytes;                         /* Sm8750CmdDbBytes once copied, else 0 */
} Sm8750CmdDb;
#ifdef __cplusplus
static_assert(sizeof(Sm8750CmdDb) > 4 * Sm8750RpmhKernelStackBytes, "Sm8750CmdDb never fits a kernel stack");
#else
_Static_assert(sizeof(Sm8750CmdDb) > 4 * Sm8750RpmhKernelStackBytes, "Sm8750CmdDb never fits a kernel stack");
#endif

typedef struct Sm8750CmdDbEntry {
    uint32_t addr;                          /* RPMh address: slave type in [31:16] (checked), offset in [15:0] */
    uint32_t slave;                         /* slv_id of the slave header holding the entry */
    uint32_t aux_offset;                    /* byte offset of the aux data in the snapshot */
    uint32_t aux_len;
} Sm8750CmdDbEntry;

typedef struct Sm8750RpmhArc {
    uint32_t addr;
    uint16_t vlvl[Sm8750RpmhArcMaxLevels];  /* aux table: vlvl[hlvl], zero padded */
    uint8_t count;                          /* usable levels (KGSL adreno_rpmh_arc_cmds); 0 when absent */
    uint8_t present;                        /* 0: not in cmd-db, or a dummy entry */
    uint16_t aux_len;
} Sm8750RpmhArc;

typedef struct Sm8750RpmhBcm {
    uint32_t addr;
    uint32_t unit;                          /* aux le32 */
    uint16_t width;                         /* aux le16 */
    uint8_t vcd;
    uint8_t present;
    uint16_t aux_len;                       /* aux bytes; below 8 only for CN0, whose unit/width/vcd then stay 0 */
} Sm8750RpmhBcm;

enum {
    Sm8750RpmhResNone, Sm8750RpmhResGfx, Sm8750RpmhResMx, Sm8750RpmhResCx, Sm8750RpmhResGmxc,
    Sm8750RpmhResGbx, Sm8750RpmhResSh0, Sm8750RpmhResMc0, Sm8750RpmhResAcv, Sm8750RpmhResCn0,
};

/* Everything S0 needs from cmd-db. gfx/mx/cx and SH0/MC0/ACV/CN0 are required; gmxc and gbx
 * are optional, and a dummy gmxc counts as absent (KGSL build_dcvs_table). */
typedef struct Sm8750RpmhResources {
    uint32_t version;                       /* cmd-db word 0, diagnostics only */
    Sm8750RpmhArc gfx, mx, cx, gmxc, gbx;
    Sm8750RpmhBcm sh0, mc0, acv, cn0;
    uint32_t failed;                        /* Sm8750RpmhRes* of the first resource that failed */
} Sm8750RpmhResources;

typedef struct Sm8750RpmhCmd {
    uint32_t addr, data;
} Sm8750RpmhCmd;

typedef struct Sm8750RpmhRequest {
    Sm8750RpmhCmd cmd[Sm8750RpmhMaxCommands];
    uint32_t count;
    uint32_t wait_mask;                     /* WAIT_FOR_CMPL: all commands, or the BCM commit bits */
} Sm8750RpmhRequest;

typedef enum {
    Sm8750RpmhStepNone,
    Sm8750RpmhStepLock,
    Sm8750RpmhStepHardware,
    Sm8750RpmhStepSelect,
    Sm8750RpmhStepIrqMask,
    Sm8750RpmhStepProgram,
    Sm8750RpmhStepTrigger,
    Sm8750RpmhStepPoll,
    Sm8750RpmhStepCleanup,
    Sm8750RpmhStepDone,
} Sm8750RpmhStep;

/* Evidence of one TCS transaction for the status block ("TCS <slot> st<status> c<cmd0> ..."). */
typedef struct Sm8750RpmhResult {
    uint32_t hardware;                      /* DRV-2 +0x00: version major in [23:16] */
    uint32_t tcs;                           /* ACTIVE TCS used, 0xffffffff when none was claimed */
    uint32_t status;                        /* last TCS STATUS read (1 = idle) */
    uint32_t command[Sm8750RpmhMaxCommands];/* CMD_STATUS per command at the last poll */
    uint32_t config;                        /* DRV-2 +0x0c: NTCS [17:12], NCMD [31:27] */
    uint32_t busy;                          /* ACTIVE TCSes found not idle, bit n = TCS n */
    uint32_t irq_enable;                    /* IRQ_ENABLE before the request */
    uint32_t irq_status;                    /* IRQ_STATUS at completion */
    uint32_t polls;                         /* 1 ms completion polls */
    uint32_t lock_waits;                    /* 10 us waits for the DRV-2 lock */
    uint32_t step;                          /* Sm8750RpmhStep reached, or where it failed */
    uint32_t reg, value, expect;            /* failed read-back: register, value read, value written */
} Sm8750RpmhResult;

/* --- cmd-db ------------------------------------------------------------------------------ */

/* Copy the whole region with aligned 32-bit reads (Device memory: no memcpy). Checks the magic
 * first and reads nothing else when it is wrong. */
int Sm8750CmdDbCopy(const Sm8750RpmhIo *db, Sm8750CmdDb *out);

/* Find a unique entry by name (1..8 bytes, compared zero padded like Linux cmd_db_get_header)
 * in a slave of type `slave`. Slave headers end at the first slv_id 0; every entry table must
 * lie inside the snapshot; a name present twice is Corrupt; the entry's address must carry the
 * slave type in bits [31:16] and be 4-byte aligned; its aux data must lie inside the snapshot. */
int Sm8750CmdDbFind(const Sm8750CmdDb *db, const char *name, unsigned slave, Sm8750CmdDbEntry *entry);

/* ARC: vlvl table and usable level count. Dummy (all levels after the first are 0) returns
 * NoData with count 0 and the table filled. More than 16 usable levels is Corrupt. */
int Sm8750CmdDbArc(const Sm8750CmdDb *db, const char *name, uint32_t *addr,
    uint16_t vlvl[Sm8750RpmhArcMaxLevels], unsigned *count);
/* BCM: aux {le32 unit, le16 width, u8 vcd, u8 reserved}; fewer than 8 aux bytes is Corrupt. */
int Sm8750CmdDbBcm(const Sm8750CmdDb *db, const char *name, uint32_t *addr, uint32_t *unit,
    uint16_t *width, uint8_t *vcd);
/* VRM: base address, 16-byte aligned (voltage +0, enable +4, mode +8). */
int Sm8750CmdDbVrm(const Sm8750CmdDb *db, const char *name, uint32_t *addr);

/* Parse every resource of Sm8750RpmhResources. SH0, MC0 and ACV need their 8-byte aux, and SH0/MC0
 * a nonzero unit and width (their votes scale by the width and divide by the unit). CN0 needs only
 * its entry: the host vote and the GMU CNOC rows are constants, and the parser of the author's
 * PKX110-qualified build accepted a CN0 without aux; its aux is recorded when it has 8 bytes. The
 * resources found must have distinct addresses. On failure `out` keeps what was parsed and `failed`
 * names the resource (for a repeated address, the later one). */
int Sm8750RpmhResourcesParse(const Sm8750CmdDb *db, Sm8750RpmhResources *out);

/* --- levels and encoding ------------------------------------------------------------------ */

/* First hlvl whose vlvl >= `vlvl` (LOW_SVS 0x40 for pagani-gpu-rails=0; 1 gives the lowest
 * nonzero level); Range when every level is lower. */
int Sm8750RpmhArcLevel(const Sm8750RpmhArc *arc, uint32_t vlvl, uint32_t *hlvl);
/* hlvl of the highest vlvl (pagani-gpu-rails=1, the default). */
int Sm8750RpmhArcTop(const Sm8750RpmhArc *arc, uint32_t *hlvl);

/* BCM_TCS_CMD: commit<<30 | valid<<29 | (x & 0x3fff)<<14 | (y & 0x3fff). */
uint32_t Sm8750BcmCommand(int commit, int valid, uint32_t x, uint32_t y);

/* {mx, cx} when raising, {cx, mx} when lowering; waits on both. */
int Sm8750RpmhRailRequest(const Sm8750RpmhResources *res, uint32_t mx_hlvl, uint32_t cx_hlvl, int raise,
    Sm8750RpmhRequest *out);

/* Both DDR encoders return {SH0, MC0, ACV} with commit on the last BCM and wherever the next BCM
 * has another VCD, and wait_mask = the commit bits. SH0/MC0: peak = kbps*width/buswidth (buswidth
 * 16 and 4), vote = peak*1000/unit clamped to [1, 0x3fff], x = y = vote. */

/* GMU bandwidth table row (A830Votes, HFI BW_VOTE_TBL; never sent from apps DRV-2): 0 kBps gives
 * valid 0 for all three; ACV y = BIT(2) from 10687500 kBps, else 0. */
int Sm8750RpmhGmuBwRequest(const Sm8750RpmhResources *res, uint32_t kbps, Sm8750RpmhRequest *out);
/* Host DDR vote (apps DRV-2, HLOS identity): SH0/MC0 are keepalive, so 0 kBps still votes
 * x = y = 1 and never valid 0; ACV y = BIT(0) when kbps > 0, valid 0 only at 0 kBps. */
int Sm8750RpmhHostDdrRequest(const Sm8750RpmhResources *res, uint32_t kbps, Sm8750RpmhRequest *out);
/* Host CN0 vote: always 0x60000001 (keepalive; 0x40000000 belongs to the GMU CNOC table). */
int Sm8750RpmhHostCn0Request(const Sm8750RpmhResources *res, Sm8750RpmhRequest *out);

/* --- apps RSC DRV-2 ----------------------------------------------------------------------- */

/* Send `n` commands through an idle ACTIVE TCS and wait for completion. Thread context only:
 * it may wait 2 s for the lock and 2 s for the TCS.
 *  1. take the DRV-2 lock;
 *  2. DRV-2 +0x00 [23:16] selects the register layout as in Linux and the SoC kit: 3 (SM8750)
 *     or 2; anything else is refused. NTCS [17:12] and NCMD [31:27] in 3..16, n <= NCMD;
 *  3. the first of TCS 0..2 with CONTROL, CMD_ENABLE and WAIT_FOR_CMPL 0, its IRQ_STATUS bit 0
 *     and STATUS 1;
 *  4. clear its IRQ_ENABLE bit; per command MSGID 0x10108, ADDR, DATA; WAIT_FOR_CMPL =
 *     wait_mask; CMD_ENABLE = (1<<n)-1; CONTROL 0x10000, then trigger with 0x01010000. Every
 *     write except the trigger must read back within 100 x 1 us;
 *  5. every 1 ms, at most 2000 times: STATUS 1 and every CMD_STATUS has 0x10100 (and is not
 *     0xffffffff);
 *  6. CONTROL 0x10000 then 0, CMD_ENABLE 0, WAIT_FOR_CMPL 0, IRQ_CLEAR the TCS bit, put back
 *     the TCS's original IRQ_ENABLE bit keeping the others as read now.
 * A TCS that timed out is left triggered with its IRQ masked: it is never idle again, so it is
 * not reused, and its registers stay as evidence. A TCS that failed before the trigger is
 * cleaned as in step 6. Commands are checked first: 4-byte aligned address of slave 3, 4 or 5;
 * ARC data <= 0xff, VRM data < 0x2000, BCM data with bits 31 and 28 clear. */
int Sm8750RpmhWriteActive(const Sm8750RpmhIo *drv2, const Sm8750RpmhCmd *cmds, unsigned n,
    uint32_t wait_mask, Sm8750RpmhResult *result);
int Sm8750RpmhSend(const Sm8750RpmhIo *drv2, const Sm8750RpmhRequest *request, Sm8750RpmhResult *result);

/* DRV-2 lock (ARCHITECTURE 4.3.4). One 32-bit word, `sm8750_rpmh_drv2_owner`, defined weak in
 * every user (this file and SOC kits/Sm8750Platform/Rpmh.h for the touch votes), so the kernel
 * link keeps a single copy: 0 = free, any other value = owner. Acquire swaps 0 for `owner`
 * atomically, waiting 10 us between tries, at most 2 s; release stores 0. Must not be held
 * across Sm8750RpmhWriteActive, which takes it itself. `waits` (optional) gets the 10 us waits. */
int Sm8750RpmhDrv2Acquire(const Sm8750RpmhIo *io, uint32_t owner, uint32_t *waits);
void Sm8750RpmhDrv2Release(void);
uint32_t Sm8750RpmhDrv2Owner(void);

/* --- votes held by this driver (ARCHITECTURE 4.3.3), all ACTIVE ---------------------------- */

/* Defaults are the author's measured votes: rails at the table top, CN0 0x60000001, DDR level 11
 * with ACV BIT(0), floor = boot level. Nothing is ever revoked: apps DRV-2 holds one vote per
 * resource, UFS and MDP have none of their own, and boot-time votes cannot be read back
 * (4.2.3 D5, 4.3.3). Every call sends nothing when the same vote is already held, so the S5
 * floor equal to the boot level costs no TCS. A failed send leaves `held` and the recorded
 * levels as they were. */
enum { Sm8750RpmhHeldRails = 1, Sm8750RpmhHeldCn0 = 2, Sm8750RpmhHeldDdr = 4 };

/* One owner at a time (the start sequence, later the DCVS thread); the DRV-2 lock serializes the
 * hardware, not this structure. `drv2` and `res` must outlive the host. */
typedef struct Sm8750RpmhHost {
    const Sm8750RpmhIo *drv2;
    const Sm8750RpmhResources *res;
    uint32_t held;                          /* Sm8750RpmhHeld* voted by us */
    uint32_t mx_hlvl, cx_hlvl;              /* while Sm8750RpmhHeldRails */
    uint32_t ddr_kbps;                      /* while Sm8750RpmhHeldDdr (0 = keepalive minimum) */
    uint32_t sent;                          /* TCS transactions attempted */
    Sm8750RpmhRequest request;              /* last request sent */
    Sm8750RpmhResult last;                  /* its TCS evidence */
} Sm8750RpmhHost;

void Sm8750RpmhHostInit(Sm8750RpmhHost *host, const Sm8750RpmhIo *drv2, const Sm8750RpmhResources *res);
/* S1: mx/cx at the top of their tables (`highest`, pagani-gpu-rails=1) or at LOW_SVS (0); mx
 * first when raising, cx first when lowering. */
int Sm8750RpmhHostRails(Sm8750RpmhHost *host, int highest);
/* S2, after GPU_INIT_REGS: CN0 0x60000001, once. */
int Sm8750RpmhHostCn0(Sm8750RpmhHost *host);
/* S2: DDR boot level (pagani-gpu-ddr, kBps of DT level 11 by default); S5: the floor
 * (pagani-gpu-ddr-floor, = boot level by default, so nothing is sent; 0 = keepalive minimum). */
int Sm8750RpmhHostDdr(Sm8750RpmhHost *host, uint32_t kbps);

#ifdef __cplusplus
}
#endif
#endif
