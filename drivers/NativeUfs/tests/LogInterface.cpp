// SPDX-License-Identifier: BSD-2-Clause-Patent
// The log window's interface (LogInterface.h) on the real command path and a model LUN, on the 13T's numbers: opening and its refusals, where
// `rel` lands, chunks, the close, the claim of the way out and its held commands, the GPT check. Then the actual writer (BootProgress UfsLog.c,
// linked in through Test/ufslog-e2e.c) over all of that: every command it sends lands inside the window, and what it leaves on the model LUN
// is its header at the right LBA. Built as the kernel is: -DNATIVE_UFS_READ_ONLY=1 -DNATIVE_UFS_LOG_WRITES=1. Host only.
//   LogInterface --off    sheng-log=0 (logMode keeps its first answer)
#define UFS_ROOT_PARTITION "unixnu"
#include "RootCommandsBridge.cpp"
#include "../LogWindow.h"
#include "../NativeUfsLog.h"
#include "LogLun.h"

extern "C" {
void e2e_boot(int resetAfter);
int e2e_open(void);
unsigned e2e_tick(void);
void e2e_emit(unsigned records);
int e2e_export(uint32_t generation,uint32_t total,const void *data);
void e2e_restart(void);
void e2e_panic(void);
int64_t e2e_resets(void);
}

// What NativeUfs.cpp has ready for LogInterface.h.
struct StoreGeometry { bool valid; uint64_t first,last,imageBytes,entriesLba; uint32_t headerCrc,entriesCrc,entryCount; };
static StoreGeometry gStoreGeometry;
static uint64_t gReserve;
static uint64_t reserveBlocks() { return gReserve; }
static uint32_t gLogArgument=1;
static bool PE_parse_boot_argn(const char *name,void *value,unsigned) {
  assert(!strcmp(name,"sheng-log")); *static_cast<uint32_t *>(value)=gLogArgument; return true;
}
static uint32_t le32(const uint8_t *at) { return uint32_t(at[0])|(uint32_t(at[1])<<8)|(uint32_t(at[2])<<16)|(uint32_t(at[3])<<24); }
static uint32_t rootCrc32(const uint8_t *data,size_t bytes,bool header=false)
{
  uint32_t crc=0xffffffffU;
  for (size_t i=0;i<bytes;++i) {
    crc^=header&&i>=16&&i<20?0:data[i];
    for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320U&(0U-(crc&1)));
  }
  return ~crc;
}
#include "../LogInterface.h"

static void put32(uint8_t *at,uint32_t value) { for (unsigned i=0;i<4;++i) at[i]=uint8_t(value>>(i*8)); }

// A fresh transport with a GPT on it, the root layout read, and nothing of the log open or closed.
static void fresh()
{
  reset();
  gHost.lock=&gHost;
  gLogRegionFirst=gLogRegionBlocks=0; gLogClosed=gLogPanicOwned=false;
  gReserve=1024*256;
  auto &header=lun[1]=filled(0), &entries=lun[2]=filled(0);
  lun[3]=lun[4]=lun[5]=filled(0);
  memcpy(entries.data()+56,"u\0n\0i\0x\0n\0u\0",12);
  const uint32_t entriesCrc=[&]{
    uint8_t all[4096*4]; for (unsigned i=0;i<4;++i) memcpy(all+4096*i,lun[2+i].data(),4096);
    return rootCrc32(all,128*128);
  }();
  memcpy(header.data(),"EFI PART",8); put32(header.data()+8,0x00010000); put32(header.data()+12,92);
  put32(header.data()+88,entriesCrc);
  const uint32_t headerCrc=rootCrc32(header.data(),92,true);
  put32(header.data()+16,headerCrc);
  gStoreGeometry={true,kUnixnuFirst,kUnixnuLast,0x22F400000,2,headerCrc,entriesCrc,128};
}

// Everything that reached the medium in a run lies in the window, or is the GPT this test wrote.
static void onlyWindow()
{
  for (const auto &entry: lun) assert(entry.first<=5 || (entry.first>=kExportFirst && entry.first<=kUnixnuLast));
  for (uint8_t opcode: opcodes) assert(opcode==0x28 || opcode==0x2a || opcode==0x35);
}

static void opening()
{
  uint64_t first=0,blocks=0; const char *why=nullptr;
  fresh(); gStoreGeometry.valid=false;
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"no root layout") && !native_ufs_export_blocks());
  fresh(); gHost.ready=false;
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"no root layout"));
  // Refusals leave everything shut: no write, no export, no flush, no claim.
  auto block=filled(0x5a);
  fresh(); lun[kLogFirst]=filled(0xa5);
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"content gate: foreign data at the log window's first block"));
  assert(!native_ufs_log_write(0,1,block.data()) && !native_ufs_export_write(0,1,block.data()) && !native_ufs_log_sync() && !native_ufs_log_panic(1000));
  assert(!gLogOpen && !native_ufs_export_blocks() && synced==0 && writes==0);
  fresh(); gReserve=0;
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"reserve too small for the log"));
  fresh(); gStoreGeometry.imageBytes=(kUnixnuLast-kUnixnuFirst+1-gReserve)*4096;
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"overlaps the root image"));
  fresh();
  assert(native_ufs_log_open(&first,&blocks,&why) && first==kLogFirst && blocks==kLogReserveBlocks && native_ufs_export_blocks()==kExportBlocks);
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"already open"));
  assert(gLogFirst==kExportFirst && gLogBlocks==kExportBlocks+kLogReserveBlocks);
  native_ufs_log_close();
  assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"closed"));
  assert(!gLogOpen && !native_ufs_log_write(0,1,block.data()) && !native_ufs_export_write(0,1,block.data()) && !native_ufs_log_sync());
  assert(!native_ufs_export_blocks() && !native_ufs_log_panic(1000) && writes==0);
}

