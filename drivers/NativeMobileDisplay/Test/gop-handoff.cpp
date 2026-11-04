// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/gop-handoff.cpp drivers/NativeMobileDisplay/NativeGOPMappingCache.h drivers/NativeMobileDisplay/NativeGOPWire.h
// Host test of the GPU hand-off's kernel side that runs without IOKit (ARCHITECTURE 5.3.2 and 8.5, M9; M12): the wire records (version 1
// unchanged byte for byte: an older plugin calling only methods 0-3 sees exactly the old layout; BAND and WAIT_SWAP2 are additions), and
// the presenter's cache of frame mappings in the client task (hits for a rotating swap chain, LRU of six, ids never reused, a new client
// task dropping everything, failed mappings, balanced references on descriptors and the task).
#include "../NativeGOPWire.h"
#include "../NativeGOPMappingCache.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

struct Task {int references;};
struct Memory {int references;};
struct Map {Memory *memory; Task *task; bool live; uint64_t address;};
struct Ops {
    Map maps[512] = {};
    unsigned created = 0;
    bool fail = false;
    Map *map(Task *task, Memory *memory) {
        if (fail) return nullptr;
        assert(created < 512);
        maps[created] = {memory, task, true, 0x100000000ull + created * 0x1000000ull};
        return &maps[created++];
    }
    void retain(Memory *memory) {memory->references++;}
    void release(Memory *memory, Map *map) {
        if (map) {assert(map->live); map->live = false;}
        if (memory) {assert(memory->references > 0); memory->references--;}
    }
    void retainTask(Task *task) {task->references++;}
    void releaseTask(Task *task) {assert(task->references > 0); task->references--;}
    unsigned live() const {unsigned n = 0; for (unsigned i = 0; i < created; i++) n += maps[i].live; return n;}
};
using Cache = NativeGOPMappingCache<Task, Memory, Map>;

