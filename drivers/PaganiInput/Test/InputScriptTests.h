// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the input script (InputScript.h), included by peripherals.cpp after the key adapter: the timeline as the
// touch device's 8 ms timer runs it, the reports of the synthetic finger (built by the real s3910_hid_report), the
// coordinates against the 405-point keyboard and the edge bands, the key device's side (the real pollButtons), the abort
// rules and the default-off path.
#include <cmath>
#include <string>
#include <tuple>

namespace script_test {
constexpr uint64_t kTick=8;
constexpr uint64_t kStart=240000;
struct Sent { uint64_t t; int down; uint16_t x,y; uint8_t report[62]; };
static void resetLink() {memset(&pagani_script_link,0,sizeof pagani_script_link);}
// The phone around the script: the key device polls each tick before the touch device runs the script, like the two
// driver timers; a Power press turns a dark display on after wakeDelay ms (never when negative).
struct World {
    pagani_script s{};
    uint64_t now=kStart-1000;
    bool displayOff=false,controllerReady=true,hidBound=true,keysAlive=true,sendWorks=true;
    int64_t wakeDelay=-1;
    uint64_t displayOnAt=~0ull;
    uint32_t realKeys=0;
    s3910_frame previous{};
    std::vector<Sent> sent;
    std::vector<std::pair<uint64_t,std::string>> lines;
    std::vector<std::tuple<uint64_t,uint8_t,uint64_t>> keyCalls;
    std::vector<std::pair<uint64_t,uint8_t>> keyReports;     // the key device's report changes
    int lastKeyReport=-1;
    unsigned ioCalls=0;
    World() {resetLink();}
    static int touch(void *ctx,int down,uint16_t x,uint16_t y) {
        auto &w=*static_cast<World *>(ctx);++w.ioCalls;
        if (!w.sendWorks) return 0;
        const s3910_frame frame=pagani_script_contact(down,x,y);   // as PaganiInput::scriptTouch
        Sent e{w.now,down,x,y,{}};s3910_hid_report(&frame,&w.previous,e.report);
        w.previous=frame;w.sent.push_back(e);return 1;
    }
    static void keys(void *ctx,uint8_t bits,uint64_t until) {
        auto &w=*static_cast<World *>(ctx);++w.ioCalls;w.keyCalls.emplace_back(w.now,bits,until);pagani_script_hold(bits,until);
        if ((bits&PAGANI_KEY_POWER) && w.displayOff && w.wakeDelay>=0) w.displayOnAt=w.now+(uint64_t)w.wakeDelay;
    }
    static void log(void *ctx,const char *line) {
        auto &w=*static_cast<World *>(ctx);++w.ioCalls;assert(strlen(line)<PAGANI_SCRIPT_LINE);w.lines.emplace_back(w.now,line);
    }
    pagani_script_io io() {pagani_script_io i={this,touch,keys,log};return i;}
    int arm(uint32_t mask,uint32_t at=PAGANI_SCRIPT_AT_DEFAULT,uint32_t width=1216,uint32_t height=2640) {
        char line[PAGANI_SCRIPT_LINE];const int armed=pagani_script_arm(&s,mask,at,width,height,line,sizeof line);
        if (armed) __atomic_store_n(&pagani_script_link.on,1u,__ATOMIC_RELEASE);
        if (line[0]) lines.emplace_back(now,line);
        return armed;
    }
    pagani_script_input input() {
        pagani_script_input in{};
        in.ms=now;in.display_off=displayOff;in.keys_alive=(uint8_t)pagani_script_keys_alive(now);
        in.controller_ready=controllerReady;in.hid_bound=hidBound;
        in.real_touch=(uint8_t)pagani_script_real_contacts(&s,&previous);in.real_keys=realKeys;in.binding=7;
        return in;
    }
    void tick() {
        if (now>=displayOnAt) displayOff=false;
        if (keysAlive) {
            const uint8_t report=pagani_script_keys(PAGANI_REPORT_RING,now);
            if (report!=lastKeyReport) {keyReports.emplace_back(now,report);lastKeyReport=report;}
        }
        const pagani_script_input in=input();auto i=io();pagani_script_tick(&s,&in,&i);
        if (!s.armed) __atomic_store_n(&pagani_script_link.on,0u,__ATOMIC_RELEASE);   // as PaganiInput::scriptEnd
        now+=kTick;
    }
    void runFor(uint64_t ms) {for (uint64_t end=now+ms;now<end;) tick();}
    void runUntilDone(uint64_t limit=120000) {for (uint64_t end=now+limit;s.armed && now<end;) tick();}
    // Ticks until a line starting with `prefix` was logged (at most 160 s of ticks).
    void runUntil(const std::string &prefix) {for (unsigned i=0;i<20000 && !has(prefix);++i) tick();assert(has(prefix));}
    // The time a line was logged, ~0 when it was not.
    uint64_t at(const std::string &line,unsigned nth=0) const {
        for (auto &l:lines) if (l.second==line && !nth--) return l.first;
        return ~0ull;
    }
    bool has(const std::string &prefix) const {
        for (auto &l:lines) if (l.second.compare(0,prefix.size(),prefix)==0) return true;
        return false;
    }
    std::vector<Sent> framesBetween(uint64_t from,uint64_t to) const {
        std::vector<Sent> out;for (auto &f:sent) if (f.t>=from && f.t<to) out.push_back(f);
        return out;
    }
};
// Independent of pagani_script_logical: the nearest logical value of a pixel position.
static uint16_t logical(double px,double size) {return (uint16_t)std::lround(px*32767.0/size);}
// One synthetic report: a single contact, PAGANI_SCRIPT_CONTACT, tip and confidence while down, confidence alone at the lift.
static void checkReport(const Sent &f) {
    assert(f.report[0]==3 && f.report[61]==1 && f.report[2]==PAGANI_SCRIPT_CONTACT);
    assert(f.report[1]==(f.down?3:2));
    assert((f.report[3]|f.report[4]<<8)==f.x && (f.report[5]|f.report[6]<<8)==f.y);
}
// IOHIDEventDriver's IOFixed position of a logical value (0..32767 over 0..1.0).
static int32_t fixed(uint16_t logicalValue) {return (int32_t)((uint32_t)logicalValue*65536u/32767u);}
// A gesture through HidEvents' edge rule with the driver's 48 px band: down, moves, lift; true when it starts at an edge
// and stays marked through its lift.
static bool edgeGesture(const std::vector<Sent> &frames) {
    bool state[256]={};const uint32_t ex=48*65536u/1216,ey=48*65536u/2640;bool first=true,marked=true,edge=false;
    for (auto &f:frames) {
        const bool changed=first || !f.down;
        const bool e=pagani_touch_edge(state,PAGANI_SCRIPT_CONTACT,changed,f.down!=0,fixed(f.x),fixed(f.y),ex,ey);
        if (first) edge=e;
        else marked&=e==edge;
        first=false;
    }
    return edge && marked;
}

static void defaultOff() {
    // pagani-input-script absent or 0: nothing armed, no line, no state; the script functions do nothing at all.
    World w;assert(!w.arm(0) && w.lines.empty() && !w.s.armed && !__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE));
    pagani_script zero{};assert(!memcmp(&w.s,&zero,sizeof zero));
    for (unsigned i=0;i<40000;++i) {
        w.displayOff=i&1;w.controllerReady=i&2;w.hidBound=i&4;w.realKeys=i;
        if (i%1000==7) w.previous.contact[3]={100,200,1};
        else w.previous=s3910_frame{};
        w.tick();
    }
    assert(w.now>kStart+200000 && !w.ioCalls && w.sent.empty() && w.keyCalls.empty() && w.lines.empty());
    auto io=w.io();pagani_script_abort(&w.s,"x",&io);assert(!w.ioCalls);   // nothing to abort
    assert(pagani_script_real_frame(&w.s,3,kStart+5000,&io)==1 && !w.ioCalls);   // real frames go out as before
    // The key device: every report passes unchanged, nothing is recorded or counted.
    for (unsigned r=0;r<256;++r) assert(pagani_script_keys((uint8_t)r,kStart+r)==r);
    assert(!pagani_script_link.keys_seen && !pagani_script_link.real_keys && !pagani_script_link.primed && !pagani_script_keys_alive(kStart));
    // Values that select nothing, or another screen, log why and arm nothing.
    World other;assert(!other.arm(8) && other.lines.size()==1 && other.lines[0].second=="input script: off, mask 8");
    assert(!other.arm(0x10,240,1216,2640) && other.lines.back().second=="input script: off, mask 10");
    assert(!other.arm(7,240,1080,2400) && other.lines.back().second=="input script: off, screen 1080x2400");
    assert(!other.s.armed && !__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE));
}

