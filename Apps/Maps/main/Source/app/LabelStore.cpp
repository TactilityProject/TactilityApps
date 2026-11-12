// SPDX-License-Identifier: GPL-3.0-or-later
#include "LabelStore.h"

#include "../platform/Platform.h"

#include <cstring>

namespace maps {

bool LabelStore::begin() {
    mutex = xSemaphoreCreateMutex();
    return mutex != nullptr;
}

void LabelStore::end() {
    for (int i = 0; i < entryCount; i++) {
        largeFree(entries[i].cells != nullptr ? static_cast<void*>(entries[i].cells) : entries[i].bytes);
    }
    entryCount = 0;
    bytesUsed = 0;
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
}

int LabelStore::find(const TileKey& tile) const {
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].tile == tile) {
            return i;
        }
    }
    return -1;
}

void LabelStore::evict(int index) {
    bytesUsed -= entries[index].size;
    largeFree(entries[index].cells != nullptr ? static_cast<void*>(entries[index].cells) : entries[index].bytes);
    entries[index] = entries[--entryCount];
}

void LabelStore::put(const TileKey& source, const uint8_t* section, size_t size, int count, bool numbers, int region) {
    if (mutex == nullptr || size == 0) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int existing = find(source);
    if (existing >= 0 && entries[existing].size == size && entries[existing].numbers == numbers) {
        // The same tile drawn again, for another zoom level or after the raster cache dropped it
        entries[existing].lastUse = ++useCounter;
        xSemaphoreGive(mutex);
        return;
    }
    xSemaphoreGive(mutex);
    if (size > MAX_SECTION_BYTES) {
        logError("Labels of %d/%d/%d not kept: %u bytes", source.z, source.x, source.y, static_cast<unsigned>(size));
        return;
    }
    auto* copy = static_cast<uint8_t*>(largeAlloc(size));
    if (copy == nullptr) {
        return;
    }
    memcpy(copy, section, size);
    xSemaphoreTake(mutex, portMAX_DELAY);
    store(source, copy, nullptr, size, count, numbers, region);
    xSemaphoreGive(mutex);
}

void LabelStore::adopt(const TileKey& source, uint8_t* block, size_t size, int count, bool numbers, int region) {
    if (mutex == nullptr || block == nullptr || size > MAX_SECTION_BYTES) {
        if (size > MAX_SECTION_BYTES) {
            logError("Labels of %d/%d/%d not kept: %u bytes", source.z, source.x, source.y, static_cast<unsigned>(size));
        }
        largeFree(block);
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    store(source, block + sizeof(LabelCells), reinterpret_cast<LabelCells*>(block), size, count, numbers, region);
    xSemaphoreGive(mutex);
}

void LabelStore::store(const TileKey& source, uint8_t* copy, LabelCells* cells, size_t size, int count, bool numbers, int region) {
    const int existing = find(source);
    if (existing >= 0) {
        evict(existing);
    }
    while (entryCount > 0 && (entryCount >= MAX_ENTRIES || bytesUsed + size > MAX_BYTES)) {
        int oldest = 0;
        for (int i = 1; i < entryCount; i++) {
            if (entries[i].lastUse < entries[oldest].lastUse) {
                oldest = i;
            }
        }
        evict(oldest);
    }
    entries[entryCount++] = { source, copy, size, count, ++useCounter, numbers, region, cells };
    bytesUsed += size;
    changes++;
}

bool LabelStore::isReading(const TileKey& tile) const {
    for (int i = 0; i < readingCount; i++) {
        if (reading[i].tile == tile) {
            return true;
        }
    }
    return false;
}

void LabelStore::queue(const TileKey& tile, int region) {
    const bool numbers = numbersWanted.load();
    // A lane reads it already: asked again afterwards only if the house numbers were left out
    for (int i = 0; i < readingCount; i++) {
        if (reading[i].tile == tile && (reading[i].numbers || !numbers)) {
            return;
        }
    }
    for (int i = 0; i < requestCount; i++) {
        if (requests[i].tile == tile) {
            requests[i].numbers = requests[i].numbers || numbers;
            return;
        }
    }
    if (requestCount == MAX_REQUESTS) {
        // The oldest request goes: its tile is probably off screen by now
        for (int i = 1; i < requestCount; i++) {
            requests[i - 1] = requests[i];
        }
        requestCount--;
    }
    requests[requestCount++] = { tile, region, numbers };
}

void LabelStore::request(const TileKey& source, int region) {
    if (mutex == nullptr) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int index = find(source);
    if (index < 0 || (numbersWanted && !entries[index].numbers)) {
        queue(source, region);
    } else {
        entries[index].lastUse = ++useCounter;
    }
    xSemaphoreGive(mutex);
}

bool LabelStore::takeRequest(Request& out) {
    if (mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool found = false;
    while (requestCount > 0 && !found && readingCount < MAX_READING) {
        // The newest first: the tiles drawn last are the ones on screen now
        out = requests[--requestCount];
        const int index = find(out.tile);
        found = !isReading(out.tile) && (index < 0 || (out.numbers && !entries[index].numbers));
    }
    if (found) {
        reading[readingCount++] = out;
    }
    xSemaphoreGive(mutex);
    return found;
}

void LabelStore::done(const TileKey& source) {
    if (mutex == nullptr) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < readingCount; i++) {
        if (reading[i].tile == source) {
            reading[i] = reading[--readingCount];
            break;
        }
    }
    xSemaphoreGive(mutex);
}

} // namespace maps
