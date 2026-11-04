/* The whole stack against a simulated WCN7850: boot ROM (PBL, SBL, mission mode over BHI/BHIe), MHI rings and channels, a QRTR node that offers the
 * WLAN firmware service, and a QMI peer that answers the driver's setup requests. Register-level firmware behaviour (copy engines, WMI) is not simulated:
 * the run ends where the driver starts talking to those, which must then time out cleanly. */
/* With PEACH=<case> (tools/peach-sim.sh runs them all) the device is peach, 0x110e, and the run is the peach entry's cold-boot calibration pass: the QMI
 * peer takes the files, gives FW_READY, calibrates when told to, and after the mode OFF does what the case says:
 *   host    nothing, as a chip that Android powers off: the host must power it down and up; the second session must see the same memory and cal_done
 *   self    restarts by itself within 2 s (what pagani-linux expects): no power cycle
 *   late    its service is back within 2 s, its FW_READY takes 5 s more: no power cycle either
 *   stuck   its service is back but no FW_READY ever: the host restarts it after the 10 s
 *   dead    nothing, and after the host's power cycle the chip never offers its service: one restart only, and the stack stays up
 *   nocal   pagani-wifi-cbc=0: one session, no mode OFF */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_os.h"
#include "lx_net.h"
#include "lx_plat.h"
#include "nw_board.h"
#include "nw_wifi.h"
#include "linux/soc/qcom/qmi.h"
#include "../out/ath12k-src/qmi.c"

#define BHIOFF_V 0x100
#define BHIEOFF_V 0x124
#define CHDBOFF_V 0x300
#define ERDBOFF_V 0x600
static u8 *g_bar; static u64 g_ms; static int g_stage;
enum { P_NONE, P_HOST, P_SELF, P_LATE, P_STUCK, P_DEAD, P_NOCAL };
static int g_peach;
static int g_resets, g_images, g_sessions, g_mem_requests, g_fw_ready, g_mode_off, g_mission, g_calibrations, g_restart_logged, g_cancel_logged, g_back_logged;
static u32 g_cal_done[4]; static u64 g_mem_addr[4][2]; static u32 g_mem_sessions;
static u64 g_cal_due, g_self_due, g_ready_due; static int g_global_reset, g_refused;
static u32 *R(u32 off) { return (u32 *)(g_bar + off); }
static u64 R64(u32 lo) { return ((u64)*R(lo + 4) << 32) | *R(lo); }

/* ---- host memory model: DMA addresses are host pointers masked to 32 bits by the allocator; keep a table ---- */
static struct { u8 *p; u64 bus; size_t n; } g_mem[8192]; static int g_nmem; static u64 g_next_bus = 0x10000000;
static size_t g_live, g_peak;
static size_t arena_size(size_t n) { n = (n + 63) & ~63UL; if (n >= 4096) { n = (n + 4095) & ~4095UL; size_t a = 4096; while (a < n && a < (2u << 20)) a <<= 1; return n + a / 2; } return n; }
static void *p_dma_alloc(size_t n, uint64_t *bus) {
  if (n >= 262144) { void *ra = __builtin_return_address(0); fprintf(stderr, "BIG %zu from %p\n", n, ra); }
  g_live += arena_size(n); if (g_live > g_peak) g_peak = g_live;
  if (g_nmem >= 8192) { { int live = 0; size_t sz[4] = {0}; for (int k = 0; k < g_nmem; k++) if (g_mem[k].p) { live++; } printf("SIM: dma table full, live=%d last sizes:", live); for (int k = g_nmem - 8; k < g_nmem; k++) printf(" %zu", g_mem[k].n); printf("\n"); (void)sz; } abort(); }
  n = (n + 4095) & ~4095UL; void *p = aligned_alloc(4096, n); memset(p, 0, n);
  g_mem[g_nmem].p = p; g_mem[g_nmem].bus = g_next_bus; g_mem[g_nmem].n = n; g_nmem++; *bus = g_next_bus; g_next_bus += n; return p; }
