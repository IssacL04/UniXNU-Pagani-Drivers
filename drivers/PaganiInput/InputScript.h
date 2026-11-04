// SPDX-License-Identifier: BSD-2-Clause-Patent
// Scripted input self-test: the phone types, presses keys and opens Control Center by itself, so a test run needs nobody at
// the phone. Off unless the boot-arg pagani-input-script=<mask> is nonzero (bit 0 keyboard via Spotlight, bit 1 volume keys,
// bit 2 Control Center); pagani-input-script-at=<s> starts it that many seconds of mach absolute time after boot (default
// 240, the clock the kbt lines print). The touch device runs the timeline from its 8 ms timer: it sends the synthetic finger
// (contact PAGANI_SCRIPT_CONTACT) through the same report builder and HID path as real contacts, and hands keys to the key
// device, whose poll adds them to report 2 (pagani_script_keys). Each step logs one ramoops U line "input script: ...".
// A real object on the screen, a real key, a stopped touch controller or a refused HID report ends the run at once (the
// synthetic finger is lifted first, the keys released); so does an unbound touch HID driver at the start.
//
// Geometry (iPadOS 17.7.11 21H461 with the iPhone16,1 identity, 1216x2640 px = 405.333x880 pt at @3x, coordinates in
// framebuffer pixels, y down):
//  * Home gesture: SpringBoard's own PPT swipe (-[SpringBoard(SBApplicationTesting) _performSwipeHomeWithComposer:]) runs from
//    the bottom centre up one third of the screen in 0.25 s; here from 12 px above the bottom (inside the 48 px edge band that
//    pagani_touch_edge marks) up 880 px.
//  * Spotlight: the home screen's pull-down (_pullDownToSpotlightForView:offset:120 duration:0.25 from the vertical middle);
//    here from the middle down 150 pt, clear of every edge band.
//  * Control Center: SBControlCenterController's trailing status-bar region on a phone in portrait is the right quarter of the
//    screen (x > 912 px); the island spans x 420-796. The swipe starts at x 1064 in the top edge band.
//  * Keyboard: iPhone-PortraitChoco405 (the 405.333-point phone family of the overlay's KBLayouts_iPhone.dat), keyplane
//    405.333x216 pt above the 75 pt dock (-[UIKeyboardDockView intrinsicContentSize]; +[UIKeyboardImpl
//    deviceSpecificPaddingForInterfaceOrientation:inputMode:] pads a phone without a home button by it in portrait), so the
//    keyplane starts at y 589 pt. Key frames from the layout file (pt):
//    Shift 0,115 52x46; a 20,61 40.667x46; 123/ABC 0,169 50.333x46, or 101 wide once the globe moves to the dock
//    (iPhone-Default-Merge-Policy); taps go to the key centre, 123/ABC at x 26 pt, inside both widths. The globe/emoji button is
//    the dock's left item (+[UIKeyboardDockView _itemFramesForBoundingSize:]): centre 7.0435 mm from the left edge and
//    33 pt + half its 5.066 mm height above the screen bottom, at the 460 ppi of the panel (and of the D83 it imitates).
#ifndef PAGANI_INPUT_SCRIPT_H
#define PAGANI_INPUT_SCRIPT_H
#include <stddef.h>
#include <stdint.h>
#include "Buttons.h"
#include "S3910.h"

