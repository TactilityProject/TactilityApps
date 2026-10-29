// SPDX-License-Identifier: GPL-3.0-or-later
#include "ByteInput.h"

#include "../platform/Platform.h"

namespace maps {

FileBuffer::~FileBuffer() {
    largeFree(data);
}

bool FileBuffer::reserve(size_t size) {
    if (size <= capacity) {
        return true;
    }
    largeFree(data);
    length = 0;
    capacity = (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
    data = static_cast<uint8_t*>(largeAlloc(capacity));
    if (data == nullptr) {
        capacity = 0;
        return false;
    }
    return true;
}

bool FileBuffer::load(const char* path) {
    length = 0;
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long fileSize = ok ? ftell(file) : -1;
    ok = ok && fileSize > 0 && fseek(file, 0, SEEK_SET) == 0;
    ok = ok && reserve(static_cast<size_t>(fileSize));
    if (ok) {
        length = fread(data, 1, static_cast<size_t>(fileSize), file);
        ok = length == static_cast<size_t>(fileSize);
    }
    fclose(file);
    if (!ok) {
        length = 0;
    }
    return ok;
}

} // namespace maps