static void p_dma_free(size_t n, void *p, uint64_t b) { (void)n; (void)b; for (int i = 0; i < g_nmem; i++) if (g_mem[i].p == p) { g_mem[i].p = 0; free(p); return; } }
static u8 *host(u64 bus) { for (int i = 0; i < g_nmem; i++) if (g_mem[i].p && bus >= g_mem[i].bus && bus < g_mem[i].bus + g_mem[i].n) return g_mem[i].p + (bus - g_mem[i].bus); return 0; }
static uint64_t p_dma_map(void *p, size_t n, int d) { (void)n; (void)d; for (int i = 0; i < g_nmem; i++) if (g_mem[i].p && (u8 *)p >= g_mem[i].p && (u8 *)p < g_mem[i].p + g_mem[i].n) return g_mem[i].bus + ((u8 *)p - g_mem[i].p); return 0; }

/* ---- MHI rings, device side ---- */
struct ctx { u32 a, b, c; u64 rbase, rlen, rp, wp; } __attribute__((packed));
static struct ctx *chctx(int ch) { return (struct ctx *)host(R64(0x58)) + ch; }
static struct ctx *erctx(int r) { return (struct ctx *)host(R64(0x60)) + r; }
static struct ctx *cmdctx(void) { return (struct ctx *)host(R64(0x68)); }
static void ev_post(int ring, u64 ptr, u32 d0, u32 d1) {
  struct ctx *e = erctx(ring); u32 *el = (u32 *)host(e->rp); memcpy(el, &ptr, 8); el[2] = d0; el[3] = d1;
  e->rp += 16; if (e->rp >= e->rbase + e->rlen) e->rp = e->rbase; lx_irq_raise(101 + ring); }

/* ---- QRTR node 7 with the firmware service ---- */
#define NODE 7
#define SVC_PORT 0x10
struct dl { u8 *d; size_t n; struct dl *next; }; static struct dl *g_dl_head, *g_dl_tail;
static void dl_push(const void *d, size_t n) { struct dl *q = malloc(sizeof *q); q->d = malloc(n); memcpy(q->d, d, n); q->n = n; q->next = 0; if (g_dl_tail) g_dl_tail->next = q; else g_dl_head = q; g_dl_tail = q; }
struct h1 { u32 ver, type, sn, sp, conf, size, dn, dp; };
static void qrtr_send(u32 type, u32 sp, u32 dn, u32 dp, const void *pl, size_t n) {
  u8 buf[2048]; struct h1 h = { 1, type, NODE, sp, 0, (u32)n, dn, dp }; memcpy(buf, &h, sizeof h); memcpy(buf + sizeof h, pl, n); dl_push(buf, sizeof h + ((n + 3) & ~3UL)); }
static void ctrl(u32 cmd, u32 svc, u32 inst, u32 node, u32 port) { u32 p[5] = { cmd, svc, inst, node, port }; qrtr_send(cmd == 4 ? 4 : cmd, 0xfffffffe, 0xffffffff, 0xfffffffe, p, sizeof p); }
static u32 g_host_port;
static int g_service_silent;   /* the chip came back from the power cycle without a service (PEACH=dead) */
/* The global reset: the chip is back in its boot ROM, what it had queued for the host is gone. */
static void dev_reset(void) {
  u32 bhi = *R(0x28), bhie = *R(0x2c);
  if (g_stage) g_resets++;
  while (g_dl_head) { struct dl *q = g_dl_head; g_dl_head = q->next; free(q->d); free(q); }
  g_dl_tail = 0; g_stage = 0; g_cal_due = g_self_due = g_ready_due = 0;
  *R(0x48) = 0; *R(0x38) = 0; *R(bhi + 0x18) = 0; *R(bhi + 0x28) = 0; *R(bhi + 0x2c) = 0; *R(bhie + 0x3c) = 0; *R(bhie + 0x44) = 0;
  for (u32 r = 0x58; r <= 0x6c; r += 4) *R(r) = 0;   /* the MHI context addresses: the boot loader wants them again before the image */
  if (g_peach == P_DEAD && g_mode_off) g_service_silent = 1; }
static void qmi_ind(u16 msg, const struct qmi_elem_info *ei, const void *st) {
  size_t len = 2048; void *b = qmi_encode_message(QMI_INDICATION, msg, &len, 0, ei, st); qrtr_send(1, SVC_PORT, 1, g_host_port, b, len); lx_free(b); }