static void program() {
    World w;assert(w.arm(PAGANI_SCRIPT_ALL) && w.lines.back().second=="input script: armed 7 at 240s");
    assert(w.s.start==kStart && w.s.count==29 && w.s.mask==7);
    const char *expected[]={"wake","home","unlock","home","spotlight","tap shift","tap a","tap a","tap a","tap 123","tap abc",
                            "tap globe","close","home","","wake","home","up 1","up 2","up 3","down 1","down 2","down 3","",
                            "wake","home","open","close","home"};
    const uint8_t owner[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2};
    for (unsigned i=0;i<w.s.count;++i) assert(!strcmp(w.s.program[i].step->name,expected[i]) && w.s.program[i].script==owner[i]);
    // A single script gets the unlocking prelude itself; masks above bit 2 are ignored.
    World vol;assert(vol.arm(2|8,30) && vol.s.count==10 && vol.s.start==30000 && vol.s.mask==2);
    assert(!strcmp(vol.s.program[2].step->name,"unlock") && vol.s.program[2].script==1 && !strcmp(vol.s.program[9].step->name,"down 3"));
    World cc;assert(cc.arm(4) && cc.s.count==7 && !strcmp(cc.s.program[4].step->name,"open"));
    World two;assert(two.arm(5) && two.s.count==4+10+1+2+3 && two.s.program[14].step->kind==PAGANI_STEP_PAUSE);
}

