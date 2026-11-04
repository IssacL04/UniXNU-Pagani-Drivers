/* SPDX-License-Identifier: MIT */
#include "Adreno830Status.h"
#include "A830Profile.h"
#include <string.h>

extern "C" {
Adreno830Status adreno830_status={.magic=Adreno830StatusMagic,.version=Adreno830StatusVersion,.bytes=uint16_t(sizeof(Adreno830Status))};
/* Weak empty defaults for boards without the boot overlay or before the publish shell provides the registry property and
 * the user-space report page; PD/drivers/BootProgress and NativeAdrenoPublish.cpp supply the strong definitions. A
 * statically linked kernel refuses undefined weak references, so the defaults are definitions, not weak imports. */
__attribute__((weak)) void boot_progress_gpu_rows(const char (*)[Adreno830StatusRowBytes],unsigned) {}
__attribute__((weak)) void boot_progress_display_status(const char *,uint64_t) {}
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) int boot_progress_trace_enabled(void) {return 0;}
__attribute__((weak)) void native_adreno_publish_status(const Adreno830Status *) {}
__attribute__((weak)) unsigned native_adreno_report_rows(char (*)[Adreno830StatusRowBytes]) {return 0;}
__attribute__((weak)) unsigned native_adreno_client_tick(Adreno830StatusClients *) {return 0;}
}

