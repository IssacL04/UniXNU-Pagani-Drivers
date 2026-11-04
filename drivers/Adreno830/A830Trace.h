/* SPDX-License-Identifier: MIT */
#ifndef A830_TRACE_H
#define A830_TRACE_H
#include <stddef.h>
#include <stdint.h>

/* The bring-up register trace (boot argument pagani-gpu-trace, default 1 while the bring-up runs; ARCHITECTURE 8.7).
 *
 * While adreno830_start runs, and while a power or recovery operation re-runs a stage body, the orchestrator passes every
 * register access it binds for the modules (its own GPU/GMU/CX_MISC/RSCC words, GCC and GPU_CC for Sm8750GpuPower, the
 * DRV-2 TCSs, AOSS QMP and IPCC, LLCC, TSENS, the SMMU io of A830Smmu and the VM, the GMU io of A830Gmu, A830GmuFirmware
 * and the CX timer, the A830Irq windows) and every SMC through this tracer BEFORE the access executes. The tracer hands one
 * text line to a sink; the orchestrator's sink is BootProgress's boot_progress_trace, which has the line in the Android
 * ramoops region (DRAM) when it returns. After a hang and a manual reset the last access line of the log names the
 * register (or the SMC) that never completed.
 *
 * Lines (at most A830TraceLineBytes - 1 characters; addresses, values, steps and IOReturns in lowercase hex without 0x,
 * counts and microseconds in decimal):
 *   g<S>.<step> <BLK> w <addr> <value>[ #<n>]            a 32-bit write, before it is made (W: 64-bit)
 *   g<S>.<step> <BLK> r <addr>[ #<n>]                    a 32-bit read, before it is made (R: 64-bit)
 *   g<S>.<step> <BLK> r <addr>[ #<n>] =<value>           what that read returned
 *   g<S>.<step> SCM c <x0> <x1> <x2> <x3>[ #<n>]         an SMC: function id and arguments, before the instruction
 *   g<S>.<step> SCM c <x0>[ #<n>] =<x0> <x1> <x2>        what it returned
 *   g<S>.<step> <BLK> <op> <addr> x<n>[ ..<last>] =<v>   a run ended: n accesses from addr (to last, for a sequential
 *                                                        run); v is the last value read or returned, or written
 *   g<S> <NAME> begin t=<us>                             a stage starts (the orchestrator's clock)
 *   g<S> <NAME> end ok|stop <us>us                       it ended (stop: pagani-gpu-stop)
 *   g<S> <NAME> end fail <ior> s<step> r<reason> <reg>=<value> <us>us
 *   g op <name> begin  /  g op <name> end <ior> <us>us   a power or recovery operation (A830PowerOps, A830RecoveryOps)
 *   g<S>.<step> note <text>[ <v>[ <v>]]                  context: a window kept quiet, what it produced
 *   g start p<profile> stop <stage|-> t=<us>  /  g done <ior> <us>us lines <n> drops <n>
 * S is the status stage (0-a, f once running), step the status step (ARCHITECTURE 8.3; the GCC/GPU_CC accesses of
 * Sm8750GpuPowerOn/Off carry its Sm8750GpuPowerStep instead, the S1 failure steps), BLK the window name the orchestrator
 * gives (GPU, RSCC, DBGC, CXM, GMU, GMUAO, GPUCC, GCC, SMMU, DRV2, AOSS, IPCC, TSNS, LLC0-3, LLCO, LLCA) or SCM, addr the
 * physical address (SCM: the function id).
 *
 * Runs keep it cheap. An access that repeats one of the last A830TraceRuns keys is counted instead of written: the same
 * block and operation and either the same address (and, for writes and SMCs, the same value and arguments: a polling loop
 * or a retry) or the next address (+4, +8 for 64-bit: a sequential block such as the GMU TCM load). Accesses 1 to
 * A830TracePollHead of a poll (1 to A830TraceNextHead of a sequential run: short blocks of distinct registers keep every
 * line), the powers of two and every A830TraceEvery-th get their lines ("#<n>" from the second); the run's end line
 * ("x<n>") follows when an access with a new key comes, when the status stage or step changes and before every stage,
 * operation and note line. A loop over at most A830TraceRuns registers therefore costs a few lines however long it spins,
 * and the line written before a hung access is that access, or (inside a long run) its run's last logged access with the
 * same register or, in a sequential run longer than A830TraceNextHead words, at most n/2 words before it. pagani-gpu-trace=2 (begin with verbatim)
 * keeps no runs: every access gets its line, without "#<n>" and without end lines.
 *
 * The tracer is plain data (constant-initialised, no constructor), formats into its own buffer with one bounded writer,
 * allocates nothing and calls nothing but the sink. It is not thread safe: the orchestrator serialises the calls (a spin
 * lock with interrupts off) and never calls it from interrupt context. */

