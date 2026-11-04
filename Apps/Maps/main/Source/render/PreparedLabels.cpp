// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreparedLabels.h"

#include "PreparedTile.h"

namespace maps {

namespace {

bool readVarint(const uint8_t*& data, const uint8_t* end, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && data < end; shift += 7) {
        const uint8_t byte = *data++;
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

int32_t unzigzag(uint64_t value) {
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}

constexpr uint64_t MAX_U16 = 0xFFFF;
constexpr uint64_t MAX_RANK = 0x7FFFFFFF;
constexpr uint64_t MAX_POINTS_VALUE = 0x7FFFFFFF;

} // namespace

void LabelReader::begin(const uint8_t* bytes, size_t size, int count) {
    data = bytes;
    length = size;
    offset = 0;
    remaining = count;
}

bool LabelReader::next(LabelRecord& out) {
    if (remaining <= 0) {
        return false;
    }
    const uint8_t* cursor = data + offset;
    const uint8_t* end = data + length;
    if (cursor >= end) {
        remaining = 0;
        return false;
    }
    const uint8_t kind = *cursor++;
    if (kind > static_cast<uint8_t>(LabelKind::Peak) || cursor >= end) {
        remaining = 0;
        return false;
    }
    const auto labelClass = static_cast<Value>(*cursor++);
    uint64_t rank;
    uint64_t nameLength;
    if (!readVarint(cursor, end, rank) || rank > MAX_RANK + 1 || !readVarint(cursor, end, nameLength) || nameLength > MAX_U16 ||
        nameLength > static_cast<uint64_t>(end - cursor)) {
        remaining = 0;
        return false;
    }
    const char* name = reinterpret_cast<const char*>(cursor);
    cursor += nameLength;
    uint64_t latinLength;
    if (!readVarint(cursor, end, latinLength) || latinLength > MAX_U16 ||
        latinLength > static_cast<uint64_t>(end - cursor)) {
        remaining = 0;
        return false;
    }
    const char* nameLatin = latinLength > 0 ? reinterpret_cast<const char*>(cursor) : nullptr;
    cursor += latinLength;
    uint64_t pointCount;
    if (!readVarint(cursor, end, pointCount) || pointCount > MAX_POINTS_VALUE) {
        remaining = 0;
        return false;
    }
    int stored = 0;
    int32_t x = 0;
    int32_t y = 0;
    for (uint64_t i = 0; i < pointCount; i++) {
        uint64_t dx;
        uint64_t dy;
        if (!readVarint(cursor, end, dx) || !readVarint(cursor, end, dy)) {
            remaining = 0;
            return false;
        }
        x += unzigzag(dx);
        y += unzigzag(dy);
        if (stored < MAX_POINTS) {
            pointBuffer[stored * 2] = x;
            pointBuffer[stored * 2 + 1] = y;
            stored++;
        }
    }
    offset = static_cast<size_t>(cursor - data);
    remaining--;
    out.kind = static_cast<LabelKind>(kind);
    out.labelClass = labelClass;
    out.rank = rank == 0 ? -1 : static_cast<int32_t>(rank - 1);
    out.name = name;
    out.nameLength = static_cast<uint16_t>(nameLength);
    out.nameLatin = nameLatin;
    out.nameLatinLength = static_cast<uint16_t>(latinLength);
    out.points = pointBuffer;
    out.pointCount = stored;
    return true;
}

bool findLabelSection(const uint8_t* body, size_t bodySize, const uint8_t*& section, size_t& sectionSize, int& count) {
    const uint8_t* cursor = body;
    const uint8_t* end = body + bodySize;
    while (cursor < end && *cursor != 0xFF) {
        const uint8_t id = *cursor++;
        uint64_t labelCount;
        uint64_t length;
        if (!readVarint(cursor, end, labelCount) || !readVarint(cursor, end, length) ||
            length > static_cast<uint64_t>(end - cursor)) {
            return false;
        }
        if (id == PREPARED_LABEL_SECTION) {
            if (labelCount > MAX_POINTS_VALUE) {
                return false;
            }
            section = cursor;
            sectionSize = static_cast<size_t>(length);
            count = static_cast<int>(labelCount);
            return true;
        }
        cursor += length;
    }
    return false;
}

} // namespace maps
