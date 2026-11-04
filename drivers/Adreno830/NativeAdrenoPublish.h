/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PUBLISH_H
#define NATIVE_ADRENO_PUBLISH_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoStatus.h"

/* The C entry points of the ABI shell (NativeAdrenoPublish.cpp, NativeAdrenoAccelerator.cpp), ARCHITECTURE 2.2, 3.3, 6. The
 * device-level fault hooks (page-fault attribution, global loss, recovery, retire) are in NativeAdrenoStatus.h. */
class NativeAdrenoVm;
class NativeAdrenoSubmit;
class NativeAdrenoRing;
class NativeAdrenoTimer;
struct NativeAdrenoRingClock;
struct NativeAdrenoAbiDeviceInfo;
struct Adreno830Status;
class IOService;
class IOUserClient;
class IOMemoryDescriptor;
class OSDictionary;

/* GPU windows of the client slots (ARCHITECTURE 6.2), chosen from the published DeviceInfo VA. Every connection maps into the one GPU
 * address space (one SMMU context), so the windows never overlap (a GPU page fault names its connection by the faulting address) and
 * together they are the aperture's client range, but for the gaps of the 16 GiB table; more slots means smaller windows. 4 GiB aperture (va_start + va_size up to 4 GiB,
 * pagani-gpu-va=4, and what the default falls back to): slot 0, the render server's (backboardd, 934 MiB reached in int-196), 0x20000000 + 0x60000000; slots
 * 1-3 large, 0x16000000 (352 MiB) each from 0x80000000, for the long-lived Metal clients (NativeAdrenoLargeClientList); slots 4-11
 * small, 0x07C00000 (124 MiB) each from 0xC2000000, for every other process (the poster extensions, apps), the last one 8 KiB short
 * so the table ends at 0xFFFFE000 as before; slots 12-31 have no window there. 16 GiB aperture (pagani-gpu-va=16, 2026-10-30, the
 * default since 2026-10-31: every app and extension may be a Metal client, and eleven windows were all a 4 GiB space could cut: run
 * 20261031-223924 refused Maps, Spotlight, MobileSlideShow, FindMy and AegirPoster a device): slot 0 as before, slots 1-3 large
 * at 0x20000000 (512 MiB) each from 0x80000000, slots 4-31 small at 0x1C000000 (448 MiB) each from 0xE0000000, the last one (slot
 * 31, 0x1FFFE000, about 512 MiB) up to 0x3FFFFE000. No window crosses a multiple of 4 GiB: slot 4 ends at 0xFC000000, slots 5, 14 and 23
 * start at 4, 8 and 12 GiB, and the 64 MiB before each of those is nobody's. Addresses past 4 GiB are Linux's and upstream's everyday, a
 * single buffer across the line nobody could show running: a BO of a client that filled slot 5 of the first table would have been one, and
 * the first client past the line comes a minute after boot.
 * 2 GiB aperture (pagani-gpu-va=2): the old table, slot 0 0x20000000 + 0x40000000
 * and slots 1-3 0x60000000 / 0x6AAA9000 / 0x75552000, 0x0AAA9000 each; slots 4-31 have no window there (bytes 0) and are never handed
 * out.
 * Twelve slots instead of four (2026-10-30): with WidgetRenderer_Default, PosterBoard and iconservicesagent the four were full, so no
 * poster extension could be a client (none was listed either): its MTLCreateSystemDefaultDevice returned nil, and Metal or Core Image
 * (which then falls back to an OpenGL ES context that does not exist here) took it down: AegirPoster aborted 22 times and
 * KaleidoscopePoster faulted 17 times when the wallpaper gallery opened (run 20261030-144045-m8-full-1). Thirty-two (2026-10-30
 * evening): MobileSMS faulted five times in CI::GLContext::init (run 20261030-213341-island-apps-1), an app the list did not name.
 * Each slot is also a submit range (NativeAdrenoSubmitRanges) with its own reserved job. */
enum : unsigned {NativeAdrenoMaxClients=32,NativeAdrenoLargeSlots=3,NativeAdrenoListedSmallKeep4G=3,NativeAdrenoListedSmallKeep16G=4};
struct NativeAdrenoClientWindow {uint64_t start,bytes;};
inline constexpr NativeAdrenoClientWindow NativeAdrenoClientWindows4G[NativeAdrenoMaxClients]={
    {0x20000000ull,0x60000000ull},
    {0x80000000ull,0x16000000ull},{0x96000000ull,0x16000000ull},{0xAC000000ull,0x16000000ull},
    {0xC2000000ull,0x07C00000ull},{0xC9C00000ull,0x07C00000ull},{0xD1800000ull,0x07C00000ull},{0xD9400000ull,0x07C00000ull},
    {0xE1000000ull,0x07C00000ull},{0xE8C00000ull,0x07C00000ull},{0xF0800000ull,0x07C00000ull},{0xF8400000ull,0x07BFE000ull}};