static void qmi_resp(u8 *req, size_t n) {
  (void)n; u16 txn = req[1] | req[2] << 8, msg = req[3] | req[4] << 8;
  u8 r[16]; r[0] = 2; r[1] = txn; r[2] = txn >> 8; r[3] = msg; r[4] = msg >> 8; r[5] = 7; r[6] = 0; r[7] = 2; r[8] = 4; r[9] = 0; r[10] = r[11] = r[12] = r[13] = 0;
  qrtr_send(1, SVC_PORT, 1, g_host_port, r, 14);
  if (getenv("V")) printf("[sim %llu] qmi request 0x%x answered\n", (unsigned long long)g_ms, msg);
  if (msg == 0x20) {   /* indication register: firmware asks for memory */
    static struct qmi_wlanfw_request_mem_ind_msg_v01 m; memset(&m, 0, sizeof m); m.mem_seg_len = 1; m.mem_seg[0].size = 0x100000; m.mem_seg[0].type = 1;
    if (g_peach) { m.mem_seg_len = 2; m.mem_seg[1].size = 0x180000; m.mem_seg[1].type = CALDB_MEM_REGION_TYPE; }
    g_sessions++;
    qmi_ind(QMI_WLANFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei, &m); }
  if (msg == 0x36) {   /* memory given: firmware is ready for capabilities */
    if (g_peach && g_mem_sessions < 4) {
      static struct qmi_wlanfw_respond_mem_req_msg_v01 r; memset(&r, 0, sizeof r);
      if (qmi_decode_message(req, n, qmi_wlanfw_respond_mem_req_msg_v01_ei, &r) >= 0) { g_mem_addr[g_mem_sessions][0] = r.mem_seg[0].addr; g_mem_addr[g_mem_sessions][1] = r.mem_seg[1].addr; }
      g_mem_sessions++; }
    g_mem_requests++;
    static struct { u8 dummy; } e; (void)e; u8 b[7] = { 4, 0, 0, 0x37, 0, 0, 0 }; qrtr_send(1, SVC_PORT, 1, g_host_port, b, 7); }
  if (!g_peach) return;
  if (msg == 0x34) {   /* host capability: what it says about the calibration */
    static struct qmi_wlanfw_host_cap_req_msg_v01 c; memset(&c, 0, sizeof c);
    if (g_sessions >= 0 && g_sessions < 4 && qmi_decode_message(req, n, qmi_wlanfw_host_cap_req_msg_v01_ei, &c) >= 0) g_cal_done[g_sessions & 3] = c.cal_done_valid << 8 | c.cal_done; }
  if (msg == 0x5a) g_ready_due = g_ms + (g_peach == P_LATE && g_mode_off ? 5000 : 100);   /* the last file of this session: FW_READY */
  if (g_peach == P_STUCK && msg == 0x5a && g_mode_off && g_resets == 0) g_ready_due = 0;
  if (msg == 0x22) {   /* wlan mode */
    struct qmi_wlanfw_wlan_mode_req_msg_v01 m; memset(&m, 0, sizeof m);
    if (qmi_decode_message(req, n, qmi_wlanfw_wlan_mode_req_msg_v01_ei, &m) < 0) return;
    if (m.mode == 7) { g_calibrations++; g_cal_due = g_ms + 3000; }
    else if (m.mode == ATH12K_FIRMWARE_MODE_OFF) { g_mode_off++; if (g_peach == P_SELF) g_self_due = g_ms + 500; if (g_peach == P_LATE || g_peach == P_STUCK) g_self_due = g_ms + 1500; }
    else g_mission++; } }

