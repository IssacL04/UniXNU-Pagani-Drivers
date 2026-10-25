// SPDX-License-Identifier: BSD-2-Clause-Patent
// TouchComm wire values: OnePlus synaptics_hbp/tcm; implementation is independent of Linux.
#include "S3910.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static const uint8_t format[] = {0x18,8, 1, 6,8, 7,8, 8,16, 9,16, 3,0};

static int message(struct s3910 *s) {
    uint8_t head[4], body[S3910_PAYLOAD_MAX+7];
    int rc = s->bus.read(s->bus.context, head, sizeof head);
    if (rc) return rc;
    // shortcut: TouchComm v1 only; add v2 after a captured identification packet requires it.
    if (head[0] != 0xa5) return -71;
    s->code = head[1]; s->length = le16(head+2);
    if (s->length > S3910_PAYLOAD_MAX) {
        // A queued HBP raw frame can precede DISABLE_REPORT's response after reset.
        if (s->ready || s->code!=0x23) return -90;
        unsigned left=s->length;
        while (left) {
            unsigned size=left>S3910_PAYLOAD_MAX?S3910_PAYLOAD_MAX:left;
            rc=s->bus.read(s->bus.context,body,size+2+(size==left?5:0));
            if (rc) return rc;
            if (body[0]!=0xa5 || body[1]!=3 || (size==left && body[size+2]!=0x5a)) return -71;
            left-=size;
        }
        return 0;
    }
    if (!s->length) return 0;
    // The vendor detection reads EOM, optional CRC/RC and trailing EOM together.
    rc = s->bus.read(s->bus.context, body, s->length+7);
    if (rc) return rc;
    if (body[0] != 0xa5 || body[1] != 3 || body[s->length+2] != 0x5a) return -71;
    if (s->code==0x10) s->crc=le16(body+s->length+3)!=0x5a5a;
    memcpy(s->payload, body+2, s->length);
    return 0;
}

static int command(struct s3910 *s, uint8_t cmd, const uint8_t *data, unsigned length) {
    uint8_t packet[S3910_PAYLOAD_MAX+5];
    if (length > S3910_PAYLOAD_MAX || (length && !data) ||
        (s->max_write && length+3+2*s->crc > s->max_write)) return -90;
    packet[0]=cmd; packet[1]=(uint8_t)length; packet[2]=(uint8_t)(length>>8);
    if (length) memcpy(packet+3,data,length);
    unsigned size=length+3;
    if (s->crc) {
        uint16_t crc=0xffff;
        for (unsigned i=0;i<size;++i) {
            crc^=(uint16_t)packet[i]<<8;
            for (unsigned b=0;b<8;++b) crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
        }
        packet[size++]=(uint8_t)crc; packet[size++]=(uint8_t)(crc>>8);
    }
    int rc=s->bus.write(s->bus.context,packet,size);
    if (rc) return rc;
    for (unsigned n=0;n<100;++n) {
        s->bus.sleep_ms(s->bus.context,2);
        rc=message(s); if (rc) return rc;
        if (s->code==1 || (cmd==2 && s->code==0x10)) return 0;
        if (s->code==0x10) return -104; // Controller restarted during a command.
        if (s->code>=0xfe || (s->code && s->code<0x10 && s->code!=4 && s->code!=7)) return -5;
    }
    return -110;
}

int s3910_start(struct s3910 *s) {
    if (!s || !s->bus.read || !s->bus.write || !s->bus.sleep_ms) return -22;
    s->ready=0; s->max_write=0; s->crc=0;
    int rc=message(s);
    if (rc) return rc;
    if (s->code!=0x10) {rc=command(s,2,0,0); if (rc) return rc;}
    if (s->length<24) return -71;
    s->mode=s->payload[1]; s->max_write=le16(s->payload+22);
    if (s->mode!=1) return -19;
    uint8_t report=0x23;
    rc=command(s,6,&report,1); if (rc) return rc;
    rc=command(s,0x20,0,0); if (rc) return rc;
    if (s->length<38 || le16(s->payload+2)!=0) return -71;
    s->config_size=le16(s->payload+12);
    s->max_x=le16(s->payload+32); s->max_y=le16(s->payload+34);
    if (!s->max_x || !s->max_y || !le16(s->payload+36) || le16(s->payload+36)>S3910_CONTACTS ||
        s->config_size<sizeof format || s->config_size>S3910_PAYLOAD_MAX) return -71;
    report=0x11;
    rc=command(s,6,&report,1); if (rc) return rc;
    // The vendor disable_hbp path disables raw frames and selects on-controller LBP (2).
    const uint8_t lbp[]={0x11,2,0};
    rc=command(s,0x24,lbp,sizeof lbp); if (rc) return rc;
    const uint8_t key=0x11;
    rc=command(s,0x23,&key,1); if (rc) return rc;
    if (s->length!=2 || le16(s->payload)!=2) return -71;
    uint8_t config[S3910_PAYLOAD_MAX]={0}; memcpy(config,format,sizeof format);
    rc=command(s,0x26,config,s->config_size); if (rc) return rc;
    rc=command(s,0x25,0,0); if (rc) return rc;
    if (s->length<sizeof format || memcmp(s->payload,format,sizeof format)) return -71;
    report=0x11; rc=command(s,5,&report,1); if (rc) return rc;
    s->ready=1; return 0;
}

int s3910_decode(const uint8_t *p,size_t length,uint16_t max_x,uint16_t max_y,struct s3910_frame *out) {
    if (!p || !out || !max_x || !max_y || !length || p[0]>S3910_CONTACTS || length!=1u+6u*p[0]) return -71;
    struct s3910_frame frame; memset(&frame,0,sizeof frame); uint16_t seen=0;
    for (unsigned n=0;n<p[0];++n) {
        const uint8_t *c=p+1+n*6; unsigned id=c[0];
        if (id>=S3910_CONTACTS || (seen&(1u<<id))) return -71;
        seen|=(uint16_t)(1u<<id);
        uint16_t x=le16(c+2),y=le16(c+4);
        if (x>max_x || y>max_y) return -71;
        // Finger and glove classifications are contacts; palms never become fingers.
        if (c[1]!=1 && c[1]!=2) continue;
        frame.contact[id].active=1;
        frame.contact[id].x=(uint16_t)((uint32_t)x*32767/max_x);
        frame.contact[id].y=(uint16_t)((uint32_t)y*32767/max_y);
    }
    *out=frame; return 0;
}

int s3910_poll(struct s3910 *s,struct s3910_frame *out) {
    if (!s || !out || !s->ready) return -19;
    int rc=message(s); if (rc) return rc;
    if (s->code==0x10) {s->ready=0; return -104;}
    if (s->code!=0x11) return 0;
    rc=s3910_decode(s->payload,s->length,s->max_x,s->max_y,out);
    return rc?rc:1;
}

void s3910_hid_report(const struct s3910_frame *frame,const struct s3910_frame *previous,uint8_t report[62]) {
    memset(report,0,62); report[0]=3; unsigned count=0;
    for (unsigned id=0;id<S3910_CONTACTS;++id) {
        const struct s3910_contact *c=&frame->contact[id];
        if (!c->active && !previous->contact[id].active) continue;
        const struct s3910_contact *position=c->active?c:&previous->contact[id];
        uint8_t *r=report+1+6*count++;
        r[0]=c->active?3:2; r[1]=(uint8_t)id;
        r[2]=(uint8_t)position->x; r[3]=(uint8_t)(position->x>>8);
        r[4]=(uint8_t)position->y; r[5]=(uint8_t)(position->y>>8);
    }
    report[61]=(uint8_t)count;
}