enum {
    PAGANI_SCRIPT_KEYBOARD=1, PAGANI_SCRIPT_VOLUME=2, PAGANI_SCRIPT_CONTROL=4, PAGANI_SCRIPT_ALL=7,
    PAGANI_SCRIPT_AT_DEFAULT=240,           // seconds
    PAGANI_SCRIPT_WIDTH=1216, PAGANI_SCRIPT_HEIGHT=2640,  // the portrait framebuffer the coordinates are drawn for
    PAGANI_SCRIPT_LOGICAL_MAX=32767,        // the touch report's X/Y logical maximum (Reports.h)
    PAGANI_SCRIPT_CONTACT=9,                // contact id of the synthetic finger; the controller numbers real ones from 0
    PAGANI_SCRIPT_REPEAT_MS=16,             // a held tap repeats its frame this often, like a finger lying still
    PAGANI_SCRIPT_WAKE_MS=3000,             // longest wait for the display after the wake press
    PAGANI_SCRIPT_KEY_SLACK_MS=40,          // the key device drops a script key this long after its planned release
    PAGANI_SCRIPT_KEYS_ALIVE_MS=100,        // the key device counts as polling when it polled this recently
    PAGANI_SCRIPT_LINE=80,                  // log line buffer
    PAGANI_SCRIPT_MAX_STEPS=32,
    PAGANI_SCRIPT_KEY_BITS=PAGANI_KEY_MENU|PAGANI_KEYS_PMIC,  // the report-2 bits a script may press (never the ring switch)
};
enum { PAGANI_STEP_PAUSE, PAGANI_STEP_WAKE, PAGANI_STEP_KEY, PAGANI_STEP_TAP, PAGANI_STEP_SWIPE };
// One step: WAKE presses `key` for `hold` ms when the display is off and waits for it to come on; KEY presses `key` for `hold`
// ms; TAP holds a finger at x0,y0 for `hold` ms; SWIPE moves a finger from x0,y0 to x1,y1 in `hold` ms (a frame every timer
// tick) and lifts it there. Every step then idles `gap` ms. A PAUSE only idles and is not logged.
struct pagani_script_step {
    const char *name;
    uint8_t kind, key;
    uint16_t hold, gap, x0, y0, x1, y1;
};
// Wake the display, then make sure the home screen is up. The first script also unlocks (no passcode): the Home key (Consumer
// Menu) does it on the lock screen and only wakes a dark one, the home gesture does it on a lit one, and either is harmless
// on the home screen, so one failing path still ends on the home screen. Later scripts start where the previous one left.
static const struct pagani_script_step pagani_script_unlock[]={
    {"wake",PAGANI_STEP_WAKE,PAGANI_KEY_POWER,120,1000,0,0,0,0},
    {"home",PAGANI_STEP_KEY,PAGANI_KEY_MENU,120,1500,0,0,0,0},
    {"unlock",PAGANI_STEP_SWIPE,0,250,1500,608,2628,608,1748},
    {"home",PAGANI_STEP_KEY,PAGANI_KEY_MENU,120,1500,0,0,0,0},
};
static const struct pagani_script_step pagani_script_home[]={
    {"wake",PAGANI_STEP_WAKE,PAGANI_KEY_POWER,120,1000,0,0,0,0},
    {"home",PAGANI_STEP_KEY,PAGANI_KEY_MENU,120,1500,0,0,0,0},
};
// Bit 0: Spotlight's search field takes focus and shows the keyboard (3 s, so no tap lands on a suggestion first); Shift,
// a three times, 123, ABC and the globe, 80 ms each with 600 ms between, then the home gesture and Home close Spotlight.
static const struct pagani_script_step pagani_script_keyboard[]={
    {"spotlight",PAGANI_STEP_SWIPE,0,250,3000,608,1320,608,1770},
    {"tap shift",PAGANI_STEP_TAP,0,80,600,78,2181,0,0},
    {"tap a",PAGANI_STEP_TAP,0,80,600,121,2019,0,0},
    {"tap a",PAGANI_STEP_TAP,0,80,600,121,2019,0,0},
    {"tap a",PAGANI_STEP_TAP,0,80,600,121,2019,0,0},
    {"tap 123",PAGANI_STEP_TAP,0,80,600,78,2343,0,0},
    {"tap abc",PAGANI_STEP_TAP,0,80,600,78,2343,0,0},
    {"tap globe",PAGANI_STEP_TAP,0,80,1000,128,2495,0,0},
    {"close",PAGANI_STEP_SWIPE,0,250,1500,608,2628,608,1748},
    {"home",PAGANI_STEP_KEY,PAGANI_KEY_MENU,120,1000,0,0,0,0},
};
// Bit 1: Volume Up three times, then Volume Down three times, 120 ms presses with 800 ms between.
static const struct pagani_script_step pagani_script_volume[]={
    {"up 1",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_UP,120,800,0,0,0,0},
    {"up 2",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_UP,120,800,0,0,0,0},
    {"up 3",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_UP,120,800,0,0,0,0},
    {"down 1",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_DOWN,120,800,0,0,0,0},
    {"down 2",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_DOWN,120,800,0,0,0,0},
    {"down 3",PAGANI_STEP_KEY,PAGANI_KEY_VOLUME_DOWN,120,800,0,0,0,0},
};
// Bit 2: Control Center from the top-right corner, 3 s open, closed by the home gesture and Home.
static const struct pagani_script_step pagani_script_control[]={
    {"open",PAGANI_STEP_SWIPE,0,300,3000,1064,12,1064,1200},
    {"close",PAGANI_STEP_SWIPE,0,250,1500,608,2628,608,1748},
    {"home",PAGANI_STEP_KEY,PAGANI_KEY_MENU,120,1000,0,0,0,0},
};
static const struct pagani_script_step pagani_script_pause[]={{"",PAGANI_STEP_PAUSE,0,0,5000,0,0,0,0}};
static const char *const pagani_script_names[]={"kb","vol","cc"};

