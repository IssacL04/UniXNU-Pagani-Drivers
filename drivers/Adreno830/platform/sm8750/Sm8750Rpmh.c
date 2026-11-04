/* SPDX-License-Identifier: MIT */
#include "Sm8750Rpmh.h"
#include <stddef.h>

/* The DRV-2 lock word (ARCHITECTURE 4.3.4). Every DRV-2 user defines it weak with this exact
 * name and type; the static kernel link coalesces the definitions into one word. */
extern volatile uint32_t sm8750_rpmh_drv2_owner;
__attribute__((weak)) volatile uint32_t sm8750_rpmh_drv2_owner;

/* DRV-2 registers outside the TCSes */
enum {
    RscId = 0x00,                           /* version major [23:16] */
    RscConfig = 0x0c,                       /* DRV_PRNT_CHLD_CONFIG: NTCS of DRV n at [6n+5:6n], NCMD [31:27] */
    RscIrqEnable = Sm8750RpmhTcsBase + 0x0,
    RscIrqStatus = Sm8750RpmhTcsBase + 0x4,
    RscIrqClear = Sm8750RpmhTcsBase + 0x8,
    RscDrvId = 2,
};

/* --- cmd-db ------------------------------------------------------------------------------ */

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | le16(p + 2) << 16;
}

int Sm8750CmdDbCopy(const Sm8750RpmhIo *db, Sm8750CmdDb *out)
{
    if (!db || !db->read || !out)
        return Sm8750RpmhArgument;
    out->bytes = 0;
    if (db->read(db->context, 4) != (uint32_t)Sm8750CmdDbMagic)
        return Sm8750RpmhNotFound;
    for (uint32_t at = 0; at < Sm8750CmdDbBytes; at += 4) {
        uint32_t word = db->read(db->context, at);
        out->snapshot[at] = (uint8_t)word;
        out->snapshot[at + 1] = (uint8_t)(word >> 8);
        out->snapshot[at + 2] = (uint8_t)(word >> 16);
        out->snapshot[at + 3] = (uint8_t)(word >> 24);
    }
    if (le32(out->snapshot + 4) != (uint32_t)Sm8750CmdDbMagic)
        return Sm8750RpmhCorrupt;
    out->bytes = Sm8750CmdDbBytes;
    return Sm8750RpmhOk;
}

static int same_id(const uint8_t *id, const uint8_t key[8])
{
    for (unsigned i = 0; i < 8; i++)
        if (id[i] != key[i])
            return 0;
    return 1;
}

int Sm8750CmdDbFind(const Sm8750CmdDb *db, const char *name, unsigned slave, Sm8750CmdDbEntry *entry)
{
    uint8_t key[8] = {0};
    unsigned length = 0, matches = 0;
    Sm8750CmdDbEntry found = {0, 0, 0, 0};
    if (!db || !name || !entry || !slave || slave > 7 || db->bytes != Sm8750CmdDbBytes)
        return Sm8750RpmhArgument;
    while (length < 8 && name[length]) {
        key[length] = (uint8_t)name[length];
        length++;
    }
    if (!length || name[length])
        return Sm8750RpmhArgument;
    if (le32(db->snapshot + 4) != (uint32_t)Sm8750CmdDbMagic)
        return Sm8750RpmhNotFound;
    for (unsigned i = 0; i < Sm8750CmdDbSlaves; i++) {
        const uint8_t *h = db->snapshot + 8 + 16 * i;
        uint32_t id = le16(h), table = Sm8750CmdDbDataOffset + le16(h + 2);
        uint32_t data = Sm8750CmdDbDataOffset + le16(h + 4), count = le16(h + 6);
        if (!id)
            break;
        if (table > db->bytes || count > (db->bytes - table) / Sm8750CmdDbEntryBytes || data > db->bytes)
            return Sm8750RpmhCorrupt;
        for (uint32_t n = 0; n < count; n++) {
            const uint8_t *e = db->snapshot + table + Sm8750CmdDbEntryBytes * n;
            if (!same_id(e, key))
                continue;
            matches++;
            found.slave = id;
            found.addr = le32(e + 16);
            found.aux_len = le16(e + 20);
            found.aux_offset = data + le16(e + 22);
        }
    }
    if (!matches)
        return Sm8750RpmhNotFound;
    if (matches != 1 || found.slave != slave || (found.addr >> 16) != slave || (found.addr & 3) ||
        found.aux_offset > db->bytes || found.aux_len > db->bytes - found.aux_offset)
        return Sm8750RpmhCorrupt;
    *entry = found;
    return Sm8750RpmhOk;
}