static int g_cmd_rp_seen, g_ul_seen;
static char *g_sp_top; static unsigned long g_sp_peak;   /* how deep the stack goes from main, for the kernel thread's 16 KiB */
static void sp_note(void) { char here; if (g_sp_top && (unsigned long)(g_sp_top - &here) > g_sp_peak) g_sp_peak = g_sp_top - &here; }
static void dev_poll(void) {
  sp_note();
  u32 bhi = *R(0x28), bhie = *R(0x2c);
  if (g_global_reset) { g_global_reset = 0; dev_reset(); }
  if (*R(0x38) & 2) *R(0x38) &= ~2u;       /* a reset request completes at once */
  if (g_peach && g_stage >= 4) {           /* the firmware's own time line */
    if (g_cal_due && g_ms >= g_cal_due) { g_cal_due = 0; u8 b[7] = { 4, 0, 0, 0x3e, 0, 0, 0 }; qrtr_send(1, SVC_PORT, 1, g_host_port, b, 7); }
    if (g_ready_due && g_ms >= g_ready_due) { g_ready_due = 0; g_fw_ready++; u8 b[7] = { 4, 0, 0, 0x38, 0, 0, 0 }; qrtr_send(1, SVC_PORT, 1, g_host_port, b, 7); }
    if (g_self_due && g_ms >= g_self_due) { g_self_due = 0; ctrl(5, 0x45, 0x101, NODE, SVC_PORT); ctrl(4, 0x45, 0x101, NODE, SVC_PORT); } }
  if (g_stage == 0 && *R(bhi + 0x18) && g_peach && !R64(0x58)) {   /* what sheng saw: an image offered right after a reset with no contexts is refused */
    if (!g_refused++) printf("device: the boot loader refuses an image offered without MHI contexts\n"); *R(bhi + 0x18) = 0; *R(bhi + 0x2c) = 3u << 30; }
  if (g_stage == 0 && *R(bhi + 0x18)) {          /* PBL took the boot loader */
    *R(bhi + 0x2c) = 0x80005eed; *R(bhi + 0x28) = 1; g_stage = 1; g_images++; }
  if (g_stage == 1) { *R(0x48) = 1; g_stage = 2; }
  if (g_stage == 2 && ((*R(0x38) >> 8) & 0xff) == 2) { *R(0x48) = 1 | 2 << 8; g_stage = 3; ev_post(0, 0, 2u << 24, 0x20u << 16); ev_post(0, 0, 1u << 24, 0x40u << 16); }
  if (g_stage == 3 && *R(bhie + 0x3c)) {
    *R(bhie + 0x44) = (*R(bhie + 0x3c) & 0x3fffffff) | 2u << 30; *R(bhi + 0x28) = 2; g_stage = 4; ev_post(0, 0, 2u << 24, 0x40u << 16); }
  if (g_stage < 4) return;
  if (!host(R64(0x58)) || !host(R64(0x60)) || !host(R64(0x68))) return;   /* the host has freed the contexts: nothing moves before the reset */
  /* command ring */
  struct ctx *c = cmdctx();
  while (c->rp != c->wp) {
    u32 *tre = (u32 *)host(c->rp); u32 type = (tre[3] >> 16) & 0xff, ch = tre[3] >> 24; u64 at = c->rp;
    if (type == 0x12) { chctx(ch)->a = (chctx(ch)->a & ~0xffu) | 2; }
    else if (type == 0x10) { chctx(ch)->a = (chctx(ch)->a & ~0xffu) | 1; }
    ev_post(0, at, 1u << 24, 0x21u << 16);
    c->rp += 16; if (c->rp >= c->rbase + c->rlen) c->rp = c->rbase; }
  /* host to device channel 20 */
  struct ctx *u = chctx(20);
  while (u->rbase && u->rp != u->wp) {
    u32 *tre = (u32 *)host(u->rp); u64 ptr; memcpy(&ptr, tre, 8); u32 len = tre[2] & 0xffff; u64 at = u->rp;
    u8 *pl = host(ptr); const struct h1 *h = (void *)pl;
    if (h->type == 2) { ctrl(2, 0, 0, 0, 0); /* answered: our hello */ if (!g_service_silent) ctrl(4, 0x45, 0x101, NODE, SVC_PORT); }
    else if (h->type == 1 && h->dp == SVC_PORT) { g_host_port = h->sp; qmi_resp(pl + sizeof *h, h->size); }
    else if (h->type == 10) { /* lookup goes to the host's own name service, not here */ }
    u->rp += 16; if (u->rp >= u->rbase + u->rlen) u->rp = u->rbase;
    ev_post(chctx(20)->c, at, 2u << 24 | len, 20u << 24 | 0x22u << 16); }
  /* device to host channel 21 */
  struct ctx *d = chctx(21);
  while (d->rbase && g_dl_head && d->rp != d->wp) {
    u32 *tre = (u32 *)host(d->rp); u64 ptr; memcpy(&ptr, tre, 8); u64 at = d->rp; struct dl *q = g_dl_head;
    memcpy(host(ptr), q->d, q->n); g_dl_head = q->next; if (!g_dl_head) g_dl_tail = 0;
    d->rp += 16; if (d->rp >= d->rbase + d->rlen) d->rp = d->rbase;
    ev_post(chctx(21)->c, at, 2u << 24 | (u32)q->n, 21u << 24 | 0x22u << 16); free(q->d); free(q); }
  (void)g_cmd_rp_seen; (void)g_ul_seen;
}
static u64 p_now(void) { return g_ms; }
static void p_sleep(unsigned ms) { g_ms += ms; }
static void p_log(int l, const char *s) {
  sp_note();
  if (getenv("V") || l <= 4) printf("[%d %llu] %s\n", l, (unsigned long long)g_ms, s);
  if (strstr(s, "no FW_READY after the mode OFF")) g_restart_logged++;
  if (strstr(s, "FW_READY again")) g_cancel_logged++;
  if (strstr(s, "service is back by itself")) g_back_logged++; }