struct pagani_script_entry { const struct pagani_script_step *step; uint8_t script; };
struct pagani_script {
    uint8_t armed,running,mask,phase,down;
    uint16_t x,y;                           // the synthetic finger's last position (logical)
    unsigned count,index;
    uint64_t start,begun,t0,until,next;
    uint32_t width,height,real_keys;
    struct pagani_script_entry program[PAGANI_SCRIPT_MAX_STEPS];
};
// What the driver sees this tick.
struct pagani_script_input {
    uint64_t ms;                            // mach absolute time
    uint8_t display_off;                    // PaganiPanel's pagani_screen_state bit 0
    uint8_t keys_alive;                     // the key device polled recently (pagani_script_keys_alive)
    uint8_t controller_ready;               // the S3910 still reports
    uint8_t hid_bound;                      // the qualified IOHIDEventDriver binding (only read at the start)
    uint8_t real_touch;                     // a real contact is down
    uint32_t real_keys;                     // pagani_script_link.real_keys
    uint32_t binding;                       // binding bits for the log
};
// What the script does: send a frame with the synthetic finger down at x,y (logical) or lifted (1 when the HID parser took
// it), hold report-2 bits until a time (0: release), log a line.
struct pagani_script_io {
    void *ctx;
    int (*touch)(void *ctx,int down,uint16_t x,uint16_t y);
    void (*keys)(void *ctx,uint8_t bits,uint64_t until);
    void (*log)(void *ctx,const char *line);
};
enum { PAGANI_PHASE_ENTER, PAGANI_PHASE_ACTIVE, PAGANI_PHASE_LIFT, PAGANI_PHASE_WAIT, PAGANI_PHASE_GAP };

// Shared by the two PaganiInput instances: the touch device runs the script, the key device presses its keys. All zero while
// no script is armed.
struct pagani_script_shared {
    volatile uint32_t on;                   // a script is armed
    volatile uint64_t key_word;             // (until_ms << 8) | report-2 bits the script holds
    volatile uint64_t keys_seen;            // ms of the key device's last poll while on
    volatile uint32_t real_keys;            // real key presses and ring/silent changes the key device saw while on
    uint8_t last_real,primed;               // key device only
};
static struct pagani_script_shared pagani_script_link;