namespace {
const char *const stageNames[A830StageCount]={"INPUTS","POWER","SECCX","SMMU","FWMEM","GMU-HFI","CP","ZAP","FENCE","METAL","DCVS"};
const char *const stageTitles[A830StageCount]={"GPU board inputs","GPU power","GPU secure CX","GPU SMMU","GPU firmware memory",
    "GPU GMU HFI","GPU command processor","GPU ZAP secure exit","GPU memory fence","GPU Metal service","GPU DCVS"};
unsigned overlayMode=1;
bool diagLines=false;
unsigned publishedCount;
bool published;
uint32_t publishLock;
uint64_t runKnobs;   /* A830StatusKnobs of the knobs S0 accepted; ORed into the run page's aux[0] by adreno830_status_dcvs */
uint64_t runTemp;    /* A830StatusTemp of the DCVS thread's last valid reading; ORed in the same way */
uint64_t runThermal; /* A830StatusThermal of the design profile's thermal loop; ORed into the run page's aux[3] the same way */
uint64_t runVm;      /* MiB mapped in the GPU VM << 49, ORed into the run page's aux[4] by adreno830_status_memory */
/* The ring's queue period the DCVS thread stored last (adreno830_status_queue): not in the block, laid out on row 3 of the run
 * page by every publish. Both under the publish lock. */
Adreno830StatusQueue runQueue;
bool runQueueSet;
/* The GPU clients the DCVS thread stored last (adreno830_status_clients): not in the block either, laid out at the end of the run
 * page's MEM row. Under the publish lock. */
Adreno830StatusClients runClients;
bool runClientsSet;
/* The render server's frontend since the last per-second line (adreno830_status_frontend). Under the publish lock. */
Adreno830StatusFrontend runFrontend;
/* The ramoops meter line (adreno830_status_meter, M14 P0): the queue periods folded since the last line (polled: those whose
 * completions were seen without the fence interrupt), the last line written ("": none yet) and the CP_SMMU_APERTURE outcome S3 stored
 * (state + 1; 0: none). Under the publish lock. */
struct MeterWindow {
    uint32_t periods,polled,rs_jobs,rs_max_us,fw_jobs,fw_max_us,fw_over4,ext_jobs,contexts,contexts_max;
    uint64_t rs_us,fw_us,ext_us;
    /* The calls (Adreno830StatusCalls): the longest of the window, the sums of the rest. */
    uint32_t call_max_us[Adreno830CallKinds],wait_max_us[Adreno830WaitKinds];
    uint64_t import_maps,shared_maps,shared_us,map_us,unmap_us,zero_kib,zero_us,contig_tries,contig_fails,outside_bos;
};
MeterWindow meterWindow;
char meterLast[Adreno830StatusMeterBytes];
uint32_t runAperture;

/* One overlay row: appends never pass Adreno830StatusRowMax characters (or the limit given: the meter line's) and the text stays
 * NUL terminated. The layouts below are sized so that no field is ever cut (tests/test_status.cpp checks the worst cases); the
 * fields that may not fit (the DCVS residency, the client figures, sw) check room() first. */
class Row {
    char *text_;
    unsigned used_=0,limit_;
public:
    explicit Row(char *text,unsigned limit=Adreno830StatusRowMax):text_(text),limit_(limit) {text_[0]=0;}
    unsigned room() const {return limit_-used_;}
    unsigned used() const {return used_;}
    Row &put(char c) {if(used_<limit_){text_[used_++]=c;text_[used_]=0;}return *this;}
    Row &str(const char *s) {while(s && *s)put(*s++);return *this;}
    Row &dec(uint64_t value) {
        char digits[20];unsigned n=0;
        do {digits[n++]=char('0'+value%10);value/=10;} while(value);
        while(n)put(digits[--n]);
        return *this;
    }
    Row &hex(uint64_t value,unsigned width) {
        char digits[16];unsigned n=0;
        do {digits[n++]="0123456789ABCDEF"[value&15];value>>=4;} while(value || n<width);
        while(n)put(digits[--n]);
        return *this;
    }
    Row &optional(bool valid,uint64_t value) {return valid?dec(value):put('-');}
    Row &optionalHex(bool valid,uint64_t value,unsigned width) {return valid?hex(value,width):put('-');}
    /* A growing counter in at most `width` (2..19) characters: in full while it has at most width digits, else in thousands,
     * millions, ... (k, M, G, T, P, E; rounded down) with at most width - 1 digits, so it never pushes a later field off. */
    Row &count(uint64_t value,unsigned width) {
        uint64_t limit=1;
        for(unsigned i=0;i<width;++i)limit*=10;
        if(value<limit)return dec(value);
        limit/=10;
        static const char suffixes[]="kMGTPE";
        for(unsigned i=0;suffixes[i];++i) {
            value/=1000;
            if(value<limit)return dec(value).put(suffixes[i]);
        }
        return dec(limit-1).put('E');
    }
};

uint64_t clamp(uint64_t value,uint64_t limit) {return value<limit?value:limit;}
uint32_t lo(uint64_t word) {return uint32_t(word);}
uint32_t hi(uint64_t word) {return uint32_t(word>>32);}
/* The S5 page's annibale fields exist once S0 accepted that profile: the design profile reads none of them. */
bool annibaleFields(const Adreno830Status *s) {return s->profile_set && s->profile==A830ProfileAnnibale;}
/* " L<messages>/<acks>/<errors>" of the late HFI drain (hfi_late, A830StatusLate) once one ran, each shown up to 99. */
void lateDrain(Row &r,uint32_t late)
{
    if(!(late&0xff))return;
    r.str(" L").dec(clamp((late>>8)&0xff,99)).put('/').dec(clamp((late>>16)&0xff,99)).put('/').dec(clamp(late>>24,99));
}

void row0(const Adreno830Status *s,Row r)
{
    const char *name=adreno830_stage_name(s->stage);
    r.str("GPU ");
    switch(s->state) {
    case A830Failed:
        /* Physical addresses fit 36 bits (9 digits); a value that fits 32 bits is shown as 8 digits, otherwise 16. */
        r.put('S').dec(s->stage).put('.').hex(s->step,2).put(' ').str(name).str(" FAIL ").hex(s->ioreturn,8).put(' ')
         .hex(s->reg&0xfffffffffULL,1).put('=').hex(s->value,s->value>>32?16:8).str(" r").dec(s->reason);
        break;
    case A830Stopped:
        r.put('S').dec(s->stage).put(' ').str(name).str(" STOP ok ").dec(s->boot_ms).str("ms");
        break;
    case A830Lost:
        r.str("LOST S").dec(s->stage).put('.').hex(s->step,2).put(' ').hex(s->ioreturn,8).str(" r").dec(s->reason)
         .str(" f").dec(s->faults);
        break;
    case A830Recovering:
        r.str("RECOVER ").dec(s->recoveries).str(" S").dec(s->stage).put(' ').dec(s->boot_ms).str("ms");
        break;
    case A830Slumber:   /* aux[5]: seconds | wakes << 24 | last wake ms << 48 */
        r.str("SLP ").dec(s->aux[5]&0xffffff).str("s w").dec((s->aux[5]>>24)&0xffffff).put(' ').dec(s->aux[5]>>48).str("ms");
        break;
    case A830Done:
        if(s->stage==A830SRunning) {
            /* Every field is clamped so the worst case is exactly 66 characters; an interrupt the storm guard turned
             * off (reason 23) is appended when it fits. */
            r.str("RUN ").dec(clamp(s->mhz,9999)).str("M b").dec(clamp(s->busy_permille/10,100)).str(" g")
             .dec(clamp(s->gmu_level,255)).str("/b").dec(clamp(s->bus_level,255)).str(" s").dec(s->submits%1000000000000ULL)
             .str(" f").dec(s->faults).str(" h").dec(s->hangs);
            if(s->irq_off && r.room()>=4)r.str(" r23");
        } else r.put('S').dec(s->stage).put(' ').str(name).str(" DONE ").dec(s->boot_ms).str("ms");
        break;
    case A830Busy:
        r.put('S').dec(s->stage).put('.').hex(s->step,2).put(' ').str(name).str(" RUN ").dec(s->boot_ms).str("ms");
        break;
    default:
        r.put('S').dec(s->stage).put(' ').str(name).str(" IDLE");
        break;
    }
    /* The bring-up profile in effect (pagani-gpu-profile, A830Profile.h), last, whenever it fits whole. */
    const unsigned digits=s->profile>=100?3u:s->profile>=10?2u:1u;
    if(s->profile_set && r.room()>=2u+digits)r.str(" p").dec(s->profile);
    /* The RUN row then shows the performance knobs of the run page (A830StatusKnobs), each when it fits whole:
     * " m<pagani-gpu-maxlevel>", " i1" for pagani-gpu-irq=1 with 332 live, " i0" once it fell back to polling. */
    if(s->profile_set && s->state==A830Done && s->stage==A830SRunning) {
        const uint32_t maxlevel=uint32_t(s->aux[0]>>A830StatusKnobsShift)&0xff;
        const bool irq=(s->aux[0]>>(A830StatusKnobsShift+8))&1;
        if(maxlevel && r.room()>=2u+(maxlevel>=100?3u:maxlevel>=10?2u:1u))r.str(" m").dec(maxlevel);
        if(irq && r.room()>=3u)r.str(" i").put(s->irq_off&A830IrqOffGpu?'0':'1');
        const uint32_t celsius=uint32_t(s->aux[0]>>A830StatusTempShift)&0x7f;
        if(celsius && r.room()>=2u+(celsius>=100?3u:celsius>=10?2u:1u))r.put(' ').dec(celsius).put('C');
    }
}

using Rows=char[][Adreno830StatusRowBytes];

void inputsPage(const Adreno830Status *s,Rows rows)
{
    const uint64_t valid=s->aux[0];
    Row(rows[1]).str("SOC ").optional(valid&A830S0SocId,s->soc_id).str(" FC ").optional(valid&A830S0FeatureCode,s->feature_code)
        .str(" BIN ").optional(valid&A830S0SpeedBin,s->speed_bin).str(" FW ")
        .str(!(valid&A830S0FirmwareChecked)?"-":valid&A830S0FirmwareOk?"ok":"bad");
    Row arc(rows[2]);
    arc.str("ARC");
    static const char *const rails[]={" gfx "," mx "," cx "," gmxc "};
    for(unsigned i=0;i<4;++i) {
        const unsigned count=unsigned(s->aux[1]>>(8*i))&0xff;
        arc.str(rails[i]).optional((valid&A830S0Arc) && count!=0xff,count);
    }
    Row bcm(rows[3]);
    bcm.str("BCM");
    static const char *const names[]={" SH0 "," MC0 "};
    for(unsigned i=0;i<2;++i) {
        if(valid&A830S0Bcm)bcm.str(names[i]).dec(lo(s->aux[2+i])).put('/').dec(uint16_t(s->aux[2+i]>>32));
        else bcm.str(names[i]).put('-');
    }
    bcm.str(" ACV ").optional(valid&A830S0Bcm,uint8_t(s->aux[4])).str(" CN0 ").optional(valid&A830S0Bcm,uint8_t(s->aux[4]>>8));
}

/* S1/S2: "PWR st<step> <blk>+<off>=<val> PLL <mode>", "SCM x0 <x0> x1 <x1> n<calls> FUSE <fuse>",
 * "TCS <slot> st<status> c<cmd0> <cmd1> <cmd2>". */
void powerPage(const Adreno830Status *s,Rows rows)
{
    const bool power=s->aux[5]&A830S1Power,scm=s->aux[5]&A830S1Scm,tcs=s->aux[5]&A830S1Tcs;
    static const char *const blocks[]={"GCC","GPUCC"};
    Row pwr(rows[1]);
    pwr.str("PWR st");
    if(power) {
        const unsigned block=unsigned(s->aux[0]>>8)&0xff;
        pwr.dec(s->aux[0]&0xff).put(' ').str(block<2?blocks[block]:"?").put('+').hex(hi(s->aux[0])&0xfffff,1).put('=')
           .hex(lo(s->aux[1]),8).str(" PLL ").hex(hi(s->aux[1]),8);
    } else pwr.str("- - PLL -");
    Row smc(rows[2]);
    smc.str("SCM x0 ");
    if(scm) {
        smc.hex(lo(s->aux[2]),1).str(" x1 ").hex(lo(s->aux[3]),1).str(" n").dec(clamp(hi(s->aux[3]),99999)).str(" FUSE ")
           .hex(lo(s->aux[4]),8);
    } else smc.str("- x1 - n- FUSE -");
    Row t(rows[3]);
    t.str("TCS ");
    if(tcs) {
        const uint64_t w=s->aux[5];
        const unsigned slot=unsigned(w&0xf);
        t.optional(slot!=0xf,slot).str(" st").dec((w>>4)&0xf).str(" c").hex((w>>8)&0x1ffff,1).put(' ').hex((w>>25)&0x1ffff,1)
         .put(' ').hex((w>>42)&0x1ffff,1);
    } else t.str("- st- c- - -");
}

/* S3: "SMMU ID <idr0> <idr1> <idr2> CR0 <scr0>", "ACR <sacr> GFSR <gfsr> SLOT <g>/<l>/<m> S2C <s2cr>[ AP<state>]",
 * "CB0 <sctlr> <actlr> FSR <fsr> CB2 <sctlr> <fsr> F<smmu_faults>". AP: the CP_SMMU_APERTURE outcome once recorded (aux[3] bits
 * 56-58, A830StatusAperture); the ACR row is then 54 characters. */
void smmuPage(const Adreno830Status *s,Rows rows)
{
    Row(rows[1]).str("SMMU ID ").hex(lo(s->aux[0]),8).put(' ').hex(hi(s->aux[0]),8).put(' ').hex(lo(s->aux[1]),8).str(" CR0 ")
        .hex(hi(s->aux[1]),8);
    const uint32_t slots=hi(s->aux[3]);
    Row acr(rows[2]);
    acr.str("ACR ").hex(lo(s->aux[2]),8).str(" GFSR ").hex(hi(s->aux[2]),8).str(" SLOT ").dec(slots&0xff).put('/')
       .dec((slots>>8)&0xff).put('/').dec((slots>>16)&0xff).str(" S2C ").hex(lo(s->aux[3]),8);
    const uint64_t aperture=s->aux[3]>>A830StatusApertureShift;
    if(aperture&4)acr.str(" AP").dec(aperture&3);
    Row(rows[3]).str("CB0 ").hex(lo(s->aux[4]),8).put(' ').hex(hi(s->aux[4]),8).str(" FSR ").hex(s->sticky[A830StickyCb0Fsr],8)
        .str(" CB2 ").hex(lo(s->aux[5]),8).put(' ').hex(s->sticky[A830StickyCb2Fsr],8).str(" F").dec(clamp(s->smmu_faults,99999999));
}

/* S4: "CXT <counter> SKEW <ticks>", "GMU BLK <n> DDR <bytes> TCM <bytes>", "SQE <bytes> AQE <bytes>". */
void firmwarePage(const Adreno830Status *s,Rows rows)
{
    Row(rows[1]).str("CXT ").hex(s->aux[0],16).str(" SKEW ").dec(clamp(s->aux[1],999999999));
    Row(rows[2]).str("GMU BLK ").dec(clamp(lo(s->aux[2]),99999)).str(" DDR ").hex(hi(s->aux[2]),1).str(" TCM ").hex(lo(s->aux[4]),1);
    Row(rows[3]).str("SQE ").hex(lo(s->aux[3]),1).str(" AQE ").hex(hi(s->aux[3]),1);
}

void hfiStatus(Row &r,const char *name,uint16_t status)
{
    r.str(name);
    if(status==A830HfiNotSent)r.put('-');
    else r.hex(status,1);
}

/* S5: "GMU FW <core> HFI <hfi> INIT <result> <ms>ms[ TCM <words>]", "HFI M1 <st> M2 <st> M6 <st> M8 <st> M9 <st>
 * [ L<m>/<a>/<e>][ G11 <general11>]", "OOB <us>us GX <pwrclk> RPMH <state> AO <ao>[ RSC <rsc>]": TCM, G11 and RSC in the annibale
 * profile (B4 read-back, GENERAL_11 before B6, RSC_STATUS0_DRV0 at B3), L once a late HFI drain ran. Worst cases 60, 66, 63. */
void gmuPage(const Adreno830Status *s,Rows rows)
{
    const bool annibale=annibaleFields(s);
    Row fw(rows[1]);
    fw.str("GMU FW ").hex(lo(s->aux[0]),8).str(" HFI ").hex(hi(s->aux[0]),8).str(" INIT ").hex(lo(s->aux[1]),8).put(' ')
      .dec((s->aux[1]>>32)&0xffff).str("ms");
    if(annibale)fw.str(" TCM ").dec(s->aux[1]>>48);
    Row hfi(rows[2]);
    hfi.str("HFI");
    hfiStatus(hfi," M1 ",uint16_t(s->aux[2]));hfiStatus(hfi," M2 ",uint16_t(s->aux[2]>>16));
    hfiStatus(hfi," M6 ",uint16_t(s->aux[2]>>32));hfiStatus(hfi," M8 ",uint16_t(s->aux[2]>>48));
    hfiStatus(hfi," M9 ",uint16_t(s->aux[3]));
    lateDrain(hfi,s->hfi_late);
    if(annibale)hfi.str(" G11 ").hex(s->gmu_general11,8);
    Row oob(rows[3]);
    oob.str("OOB ").dec(clamp(hi(s->aux[3]),999999)).str("us GX ").hex(lo(s->aux[4]),8).str(" RPMH ").hex(hi(s->aux[4]),1)
       .str(" AO ").hex(s->sticky[A830StickyGmuAo],8);
    if(annibale)oob.str(" RSC ").hex(lo(s->aux[5]),8);
}

/* S6/S7: "CP RPTR <r> WPTR <w> RBBM <status> HW <fault>", "SQE PC <br>/<bv> PROT <prot> UNMASK <shadow>",
 * "ZAP x0 <x0> x1 <x1> step <n> wq <n>[ L<m>/<a>/<e>]" (the late HFI drain of an S6/S7 failure or a recovery, worst case 53). */
void cpPage(const Adreno830Status *s,Rows rows)
{
    Row(rows[1]).str("CP RPTR ").dec(clamp(lo(s->aux[0]),99999)).str(" WPTR ").dec(clamp(hi(s->aux[0]),99999)).str(" RBBM ")
        .hex(lo(s->aux[1]),8).str(" HW ").hex(hi(s->aux[1]),1);
    Row(rows[2]).str("SQE PC ").hex(lo(s->aux[2]),1).put('/').hex(hi(s->aux[2]),1).str(" PROT ").hex(lo(s->aux[3]),1)
        .str(" UNMASK ").hex(hi(s->aux[3]),1);
    Row zap(rows[3]);
    zap.str("ZAP x0 ").hex(lo(s->aux[4]),1).str(" x1 ").hex(lo(s->aux[5]),1).str(" step ").dec((s->aux[5]>>32)&0xff)
       .str(" wq ").dec((s->aux[5]>>40)&0xff);
    lateDrain(zap,s->hfi_late);
}

char coherence(uint64_t bits,unsigned test)
{
    switch((bits>>(2*test))&3) {
    case A830CohPass: return 'p';
    case A830CohFail: return 'f';
    default: return '-';
    }
}

/* S8: "IRQ 332 rb <readback> n<count> wake <us>us off <irq_off>", "COH V4 <p/f> V9a <p/f> V9b <p/f/-> V9c <p/f/->
 * [ L<m>/<a>/<e>]" (the late HFI drain of an S8 failure), "TS CP <hi:lo> AO <hi:lo>". */
void fencePage(const Adreno830Status *s,Rows rows)
{
    Row(rows[1]).str("IRQ 332 rb ").hex(lo(s->aux[0])&0xffff,4).str(" n").dec(clamp(s->aux[1],9999999999ULL)).str(" wake ")
        .dec(clamp(hi(s->aux[0]),9999999)).str("us off ").hex(s->irq_off,8);
    Row coh(rows[2]);
    coh.str("COH V4 ").put(coherence(s->aux[2],0)).str(" V9a ").put(coherence(s->aux[2],1)).str(" V9b ")
       .put(coherence(s->aux[2],2)).str(" V9c ").put(coherence(s->aux[2],3));
    lateDrain(coh,s->hfi_late);
    Row(rows[3]).str("TS CP ").hex(hi(s->aux[3]),8).put(':').hex(lo(s->aux[3]),8).str(" AO ").hex(hi(s->aux[4]),8).put(':')
        .hex(lo(s->aux[4]),8);
}

unsigned digitsOf(uint64_t value) {unsigned n=1;while(value>=10) {value/=10;++n;}return n;}

/* The ring's queue period at the end of row 3 (25 characters at most): " | q <avg>/<max>ms d<depth> s<slow>", "q~" when the
 * completions were seen without the fence interrupt (upper bounds), " | q -" for a period without any submission or
 * completion, "-/-" when work was submitted but nothing completed. avg rounds to the nearest ms, max up. Then " sw<n>", the
 * context entries (client switches) of the period, when there were any and the row has room for the field whole (M14 P0). */
void queueTail(Row &r,const Adreno830StatusQueue &q)
{
    r.str(" | q");
    if(!q.submitted && !q.completed && !q.missed) {r.str(" -");return;}
    r.put(q.polled?'~':' ');
    if(q.completed)r.dec(clamp((uint64_t(q.avg_us)+500)/1000,999)).put('/').dec(clamp((uint64_t(q.max_us)+999)/1000,9999));
    else r.str("-/-");
    r.str("ms d").dec(clamp(q.depth,999)).str(" s").dec(clamp(q.slow,999));
    const uint64_t contexts=clamp(q.contexts,9999);
    if(contexts && r.room()>=3u+digitsOf(contexts))r.str(" sw").dec(contexts);
}

/* The MEM row's job attribution (preemption phase 0, ARCHITECTURE 4.17): " fw<max>ms/<n4>", the longest external service time ahead
 * of a render-server job in the period (rounded up to the ms, at most 9999) and how many of those times exceeded 4 ms (at most 999),
 * while that longest time is not 0: 13 characters at most, before the client figures, which take what room is left. */
void attributionTail(Row &r,const Adreno830StatusQueue &q)
{
    if(!q.fw_jobs || !q.fw_max_us)return;
    r.str(" fw").dec(clamp((uint64_t(q.fw_max_us)+999)/1000,9999)).str("ms/").dec(clamp(q.fw_over4,999));
}

/* The MEM row's client figures (adreno830_status_clients, ARCHITECTURE 8.4): " sc<MiB>" while own BOs live outside the BO pool (and
 * the field fits whole: after a job attribution field it may not), then the client holding the most GPU memory as
 * " <name>:<own>+<imports>" when it holds imports and that leaves six characters of its name (or all of a shorter one), else
 * " <name>:<MiB>" with at least three; "~" before the name for an orphan. The name is cut to the room left; with less room the field
 * is left out whole. Every number is at most 99999. */
void clientTail(Row &r,const Adreno830StatusClients &c)
{
    const uint64_t fallback=clamp(c.fallback_mib,99999);
    if(fallback && r.room()>=3u+digitsOf(fallback))r.str(" sc").dec(fallback);
    if(!c.top[0] || !c.top_mib)return;
    const uint64_t total=clamp(c.top_mib,99999),imported=clamp(c.top_import_mib<c.top_mib?c.top_import_mib:c.top_mib,99999);
    const uint64_t own=clamp(c.top_mib-(c.top_import_mib<c.top_mib?c.top_import_mib:c.top_mib),99999);
    const unsigned head=1+(c.top_orphan?1u:0u)+1;   /* " ", "~", ":" */
    unsigned length=0;
    while(length<sizeof c.top && c.top[length])++length;
    const unsigned splitName=length<6?length:6,totalName=length<3?length:3;
    const bool split=imported && r.room()>=head+digitsOf(own)+1+digitsOf(imported)+splitName;
    const unsigned numbers=split?digitsOf(own)+1+digitsOf(imported):digitsOf(total);
    if(r.room()<head+numbers+(split?splitName:totalName))return;
    const unsigned keep=length<r.room()-head-numbers?length:r.room()-head-numbers;
    r.put(' ');
    if(c.top_orphan)r.put('~');
    for(unsigned i=0;i<keep;++i)r.put(c.top[i]>' ' && c.top[i]<0x7f?c.top[i]:'?');
    r.put(':');
    if(split)r.dec(own).put('+').dec(imported);
    else r.dec(total);
}

/* Run page (S10 and Running): "DCVS <min>-<max>[ T<target><trend>[!]] t<permille>/<permille>...", "MEM pool <MiB> used <MiB>
 * imp <n> coh <c>", "IRQ <n> wait <n> to <n> late <n> wptr <n>", or with the ring's queue period "IRQ <n> wait <n> to <n> late
 * <n> | q ...". The thermal field (aux[3] bits 40-50, A830StatusThermal) is at most 7 characters and comes before the residency,
 * which is cut with '+' when the row is full. */
void runPage(const Adreno830Status *s,Rows rows,const Adreno830StatusQueue *queue,const Adreno830StatusClients *clients)
{
    Row d(rows[1]);
    d.str("DCVS ").dec(s->aux[0]&0xffff).put('-').dec((s->aux[0]>>16)&0xffff);
    const uint64_t thermal=s->aux[3]>>A830StatusThermalShift;
    if(thermal&0x7f) {
        static const char trends[]="?=+-";
        d.str(" T").dec(thermal&0x7f).put(trends[(thermal>>7)&3]);
        if((thermal>>9)&1)d.put('!');
    }
    d.str(" t");
    const unsigned count=unsigned(s->aux[0]>>40)&0xff;
    for(unsigned i=0;i<count && i<16;++i) {
        const uint64_t permille=(s->aux[1+i/6]>>((i%6)*10))&0x3ff;
        if(d.room()<(permille>=1000?4u:permille>=100?3u:permille>=10?2u:1u)+(i?1u:0u)+1u) {d.put('+');break;}
        if(i)d.put('/');
        d.dec(permille);
    }
    const uint64_t m=s->aux[4];
    Row mem(rows[2]);
    mem.str("MEM pool ").dec(m&0xffff).str(" used ").dec((m>>16)&0xffff).str(" imp ").dec((m>>34)&0x7fff)
       .str(" coh ").dec((m>>32)&3);
    if(m>>49)mem.str(" vm ").dec(m>>49);
    if(queue)attributionTail(mem,*queue);
    if(clients)clientTail(mem,*clients);
    Row irq(rows[3]);
    if(!queue) {
        irq.str("IRQ ").dec(clamp(s->irqs,99999999)).str(" wait ").dec(clamp(s->waits,99999999)).str(" to ")
           .dec(clamp(s->wait_timeouts,9999999)).str(" late ").dec(clamp(s->late_wakeups,9999999)).str(" wptr ")
           .dec(clamp(s->wptr_retries,9999999));
        return;
    }
    /* 41 characters at most, the queue tail 25: exactly 66 at worst. */
    irq.str("IRQ ").count(s->irqs,6).str(" wait ").count(s->waits,6).str(" to ").count(s->wait_timeouts,5).str(" late ")
       .count(s->late_wakeups,4);
    queueTail(irq,*queue);
}

/* Stages without a page of their own (S9) show the stage words: the six aux words. */
void genericPage(const Adreno830Status *s,Rows rows)
{
    for(unsigned i=0;i<3;++i)
        Row(rows[1+i]).str("AUX").dec(2*i).put(' ').hex(s->aux[2*i],16).str(" AUX").dec(2*i+1).put(' ').hex(s->aux[2*i+1],16);
}

/* Which page a stage shows; stages on the same page share their aux words. */
unsigned pageOf(uint8_t stage)
{
    switch(stage) {
    case A830SInputs: return 0;
    case A830SPower: case A830SSecureCx: return 1;
    case A830SSmmu: return 2;
    case A830SFirmware: return 3;
    case A830SGmu: return 4;
    case A830SCp: case A830SZap: return 5;
    case A830SFence: return 6;
    case A830SDcvs: case A830SRunning: return 8;
    default: return 7;
    }
}

void page(const Adreno830Status *s,Rows rows,const Adreno830StatusQueue *queue,const Adreno830StatusClients *clients)
{
    switch(pageOf(s->stage)) {
    case 0: inputsPage(s,rows);break;
    case 1: powerPage(s,rows);break;
    case 2: smmuPage(s,rows);break;
    case 3: firmwarePage(s,rows);break;
    case 4: gmuPage(s,rows);break;
    case 5: cpPage(s,rows);break;
    case 6: fencePage(s,rows);break;
    case 8: runPage(s,rows,queue,clients);break;
    default: genericPage(s,rows);break;
    }
}

/* The lock serialises publishers (stage thread, DCVS thread, bottom halves); never taken in a primary interrupt. */
void lock() {while(__atomic_exchange_n(&publishLock,1u,__ATOMIC_ACQUIRE)) {}}
void unlock() {__atomic_store_n(&publishLock,0u,__ATOMIC_RELEASE);}
}

