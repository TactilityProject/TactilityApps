// SPDX-License-Identifier: GPL-3.0-or-later
#include "ChunkedBody.h"

#include "../platform/Platform.h"

#include <cstring>

namespace maps {

ChunkedBody::~ChunkedBody() {
    release();
}

void ChunkedBody::release() {
    for (int i = 0; i < allocated; i++) {
        largeFree(chunks[i]);
        chunks[i] = nullptr;
    }
    allocated = 0;
    clear();
}

void ChunkedBody::trim() {
    while (allocated > KEEP_CHUNKS) {
        allocated--;
        largeFree(chunks[allocated]);
        chunks[allocated] = nullptr;
    }
    clear();
}

bool ChunkedBody::ensure(size_t total) {
    const size_t needed = (total + CHUNK_BYTES - 1) / CHUNK_BYTES;
    if (needed > MAX_CHUNKS) {
        return false;
    }
    while (static_cast<size_t>(allocated) < needed) {
        auto* piece = static_cast<uint8_t*>(largeAlloc(CHUNK_BYTES));
        if (piece == nullptr) {
            return false;
        }
        chunks[allocated++] = piece;
    }
    return true;
}

void ChunkedBody::putBytes(const void* bytes, size_t count) {
    if (error) {
        return;
    }
    if (!ensure(length + count)) {
        error = true;
        return;
    }
    const auto* source = static_cast<const uint8_t*>(bytes);
    while (count > 0) {
        const size_t offset = length % CHUNK_BYTES;
        const size_t part = count < CHUNK_BYTES - offset ? count : CHUNK_BYTES - offset;
        memcpy(chunks[length / CHUNK_BYTES] + offset, source, part);
        source += part;
        length += part;
        count -= part;
    }
}

size_t ChunkedBody::chunkSize(int index) const {
    const size_t start = static_cast<size_t>(index) * CHUNK_BYTES;
    if (start >= length) {
        return 0;
    }
    return length - start < CHUNK_BYTES ? length - start : CHUNK_BYTES;
}

} // namespace maps
