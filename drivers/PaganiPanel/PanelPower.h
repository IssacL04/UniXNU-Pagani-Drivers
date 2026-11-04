// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AA594_P_7_A0020 panel's own power sequences for PaganiPanel (boot-arg pagani-screenoff bit 2), free of kernel headers so
// Test/panel-power.cpp runs them against a DSI register model and a virtual clock.
//
// Source: the PKX110 live device tree (qcom,mdss_dsi_panel_AA594_P_7_A0020_dsc_cmd; the same in its sa_fhd_120, sa_fhd_90, sa_fhd_60
// and oa_fhd_120 timings). Each packet is the DT's {data type, payload} and the delay the DT asks for after it.
//  - Off (qcom,mdss-dsi-off-command): display off (0x28), 20 ms; enter sleep mode (0x10), 120 ms. Android then turns the panel rails
//    off; here the rails, the reset line, DSI, the PHY, the DSC setup and the scanout stay as the boot loader left them.
//  - Back (the end of qcom,mdss-dsi-on-command, from exit sleep on): exit sleep mode (0x11), 120 ms; the 51 writes the vendor
//    sends after it (page 0x51 and 0x52 tables, identical in all four timings, while the part before exit sleep differs per
//    timing and stays as the boot loader wrote it); page 0; display on (0x29).
// Without the sleep option the sequences are display off (0x28) and display on (0x29) alone, as PaganiPanel always sent them.
//
// pagani_panel_power_step runs at most one sequence at a time and lets each run to its end: a request that changes meanwhile is
// taken up when the last packet's delay passed (MIPI DCS: 120 ms between entering and leaving sleep mode); the one exception is a
// display asked back on between 0x28 and 0x10, which skips the sleep (display on then follows at once). Packets go out through
// the caller's send (PaganiPanel's over dsi_panel_packet_mode), consecutive packets without a delay in one call; a send error is returned as
// it is, with the sequence left where it failed (pagani_panel_tick, PanelBrightness.h: sent again when the packet was not sent, else the end).
#ifndef PAGANI_PANEL_POWER_H
#define PAGANI_PANEL_POWER_H
#include <stdint.h>

struct pagani_panel_packet {uint8_t type,length,waitMs;uint8_t payload[7];};