const char *adreno830_stage_name(uint8_t stage) {return stage<A830StageCount?stageNames[stage]:stage==A830SRunning?"RUN":"?";}
const char *adreno830_stage_title(uint8_t stage) {return stage<A830StageCount?stageTitles[stage]:"GPU";}

unsigned adreno830_status_format(const Adreno830Status *status,char rows[Adreno830StatusRows][Adreno830StatusRowBytes],unsigned mode,
                                 const Adreno830StatusQueue *queue,const Adreno830StatusClients *clients)
{
    if(!rows)return 0;
    memset(rows,0,Adreno830StatusRows*Adreno830StatusRowBytes);
    if(!status)return 0;
    row0(status,Row(rows[0]));
    page(status,rows,queue,clients);
    return mode==0?0:mode==1?1:Adreno830StatusRows;
}

unsigned adreno830_status_format(const Adreno830Status *status,char rows[Adreno830StatusRows][Adreno830StatusRowBytes],unsigned mode,
                                 const Adreno830StatusQueue *queue)
{
    return adreno830_status_format(status,rows,mode,queue,nullptr);
}

unsigned adreno830_status_format(const Adreno830Status *status,char rows[Adreno830StatusRows][Adreno830StatusRowBytes],unsigned mode)
{
    return adreno830_status_format(status,rows,mode,nullptr);
}