static void writing()
{
  uint64_t first=0,blocks=0; const char *why=nullptr;
  fresh(); assert(native_ufs_log_open(&first,&blocks,&why));
  auto block=filled(0x5a);
  // `rel` counts from each region's first block, to its last.
  assert(native_ufs_log_write(0,1,block.data()) && lun.count(kLogFirst));
  assert(native_ufs_log_write(kLogReserveBlocks-1,1,block.data()) && lun.count(kUnixnuLast));
  assert(native_ufs_export_write(0,1,block.data()) && lun.count(kExportFirst));
  assert(native_ufs_export_write(kExportBlocks-1,1,block.data()) && lun.count(kLogFirst-1));
  const unsigned count=writes;
  for (const uint64_t rel: {kLogReserveBlocks,kLogReserveBlocks+1,uint64_t(1)<<32,UINT64_MAX}) assert(!native_ufs_log_write(rel,1,block.data()));
  assert(!native_ufs_log_write(kLogReserveBlocks-1,2,block.data()) && !native_ufs_log_write(0,0,block.data()) && !native_ufs_log_write(0,1,nullptr));
  for (const uint64_t rel: {kExportBlocks,kExportBlocks+1,uint64_t(1)<<32,UINT64_MAX}) assert(!native_ufs_export_write(rel,1,block.data()));
  assert(!native_ufs_export_write(kExportBlocks-1,2,block.data()) && !native_ufs_export_write(0,0,block.data()) && !native_ufs_export_write(0,1,nullptr));
  assert(writes==count && !lun.count(kUnixnuLast+1) && !lun.count(kExportFirst-1));
  // More than one command holds: chunks, in order, all of it there.
  std::vector<uint8_t> many(100*4096);
  for (size_t i=0;i<many.size();++i) many[i]=uint8_t(i>>12);
  assert(native_ufs_log_write(100,100,many.data()) && writes==count+4);
  assert(native_ufs_export_write(1000,100,many.data()) && writes==count+8);
  for (unsigned i=0;i<100;++i) {
    assert(lun[kLogFirst+100+i][7]==uint8_t(i));
    assert(lun[kExportFirst+1000+i][7]==uint8_t(i));
  }
  // A failure comes back as a failure.
  assert(native_ufs_log_sync() && synced==1);
  hang=true; assert(!native_ufs_log_write(0,1,block.data()) && gHost.poisoned);
  assert(!native_ufs_log_sync() && !native_ufs_export_write(0,1,block.data()));
  onlyWindow();
}

static void wayOut()
{
  uint64_t first=0,blocks=0; const char *why=nullptr;
  auto block=filled(0x5a);
  fresh(); assert(native_ufs_log_open(&first,&blocks,&why));
  // Nothing held yet: the held commands refuse.
  assert(!native_ufs_log_panic_write(0,1,block.data(),200000) && !native_ufs_log_panic_sync(200000) && writes==0);
  // A holder that does not let go costs the wait and nothing else, the claim's time is what was asked for.
  gLockBusy=UINT32_MAX;
  uint64_t started=now;
  assert(!native_ufs_log_panic(1000) && now-started>=1000 && now-started<1100 && !gLogPanicOwned);
  // One that does: claimed, for good, as often as it is asked.
  gLockBusy=50;
  assert(native_ufs_log_panic(1000) && gLogPanicOwned && gLockClaimed && native_ufs_log_panic(1000));
  assert(native_ufs_log_panic_write(1,1,block.data(),200000) && lun.count(kLogFirst+1));
  assert(native_ufs_log_panic_sync(200000) && synced==1);
  // The window's checks hold, the export region is out of its reach, and FUA or a write outside does not exist here.
  const unsigned count=writes;
  assert(!native_ufs_log_panic_write(kLogReserveBlocks,1,block.data(),200000) && !native_ufs_log_panic_write(kLogReserveBlocks-1,2,block.data(),200000));
  assert(!native_ufs_log_panic_write(UINT64_MAX,1,block.data(),200000) && !native_ufs_log_panic_write(0,1,nullptr,200000) && writes==count);
  // A short timeout is honoured; the transport is gone after it, and so is the claim's use.
  hang=true; started=now;
  assert(!native_ufs_log_panic_write(1,1,block.data(),5000) && now-started<8000 && gHost.poisoned);
  assert(!native_ufs_log_panic_sync(5000) && !native_ufs_log_panic(1000));
  // A quarantined transport is not claimed at all.
  fresh(); assert(native_ufs_log_open(&first,&blocks,&why));
  gHost.poisoned=true; gHost.ready=false;
  assert(!native_ufs_log_panic(1000) && !gLogPanicOwned && !gLockClaimed);
  // Quarantined while the claim waited: claimed, and refused; no command goes out.
  fresh(); assert(native_ufs_log_open(&first,&blocks,&why));
  gLockHook=[] { gHost.poisoned=true; gHost.ready=false; };
  assert(!native_ufs_log_panic(1000) && gLogPanicOwned);
  const unsigned refused=writes;
  assert(!native_ufs_log_panic_write(1,1,block.data(),5000) && !native_ufs_log_panic_sync(5000) && writes==refused && synced==0);
  onlyWindow();
}