static void timeline() {
    World w;assert(w.arm(PAGANI_SCRIPT_ALL));
    w.runUntilDone();
    assert(!w.s.armed && !w.s.running && !w.s.down && !__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE));
    const std::vector<std::string> expected={
        "input script: armed 7 at 240s",
        "input script: start 7 at 240000ms keys on",
        "input script: kb wake on","input script: kb home","input script: kb unlock 608,2628>608,1748 250ms","input script: kb home",
        "input script: kb spotlight 608,1320>608,1770 250ms","input script: kb tap shift 78,2181",
        "input script: kb tap a 121,2019","input script: kb tap a 121,2019","input script: kb tap a 121,2019",
        "input script: kb tap 123 78,2343","input script: kb tap abc 78,2343","input script: kb tap globe 128,2495",
        "input script: kb close 608,2628>608,1748 250ms","input script: kb home",
        "input script: vol wake on","input script: vol home","input script: vol up 1","input script: vol up 2",
        "input script: vol up 3","input script: vol down 1","input script: vol down 2","input script: vol down 3",
        "input script: cc wake on","input script: cc home","input script: cc open 1064,12>1064,1200 300ms",
        "input script: cc close 608,2628>608,1748 250ms","input script: cc home"};
    assert(w.lines.size()==expected.size()+1);
    size_t bytes=0;
    for (size_t i=0;i<w.lines.size();++i) {
        if (i<expected.size()) assert(w.lines[i].second==expected[i]);
        assert(w.lines[i].second.size()<=56);
        bytes+=33+w.lines[i].second.size()+1;   // the ramoops record: header, text, newline
    }
    const uint64_t total=w.lines.back().first-kStart;
    assert(w.lines.back().second=="input script: done 7 in "+std::to_string(total)+"ms" && total>40000 && total<50000);
    assert(bytes<3000);   // well inside the 32 KiB events ring
    assert(w.at("input script: start 7 at 240000ms keys on")==kStart && w.at("input script: kb wake on")==kStart);

    // Every synthetic report is one well-formed contact; down and lift alternate per gesture; no frame after the end.
    for (auto &f:w.sent) checkReport(f);
    assert(w.sent.back().down==0 && w.sent.back().t<w.lines.back().first);

    // Taps: down at the key, repeated every 16 ms while held, lifted at 80 ms; the next tap 600 ms after the lift.
    const char *taps[]={"input script: kb tap shift 78,2181","input script: kb tap a 121,2019","input script: kb tap 123 78,2343",
                        "input script: kb tap abc 78,2343","input script: kb tap globe 128,2495"};
    const uint16_t tapXY[][2]={{78,2181},{121,2019},{78,2343},{78,2343},{128,2495}};
    for (unsigned k=0;k<5;++k) {
        for (unsigned n=0;n<(k==1?3u:1u);++n) {
            const uint64_t t0=w.at(taps[k],n);assert(t0!=~0ull);
            auto f=w.framesBetween(t0,t0+100);
            assert(f.size()==6);
            for (unsigned i=0;i<5;++i) {
                assert(f[i].down && f[i].t==t0+16*i && f[i].x==logical(tapXY[k][0],1216) && f[i].y==logical(tapXY[k][1],2640));
            }
            assert(!f[5].down && f[5].t==t0+80 && f[5].x==f[0].x && f[5].y==f[0].y);
        }
    }
    const uint64_t shift=w.at(taps[0]);
    assert(w.at(taps[1])==shift+680 && w.at(taps[1],1)==shift+2*680 && w.at(taps[1],2)==shift+3*680);
    assert(w.at(taps[2])==shift+4*680 && w.at(taps[3])==shift+5*680 && w.at(taps[4])==shift+6*680);

    // Swipes: a frame every 8 ms from the start point to the end point in the stated time, then the lift there.
    struct Swipe { const char *line; unsigned n; uint16_t x0,y0,x1,y1; uint64_t ms; bool edge; };
    const Swipe swipes[]={{"input script: kb unlock 608,2628>608,1748 250ms",0,608,2628,608,1748,250,true},
                          {"input script: kb spotlight 608,1320>608,1770 250ms",0,608,1320,608,1770,250,false},
                          {"input script: kb close 608,2628>608,1748 250ms",0,608,2628,608,1748,250,true},
                          {"input script: cc open 1064,12>1064,1200 300ms",0,1064,12,1064,1200,300,true},
                          {"input script: cc close 608,2628>608,1748 250ms",0,608,2628,608,1748,250,true}};
    for (auto &sw:swipes) {
        const uint64_t t0=w.at(sw.line,sw.n);assert(t0!=~0ull);
        auto f=w.framesBetween(t0,t0+sw.ms+100);
        const size_t moves=(sw.ms+kTick-1)/kTick;   // frames after the first, the last one at the end point
        assert(f.size()==1+moves+1);
        assert(f[0].down && f[0].t==t0 && f[0].x==logical(sw.x0,1216) && f[0].y==logical(sw.y0,2640));
        for (size_t i=1;i<=moves;++i) {
            assert(f[i].down && f[i].t==f[i-1].t+kTick);
            const double t=std::min<double>((double)(f[i].t-t0),(double)sw.ms)/(double)sw.ms;
            const double y=sw.y0+(sw.y1-(double)sw.y0)*t;
            assert(std::abs((double)f[i].y-(double)logical(y,2640))<=13.0);   // within a pixel of the straight path
            assert(sw.y1<sw.y0?f[i].y<=f[i-1].y:f[i].y>=f[i-1].y);
        }
        assert(f[moves].x==logical(sw.x1,1216) && f[moves].y==logical(sw.y1,2640));
        assert(!f[moves+1].down && f[moves+1].t==f[moves].t+kTick && f[moves+1].y==f[moves].y);
        assert(edgeGesture(f)==sw.edge);
    }
    // Taps never start at an edge (the dock and the keys lie inside the bands).
    for (auto &t:taps) {auto f=w.framesBetween(w.at(t),w.at(t)+100);assert(!edgeGesture(f));}

    // Keys: the key device reports each press for its hold time (120 ms), Volume Up three times, then Volume Down three
    // times, 920 ms apart; Home (Consumer Menu) presses for the preludes and closes. Never the ring bit, never two at once.
    std::vector<std::pair<uint64_t,uint8_t>> presses;
    for (size_t i=0;i+1<w.keyReports.size();++i) {
        const uint8_t bits=w.keyReports[i].second&~PAGANI_REPORT_RING;
        assert(w.keyReports[i].second&PAGANI_REPORT_RING);
        if (!bits) continue;
        assert(bits==PAGANI_KEY_MENU || bits==PAGANI_KEY_VOLUME_UP || bits==PAGANI_KEY_VOLUME_DOWN);
        assert(w.keyReports[i+1].second==PAGANI_REPORT_RING && w.keyReports[i+1].first-w.keyReports[i].first==120);
        presses.emplace_back(w.keyReports[i].first,bits);
    }
    std::vector<uint8_t> order;for (auto &p:presses) order.push_back(p.second);
    const std::vector<uint8_t> expectedKeys={1,1,1,1,4,4,4,8,8,8,1,1};
    assert(order==expectedKeys);
    for (unsigned i=5;i<10;++i) assert(presses[i].first-presses[i-1].first==920);
    assert(w.at("input script: vol up 1")+8==presses[4].first);
    // 5 s between scripts: from the end of the keyboard script's last gap to the next prelude.
    assert(w.at("input script: vol wake on")-w.at("input script: kb home",2)==120+1000+5000);
    assert(w.at("input script: cc wake on")-w.at("input script: vol down 3")==120+800+5000);
    // Control Center stays up 3 s before the close.
    assert(w.at("input script: cc close 608,2628>608,1748 250ms")-w.at("input script: cc open 1064,12>1064,1200 300ms")==304+8+3000);
    // After the run nothing more happens.
    const unsigned calls=w.ioCalls;w.runFor(60000);assert(w.ioCalls==calls);
}

