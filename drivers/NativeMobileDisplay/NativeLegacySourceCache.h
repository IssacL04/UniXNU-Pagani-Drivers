// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// Kernel mappings of the IOSurfaces CoreAnimation rotates through, kept across swaps instead of mapping and
// unmapping a whole surface on every frame. Each entry holds a reference on its descriptor, so a cached pointer
// cannot be reused by another surface. A surface missing from the last Idle swaps has left the rotation (a replaced
// swap chain, a restarted client) and is released at the next lookup; drop() releases everything (display off,
// stop, free). Up to Count surfaces stay mapped while a lit display gets no swaps. Not thread safe: the display
// calls it under its work-loop gate.
// Ops: Map *map(Memory *) (nullptr on failure), uint64_t length(Map *), void retain(Memory *),
// void release(Memory *, Map *) (either may be nullptr).
template <class Memory, class Map, unsigned Count = 4, uint64_t Idle = 16>
struct NativeLegacySourceCache {
    struct Entry {Memory *memory; Map *map; uint64_t used;};
    Entry entries[Count] = {};
    uint64_t clock = 0;

    template <class Ops> Map *lookup(Ops &ops, Memory *memory, uint64_t allocation) {
        const uint64_t now = ++clock;
        for (auto &entry : entries)
            if (entry.map && entry.memory != memory && now - entry.used > Idle) clear(ops, entry);
        for (auto &entry : entries) {
            if (entry.memory != memory || !entry.map) continue;
            if (ops.length(entry.map) >= allocation) {entry.used = now; return entry.map;}
            clear(ops, entry);                      // the surface grew: map it again
        }
        Entry *slot = nullptr;
        for (auto &entry : entries) if (!entry.map) {slot = &entry; break;}
        if (!slot) {
            slot = &entries[0];
            for (auto &entry : entries) if (entry.used < slot->used) slot = &entry;
            clear(ops, *slot);
        }
        Map *map = ops.map(memory);
        if (!map) return nullptr;
        if (ops.length(map) < allocation) {ops.release(nullptr, map); return nullptr;}
        ops.retain(memory);
        *slot = {memory, map, now};
        return map;
    }
    template <class Ops> void drop(Ops &ops) {for (auto &entry : entries) if (entry.map) clear(ops, entry);}

private:
    template <class Ops> static void clear(Ops &ops, Entry &entry) {ops.release(entry.memory, entry.map); entry = {};}
};
