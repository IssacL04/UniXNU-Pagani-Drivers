// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include "Buttons.h"
#include "S3910.h"
#include <PeripheralIO.h>

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
int main() {touch();buttons();buses();panel();puts("PASS: TouchComm startup/contact lifecycle, button debounce, SPMI/GCC and DSI faults");}