// The vendor's off sequence; without the sleep option only its first packet.
static const struct pagani_panel_packet pagani_panel_off[2]={{0x05,1,20,{0x28}},{0x05,1,120,{0x10}}};
// Display on alone (the panel was only blanked).
static const struct pagani_panel_packet pagani_panel_display_on[1]={{0x05,1,0,{0x29}}};
// Out of sleep: exit sleep, 120 ms, the vendor's 51 writes after it, display on (crc32 0xb8454479 over the 509 bytes of
// {type, length, delay, payload} per packet, Test/panel-power.cpp).
enum {PAGANI_PANEL_WAKE_PACKETS=53};
static const struct pagani_panel_packet pagani_panel_wake[PAGANI_PANEL_WAKE_PACKETS]={
    {0x05,1,120,{0x11}},{0x39,4,0,{0xff,0x5a,0xa5,0x51}},{0x39,7,0,{0x80,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},
    {0x39,7,0,{0x81,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},{0x39,7,0,{0x82,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},
    {0x39,7,0,{0x83,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},{0x39,7,0,{0x84,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},
    {0x39,7,0,{0x85,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},{0x39,7,0,{0x86,0xd4,0xd4,0xd4,0xd4,0x22,0x22}},
    {0x39,7,0,{0x87,0xd4,0xd4,0x00,0x00,0x22,0x00}},{0x39,7,0,{0xa0,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},
    {0x39,7,0,{0xa1,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},{0x39,7,0,{0xa2,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},
    {0x39,7,0,{0xa3,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},{0x39,7,0,{0xa4,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},
    {0x39,7,0,{0xa5,0xb6,0xb6,0xb6,0xb6,0x22,0x22}},{0x39,7,0,{0xa6,0xb6,0xb6,0xb1,0xb1,0x22,0x22}},
    {0x39,7,0,{0xa7,0xd4,0xd4,0x00,0x00,0x22,0x00}},{0x39,7,0,{0xc0,0x68,0x36,0xbe,0xb4,0x11,0x00}},
    {0x39,7,0,{0xc1,0xaa,0xaa,0xa0,0xa0,0x00,0x00}},{0x39,7,0,{0xc2,0x96,0x96,0x8c,0x8c,0x00,0x00}},
    {0x39,7,0,{0xc3,0x8c,0x82,0x82,0x78,0x00,0x00}},{0x39,7,0,{0xc4,0x78,0x6e,0x6e,0x6e,0x00,0x00}},
    {0x39,7,0,{0xc5,0x64,0x64,0x64,0x64,0x00,0x00}},{0x39,7,0,{0xc6,0x64,0x64,0x64,0x64,0x00,0x00}},
    {0x39,7,0,{0xc7,0x54,0x54,0x00,0x00,0x11,0x00}},{0x39,4,0,{0xff,0x5a,0xa5,0x52}},{0x39,7,0,{0x80,0x64,0x32,0xba,0xb0,0x11,0x00}},
    {0x39,7,0,{0x81,0xa6,0xa6,0x9c,0x9c,0x00,0x00}},{0x39,7,0,{0x82,0x92,0x92,0x88,0x88,0x00,0x00}},
    {0x39,7,0,{0x83,0x88,0x7e,0x7e,0x74,0x00,0x00}},{0x39,7,0,{0x84,0x74,0x6a,0x6a,0x6a,0x00,0x00}},
    {0x39,7,0,{0x85,0x60,0x60,0x5f,0x5f,0x00,0x00}},{0x39,7,0,{0x86,0x5f,0x5f,0x5d,0x5d,0x00,0x00}},
    {0x39,7,0,{0x87,0x52,0x52,0x00,0x00,0x11,0x00}},{0x39,7,0,{0xa0,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xa1,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xa2,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xa3,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xa4,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xa5,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xa6,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xa7,0x54,0x54,0x00,0x00,0x11,0x00}},{0x39,7,0,{0xc0,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xc1,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xc2,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xc3,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xc4,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xc5,0x54,0x54,0x54,0x54,0x11,0x11}},{0x39,7,0,{0xc6,0x54,0x54,0x54,0x54,0x11,0x11}},
    {0x39,7,0,{0xc7,0x54,0x54,0x00,0x00,0x11,0x00}},{0x39,4,0,{0xff,0x5a,0xa5,0x00}},{0x05,1,0,{0x29}}
};

enum {
    PAGANI_PANEL_ON,          // displaying
    PAGANI_PANEL_BLANK,       // display off (0x28)
    PAGANI_PANEL_ASLEEP,      // display off and sleep mode (0x28, 0x10)
};
struct pagani_panel_power {
    uint8_t state;            // PAGANI_PANEL_*: what the panel is in once the running sequence ended
    uint8_t sleep;            // the option: off enters sleep mode, back leaves it
    uint8_t ends;             // the state the running sequence ends in
    uint8_t refresh;          // the panel left sleep mode: the caller writes the brightness again (and clears this)
    uint8_t count,next;       // packets of the running sequence and the next one to send
    const struct pagani_panel_packet *seq;   // the running sequence (0: none)
    uint64_t notBefore;       // ns: the next packet (or the sequence's end) not before this
    uint64_t started;         // ns: the running sequence began
    uint64_t lastNs;          // ns: how long the last finished sequence took (first packet to the end of its last delay)
    uint32_t packets,offs,sleeps,wakes,ons;   // packets sent; sequences finished: blank, asleep, out of sleep, display on
    uint8_t last;             // the state the last finished sequence ended in
};
typedef int (*pagani_panel_send)(void *context,uint8_t type,const uint8_t *payload,unsigned length);

static inline void pagani_panel_power_init(struct pagani_panel_power *p,int sleep)
{
    struct pagani_panel_power z={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    *p=z;p->state=PAGANI_PANEL_ON;p->sleep=sleep?1:0;p->last=PAGANI_PANEL_ON;
}

// One call at `now` (ns) with the power the display asks for (0 off). Returns 0 when nothing failed (*waitNs: the time until the
// next packet or the end of the running sequence, 0 when none runs), else the first send error. *finished (may be null) counts the
// sequences that ended in this call.
static inline int pagani_panel_power_step(struct pagani_panel_power *p,uint64_t now,unsigned want,pagani_panel_send send,
                                          void *context,uint64_t *waitNs,unsigned *finished)
{
    if (waitNs) *waitNs=0;
    if (finished) *finished=0;
    for (unsigned guard=0;guard<4;++guard) {
        if (!p->seq) {
            if (want ? p->state==PAGANI_PANEL_ON : p->state!=PAGANI_PANEL_ON) return 0;
            if (!want) {p->seq=pagani_panel_off;p->count=p->sleep?2:1;p->ends=p->sleep?PAGANI_PANEL_ASLEEP:PAGANI_PANEL_BLANK;}
            else if (p->state==PAGANI_PANEL_ASLEEP) {p->seq=pagani_panel_wake;p->count=PAGANI_PANEL_WAKE_PACKETS;p->ends=PAGANI_PANEL_ON;}
            else {p->seq=pagani_panel_display_on;p->count=1;p->ends=PAGANI_PANEL_ON;}
            p->next=0;p->started=now;
            if (p->notBefore<now) p->notBefore=now;
        }
        while (p->next<p->count) {
            if (now<p->notBefore) {if (waitNs) *waitNs=p->notBefore-now;return 0;}
            // Asked back on before sleep mode was entered: the off sequence stops at the blank display (display on follows).
            if (want && p->seq==pagani_panel_off && p->next==1) {p->count=1;p->ends=PAGANI_PANEL_BLANK;break;}
            const struct pagani_panel_packet *packet=&p->seq[p->next];
            const int rc=send(context,packet->type,packet->payload,packet->length);
            if (rc) return rc;
            ++p->packets;++p->next;
            p->notBefore=now+(uint64_t)packet->waitMs*1000000u;
        }
        if (now<p->notBefore) {if (waitNs) *waitNs=p->notBefore-now;return 0;}
        // The sequence ended (its last delay passed): what the panel is in now; then the request is looked at again.
        if (p->seq==pagani_panel_wake) {++p->wakes;p->refresh=1;}
        else if (p->seq==pagani_panel_display_on) ++p->ons;
        else if (p->ends==PAGANI_PANEL_ASLEEP) ++p->sleeps;
        else ++p->offs;
        p->state=p->ends;p->last=p->ends;p->lastNs=now-p->started;p->seq=0;
        if (finished) ++*finished;
    }
    return 0;
}
#endif