int main() {
    unsigned checks = 0;
    {   // Wire: the version-1 records keep their sizes and field offsets; the additions sit after them.
        static_assert(NGP_VERSION == 1 && NGP_BGRX8888 == 1, "version 1");
        static_assert(NGP_METHOD_MODE == 0 && NGP_METHOD_PRESENT == 1 && NGP_METHOD_WAIT_SWAP == 2 && NGP_METHOD_COMPLETE_SWAP == 3 &&
                      NGP_METHOD_BAND == 4 && NGP_METHOD_WAIT_SWAP2 == 5 && NGP_MEMORY_SCANOUT_RW == 1, "method numbers");
        static_assert(offsetof(NGPMode, width) == 8 && offsetof(NGPMode, bytes) == 32, "NGPMode");
        static_assert(offsetof(NGPSwap, address) == 8 && offsetof(NGPSwap, stride) == 24 && offsetof(NGPSwap, width) == 32 &&
                      offsetof(NGPSwap, reserved) == 44, "NGPSwap");
        static_assert(offsetof(NGPComplete, status) == 8 && offsetof(NGPPresent, stride) == 24, "NGPComplete/NGPPresent");
        static_assert(offsetof(NGPBand, y0) == 0 && offsetof(NGPBand, y1) == 4 && offsetof(NGPBand, rows) == 8, "NGPBand");
        static_assert(offsetof(NGPSwap2, swap) == 0 && offsetof(NGPSwap2, target_offset) == 48, "NGPSwap2");
        // A WAIT_SWAP2 answer starts with exactly the WAIT_SWAP answer.
        NGPSwap2 two = {{7, 0x1000, 0x2000, 4864, 1216, 2640, NGP_BGRX8888, 9}, 0xc3f000};
        NGPSwap one = two.swap;
        assert(one.sequence == 7 && one.reserved == 9 && two.target_offset == 0xc3f000);
        checks++;
    }
    {   // A triple-buffered chain maps each surface once; ids are stable for a surface and distinct between surfaces.
        Ops ops; Cache cache; Task task = {0}; Memory m[3] = {};
        int surfaces[3] = {};
        Map *first[3] = {}; uint32_t ids[3] = {};
        for (unsigned frame = 0; frame < 300; frame++) {
            Map *map = nullptr;
            const uint32_t id = cache.lookup(ops, &task, &surfaces[frame % 3], &m[frame % 3], &map);
            assert(id && map && map->live && map->memory == &m[frame % 3] && map->task == &task);
            if (frame < 3) {first[frame] = map; ids[frame] = id;}
            else assert(map == first[frame % 3] && id == ids[frame % 3]);
        }
        assert(ids[0] != ids[1] && ids[1] != ids[2] && ops.created == 3 && cache.hits == 297 && cache.misses == 3);
        assert(task.references == 1);
        for (auto &memory : m) assert(memory.references == 1);
        // The low three bits are the slot plus one; the generation above them.
        for (auto id : ids) assert((id & 7) >= 1 && (id & 7) <= 6 && (id >> 3));
        cache.drop(ops);
        assert(task.references == 0 && ops.live() == 0 && !cache.owner);
        for (auto &memory : m) assert(memory.references == 0);
        checks += 2;
    }
    {   // Seven surfaces through six entries: the least recently used goes; an id is never handed out again for another mapping.
        Ops ops; Cache cache; Task task = {0}; Memory m[7] = {};
        uint32_t seen[64] = {}; unsigned n = 0;
        for (unsigned frame = 0; frame < 28; frame++) {
            Map *map = nullptr;
            const uint32_t id = cache.lookup(ops, &task, nullptr, &m[frame % 7], &map);
            assert(id && map);
            if (map == &ops.maps[ops.created - 1] && cache.misses == ops.created) {
                for (unsigned i = 0; i < n; i++) assert(seen[i] != id);
                seen[n++] = id;
            }
        }
        assert(cache.live() == 6 && ops.live() == 6 && cache.evictions == ops.created - 6);
        unsigned held = 0;
        for (auto &memory : m) {assert(memory.references <= 1); held += memory.references;}
        assert(held == 6);
        cache.drop(ops);
        checks++;
    }
    {   // Same descriptor, another surface object: a separate mapping (the surface is part of the key).
        Ops ops; Cache cache; Task task = {0}; Memory memory = {0}; int a = 0, b = 0;
        Map *ma = nullptr, *mb = nullptr;
        const uint32_t ia = cache.lookup(ops, &task, &a, &memory, &ma), ib = cache.lookup(ops, &task, &b, &memory, &mb);
        assert(ia && ib && ia != ib && ma != mb && memory.references == 2);
        cache.drop(ops);
        assert(memory.references == 0);
        checks++;
    }
    {   // A new client task: everything of the old one is released first. Failed mappings keep nothing.
        Ops ops; Cache cache; Task one = {0}, two = {0}; Memory m[2] = {};
        Map *map = nullptr;
        assert(cache.lookup(ops, &one, nullptr, &m[0], &map) && cache.lookup(ops, &one, nullptr, &m[1], &map));
        assert(one.references == 1 && ops.live() == 2);
        assert(cache.lookup(ops, &two, nullptr, &m[0], &map) && map->task == &two);
        assert(one.references == 0 && two.references == 1 && ops.live() == 1 && m[1].references == 0 && m[0].references == 1);
        ops.fail = true;
        assert(!cache.lookup(ops, &two, nullptr, &m[1], &map) && !map && m[1].references == 0 && cache.failures == 1);
        assert(!cache.lookup(ops, nullptr, nullptr, &m[1], &map) && !cache.lookup(ops, &two, nullptr, nullptr, &map));
        ops.fail = false;
        cache.drop(ops);
        cache.drop(ops);                       // dropping an empty cache is harmless
        assert(two.references == 0 && ops.live() == 0 && m[0].references == 0);
        checks++;
    }
    {   // The generation wraps without ever producing 0 or an id of the slot's earlier mappings in a short window.
        Ops ops; Cache cache; Task task = {0}; Memory m[2] = {};
        cache.generation = (1u << 29) - 2;
        Map *map = nullptr;
        const uint32_t a = cache.lookup(ops, &task, nullptr, &m[0], &map);
        const uint32_t b = cache.lookup(ops, &task, nullptr, &m[1], &map);
        assert(a && b && a != b && (b >> 3) == 1);
        cache.drop(ops);
        checks++;
    }
    printf("PASS: GPU hand-off wire records and frame mapping cache, %u checks\n", checks);
    return 0;
}