static void geometry() {
    // Key frames of iPhone-PortraitChoco405-QWERTY (KBLayouts_iPhone.dat, points in the 405.333x216 keyplane), the keyplane on
    // the 75-point dock at the bottom of the 880-point screen.
    const double keyplane=880-75-216;
    struct Key { const char *name; double x,y,w,h; uint16_t px,py; };
    const Key keys[]={{"shift",0,115,52,46,78,2181},{"a",20,61,40+2.0/3,46,121,2019},{"123",0,169,50+1.0/3,46,78,2343},
                      {"123 merged",0,169,101,46,78,2343}};
    for (auto &k:keys) {
        const double left=k.x*3,right=(k.x+k.w)*3,top=(keyplane+k.y)*3,bottom=(keyplane+k.y+k.h)*3;
        assert(k.px>left+20 && k.px<right-20 && k.py>top+20 && k.py<bottom-20);   // well inside the key
        assert(std::abs(k.py-(top+bottom)/2)<=0.5);
        if (k.x==0 && k.w>100) continue;
        if (!strcmp(k.name,"123")) assert(k.px<=k.w*3);
        else assert(std::abs(k.px-(left+right)/2)<=0.5);
    }
    // The globe/emoji button: +[UIKeyboardDockView _itemFramesForBoundingSize:] at 460 ppi (points per mm 460/25.4/3).
    const double ppm=460/25.4/3;
    const double gx=ppm*7.043516123248725*3,gy=(880-33-ppm*5.066129330110622/2)*3;
    assert(std::abs(gx-128)<=0.5 && std::abs(gy-2495)<=0.5);
    // Edge bands of HidEvents (48 framebuffer pixels): the home gesture starts inside the bottom band, Control Center inside
    // the top band right of the island (x 420..796) in the trailing quarter SpringBoard gives it, the pull-down far from all.
    assert(2628>2640-48 && 12<48 && 1064>796 && 1064>1216*3/4 && 1064<1216-48 && 608>48 && 1320>48 && 1770<2640-48);
    assert(2628-1748==2640/3 && 1770-1320>=120*3);   // a third of the screen up (SpringBoard's PPT swipe), at least its 120 pt pull
    // Logical coordinates: the report's 0..32767 spans the 1216x2640 framebuffer.
    assert(pagani_script_logical(0,1216)==0 && pagani_script_logical(1215,1216)==32740 && pagani_script_logical(5000,1216)==32740);
    assert(pagani_script_logical(608,1216)==16384 && pagani_script_logical(2628,2640)==32618 && pagani_script_logical(12,2640)==149);
    assert(pagani_script_logical(121,1216)==3261 && pagani_script_logical(2019,2640)==25059 && pagani_script_logical(1,0)==0);
}

