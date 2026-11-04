// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileCache.h"

#include "../platform/Platform.h"
#include "../tiles/TileMath.h"

namespace maps {

TileCache::~TileCache() {
    for (int i = 0; i < count; i++) {
        largeFree(slots[i].pixels);
    }
    largeFree(slots);
}

int TileCache::init(int maxSlots) {
    slots = static_cast<Slot*>(largeAlloc(sizeof(Slot) * maxSlots));
    if (slots == nullptr) {
        return 0;
    }
    count = 0;
    for (int i = 0; i < maxSlots; i++) {
        auto* pixels = static_cast<uint16_t*>(largeAlloc(TILE_BYTES));
        if (pixels == nullptr) {
            break;
        }
        slots[count++] = { { -1, 0, 0 }, 0, false, false, false, 0, pixels };
    }
    return count;
}

const uint16_t* TileCache::find(const TileKey& tile) {
    for (int i = 0; i < count; i++) {
        if (slots[i].valid && slots[i].key == tile) {
            slots[i].lastUse = ++useCounter;
            return slots[i].pixels;
        }
    }
    return nullptr;
}

int TileCache::sourceZoomOf(const TileKey& tile) const {
    for (int i = 0; i < count; i++) {
        if (slots[i].valid && slots[i].key == tile) {
            return slots[i].sourceZoom;
        }
    }
    return -1;
}

const uint16_t* TileCache::peek(const TileKey& tile) const {
    for (int i = 0; i < count; i++) {
        if (slots[i].valid && slots[i].key == tile) {
            return slots[i].pixels;
        }
    }
    return nullptr;
}

bool TileCache::contains(const TileKey& tile) const {
    for (int i = 0; i < count; i++) {
        if (slots[i].valid && !slots[i].stale && slots[i].key == tile) {
            return true;
        }
    }
    return false;
}

uint16_t* TileCache::reserve(const TileKey& tile) {
    if (count == 0) {
        return nullptr;
    }
    int victim = -1;
    for (int i = 0; i < count; i++) {
        if (slots[i].reserved || slots[i].pixels == nullptr) {
            continue;
        }
        if (!slots[i].valid) {
            victim = i;
            break;
        }
        if (victim < 0 || slots[i].lastUse < slots[victim].lastUse) {
            victim = i;
        }
    }
    if (victim < 0) {
        return nullptr;
    }
    slots[victim].valid = false;
    slots[victim].reserved = true;
    slots[victim].key = tile;
    return slots[victim].pixels;
}

void TileCache::commit(const TileKey& tile, int sourceZoom) {
    int committed = -1;
    for (int i = 0; i < count && committed < 0; i++) {
        if (slots[i].reserved && slots[i].key == tile) {
            slots[i].valid = true;
            slots[i].stale = false;
            slots[i].sourceZoom = static_cast<int8_t>(sourceZoom);
            slots[i].reserved = false;
            slots[i].lastUse = ++useCounter;
            committed = i;
        }
    }
    // The stale copy it replaces goes
    for (int i = 0; i < count && committed >= 0; i++) {
        if (i != committed && slots[i].valid && slots[i].key == tile) {
            slots[i].valid = false;
        }
    }
}

void TileCache::release(const TileKey& tile) {
    for (int i = 0; i < count; i++) {
        if (slots[i].reserved && slots[i].key == tile) {
            slots[i].reserved = false;
            return;
        }
    }
}

void TileCache::dropMagnified() {
    for (int i = 0; i < count; i++) {
        const int best = slots[i].key.z < MAX_SOURCE_ZOOM ? slots[i].key.z : MAX_SOURCE_ZOOM;
        if (slots[i].valid && slots[i].sourceZoom < best) {
            slots[i].stale = true;
        }
    }
}

void TileCache::dropMagnified(const TileKey* arrived, int count) {
    for (int i = 0; i < count; i++) {
        const TileKey& tile = arrived[i];
        for (int j = 0; j < this->count; j++) {
            Slot& slot = slots[j];
            if (slot.valid && tile.z >= slot.sourceZoom && tile.z <= slot.key.z && ancestorOf(slot.key, tile.z) == tile) {
                slot.stale = true;
            }
        }
    }
}

bool TileCache::freeSlot() {
    int victim = -1;
    for (int i = 0; i < count; i++) {
        if (slots[i].reserved || slots[i].pixels == nullptr) {
            continue;
        }
        if (!slots[i].valid) {
            victim = i;
            break;
        }
        if (victim < 0 || slots[i].lastUse < slots[victim].lastUse) {
            victim = i;
        }
    }
    if (victim < 0) {
        return false;
    }
    largeFree(slots[victim].pixels);
    slots[victim].pixels = nullptr;
    slots[victim].valid = false;
    freed++;
    return true;
}

int TileCache::regrow() {
    for (int i = 0; i < count && freed > 0; i++) {
        if (slots[i].pixels != nullptr) {
            continue;
        }
        auto* pixels = static_cast<uint16_t*>(largeAlloc(TILE_BYTES));
        if (pixels == nullptr) {
            break;
        }
        slots[i] = { { -1, 0, 0 }, 0, false, false, false, 0, pixels };
        freed--;
    }
    return freed;
}

void TileCache::clear() {
    for (int i = 0; i < count; i++) {
        if (!slots[i].reserved) {
            slots[i].valid = false;
        }
    }
}

} // namespace maps
