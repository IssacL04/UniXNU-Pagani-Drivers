// SPDX-License-Identifier: BSD-2-Clause-Patent
// The diagnostic log's gates on the real command path (RootCommands.h through RootCommandsBridge.cpp) and on the checks of LogWindow.h,
// against a model LUN that keeps every block written to it. Built as the kernel is: -DNATIVE_UFS_READ_ONLY=1 -DNATIVE_UFS_LOG_WRITES=1;
// with NATIVE_UFS_READ_ONLY alone it proves that nothing writes; without either, hostWrite4k's own checks. Host only.
#include "RootCommandsBridge.cpp"
#include "../LogWindow.h"
#include "LogLun.h"
[[maybe_unused]] static bool writeRefused(const uint8_t *cdb,unsigned length,unsigned direction,uint32_t bytes)
{
  uint32_t scsi=0; const unsigned before=unsigned(opcodes.size());
  const uint32_t r=hostScsiDir(0,cdb,length,direction,bytes,200000,&scsi);
  return r==kIOReturnBadArgument && opcodes.size()==before;
}

// The plan: the 13T's partition, its reserve, its image.
static void plans()
{
  constexpr uint64_t first=kUnixnuFirst, last=kUnixnuLast, image=0x22F400000, reserve=1024*256;
  UfsLogPlan plan=ufsLogPlan("unixnu",first,last,image,reserve);
  assert(!plan.refusal && plan.logFirst==first+16769024 && plan.exportFirst==first+16703488);
  assert(plan.logFirst==124178176 && plan.exportFirst==124112640);
  assert(plan.logFirst+kLogReserveBlocks==last+1 && plan.exportFirst+kExportBlocks==plan.logFirst);
  assert(ufsLogPlan("userdata",first,last,image,reserve).refusal);
  assert(ufsLogPlan("unixn",first,last,image,reserve).refusal);
  assert(ufsLogPlan("unixnu2",first,last,image,reserve).refusal);
  assert(ufsLogPlan(nullptr,first,last,image,reserve).refusal);
  assert(ufsLogPlan("unixnu",first,last,image,kLogReserveBlocks+kExportBlocks-1).refusal);       // the export region would not fit
  assert(!ufsLogPlan("unixnu",first,last,image,kLogReserveBlocks+kExportBlocks).refusal);
  assert(ufsLogPlan("unixnu",last,first,image,reserve).refusal);                                 // inverted
  assert(ufsLogPlan("unixnu",0,UINT64_MAX,image,reserve).refusal);
  const uint64_t partition=last-first+1;
  assert(ufsLogPlan("unixnu",first,last,image,partition).refusal);                               // a wrap must not pass
  assert(ufsLogPlan("unixnu",first,last,image,partition+1).refusal);
  assert(ufsLogPlan("unixnu",first,last,(partition-reserve)*4096,reserve).refusal);              // up to the root image
  assert(ufsLogPlan("unixnu",first,last,(partition-reserve-1023)*4096,reserve).refusal);         // inside the 4 MiB guard
  assert(!ufsLogPlan("unixnu",first,last,(partition-reserve-1024)*4096,reserve).refusal);
}