void adreno830_status_mode(unsigned mode) {__atomic_store_n(&overlayMode,mode>3?3:mode,__ATOMIC_RELAXED);}
unsigned adreno830_status_current_mode(void) {return __atomic_load_n(&overlayMode,__ATOMIC_RELAXED);}
void adreno830_status_diag(bool on) {__atomic_store_n(&diagLines,on,__ATOMIC_RELAXED);}
bool adreno830_status_current_diag(void) {return __atomic_load_n(&diagLines,__ATOMIC_RELAXED);}
bool adreno830_status_diag_lines(void)
{
    return adreno830_status_current_diag() && adreno830_status_current_mode()>=2 && boot_progress_trace_enabled()!=0;
}

void adreno830_status_reset(void)
{
    lock();
    Adreno830Status *s=&adreno830_status;
    const uint32_t seq=s->seq;
    memset(s,0,sizeof *s);
    s->magic=Adreno830StatusMagic;s->version=Adreno830StatusVersion;s->bytes=uint16_t(sizeof *s);
    s->seq=seq;   /* stays monotonic: a reader compares it with the last value it drew */
    published=false;publishedCount=0;
    __atomic_store_n(&runKnobs,uint64_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(&runTemp,uint64_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(&runThermal,uint64_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(&runVm,uint64_t(0),__ATOMIC_RELAXED);
    runQueueSet=false;
    runClientsSet=false;
    runFrontend={};
    meterWindow={};meterLast[0]=0;runAperture=0;
    unlock();
}

void adreno830_status_queue(const Adreno830StatusQueue *queue)
{
    lock();
    if(queue)runQueue=*queue;
    runQueueSet=queue!=nullptr;
    unlock();
}

namespace {
/* One tick's calls into the meter line's window, once: the status block keeps only the last tick's clients. */
void foldCalls(MeterWindow &w,const Adreno830StatusCalls &c)
{
    for(unsigned i=0;i<Adreno830CallKinds;++i)if(c.max_us[i]>w.call_max_us[i])w.call_max_us[i]=c.max_us[i];
    for(unsigned i=0;i<Adreno830WaitKinds;++i)if(c.wait_us[i]>w.wait_max_us[i])w.wait_max_us[i]=c.wait_us[i];
    w.import_maps+=c.import_maps;w.shared_maps+=c.shared_maps;w.shared_us+=c.shared_us;w.map_us+=c.map_us;w.unmap_us+=c.unmap_us;
    w.zero_kib+=c.zero_kib;w.zero_us+=c.zero_us;w.contig_tries+=c.contig_tries;w.contig_fails+=c.contig_fails;w.outside_bos+=c.outside_bos;
}
}

void adreno830_status_clients(const Adreno830StatusClients *clients)
{
    lock();
    if(clients) {runClients=*clients;runClients.top[sizeof runClients.top-1]=0;foldCalls(meterWindow,clients->calls);}
    runClientsSet=clients!=nullptr;
    unlock();
}

void adreno830_status_frontend(const Adreno830StatusFrontend *frontend)
{
    if(!frontend)return;
    const auto add=[](uint32_t &sum,uint32_t n) {sum=n<UINT32_MAX-sum?sum+n:UINT32_MAX;};
    const auto longest=[](uint32_t &most,uint32_t n) {if(n>most)most=n;};
    lock();
    Adreno830StatusFrontend &f=runFrontend;
    add(f.states,frontend->states);add(f.state_us,frontend->state_us);longest(f.state_max_us,frontend->state_max_us);
    add(f.makings,frontend->makings);add(f.making_us,frontend->making_us);longest(f.making_max_us,frontend->making_max_us);
    add(f.clean_us,frontend->clean_us);longest(f.clean_max_us,frontend->clean_max_us);
    unlock();
}

void adreno830_status_aperture(uint32_t state)
{
    lock();
    runAperture=(state<3?state:3)+1;
    unlock();
}

namespace {
/* Milliseconds with one decimal from microseconds: averages to the nearest 0.1 ms, maxima up; at most 9999.9. */
void tenths(Row &r,uint64_t us,bool up)
{
    const uint64_t t=clamp((us+(up?99:50))/100,99999);
    r.dec(t/10).put('.').dec(t%10);
}
/* Whole milliseconds, up: a call figure of the meter line. */
uint64_t wholeMs(uint64_t us,uint64_t limit) {return clamp((us+999)/1000,limit);}
/* The call figures of the meter line (Adreno830StatusCalls, ARCHITECTURE 8.8), each bracket left out while it is all 0: at most 123
 * characters, every longest-so-far 9999 ms and every sum 99999 at most. */
void meterCalls(Row &r,const MeterWindow &w)
{
    const auto longest=[&r](const char *name,const uint32_t *us,unsigned count) {
        uint32_t any=0;
        for(unsigned i=0;i<count;++i)any|=us[i];
        if(!any)return;
        r.str(name);
        for(unsigned i=0;i<count;++i) {
            if(i)r.put('/');
            r.dec(wholeMs(us[i],9999));
        }
    };
    longest(" ca ",w.call_max_us,Adreno830CallKinds);
    longest(" lk ",w.wait_max_us,Adreno830WaitKinds);
    if(w.import_maps)r.str(" im ").dec(clamp(w.import_maps,99999)).put('/').dec(clamp(w.shared_maps,99999)).put('/').dec(wholeMs(w.shared_us,99999));
    if(w.map_us || w.unmap_us)r.str(" vt ").dec(wholeMs(w.map_us,99999)).put('/').dec(wholeMs(w.unmap_us,99999));
    if(w.zero_kib || w.zero_us)r.str(" zf ").dec(clamp((w.zero_kib+1023)/1024,99999)).put('/').dec(wholeMs(w.zero_us,99999));
    if(w.contig_tries || w.outside_bos)
        r.str(" cs ").dec(clamp(w.contig_tries,99999)).put('/').dec(clamp(w.contig_fails,99999)).put('/').dec(clamp(w.outside_bos,99999));
}
/* "gpu sec ..." (adreno830_status_meter, one second's figures; the format is in Adreno830Status.h): the process name cut to 12 characters,
 * every field whole: at most 315 characters. */
void secondLine(const Adreno830StatusQueue &q,const Adreno830StatusClients *c,const Adreno830StatusFrontend &f,char *line)
{
    Row r(line,Adreno830StatusMeterBytes-1);
    const auto longest=[&r](uint64_t us) {r.put(' ');tenths(r,us,true);r.str("ms");};
    r.str("gpu sec rs ").dec(clamp(q.rs_jobs,99999));
    if(q.rs_jobs)longest(q.rs_max_us);
    r.str(" ext ").dec(clamp(q.ext_jobs,99999));
    if(q.ext_jobs)longest(q.ext_max_us);
    if(q.completed) {r.str(" q");longest(q.max_us);r.str(" d").dec(clamp(q.depth,999));}
    if(q.fw_max_us) {r.str(" fw");longest(q.fw_max_us);}
    if(!c)return;
    if(c->calls.fence_us) {r.str(" wf");longest(c->calls.fence_us);}
    r.str(" cl ").dec(clamp(c->second_calls,99999999));
    uint32_t any=0;
    for(const uint32_t us:c->calls.max_us)any|=us;
    if(any) {
        r.str(" ca ");
        for(unsigned i=0;i<Adreno830CallKinds;++i) {
            if(i)r.put('/');
            r.dec(wholeMs(c->calls.max_us[i],9999));
        }
    }
    if(c->calls.map_us || c->calls.unmap_us)r.str(" vt ").dec(wholeMs(c->calls.map_us,99999)).put('/').dec(wholeMs(c->calls.unmap_us,99999));
    if(c->calls.import_maps)r.str(" im ").dec(clamp(c->calls.import_maps,99999));
    const auto spent=[&r](uint64_t us,uint64_t most) {r.dec(wholeMs(us,99999)).put('/').dec(wholeMs(most,9999));};
    if(f.states || f.state_us) {r.str(" pl ").dec(clamp(f.states,9999)).put(' ');spent(f.state_us,f.state_max_us);}
    if(f.makings || f.making_us) {r.str(" vk ").dec(clamp(f.makings,9999)).put(' ');spent(f.making_us,f.making_max_us);}
    if(f.clean_us || f.clean_max_us) {r.str(" cln ");spent(f.clean_us,f.clean_max_us);}
    if(!c->second_top[0] || !c->second_top_calls)return;
    r.str(" top ");
    for(unsigned i=0;i<12 && c->second_top[i];++i)r.put(c->second_top[i]>' ' && c->second_top[i]<0x7f?c->second_top[i]:'?');
    r.put(' ').dec(clamp(c->second_top_calls,99999999));
    static const char classes[Adreno830CallClasses][3]={"al","mp","fr","im","su","wf","cp","ot"};
    for(unsigned i=0;i<Adreno830CallClasses;++i)
        if(c->second_classes[i])r.put(' ').str(classes[i]).dec(clamp(c->second_classes[i],99999999));
}
/* "gpu meter ..." (adreno830_status_meter): every count at most 99999 (the fault counts 9999, connections 99, polled periods 99), so
 * the widest line without call figures is 186 characters, one ramoops record, and with them 309 (past 200, a '+' record). */
unsigned meterLine(const MeterWindow &w,const Adreno830StatusClients *c,uint32_t aperture,char *line)
{
    Row r(line,Adreno830StatusMeterBytes-1);
    r.str("gpu meter rs ").dec(clamp(w.rs_jobs,99999));
    if(w.rs_jobs) {r.put(' ');tenths(r,w.rs_us/w.rs_jobs,false);r.put('/');tenths(r,w.rs_max_us,true);r.str("ms");}
    r.str(" fw ").dec(clamp(w.fw_jobs,99999));
    if(w.fw_jobs) {r.put(' ');tenths(r,w.fw_us/w.fw_jobs,false);r.put('/');tenths(r,w.fw_max_us,true);r.str("ms");}
    r.str(" n4 ").dec(clamp(w.fw_over4,99999)).str(" ext ").dec(clamp(w.ext_jobs,99999)).put(' ');
    tenths(r,w.ext_us,false);
    r.str("ms ctx ").dec(clamp(w.contexts,99999)).put(' ').dec(clamp(w.contexts_max,9999)).str("/s poll ").dec(clamp(w.polled,99));
    if(c)
        r.str(" conn ").dec(clamp(c->clients,99)).put('/').dec(clamp(c->peak_clients,99)).str(" nos ").dec(clamp(c->slot_refusals,99999))
         .str(" ovf ").dec(clamp(c->window_overflows,99999)).str(" vmr ").dec(clamp(c->reserve_objects,99999)).put('/').dec(clamp(c->reserve_spans,99999)).str(" pf ")
         .dec(clamp(c->faults_alike,9999)).put('/').dec(clamp(c->faults_apart,9999)).put('/').dec(clamp(c->faults_undecided,9999))
         .put('/').dec(clamp(c->faults_polled,9999));
    meterCalls(r,w);
    r.str(" ap");
    if(aperture)r.dec(aperture-1);
    else r.put('-');
    return r.used();
}
}

void adreno830_status_meter(void)
{
    char line[Adreno830StatusMeterBytes],second[Adreno830StatusMeterBytes];
    bool write=false,writeSecond=false;
    /* The diagnostic pages only (pagani-gpu-status 2 or 3, what the test images run): the default log stays as it was. */
    const bool logging=adreno830_status_current_mode()>=2 && boot_progress_trace_enabled()!=0;
    lock();
    if(runQueueSet) {
        const Adreno830StatusQueue &q=runQueue;
        MeterWindow &w=meterWindow;
        w.rs_jobs+=q.rs_jobs;w.rs_us+=uint64_t(q.rs_avg_us)*q.rs_jobs;
        if(q.rs_max_us>w.rs_max_us)w.rs_max_us=q.rs_max_us;
        w.fw_jobs+=q.fw_jobs;w.fw_us+=uint64_t(q.fw_avg_us)*q.fw_jobs;w.fw_over4+=q.fw_over4;
        if(q.fw_max_us>w.fw_max_us)w.fw_max_us=q.fw_max_us;
        w.ext_jobs+=q.ext_jobs;w.ext_us+=q.ext_busy_us;
        w.contexts+=q.contexts;
        if(q.contexts>w.contexts_max)w.contexts_max=q.contexts;
        if(q.polled)++w.polled;
        const uint32_t calls=runClientsSet?runClients.second_calls:0;
        writeSecond=adreno830_status_diag_lines() && (q.submitted || q.completed || calls>=Adreno830StatusSecondBurst);
        if(writeSecond) {
            secondLine(q,runClientsSet?&runClients:nullptr,runFrontend,second);
            if(runClientsSet)runFrontend={};   /* a line without the clients has no place for them */
        }
    }
    /* One line per window at most, and only a changed one: an idle GPU writes nothing after its first. */
    if(++meterWindow.periods>=Adreno830StatusMeterPeriods) {
        if(logging) {
            meterLine(meterWindow,runClientsSet?&runClients:nullptr,runAperture,line);
            write=strcmp(line,meterLast)!=0;
            if(write)memcpy(meterLast,line,sizeof meterLast);
        }
        meterWindow={};
    }
    unlock();
    if(writeSecond)boot_progress_trace(second);
    if(write)boot_progress_trace(line);
}

void adreno830_status_knobs(uint32_t maxlevel,uint32_t irq)
{
    __atomic_store_n(&runKnobs,A830StatusKnobs(maxlevel,irq),__ATOMIC_RELAXED);
}

void adreno830_status_temperature(int32_t deci)
{
    __atomic_store_n(&runTemp,A830StatusTemp(deci),__ATOMIC_RELAXED);
}

void adreno830_status_thermal(int32_t target_deci,uint32_t trend,uint32_t flags)
{
    __atomic_store_n(&runThermal,A830StatusThermal(target_deci,trend,flags),__ATOMIC_RELAXED);
}

void adreno830_status_vm(uint64_t bytes)
{
    const uint64_t mib=(bytes+(1u<<20)-1)>>20;
    __atomic_store_n(&runVm,(mib<0x7fff?mib:0x7fff)<<49,__ATOMIC_RELAXED);
}

void adreno830_status_publish(void)
{
    const unsigned mode=__atomic_load_n(&overlayMode,__ATOMIC_RELAXED);
    char rows[Adreno830StatusRows][Adreno830StatusRowBytes];
    /* The user-space report page replaces the diagnostic page once the plugin reported something (mode 3). */
    char report[3][Adreno830StatusRowBytes]={};
    const unsigned reported=mode==3?native_adreno_report_rows(report):0;
    lock();
    unsigned count=adreno830_status_format(&adreno830_status,rows,mode,runQueueSet?&runQueue:nullptr,
                                           runClientsSet?&runClients:nullptr);
    for(unsigned i=0;i<reported && i<3;++i) {
        report[i][Adreno830StatusRowMax]=0;
        memset(rows[1+i],0,Adreno830StatusRowBytes);
        strncpy(rows[1+i],report[i],Adreno830StatusRowMax);
    }
    bool changed=false;
    if(!published || count!=publishedCount || memcmp(rows,adreno830_status.rows,sizeof rows)) {
        memcpy(adreno830_status.rows,rows,sizeof rows);
        __atomic_store_n(&adreno830_status.seq,adreno830_status.seq+1,__ATOMIC_RELEASE);
        published=true;publishedCount=count;changed=true;
        if(count)boot_progress_gpu_rows(adreno830_status.rows,count);
    }
    unlock();
    /* The registry property allocates; outside the lock. */
    if(changed)native_adreno_publish_status(&adreno830_status);
}

void adreno830_status_begin(uint8_t stage)
{
    Adreno830Status *s=&adreno830_status;
    /* Entering the first stage is not a stage switch: a run that stops or fails in S0 publishes exactly once. */
    const bool switched=s->state!=A830Idle;
    if(switched && pageOf(stage)!=pageOf(s->stage))memset(s->aux,0,sizeof s->aux);
    s->stage=stage;s->state=A830Busy;s->step=0;
    if(switched)adreno830_status_publish();
}

void adreno830_status_step(uint16_t step) {adreno830_status.step=step;}

void adreno830_status_end(uint8_t stage,uint32_t elapsed_us)
{
    Adreno830Status *s=&adreno830_status;
    if(stage<A830StageCount)s->stage_us[stage]=elapsed_us;
    uint64_t total=0;
    for(unsigned i=0;i<A830StageCount;++i)total+=s->stage_us[i];
    s->boot_ms=uint32_t(total/1000);
}

void adreno830_status_stop(uint8_t stage,uint16_t reason)
{
    Adreno830Status *s=&adreno830_status;
    s->stage=stage;s->state=A830Stopped;s->reason=reason;
    adreno830_status_publish();
}

void adreno830_status_fail(uint8_t stage,uint16_t step,uint16_t reason,IOReturn ior,uint64_t reg,uint64_t value,uint64_t expect)
{
    Adreno830Status *s=&adreno830_status;
    s->stage=stage;s->state=A830Failed;s->step=step;s->reason=reason;s->ioreturn=uint32_t(ior);
    s->reg=reg;s->value=value;s->expect=expect;
    adreno830_status_publish();
    /* Also into the boot stage history, which the 30 s no-frame fallback page lists. */
    boot_progress_display_status(adreno830_stage_title(stage),uint32_t(ior));
}

void adreno830_status_running(void)
{
    Adreno830Status *s=&adreno830_status;
    if(pageOf(s->stage)!=pageOf(A830SRunning))memset(s->aux,0,sizeof s->aux);
    s->stage=A830SRunning;s->state=A830Done;s->step=0;s->reason=A830ReasonNone;s->ioreturn=0;
    adreno830_status_publish();
}

void adreno830_status_lost(uint8_t stage,uint16_t step,uint16_t reason,IOReturn ior)
{
    Adreno830Status *s=&adreno830_status;
    s->stage=stage;s->state=A830Lost;s->step=step;s->reason=reason;s->ioreturn=uint32_t(ior);
    adreno830_status_publish();
    boot_progress_display_status("GPU lost",uint32_t(ior));
}

void adreno830_status_recovering(uint8_t stage,uint32_t elapsed_ms)
{
    Adreno830Status *s=&adreno830_status;
    if(pageOf(stage)!=pageOf(s->stage))memset(s->aux,0,sizeof s->aux);
    s->stage=stage;s->state=A830Recovering;s->boot_ms=elapsed_ms;
    adreno830_status_publish();
}

void adreno830_status_slumber(uint32_t seconds,uint32_t wakes,uint32_t last_wake_ms)
{
    Adreno830Status *s=&adreno830_status;
    s->aux[5]=uint64_t(clamp(seconds,0xffffff))|uint64_t(clamp(wakes,0xffffff))<<24|uint64_t(clamp(last_wake_ms,0xffff))<<48;
    s->stage=A830SRunning;s->state=A830Slumber;
    adreno830_status_publish();
}

void adreno830_status_irq_off(uint32_t bits) {__atomic_fetch_or(&adreno830_status.irq_off,bits,__ATOMIC_RELAXED);}

void adreno830_status_sticky(unsigned index,uint32_t value)
{
    if(index<4 && value)__atomic_store_n(&adreno830_status.sticky[index],value,__ATOMIC_RELAXED);
}

void adreno830_status_dcvs(uint32_t mhz,uint32_t busy_permille,uint32_t gmu_level,uint32_t bus_level,uint32_t min_mhz,
                           uint32_t max_mhz,unsigned first,unsigned count,const uint16_t *residency_permille)
{
    Adreno830Status *s=&adreno830_status;
    s->mhz=mhz;s->busy_permille=busy_permille;s->gmu_level=gmu_level;s->bus_level=bus_level;
    if(pageOf(s->stage)==pageOf(A830SRunning)) {
        if(count>16 || !residency_permille)count=residency_permille?16:0;
        uint64_t words[3]={};
        for(unsigned i=0;i<count;++i)words[i/6]|=uint64_t(clamp(residency_permille[i],1000))<<((i%6)*10);
        s->aux[0]=clamp(min_mhz,0xffff)|clamp(max_mhz,0xffff)<<16|uint64_t(first&0xff)<<32|uint64_t(count)<<40|
                  __atomic_load_n(&runKnobs,__ATOMIC_RELAXED)|__atomic_load_n(&runTemp,__ATOMIC_RELAXED);
        s->aux[1]=words[0];s->aux[2]=words[1];s->aux[3]=words[2]|__atomic_load_n(&runThermal,__ATOMIC_RELAXED);
    }
    adreno830_status_publish();
}

void adreno830_status_memory(uint32_t pool_mib,uint32_t used_mib,uint32_t imports,uint32_t coherent)
{
    Adreno830Status *s=&adreno830_status;
    if(pageOf(s->stage)==pageOf(A830SRunning))
        s->aux[4]=clamp(pool_mib,0xffff)|clamp(used_mib,0xffff)<<16|uint64_t(coherent&3)<<32|clamp(imports,0x7fff)<<34|
                  __atomic_load_n(&runVm,__ATOMIC_RELAXED);
    adreno830_status_publish();
}