// Bounded appends; the line stays NUL-terminated.
static inline size_t pagani_script_put(char *line,size_t size,size_t at,const char *s) {
    while (s && *s && at+1<size) line[at++]=*s++;
    if (size) line[at<size?at:size-1]='\0';
    return at;
}
static inline size_t pagani_script_put_u(char *line,size_t size,size_t at,uint64_t v,unsigned base) {
    char digits[24];unsigned n=0;
    do {unsigned d=(unsigned)(v%base);digits[n++]=(char)(d<10?'0'+d:'a'+d-10);v/=base;} while (v && n<sizeof digits);
    char text[25];unsigned i=0;while (n) text[i++]=digits[--n];
    text[i]='\0';return pagani_script_put(line,size,at,text);
}
// Framebuffer pixel to the report's logical 0..32767 (IOHIDEventDriver scales it back by the same range).
static inline uint16_t pagani_script_logical(uint32_t px,uint32_t size) {
    if (!size) return 0;
    if (px>=size) px=size-1;
    return (uint16_t)(((uint64_t)px*PAGANI_SCRIPT_LOGICAL_MAX+size/2)/size);
}
static inline void pagani_script_append(struct pagani_script *s,const struct pagani_script_step *steps,unsigned n,uint8_t script) {
    for (unsigned i=0;i<n && s->count<PAGANI_SCRIPT_MAX_STEPS;++i) {
        s->program[s->count].step=&steps[i];s->program[s->count].script=script;++s->count;
    }
}
// Boot-arg values to a program. 1 when armed; `line` gets the U line to log ("" when the mask is 0: nothing at all happens).
static inline int pagani_script_arm(struct pagani_script *s,uint32_t mask,uint32_t at_s,uint32_t width,uint32_t height,
                                    char *line,size_t size) {
    struct pagani_script empty={};*s=empty;
    pagani_script_put(line,size,0,"");
    if (!mask) return 0;
    size_t at=pagani_script_put(line,size,0,"input script: ");
    if (!(mask&PAGANI_SCRIPT_ALL)) {
        at=pagani_script_put(line,size,at,"off, mask ");pagani_script_put_u(line,size,at,mask,16);return 0;
    }
    if (width!=PAGANI_SCRIPT_WIDTH || height!=PAGANI_SCRIPT_HEIGHT) {
        at=pagani_script_put(line,size,at,"off, screen ");at=pagani_script_put_u(line,size,at,width,10);
        at=pagani_script_put(line,size,at,"x");pagani_script_put_u(line,size,at,height,10);return 0;
    }
    s->mask=(uint8_t)(mask&PAGANI_SCRIPT_ALL);s->width=width;s->height=height;
    s->start=(uint64_t)at_s*1000;
    const struct { const struct pagani_script_step *steps; unsigned n; } scripts[]={
        {pagani_script_keyboard,sizeof pagani_script_keyboard/sizeof *pagani_script_keyboard},
        {pagani_script_volume,sizeof pagani_script_volume/sizeof *pagani_script_volume},
        {pagani_script_control,sizeof pagani_script_control/sizeof *pagani_script_control},
    };
    for (uint8_t i=0;i<3;++i) {
        if (!(s->mask&(1u<<i))) continue;
        if (s->count) pagani_script_append(s,pagani_script_pause,1,i);
        if (s->count) pagani_script_append(s,pagani_script_home,sizeof pagani_script_home/sizeof *pagani_script_home,i);
        else pagani_script_append(s,pagani_script_unlock,sizeof pagani_script_unlock/sizeof *pagani_script_unlock,i);
        pagani_script_append(s,scripts[i].steps,scripts[i].n,i);
    }
    s->armed=1;
    at=pagani_script_put(line,size,at,"armed ");at=pagani_script_put_u(line,size,at,s->mask,10);
    at=pagani_script_put(line,size,at," at ");at=pagani_script_put_u(line,size,at,at_s,10);pagani_script_put(line,size,at,"s");
    return 1;
}
// The start time has come for an armed script (real frames go through unchanged before).
static inline int pagani_script_due(const struct pagani_script *s,uint64_t ms) {return s->armed && ms>=s->start;}

