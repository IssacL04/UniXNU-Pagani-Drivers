// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include "Buttons.h"
#include "S3910.h"
#include "Reports.h"
#include <GeniSpi.h>
#include <PeripheralIO.h>
#include "../../BootProgress/PeripheralStatus.h"

static unsigned spiReads,spiWrites,spiSnapshots,dmaSnapshots,readSnapshots,statusReads;
static int spiResult;
static uint8_t spiRxByte=0xa5;
static uint32_t spiSnapshot[6];
static uint32_t dmaSnapshot[4];
static uint32_t readSnapshot[7];
struct PeripheralWindow {
    uint32_t values[2]={1,0};unsigned reads=0;
    static uint32_t read(void *ctx,uint32_t reg) {
        assert(reg==4);auto &p=*static_cast<PeripheralWindow *>(ctx);
        return p.values[p.reads++?1:0];
    }
};
static uint32_t spiStatus(void *,uint32_t reg) {assert(reg==SE_GENI_STATUS);++statusReads;return 0x40;}
void boot_progress_touch_read(uint32_t length,uint32_t head,uint32_t pins,uint32_t dma,
                              uint32_t state,uint32_t txin,uint32_t rxin) {
    const uint32_t values[]={length,head,pins,dma,state,txin,rxin};
    memcpy(readSnapshot,values,sizeof values);++readSnapshots;
}
void boot_progress_touch_dma(uint32_t tx,uint32_t rx,uint32_t txin,uint32_t rxin) {
    const uint32_t values[]={tx,rx,txin,rxin};memcpy(dmaSnapshot,values,sizeof values);++dmaSnapshots;
}
void boot_progress_touch_spi(uint32_t fw,uint32_t disabled,uint32_t state,uint32_t irq,uint32_t tx,uint32_t rx) {
    const uint32_t values[]={fw,disabled,state,irq,tx,rx};
    memcpy(spiSnapshot,values,sizeof values);++spiSnapshots;
}
static int touchTransfer(geni_spi *spi,const uint8_t *tx,uint8_t *rx,size_t size,int keep) {
    assert(tx && size && !keep && !spi->snap_valid);
    if (rx) {
        for (size_t i=0;i<size;++i) assert(tx[i]==0xff);
        memset(rx,spiRxByte,size);++spiReads;
    } else ++spiWrites;
    if (spiResult) {
        spi->snap_valid=1;spi->snap_status=1;spi->snap_irq=0x40000000;
        spi->snap_txfs=1;spi->snap_rxfs=0;
        spi->snap_dma_tx=4;spi->snap_dma_rx=0;spi->snap_dma_txin=2;spi->snap_dma_rxin=1;
    }
    spi->last_tx_in=spi->use_dma?(uint32_t)size:0;
    spi->last_rx_in=spi->use_dma && rx?(uint32_t)size:0;
    return spiResult;
}
struct PaganiInput {
    geni_spi spi={};
    bool dmaExposed=false,firstReadCaptured=false;
    PeripheralWindow pins[6];
    PaganiInput() {spi.se.rd=spiStatus;pins[4].values[0]=pins[4].values[1]=3;}
#define geni_spi_transfer touchTransfer
#include "PaganiReadTouch.inc"
#undef geni_spi_transfer
};
static void spiRead() {
    PaganiInput device;uint8_t bytes[S3910_PAYLOAD_MAX+8]={};
    device.spi.info.fw_revision=0x101;device.spi.info.if_disable=1;
    assert(!PaganiInput::readTouch(&device,bytes,4) && bytes[0]==0xa5 && spiReads==1);
    const uint32_t expectedRead[]={4,0xa5a5a5a5,13,0,0x40,0,0};
    assert(readSnapshots==1 && statusReads==1 && !memcmp(readSnapshot,expectedRead,sizeof expectedRead));
    assert(!PaganiInput::readTouch(&device,bytes,sizeof(bytes)-1) && spiReads==2);
    assert(readSnapshots==1 && statusReads==1 && device.pins[5].reads==2);
    assert(!spiSnapshots);
    spiResult=-110;assert(PaganiInput::readTouch(&device,bytes,4)==-110 && spiReads==3);
    const uint32_t expected[]={0x101,1,1,0x40000000,1,0};
    assert(spiSnapshots==1 && !memcmp(spiSnapshot,expected,sizeof expected));
    assert(PaganiInput::readTouch(&device,bytes,sizeof bytes)==-90 && spiReads==3);
    assert(PaganiInput::readTouch(&device,nullptr,4)==-22 && spiReads==3);
    assert(PaganiInput::readTouch(&device,bytes,0)==-22 && spiReads==3);
    assert(spiSnapshots==1);
    assert(PaganiInput::writeTouch(&device,bytes,4)==-110 && spiWrites==1 && spiSnapshots==2);
    spiResult=0;assert(!PaganiInput::readTouch(&device,bytes,4) && !device.spi.snap_valid && spiSnapshots==2);
    assert(!device.dmaExposed && !dmaSnapshots);
    device.spi.use_dma=1;
    assert(!PaganiInput::readTouch(&device,bytes,4) && device.dmaExposed && !dmaSnapshots);
    spiResult=-5;assert(PaganiInput::readTouch(&device,bytes,4)==-5 && dmaSnapshots==1);
    const uint32_t expectedDma[]={4,0,2,1};assert(!memcmp(dmaSnapshot,expectedDma,sizeof expectedDma));
    spiResult=0;
    // Build 54: a successful DMA read containing zeros must retain evidence even though TouchComm rejects it.
    PaganiInput zero;zero.spi.use_dma=1;spiRxByte=0;
    s3910 chip={};chip.bus={&zero,PaganiInput::readTouch,PaganiInput::writeTouch,[](void *,unsigned){}};
    assert(s3910_start(&chip)==-71 && !chip.version && !chip.detail && !strcmp(chip.stage,"header"));
    const uint32_t zeroRead[]={4,0,13,1,0x40,4,4};
    assert(readSnapshots==2 && !memcmp(readSnapshot,zeroRead,sizeof zeroRead));
    assert(zero.firstReadCaptured && zero.dmaExposed && !zero.spi.snap_valid);
    spiRxByte=0xa5;
    PaganiInput shortRead;uint8_t byte=0;
    assert(!PaganiInput::readTouch(&shortRead,&byte,1) && readSnapshot[0]==1 && readSnapshot[1]==0xa5);
    PaganiInput failed;spiResult=-110;unsigned before=statusReads;
    assert(PaganiInput::readTouch(&failed,bytes,4)==-110 && !failed.firstReadCaptured);
    assert(statusReads==before && failed.pins[5].reads==1 && failed.pins[4].reads==0);
    spiResult=0;
}

