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
        largeFree(entries[i].bytes);
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
    largeFree(entries[index].bytes);
    entries[index] = entries[--entryCount];
}

void LabelStore::put(const TileKey& source, const uint8_t* section, size_t size, int count) {
    if (mutex == nullptr || size == 0 || size > MAX_BYTES / 4) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int existing = find(source);
    if (existing >= 0 && entries[existing].size == size) {
        // The same tile drawn again, for another zoom level or after the raster cache dropped it
        entries[existing].lastUse = ++useCounter;
        xSemaphoreGive(mutex);
        return;
    }
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
    auto* copy = static_cast<uint8_t*>(largeAlloc(size));
    if (copy != nullptr) {
        memcpy(copy, section, size);
        entries[entryCount++] = { source, copy, size, count, ++useCounter };
        bytesUsed += size;
        changes++;
    }
    xSemaphoreGive(mutex);
}

} // namespace maps