inline constexpr NativeAdrenoClientWindow NativeAdrenoClientWindows2G[NativeAdrenoMaxClients]={
    {0x20000000ull,0x40000000ull},{0x60000000ull,0x0AAA9000ull},{0x6AAA9000ull,0x0AAA9000ull},{0x75552000ull,0x0AAA9000ull}};
inline constexpr NativeAdrenoClientWindow NativeAdrenoClientWindows16G[NativeAdrenoMaxClients]={
    {0x20000000ull,0x60000000ull},
    {0x80000000ull,0x20000000ull},{0xA0000000ull,0x20000000ull},{0xC0000000ull,0x20000000ull},
    {0xE0000000ull,0x1C000000ull},
    {0x100000000ull,0x1C000000ull},{0x11C000000ull,0x1C000000ull},{0x138000000ull,0x1C000000ull},{0x154000000ull,0x1C000000ull},
    {0x170000000ull,0x1C000000ull},{0x18C000000ull,0x1C000000ull},{0x1A8000000ull,0x1C000000ull},{0x1C4000000ull,0x1C000000ull},
    {0x1E0000000ull,0x1C000000ull},
    {0x200000000ull,0x1C000000ull},{0x21C000000ull,0x1C000000ull},{0x238000000ull,0x1C000000ull},{0x254000000ull,0x1C000000ull},
    {0x270000000ull,0x1C000000ull},{0x28C000000ull,0x1C000000ull},{0x2A8000000ull,0x1C000000ull},{0x2C4000000ull,0x1C000000ull},
    {0x2E0000000ull,0x1C000000ull},
    {0x300000000ull,0x1C000000ull},{0x31C000000ull,0x1C000000ull},{0x338000000ull,0x1C000000ull},{0x354000000ull,0x1C000000ull},
    {0x370000000ull,0x1C000000ull},{0x38C000000ull,0x1C000000ull},{0x3A8000000ull,0x1C000000ull},{0x3C4000000ull,0x1C000000ull},
    {0x3E0000000ull,0x1FFFE000ull}};
/* Whether a table is page aligned, ordered and without overlap inside [base, end), and its slots without a window come after the
 * last one with a window. */
constexpr bool NativeAdrenoClientWindowsValid(const NativeAdrenoClientWindow *windows,uint64_t base,uint64_t end)
{
    uint64_t at=base;
    bool closed=false;
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s) {
        if(!windows[s].bytes) {closed=true;continue;}
        if(closed || windows[s].start<at || ((windows[s].start|windows[s].bytes)&0xFFFull) || windows[s].start+windows[s].bytes>end)
            return false;
        at=windows[s].start+windows[s].bytes;
    }
    return true;
}
static_assert(NativeAdrenoClientWindowsValid(NativeAdrenoClientWindows4G,0x20000000ull,0xFFFFE000ull) &&
              NativeAdrenoClientWindowsValid(NativeAdrenoClientWindows2G,0x20000000ull,0x80000000ull) &&
              NativeAdrenoClientWindowsValid(NativeAdrenoClientWindows16G,0x20000000ull,0x3FFFFE000ull) &&
              NativeAdrenoClientWindows4G[11].start+NativeAdrenoClientWindows4G[11].bytes==0xFFFFE000ull &&
              NativeAdrenoClientWindows4G[0].bytes==0x60000000ull && !NativeAdrenoClientWindows4G[12].bytes &&
              NativeAdrenoClientWindows16G[NativeAdrenoMaxClients-1].start+NativeAdrenoClientWindows16G[NativeAdrenoMaxClients-1].bytes==
              0x3FFFFE000ull && NativeAdrenoClientWindows16G[0].bytes==0x60000000ull &&
              NativeAdrenoLargeSlots<NativeAdrenoMaxClients && NativeAdrenoMaxClients<=32,"client windows");
/* The table for the aperture the published DeviceInfo describes (va_start + va_size; 0 = none: the 4 GiB one): above 4 GiB the 16 GiB
 * table, above 2 GiB the 4 GiB one, else the 2 GiB one. */