// The content gate reads through the same path; a block that is neither blank nor ours refuses.
static void contents()
{
  constexpr uint64_t log=1000000, exp=log-kExportBlocks;
  uint8_t block[4096];
  auto read=[](uint64_t lba,uint8_t *to) { return ufs_test_read(0,lba,1,to)==0; };
  reset(); assert(!ufsLogContentGate(log,exp,block,read));
  const unsigned reads=unsigned(opcodes.size()); assert(reads==4);
  for (uint8_t op: opcodes) assert(op==0x28);                                                    // the gate reads and only reads
  assert(lun.empty());
  for (const uint64_t lba: {log,log+kLogReserveBlocks-1,exp,exp+kExportBlocks-1}) {
    reset(); lun[lba]=filled(0xa5); assert(ufsLogContentGate(log,exp,block,read)!=nullptr);
    reset(); lun[lba]=filled(0); lun[lba][4095]=1; assert(ufsLogContentGate(log,exp,block,read)!=nullptr);
    reset(); lun[lba]=filled(0); assert(!ufsLogContentGate(log,exp,block,read));
  }
  auto stamped=[](const char *magic) { auto b=filled(0x3c); memcpy(b.data(),magic,8); return b; };
  reset(); lun[log]=stamped("PGLOG001"); assert(!ufsLogContentGate(log,exp,block,read));
  reset(); lun[log]=stamped("PGLOG002"); assert(ufsLogContentGate(log,exp,block,read));
  reset(); lun[log]=stamped("SHEXR001"); assert(ufsLogContentGate(log,exp,block,read));          // each place has its own magic
  reset(); lun[exp]=stamped("SHEXR001"); assert(!ufsLogContentGate(log,exp,block,read));
  reset(); lun[exp]=stamped("PGLOG001"); assert(ufsLogContentGate(log,exp,block,read));
  reset(); lun[exp]=stamped("SHLOG001"); assert(ufsLogContentGate(log,exp,block,read));
  // The last block of each is never written: a magic there is an old window at another place.
  reset(); lun[log+kLogReserveBlocks-1]=stamped("PGLOG001"); assert(ufsLogContentGate(log,exp,block,read));
  reset(); lun[exp+kExportBlocks-1]=stamped("SHEXR001"); assert(ufsLogContentGate(log,exp,block,read));
  // An unreadable block refuses too, at each of the four places.
  for (unsigned failing=0;failing<4;++failing) {
    reset(); unsigned at=0;
    memset(block,0,sizeof(block));                                                               // what a failed read leaves behind reads blank
    auto flaky=[&](uint64_t lba,uint8_t *to) { return at++==failing?false:ufs_test_read(0,lba,1,to)==0; };
    assert(ufsLogContentGate(log,exp,block,flaky));
  }
}

// All of it together, on the 13T's numbers: a foreign block anywhere leaves the window closed; blank or ours opens exactly the window.
static void opens()
{
  constexpr uint64_t first=kUnixnuFirst, last=kUnixnuLast, image=0x22F400000, reserve=1024*256;
  const uint64_t log=first+16769024, exp=first+16703488;
  uint8_t block[4096];
  auto read=[](uint64_t lba,uint8_t *to) { return ufs_test_read(0,lba,1,to)==0; };
  auto open=[](uint64_t at,uint64_t size) { return ufs_test_log_open(at,size)!=0; };
  auto write=filled(0x11);
  reset(); lun[exp+kExportBlocks-1]=filled(1);
  uint64_t got=0;
  assert(ufsLogOpenWindow("unixnu",first,last,image,reserve,block,read,open,&got) && !got);
  assert(ufs_test_write(0,log,1,write.data())==kIOReturnBadArgument);            // refused: nothing opened
  reset();
  assert(ufsLogOpenWindow("userdata",first,last,image,reserve,block,read,open,&got) && opcodes.empty());
  assert(ufsLogOpenWindow("unixnu",first,last,image,kExportBlocks,block,read,open,&got) && opcodes.empty());
#if !defined(NATIVE_UFS_READ_ONLY) || !NATIVE_UFS_READ_ONLY || (defined(NATIVE_UFS_LOG_WRITES) && NATIVE_UFS_LOG_WRITES)
  assert(!ufsLogOpenWindow("unixnu",first,last,image,reserve,block,read,open,&got) && got==log);
  assert(ufs_test_write(0,log,1,write.data())==0 && ufs_test_write(0,exp,1,write.data())==0);
  assert(ufs_test_write(0,exp-1,1,write.data())==kIOReturnBadArgument && ufs_test_write(0,last+1,1,write.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,last,1,write.data())==0);
  assert(!strcmp(ufsLogOpenWindow("unixnu",first,last,image,reserve,block,read,open,&got),"content gate: foreign data at the log window's first block"));
#else
  assert(!strcmp(ufsLogOpenWindow("unixnu",first,last,image,reserve,block,read,open,&got),"window refused") && !got);
#endif
}