static void gptCheck()
{
  uint64_t first=0,blocks=0; const char *why=nullptr;
  fresh(); assert(native_ufs_log_open(&first,&blocks,&why));
  assert(native_ufs_store_gpt_check()==0);
  fresh(); lun[3][9]=1; assert(native_ufs_store_gpt_check()==5);
  fresh(); lun[1][40]=1; assert(native_ufs_store_gpt_check()==3);
  fresh(); gStoreGeometry.headerCrc^=1; assert(native_ufs_store_gpt_check()==3);
  fresh(); gStoreGeometry.entryCount=1024; assert(native_ufs_store_gpt_check()==4);
  fresh(); gStoreGeometry.valid=false; assert(native_ufs_store_gpt_check()==1);
  fresh(); hang=true; assert(native_ufs_store_gpt_check()==2);
  assert(native_ufs_store_gpt_check()==1);                       // quarantined
}

// Fields of the header on the model LUN (UfsLog.h struct ufs_log_header).
enum : unsigned { kRounds=76, kState=92, kExportFirstField=96, kExportBytes=104, kExportGeneration=108 };
static uint32_t headerField(unsigned offset) { return le32(lun[kLogFirst].data()+offset); }

// The writer all the way down.
static void endToEnd()
{
  static uint8_t blob[100000];
  memset(blob,0x37,sizeof(blob)); memcpy(blob,"SHEXR001",8);
  // A run, an export, a reset.
  fresh(); e2e_boot(600);
  assert(e2e_open()==1);
  assert(!memcmp(lun[kLogFirst].data(),"PGLOG001",8) && le32(lun[kLogFirst].data()+24)==uint32_t(kLogFirst));
  for (unsigned round=0;round<40;++round) {
    e2e_emit(100);
    assert(e2e_tick());
    if (round==5) assert(!e2e_export(1,sizeof(blob),blob));
  }
  onlyWindow();
  assert(lun.count(kLogFirst+33) && !memcmp(lun[kExportFirst].data(),"SHEXR001",8));
  assert(headerField(kExportBytes)==sizeof(blob) && !headerField(kExportFirstField) && headerField(kExportGeneration)==1);
  assert(headerField(kState)==2 && headerField(kRounds)>=40);
  const unsigned before=writes;
  e2e_restart();
  assert(e2e_resets()==1 && writes==before+2 && gLogPanicOwned && headerField(kState)==6);
  const unsigned count=unsigned(opcodes.size());
  assert(opcodes[count-1]==0x35 && opcodes[count-2]==0x2a);                                 // the reset's flush is the last command
  onlyWindow();
  // A panic: the same, in state 5.
  fresh(); e2e_boot(600);
  assert(e2e_open()==1);
  e2e_emit(100);
  assert(e2e_tick());
  e2e_panic();
  assert(headerField(kState)==5 && gLogPanicOwned && e2e_resets()==1);
  onlyWindow();
  // A window that is not blank at the wrong place stays shut, and the writer with it.
  fresh(); lun[kUnixnuLast]=filled(1);
  e2e_boot(600);
  const unsigned none=writes;
  assert(e2e_open()==-1 && writes==none && !gLogOpen);
  e2e_restart();
  assert(e2e_resets()==1 && writes==none);
  // A GPT that changed under the log closes it.
  fresh(); e2e_boot(600);
  assert(e2e_open()==1);
  lun[3][9]=1;
  unsigned ticks=0;
  for (;ticks<100;++ticks) { e2e_emit(1); if (!e2e_tick()) break; }
  assert(ticks<40 && !gLogOpen && headerField(kState)==2);
  const unsigned closed=writes;
  e2e_restart();
  assert(writes==closed && !gLogPanicOwned);
}

int main(int argc,char **argv)
{
  if (argc==2 && !strcmp(argv[1],"--off")) {
    gLogArgument=0;
    uint64_t first=0,blocks=0; const char *why=nullptr;
    fresh();
    assert(!native_ufs_log_open(&first,&blocks,&why) && !strcmp(why,"off") && !gLogOpen && opcodes.empty());
    puts("PASS: sheng-log=0: the window stays shut, nothing read");
    return 0;
  }
  opening(); writing(); wayOut(); gptCheck(); endToEnd();
  puts("PASS: log interface on the real command path: opening, refusals, where rel lands, chunks, close, claim, held commands, GPT check; the actual writer down to the model LUN");
}