static inline const NativeAdrenoClientWindow *NativeAdrenoClientWindowsFor(uint64_t va_end)
{
    if(!va_end)return NativeAdrenoClientWindows4G;
    return va_end>0x100000000ull?NativeAdrenoClientWindows16G:va_end>0x80000000ull?NativeAdrenoClientWindows4G:NativeAdrenoClientWindows2G;
}
/* The end (exclusive) of the highest window of a table: it must lie inside the GPU's input range. */
static inline uint64_t NativeAdrenoClientWindowsEnd(const NativeAdrenoClientWindow *windows)
{
    uint64_t end=0;
    for(unsigned s=0;windows && s<NativeAdrenoMaxClients;++s)
        if(windows[s].bytes && windows[s].start+windows[s].bytes>end)end=windows[s].start+windows[s].bytes;
    return end;
}
/* The slot whose window holds a GPU address (the page-fault attribution of ARCHITECTURE 4.12.3); NativeAdrenoMaxClients for an
 * address no window holds (the kernel range below 0x20000000 among them). */
static inline unsigned NativeAdrenoClientSlotAt(const NativeAdrenoClientWindow *windows,uint64_t address)
{
    for(unsigned s=0;windows && s<NativeAdrenoMaxClients;++s)
        if(address>=windows[s].start && address-windows[s].start<windows[s].bytes)return s;
    return NativeAdrenoMaxClients;
}
/* What the jobs in flight at a CB0 page fault say next to its FAR window (M14 P0, ARCHITECTURE 4.12.3): a verdict that is counted
 * (native_adreno_fault_attribution, the meter line's pf), never charged. ranges: the client ranges of the fences in (completed, issued]
 * the interrupt's top half read (NativeAdrenoSubmit::rangesInFlight; known: that answer exists). Every client switch drains the ring, so
 * one connection alone in flight is normally the one whose job made the access: alike, it is the connection whose window holds FAR
 * (far_slot); apart, it is another one, or FAR lies in no window (far_slot NativeAdrenoMaxClients: the kernel range, a free window);
 * undecided, nothing or several connections were in flight or the history cannot say; polled, the poll found the fault (no fences at
 * the fault: the annibale profile without 710). Apart is no proof against anybody: the faulting job may complete in the microseconds
 * before the top half reads the fences (terminate mode with HUPCF runs it to its end) and leave another connection's job alone in
 * flight, an SQE prefetch into the kernel range is nobody's (R2 cp-zap), and the kernel's own client shares range 0 with slot 0. */
enum NativeAdrenoFaultVerdict : uint32_t {NativeAdrenoFaultAlike,NativeAdrenoFaultApart,NativeAdrenoFaultUndecided,NativeAdrenoFaultPolled,
                                          NativeAdrenoFaultVerdicts};
static inline NativeAdrenoFaultVerdict NativeAdrenoFaultVerdictFor(unsigned far_slot,bool sampled,bool known,uint32_t ranges)
{
    if(!sampled)return NativeAdrenoFaultPolled;
    if(!known || !ranges || (ranges&(ranges-1)))return NativeAdrenoFaultUndecided;
    return unsigned(__builtin_ctz(ranges))==far_slot?NativeAdrenoFaultAlike:NativeAdrenoFaultApart;
}
/* native_adreno_note_page_fault's attribution: the slot whose window holds far (windows null before the shell published: none) is the
 * one charged, as before M14; the verdict above, from the submit object's fence history (null: no answer), goes to
 * counts[NativeAdrenoFaultVerdicts]. Returns that slot, NativeAdrenoMaxClients for nobody. Thread context (the history takes the Ring
 * lock). */
template<class Submit>
static inline unsigned NativeAdrenoFaultNote(const NativeAdrenoClientWindow *windows,const Submit *submit,uint64_t far,uint32_t completed,
    uint32_t issued,bool sampled,volatile uint32_t *counts)
{
    const unsigned slot=NativeAdrenoClientSlotAt(windows,far);
    uint32_t ranges=0;
    const bool known=sampled && submit && submit->rangesInFlight(completed,issued,&ranges)==kIOReturnSuccess;
    const NativeAdrenoFaultVerdict verdict=NativeAdrenoFaultVerdictFor(slot,sampled,known,ranges);
    counts[verdict]=counts[verdict]+1;
    return slot;
}
/* Process lists, comma separated. NativeAdrenoSandboxGrantList is the accelerator's NativeSandboxGrantProcess (Adreno830Start.cpp
 * kMetalPrepare): the processes whose open of the accelerator the kernel (zzw-native-adreno-sandbox-grant.patch) grants in place of the
 * sandbox's verdict, i.e. every process the seed's NativeAdrenoMetal.clients lists besides the render server; a listed process the grant
 * leaves out may be refused by its own profile and then has no device either. The poster extensions are the ones whose 21H461 code
 * creates a Metal device: MTLCreateSystemDefaultDevice in the executable (AegirPoster, MercuryPosterExtension, MonogramPosterExtension,
 * WeatherPoster) or in its poster framework (HelloPoster; WatchFacesWallpaperSupport for KaleidoscopePoster, PridePosterExtension,
 * RhizomePoster, ExtragalacticPoster; PhotosUIPrivate / PhotoImaging for the two photo providers; PaperBoardUI for LegacyPoster), Core
 * Image (KaleidoscopePoster), SpriteKit (EmojiPoster) or SceneKit (AvatarKit). Gradient, Unity, Clock, Collections and Infograph posters
 * reach no Metal device in their code and are left out. NativeAdrenoLargeClientList: the processes whose connections prefer a large
 * window (slots 1..NativeAdrenoLargeSlots): the long-lived clients that had 682 MiB windows before (R1: widgets blank in 170 MiB;
 * int-198: iconservicesagent reached 128 MiB). */