static void wake() {
    // Dark display: Power for 120 ms, then the wait for the display (on after 400 ms here) before the rest.
    World w;w.displayOff=true;w.wakeDelay=400;assert(w.arm(PAGANI_SCRIPT_KEYBOARD));w.runUntilDone();
    assert(w.lines[1].second=="input script: start 1 at 240000ms keys on");
    assert(w.lines[2].second=="input script: kb wake power, on 400ms" && w.lines[2].first==kStart+400);
    assert(w.keyReports[1].second==(PAGANI_REPORT_RING|PAGANI_KEY_POWER) && w.keyReports[1].first==kStart+8);
    assert(w.keyReports[2].second==PAGANI_REPORT_RING && w.keyReports[2].first==kStart+128);
    assert(w.at("input script: kb home")==kStart+400+1000 && w.has("input script: done 1 in "));
    // A display that stays dark: 3 s, logged, and the script goes on (Home and the swipe can still wake and unlock).
    World off;off.displayOff=true;assert(off.arm(PAGANI_SCRIPT_KEYBOARD));off.runUntilDone();
    assert(off.lines[2].second=="input script: kb wake power, off 3000ms" && off.lines[2].first==kStart+3000);
    assert(off.at("input script: kb home")==kStart+4000 && off.has("input script: done 1 in "));
    // No key device: the dark display is logged, key steps say so and take their time, the touches still run.
    World nokeys;nokeys.displayOff=true;nokeys.keysAlive=false;assert(nokeys.arm(PAGANI_SCRIPT_ALL));nokeys.runUntilDone();
    assert(nokeys.lines[1].second=="input script: start 7 at 240000ms keys off");
    assert(nokeys.lines[2].second=="input script: kb wake off, no keys" && nokeys.at("input script: kb home, no keys")==kStart+1000);
    assert(nokeys.at("input script: vol up 1, no keys")!=~0ull && nokeys.keyReports.empty());
    assert(nokeys.keyCalls.size()==1 && !std::get<1>(nokeys.keyCalls[0]));   // only the release at the end
    assert(nokeys.has("input script: done 7 in ") && nokeys.sent.size()>100);
}

