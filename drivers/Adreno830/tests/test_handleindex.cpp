/* SPDX-License-Identifier: MIT */
/* NativeAdrenoHandleIndex.h: the handle -> slot index the VM and the user client look handles up in, and the M10 handle slots
 * (ARCHITECTURE 6.5 step 2): first in, first out, a freed slot quarantined for 4096 further allocations, the handle carrying
 * slot + 1 in its low 32 bits and the slot's generation in its high 32 bits.
 * host-test: NativeAdrenoHandleIndex.h */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoHandleIndex.h"
#include <cstdio>
#include <set>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main()
{
    unsigned checks=0;
    /* The index: insert, find, erase with backward shift over colliding runs. */
    {
        NativeAdrenoHandleIndex<1024> index;
        CHECK(index.find(5)==UINT32_MAX && index.allocate());
        for(uint64_t key=1;key<=1024;++key)index.insert(key*7919,uint32_t(key));
        for(uint64_t key=1;key<=1024;++key)CHECK(index.find(key*7919)==uint32_t(key));
        for(uint64_t key=1;key<=1024;key+=2)index.erase(key*7919);
        for(uint64_t key=1;key<=1024;++key)CHECK(index.find(key*7919)==(key&1?UINT32_MAX:uint32_t(key)));
        CHECK(index.find(0)==UINT32_MAX);
        index.release();
        checks+=3;
    }
    /* A runtime size below the bound (a small Metal client's connection): power of two up to Slots, the full key load, a reallocation
     * to another size starts empty, refusals leave nothing allocated. */
    {
        NativeAdrenoHandleIndex<16384> index;
        CHECK(!index.allocate(0) && !index.allocate(3000) && !index.allocate(32768) && index.capacity()==0);
        CHECK(index.allocate(4096) && index.capacity()==4096);
        for(uint64_t key=1;key<=4096;++key)index.insert(key<<20|key,uint32_t(key));
        for(uint64_t key=1;key<=4096;++key)CHECK(index.find(key<<20|key)==uint32_t(key));
        for(uint64_t key=2;key<=4096;key+=2)index.erase(key<<20|key);
        for(uint64_t key=1;key<=4096;++key)CHECK(index.find(key<<20|key)==(key&1?uint32_t(key):UINT32_MAX));
        CHECK(index.allocate(256) && index.capacity()==256 && index.find(1<<20|1)==UINT32_MAX);
        for(uint64_t key=1;key<=256;++key)index.insert(key*0x9e3779b9ull,uint32_t(key));
        for(uint64_t key=1;key<=256;++key)CHECK(index.find(key*0x9e3779b9ull)==uint32_t(key));
        CHECK(index.allocate() && index.capacity()==16384 && index.find(0x9e3779b9ull)==UINT32_MAX);
        index.release();
        CHECK(index.capacity()==0 && index.find(1)==UINT32_MAX);
        checks+=4;
    }
    /* The slots: never-used slots first in order; a freed slot comes back only after 4096 further allocations; handles. */
    {
        NativeAdrenoHandleSlots<16384> slots;
        CHECK(slots.take()==UINT32_MAX && slots.allocate() && slots.freeSlots()==16384);
        std::vector<uint32_t> taken;
        for(uint32_t i=0;i<5000;++i) {const uint32_t s=slots.take();CHECK(s==i);taken.push_back(s);}
        const uint64_t handle=slots.handleFor(7);
        CHECK(uint32_t(handle)==8 && (handle>>32)==0 && slots.current(handle) && NativeAdrenoHandleSlots<16384>::slotOf(handle)==7);
        CHECK(slots.give(7) && !slots.current(handle) && (slots.handleFor(7)>>32)==1);
        /* 16384 - 5000 never-used slots go first; slot 7 only once 4096 allocations passed since it was freed. */
        uint64_t sinceFree=0;
        for(;;) {
            const uint32_t s=slots.take();
            CHECK(s!=UINT32_MAX);
            ++sinceFree;
            if(s==7)break;
            CHECK(s>=5000);
        }
        CHECK(sinceFree==16384-5000+1 && sinceFree>4096);
        CHECK(slots.take()==UINT32_MAX && !slots.freeSlots());   /* everything taken */
        /* Freed slots come back first in, first out, and not before their quarantine. */
        CHECK(slots.give(100) && slots.give(200) && slots.take()==UINT32_MAX && slots.freeSlots()==2);
        slots.release();
        checks+=5;
    }
    /* Quarantine with a free supply: a churn of allocate/free never hands out a slot freed fewer than 4096 allocations ago. */
    {
        NativeAdrenoHandleSlots<8192> slots;
        CHECK(slots.allocate());
        std::vector<uint64_t> freedAt(8192,0);
        std::vector<bool> live(8192,false);
        std::vector<uint32_t> held;
        uint64_t allocations=0;
        for(unsigned round=0;round<70000;++round) {
            if(held.size()<64) {
                const uint32_t s=slots.take();
                if(s==UINT32_MAX)continue;
                CHECK(!live[s] && (!freedAt[s] || allocations-freedAt[s]>=4096));
                live[s]=true;held.push_back(s);++allocations;
            } else {
                const uint32_t s=held.front();held.erase(held.begin());
                CHECK(slots.give(s));live[s]=false;freedAt[s]=allocations;
            }
        }
        CHECK(allocations>30000 && !slots.give(8192));
        CHECK(NativeAdrenoHandleSlots<8192>::slotOf(0)==UINT32_MAX && NativeAdrenoHandleSlots<8192>::slotOf(8193)==UINT32_MAX);
        slots.release();
        checks+=2;
    }
    CHECK(fixture.metadata.empty());
    std::printf("PASS: %u handle index and FIFO-quarantined handle slot checks (slot + 1 | generation << 32, 4096-allocation "
        "quarantine)\n",checks);
}