inline constexpr char NativeAdrenoSandboxGrantList[]=
    "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard,AegirPoster,KaleidoscopePoster,PhotosPosterProvider,"
    "AmbientPhotoFramePosterProvider,MercuryPosterExtension,MonogramPosterExtension,HelloPosterExtension,PridePosterExtension,"
    "RhizomePoster,ExtragalacticPoster,EmojiPosterExtension,AvatarPosterExtension,WeatherPoster,LegacyPoster";
inline constexpr char NativeAdrenoLargeClientList[]="WidgetRenderer_Default,PosterBoard,iconservicesagent";
/* The accelerator's NativeSandboxGrantAll (2026-10-30): with the optional XNU patch zzwc-native-adreno-sandbox-grant-all.patch every
 * process's open of this accelerator's user client is granted in place of its sandbox's verdict (setting its properties is not), so
 * an app or extension the plugin's NativeAdrenoMetal.clients names by class (@apps, @extensions) is never refused by a profile that
 * does not name AGXDeviceUserClient. Without that patch the property is inert and only NativeAdrenoSandboxGrantList is granted. Which
 * processes get a device stays the plugin's rule. */
inline constexpr bool NativeAdrenoSandboxGrantAll=true;
/* Whether a comma-separated list names a process, as proc_selfname copied its name into a buffer of capacity bytes (at most capacity - 1
 * characters): an entry shorter than that must equal the whole name, a longer one is compared over what the buffer holds. This is the
 * kernel grant's own rule with capacity 17 (zzw-native-adreno-sandbox-grant.patch); the publish shell uses its 20-byte name buffer. */
static inline bool NativeAdrenoListNamesProcess(const char *list,const char *name,size_t capacity)
{
    if(!list || !name || capacity<2)return false;
    for(const char *at=list;*at;) {
        size_t length=0;
        while(at[length] && at[length]!=',')++length;
        const size_t compared=length<capacity-1?length:capacity-1;
        size_t same=0;
        while(same<compared && name[same]==at[same])++same;
        if(length && same==compared && (length>=capacity-1 || !name[length]))return true;
        at+=length+(at[length]==','?1:0);
    }
    return false;
}
/* The slot a new GPU connection takes (ARCHITECTURE 6.2): slot 0 (the render server's window) for the render server when free; the
 * render server's second connection and NativeAdrenoLargeClientList a free large slot (1..NativeAdrenoLargeSlots), else a small one. A
 * small-class process the grant list names (listed: NativeAdrenoSandboxGrantList, the poster extensions) a free small slot, else
 * borrows a free large one. Any other process (an app or extension the plugin admits by class, 2026-10-30) only a small slot, and only
 * while more than NativeAdrenoListedSmallKeep4G (the 4 GiB table's eight) or NativeAdrenoListedSmallKeep16G (the 16 GiB table's 28)
 * of them are free: the listed clients always find a window however many apps hold one (with apps in every small slot the
 * lock-screen poster could not get a device and went black). The 2 GiB table has no small window: only listed processes get one of
 * slots 1-3 there. NativeAdrenoMaxClients when nothing is free for the connection. Slot 0 is never another process's. */
