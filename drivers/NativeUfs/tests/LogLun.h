// SPDX-License-Identifier: BSD-2-Clause-Patent
// A model LUN behind the real command path, for tests/LogWindow.cpp and tests/LogInterface.cpp: it keeps every block written to it and every
// SCSI opcode that reached the doorbell, and takes `slow` microseconds over a command. Include after RootCommandsBridge.cpp.
#pragma once
#include <map>
#include <vector>
#include <array>
#include <string>
// The 13T's unixnu partition (512 GB PKX110; board.toml [userdata], UniXNU/out/dual_sys/applied2/plan.json), the window on it.
constexpr uint64_t kUnixnuFirst=107409152, kUnixnuLast=124186367, kExportFirst=124112640, kLogFirst=124178176;
static std::map<uint64_t,std::array<uint8_t,4096>> lun;      // what the model LUN holds (blank when absent)
static std::vector<uint8_t> opcodes;                         // every SCSI opcode the doorbell carried
static unsigned synced, writes;
static bool released, hang;
static uint64_t now, slow, doneAt;

static uint64_t io(unsigned op,uint64_t address,uint64_t value,void *)
{
  if (op==0) return address==0x34?1:address==0x30?15:address==0x58?now<doneAt:0;
  if (op==1 && address==0x58) {
    assert(released); released=false;
    if (hang) return 0;
    doneAt=now+slow;
    uint8_t *cmd=memory+kUcdOffset, *rsp=cmd+kRspOffset, *cdb=cmd+16, *data=memory+kDataOffset;
    assert(cmd[0]==1); opcodes.push_back(cdb[0]);
    const uint32_t lba=loadBe32(cdb+2), blocks=(uint32_t(cdb[7])<<8)|cdb[8];
    memset(rsp,0,512); memcpy(rsp,cmd,32); rsp[0]=0x21; rsp[1]=0; putBe32(rsp+12,0);
    if (cdb[0]==0x2a) {
      assert(cmd[1]==0x20 && cmd[2]==0 && loadBe32(cmd+12)==blocks*4096);
      assert(*reinterpret_cast<uint32_t *>(memory)>>25==0x8 + 1);   // UTP command, data out
      for (uint32_t i=0;i<blocks;++i) memcpy(lun[lba+i].data(),data+i*4096,4096);
      ++writes;
    } else if (cdb[0]==0x28) {
      for (uint32_t i=0;i<blocks;++i) {
        auto f=lun.find(lba+i);
        if (f==lun.end()) memset(data+i*4096,0,4096); else memcpy(data+i*4096,f->second.data(),4096);
      }
    } else if (cdb[0]==0x35) ++synced;
    *reinterpret_cast<uint32_t *>(memory+8)=0;
  }
  if (op==2) released=true;
  if (op==4) return now+=1;
  if (op==5) now+=value;
  return 0;
}

static void reset()
{
  lun.clear(); opcodes.clear(); synced=writes=0; released=hang=false; now=slow=doneAt=0;
  ufs_test_reset(io,0x8cb5e9000);
}

static std::array<uint8_t,4096> filled(uint8_t value) { std::array<uint8_t,4096> b; b.fill(value); return b; }