static inline size_t pagani_script_head(const struct pagani_script *s,char *line,size_t size,const char *what) {
    size_t at=pagani_script_put(line,size,0,"input script: ");
    if (what) at=pagani_script_put(line,size,at,what);
    if (s->index<s->count) {
        const struct pagani_script_entry *e=&s->program[s->index];
        if (what) at=pagani_script_put(line,size,at," ");
        at=pagani_script_put(line,size,at,pagani_script_names[e->script]);
        if (e->step->name[0]) {at=pagani_script_put(line,size,at," ");at=pagani_script_put(line,size,at,e->step->name);}
    }
    return at;
}
// Ends the run: lifts the synthetic finger, releases the keys, logs why.
static inline void pagani_script_abort(struct pagani_script *s,const char *why,const struct pagani_script_io *io) {
    if (!s->armed) return;
    if (s->down) {io->touch(io->ctx,0,s->x,s->y);s->down=0;}
    io->keys(io->ctx,0,0);
    char line[PAGANI_SCRIPT_LINE];
    size_t at=s->running?pagani_script_head(s,line,sizeof line,"abort"):pagani_script_put(line,sizeof line,0,"input script: abort");
    at=pagani_script_put(line,sizeof line,at,": ");pagani_script_put(line,sizeof line,at,why);
    io->log(io->ctx,line);
    s->armed=0;s->running=0;
}
static inline int pagani_script_send(struct pagani_script *s,int down,uint16_t x,uint16_t y,const struct pagani_script_io *io) {
    if (!io->touch(io->ctx,down,x,y)) {pagani_script_abort(s,"HID report refused",io);return 0;}
    s->down=(uint8_t)(down!=0);s->x=x;s->y=y;return 1;
}
static inline size_t pagani_script_point(char *line,size_t size,size_t at,const char *sep,uint16_t x,uint16_t y) {
    at=pagani_script_put(line,size,at,sep);at=pagani_script_put_u(line,size,at,x,10);
    at=pagani_script_put(line,size,at,",");return pagani_script_put_u(line,size,at,y,10);
}
// Enters the current step: logs it and starts its first action (a step without one goes straight to its gap).
static inline void pagani_script_enter(struct pagani_script *s,const struct pagani_script_input *in,
                                       const struct pagani_script_io *io) {
    const struct pagani_script_step *p=s->program[s->index].step;
    char line[PAGANI_SCRIPT_LINE];size_t at=pagani_script_head(s,line,sizeof line,0);
    s->t0=in->ms;
    switch (p->kind) {
    case PAGANI_STEP_PAUSE:
        s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;return;
    case PAGANI_STEP_WAKE:
        if (!in->display_off || !in->keys_alive) {
            pagani_script_put(line,sizeof line,at,in->display_off?" off, no keys":" on");io->log(io->ctx,line);
            s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;return;
        }
        io->keys(io->ctx,p->key,in->ms+p->hold+PAGANI_SCRIPT_KEY_SLACK_MS);s->phase=PAGANI_PHASE_ACTIVE;return;
    case PAGANI_STEP_KEY:
        if (!in->keys_alive) {
            pagani_script_put(line,sizeof line,at,", no keys");io->log(io->ctx,line);
            s->until=in->ms+p->hold+p->gap;s->phase=PAGANI_PHASE_GAP;return;
        }
        io->log(io->ctx,line);
        io->keys(io->ctx,p->key,in->ms+p->hold+PAGANI_SCRIPT_KEY_SLACK_MS);s->phase=PAGANI_PHASE_ACTIVE;return;
    case PAGANI_STEP_TAP:
        pagani_script_point(line,sizeof line,at," ",p->x0,p->y0);io->log(io->ctx,line);
        if (!pagani_script_send(s,1,pagani_script_logical(p->x0,s->width),pagani_script_logical(p->y0,s->height),io)) return;
        s->next=in->ms+PAGANI_SCRIPT_REPEAT_MS;s->phase=PAGANI_PHASE_ACTIVE;return;
    default: {
        at=pagani_script_point(line,sizeof line,at," ",p->x0,p->y0);
        at=pagani_script_point(line,sizeof line,at,">",p->x1,p->y1);at=pagani_script_put(line,sizeof line,at," ");
        at=pagani_script_put_u(line,sizeof line,at,p->hold,10);pagani_script_put(line,sizeof line,at,"ms");
        io->log(io->ctx,line);
        if (!pagani_script_send(s,1,pagani_script_logical(p->x0,s->width),pagani_script_logical(p->y0,s->height),io)) return;
        s->phase=PAGANI_PHASE_ACTIVE;return;
    }
    }
}
// One timer tick of the touch device.
static inline void pagani_script_tick(struct pagani_script *s,const struct pagani_script_input *in,
                                      const struct pagani_script_io *io) {
    if (!pagani_script_due(s,in->ms)) return;
    if (!s->running) {
        if (!in->controller_ready) {pagani_script_abort(s,"touch stopped",io);return;}
        if (!in->hid_bound) {
            char why[32];size_t at=pagani_script_put(why,sizeof why,0,"touch HID not ready b=");
            pagani_script_put_u(why,sizeof why,at,in->binding,16);pagani_script_abort(s,why,io);return;
        }
        if (in->real_touch) {pagani_script_abort(s,"real touch",io);return;}
        s->running=1;s->begun=in->ms;s->real_keys=in->real_keys;s->index=0;s->phase=PAGANI_PHASE_ENTER;
        char line[PAGANI_SCRIPT_LINE];size_t at=pagani_script_put(line,sizeof line,0,"input script: start ");
        at=pagani_script_put_u(line,sizeof line,at,s->mask,10);at=pagani_script_put(line,sizeof line,at," at ");
        at=pagani_script_put_u(line,sizeof line,at,in->ms,10);
        pagani_script_put(line,sizeof line,at,in->keys_alive?"ms keys on":"ms keys off");io->log(io->ctx,line);
    }
    if (!in->controller_ready) {pagani_script_abort(s,"touch stopped",io);return;}
    if (in->real_touch) {pagani_script_abort(s,"real touch",io);return;}
    if (in->real_keys!=s->real_keys) {pagani_script_abort(s,"real key",io);return;}
    for (unsigned guard=0;guard<2*PAGANI_SCRIPT_MAX_STEPS+4;++guard) {
        if (s->index>=s->count) {
            io->keys(io->ctx,0,0);
            char line[PAGANI_SCRIPT_LINE];size_t at=pagani_script_put(line,sizeof line,0,"input script: done ");
            at=pagani_script_put_u(line,sizeof line,at,s->mask,10);at=pagani_script_put(line,sizeof line,at," in ");
            at=pagani_script_put_u(line,sizeof line,at,in->ms-s->begun,10);pagani_script_put(line,sizeof line,at,"ms");
            io->log(io->ctx,line);s->armed=0;s->running=0;return;
        }
        const struct pagani_script_step *p=s->program[s->index].step;
        switch (s->phase) {
        case PAGANI_PHASE_ENTER:
            pagani_script_enter(s,in,io);
            if (!s->armed || s->phase!=PAGANI_PHASE_GAP) return;
            continue;
        case PAGANI_PHASE_ACTIVE:
            if (p->kind==PAGANI_STEP_TAP) {
                if (in->ms>=s->t0+p->hold) {
                    if (!pagani_script_send(s,0,s->x,s->y,io)) return;
                    s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;continue;
                }
                if (in->ms>=s->next) {
                    if (!pagani_script_send(s,1,s->x,s->y,io)) return;
                    s->next=in->ms+PAGANI_SCRIPT_REPEAT_MS;
                }
                return;
            }
            if (p->kind==PAGANI_STEP_SWIPE) {
                uint64_t t=in->ms-s->t0;if (t>p->hold) t=p->hold;
                const int64_t x=p->x0+((int64_t)p->x1-p->x0)*(int64_t)t/(p->hold?p->hold:1);
                const int64_t y=p->y0+((int64_t)p->y1-p->y0)*(int64_t)t/(p->hold?p->hold:1);
                if (!pagani_script_send(s,1,pagani_script_logical((uint32_t)x,s->width),pagani_script_logical((uint32_t)y,s->height),io))
                    return;
                if (t>=p->hold) s->phase=PAGANI_PHASE_LIFT;
                return;
            }
            if (in->ms<s->t0+p->hold) return;
            io->keys(io->ctx,0,0);
            if (p->kind==PAGANI_STEP_KEY) {s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;continue;}
            s->phase=PAGANI_PHASE_WAIT;
            continue;
        case PAGANI_PHASE_LIFT:
            if (!pagani_script_send(s,0,s->x,s->y,io)) return;
            s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;continue;
        case PAGANI_PHASE_WAIT: {
            const uint64_t waited=in->ms-s->t0;
            if (in->display_off && waited<PAGANI_SCRIPT_WAKE_MS) return;
            char line[PAGANI_SCRIPT_LINE];size_t at=pagani_script_head(s,line,sizeof line,0);
            at=pagani_script_put(line,sizeof line,at,in->display_off?" power, off ":" power, on ");
            at=pagani_script_put_u(line,sizeof line,at,waited,10);pagani_script_put(line,sizeof line,at,"ms");io->log(io->ctx,line);
            s->until=in->ms+p->gap;s->phase=PAGANI_PHASE_GAP;continue;
        }
        default:
            if (in->ms<s->until) return;
            ++s->index;s->phase=PAGANI_PHASE_ENTER;continue;
        }
    }
}