int Sm8750CmdDbArc(const Sm8750CmdDb *db, const char *name, uint32_t *addr,
    uint16_t vlvl[Sm8750RpmhArcMaxLevels], unsigned *count)
{
    Sm8750CmdDbEntry e;
    if (!addr || !vlvl || !count)
        return Sm8750RpmhArgument;
    int rc = Sm8750CmdDbFind(db, name, Sm8750CmdDbSlaveArc, &e);
    if (rc)
        return rc;
    const uint8_t *aux = db->snapshot + e.aux_offset;
    uint32_t entries = e.aux_len / 2, num;
    int dummy = 1;
    /* KGSL adreno_rpmh_arc_cmds: count up to the zero padding; dummy if nothing after entry 0. */
    for (num = 1; num < entries; num++) {
        uint32_t level = le16(aux + 2 * num);
        if (level)
            dummy = 0;
        if (le16(aux + 2 * (num - 1)) && !level)
            break;
    }
    if (!dummy && num > Sm8750RpmhArcMaxLevels)
        return Sm8750RpmhCorrupt;
    for (uint32_t i = 0; i < Sm8750RpmhArcMaxLevels; i++)
        vlvl[i] = (uint16_t)(i < entries ? le16(aux + 2 * i) : 0);
    *addr = e.addr;
    *count = dummy ? 0 : num;
    return dummy ? Sm8750RpmhNoData : Sm8750RpmhOk;
}

/* BCM aux {le32 unit, le16 width, u8 vcd, u8 reserved} of an entry Sm8750CmdDbFind returned. */
static int bcm_aux(const Sm8750CmdDb *db, const Sm8750CmdDbEntry *e, uint32_t *unit, uint16_t *width, uint8_t *vcd)
{
    if (e->aux_len < 8)
        return Sm8750RpmhCorrupt;
    const uint8_t *aux = db->snapshot + e->aux_offset;
    *unit = le32(aux);
    *width = (uint16_t)le16(aux + 4);
    *vcd = aux[6];
    return Sm8750RpmhOk;
}

int Sm8750CmdDbBcm(const Sm8750CmdDb *db, const char *name, uint32_t *addr, uint32_t *unit,
    uint16_t *width, uint8_t *vcd)
{
    Sm8750CmdDbEntry e;
    if (!addr || !unit || !width || !vcd)
        return Sm8750RpmhArgument;
    int rc = Sm8750CmdDbFind(db, name, Sm8750CmdDbSlaveBcm, &e);
    if (rc)
        return rc;
    if ((rc = bcm_aux(db, &e, unit, width, vcd)))
        return rc;
    *addr = e.addr;
    return Sm8750RpmhOk;
}

int Sm8750CmdDbVrm(const Sm8750CmdDb *db, const char *name, uint32_t *addr)
{
    Sm8750CmdDbEntry e;
    if (!addr)
        return Sm8750RpmhArgument;
    int rc = Sm8750CmdDbFind(db, name, Sm8750CmdDbSlaveVrm, &e);
    if (rc)
        return rc;
    if (e.addr & 15)
        return Sm8750RpmhCorrupt;
    *addr = e.addr;
    return Sm8750RpmhOk;
}

static int parse_arc(const Sm8750CmdDb *db, const char *name, Sm8750RpmhArc *arc)
{
    unsigned count = 0;
    int rc = Sm8750CmdDbArc(db, name, &arc->addr, arc->vlvl, &count);
    arc->count = (uint8_t)count;
    arc->present = rc == Sm8750RpmhOk;
    if (rc == Sm8750RpmhOk || rc == Sm8750RpmhNoData) {
        Sm8750CmdDbEntry e;
        if (!Sm8750CmdDbFind(db, name, Sm8750CmdDbSlaveArc, &e))
            arc->aux_len = (uint16_t)e.aux_len;
    }
    return rc;
}