static void descriptor(const uint8_t *bytes,size_t length,bool touch) {
    unsigned bits[4]={},id=0,size=0,count=0,page=0,usage=0,depth=0;
    std::vector<std::pair<unsigned,unsigned>> applications;
    for (size_t offset=0;offset<length;) {
        uint8_t tag=bytes[offset++];assert(tag!=0xfe);
        unsigned n=tag&3;if (n==3) n=4;
        assert(n<=length-offset);
        unsigned value=0;for (unsigned i=0;i<n;++i) value|=(unsigned)bytes[offset++]<<(8*i);
        switch (tag&0xfc) {
            case 0x04:page=value;break;
            case 0x08:usage=value;break;
            case 0x74:size=value;break;
            case 0x94:count=value;break;
            case 0x84:id=value;assert(id && id<4);break;
            case 0x80:assert(id);bits[id]+=size*count;break;
            case 0xa0:if (!depth) {assert(value==1);applications.emplace_back(page,usage);}++depth;break;
            case 0xc0:assert(depth);--depth;break;
        }
    }
    assert(!depth && bits[1]==64 && bits[2]==8 && bits[3]==(touch?61*8:0));
    assert(applications.size()==(touch?2:1) && applications[0]==std::make_pair(1u,6u));
    if (touch) assert(applications[1]==std::make_pair(13u,4u));
}