// The frame the synthetic finger sends: only contact PAGANI_SCRIPT_CONTACT, down at x,y, or no contact at all, which
// s3910_hid_report turns into the lift (tip off) at the last position.
static inline struct s3910_frame pagani_script_contact(int down,uint16_t x,uint16_t y) {
    struct s3910_frame frame={};
    if (down) {frame.contact[PAGANI_SCRIPT_CONTACT].x=x;frame.contact[PAGANI_SCRIPT_CONTACT].y=y;frame.contact[PAGANI_SCRIPT_CONTACT].active=1;}
    return frame;
}
// A real contact report while the script is due (`objects`: s3910_objects of it). An object on the screen, palms included,
// ends the run, the synthetic finger lifted before the report goes out; a report without objects is held back while the
// synthetic finger is down, since it would lift it. Returns whether the caller delivers the report.
static inline int pagani_script_real_frame(struct pagani_script *s,unsigned objects,uint64_t ms,const struct pagani_script_io *io) {
    if (!pagani_script_due(s,ms)) return 1;
    if (objects) {pagani_script_abort(s,"real touch",io);return 1;}
    return !s->down;
}
// A real contact is held in the last report sent (the synthetic finger aside).
static inline int pagani_script_real_contacts(const struct pagani_script *s,const struct s3910_frame *sent) {
    for (unsigned i=0;i<S3910_CONTACTS;++i)
        if (sent->contact[i].active && !(i==PAGANI_SCRIPT_CONTACT && s->down)) return 1;
    return 0;
}

