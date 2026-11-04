/* SPDX-License-Identifier: MIT */
/* The bring-up register trace (A830Trace.h, boot argument pagani-gpu-trace; ARCHITECTURE 8.7).
 *  1. A830Trace alone, line by line: access, value and SMC lines; polls (lines 1-4, the powers of two, every 1024th, the end
 *     line at a new key, a step change or a stage line), sequential runs (words 1-16), polls over up to 8 registers, LRU
 *     eviction, writes that change value, stage/operation/note/start/done lines, verbatim mode, bounded lines.
 *  2. The orchestrator over the module doubles of tests/test_start.cpp (built with main renamed), whose S1-S4 calls make
 *     scripted register traffic through the tables the orchestrator hands them (IoScript): for S1-S3 in both profiles the
 *     verbatim trace (pagani-gpu-trace=2) is exactly the scripts' accesses and the orchestrator's own reads, in order, with
 *     stage, step (Sm8750GpuPowerStep for the power-up), block, physical address and value; every write line was written
 *     while the register still held its old value and every SMC line before the instruction; the default trace (1) is that
 *     sequence with its runs counted. pagani-gpu-trace=0, the ramoops log off and a full run without the trace write
 *     nothing; pagani-gpu-trace=3 is refused in S0; the CX timer seed stays quiet; the power and recovery operations open
 *     their own scopes and nothing outside a scope writes a line.
 * host-test: Adreno830Start.cpp Adreno830Status.cpp NativeAdrenoFirmware.cpp NativeAdrenoSha256.cpp */
#define main test_start_main
#include "test_start.cpp"
#undef main
#include "A830Trace.h"
#include <algorithm>

namespace tr {
/* ---- What reached BootProgress ------------------------------------------------------------------------------------- */
struct Captured {std::string text;bool peeked;uint64_t memory;};
std::vector<Captured> lines;
int logOn=1;
unsigned checks;
#define TCHECK(x) do {++tr::checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

/* The register a line names, as the host doubles hold it: the SMMU double's registers, else the block's mapped window
 * (by name: the GMU window overlaps CX_MISC on the SoC, the host keeps them apart). */
bool peek(const std::string &block,uint64_t phys,unsigned bytes,uint64_t *value)
{
    if(block=="SMMU") {
        if(phys<0x3da0000 || phys+bytes>0x3da0000+sizeof smmuRegs)return false;
        const uint64_t at=(phys-0x3da0000)/4;
        *value=smmuRegs[at]|(bytes==8?uint64_t(smmuRegs[at+1])<<32:0);
        return true;
    }
    static const std::map<std::string,uint64_t> bases={{"GPU",0x3d00000},{"GMU",0x3d37000},{"GMUAO",0x3d40000},
        {"RSCC",0x3d50000},{"CXM",0x3d9e000},{"GPUCC",0x3d90000},{"GCC",0x100000},{"DRV2",0x16520000},{"AOSS",0xc300000},
        {"IPCC",0x406000}};
    const auto base=bases.find(block);
    if(base==bases.end())return false;
    const auto window=windows.find(base->second);
    if(window==windows.end() || phys<window->first || phys+bytes>window->first+window->second.size())return false;
    *value=0;
    std::memcpy(value,window->second.data()+(phys-window->first),bytes);
    return true;
}
}

extern "C" int boot_progress_trace_enabled(void) {return tr::logOn;}
extern "C" void boot_progress_trace(const char *text)
{
    TCHECK(text && std::strlen(text)<A830TraceLineBytes);
    tr::Captured line={text,false,0};
    /* A write line: the register's content when the line was written (the write must not have landed yet). */
    char block[16]={},op=0;unsigned long long address=0,value=0;
    if(std::sscanf(text,"g%*x.%*x %15s %c %llx %llx",block,&op,&address,&value)==4 && (op=='w' || op=='W'))
        line.peeked=tr::peek(block,address,op=='W'?8:4,&line.memory);
    tr::lines.push_back(line);
}

