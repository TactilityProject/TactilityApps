// SPDX-License-Identifier: GPL-3.0-or-later
#include "CacheIndex.h"

#include "../platform/Platform.h"
#include "PreparedTile.h"

#include <cstring>

namespace maps {

namespace {

constexpr char MAGIC[4] = { 'T', 'M', 'C', 'I' };
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t ENTRY_BYTES = 17;

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

} // namespace

CacheIndex::~CacheIndex() {
    largeFree(entries);
}

bool CacheIndex::init(int maxEntries, uint64_t capBytes) {
    largeFree(entries);
    entries = static_cast<Entry*>(largeAlloc(sizeof(Entry) * static_cast<size_t>(maxEntries)));
    capacity = entries != nullptr ? maxEntries : 0;
    cap = capBytes;
    clear();
    return entries != nullptr;
}

void CacheIndex::clear() {
    count = 0;
    total = 0;
    clock = 0;
    evicting = false;
    complete = false;
    dirty = false;
}

int CacheIndex::find(const TileKey& tile) const {
    for (int i = 0; i < count; i++) {
        if (entries[i].tile == tile) {
            return i;
        }
    }
    return -1;
}

bool CacheIndex::contains(const TileKey& tile) const {
    return find(tile) >= 0;
}

void CacheIndex::put(const TileKey& tile, uint32_t size) {
    const int index = find(tile);
    if (index >= 0) {
        total = total - entries[index].size + size;
        entries[index].size = size;
        entries[index].lastUse = ++clock;
    } else if (count < capacity) {
        entries[count++] = { tile, size, ++clock };
        total += size;
    } else {
        // The index is full: the tile is on the card but unknown, the next evictions make room
        return;
    }
    dirty = true;
}

void CacheIndex::touch(const TileKey& tile) {
    const int index = find(tile);
    if (index >= 0) {
        entries[index].lastUse = ++clock;
        dirty = true;
    }
}

void CacheIndex::erase(const TileKey& tile) {
    const int index = find(tile);
    if (index < 0) {
        return;
    }
    total -= entries[index].size;
    entries[index] = entries[--count];
    dirty = true;
}

bool CacheIndex::nextEviction(TileKey& tile) {
    const uint64_t lowWater = cap * LOW_WATER_PERCENT / 100;
    const int lowEntries = capacity * LOW_WATER_PERCENT / 100;
    if (isOverCap()) {
        evicting = true;
    }
    if (evicting && total <= lowWater && count <= lowEntries) {
        evicting = false;
    }
    if (!evicting || count == 0) {
        return false;
    }
    int oldest = 0;
    for (int i = 1; i < count; i++) {
        if (entries[i].lastUse < entries[oldest].lastUse) {
            oldest = i;
        }
    }
    tile = entries[oldest].tile;
    return true;
}

void CacheIndex::save(ByteWriter& out) const {
    out.clear();
    out.putBytes(MAGIC, 4);
    out.putU32(FORMAT_VERSION);
    out.putU32(static_cast<uint32_t>(count));
    out.putU32(clock);
    for (int i = 0; i < count; i++) {
        const Entry& entry = entries[i];
        out.put(static_cast<uint8_t>(entry.tile.z));
        out.putU32(static_cast<uint32_t>(entry.tile.x));
        out.putU32(static_cast<uint32_t>(entry.tile.y));
        out.putU32(entry.size);
        out.putU32(entry.lastUse);
    }
}

bool CacheIndex::load(const uint8_t* data, size_t size) {
    clear();
    if (data == nullptr || size < 16 || memcmp(data, MAGIC, 4) != 0 || readU32(data + 4) != FORMAT_VERSION) {
        return false;
    }
    const uint32_t stored = readU32(data + 8);
    if (size != 16 + static_cast<size_t>(stored) * ENTRY_BYTES) {
        return false;
    }
    clock = readU32(data + 12);
    const uint8_t* cursor = data + 16;
    for (uint32_t i = 0; i < stored && count < capacity; i++, cursor += ENTRY_BYTES) {
        const TileKey tile { cursor[0], static_cast<int>(readU32(cursor + 1)), static_cast<int>(readU32(cursor + 5)) };
        const uint32_t fileSize = readU32(cursor + 9);
        const uint32_t lastUse = readU32(cursor + 13);
        if (!isValidTile(tile)) {
            continue;
        }
        entries[count++] = { tile, fileSize, lastUse };
        total += fileSize;
        clock = lastUse > clock ? lastUse : clock;
    }
    complete = true;
    return true;
}

} // namespace maps
