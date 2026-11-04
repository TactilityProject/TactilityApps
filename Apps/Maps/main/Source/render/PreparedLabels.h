// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/Schema.h"

#include <cstddef>
#include <cstdint>

namespace maps {

enum class LabelKind : uint8_t { Place, Road, Water, Waterway, Peak };

struct LabelRecord {
    LabelKind kind;
    Value labelClass;
    /** -1 when absent */
    int32_t rank;
    const char* name;
    uint16_t nameLength;
    /** nullptr when the record has none */
    const char* nameLatin;
    uint16_t nameLatinLength;
    /** x, y pairs in the 2048 extent, valid until the next call of next() */
    const int32_t* points;
    int pointCount;
};

/** Walks the labels of a label section (its bytes after the section header). */
class LabelReader {
public:
    static constexpr int MAX_POINTS = 256;
    void begin(const uint8_t* bytes, size_t size, int count);
    /** False at the end or on damaged bytes. Lines with more points than MAX_POINTS are cut to MAX_POINTS. */
    bool next(LabelRecord& out);

private:
    const uint8_t* data = nullptr;
    size_t length = 0;
    size_t offset = 0;
    int remaining = 0;
    int32_t pointBuffer[2 * MAX_POINTS] {};
};

/** Finds the label section in an inflated prepared body. False when there is none (also for v2 tiles). */
bool findLabelSection(const uint8_t* body, size_t bodySize, const uint8_t*& section, size_t& sectionSize, int& count);

} // namespace maps