static inline unsigned NativeAdrenoListedSmallKeep(const NativeAdrenoClientWindow *windows)
{
    return windows==NativeAdrenoClientWindows16G?unsigned(NativeAdrenoListedSmallKeep16G):unsigned(NativeAdrenoListedSmallKeep4G);
}
static inline unsigned NativeAdrenoClientSlotFor(bool render_server,bool large,bool listed,const NativeAdrenoClientWindow *windows,
    const bool taken[NativeAdrenoMaxClients])
{
    if(!windows || !taken)return NativeAdrenoMaxClients;
    if(render_server && windows[0].bytes && !taken[0])return 0;
    unsigned first_large=NativeAdrenoMaxClients,first_small=NativeAdrenoMaxClients,small_free=0;
    for(unsigned s=1;s<NativeAdrenoMaxClients;++s) {
        if(!windows[s].bytes || taken[s])continue;
        if(s<=NativeAdrenoLargeSlots) {if(first_large==NativeAdrenoMaxClients)first_large=s;}
        else {if(first_small==NativeAdrenoMaxClients)first_small=s;++small_free;}
    }
    if(render_server || large)return first_large<NativeAdrenoMaxClients?first_large:first_small;
    if(listed)return first_small<NativeAdrenoMaxClients?first_small:first_large;
    return small_free>NativeAdrenoListedSmallKeep(windows)?first_small:NativeAdrenoMaxClients;
}
/* Why an open found no slot, in one log line (adreno830_status_diag_lines, ARCHITECTURE 6.2): the process, its class (L large, l listed, a any
 * other), the free small windows against the ones kept for the listed processes, then every holder as <slot>:<name>/<seconds without a
 * call into the driver>, "~" before the name of an orphan, the name cut to 10 characters. That table is what the choice between more
 * windows and giving an idle connection's back needs: run 20261031-223924 knew only that the opens were refused. "+<n>" counts the
 * lines the rate limit dropped since the last one. At most 26 + 19 + 21 x 32 characters: BootProgress continues it in '+' records.
 * NativeAdrenoSlotLogDue is the rate limit: one line per 5 s, 40 a boot, so a crash loop's opens cannot fill the events ring. */
enum : unsigned {NativeAdrenoSlotLineBytes=720,NativeAdrenoSlotLogLines=40,NativeAdrenoSlotNameChars=10};
enum : uint64_t {NativeAdrenoSlotLogUs=5000000};
struct NativeAdrenoSlotHolder {const char *name;uint32_t quiet_s;bool orphan;};
struct NativeAdrenoSlotLog {uint64_t next_us;uint32_t lines,skipped;};
static inline bool NativeAdrenoSlotLogDue(NativeAdrenoSlotLog *log,uint64_t now_us)
{
    if(log->lines>=NativeAdrenoSlotLogLines || now_us<log->next_us) {++log->skipped;return false;}
    ++log->lines;log->next_us=now_us+NativeAdrenoSlotLogUs;
    return true;
}
static inline unsigned NativeAdrenoClientSmallFree(const NativeAdrenoClientWindow *windows,const bool taken[NativeAdrenoMaxClients])
{
    unsigned free=0;
    for(unsigned s=NativeAdrenoLargeSlots+1;windows && taken && s<NativeAdrenoMaxClients;++s)if(windows[s].bytes && !taken[s])++free;
    return free;
}
static inline void NativeAdrenoSlotLine(char *line,const char *name,char kind,unsigned small_free,unsigned keep,uint32_t skipped,
    const NativeAdrenoSlotHolder holders[NativeAdrenoMaxClients])
{
    size_t at=0;
    const auto put=[&](char c) {if(at+1<NativeAdrenoSlotLineBytes)line[at++]=c;};
    const auto text=[&](const char *s,size_t limit) {for(size_t i=0;s && i<limit && s[i];++i)put(s[i]);};
    const auto number=[&](uint32_t value) {
        char digits[10];
        unsigned n=0;
        do {digits[n++]=char('0'+value%10);value/=10;} while(value);
        while(n)put(digits[--n]);
    };
    text("gpu noslot ",11);
    text(name,19);
    put(' ');put(kind);
    text(" small ",7);
    number(small_free);put('/');number(keep);
    if(skipped) {text(" +",2);number(skipped);}
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s) {
        if(!holders[s].name)continue;
        put(' ');number(s);put(':');
        if(holders[s].orphan)put('~');
        text(holders[s].name,NativeAdrenoSlotNameChars);
        put('/');number(holders[s].quiet_s<9999?holders[s].quiet_s:9999);
    }
    line[at]=0;
}
/* The first mapping of a boot that reaches past 4 GiB (end, the connection's windowPeak(), is exclusive): the line a page fault or a broken
 * picture has to be read against, since no run has logged a GPU address past 4 GiB yet. Slot 5 of the 16 GiB table starts on that line, so it is the
 * first mapping of the first client there; the address still decides, not the slot: a mapping that ends at 4 GiB is below it, and one that crosses
 * it, which no window lets happen any more, would count. "gpu va past 4 GiB: <process> slot <n> iova 0x<mapping> end 0x<end>", at most 93 characters. */