static void p_halt(void) { abort(); }
static void *p_alloc(size_t n) { return malloc(n); }
static void p_free(void *p) { free(p); }
static void p_dma_sync(uint64_t b, size_t n, int d) { (void)b; (void)n; (void)d; }
extern const u8 *g_fw_amss; extern size_t g_fw_amss_len;
static u8 *g_blob; static size_t g_blob_n = 6 * 1024 * 1024;
static int p_fw(const char *n, const uint8_t **d, size_t *s) {
  if (strstr(n, "amss.bin") || strstr(n, "m3.bin")) { if (!g_blob) { g_blob = malloc(g_blob_n); for (size_t i = 0; i < g_blob_n; i++) g_blob[i] = (u8)(i * 7 + 3); }
    *d = g_blob; *s = strstr(n, "amss") ? g_blob_n : 200000; return 0; }
  if (g_peach && (strstr(n, "regdb.bin") || strstr(n, "board.bin") || strstr(n, "aux_ucode.bin") || strstr(n, "tmel_peach_20.elf"))) {
    static u8 small[8192]; memset(small, 0x5a, sizeof small); memcpy(small, "\177ELF", strstr(n, "regdb") ? 0 : 4); *d = small; *s = sizeof small; return 0; }
  return -1; }
/* The board's options: the default of each, but for the case that turns the calibration off. */
int nw_board_option(const char *name, int fallback) { return g_peach == P_NOCAL && !strcmp(name, "cbc") ? 0 : fallback; }
static const struct lx_platform plat = { p_now, p_sleep, dev_poll, p_log, p_halt, p_alloc, p_free, p_dma_alloc, p_dma_free, p_dma_map, p_dma_sync, p_fw };
static int cfg_read(int off, int size, u32 *v) { static u32 msi[4] = { 0x00809005, 0, 0, 0 }; (void)size;
  if (off == 2) *v = g_peach ? 0x110e : 0x1107; else if (off == 0x2c) *v = g_peach ? 0x110e17cb : 0x110717cb; else if (off >= 0x50 && off < 0x60) *v = msi[(off - 0x50) / 4]; else *v = 0; return 0; }
static int cfg_write(int off, int size, u32 v) { (void)off; (void)size; (void)v; return 0; }
static int msi_target(u32 *lo, u32 *hi, u32 *data) { *lo = 0x1000; *hi = 0; *data = 0; return 0; }
static struct nw_board board;
void lx_irq_raise(unsigned int irq);