struct Tcm {
    std::vector<std::vector<uint8_t>> reads,writes;
    size_t next=0;
    static int read(void *ctx,uint8_t *p,size_t n) {
        auto &m=*static_cast<Tcm *>(ctx);
        if (m.next>=m.reads.size()) return -110;
        const auto &r=m.reads[m.next++];assert(r.size()==n);memcpy(p,r.data(),n);return 0;
    }
    static int write(void *ctx,const uint8_t *p,size_t n) {
        static_cast<Tcm *>(ctx)->writes.emplace_back(p,p+n);return 0;
    }
    static void sleep(void *,unsigned) {}
    void message(uint8_t code,const std::vector<uint8_t> &body={}) {
        reads.push_back({0xa5,code,(uint8_t)body.size(),(uint8_t)(body.size()>>8)});
        if (body.empty()) return;
        std::vector<uint8_t> next{0xa5,3};next.insert(next.end(),body.begin(),body.end());
        next.insert(next.end(),5,0x5a);reads.push_back(next);
    }
    s3910 device() {s3910 d={};d.bus={this,read,write,sleep};return d;}
    void startup(bool corruptReadback=false) {
        std::vector<uint8_t> id(24),app(38);
        id[1]=1;id[23]=2;message(0x10,id);
        app[12]=16;app[32]=0;app[33]=0x4c;app[34]=0;app[35]=0xa5;app[36]=10;
        message(1);message(1,app);message(1);message(1);message(1,{2,0});message(1);
        std::vector<uint8_t> format={0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0};
        if (corruptReadback) format[0]=0;
        message(1,format);if (!corruptReadback) message(1);
    }
};

// Independent table CRC and fixed IDENTIFY bytes check the production bitwise encoder.
static uint8_t headerCrc(const uint8_t *p,unsigned bits) {
    static const uint16_t table[]={0,268,536,788,1072,1340,1576,1828,2144,2412,2680,2932,3152,3420,3656,3908};
    uint16_t r=0xfc;
    for (;bits>8;bits-=8) {
        r^=*p++;
        r=(uint16_t)((r<<4)^table[(r>>4)&15]);r=(uint16_t)((r<<4)^table[(r>>4)&15]);
    }
    unsigned x=*p;
    while (bits--) {if (x&0x80) r^=0x80;x<<=1;r<<=1;if (r&0x100) r^=12;}
    return (r>>2)&63;
}
static uint16_t packetCrc(const std::vector<uint8_t> &bytes) {
    uint16_t crc=0xffff;
    for (auto byte:bytes) {
        uint16_t x=(crc>>8)^byte;x^=x>>4;
        crc=(uint16_t)((crc<<8)^(x<<12)^(x<<5)^x);
    }
    return crc;
}
struct Tcm2 : Tcm {
    static std::vector<uint8_t> header(uint8_t code,unsigned length) {
        std::vector<uint8_t> h={code,(uint8_t)length,(uint8_t)(length>>8),0};
        h[3]=headerCrc(h.data(),26);return h;
    }
    void response(uint8_t code,const std::vector<uint8_t> &body={},unsigned cap=64) {
        reads.push_back(header(code,body.size()));
        for (size_t off=0;off<body.size();) {
            size_t n=body.size()-off;if (n>cap-6) n=cap-6;
            auto packet=header(code,n);packet.insert(packet.end(),body.begin()+off,body.begin()+off+n);
            uint16_t crc=packetCrc(packet);packet.push_back(crc>>8);packet.push_back(crc);
            reads.push_back(packet);off+=n;
        }
    }
    void startup(unsigned maxRead=64,unsigned maxWrite=64) {
        reads.push_back({4,0,0,0x3c});
        std::vector<uint8_t> id(26),app(38);
        id[1]=1;id[22]=(uint8_t)maxWrite;id[23]=(uint8_t)(maxWrite>>8);
        id[24]=(uint8_t)maxRead;id[25]=(uint8_t)(maxRead>>8);response(0x10,id,256);
        app[12]=16;app[33]=0x4c;app[35]=0xa5;app[36]=10;
        response(1);response(1,app,maxRead);response(1);response(1);response(1,{2,0},maxRead);
        for (unsigned off=0;off<16;) {off+=maxWrite-6;response(off<16?7:1);}
        response(1,{0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0},maxRead);response(1);
    }
    void checkPackets() const {
        for (const auto &p:writes) {
            assert(p.size()>=4 && !headerCrc(p.data(),32));
            unsigned length=p[1]+(p[2]<<8);assert(p.size()==length+4+(length?2:0));
            if (length) assert(!packetCrc(p));
        }
    }
};