enum : uint64_t {NativeAdrenoVa4G=UINT64_C(0x100000000)};
enum : unsigned {NativeAdrenoPast4GLineBytes=100};
static inline bool NativeAdrenoPast4G(uint64_t end) {return end>NativeAdrenoVa4G;}
static inline void NativeAdrenoPast4GLine(char *line,const char *name,unsigned slot,uint64_t iova,uint64_t end)
{
    size_t at=0;
    const auto put=[&](char c) {if(at+1<NativeAdrenoPast4GLineBytes)line[at++]=c;};
    const auto text=[&](const char *s,size_t limit) {for(size_t i=0;s && i<limit && s[i];++i)put(s[i]);};
    const auto hex=[&](uint64_t value) {
        text("0x",2);
        unsigned shift=60;
        while(shift && !(value>>shift))shift-=4;
        for(;;shift-=4) {
            put("0123456789abcdef"[(value>>shift)&15]);
            if(!shift)break;
        }
    };
    text("gpu va past 4 GiB: ",19);
    text(name,19);
    text(" slot ",6);
    if(slot>=10)put(char('0'+slot/10));
    put(char('0'+slot%10));
    text(" iova ",6);hex(iova);
    text(" end ",5);hex(end);
    line[at]=0;
}
/* The objects a connection can hold (NativeAdrenoUserClient::open's capacity, a power of two): the render server's (and the kernel
 * client's) 16384, a large client's 8192, everyone else's 4096. Each object costs its connection 200 bytes of record, 32 of index
 * and 16 of CPU-view cache up front: 32 connections at 16384 would pin 128 MiB. */
static inline size_t NativeAdrenoClientObjectsFor(bool render_server,bool large)
{
    return render_server?16384u:large?8192u:4096u;
}
/* What mount() gives a connection besides its slot: objects, the objects it can hold (NativeAdrenoClientObjectsFor); reserve, whether it
 * leaves the render server its reserve of the shared VM tables and the BO pool (NativeAdrenoUserClient::setReserve); mailbox, whether it
 * serves the mailbox page. The render server's share goes with the process, whatever slot it got: a backboardd restarted while its
 * dead predecessor's orphan still held slot 0 lands in a large slot and must not get a large client's 8192 objects and the reserves
 * meant for the others (its own BoMap could be refused). The mailbox stays with the render server's slot 0. The kernel's own client
 * (a board self-test) holds 16384 objects and keeps no reserve. */
struct NativeAdrenoClientTerms {size_t objects;bool reserve,mailbox;};
static inline NativeAdrenoClientTerms NativeAdrenoClientTermsFor(bool kernel,bool render_server,bool large,unsigned slot)
{
    const bool own=kernel || render_server;
    return {NativeAdrenoClientObjectsFor(own,large),!own,!kernel && render_server && slot==0};
}
/* The share of the BO pool the render server keeps (pagani-gpu-pool / NativeAdrenoPoolRenderShare): every other client allocates from
 * the pool only while that much of it stays free, else from contiguous or scattered pages as when the pool is full
 * (NativeAdrenoUserClient::setReserve). */
enum : size_t {NativeAdrenoPoolRenderShare=4};
/* GPU hangs a process name may cause before its opens are refused for the rest of the boot (the culprit is the client range whose job
 * the CP stood in when the hang was detected, NativeAdrenoSubmit::halt). A830Recovery gives up the GPU for good at the third recovery
 * within 60 s, and an app that hangs it every launch would take every process's GPU with it. The render server is never refused. */
enum : unsigned {NativeAdrenoClientHangLimit=2};
/* The open policy of ARCHITECTURE 4.13.4 and 6.2 for a GPU connection: a global device loss refuses it (a page fault never does);
 * limit (sheng-gpu-metal-opens, 0 = unlimited) caps the connections granted to one process name since its last healthy one; a name
 * whose jobs hung the GPU NativeAdrenoClientHangLimit times is refused (kIOReturnNotPermitted: its plugin then has no device and
 * Core Image no Metal context); there is no other per-process cap, only the NativeAdrenoMaxClients slots at a time. 0 = admit. */