static void aborts() {
    // At the start: no binding, a stopped controller, a finger already down. Nothing is sent, the line says why.
    {World w;w.hidBound=false;assert(w.arm(7));w.runUntilDone();
     assert(w.lines.size()==2 && w.lines[1].second=="input script: abort: touch HID not ready b=7" && w.lines[1].first==kStart);
     assert(w.sent.empty() && !w.s.armed && !__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE));}
    {World w;w.controllerReady=false;assert(w.arm(7));w.runUntilDone();
     assert(w.lines.back().second=="input script: abort: touch stopped" && w.sent.empty());}
    {World w;w.previous.contact[0]={100,100,1};assert(w.arm(7));w.runUntilDone();
     assert(w.lines.back().second=="input script: abort: real touch" && w.sent.empty());}
    // A real object during a swipe: the synthetic finger is lifted at once, then nothing more.
    {World w;assert(w.arm(7));
     w.runUntil("input script: kb unlock");
     w.runFor(100);assert(w.s.down && w.sent.back().down);
     auto io=w.io();
     assert(pagani_script_real_frame(&w.s,0,w.now,&io)==0);           // an empty real report would lift our finger: held back
     const size_t before=w.sent.size();
     assert(pagani_script_real_frame(&w.s,1,w.now,&io)==1);           // a finger: delivered, after the lift
     assert(w.sent.size()==before+1 && !w.sent.back().down && w.sent.back().y==w.sent[before-1].y);
     assert(w.lines.back().second=="input script: abort kb unlock: real touch" && !w.s.armed && !w.s.down);
     w.tick();assert(!__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE));
     const unsigned calls=w.ioCalls;w.runFor(60000);assert(w.ioCalls==calls && w.sent.size()==before+1);
     assert(pagani_script_real_frame(&w.s,1,w.now,&io)==1 && w.ioCalls==calls);}   // afterwards, as before
    // A palm the decoder drops is still an object (s3910_objects), class "none" is not.
    {const uint8_t palm[]={2,3,6,0,1,0,1,4,0,0,2,0,2};assert(s3910_objects(palm,sizeof palm)==1);
     const uint8_t finger[]={1,0,1,0,0,0,0};assert(s3910_objects(finger,sizeof finger)==1);
     assert(!s3910_objects(palm,sizeof palm-1) && !s3910_objects(nullptr,7) && !s3910_objects(palm,0));
     const uint8_t many[]={11,0,1,0,0,0,0};assert(!s3910_objects(many,sizeof many));}
    // A real key (or ring switch) press during the run; the held script key is released with it.
    {World w;assert(w.arm(PAGANI_SCRIPT_VOLUME));
     w.runUntil("input script: vol up 2");
     w.runFor(40);assert(w.lastKeyReport==(PAGANI_REPORT_RING|PAGANI_KEY_VOLUME_UP));
     ++w.realKeys;w.tick();
     assert(w.lines.back().second=="input script: abort vol up 2: real key" && !pagani_script_link.key_word);
     w.tick();assert(w.lastKeyReport==PAGANI_REPORT_RING);}
    // A refused report during a tap; the controller stopping during the volume keys.
    {World w;assert(w.arm(PAGANI_SCRIPT_KEYBOARD));
     w.runUntil("input script: kb tap a");
     w.sendWorks=false;w.runFor(100);
     assert(w.lines.back().second=="input script: abort kb tap a: HID report refused" && !w.s.armed);}
    {World w;assert(w.arm(PAGANI_SCRIPT_VOLUME));
     w.runUntil("input script: vol down 1");
     w.controllerReady=false;w.tick();
     assert(w.lines.back().second=="input script: abort vol down 1: touch stopped" && !pagani_script_link.key_word);}
    // A real touch in the 5 s pause between two scripts names the script that was about to start.
    {World w;assert(w.arm(PAGANI_SCRIPT_KEYBOARD|PAGANI_SCRIPT_VOLUME));
     for (unsigned i=0;i<100000 && w.s.armed && w.s.index<14;++i) w.tick();
     w.runFor(1000);assert(w.s.program[w.s.index].step->kind==PAGANI_STEP_PAUSE && !w.s.down);
     auto io=w.io();assert(pagani_script_real_frame(&w.s,2,w.now,&io)==1);
     assert(w.lines.back().second=="input script: abort vol: real touch");}
    // The driver stopping (PaganiInput::stop) in a gap.
    {World w;assert(w.arm(PAGANI_SCRIPT_CONTROL));
     w.runUntil("input script: cc open");
     w.runFor(1000);assert(!w.s.down);
     auto io=w.io();const size_t frames=w.sent.size();pagani_script_abort(&w.s,"driver stopped",&io);
     assert(w.lines.back().second=="input script: abort cc open: driver stopped" && w.sent.size()==frames);}
}

