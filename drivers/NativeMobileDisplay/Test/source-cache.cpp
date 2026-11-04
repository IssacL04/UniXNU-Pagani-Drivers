// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of NativeLegacySourceCache: hits for a rotating swap chain, aging of replaced surfaces, LRU when more
// surfaces rotate than it holds, remapping a grown surface, failed or short mappings, and balanced references.
#include "../NativeLegacySourceCache.h"
#include <assert.h>
#include <stdio.h>

struct Memory {int references; uint64_t length;};
struct Map {Memory *memory; uint64_t length; bool live;};

struct Ops {
    Map maps[256] = {};
    unsigned created = 0, released = 0;
    bool fail = false, shortMap = false;
    Map *map(Memory *memory) {
        if (fail) return nullptr;
        assert(created < 256);
        maps[created] = {memory, shortMap ? memory->length / 2 : memory->length, true};
        return &maps[created++];
    }
    uint64_t length(Map *map) {assert(map->live); return map->length;}
    void retain(Memory *memory) {memory->references++;}
    void release(Memory *memory, Map *map) {
        if (map) {assert(map->live); map->live = false; released++;}
        if (memory) {assert(memory->references > 0); memory->references--;}
    }
    unsigned live() const {unsigned n = 0; for (unsigned i = 0; i < created; i++) n += maps[i].live; return n;}
};

using Cache = NativeLegacySourceCache<Memory, Map>;

static Map *use(Cache &cache, Ops &ops, Memory &memory)
{
    Map *map = cache.lookup(ops, &memory, memory.length);
    assert(map && map->live && map->memory == &memory && map->length >= memory.length);
    return map;
}

int main(void)
{
    unsigned checks = 0;
    {   // A triple-buffered chain maps each surface once, then only hits.
        Ops ops; Cache cache; Memory a[3] = {{0, 4096}, {0, 4096}, {0, 4096}};
        for (unsigned frame = 0; frame < 1000; frame++) use(cache, ops, a[frame % 3]);
        assert(ops.created == 3 && ops.released == 0 && ops.live() == 3);
        for (auto &m : a) assert(m.references == 1);
        // The chain is replaced: the old surfaces are released within Idle swaps, the new ones stay.
        Memory b[3] = {{0, 4096}, {0, 4096}, {0, 4096}};
        for (unsigned frame = 0; frame < 17; frame++) use(cache, ops, b[frame % 3]);
        for (auto &m : a) assert(m.references == 0);
        for (auto &m : b) assert(m.references == 1);
        assert(ops.created == 6 && ops.live() == 3);
        cache.drop(ops);
        for (auto &m : b) assert(m.references == 0);
        assert(ops.live() == 0 && ops.created == ops.released);
        checks += 3;
    }
    {   // A surface shown again after a long still screen is a hit: aging spares the surface being looked up.
        Ops ops; Cache cache; Memory a{0, 4096}, b{0, 4096};
        Map *first = use(cache, ops, a);
        use(cache, ops, b);
        assert(use(cache, ops, a) == first && ops.created == 2);
        for (unsigned frame = 0; frame < 40; frame++) use(cache, ops, b);
        assert(a.references == 0 && ops.live() == 1);     // a aged out while only b was shown
        use(cache, ops, a);
        assert(ops.created == 3 && a.references == 1);
        cache.drop(ops);
        checks++;
    }
    {   // Five surfaces in rotation through four entries: least recently used goes, every lookup still maps the right one.
        Ops ops; Cache cache; Memory a[5] = {{0, 64}, {0, 64}, {0, 64}, {0, 64}, {0, 64}};
        for (unsigned frame = 0; frame < 50; frame++) use(cache, ops, a[frame % 5]);
        assert(ops.live() == 4 && ops.created == 50);     // LRU thrashes but stays correct and bounded
        unsigned held = 0;
        for (auto &m : a) {assert(m.references <= 1); held += m.references;}
        assert(held == 4);
        cache.drop(ops);
        for (auto &m : a) assert(m.references == 0);
        checks++;
    }
    {   // A grown surface is mapped again; failed and short mappings retain nothing.
        Ops ops; Cache cache; Memory a{0, 4096};
        Map *small = use(cache, ops, a);
        a.length = 8192;
        Map *large = use(cache, ops, a);
        assert(large != small && !small->live && a.references == 1 && ops.live() == 1);
        Memory b{0, 4096};
        ops.fail = true;
        assert(!cache.lookup(ops, &b, b.length) && b.references == 0);
        ops.fail = false; ops.shortMap = true;
        assert(!cache.lookup(ops, &b, b.length) && b.references == 0 && ops.live() == 1);
        ops.shortMap = false;
        use(cache, ops, b);
        cache.drop(ops);
        assert(a.references == 0 && b.references == 0 && ops.live() == 0 && ops.created == ops.released);
        cache.drop(ops);                                    // dropping an empty cache is harmless
        assert(ops.created == ops.released);
        checks++;
    }
    printf("PASS: source mapping cache, %u scenarios\n", checks);
    return 0;
}