static inline IOReturn NativeAdrenoOpenRefusal(bool device_lost,unsigned limit,unsigned opens_so_far,unsigned hangs=0)
{
    if(device_lost)return NativeAdrenoReturnNotResponding;
    if(hangs>=NativeAdrenoClientHangLimit)return NativeAdrenoReturnNotPermitted;
    if(limit && opens_so_far>=limit)return kIOReturnBusy;
    return kIOReturnSuccess;
}
/* One row of the publish shell's per-name table (native_adreno_client_log, NativeAdrenoPublish.cpp): the process name as the shell's
 * 20-byte buffer holds it, opens granted, refused, closes, the slot of its live connection (0xff none), the highest GPU address its
 * mappings reached (MiB), its last refusal, and the GPU hangs its jobs caused (NativeAdrenoClientHangLimit). */
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal, hangs;};
/* Whether a row holds name (compared as strncmp over the row's 20 bytes). */
static inline bool NativeAdrenoClientRecordIs(const NativeAdrenoClientRecord &row,const char *name)
{
    for(size_t i=0;i<sizeof row.name;++i) {
        if(row.name[i]!=name[i])return false;
        if(!name[i])return true;
    }
    return true;
}
/* The row of a process name among count rows: its own, else the first unused one, else (every row used: every app may be a client
 * since 2026-10-30) the row of a name with no live connection (live(name) false: no connection or orphan holds a slot) and no hang,
 * the one with the fewest opens first, cleared for name. A row charged with hangs is never given up, so a name refused for its hangs
 * stays refused however many other names opened since (with no row at all its hangs were neither counted nor refused), nor is a row
 * of a live name. live() (the shell walks its 32 slots) is asked only with every row used: a name's own row, which every BoMap
 * looks up under g.lock, and an unused one are found without it. *fresh: the row was (re)initialised, the caller restarts its open
 * streak. Null only when every row is live or charged. The caller serialises the table (the shell's g.lock). */
template<class Live>
static inline NativeAdrenoClientRecord *NativeAdrenoClientRecordFor(NativeAdrenoClientRecord *rows,size_t count,const char *name,Live live,
    bool *fresh)
{
    if(fresh)*fresh=false;
    if(!rows || !name)return nullptr;
    NativeAdrenoClientRecord *unused=nullptr,*idle=nullptr;
    for(size_t i=0;i<count;++i) {
        NativeAdrenoClientRecord &row=rows[i];
        if(!row.name[0]) {if(!unused)unused=&row;continue;}
        if(NativeAdrenoClientRecordIs(row,name))return &row;
    }
    for(size_t i=0;!unused && i<count;++i) {
        NativeAdrenoClientRecord &row=rows[i];
        if(!row.hangs && (!idle || row.opens<idle->opens) && !live(row.name))idle=&row;
    }
    NativeAdrenoClientRecord *row=unused?unused:idle;
    if(!row)return nullptr;
    *row=NativeAdrenoClientRecord{};
    for(size_t n=0;n+1<sizeof row->name && name[n];++n)row->name[n]=name[n];
    row->slot=0xff;
    if(fresh)*fresh=true;
    return row;
}

/* The display hand-off methods forwarded unchanged to the presenter (NGP_METHOD 0..5 = MODE, PRESENT, WAIT_SWAP, COMPLETE_SWAP,
 * BAND (M9), WAIT_SWAP2 (M12)), without any GPU lock; memory type NativeAdrenoScanoutType is the scanout. */
enum : uint32_t {NativeAdrenoHandoffBase=0x7000,NativeAdrenoHandoffLast=0x7005,NativeAdrenoScanoutType=0x5343,
                 NativeAdrenoPluginOpenType=0x4147,NativeAdrenoHandoffOpenType=0x4148};
/* The frontend's second (2026-11-01): the render server's record for the per-second line (Adreno830StatusFrontend, AdrenoMetal's
 * FrontendSecond.h), a tunnelled method on any of its connections. Only from the render server; kIOReturnUnsupported without the line, which
 * ends the plugin's sender for good. */
enum : uint32_t {NativeAdrenoFrontendMethod=0x7101};
/* Whether a tunnelled method number is forwarded to the presenter (0x7000..0x7005; M9 widened the range from 0x7003). */
static inline bool NativeAdrenoIsHandoffMethod(uint32_t method)
{
    return method>=NativeAdrenoHandoffBase && method<=NativeAdrenoHandoffLast;
}