static void touchV2() {
    Tcm2 bus;bus.startup();auto d=bus.device();assert(!s3910_start(&d) && d.ready && d.version==2);
    assert(d.max_read==64 && d.max_write==64 && bus.next==bus.reads.size());
    assert(bus.writes[0]==std::vector<uint8_t>({2,0,0,0x19}));bus.checkPackets();
    s3910_frame frame={};bus.response(0x11,{1,2,1,0,0x26,0,0x52});
    assert(s3910_poll(&d,&frame)==1 && frame.contact[2].active && bus.writes[bus.writes.size()-2][0]==0x0a);
    bus.checkPackets();
    bus.response(0xff);assert(s3910_poll(&d,&frame)==-5 && !strcmp(d.stage,"reply"));
    Tcm2 chunks;chunks.startup(16,16);auto fragmented=chunks.device();assert(!s3910_start(&fragmented));
    std::vector<uint8_t> config;bool setting=false;
    for (const auto &p:chunks.writes) {
        assert(p.size()<=16);
        if (p[0]==0x26) setting=true;
        if (setting && (p[0]==0x26 || p[0]==1)) config.insert(config.end(),p.begin()+4,p.end()-2);
        if (p[0]==0x25) setting=false;
    }
    assert(config==std::vector<uint8_t>({0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0}));chunks.checkPackets();
    Tcm2 retry;retry.startup();retry.reads.insert(retry.reads.begin()+1,Tcm2::header(8,0));
    auto repeated=retry.device();assert(!s3910_start(&repeated) && retry.writes[0]==retry.writes[1]);retry.checkPackets();
    Tcm2 pending;pending.startup();pending.reads.insert(pending.reads.begin()+1,Tcm2::header(7,0));
    auto delayed=pending.device();assert(!s3910_start(&delayed) && pending.writes[1][0]==0x0a);pending.checkPackets();
    Tcm2 crc;crc.startup();auto damaged=crc.reads[2];damaged.back()^=1;
    crc.reads.insert(crc.reads.begin()+2,damaged);auto recovered=crc.device();assert(!s3910_start(&recovered));
    assert(crc.writes[1][0]==7 && crc.writes[2][0]==8);crc.checkPackets();
    Tcm2 fail;fail.startup();fail.reads[2]=damaged;fail.reads.insert(fail.reads.begin()+2,4,damaged);
    auto rejected=fail.device();assert(s3910_start(&rejected)==-74 && !rejected.ready);
    assert(!strcmp(rejected.stage,"body CRC") && fail.writes.size()==6);
    Tcm2 badHeader;badHeader.startup();badHeader.reads[1][3]^=1;auto broken=badHeader.device();
    assert(s3910_start(&broken)==-74 && !strcmp(broken.stage,"header") && badHeader.writes.size()==1);
    Tcm2 oversized;oversized.reads={{4,0,0,0x3c},Tcm2::header(0x10,513)};auto large=oversized.device();
    assert(s3910_start(&large)==-90 && oversized.writes.size()==1);
    Tcm2 raw;raw.startup();Tcm2 queued;queued.response(0x23,std::vector<uint8_t>(518),64);
    raw.reads.insert(raw.reads.begin()+3,queued.reads.begin(),queued.reads.end());auto drained=raw.device();
    assert(!s3910_start(&drained) && drained.ready && raw.next==raw.reads.size());raw.checkPackets();
    for (bool readLimit:{false,true}) {
        Tcm2 limit;limit.startup();auto &id=limit.reads[2];id.resize(id.size()-2);
        id[readLimit?28:26]=6;uint16_t crc=packetCrc(id);id.push_back(crc>>8);id.push_back(crc);
        auto invalid=limit.device();assert(s3910_start(&invalid)==-90 && !invalid.ready);
        assert(!strcmp(invalid.stage,readLimit?"read limit":"write limit"));
    }
    Tcm floating;floating.reads={{0xff,0xff,0xff,0xff}};auto absent=floating.device();
    assert(s3910_start(&absent)==-71 && !absent.version && floating.writes.empty());
}
struct MMIO {
    std::map<uint32_t,uint32_t> regs;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    bool complete=true,dsi=false,spmi=false;
    unsigned waited=0;
    static uint32_t read(void *ctx,uint32_t reg) {return static_cast<MMIO *>(ctx)->regs[reg];}
    static void write(void *ctx,uint32_t reg,uint32_t value) {
        auto &m=*static_cast<MMIO *>(ctx);m.writes.emplace_back(reg,value);
        if (m.dsi && reg==0x110) m.regs[reg]=(value&0xa222aa02)|(m.regs[reg]&0x55555555&~value);
        else m.regs[reg]=value;
        if (m.dsi && reg==0x90 && m.complete) m.regs[0x110]|=1;
        if (m.spmi && (reg&31)==0) m.regs[reg+8]=m.complete?1:0;
    }
    static void delay(void *ctx,unsigned us) {static_cast<MMIO *>(ctx)->waited+=us;}
    peripheral_io io() {return {this,read,write,delay};}
};
static void touch() {
    Tcm bus;bus.startup();auto d=bus.device();assert(s3910_start(&d)==0 && d.ready);
    assert(d.max_x==19456 && d.max_y==42240 && bus.next==bus.reads.size());
    assert(bus.writes[3]==std::vector<uint8_t>({0x24,3,0,0x11,2,0}));
    assert(bus.writes.back()==std::vector<uint8_t>({5,1,0,0x11}));
    assert(bus.writes[5].size()==19 && bus.writes[5][0]==0x26);
    Tcm bad;bad.startup(true);auto rejected=bad.device();assert(s3910_start(&rejected)==-71 && !rejected.ready);
    assert(bad.writes.back()[0]==0x25); // No touch enable after an unconfirmed format.
    assert(rejected.command==0x25 && !strcmp(rejected.stage,"format") && rejected.detail==0x06010800);
    Tcm crc;crc.startup();crc.reads[1][27]=0x34;crc.reads[1][28]=0x12;
    auto cd=crc.device();assert(s3910_start(&cd)==0 && cd.crc);
    assert(crc.writes[1]==std::vector<uint8_t>({0x20,0,0,0x5a,0x4a}));
    Tcm raw;raw.startup();
    std::vector<uint8_t> first(514,0),last(13,0x5a);first[0]=last[0]=0xa5;first[1]=last[1]=3;
    raw.reads.insert(raw.reads.begin()+2,{{0xa5,0x23,6,2},first,last});
    auto rd=raw.device();assert(!s3910_start(&rd) && rd.ready);
    Tcm oversized;oversized.reads={{0xa5,0x10,1,2}};auto od=oversized.device();
    assert(s3910_start(&od)==-90 && oversized.writes.empty());
    Tcm version;version.reads={{0x10,0,0,0}};auto vd=version.device();assert(s3910_start(&vd)==-71);
    assert(!strcmp(vd.stage,"header") && vd.detail==0x10 && vd.command==0);
    Tcm continuation;continuation.startup();continuation.reads[1][1]=4;
    auto failedBody=continuation.device();assert(s3910_start(&failedBody)==-71);
    assert(!strcmp(failedBody.stage,"continued") && failedBody.detail==0x010004a5 && continuation.writes.empty());
    Tcm app;app.startup();app.reads[4][4]=1;auto failedApp=app.device();assert(s3910_start(&failedApp)==-71);
    assert(failedApp.command==0x20 && !strcmp(failedApp.stage,"app info") && failedApp.detail==1);
    s3910_frame frame={},old={};
    const uint8_t sample[]={3, 2,1,0,0x26,0,0x52, 7,2,0,0x4c,0,0xa5, 9,6,0,0,0,0};
    assert(s3910_decode(sample,sizeof sample,19456,42240,&frame)==0);
    assert(frame.contact[2].active && frame.contact[2].x==16383 && frame.contact[2].y==16284);
    assert(frame.contact[7].active && frame.contact[7].x==32767 && !frame.contact[9].active);
    uint8_t report[62];s3910_hid_report(&frame,&old,report);
    assert(report[0]==3 && report[61]==2 && report[1]==3 && report[2]==2 && report[8]==7);
    old=frame;frame={};frame.contact[0]={1,2,1};s3910_hid_report(&frame,&old,report);
    assert(report[61]==3 && report[1]==3 && report[7]==2 && report[13]==2); // Two explicit releases plus new contact.
    const uint8_t duplicate[]={2,0,1,0,0,0,0,0,1,0,0,0,0};
    assert(s3910_decode(duplicate,sizeof duplicate,10,10,&frame)==-71);
    assert(s3910_decode(sample,sizeof sample-1,19456,42240,&frame)==-71);
    const uint8_t outOfRange[]={1,10,1,0,0,0,0};assert(s3910_decode(outOfRange,7,10,10,&frame)==-71);
    for (unsigned n=0;n<10;++n) old.contact[n]={10,20,1};
    frame={};s3910_hid_report(&frame,&old,report);assert(report[61]==10);
    for (unsigned n=0;n<10;++n) assert(report[1+6*n]==2 && report[2+6*n]==n);
    bus.message(0x10,std::vector<uint8_t>(24));assert(s3910_poll(&d,&frame)==-104 && !d.ready);
}
static void buttons() {
    assert(pagani_buttons(0,1)==0 && pagani_buttons(0x80,1)==2);
    assert(pagani_buttons(0x40,1)==4 && pagani_buttons(0,0)==8 && pagani_buttons(0xc0,0)==14);
    pagani_button_filter f={};
    assert(pagani_buttons_filter(&f,1,2,0)==0);
    assert(pagani_buttons_filter(&f,1,0,16)==0);
    assert(pagani_buttons_filter(&f,1,2,32)==0);
    assert(pagani_buttons_filter(&f,1,2,63)==0);
    assert(pagani_buttons_filter(&f,1,2,64)==2);
    assert(pagani_buttons_filter(&f,1,2,10000)==2); // Long press remains asserted.
    assert(pagani_buttons_filter(&f,0,2,10001)==0); // A bus failure releases it.
    assert(pagani_buttons_filter(&f,1,2,10002)==0);
    assert(pagani_buttons_filter(&f,1,2,10034)==2);
    assert(pagani_buttons_filter(&f,1,0,10035)==2);
    assert(pagani_buttons_filter(&f,1,0,10067)==0);
}
static void buses() {
    MMIO core,cnfg,obs;auto c=core.io(),f=cnfg.io(),o=obs.io();
    core.regs[0]=0x70000000;core.regs[4]=3;core.regs[0x2000]=0x1300;core.regs[0x2008]=0x1300;
    cnfg.regs[0]=2;cnfg.regs[8]=0;assert(spmi_find(&c,&f,0x13,0)==2);
    assert(spmi_find(&c,&f,0x18d,0)==-19);assert(core.writes.empty() && cnfg.writes.empty());
    obs.spmi=true;obs.regs[0x58]=0xc0;uint8_t value=0;
    assert(!spmi_read8(&o,0,2,0x10,&value) && value==0xc0);
    assert(obs.writes.back()==std::make_pair(0x40u,0x08000100u));
    obs.complete=false;assert(spmi_read8(&o,0,2,0x10,&value)==-110 && obs.waited==1000);
    core.regs[0]=0x80000000;assert(spmi_find(&c,&f,0x13,0)==-19);
    MMIO gcc;auto g=gcc.io();gcc.regs[0x52008]=0x80000000;unsigned hz=0,sel=99;
    assert(!sm8750_touch_clock(&g,&hz,&sel) && hz==19200000 && sel==0);
    assert(gcc.regs[0x52008]==0x843c0000);
    gcc.regs[0x183e0]=0x300;assert(sm8750_touch_clock(&g,&hz,&sel)==-95);
    gcc.regs[0x2315c]=0x80000000;assert(sm8750_touch_clock(&g,&hz,&sel)==-110);
}
static void panel() {
    MMIO m;m.dsi=true;m.regs[4]=1;m.regs[0x110]=0x20010102;m.regs[0x3c]=0xa5;m.regs[0x390]=2;m.regs[0x4c]=32;
    auto io=m.io();assert(!dsi_panel_command(&io,0x51,0x599));
    std::vector<uint32_t> fifo;for (auto [reg,value]:m.writes) if (reg==0x17c) fifo.push_back(value);
    assert(fifo==std::vector<uint32_t>({0xc0390003,0xff059951}));
    assert(m.regs[0x3c]==0xa5 && m.regs[0x390]==2 && m.regs[0x4c]==32 && !m.regs[0x15c]);
    assert((m.regs[0x110]&0x10100)==0x10100); // Do not acknowledge unrelated frame completions.
    m.writes.clear();assert(!dsi_panel_command(&io,0x28,0));
    assert(m.writes[2]==std::make_pair(0x17cu,0x80050028u));
    m.writes.clear();assert(dsi_panel_command(&io,0x51,0x10000)==-22 && m.writes.empty());
    m.regs[8]=2;assert(dsi_panel_command(&io,0x51,1)==-16 && m.writes.empty());
    m.regs[8]=0;m.complete=false;assert(dsi_panel_command(&io,0x51,1)==-110);
    assert(m.regs[0x15c]==0x30006); // No reset while the engine may still own the command.
    m.writes.clear();assert(dsi_panel_command(&io,0x51,1)==-19 && m.writes.empty());
}
int main() {
    spiRead();touchV2();
    descriptor(buttonsDescriptor,sizeof buttonsDescriptor,false);descriptor(touchDescriptor,sizeof touchDescriptor,true);
    touch();buttons();buses();panel();puts("PASS: SPI read bytes, TouchComm v1/v2 CRC/retry/fragments/bounds, HID contacts, buttons and MMIO faults");
}