namespace tr {
/* ---- Part 1: A830Trace alone ---------------------------------------------------------------------------------------- */
std::vector<std::string> got;
void sink(const char *line) {got.emplace_back(line);}
const char *const names[]={"GPU","GMU","SMMU","SCM"};
enum : uint8_t {Gpu,Gmu,Smmu,Scm};
std::string hex(uint64_t v) {char b[24];std::snprintf(b,sizeof b,"%llx",static_cast<unsigned long long>(v));return b;}
void expect(const std::vector<std::string> &want)
{
    if(got!=want) {
        std::fprintf(stderr,"got %zu lines, want %zu\n",got.size(),want.size());
        for(size_t i=0;i<std::max(got.size(),want.size());++i)
            std::fprintf(stderr,"  %-60s | %s\n",i<got.size()?got[i].c_str():"",i<want.size()?want[i].c_str():"");
        std::exit(1);
    }
    ++checks;
    got.clear();
}
/* A read: its line, then what it returned. */
void read(A830Trace &t,uint8_t stage,uint16_t step,uint8_t block,uint64_t address,uint64_t value)
{
    const A830TraceToken token=t.access(stage,step,block,A830TraceRead,address,0);
    t.value(token,value);
}
unsigned unitCases()
{
    unsigned cases=0;
    A830Trace t;
    t.begin(sink,names,4);
    /* Single accesses: the line before, the value after; a write carries its value. */
    t.access(1,0x10,Gpu,A830TraceWrite,0x3d00100,5);
    read(t,1,0x10,Gpu,0x3d00104,7);
    expect({"g1.10 GPU w 3d00100 5","g1.10 GPU r 3d00104","g1.10 GPU r 3d00104 =7"});
    ++cases;
    /* A poll of 3000 reads: 1-4, the powers of two, 1024 and 2048; its end line when another register comes. */
    std::vector<std::string> want;
    for(uint32_t n=1;n<=3000;++n) {
        read(t,5,0x10,Gmu,0x3d7e070,n==3000?0x100:0);
        if(n<=4 || (n<1024 && !(n&(n-1))) || !(n%1024)) {
            const std::string p="g5.10 GMU r 3d7e070"+(n>1?" #"+std::to_string(n):std::string());
            want.push_back(p);want.push_back(p+" =0");
        }
    }
    t.access(5,0x10,Gmu,A830TraceWrite,0x3d7e074,1);
    want.push_back("g5.10 GMU r 3d7e070 x3000 =100");
    want.push_back("g5.10 GMU w 3d7e074 1");
    TCHECK(want.size()==2*13+2);
    expect(want);
    ++cases;
    /* A sequential block of 139 words (the GMU TCM): words 1-16, 32, 64, 128, then its end line with the last address. */
    want.clear();
    t.stageBegin(5,"GMU-HFI",100);
    want.push_back("g5 GMU-HFI begin t=100");
    for(uint32_t n=0;n<139;++n) {
        t.access(5,4,Gmu,A830TraceWrite,0x3d37000+4*n,0x1000+n);
        const uint32_t k=n+1;
        if(k<=16 || k==32 || k==64 || k==128)
            want.push_back("g5.4 GMU w "+hex(0x3d37000+4*n)+" "+hex(0x1000+n)+(k>1?" #"+std::to_string(k):std::string()));
    }
    t.stageEnd(5,"GMU-HFI",A830TraceOk,0,0,0,0,0,2500);
    want.push_back("g5.4 GMU w 3d37000 x139 ..3d37228 =108a");
    want.push_back("g5 GMU-HFI end ok 2500us");
    expect(want);
    ++cases;
    /* A polling round over three registers (status, two command words): ten rounds count in three runs, whose end lines
     * come in slot order at the next new register; nothing in between. */
    want.clear();
    for(uint32_t round=1;round<=10;++round)
        for(uint64_t address:{0x16520d20ull,0x16520d40ull,0x16520d58ull}) {
            read(t,1,0x10,Gpu,address,round);
            if(round<=4 || round==8) {
                const std::string p="g1.10 GPU r "+hex(address)+(round>1?" #"+std::to_string(round):std::string());
                want.push_back(p);want.push_back(p+" ="+hex(round));
            }
        }
    t.access(1,0x10,Gpu,A830TraceWrite,0x16520d08,1);
    for(uint64_t address:{0x16520d20ull,0x16520d40ull,0x16520d58ull})want.push_back("g1.10 GPU r "+hex(address)+" x10 =a");
    want.push_back("g1.10 GPU w 16520d08 1");
    expect(want);
    ++cases;
    /* Eight registers polled in turn still count; a ninth pushes the oldest out, which then starts a new run. */
    for(uint32_t round=1;round<=5;++round)
        for(uint32_t r=0;r<8;++r)read(t,6,0x41,Gpu,0x3d00000+0x100*r,0);
    got.clear();
    read(t,6,0x41,Gpu,0x3d00900,0);   /* new: the 8 runs end (x5), the least recently used slot (register 0) goes */
    read(t,6,0x41,Gpu,0x3d00000,0);   /* register 0 again: a new run, no "#" */
    TCHECK(got.size()==8+2+2 && got[0]=="g6.41 GPU r 3d00000 x5 =0" && got[7]=="g6.41 GPU r 3d00700 x5 =0");
    TCHECK(got[8]=="g6.41 GPU r 3d00900" && got[10]=="g6.41 GPU r 3d00000" && got[11]=="g6.41 GPU r 3d00000 =0");
    got.clear();
    ++cases;
    /* Writes: the same value again is a poll; another value is a new line without "#" (the register keeps one run). */
    for(uint32_t n=0;n<6;++n)t.access(7,2,Gpu,A830TraceWrite,0x3d00800,1);
    t.access(7,2,Gpu,A830TraceWrite,0x3d00800,2);
    t.access(7,2,Gpu,A830TraceWrite,0x3d00800,2);
    expect({"g7.2 GPU w 3d00800 1","g7.2 GPU w 3d00800 1 #2","g7.2 GPU w 3d00800 1 #3","g7.2 GPU w 3d00800 1 #4",
            "g7.2 GPU w 3d00800 x6 =1","g7.2 GPU w 3d00800 2","g7.2 GPU w 3d00800 2 #2"});
    ++cases;
    /* A step change ends the runs under the old step; a note ends them too, without forgetting them. */
    for(uint32_t n=0;n<6;++n)read(t,3,1,Smmu,0x3da0020,0x4c);
    got.clear();
    read(t,3,2,Smmu,0x3da0000,0);
    TCHECK(got.size()==3 && got[0]=="g3.1 SMMU r 3da0020 x6 =4c" && got[1]=="g3.2 SMMU r 3da0000");
    got.clear();
    for(uint32_t n=0;n<5;++n)read(t,3,2,Smmu,0x3da0000,0);
    got.clear();
    t.note(3,2,"marker",2,0x10,0x20);
    read(t,3,2,Smmu,0x3da0000,0);   /* still the same run: #7 has no line, and the run ends again at the next flush */
    t.flush();
    expect({"g3.2 SMMU r 3da0000 x6 =0","g3.2 note marker 10 20","g3.2 SMMU r 3da0000 x7 =0"});
    ++cases;
    /* SMCs: x0 x1 x2 x3 before, x0 x1 x2 after; the same call again is a poll, other arguments a new line. */
    {
        A830TraceToken token=t.access(2,3,Scm,A830TraceCall,0x42002801,1,9,0);
        t.value(token,0,0,0);
        token=t.access(2,3,Scm,A830TraceCall,0x42002801,1,9,0);
        t.value(token,uint64_t(-4),0,0);
        token=t.access(2,3,Scm,A830TraceCall,0x42002801,1,8,0);
        t.value(token,0,0,0);
        expect({"g2.3 SCM c 42002801 1 9 0","g2.3 SCM c 42002801 =0 0 0","g2.3 SCM c 42002801 1 9 0 #2",
                "g2.3 SCM c 42002801 #2 =fffffffffffffffc 0 0","g2.3 SCM c 42002801 1 8 0","g2.3 SCM c 42002801 =0 0 0"});
        ++cases;
    }
    /* 64-bit: R and W, sequential by 8. */
    t.access(3,0x231,Smmu,A830TraceWrite64,0x3da2020,0x30009a0001000ull);
    t.access(3,0x231,Smmu,A830TraceWrite64,0x3da2028,0x10);
    read(t,3,0x231,Smmu,0x3da2020,0x30009a0001000ull);
    {
        const A830TraceToken token=t.access(3,0x231,Smmu,A830TraceRead64,0x3da2028,0);
        t.value(token,0x10);
    }
    expect({"g3.231 SMMU W 3da2020 30009a0001000","g3.231 SMMU W 3da2028 10 #2","g3.231 SMMU r 3da2020",
            "g3.231 SMMU r 3da2020 =30009a0001000","g3.231 SMMU R 3da2028","g3.231 SMMU R 3da2028 =10"});
    ++cases;
    /* Stage, operation, start and done lines. */
    t.stageEnd(3,"SMMU",A830TraceFail,0xe00002d5,0x10f,10,0x3da1000,0x5,1234);
    t.stageEnd(3,"SMMU",A830TraceStop,0,0,0,0,0,7);
    t.operation("cxPowerOn",true,0,0);
    t.operation("cxPowerOn",false,0xe00002bc,321);
    t.opened(1,0xffffffff,42);
    t.opened(0,3,43);
    expect({"g3 SMMU end fail e00002d5 s10f r10 3da1000=5 1234us","g3 SMMU end stop 7us","g op cxPowerOn begin",
            "g op cxPowerOn end e00002bc 321us","g start p1 stop - t=42","g start p0 stop 3 t=43"});
    t.closed(0,99,2);
    TCHECK(got.size()==1 && got[0]=="g done 0 99us lines "+std::to_string(t.lines())+" drops 2");
    got.clear();
    ++cases;
    /* A value whose run slot was taken meanwhile still gets its line; a token of nothing traced writes nothing. */
    {
        const A830TraceToken token=t.access(4,1,Gpu,A830TraceRead,0x3d00010,0);
        for(uint32_t r=0;r<9;++r)t.access(4,1,Gpu,A830TraceWrite,0x3d01000+0x10*r,r+1);
        t.value(token,0x77);
        t.value(A830TraceToken{},0x55);
        TCHECK(got.size()==11 && got[10]=="g4.1 GPU r 3d00010 =77");
        got.clear();
        ++cases;
    }
    /* Bounded: a 300-character name cannot overrun the line. */
    {
        static char longName[301];
        std::memset(longName,'N',300);
        const char *const longNames[]={longName};
        A830Trace b;
        b.begin(sink,longNames,1);
        b.access(1,1,0,A830TraceWrite,~0ull,~0ull);
        TCHECK(got.size()==1 && got[0].size()==A830TraceLineBytes-1);
        got.clear();
        ++cases;
    }
    /* Verbatim (pagani-gpu-trace=2): no runs, no "#", no end lines. */
    {
        A830Trace v;
        v.begin(sink,names,4,true);
        for(uint32_t n=0;n<5;++n)read(v,5,4,Gmu,0x3d37000+4*n,n);
        for(uint32_t n=0;n<3;++n)read(v,5,4,Gmu,0x3d7e070,0);
        v.access(5,4,Gmu,A830TraceWrite,0x3d7e074,1);
        TCHECK(got.size()==8*2+1 && got[2]=="g5.4 GMU r 3d37004" && got[11]=="g5.4 GMU r 3d7e070 =0" &&
               got[12]=="g5.4 GMU r 3d7e070" && got[16]=="g5.4 GMU w 3d7e074 1");
        for(const std::string &line:got)TCHECK(line.find('#')==std::string::npos && line.find(" x")==std::string::npos);
        got.clear();
        ++cases;
    }
    return cases;
}

/* ---- Part 2: the orchestrator, scripted register traffic through the tables it hands the doubles --------------------- */
/* One scripted access: a write's value or what a read got; an SMC's x1..x3 (value, a2, a3) and x0..x2 after (out). */
struct Access {uint8_t stage;uint16_t step;std::string block;char op;uint64_t address,value,a2,a3,out[3];};
std::vector<Access> script;   /* what the scripts did, in order */
void did(const char *block,char op,uint64_t address,uint64_t value,int step=-1)
{
    const uint16_t now=step<0?adreno830_status.step:uint16_t(step);
    script.push_back({adreno830_status.stage,now,block,op,address,value,0,0,{0,0,0}});
}
/* The verbatim lines of one access (A830Trace.h). */
void linesOf(const Access &a,std::vector<std::string> *out)
{
    const std::string p="g"+hex(a.stage)+"."+hex(a.step)+" "+a.block+" "+a.op+" "+hex(a.address);
    switch(a.op) {
    case A830TraceWrite: case A830TraceWrite64: out->push_back(p+" "+hex(a.value));break;
    case A830TraceRead: case A830TraceRead64: out->push_back(p);out->push_back(p+" ="+hex(a.value));break;
    default:
        out->push_back(p+" "+hex(a.value)+" "+hex(a.a2)+" "+hex(a.a3));
        out->push_back(p+" ="+hex(a.out[0])+" "+hex(a.out[1])+" "+hex(a.out[2]));
    }
}

/* The DRV-2, AOSS and IPCC windows (Sm8750RpmhIo / Sm8750AossQmpIo). */
template<typename Io> uint32_t ioRead(const Io *io,const char *block,uint64_t base,uint32_t offset)
{
    const uint32_t value=io->read(io->context,offset);
    did(block,A830TraceRead,base+offset,value);
    return value;
}
template<typename Io> void ioWrite(const Io *io,const char *block,uint64_t base,uint32_t offset,uint32_t value)
{
    did(block,A830TraceWrite,base+offset,value);
    io->write(io->context,offset,value);
}
constexpr uint64_t Drv2=0x16520000,Gcc=0x100000,GpuCc=0x3d90000,Smmu0=0x3da0000,Aoss=0xc300000,Ipcc=0x406000;
void qmpScript(Sm8750AossQmp *qmp,int val)
{
    for(uint32_t i=0;i<16;++i)ioWrite(qmp->msgram,"AOSS",Aoss,0x100+4*i,0x61630000u+uint32_t(val)*0x100+i+1);
    ioWrite(qmp->msgram,"AOSS",Aoss,0xfc,64);
    ioWrite(qmp->ipcc,"IPCC",Ipcc,0xc,1);
    for(uint32_t i=0;i<3;++i)ioRead(qmp->msgram,"AOSS",Aoss,0xfc);
}
/* One ACTIVE TCS transaction as Sm8750Rpmh makes it: the slot checks, the commands, the trigger, a poll of the status and
 * the command words, the cleanup. */
void tcsScript(const Sm8750RpmhIo *drv2,unsigned commands,unsigned polls)
{
    ioRead(drv2,"DRV2",Drv2,0x0);ioRead(drv2,"DRV2",Drv2,0xc);
    for(uint32_t offset:{0xd24u,0xd28u,0xd2cu,0xd04u,0xd20u})ioRead(drv2,"DRV2",Drv2,offset);
    ioRead(drv2,"DRV2",Drv2,0xd00);
    ioWrite(drv2,"DRV2",Drv2,0xd00,6);
    for(uint32_t i=0;i<commands;++i) {
        ioWrite(drv2,"DRV2",Drv2,0xd34+24*i,0x10108);
        ioWrite(drv2,"DRV2",Drv2,0xd38+24*i,0x30080+0x10*i);
        ioWrite(drv2,"DRV2",Drv2,0xd3c+24*i,0x1+i);
    }
    ioWrite(drv2,"DRV2",Drv2,0xd2c,(1u<<commands)-1);
    ioWrite(drv2,"DRV2",Drv2,0xd28,(1u<<commands)-1);
    ioWrite(drv2,"DRV2",Drv2,0xd24,0x10000);
    ioWrite(drv2,"DRV2",Drv2,0xd24,0x10001);
    for(uint32_t p=0;p<polls;++p) {
        ioRead(drv2,"DRV2",Drv2,0xd20);
        for(uint32_t i=0;i<commands;++i)ioRead(drv2,"DRV2",Drv2,0xd40+24*i);
    }
    ioRead(drv2,"DRV2",Drv2,0xd04);
    ioWrite(drv2,"DRV2",Drv2,0xd24,0x10000);
    ioWrite(drv2,"DRV2",Drv2,0xd08,1);
    ioRead(drv2,"DRV2",Drv2,0xd00);
    ioWrite(drv2,"DRV2",Drv2,0xd00,7);
}
void railsScript(const Sm8750RpmhIo *drv2) {tcsScript(drv2,2,6);}
void cn0Script(const Sm8750RpmhIo *drv2) {tcsScript(drv2,1,2);}
void ddrScript(const Sm8750RpmhIo *drv2,uint32_t) {tcsScript(drv2,3,3);}
/* GCC/GPU_CC through Sm8750GpuPowerIo: the lines carry the power-up step the module keeps in its trace. */
uint32_t powerRead(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *t,unsigned block,uint32_t offset)
{
    uint32_t value=0;
    TCHECK(!io->read32(io->cookie,block,offset,&value));
    did(block==Sm8750GpuGcc?"GCC":"GPUCC",A830TraceRead,(block==Sm8750GpuGcc?Gcc:GpuCc)+offset,value,int(t->step));
    return value;
}
void powerWrite(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *t,unsigned block,uint32_t offset,uint32_t value)
{
    did(block==Sm8750GpuGcc?"GCC":"GPUCC",A830TraceWrite,(block==Sm8750GpuGcc?Gcc:GpuCc)+offset,value,int(t->step));
    TCHECK(!io->write32(io->cookie,block,offset,value));
}
void powerScript(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *t,bool on)
{
    if(!on) {
        t->step=Sm8750GpuStepOffBranches;
        powerWrite(io,t,Sm8750GpuGpuCc,0x90bc,0x2);
        for(int i=0;i<2;++i)powerRead(io,t,Sm8750GpuGpuCc,0x90bc);
        t->step=Sm8750GpuStepOffGdsc;
        powerWrite(io,t,Sm8750GpuGpuCc,0x9080,0x1);
        for(int i=0;i<4;++i)powerRead(io,t,Sm8750GpuGpuCc,0x9084);
        return;
    }
    t->step=Sm8750GpuStepGccAhb;powerWrite(io,t,Sm8750GpuGcc,0x71004,0x1);
    t->step=Sm8750GpuStepResets;powerWrite(io,t,Sm8750GpuGpuCc,0x907c,0x1);powerWrite(io,t,Sm8750GpuGpuCc,0x907c,0x0);
    t->step=Sm8750GpuStepCxGdsc;
    powerRead(io,t,Sm8750GpuGpuCc,0x9080);
    powerWrite(io,t,Sm8750GpuGpuCc,0x9080,0x200000);
    for(int i=0;i<300;++i)powerRead(io,t,Sm8750GpuGpuCc,0x9080);   /* the tight poll: 301 reads of CX_GDSCR */
    t->step=Sm8750GpuStepGpll0;
    powerRead(io,t,Sm8750GpuGcc,0x52000);powerWrite(io,t,Sm8750GpuGcc,0x52000,0x18000);
    for(int i=0;i<5;++i)powerRead(io,t,Sm8750GpuGcc,0x0);
    t->step=Sm8750GpuStepPll0;
    const uint32_t config[7]={0x440034,0x1555,0x400,0x2,0x19660387,0x098060a0,0xb416cb20};
    for(uint32_t i=0;i<7;++i)powerWrite(io,t,Sm8750GpuGpuCc,0x10+4*i,config[i]);
    powerWrite(io,t,Sm8750GpuGpuCc,0x0,0x4);powerWrite(io,t,Sm8750GpuGpuCc,0x0,0x7);
    for(int i=0;i<20;++i)powerRead(io,t,Sm8750GpuGpuCc,0x0);
    t->step=Sm8750GpuStepRcg;
    powerWrite(io,t,Sm8750GpuGpuCc,0x9318,0x1);
    for(int i=0;i<3;++i)powerRead(io,t,Sm8750GpuGpuCc,0x9318);
    powerWrite(io,t,Sm8750GpuGpuCc,0x93ec,0x1);
    for(int i=0;i<2;++i)powerRead(io,t,Sm8750GpuGpuCc,0x93ec);
    t->step=Sm8750GpuStepBranches;
    powerWrite(io,t,Sm8750GpuGpuCc,0x90bc,0x1);
    for(int i=0;i<2;++i)powerRead(io,t,Sm8750GpuGpuCc,0x90bc);
}
/* One SMC through the orchestrator's table: x0..x3 before, x0..x2 after (the host instruction answers x0 = 0). */
void scmScript(Sm8750Scm *scm,uint32_t function,uint64_t argument)
{
    const bool fast=function&0x80000000u;
    uint64_t regs[8]={function,fast?0u:1u,argument,0,0,0,0,0};
    const uint64_t x1=regs[1],x2=regs[2],x3=regs[3];
    TCHECK(scm && scm->ops && scm->ops->smc && !scm->ops->smc(scm->ops->cookie,regs));
    did("SCM",A830TraceCall,function,x1);
    Access &a=script.back();
    a.a2=x2;a.a3=x3;a.out[0]=regs[0];a.out[1]=regs[1];a.out[2]=regs[2];
}
/* The SMMU io (ArmSmmu4KIo). */
uint64_t smmuRead(const ArmSmmu4KIo *io,uint32_t offset,bool wide=false)
{
    uint64_t value=0;
    if(wide)TCHECK(!io->read64(io->cookie,offset,&value));
    else {uint32_t v=0;TCHECK(!io->read32(io->cookie,offset,&v));value=v;}
    did("SMMU",wide?A830TraceRead64:A830TraceRead,Smmu0+offset,value);
    return value;
}
void smmuWrite(const ArmSmmu4KIo *io,uint32_t offset,uint64_t value,bool wide=false)
{
    did("SMMU",wide?A830TraceWrite64:A830TraceWrite,Smmu0+offset,value);
    if(wide)TCHECK(!io->write64(io->cookie,offset,value));
    else TCHECK(!io->write32(io->cookie,offset,uint32_t(value)));
}
uint32_t bankOffset(unsigned bank) {return (32+bank)*A830_SMMU_PAGE_BYTES;}
void smmuScript(const ArmSmmu4KIo *io,const char *call,unsigned bank)
{
    TCHECK(io);
    const std::string what=call;
    if(what=="survey") {
        smmuRead(io,A830_SMMU_GR0_IDR0);smmuRead(io,A830_SMMU_GR0_IDR1);smmuRead(io,A830_SMMU_GR0_IDR2);
        smmuRead(io,A830_SMMU_GR0_SCR0);smmuRead(io,A830_SMMU_GR0_SACR);smmuRead(io,A830_SMMU_GR0_SGFSR);
        for(uint32_t n=0;n<24;++n) {smmuRead(io,A830_SMMU_GR0_SMR(n));smmuRead(io,A830_SMMU_GR0_S2CR(n));}
        for(unsigned cb=0;cb<4;++cb) {
            const uint32_t off=bankOffset(cb);
            smmuRead(io,off+A830_SMMU_CB_SCTLR);smmuRead(io,off+A830_SMMU_CB_ACTLR);smmuRead(io,off+A830_SMMU_CB_TCR);
            smmuRead(io,off+A830_SMMU_CB_TTBR0,true);smmuRead(io,off+A830_SMMU_CB_FSR);
        }
    } else if(what=="releaseCacheLock") {
        smmuRead(io,A830_SMMU_GR0_SACR);
        if(!adreno830_annibale())smmuWrite(io,A830_SMMU_GR0_SACR,0x10);
    } else if(what=="clearGlobalFault") {
        smmuRead(io,A830_SMMU_GR0_SGFSR);
        if(!adreno830_annibale())smmuWrite(io,A830_SMMU_GR0_SGFSR,0x80000002);
    } else if(what=="resetBank") {
        const uint32_t off=bankOffset(bank);
        smmuWrite(io,off+A830_SMMU_CB_SCTLR,0);
        smmuWrite(io,off+A830_SMMU_CB_FSR,A830_SMMU_FSR_FAULT_MASK);
        smmuWrite(io,off+A830_SMMU_CB_TLBIALL,0x1);
        smmuWrite(io,off+A830_SMMU_CB_TLBSYNC,0x1);
        for(int i=0;i<3;++i)smmuRead(io,off+A830_SMMU_CB_TLBSTATUS);
    } else if(what=="context") {
        const uint32_t off=bankOffset(bank),slot=bank==2?1:0,sid=bank==2?5:0,asid=bank+1;
        smmuWrite(io,off+A830_SMMU_CB_SCTLR,0);
        smmuWrite(io,off+A830_SMMU_CB_TCR,0x803519);
        smmuWrite(io,off+A830_SMMU_CB_TTBR0,uint64_t(asid)<<48|0x9a0001000ull,true);
        smmuWrite(io,off+A830_SMMU_CB_MAIR0,0xf404ff44);
        smmuWrite(io,off+A830_SMMU_CB_SCTLR,0x27);
        smmuWrite(io,A830_SMMU_GR0_SMR(slot),0x80000000u|sid);
        smmuWrite(io,A830_SMMU_GR0_S2CR(slot),bank);
        smmuWrite(io,off+A830_SMMU_CB_TLBIASID,asid);
        smmuWrite(io,off+A830_SMMU_CB_TLBSYNC,0x1);
        for(int i=0;i<2;++i)smmuRead(io,off+A830_SMMU_CB_TLBSTATUS);
    } else TCHECK(!"unknown SMMU call");
}
/* A working GMU io over the windows (the A830GmuBindMmio double binds none), and the CX_MISC scripts through it. */
int gmuRead(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    const A830GmuMmio *m=static_cast<const A830GmuMmio *>(cookie);
    if(window>=A830GmuWindows || !m->base[window] || offset+4>m->bytes[window])return 1;
    std::memcpy(value,const_cast<const uint8_t *>(m->base[window])+offset,4);
    return 0;
}
int gmuWrite(void *cookie,unsigned window,uint32_t offset,uint32_t value)
{
    const A830GmuMmio *m=static_cast<const A830GmuMmio *>(cookie);
    if(window>=A830GmuWindows || !m->base[window] || offset+4>m->bytes[window])return 1;
    std::memcpy(const_cast<uint8_t *>(m->base[window])+offset,&value,4);
    return 0;
}
void gmuBarrier(void *) {}
void gmuBind(A830GmuMmio *mmio,A830GmuIo *io) {*io={mmio,gmuRead,gmuWrite,gmuBarrier};}
constexpr uint64_t CxMisc=0x3d9e000;
void ahbScript(A830CxMisc &cx)
{
    const A830GmuIo &io=*cx.io;
    uint32_t inherited[5]={};
    for(uint32_t i=0;i<5;++i) {
        TCHECK(!io.read32(io.cookie,A830GmuWindowCxMisc,0x40+4*i,&inherited[i]));
        did("CXM",A830TraceRead,CxMisc+0x40+4*i,inherited[i]);
    }
    for(uint32_t i=0;i<5;++i) {
        if(inherited[i]==0x1B12)continue;
        did("CXM",A830TraceWrite,CxMisc+0x40+4*i,0x1B12);
        TCHECK(!io.write32(io.cookie,A830GmuWindowCxMisc,0x40+4*i,0x1B12));
        uint32_t back=0;
        TCHECK(!io.read32(io.cookie,A830GmuWindowCxMisc,0x40+4*i,&back));
        did("CXM",A830TraceRead,CxMisc+0x40+4*i,back);
    }
}
unsigned seeds;
void seedScript(A830CxMisc &cx)   /* the design seed's 17 LO/HI pairs, with interrupts off: no line may show them */
{
    const A830GmuIo &io=*cx.io;
    for(uint32_t i=0;i<17;++i) {
        TCHECK(!io.write32(io.cookie,A830GmuWindowCxMisc,0x200,0x1000+i));
        TCHECK(!io.write32(io.cookie,A830GmuWindowCxMisc,0x204,0));
    }
    ++seeds;
}
void install()
{
    ioScript={qmpScript,railsScript,cn0Script,ddrScript,powerScript,scmScript,smmuScript,ahbScript,seedScript,gmuBind};
}

/* '*' in a wanted line matches one run of (hex) digits: a time, a duration, a line count. */
bool matches(const std::string &line,const std::string &want)
{
    auto digit=[](char c) {return (c>='0' && c<='9') || (c>='a' && c<='f');};
    size_t i=0,j=0;
    while(j<want.size()) {
        if(want[j]=='*') {
            ++j;
            if(i>=line.size() || !digit(line[i]))return false;
            while(i<line.size() && digit(line[i]))++i;
            continue;
        }
        if(i>=line.size() || line[i]!=want[j])return false;
        ++i;++j;
    }
    return i==line.size();
}
void expectTrace(const std::vector<std::string> &want)
{
    bool same=lines.size()==want.size();
    for(size_t i=0;same && i<want.size();++i)same=matches(lines[i].text,want[i]);
    if(!same) {
        std::fprintf(stderr,"trace: %zu lines, want %zu\n",lines.size(),want.size());
        size_t first=0;
        while(first<lines.size() && first<want.size() && matches(lines[first].text,want[first]))++first;
        for(size_t i=first>3?first-3:0;i<std::min(std::max(lines.size(),want.size()),first+12);++i)
            std::fprintf(stderr,"  %c %-56s | %s\n",i==first?'>':' ',i<lines.size()?lines[i].text.c_str():"",
                         i<want.size()?want[i].c_str():"");
        std::exit(1);
    }
    ++checks;
}
uint32_t word(const char *block,uint64_t phys) {uint64_t v=0;TCHECK(peek(block,phys,4,&v));return uint32_t(v);}
uint64_t dwordPhys(uint32_t dword) {return 0x3d00000ull+4ull*dword;}
/* S1-S3 of one profile: the whole verbatim trace, line for line. */
void verbatimRun(uint32_t profile,unsigned *runs)
{
    Adreno830Board b=board13t(profile);
    b.knobs.trace=2;b.knobs.stop=A830SSmmu;b.knobs.qmp=1;
    lines.clear();script.clear();
    TCHECK(run(&b)==kIOReturnSuccess && adreno830_status.state==A830Stopped && adreno830_status.stage==A830SSmmu);
    std::vector<std::string> want={"g start p"+std::to_string(profile)+" stop 3 t=*","g0 INPUTS begin t=*","g0 INPUTS end ok *us",
                                   "g1 POWER begin t=*"};
    size_t at=0;
    auto scripted=[&](uint8_t stage) {for(;at<script.size() && script[at].stage==stage;++at)linesOf(script[at],&want);};
    auto own=[&](uint8_t stage,uint16_t step,const char *block,uint64_t phys) {   /* the orchestrator's own reads */
        linesOf(Access{stage,step,block,A830TraceRead,phys,word(block,phys),0,0,{0,0,0}},&want);
    };
    scripted(A830SPower);
    own(1,A830S1StepCm3,"GMU",dwordPhys(A8XX_GMUCX_CM3_SYSRESET));
    own(1,A830S1StepCm3,"GPUCC",GpuCc+SM8750_GPUCC_PLL0_MODE);
    want.insert(want.end(),{"g1 POWER end ok *us","g2 SECCX begin t=*"});
    scripted(A830SSecureCx);
    own(2,A830S2StepFuse,"CXM",dwordPhys(A8XX_CX_MISC_SW_FUSE_VALUE));
    own(2,A830S2StepFuse,"CXM",dwordPhys(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS));
    own(2,A830S2StepFuse,"CXM",dwordPhys(A8XX_CX_MISC_TCM_RET_CNTL));
    want.insert(want.end(),{"g2 SECCX end ok *us","g3 SMMU begin t=*"});
    /* S3 up to the aperture grant (step 0x40), then the note of the aperture the GPU context built (4 GiB, 2053 table pages, in
     * both profiles), then the grant. */
    for(;at<script.size() && script[at].stage==A830SSmmu && script[at].step!=A830S3StepAperture;++at)linesOf(script[at],&want);
    want.push_back("g3."+hex(A830S3StepContext+A830S3Cb0)+" note gpu aperture GiB, table pages 10 2011");
    scripted(A830SSmmu);
    TCHECK(at==script.size());
    /* The grant's outcome, once per boot (M14 P0): the scripted SMC answered x0 0, so granted. */
    want.push_back("g3."+hex(A830S3StepAperture)+" note gpu aperture 1 x0=0");
    want.insert(want.end(),{"g3 SMMU end stop *us","g done 0 *us lines "+std::to_string(want.size()+2)+" drops 0"});
    expectTrace(want);
    /* What the scripts did, by stage and step: QMP at step 0, the rails at 0x10, the power-up at its own steps 1-7, the
     * annibale AHB timeouts at 0x30; SCM at 1-3, CN0/DDR at 4/5; the SMMU at 4.5.3/4.5.4's steps, the aperture at 0x40. */
    auto steps=[&](uint8_t stage) {
        std::vector<uint16_t> seen;
        for(const Access &a:script)if(a.stage==stage && (seen.empty() || seen.back()!=a.step))seen.push_back(a.step);
        return seen;
    };
    if(profile==A830ProfileAnnibale)TCHECK((steps(1)==std::vector<uint16_t>{0,0x10,1,2,3,4,5,6,7,0x30}));
    else TCHECK((steps(1)==std::vector<uint16_t>{0,0x10,1,2,3,4,5,6,7}));
    TCHECK((steps(2)==std::vector<uint16_t>{1,2,3,4,5}));
    TCHECK((steps(3)==std::vector<uint16_t>{1,3,4,0x20f,0x10f,0x231,0x131,0x40}));
    /* Every write line went out while its register still held the old value (each scripted write stores a new value). */
    std::map<uint64_t,uint64_t> last;
    unsigned before=0;
    for(const Captured &line:lines) {
        char block[16]={},op=0;unsigned long long address=0,value=0;
        if(std::sscanf(line.text.c_str(),"g%*x.%*x %15s %c %llx %llx",block,&op,&address,&value)!=4 || (op!='w' && op!='W'))
            continue;
        const uint64_t old=last.count(address)?last[address]:0;
        last[address]=value;
        if(!line.peeked || value==old || (std::string(block)=="SMMU" && op=='W'))continue;   /* the double drops 64-bit stores */
        TCHECK(line.memory!=value);
        ++before;
    }
    TCHECK(before>=60);
    /* Every SMC line shows the function id the instruction then replaced with 0. */
    unsigned calls=0;
    for(const Captured &line:lines)
        if(line.text.find(" SCM c 42")!=std::string::npos || line.text.find(" SCM c c2")!=std::string::npos) {
            ++calls;
            TCHECK(line.text.find(" =")==std::string::npos?line.text.find(" c 0 ")==std::string::npos:
                   line.text.find(" =0 ")!=std::string::npos);
        }
    TCHECK(calls==2*(1+3+1+1));
    ++*runs;
}
/* A run end line ("x<n>"), and a line without its "#<n>", its times and its line count. */
bool isEnd(const std::string &line) {return line.size()>2 && line[1]!=' ' && line.find(" x")!=std::string::npos;}
std::string untimed(const std::string &line)
{
    std::string out,token;
    bool skipNext=false;
    auto flushToken=[&]() {
        if(token.empty())return;
        const bool drop=skipNext || token[0]=='#' || token.rfind("t=",0)==0 ||
                        (token.size()>2 && token.compare(token.size()-2,2,"us")==0 && token[0]>='0' && token[0]<='9');
        skipNext=token=="lines";
        if(!drop) {if(!out.empty())out+=' ';out+=token;}
        token.clear();
    };
    for(char c:line) {if(c==' ')flushToken();else token+=c;}
    flushToken();
    return out;
}
size_t countedLines,verbatimLines;
unsigned runEnds;
/* The same run with pagani-gpu-trace=1: the verbatim lines with the runs counted. */
void countedRun(uint32_t profile,unsigned *runs)
{
    Adreno830Board b=board13t(profile);
    b.knobs.trace=2;b.knobs.stop=A830SSmmu;b.knobs.qmp=1;
    lines.clear();script.clear();
    TCHECK(run(&b)==kIOReturnSuccess);
    std::vector<std::string> verbatim;
    for(const Captured &line:lines)verbatim.push_back(line.text);
    b.knobs.trace=1;
    lines.clear();script.clear();
    TCHECK(run(&b)==kIOReturnSuccess);
    std::vector<std::string> counted;
    for(const Captured &line:lines)counted.push_back(line.text);
    TCHECK(counted.size()+500<verbatim.size());   /* the 301-read CX GDSC poll alone gives up 580 lines */
    /* Without its "#<n>", every counted access or value line is a verbatim line, in the same order; the stage, start and
     * done lines are the same but for times and the line count. */
    size_t v=0;
    unsigned ends=0;
    for(const std::string &line:counted) {
        if(isEnd(line)) {++ends;continue;}   /* run end lines: below */
        const std::string plain=untimed(line);
        while(v<verbatim.size() && untimed(verbatim[v])!=plain)++v;
        TCHECK(v<verbatim.size());
        ++v;
    }
    if(profile==A830ProfileDesign) {countedLines=counted.size();verbatimLines=verbatim.size();runEnds=ends;}
    auto has=[&](const std::string &want) {return std::find(counted.begin(),counted.end(),want)!=counted.end();};
    auto missing=[&](const std::string &prefix) {
        for(const std::string &line:counted)if(line.rfind(prefix,0)==0)return false;
        return true;
    };
    /* The CX GDSC poll: 301 reads give lines 1-4, 8 ... 256 and the end line under its step (3) when the step changes. */
    for(const char *n:{"#2","#3","#4","#8","#16","#32","#64","#128","#256"})
        TCHECK(has(std::string("g1.3 GPUCC r 3d99080 ")+n) && has(std::string("g1.3 GPUCC r 3d99080 ")+n+" =200000"));
    TCHECK(has("g1.3 GPUCC r 3d99080 x301 =200000") && missing("g1.3 GPUCC r 3d99080 #5") &&
           missing("g1.3 GPUCC r 3d99080 #300"));
    /* The PLL0 configuration (7 distinct registers in a row) keeps every line. */
    TCHECK(has("g1.5 GPUCC w 3d90028 b416cb20 #7") && missing("g1.5 GPUCC w 3d90010 x"));
    /* The rails poll over three registers: four rounds each, then its end lines. */
    /* (STATUS was read once by the slot check before the six polls: the same register, one run of 7.) */
    TCHECK(has("g1.10 DRV2 r 16520d20 #4 =0") && missing("g1.10 DRV2 r 16520d20 #5") &&
           has("g1.10 DRV2 r 16520d20 x7 =0") && has("g1.10 DRV2 r 16520d40 x6 =0") && has("g1.10 DRV2 r 16520d58 x6 =0"));
    /* The SMR/S2CR survey (24 each, interleaved): words 1-16 of both, their end lines; the CB reads keep theirs. */
    TCHECK(has("g3.1 SMMU r 3da083c #16") && has("g3.1 SMMU r 3da0c3c #16 =0") && missing("g3.1 SMMU r 3da0840") &&
           has("g3.1 SMMU r 3da0800 x24 ..3da085c =0") && has("g3.1 SMMU r 3da0c00 x24 ..3da0c5c =0") &&
           has("g3.1 SMMU R 3dc0020 =0") && has("g3.1 SMMU R 3dc3020 =0"));
    /* The annibale AHB timeouts: five reads, five writes and five read-backs, every one with its line. */
    if(profile==A830ProfileAnnibale)
        for(uint32_t i=0;i<5;++i) {
            const std::string at=hex(CxMisc+0x40+4*i),n=i?" #"+std::to_string(i+1):std::string();
            TCHECK(has("g1.30 CXM r "+at+n+" =0") && has("g1.30 CXM w "+at+" 1b12"+n) && has("g1.30 CXM r "+at+n+" =1b12"));
        }
    else TCHECK(missing("g1.30"));
    ++*runs;
}
}