static u32 g_win;
static void trace(volatile void *a, u32 v) {
  if ((u8 *)a == g_bar + 0x3008 && (v & 1)) g_global_reset = 1;   /* PCIE_SOC_GLOBAL_RESET: the device model takes it at its next look */
  if (!getenv("T")) return; unsigned long off = (unsigned long)((u8 *)a - g_bar); if ((u8 *)a < g_bar || off >= 0x200000) return;
  if (off == 0x310c || off == 0x3278) { g_win = v & 0x3f; printf("[w] window <- %02x\n", v); return; }
  if (off >= 0x80000) printf("[w] chip %07lx <- %08x\n", ((unsigned long)g_win << 19) | (off - 0x80000), v); else printf("[w] bar %04lx <- %08x\n", off, v); }
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s: %s\n", getenv("PEACH"), #c); bad++; } } while (0)
/* What each case must have left behind. */
static int peach_verdict(void) {
  int bad = 0, restarts = g_peach == P_HOST || g_peach == P_STUCK || g_peach == P_DEAD;
  printf("peach %s: sessions %d, boot images %d, resets of a running chip %d, FW_READY %d, calibrations %d, mode OFF %d, mission mode %d, host restarts logged %d\n",
         getenv("PEACH"), g_sessions, g_images, g_resets, g_fw_ready, g_calibrations, g_mode_off, g_mission, g_restart_logged);
  printf("stack from main: %lu bytes at most\n", g_sp_peak);
  EXPECT(g_restart_logged == restarts);
  EXPECT(g_refused == 0);
  EXPECT(g_images == 1 + restarts);
  EXPECT(g_resets >= restarts);
  if (g_peach == P_NOCAL) { EXPECT(g_sessions == 1); EXPECT(g_calibrations == 0); EXPECT(g_mode_off == 0); EXPECT(g_mission >= 1); EXPECT(g_fw_ready == 1); return bad; }
  EXPECT(g_calibrations == 1); EXPECT(g_mode_off == 1);
  if (g_peach == P_DEAD) { EXPECT(g_sessions == 1); EXPECT(g_mission == 0); EXPECT(g_fw_ready == 1); return bad; }
  EXPECT(g_sessions == 2 + (g_peach == P_STUCK)); EXPECT(g_mem_requests == g_sessions);
  EXPECT(g_mission >= 1);                                  /* the second FW_READY started mission mode */
  EXPECT(g_fw_ready == 2);
  EXPECT(g_cal_done[1] == 0x100);                          /* first session: valid, not calibrated */
  EXPECT(g_cal_done[2] == 0x101);                          /* then the firmware is told it is */
  EXPECT(g_mem_addr[0][0] && g_mem_addr[0][0] == g_mem_addr[1][0]);   /* the memory it was given stays, its calibration data with it */
  EXPECT(g_mem_addr[0][1] && g_mem_addr[0][1] == g_mem_addr[1][1]);
  EXPECT(g_cancel_logged == 1 || restarts);
  EXPECT(g_back_logged == (g_peach == P_SELF || g_peach == P_LATE || g_peach == P_STUCK));
  return bad; }

int main(void) {
  char top; g_sp_top = &top;
  extern void (*lx_mmio_trace)(volatile void *, u32); lx_mmio_trace = trace;
  setvbuf(stdout, 0, _IONBF, 0);
  { const char *c = getenv("PEACH"); static const char *const names[] = { "", "host", "self", "late", "stuck", "dead", "nocal" };
    if (c) { for (int i = 1; i < 7; i++) if (!strcmp(c, names[i])) g_peach = i; if (!g_peach) { printf("PEACH=%s: no such case\n", c); return 2; } } }
  g_bar = calloc(1, 0x200000); *(unsigned *)(g_bar + 0x80000) = 0x200;
  *R(0) = 0x200000; *R(8) = 0x01000000; *R(0x18) = CHDBOFF_V; *R(0x20) = ERDBOFF_V; *R(0x28) = BHIOFF_V; *R(0x2c) = BHIEOFF_V; *R(BHIOFF_V + 0x28) = 0;
  board = (struct nw_board){ .bar = g_bar, .bar_len = 0x200000, .device_id = g_peach ? 0x110e : 0x1107, .msi_cap = 0x50, .irq_base = 100, .cfg_read = cfg_read, .cfg_write = cfg_write, .msi_target = msi_target };
  /* the boot ROM clears the reset bit once the host asks for a reset; MHICTRL.RESET is bit 1 */
  int r = nw_wifi_start(&plat, &board);
  printf("start: %d stage=%u dev-stage=%d\n", r, nw_wifi_stats.stage, g_stage);
  for (int i = 0; i < 40000 && g_ms < 200000; i++) { nw_wifi_step(); lx_idle(1); }
  printf("after run: stage=%u scans=%u err=%d dev-stage=%d time=%llu ms\n", nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.last_err, g_stage, (unsigned long long)g_ms);
  if (g_peach) { int bad = peach_verdict(); if (!bad) printf("PASS peach %s\n", getenv("PEACH")); fflush(stdout); return bad != 0; }
  { extern unsigned long lx_stack_peak, lx_depth_peak; printf("stack peak %lu bytes at pump depth %lu\n", lx_stack_peak, lx_depth_peak); }
  for (int k = 0; k < g_nmem; k++) if (g_mem[k].p && g_mem[k].n >= 65536) printf("SIM: live %zu KiB\n", g_mem[k].n / 1024);
  printf("SIM: dma allocated, frees not counted: %zu KiB\n", g_peak / 1024);
  return 0;
}