enum : unsigned {
    A830TraceLineBytes=128,     /* the longest line (an SMC with three 64-bit arguments and #<n>) is under 100 characters */
    A830TraceRuns=8,            /* registers one polling round may touch and still be counted (A830Cp's annibale idle: 7) */
    A830TracePollHead=4,        /* accesses 1..4 of a poll always get their line */
    A830TraceNextHead=16,       /* words 1..16 of a sequential run always get their line */
    A830TraceEvery=1024,        /* after the powers of two below it, every 1024th access of a run gets its line */
};
enum : char {A830TraceRead='r',A830TraceWrite='w',A830TraceRead64='R',A830TraceWrite64='W',A830TraceCall='c'};
enum : uint8_t {A830TraceOk,A830TraceStop,A830TraceFail};   /* how a stage ended */

/* What an access line tells the matching value line (A830Trace::value). op 0: nothing was traced. */
struct A830TraceToken {
    uint64_t address;
    uint32_t generation;        /* of the run slot the access was counted in */
    uint32_t count;             /* the access's number in its run (1: the first) */
    uint16_t step;
    uint8_t stage,block,slot;
    char op;
    bool logged;                /* the access had its own line: its value gets one too */
};

struct A830Trace {
    using Sink=void (*)(const char *line);
    enum : uint8_t {RunNew,RunPoll,RunNext};
    struct Run {
        uint64_t first,last;    /* the run's first and latest address */
        uint64_t key,key2,key3; /* writes: the value; SMCs: x1..x3; what a poll must repeat */
        uint64_t result;        /* the latest value read (reads), x0 returned (SMCs), the value written (writes) */
        uint32_t count,logged;  /* accesses counted; the count of the latest line written for the run */
        uint32_t generation,used;
        uint8_t block,shape;    /* RunNew until the second access says poll or sequential */
        char op;
        bool valid;
    };

    Sink sink_;
    const char *const *names_;
    unsigned name_count_;
    Run runs_[A830TraceRuns];
    uint32_t clock_,generation_,lines_;
    uint16_t step_;
    uint8_t stage_;
    bool keyed_;                /* stage_/step_ hold the stage and step the runs belong to */
    bool verbatim_;             /* no runs: a line for every access */
    char line_[A830TraceLineBytes];
    unsigned used_;

    /* ---- the one bounded writer ---- */
    void put(char c)
    {
        if(used_+1>=A830TraceLineBytes)return;
        line_[used_++]=c;
        line_[used_]=0;
    }
    void put(const char *text) {while(text && *text)put(*text++);}
    void hex(uint64_t value)
    {
        char digits[16];
        unsigned n=0;
        do {digits[n++]="0123456789abcdef"[value&15];value>>=4;} while(value);
        while(n)put(digits[--n]);
    }
    void dec(uint64_t value)
    {
        char digits[20];
        unsigned n=0;
        do {digits[n++]=char('0'+value%10);value/=10;} while(value);
        while(n)put(digits[--n]);
    }
    void start() {used_=0;line_[0]=0;put('g');}
    void prefix(uint8_t stage,uint16_t step,uint8_t block,char op,uint64_t address)
    {
        start();
        hex(stage);put('.');hex(step);put(' ');
        put(block<name_count_ && names_ && names_[block]?names_[block]:"?");
        put(' ');put(op);put(' ');hex(address);
    }
    void count(uint32_t n) {if(n>1) {put(" #");dec(n);}}
    void finish()
    {
        ++lines_;
        if(sink_)sink_(line_);
    }