/* A BCM of Sm8750RpmhResources. Without `need_aux` (CN0) a short aux leaves unit/width/vcd 0. */
static int parse_bcm(const Sm8750CmdDb *db, const char *name, int need_aux, Sm8750RpmhBcm *bcm)
{
    Sm8750CmdDbEntry e;
    int rc = Sm8750CmdDbFind(db, name, Sm8750CmdDbSlaveBcm, &e);
    if (rc)
        return rc;
    bcm->addr = e.addr;
    bcm->aux_len = (uint16_t)e.aux_len;
    if ((rc = bcm_aux(db, &e, &bcm->unit, &bcm->width, &bcm->vcd)) && need_aux)
        return rc;
    bcm->present = 1;
    return Sm8750RpmhOk;
}

int Sm8750RpmhResourcesParse(const Sm8750CmdDb *db, Sm8750RpmhResources *out)
{
    static const char *const arcs[] = {"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "gbx.lvl"};
    static const char *const bcms[] = {"SH0", "MC0", "ACV", "CN0"};
    if (!db || !out || db->bytes != Sm8750CmdDbBytes)
        return Sm8750RpmhArgument;
    *out = (Sm8750RpmhResources){0};
    out->version = le32(db->snapshot);
    Sm8750RpmhArc *arc[] = {&out->gfx, &out->mx, &out->cx, &out->gmxc, &out->gbx};
    Sm8750RpmhBcm *bcm[] = {&out->sh0, &out->mc0, &out->acv, &out->cn0};
    for (unsigned i = 0; i < 5; i++) {
        int rc = parse_arc(db, arcs[i], arc[i]);
        int optional = i >= 3 && (rc == Sm8750RpmhNotFound || rc == Sm8750RpmhNoData);
        /* One ARC under two names is malformed (the author's PKX110 build refused mx == cx). */
        for (unsigned j = 0; !rc && j < i; j++)
            if (arc[j]->present && arc[j]->addr == arc[i]->addr)
                rc = Sm8750RpmhCorrupt;
        if (rc && !optional) {
            out->failed = Sm8750RpmhResGfx + i;
            return rc;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        int rc = parse_bcm(db, bcms[i], i != 3, bcm[i]);
        if (!rc && i < 2 && (!bcm[i]->unit || !bcm[i]->width))    /* SH0, MC0: kBps * width / buswidth * 1000 / unit */
            rc = Sm8750RpmhCorrupt;
        for (unsigned j = 0; !rc && j < i; j++)
            if (bcm[j]->addr == bcm[i]->addr)
                rc = Sm8750RpmhCorrupt;
        if (rc) {
            out->failed = Sm8750RpmhResSh0 + i;
            return rc;
        }
    }
    return Sm8750RpmhOk;
}

/* --- levels and encoding ------------------------------------------------------------------ */

static int arc_usable(const Sm8750RpmhArc *arc)
{
    return arc && arc->present && arc->count && arc->count <= Sm8750RpmhArcMaxLevels;
}

int Sm8750RpmhArcLevel(const Sm8750RpmhArc *arc, uint32_t vlvl, uint32_t *hlvl)
{
    if (!arc_usable(arc) || !hlvl)
        return Sm8750RpmhArgument;
    for (uint32_t h = 0; h < arc->count; h++) {
        if (arc->vlvl[h] >= vlvl) {
            *hlvl = h;
            return Sm8750RpmhOk;
        }
    }
    return Sm8750RpmhRange;
}

int Sm8750RpmhArcTop(const Sm8750RpmhArc *arc, uint32_t *hlvl)
{
    uint32_t top = 0;
    if (!arc_usable(arc) || !hlvl)
        return Sm8750RpmhArgument;
    for (uint32_t h = 1; h < arc->count; h++)
        if (arc->vlvl[h] > arc->vlvl[top])
            top = h;
    if (!arc->vlvl[top])
        return Sm8750RpmhRange;
    *hlvl = top;
    return Sm8750RpmhOk;
}

uint32_t Sm8750BcmCommand(int commit, int valid, uint32_t x, uint32_t y)
{
    return (commit ? 1u << 30 : 0) | (valid ? 1u << 29 : 0) | (x & Sm8750RpmhBcmVoteMask) << 14 |
        (y & Sm8750RpmhBcmVoteMask);
}

int Sm8750RpmhRailRequest(const Sm8750RpmhResources *res, uint32_t mx_hlvl, uint32_t cx_hlvl, int raise,
    Sm8750RpmhRequest *out)
{
    if (!res || !out || !arc_usable(&res->mx) || !arc_usable(&res->cx) || mx_hlvl >= res->mx.count ||
        cx_hlvl >= res->cx.count)
        return Sm8750RpmhArgument;
    Sm8750RpmhCmd mx = {res->mx.addr, mx_hlvl}, cx = {res->cx.addr, cx_hlvl};
    *out = (Sm8750RpmhRequest){0};
    out->cmd[0] = raise ? mx : cx;          /* MX never below CX: raise MX first, lower CX first */
    out->cmd[1] = raise ? cx : mx;
    out->count = 2;
    out->wait_mask = 3;
    return Sm8750RpmhOk;
}

typedef enum { GmuTable, HostVote } BcmUse;

/* {SH0, MC0, ACV} for one bandwidth, in one of the two encodings of ARCHITECTURE 4.3.2. */
static int ddr_request(const Sm8750RpmhResources *res, uint32_t kbps, BcmUse use, Sm8750RpmhRequest *out)
{
    if (!res || !out)
        return Sm8750RpmhArgument;
    const Sm8750RpmhBcm *bcm[3] = {&res->sh0, &res->mc0, &res->acv};
    static const uint32_t buswidth[2] = {Sm8750RpmhSh0BusWidth, Sm8750RpmhMc0BusWidth};
    for (unsigned i = 0; i < 3; i++)
        if (!bcm[i]->present || (i < 2 && !bcm[i]->unit))
            return Sm8750RpmhArgument;
    *out = (Sm8750RpmhRequest){0};
    for (unsigned i = 0; i < 3; i++) {
        int commit = i == 2 || bcm[i]->vcd != bcm[i + 1]->vcd;
        uint32_t data;
        if (i == 2) {                       /* ACV: a fixed BCM, an enable bit and no bandwidth */
            uint32_t bit = use == HostVote ? (uint32_t)Sm8750RpmhAcvHlosEnable :
                kbps >= (uint32_t)Sm8750RpmhAcvPerfmodeKbps ? (uint32_t)Sm8750RpmhAcvGpuPerfmode : 0;
            data = kbps ? Sm8750BcmCommand(commit, 1, 0, bit) : Sm8750BcmCommand(commit, 0, 0, 0);
        } else if (!kbps && use == GmuTable) {
            data = Sm8750BcmCommand(commit, 0, 0, 0);
        } else {                            /* host SH0/MC0 are keepalive: never below x = y = 1 */
            uint64_t peak = (uint64_t)kbps * bcm[i]->width / buswidth[i];
            uint64_t vote = peak * 1000 / bcm[i]->unit;
            if (!vote)
                vote = 1;
            if (vote > Sm8750RpmhBcmVoteMask)
                vote = Sm8750RpmhBcmVoteMask;
            data = Sm8750BcmCommand(commit, 1, (uint32_t)vote, (uint32_t)vote);
        }
        out->cmd[i].addr = bcm[i]->addr;
        out->cmd[i].data = data;
        if (commit)
            out->wait_mask |= 1u << i;
    }
    out->count = 3;
    return Sm8750RpmhOk;
}

int Sm8750RpmhGmuBwRequest(const Sm8750RpmhResources *res, uint32_t kbps, Sm8750RpmhRequest *out)
{
    return ddr_request(res, kbps, GmuTable, out);
}

int Sm8750RpmhHostDdrRequest(const Sm8750RpmhResources *res, uint32_t kbps, Sm8750RpmhRequest *out)
{
    return ddr_request(res, kbps, HostVote, out);
}

int Sm8750RpmhHostCn0Request(const Sm8750RpmhResources *res, Sm8750RpmhRequest *out)
{
    if (!res || !out || !res->cn0.present)
        return Sm8750RpmhArgument;
    *out = (Sm8750RpmhRequest){0};
    out->cmd[0].addr = res->cn0.addr;
    out->cmd[0].data = Sm8750RpmhCn0On;
    out->count = 1;
    out->wait_mask = 1;
    return Sm8750RpmhOk;
}

/* --- DRV-2 lock --------------------------------------------------------------------------- */

int Sm8750RpmhDrv2Acquire(const Sm8750RpmhIo *io, uint32_t owner, uint32_t *waits)
{
    if (!io || !io->delay || !owner)
        return Sm8750RpmhArgument;
    for (uint32_t n = 0;; n++) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_rpmh_drv2_owner, &expected, owner, 0, __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED)) {
            if (waits)
                *waits = n;
            return Sm8750RpmhOk;
        }
        if (n == Sm8750RpmhLockSteps) {
            if (waits)
                *waits = n;
            return Sm8750RpmhBusy;
        }
        io->delay(io->context, Sm8750RpmhLockStepUs);
    }
}