// The panic path's claim ends at its deadline whatever the holder does.
static void claims()
{
  unsigned tries=0, pauses=0, left=100;
  assert(!ufsLogClaim([&]{ ++tries; return false; },[&]{ return !left--; },[&]{ ++pauses; }));
  assert(tries==101 && pauses==100);
  tries=0; left=100;
  assert(ufsLogClaim([&]{ return ++tries==7; },[&]{ return !left--; },[]{}));
  assert(tries==7);
  tries=0;
  assert(ufsLogClaim([&]{ ++tries; return true; },[]{ assert(false); return true; },[]{ assert(false); }));
  assert(tries==1);
}

#if !defined(NATIVE_UFS_READ_ONLY) || !NATIVE_UFS_READ_ONLY
// Without the read-only gate in hostScsiDir, hostWrite4k's own check is what holds: the window, the LUN, the size.
static void gates()
{
  constexpr uint64_t first=kExportFirst, blocks=1000;
  auto block=filled(0x5a); std::vector<uint8_t> many(kDataMax,0x5a);
  reset(); assert(ufs_test_log_open(first,blocks));
  assert(ufs_test_write(0,first,1,block.data())==0 && ufs_test_write(0,first+blocks-1,1,block.data())==0 && writes==2);
  assert(ufs_test_write(0,first-1,1,block.data())==kIOReturnBadArgument && ufs_test_write(0,first+blocks,1,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first+blocks-1,2,many.data())==kIOReturnBadArgument && ufs_test_write(1,first,1,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first,uint32_t(kDataMax/4096)+1,many.data())==kIOReturnBadArgument);
  assert(writes==2);
}
#elif defined(NATIVE_UFS_LOG_WRITES) && NATIVE_UFS_LOG_WRITES
static void gates()
{
  constexpr uint64_t first=kExportFirst; constexpr uint64_t blocks=kExportBlocks+kLogReserveBlocks, end=first+blocks;
  auto block=filled(0x5a); std::vector<uint8_t> many(kDataMax,0x5a);
  reset();
  // Nothing passes before the window opens.
  uint8_t sync[10]={0x35}; uint8_t write10[10]={0x2a,0,uint8_t(first>>24),uint8_t(first>>16),uint8_t(first>>8),uint8_t(first),0,0,1,0};
  assert(writeRefused(write10,10,1,4096) && writeRefused(sync,10,0,0));
  assert(ufs_test_write(0,first,1,block.data())==kIOReturnBadArgument && ufs_test_sync()==kIOReturnNotReady);
  // The Data store's window never opens, with the log window closed or open.
  assert(!ufs_test_store_open(100,4));
  assert(ufs_test_log_open(first,blocks));
  assert(!ufs_test_store_open(100,4) && !ufs_test_store_open(first,blocks));
  assert(!ufs_test_log_open(first,blocks));                                                      // once
  assert(lun.empty() && writes==0);
  // Inside: the first and last block, the largest command, and the cache flush.
  assert(ufs_test_write(0,first,1,block.data())==0 && ufs_test_write(0,end-1,1,block.data())==0);
  assert(ufs_test_write(0,first+100,uint32_t(kDataMax/4096),many.data())==0);
  assert(lun.count(first) && lun.count(end-1) && lun.count(first+131) && !lun.count(first+132) && writes==3);
  assert(ufs_test_sync()==0 && synced==1);
  // Outside, by one block on either side, across each edge, and on another LUN: nothing reaches the device.
  const unsigned before=unsigned(opcodes.size());
  assert(ufs_test_write(0,first-1,1,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,end,1,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first-1,2,many.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,end-1,2,many.data())==kIOReturnBadArgument);
  assert(ufs_test_write(1,first,1,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first,0,block.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first,uint32_t(kDataMax/4096)+1,many.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,first,1,nullptr)==kIOReturnBadArgument);
  assert(ufs_test_write(0,UINT64_MAX,2,many.data())==kIOReturnBadArgument);
  assert(ufs_test_write(0,0,1,block.data())==kIOReturnBadArgument);                               // the GPT
  assert(opcodes.size()==before && !lun.count(first-1) && !lun.count(end) && !lun.count(0));
  // The same writes straight at hostScsiDir, past hostWrite4k's check: its own gate reads the CDB.
  uint8_t outside[10]={0x2a,0,uint8_t(end>>24),uint8_t(end>>16),uint8_t(end>>8),uint8_t(end),0,0,1,0};
  assert(writeRefused(outside,10,1,4096));
  const uint64_t edge=end-1;
  uint8_t across[10]={0x2a,0,uint8_t(edge>>24),uint8_t(edge>>16),uint8_t(edge>>8),uint8_t(edge),0,0,2,0};
  assert(writeRefused(across,10,1,8192));
  uint8_t longer[10]; memcpy(longer,write10,10); longer[8]=2;
  assert(writeRefused(longer,10,1,4096));                                                         // the CDB and the data disagree
  uint8_t none[10]; memcpy(none,write10,10); none[8]=0;
  assert(writeRefused(none,10,1,4096));
  uint32_t scsi=0;
  assert(hostScsiDir(1,write10,10,1,4096,200000,&scsi)==kIOReturnBadArgument);                    // only LUN 0
  assert(hostScsiDir(1,sync,10,0,0,200000,&scsi)==kIOReturnBadArgument);
  // Flag bits, including FUA: the writer needs none.
  for (const unsigned at: {1U,6U,9U}) { uint8_t flag[10]; memcpy(flag,write10,10); flag[at]=0x08; assert(writeRefused(flag,10,1,4096)); }
  for (unsigned at=1;at<10;++at) { uint8_t flag[10]={0x35}; flag[at]=1; assert(writeRefused(flag,10,0,0)); }
  // Every other command with data going out, and the commands that change the medium without data.
  for (unsigned opcode=0;opcode<256;++opcode) {
    uint8_t cdb[16]={uint8_t(opcode)};
    memcpy(cdb+2,write10+2,8);
    if (opcode!=0x2a) { assert(writeRefused(cdb,10,1,4096)); assert(writeRefused(cdb,16,1,4096)); assert(writeRefused(cdb,6,1,4096)); }
  }
  for (const uint8_t opcode: {uint8_t(0x0a),uint8_t(0x2e),uint8_t(0x41),uint8_t(0x42),uint8_t(0x55),uint8_t(0x5a),uint8_t(0x91),uint8_t(0x93),
                              uint8_t(0x8a),uint8_t(0x8e),uint8_t(0xaa),uint8_t(0xae),uint8_t(0x35)}) {
    uint8_t cdb[16]={opcode};
    if (opcode!=0x35) assert(writeRefused(cdb,10,0,0));
    assert(writeRefused(cdb,16,0,0));                                                             // SYNCHRONIZE CACHE(16) too
    if (opcode==0x35) assert(writeRefused(cdb,6,0,0));
  }
  uint8_t ssu[6]={0x1b,1,0,0,0,0}; assert(writeRefused(ssu,6,0,0));                               // only the wake START STOP UNIT
  // Reads keep working around all this, and READ(10) with data going out is still no write.
  auto out=filled(0); assert(ufs_test_read(0,first,1,out.data())==0 && out[0]==0x5a);
  uint8_t read10[10]={0x28,0,0,0,0,1,0,0,1,0}; assert(writeRefused(read10,10,1,4096));
  // A close ends it: the same write that passed above is refused now.
  ufs_test_log_close();
  assert(ufs_test_write(0,first,1,block.data())==kIOReturnBadArgument && writeRefused(write10,10,1,4096));
  assert(ufs_test_sync()==kIOReturnNotReady && writeRefused(sync,10,0,0));
  // The panic path's writer: the held functions go through the same gate.
  reset(); assert(ufs_test_log_open(first,blocks));
  assert(hostWrite4kHeld(first,1,block.data(),false,200000)==0 && hostSyncCacheHeld(200000)==0 && synced==1);
  const unsigned count=unsigned(opcodes.size());
  assert(hostWrite4kHeld(end,1,block.data(),false,200000)==kIOReturnBadArgument);
  assert(hostWrite4kHeld(first,1,block.data(),true,200000)==kIOReturnBadArgument);                 // FUA stays refused
  assert(opcodes.size()==count);
  // A write that never completes quarantines the transport, within its timeout, and the panic path's short one is honoured.
  hang=true; const uint64_t started=now;
  assert(hostWrite4kHeld(first,1,block.data(),false,5000)==kIOReturnTimeout);
  assert(now-started<8000 && ufs_test_poisoned());
  assert(hostSyncCacheHeld(5000)!=0);
}
#else
// A read-only image without the switch: neither window opens and no write reaches the device.
static void gates()
{
  constexpr uint64_t first=kExportFirst;
  auto block=filled(0x5a);
  reset();
  assert(!ufs_test_log_open(first,1000) && !ufs_test_store_open(first,1000));
  assert(ufs_test_write(0,first,1,block.data())==kIOReturnBadArgument && ufs_test_sync()==kIOReturnNotReady);
  uint8_t write10[10]={0x2a,0,uint8_t(first>>24),uint8_t(first>>16),uint8_t(first>>8),uint8_t(first),0,0,1,0}, sync[10]={0x35};
  assert(writeRefused(write10,10,1,4096) && writeRefused(sync,10,0,0));
  assert(opcodes.empty() && lun.empty());
}
#endif