int main()
{
    const size_t sizes[4]={88896,111892,25340,12088};   /* as test_start: the gen80000 lengths, a counter pattern */
    for(unsigned i=0;i<4;++i) {
        images[i].resize(sizes[i]);
        for(size_t b=0;b<sizes[i];++b)images[i][b]=uint8_t(b*(i+3)+i);
    }
    const unsigned unit=tr::unitCases();
    tr::install();
    unsigned verbatim=0,counted=0,silent=0,refused=0,quiet=0,scopes=0,meters=0;
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        tr::verbatimRun(profile,&verbatim);
        tr::countedRun(profile,&counted);
    }
    /* The aperture note follows what the GPU context built, in both profiles: 16 GiB and 8209 table pages for pagani-gpu-va=16, 4 GiB
     * and 2053 when that fell back (no 34-bit UBS or no 32 MiB of tables), 2 GiB and 1027 for pagani-gpu-va=2. */
    unsigned apertures=0;
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        struct Case {uint32_t va;bool fallback;const char *values;};
        for(const Case &c:{Case{16,false,"10 2011"},Case{16,true,"4 805"},Case{2,false,"2 403"}}) {
            Adreno830Board b=board13t(profile);
            b.knobs.stop=A830SSmmu;b.knobs.va=c.va;wideFallback=c.fallback;
            tr::lines.clear();
            TCHECK(run(&b)==kIOReturnSuccess);
            const std::string note=std::string("g3.")+tr::hex(A830S3StepContext+A830S3Cb0)+" note gpu aperture GiB, table pages "+
                                   c.values;
            TCHECK(std::count_if(tr::lines.begin(),tr::lines.end(),[&](const tr::Captured &line) {return line.text==note;})==1);
            ++apertures;
        }
    }
    wideFallback=false;
    /* Nothing without the trace or without the log: S1-S3 and a full run, both profiles. */
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)})
        for(const bool full:{false,true}) {
            Adreno830Board b=board13t(profile);
            if(!full)b.knobs.stop=A830SSmmu;
            b.knobs.trace=0;tr::lines.clear();
            TCHECK(run(&b)==kIOReturnSuccess && tr::lines.empty() && !tr::script.empty());
            b.knobs.trace=1;tr::logOn=0;tr::lines.clear();
            TCHECK(run(&b)==kIOReturnSuccess && tr::lines.empty());
            tr::logOn=1;
            silent+=2;
        }
    /* pagani-gpu-trace=3 does not exist: S0 refuses it (no line either: the level is not one the trace runs at). */
    {
        Adreno830Board b=board13t();
        b.knobs.trace=3;tr::lines.clear();
        TCHECK(run(&b)==kIOReturnBadArgument && adreno830_status.stage==A830SInputs && adreno830_status.step==A830S0StepKnobs &&
               adreno830_status.reason==A830ReasonArgument && adreno830_status.reg==Adreno830KnobTrace &&
               adreno830_status.value==3 && adreno830_status.expect==2 && tr::lines.empty());
        ++refused;
    }
    /* A failing stage: its end line carries IOReturn, step, reason, register and value, and the done line its IOReturn. */
    {
        Adreno830Board b=board13t();
        tr::lines.clear();
        faults["A830Smmu::survey"]={kIOReturnBusy,A830SmmuStepSlots,A830ReasonSmmuRoute,0};
        TCHECK(run(&b)==kIOReturnBusy && adreno830_status.stage==A830SSmmu && adreno830_status.state==A830Failed);
        faults.clear();
        const Adreno830Status &st=adreno830_status;
        const std::string busy=tr::hex(uint32_t(kIOReturnBusy));
        const std::string end="g3 SMMU end fail "+busy+" s"+tr::hex(st.step)+" r"+std::to_string(st.reason)+" "+tr::hex(st.reg)+
                              "="+tr::hex(st.value)+" *us";
        TCHECK(st.step==A830SmmuStepSlots && st.reason==A830ReasonSmmuRoute && tr::lines.size()>=2 &&
               tr::matches(tr::lines[tr::lines.size()-2].text,end) &&
               tr::matches(tr::lines.back().text,"g done "+busy+" *us lines * drops 0"));
        ++refused;
    }
    /* A full run (design profile, default trace): the CX timer seed writes no line, only its notes; the run ends with S10
     * and the done line; then the power and recovery operations open their scopes, and nothing else writes a line. */
    {
        Adreno830Board b=board13t();
        tr::lines.clear();tr::seeds=0;
        TCHECK(b.knobs.trace==1 && run(&b)==kIOReturnSuccess && adreno830_running() && tr::seeds==1);
        auto find=[](const std::string &want,size_t from=0) {
            for(size_t i=from;i<tr::lines.size();++i)if(tr::matches(tr::lines[i].text,want))return long(i);
            return -1l;
        };
        const long notice=find("g4.1 note cx timer seed: AO_COUNTER 3d9e200/204 writes not logged");
        const long result=find("g4.1 note cx timer seed 0 1");
        TCHECK(notice>0 && result==notice+1);
        for(const tr::Captured &line:tr::lines) {   /* no access line of AO_COUNTER_LO/HI */
            char block[16]={},op=0;unsigned long long address=0;
            if(std::sscanf(line.text.c_str(),"g%*x.%*x %15s %c %llx",block,&op,&address)==3)
                TCHECK(address!=0x3d9e200 && address!=0x3d9e204);
        }
        TCHECK(find("ga DCVS end ok *us")>result && tr::matches(tr::lines.back().text,"g done 0 *us lines * drops 0"));
        ++quiet;
        const size_t bringUp=tr::lines.size();
        /* Outside any scope: no line (the CP health hook reads GMU registers, the timer the AO counter). */
        TCHECK(cpHealth.gmu_fault && !cpHealth.gmu_fault(cpHealth.cookie) && tr::lines.size()==bringUp);
        /* cxPowerOn after the bring-up: its own scope, the power-up lines under the running stage (f) with the power steps. */
        TCHECK(powerOps.cxPowerOn(powerOps.context)==kIOReturnSuccess);
        TCHECK(tr::lines[bringUp].text=="g op cxPowerOn begin" && tr::lines[bringUp+1].text=="gf.1 GCC w 171004 1" &&
               tr::matches(tr::lines.back().text,"g op cxPowerOn end 0 *us"));
        ++scopes;
        /* The hang snapshot: CB0 then CB2 FSR, FAR low, FAR high, in that order. */
        const size_t snapshot=tr::lines.size();
        A830HangSnapshot hang={};
        recoveryOps.snapshot(recoveryOps.context,&hang);
        std::vector<std::string> reads;
        for(size_t i=snapshot;i<tr::lines.size();++i)
            if(tr::lines[i].text.find(" SMMU r ")!=std::string::npos && tr::lines[i].text.find(" =")==std::string::npos)
                reads.push_back(tr::lines[i].text);
        TCHECK(tr::lines[snapshot].text=="g op snapshot begin" &&
               (reads==std::vector<std::string>{"gf.0 SMMU r 3dc0058","gf.0 SMMU r 3dc0060","gf.0 SMMU r 3dc0064 #2",
                                                "gf.0 SMMU r 3dc2058","gf.0 SMMU r 3dc2060","gf.0 SMMU r 3dc2064 #2"}) &&
               tr::matches(tr::lines.back().text,"g op snapshot end 0 *us"));
        ++scopes;
        /* A recovery is one scope from its gate going to Recovering to Open: the accesses it makes itself between the
         * operations get lines (here the GMU reads of the CP health hook); a Lost with no recovery open writes nothing. */
        const size_t recovery=tr::lines.size();
        recoveryOps.gate(recoveryOps.context,A830GateRecovering,A830FaultHang);
        TCHECK(!cpHealth.gmu_fault(cpHealth.cookie));
        recoveryOps.gate(recoveryOps.context,A830GateOpen,A830FaultHang);
        const size_t closed=tr::lines.size();
        recoveryOps.gate(recoveryOps.context,A830GateLost,A830FaultHang);
        const std::string info=tr::hex(tr::dwordPhys(A8XX_GMUCX_GMU2HOST_INTR_INFO));
        const std::string init=tr::hex(tr::dwordPhys(A8XX_GMUCX_CM3_FW_INIT_RESULT));
        TCHECK(closed==recovery+6 && tr::lines.size()==closed && tr::lines[recovery].text=="g op recovery begin" &&
               tr::lines[recovery+1].text=="gf.0 GMU r "+info && tr::lines[recovery+2].text=="gf.0 GMU r "+info+" =0" &&
               tr::lines[recovery+3].text=="gf.0 GMU r "+init && tr::lines[recovery+4].text=="gf.0 GMU r "+init+" =0" &&
               tr::matches(tr::lines[recovery+5].text,"g op recovery end 0 *us"));
        ++scopes;
        /* The DCVS thread's queue op also feeds the ramoops meter line (M14 P0), outside every trace scope. This run has the default
         * overlay (pagani-gpu-status=1): no line, window after window. On the diagnostic page: one line at the sixtieth period while
         * the log runs, with the aperture outcome S3 stored (granted here); the same window again writes nothing; the log off writes
         * nothing. */
        const size_t before=tr::lines.size();
        TCHECK(adreno830_status_current_mode()==1);
        for(unsigned second=0;second<2*Adreno830StatusMeterPeriods;++second)lastDcvsOps.queueRow(lastDcvsOps.context);
        TCHECK(tr::lines.size()==before);
        adreno830_status_mode(2);
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)lastDcvsOps.queueRow(lastDcvsOps.context);
        TCHECK(tr::lines.size()==before+1 && tr::lines.back().text=="gpu meter rs 0 fw 0 n4 0 ext 0 0.0ms ctx 0 0/s poll 0 ap1");
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)lastDcvsOps.queueRow(lastDcvsOps.context);
        tr::logOn=0;
        queueToTake.submitted=1;
        for(unsigned second=0;second<Adreno830StatusMeterPeriods;++second)lastDcvsOps.queueRow(lastDcvsOps.context);
        tr::logOn=1;queueToTake={};
        TCHECK(tr::lines.size()==before+1);
        adreno830_status_mode(1);
        ++meters;
    }
    std::printf("PASS: %u meter line wiring case (the DCVS thread's queue op, one line per sixty periods outside the trace scopes "
                "on the diagnostic page, with S3's aperture outcome; none with the default overlay, none repeated, none with the log "
                "off)\n",meters);
    std::printf("PASS: A830Trace %u line-level cases; S1-S3 over scripted register traffic, design and annibale profiles: %u "
                "verbatim runs (every access in order with stage, step, block, address and value, writes before they land, "
                "SMCs before the instruction; design %zu lines), %u counted runs (polls 1-4 and powers of two, sequential "
                "runs to 16, %u end lines; design %zu lines), %u silent runs (pagani-gpu-trace=0 or the log off), %u stage "
                "failure and S0 refusal (trace=3) cases, %u quiet CX seed, %u operation scopes, %u aperture notes (16, 4 and 2 GiB)\n",
                unit,verbatim,tr::verbatimLines,counted,tr::runEnds,tr::countedLines,silent,refused,quiet,scopes,apertures);
    return 0;
}