// The key device's poll: the keys the script holds join the debounced report until their failsafe time; real presses and
// ring/silent changes are counted for the abort rule. Without an armed script the report passes unchanged.
static inline uint8_t pagani_script_keys(uint8_t report,uint64_t ms) {
    if (!__atomic_load_n(&pagani_script_link.on,__ATOMIC_ACQUIRE)) return report;
    __atomic_store_n(&pagani_script_link.keys_seen,ms?ms:1,__ATOMIC_RELAXED);
    const uint8_t real=(uint8_t)(report&(PAGANI_KEYS_PMIC|PAGANI_REPORT_RING));
    const uint8_t last=pagani_script_link.last_real;
    if (pagani_script_link.primed && ((real&~last&PAGANI_KEYS_PMIC) || ((real^last)&PAGANI_REPORT_RING)))
        __atomic_fetch_add(&pagani_script_link.real_keys,1u,__ATOMIC_RELAXED);
    pagani_script_link.last_real=real;pagani_script_link.primed=1;
    const uint64_t word=__atomic_load_n(&pagani_script_link.key_word,__ATOMIC_ACQUIRE);
    if (word && ms<(word>>8)) report=(uint8_t)(report|(word&PAGANI_SCRIPT_KEY_BITS));
    return report;
}
// The touch device's side: hold `bits` until `until` (0 bits: release now).
static inline void pagani_script_hold(uint8_t bits,uint64_t until) {
    const uint8_t keys=(uint8_t)(bits&PAGANI_SCRIPT_KEY_BITS);
    __atomic_store_n(&pagani_script_link.key_word,keys?(until<<8)|keys:0,__ATOMIC_RELEASE);
}
static inline int pagani_script_keys_alive(uint64_t ms) {
    const uint64_t seen=__atomic_load_n(&pagani_script_link.keys_seen,__ATOMIC_RELAXED);
    return seen && ms>=seen && ms-seen<PAGANI_SCRIPT_KEYS_ALIVE_MS;
}
#endif