#if !defined(NATIVE_UFS_READ_ONLY) || !NATIVE_UFS_READ_ONLY || (defined(NATIVE_UFS_LOG_WRITES) && NATIVE_UFS_LOG_WRITES)
static void quarantine() { gHost.poisoned=true; gHost.ready=false; }

// A command that times out while a write waits for the lock: the data area may still be the controller's, so the write stops short of it.
static void quarantines()
{
  auto block=filled(0x5a);
  reset(); assert(ufs_test_log_open(kExportFirst,1000));
  memset(memory+kDataOffset,0x11,4096);
  gLockHook=quarantine;
  assert(ufs_test_write(0,kExportFirst,1,block.data())==kIOReturnNotReady);
  assert(memory[kDataOffset]==0x11 && memory[kDataOffset+4095]==0x11 && opcodes.empty() && writes==0);
}

// A read may wait behind the device's cache only while the log is writing: with the window open it gets a write's time, as a timeout ends
// the root volume; without it a second, as ever.
static void slowReads()
{
  uint8_t out[4096];
  reset(); slow=999000; assert(ufs_test_read(0,1,1,out)==0);
  reset(); slow=1100000; assert(ufs_test_read(0,1,1,out)==kIOReturnTimeout && gHost.poisoned);
  reset(); assert(ufs_test_log_open(kExportFirst,1000));
  slow=5000000; assert(ufs_test_read(0,1,1,out)==0 && !gHost.poisoned);
  reset(); assert(ufs_test_log_open(kExportFirst,1000));
  slow=11000000; assert(ufs_test_read(0,1,1,out)==kIOReturnTimeout && gHost.poisoned);
}
#endif

int main()
{
  plans(); contents(); opens(); claims(); gates();
#if !defined(NATIVE_UFS_READ_ONLY) || !NATIVE_UFS_READ_ONLY || (defined(NATIVE_UFS_LOG_WRITES) && NATIVE_UFS_LOG_WRITES)
  quarantines(); slowReads();
#endif
  puts("PASS: log window plan, content gate, panic claim and the write gates on the real command path");
}