#ifdef __cplusplus
extern "C" {
#endif
/* S9 (ARCHITECTURE 3.3): publishes NativeAdrenoGpuPublished ("NativeAdrenoGpu") once. Everything passed in must outlive the system
 * and be attached to the live GPU context; output_bits is IDR2.OAS (36), input_bits the GPU context's input size (32, or 34 for the
 * 16 GiB aperture: NativeAdrenoGpuContext::inputBits()), which the client windows of the DeviceInfo VA must fit. The shared submit
 * object gets the shell's Ring/Memory locks here (M10), so nothing may use it concurrently from now on except through the shell or
 * with those locks.
 * Fence waits (WaitFence, and the submit retry of a caller with nothing in flight) sleep in the ring's completion wait without
 * any lock of the shell (NativeAdrenoRing::observeFence): A830Fence::wait once the orchestrator installed it as the ring's waiter
 * (NativeAdrenoRing::setWaiter, ARCHITECTURE 4.13.3), else polling with the clock's idle hook. */
IOReturn native_adreno_publish(NativeAdrenoVm *vm,NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,
    const NativeAdrenoRingClock *clock,const NativeAdrenoTimer *timer,const NativeAdrenoAbiDeviceInfo *info,
    uint32_t input_bits,uint32_t output_bits);
/* The two hooks Adreno830Status.cpp calls (weak empty defaults there, the definitions here; thread context, never a hot path):
 * the status block as the OSData property "Adreno830Status" of NativeAdrenoGpuPublished whenever its rows changed (a no-op
 * before S9; ARCHITECTURE 8.2), and rows 1-3 of the pagani-gpu-status=3 page (ARCHITECTURE 8.4): the plugin's latest report
 * line, its Turnip-phase line, and "CLI o<opens> c<calls> s<last status> NGP <offers>/<done>/<fail>"; returns the rows
 * filled (3), or 0 while no plugin reached the driver (the diagnostic page stays). Each row NUL-terminated, <= 66 characters. */
void native_adreno_publish_status(const Adreno830Status *status);
unsigned native_adreno_report_rows(char rows[3][68]);
/* The coherence level the plugin may rely on (NativeAdrenoCoherentImports, user-space patch M8, ARCHITECTURE 4.14.1): the VM's
 * effective level (NativeAdrenoVm::coherence(): 0 none, 1 GPU-read-only imports coherent after V9a, 2 also GPU-written imports after
 * V9b/V9c with pagani-gpu-coherent=2), never above 1 unless the published DeviceInfo carries NativeAdrenoDeviceCachedCoherent. */
uint32_t native_adreno_coherent_imports(void);
/* The promise behind the plugin's seed rule (AdrenoMetal SurfaceMaintenance.h, METAL2VULKAN_SURFACE_CLEAN=seed), a property of the accelerator and
 * the published service, value 1: NativeAdrenoVm::mapLocked cleans and invalidates an import's pages through their physical addresses, once,
 * after the pin and before the GPU can see them, so lines the zero fill or the pages' last owner left dirty are gone before the first GPU
 * write (ARCHITECTURE 4.14.1). A change of that order must take this property away. */
#define NativeAdrenoImportCleanKey "NativeAdrenoImportClean"
/* Connections opened and closed so far, and the published service (null before S9). */
uint32_t native_adreno_publish_stats(uint32_t *closed);
IOService *native_adreno_published_service(void);
/* IOService::newUserClient of both services (provider = the service; limit = sheng-gpu-metal-opens, 0 = unlimited, connections ever
 * granted per process name). */
IOReturn native_adreno_new_user_client(IOService *provider,task_t owningTask,void *securityID,uint32_t type,OSDictionary *properties,
    IOUserClient **handler,unsigned limit);
void native_adreno_note_property_read(const char *key);
/* Weak defaults for boards without the provider (the statically linked kernel refuses undefined weak references): the display
 * hand-off (PD NativeGOPPresenter) answers kIOReturnUnsupported and has no scanout; the UFS log export refuses (-1). */
IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize);
IOMemoryDescriptor *native_gop_handoff_memory(void);
/* The connection through which task reached the hand-off methods closed (weak no-op default): the presenter abandons a frame it
 * delivered to that task and drops its mappings there, as for the end of its own user client. */
void native_gop_handoff_closed(task_t task);
int boot_log_export_put(uint32_t generation,uint32_t total,uint32_t offset,const void *data,uint32_t size);
/* NativeAdrenoAccelerator.cpp: the staged AGXAccelerator (IOAcceleratorES, 136 bytes). publishMetal allocates, init()s, sets the
 * three discovery properties, attaches and registers it; registerService also publishes NativeAdrenoCoherentImports (unless it was
 * set before), and native_adreno_accelerator_set_coherent_imports sets or changes it explicitly. */
IOService *native_adreno_accelerator_allocate(void);
unsigned native_adreno_accelerator_instance_size(void);
void native_adreno_accelerator_set_coherent_imports(IOService *accelerator,uint32_t level);
#ifdef __cplusplus
}
#endif
#endif
