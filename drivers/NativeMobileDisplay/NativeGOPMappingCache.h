// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU client's mappings of the compositor's surfaces (ARCHITECTURE 5.3.2 item 3, M9): NativeGOPPresenter::waitSwap mapped each
// offered frame's pages into the client task and dropped the mapping at completion, so the client pinned and imported 12.8 MB on every
// frame. The compositor rotates through a few surfaces; their mappings are kept here, up to Count, least recently used first out, each
// holding a reference on its descriptor (a cached pointer cannot come back as another surface). The id handed out with a mapping
// (NGPSwap.reserved) is the slot plus a generation, so it is never reused for another mapping and the client may key its GPU imports by
// it. The cache belongs to one client task: a frame for another task drops everything first; drop() also runs when the client goes away.
// Not thread safe: NativeGOPPresenter serializes it with its own lock.
// Ops: Map *map(Task *, Memory *) (nullptr on failure), void retain(Memory *), void release(Memory *, Map *) (either may be nullptr),
// void retainTask(Task *), void releaseTask(Task *).
#pragma once
#include <stdint.h>

template <class Task, class Memory, class Map, unsigned Count = 6>
struct NativeGOPMappingCache {
    static_assert(Count>=1 && Count<=7,"the slot is kept in the low three bits of the id");
    struct Entry {Memory *memory; const void *surface; Map *map; uint64_t used; uint32_t id;};
    Entry entries[Count] = {};
    Task *owner = nullptr;
    uint64_t clock = 0;
    uint32_t generation = 0;
    uint64_t hits = 0, misses = 0, evictions = 0, failures = 0;

    // The mapping of `memory` (the frame of `surface`, which may be null) in `task`: *map and its nonzero id, or 0 when mapping failed.
    template <class Ops> uint32_t lookup(Ops &ops, Task *task, const void *surface, Memory *memory, Map **map) {
        *map = nullptr;
        if (!task || !memory) return 0;
        if (task != owner) {
            drop(ops);
            ops.retainTask(task);
            owner = task;
        }
        const uint64_t now = ++clock;
        for (auto &entry : entries)
            if (entry.map && entry.memory == memory && entry.surface == surface) {
                entry.used = now; ++hits;
                *map = entry.map;
                return entry.id;
            }
        unsigned slot = Count;
        for (unsigned i = 0; i < Count && slot == Count; ++i) if (!entries[i].map) slot = i;
        if (slot == Count) {
            slot = 0;
            for (unsigned i = 1; i < Count; ++i) if (entries[i].used < entries[slot].used) slot = i;
            clear(ops, entries[slot]); ++evictions;
        }
        Map *created = ops.map(task, memory);
        if (!created) {++failures; return 0;}
        ops.retain(memory);
        if (!++generation || generation >= (1u << 29)) generation = 1;
        entries[slot] = {memory, surface, created, now, (generation << 3) | (slot + 1)};
        ++misses;
        *map = created;
        return entries[slot].id;
    }
    template <class Ops> void drop(Ops &ops) {
        for (auto &entry : entries) if (entry.map) clear(ops, entry);
        if (owner) {ops.releaseTask(owner); owner = nullptr;}
    }
    unsigned live() const {unsigned n = 0; for (auto &entry : entries) n += entry.map != nullptr; return n;}

private:
    template <class Ops> static void clear(Ops &ops, Entry &entry) {ops.release(entry.memory, entry.map); entry = {};}
};