    /* ---- runs ---- */
    static bool isRead(char op) {return op==A830TraceRead || op==A830TraceRead64;}
    static bool wide(char op) {return op==A830TraceRead64 || op==A830TraceWrite64;}
    static bool due(uint32_t n,uint8_t shape)
    {
        return n<=(shape==RunNext?A830TraceNextHead:A830TracePollHead) || (n<A830TraceEvery && !(n&(n-1))) ||
               !(n%A830TraceEvery);
    }
    void endLine(const Run &r)
    {
        prefix(stage_,step_,r.block,r.op,r.first);
        put(" x");dec(r.count);
        if(r.shape==RunNext) {put(" ..");hex(r.last);}
        put(" =");hex(r.result);
        finish();
    }
    /* Every run with accesses after its latest line gets its end line (it stays: the loop may go on). */
    void flush()
    {
        for(Run &r:runs_)
            if(r.valid && r.count>r.logged) {
                endLine(r);
                r.logged=r.count;
            }
    }
    void forget() {for(Run &r:runs_)r.valid=false;}
    void rekey(uint8_t stage,uint16_t step)
    {
        if(keyed_ && stage==stage_ && step==step_)return;
        if(keyed_) {flush();forget();}
        stage_=stage;step_=step;keyed_=true;
    }

    /* ---- the interface ---- */
    /* A fresh trace: no runs, no lines; names[block] is what a line shows for block (count of them). */
    void begin(Sink sink,const char *const *names,unsigned count,bool verbatim=false)
    {
        *this=A830Trace{};
        sink_=sink;names_=names;name_count_=count;verbatim_=verbatim;
    }
    uint32_t lines() const {return lines_;}