void Sm8750RpmhDrv2Release(void)
{
    __atomic_store_n(&sm8750_rpmh_drv2_owner, 0, __ATOMIC_RELEASE);
}

uint32_t Sm8750RpmhDrv2Owner(void)
{
    return __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
}

/* --- ACTIVE TCS --------------------------------------------------------------------------- */

typedef struct {
    uint32_t wait, control, status, enable, msgid, stride;
} Layout;

typedef struct {
    const Sm8750RpmhIo *io;
    Sm8750RpmhResult *result;
} Ctx;

static uint32_t rd(Ctx *c, uint32_t reg)
{
    return c->io->read(c->io->context, reg);
}

static void wr(Ctx *c, uint32_t reg, uint32_t value)
{
    c->io->write(c->io->context, reg, value);
}

/* Write and wait until it reads back, as Linux write_tcs_reg_sync and the SoC kit do. */
static int wr_checked(Ctx *c, uint32_t reg, uint32_t value)
{
    uint32_t seen = 0;
    wr(c, reg, value);
    for (unsigned i = 0; i < Sm8750RpmhReadbackTries; i++) {
        seen = rd(c, reg);
        if (seen == value)
            return Sm8750RpmhOk;
        c->io->delay(c->io->context, 1);
    }
    c->result->reg = reg;
    c->result->value = seen;
    c->result->expect = value;
    return Sm8750RpmhIoError;
}