static void keyDevice() {
    // The failsafe: a key the touch device never released drops out 40 ms after its planned release.
    resetLink();__atomic_store_n(&pagani_script_link.on,1u,__ATOMIC_RELEASE);
    pagani_script_hold(PAGANI_KEY_POWER|PAGANI_REPORT_RING|0x20,1000+120+PAGANI_SCRIPT_KEY_SLACK_MS);
    assert(pagani_script_keys(PAGANI_REPORT_RING,1000)==(PAGANI_REPORT_RING|PAGANI_KEY_POWER));   // never the ring bit or padding
    assert(pagani_script_keys(0,1159)==PAGANI_KEY_POWER && pagani_script_keys(0,1160)==0 && pagani_script_keys(0,5000)==0);
    pagani_script_hold(PAGANI_KEY_MENU,6000);assert(pagani_script_keys(0,5000)==PAGANI_KEY_MENU);
    pagani_script_hold(0,6000);assert(pagani_script_keys(0,5000)==0 && !pagani_script_link.key_word);
    // Real presses: the first poll primes, a new press counts once however long it is held, a ring change counts.
    resetLink();__atomic_store_n(&pagani_script_link.on,1u,__ATOMIC_RELEASE);
    pagani_script_keys(PAGANI_KEY_POWER,1);assert(!pagani_script_link.real_keys);           // held since before the run
    pagani_script_keys(PAGANI_KEY_POWER,9);pagani_script_keys(0,17);assert(!pagani_script_link.real_keys);
    pagani_script_keys(PAGANI_KEY_VOLUME_DOWN,25);pagani_script_keys(PAGANI_KEY_VOLUME_DOWN,33);assert(pagani_script_link.real_keys==1);
    pagani_script_keys(PAGANI_KEY_VOLUME_DOWN|PAGANI_REPORT_RING,41);assert(pagani_script_link.real_keys==2);
    pagani_script_hold(PAGANI_KEY_VOLUME_UP,100);pagani_script_keys(PAGANI_REPORT_RING,49);   // script keys are not real
    assert(pagani_script_link.real_keys==2 && pagani_script_keys_alive(49) && pagani_script_keys_alive(148) && !pagani_script_keys_alive(149));
    resetLink();

    // The driver's own poll (PaganiButtons.inc): the script's key goes out in report 2 and counts as a key event; real
    // keys are still debounced and reported, and counted for the abort rule.
    using keys_adapter::Device;
    Device d;d.setupPlusKey();uint64_t ms=1000;
    for (unsigned i=0;i<10;++i) d.pollButtons(ms+=8);
    assert(d.reports==std::vector<uint8_t>({PAGANI_REPORT_RING}));
    __atomic_store_n(&pagani_script_link.on,1u,__ATOMIC_RELEASE);
    const uint32_t events=pagani_input_key_events;
    pagani_script_hold(PAGANI_KEY_VOLUME_UP,ms+8+160);
    d.pollButtons(ms+=8);
    assert(d.reports.back()==(PAGANI_REPORT_RING|PAGANI_KEY_VOLUME_UP) && pagani_input_key_events==events+1);
    for (unsigned i=0;i<10;++i) d.pollButtons(ms+=8);
    assert(d.reports.size()==2);
    pagani_script_hold(0,0);d.pollButtons(ms+=8);
    assert(d.reports.back()==PAGANI_REPORT_RING && pagani_input_key_events==events+2 && !pagani_script_link.real_keys);
    // A script's Power key goes out in the report, and the PMIC knows nothing of it: the UFS log's word stays down.
    pagani_script_hold(PAGANI_KEY_POWER,ms+8+160);d.pollButtons(ms+=8);
    assert((d.reports.back()&PAGANI_KEY_POWER) && !pagani_input_power_held);
    pagani_script_hold(0,0);d.pollButtons(ms+=8);
    assert(d.reports.back()==PAGANI_REPORT_RING);
    d.levels(0x80,1,1);for (unsigned i=0;i<4;++i) d.pollButtons(ms+=8);   // a real Power press, 16 ms debounce
    assert(d.reports.back()==(PAGANI_REPORT_RING|PAGANI_KEY_POWER) && pagani_script_link.real_keys==1);
    d.levels(0,1,1);for (unsigned i=0;i<4;++i) d.pollButtons(ms+=8);
    assert(d.reports.back()==PAGANI_REPORT_RING && pagani_script_link.real_keys==1 && pagani_script_keys_alive(ms));
    resetLink();
    // Report 2's bit 0 is Consumer Menu, the key the script presses for Home.
    assert(buttonFields()[__builtin_ctz(PAGANI_KEY_MENU)]==0xc0040 && !(pagani_buttons(0xff,0,0)&PAGANI_KEY_MENU));
}
}

static void inputScript() {
    script_test::defaultOff();script_test::program();script_test::timeline();script_test::geometry();script_test::wake();
    script_test::aborts();script_test::keyDevice();
}