    /* An access, before it is made. value: what a write stores (reads: 0); an SMC passes x0 as address, x1 as value and
     * x2/x3 as arg2/arg3. The token goes to value() once a read or an SMC returned. */
    A830TraceToken access(uint8_t stage,uint16_t step,uint8_t block,char op,uint64_t address,uint64_t value,
                          uint64_t arg2=0,uint64_t arg3=0)
    {
        rekey(stage,step);
        const bool read=isRead(op);
        const uint64_t width=wide(op)?8:4;
        unsigned slot=A830TraceRuns;
        uint8_t shape=RunNew;
        for(unsigned i=0;i<A830TraceRuns && !verbatim_;++i) {
            const Run &r=runs_[i];
            if(!r.valid || r.block!=block || r.op!=op)continue;
            if(address==r.last && r.shape!=RunNext &&
               (read || (value==r.key && (op!=A830TraceCall || (arg2==r.key2 && arg3==r.key3))))) {slot=i;shape=RunPoll;break;}
            if(op!=A830TraceCall && address==r.last+width && r.shape!=RunPoll) {slot=i;shape=RunNext;break;}
        }
        A830TraceToken token={};
        token.address=address;token.step=step;token.stage=stage;token.block=block;token.op=op;
        if(slot<A830TraceRuns) {
            Run &r=runs_[slot];
            r.shape=shape;r.last=address;r.used=++clock_;++r.count;
            if(!read) {r.key=value;r.result=value;}
            if(op==A830TraceCall) {r.key2=arg2;r.key3=arg3;}
            token.slot=uint8_t(slot);token.generation=r.generation;token.count=r.count;
            token.logged=due(r.count,shape);
            if(!token.logged)return token;
            r.logged=r.count;
        } else if(verbatim_) {
            token.slot=uint8_t(A830TraceRuns);token.count=1;token.logged=true;
        } else {
            flush();   /* a new key: the pending runs ended here */
            /* The slot of the same register (a write of another value, a read after a sequential run) is replaced, so
             * one register has one run; else a free slot, else the least recently used. */
            for(unsigned i=0;i<A830TraceRuns && slot==A830TraceRuns;++i)
                if(runs_[i].valid && runs_[i].block==block && runs_[i].op==op && runs_[i].last==address)slot=i;
            for(unsigned i=0;i<A830TraceRuns && slot==A830TraceRuns;++i)
                if(!runs_[i].valid)slot=i;
            if(slot==A830TraceRuns) {
                slot=0;
                for(unsigned i=1;i<A830TraceRuns;++i)
                    if(runs_[i].used<runs_[slot].used)slot=i;
            }
            Run &r=runs_[slot];
            r=Run{};
            r.first=r.last=address;r.key=value;r.key2=arg2;r.key3=arg3;r.result=read?0:value;
            r.count=r.logged=1;r.generation=++generation_;r.used=++clock_;r.block=block;r.shape=RunNew;r.op=op;r.valid=true;
            token.slot=uint8_t(slot);token.generation=r.generation;token.count=1;token.logged=true;
        }
        prefix(stage,step,block,op,address);
        if(!read) {
            put(' ');hex(value);
            if(op==A830TraceCall) {put(' ');hex(arg2);put(' ');hex(arg3);}
        }
        count(token.count);
        finish();
        return token;
    }
    /* What the read (or the SMC: x0, x1, x2) of token returned. */
    void value(const A830TraceToken &token,uint64_t value,uint64_t result1=0,uint64_t result2=0)
    {
        if(!token.op)return;
        if(token.slot<A830TraceRuns) {
            Run &r=runs_[token.slot];
            if(r.valid && r.generation==token.generation)r.result=value;
        }
        if(!token.logged)return;
        prefix(token.stage,token.step,token.block,token.op,token.address);
        count(token.count);
        put(" =");hex(value);
        if(token.op==A830TraceCall) {put(' ');hex(result1);put(' ');hex(result2);}
        finish();
    }
    /* g<S> <name> begin t=<us>: the runs of the stage before end first. */
    void stageBegin(uint8_t stage,const char *name,uint64_t now_us)
    {
        flush();forget();keyed_=false;
        start();hex(stage);put(' ');put(name);put(" begin t=");dec(now_us);
        finish();
    }
    void stageEnd(uint8_t stage,const char *name,uint8_t outcome,uint32_t ior,uint16_t step,uint16_t reason,uint64_t reg,
                  uint64_t value,uint64_t elapsed_us)
    {
        flush();forget();keyed_=false;
        start();hex(stage);put(' ');put(name);put(" end ");
        if(outcome==A830TraceFail) {
            put("fail ");hex(ior);put(" s");hex(step);put(" r");dec(reason);put(' ');hex(reg);put('=');hex(value);
        } else put(outcome==A830TraceStop?"stop":"ok");
        put(' ');dec(elapsed_us);put("us");
        finish();
    }
    /* g op <name> begin / end <ior> <us>us. */
    void operation(const char *name,bool begin,uint32_t ior,uint64_t elapsed_us)
    {
        flush();forget();keyed_=false;
        start();put(" op ");put(name);
        if(begin)put(" begin");
        else {put(" end ");hex(ior);put(' ');dec(elapsed_us);put("us");}
        finish();
    }
    /* g<S>.<step> note <text>[ <v1>[ <v2>]] (values: how many of v1, v2 to show). */
    void note(uint8_t stage,uint16_t step,const char *text,unsigned values=0,uint64_t v1=0,uint64_t v2=0)
    {
        flush();
        start();hex(stage);put('.');hex(step);put(" note ");put(text);
        if(values>0) {put(' ');hex(v1);}
        if(values>1) {put(' ');hex(v2);}
        finish();
    }
    /* g start p<profile> stop <stage|-> t=<us>; g done <ior> <us>us lines <n> drops <n>. */
    void opened(uint32_t profile,uint32_t stop,uint64_t now_us)
    {
        start();put(" start p");dec(profile);put(" stop ");
        if(stop==0xffffffffu)put('-');
        else dec(stop);
        put(" t=");dec(now_us);
        finish();
    }
    void closed(uint32_t ior,uint64_t elapsed_us,uint32_t drops)
    {
        flush();forget();keyed_=false;
        start();put(" done ");hex(ior);put(' ');dec(elapsed_us);put("us lines ");dec(uint64_t(lines_)+1);put(" drops ");dec(drops);
        finish();
    }
};
#endif