/* Values the RPMh accelerators accept from the apps DRV, as the PKX110-tested vote() checks them. */
static int command_ok(const Sm8750RpmhCmd *cmd)
{
    if (!cmd->addr || (cmd->addr & 3))
        return 0;
    switch (cmd->addr >> 16) {
    case Sm8750CmdDbSlaveArc:
        return cmd->data <= 0xff;
    case Sm8750CmdDbSlaveVrm:
        return cmd->data < 0x2000;
    case Sm8750CmdDbSlaveBcm:
        return !(cmd->data & 0x90000000u);
    default:
        return 0;
    }
}

static int transact(Ctx *c, const Sm8750RpmhCmd *cmds, unsigned n, uint32_t wait_mask)
{
    Sm8750RpmhResult *r = c->result;
    r->step = Sm8750RpmhStepHardware;
    r->hardware = rd(c, RscId);
    r->config = rd(c, RscConfig);
    uint32_t major = (r->hardware >> 16) & 0xff, ntcs = (r->config >> (6 * RscDrvId)) & 0x3f;
    uint32_t ncmd = (r->config >> 27) & 0x1f;
    if ((major != 2 && major != 3) || ntcs < 3 || ntcs > 16 || ncmd < 3 || ncmd > 16 || n > ncmd)
        return Sm8750RpmhUnsupported;
    /* Linux rpmh_rsc_reg_offset_ver_2_7 / _ver_3_0 */
    const Layout l = major == 2 ? (Layout){0x10, 0x14, 0x18, 0x1c, 0x30, 20} : (Layout){0x20, 0x24, 0x28, 0x2c, 0x34, 24};

    r->step = Sm8750RpmhStepSelect;
    uint32_t slot;
    for (slot = 0; slot < Sm8750RpmhActiveTcs; slot++) {
        uint32_t b = Sm8750RpmhTcsBase + slot * Sm8750RpmhTcsStride;
        if (rd(c, b + l.control) || rd(c, b + l.enable) || rd(c, b + l.wait) || (rd(c, RscIrqStatus) & (1u << slot)) ||
            rd(c, b + l.status) != 1) {
            r->busy |= 1u << slot;
            continue;
        }
        break;
    }
    if (slot == Sm8750RpmhActiveTcs)
        return Sm8750RpmhBusy;
    r->tcs = slot;
    const uint32_t base = Sm8750RpmhTcsBase + slot * Sm8750RpmhTcsStride, bit = 1u << slot;
    const uint32_t all = (1u << n) - 1;

    r->step = Sm8750RpmhStepIrqMask;
    const uint32_t irq = r->irq_enable = rd(c, RscIrqEnable);
    int rc = wr_checked(c, RscIrqEnable, irq & ~bit);
    if (rc)
        return rc;

    r->step = Sm8750RpmhStepProgram;
    for (unsigned i = 0; !rc && i < n; i++) {
        uint32_t at = base + l.msgid + i * l.stride;
        rc = wr_checked(c, at, Sm8750RpmhMsgIdWrite);
        if (!rc)
            rc = wr_checked(c, at + 4, cmds[i].addr);
        if (!rc)
            rc = wr_checked(c, at + 8, cmds[i].data);
    }
    if (!rc)
        rc = wr_checked(c, base + l.wait, wait_mask);
    if (!rc)
        rc = wr_checked(c, base + l.enable, all);
    if (!rc)
        rc = wr_checked(c, base + l.control, Sm8750RpmhAmcEnable);
    if (!rc) {
        r->step = Sm8750RpmhStepTrigger;
        wr(c, base + l.control, Sm8750RpmhAmcEnable | Sm8750RpmhAmcTrigger);
        r->step = Sm8750RpmhStepPoll;
        rc = Sm8750RpmhTimeout;
        for (uint32_t p = 0; p < Sm8750RpmhPolls; p++) {
            c->io->delay(c->io->context, Sm8750RpmhPollUs);
            r->polls = p + 1;
            r->status = rd(c, base + l.status);
            int done = r->status == 1;
            for (unsigned i = 0; i < n; i++) {
                uint32_t v = rd(c, base + l.msgid + 12 + i * l.stride);
                r->command[i] = v;
                done &= v != 0xffffffffu && (v & Sm8750RpmhCmdComplete) == Sm8750RpmhCmdComplete;
            }
            if (done) {
                rc = Sm8750RpmhOk;
                break;
            }
        }
        /* A TCS still owned by the hardware stays triggered with its IRQ masked; resetting or
         * reusing it would need hardware qualification first. */
        if (rc)
            return rc;
        r->irq_status = rd(c, RscIrqStatus);
    }

    /* Only an untriggered or a completed TCS goes back to the pool. */
    const uint32_t failed_step = r->step;
    r->step = Sm8750RpmhStepCleanup;
    if (wr_checked(c, base + l.control, Sm8750RpmhAmcEnable) || wr_checked(c, base + l.control, 0) ||
        wr_checked(c, base + l.enable, 0) || wr_checked(c, base + l.wait, 0))
        return Sm8750RpmhIoError;
    wr(c, RscIrqClear, bit);
    uint32_t current = rd(c, RscIrqEnable);
    if (wr_checked(c, RscIrqEnable, (current & ~bit) | (irq & bit)))
        return Sm8750RpmhIoError;
    r->step = rc ? failed_step : (uint32_t)Sm8750RpmhStepDone;
    return rc;
}

int Sm8750RpmhWriteActive(const Sm8750RpmhIo *drv2, const Sm8750RpmhCmd *cmds, unsigned n,
    uint32_t wait_mask, Sm8750RpmhResult *result)
{
    if (!result)
        return Sm8750RpmhArgument;
    *result = (Sm8750RpmhResult){0};
    result->tcs = 0xffffffffu;
    if (!drv2 || !drv2->read || !drv2->write || !drv2->delay || !cmds || !n || n > Sm8750RpmhMaxCommands)
        return Sm8750RpmhArgument;
    if (!wait_mask || (wait_mask & ~((1u << n) - 1)))
        return Sm8750RpmhArgument;
    for (unsigned i = 0; i < n; i++)
        if (!command_ok(&cmds[i]))
            return Sm8750RpmhArgument;
    result->step = Sm8750RpmhStepLock;
    int rc = Sm8750RpmhDrv2Acquire(drv2, Sm8750RpmhLockOwnerGpu, &result->lock_waits);
    if (rc)
        return rc;
    Ctx ctx = {drv2, result};
    rc = transact(&ctx, cmds, n, wait_mask);
    Sm8750RpmhDrv2Release();
    return rc;
}

int Sm8750RpmhSend(const Sm8750RpmhIo *drv2, const Sm8750RpmhRequest *request, Sm8750RpmhResult *result)
{
    if (!request)
        return Sm8750RpmhArgument;
    return Sm8750RpmhWriteActive(drv2, request->cmd, request->count, request->wait_mask, result);
}

/* --- votes held by this driver ------------------------------------------------------------ */

void Sm8750RpmhHostInit(Sm8750RpmhHost *host, const Sm8750RpmhIo *drv2, const Sm8750RpmhResources *res)
{
    if (!host)
        return;
    *host = (Sm8750RpmhHost){0};
    host->drv2 = drv2;
    host->res = res;
    host->last.tcs = 0xffffffffu;
}

static int host_send(Sm8750RpmhHost *host, const Sm8750RpmhRequest *request)
{
    host->request = *request;
    host->sent++;
    return Sm8750RpmhSend(host->drv2, request, &host->last);
}

int Sm8750RpmhHostRails(Sm8750RpmhHost *host, int highest)
{
    uint32_t mx = 0, cx = 0;
    Sm8750RpmhRequest request;
    if (!host || !host->res)
        return Sm8750RpmhArgument;
    const Sm8750RpmhResources *res = host->res;
    int rc = highest ? Sm8750RpmhArcTop(&res->mx, &mx) : Sm8750RpmhArcLevel(&res->mx, Sm8750RpmhLowSvs, &mx);
    if (!rc)
        rc = highest ? Sm8750RpmhArcTop(&res->cx, &cx) : Sm8750RpmhArcLevel(&res->cx, Sm8750RpmhLowSvs, &cx);
    if (rc)
        return rc;
    int held = (host->held & Sm8750RpmhHeldRails) != 0;
    if (held && mx == host->mx_hlvl && cx == host->cx_hlvl)
        return Sm8750RpmhOk;
    int raise = !held || mx > host->mx_hlvl || cx > host->cx_hlvl;
    if ((rc = Sm8750RpmhRailRequest(res, mx, cx, raise, &request)) || (rc = host_send(host, &request)))
        return rc;
    host->mx_hlvl = mx;
    host->cx_hlvl = cx;
    host->held |= Sm8750RpmhHeldRails;
    return Sm8750RpmhOk;
}

int Sm8750RpmhHostCn0(Sm8750RpmhHost *host)
{
    Sm8750RpmhRequest request;
    int rc;
    if (!host || !host->res)
        return Sm8750RpmhArgument;
    if (host->held & Sm8750RpmhHeldCn0)
        return Sm8750RpmhOk;
    if ((rc = Sm8750RpmhHostCn0Request(host->res, &request)) || (rc = host_send(host, &request)))
        return rc;
    host->held |= Sm8750RpmhHeldCn0;
    return Sm8750RpmhOk;
}

int Sm8750RpmhHostDdr(Sm8750RpmhHost *host, uint32_t kbps)
{
    Sm8750RpmhRequest request;
    int rc;
    if (!host || !host->res)
        return Sm8750RpmhArgument;
    if ((host->held & Sm8750RpmhHeldDdr) && host->ddr_kbps == kbps)
        return Sm8750RpmhOk;
    if ((rc = Sm8750RpmhHostDdrRequest(host->res, kbps, &request)) || (rc = host_send(host, &request)))
        return rc;
    host->ddr_kbps = kbps;
    host->held |= Sm8750RpmhHeldDdr;
    return Sm8750RpmhOk;
}
