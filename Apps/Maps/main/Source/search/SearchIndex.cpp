// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchIndex.h"

#include "QueryParse.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

void put16(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put24(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
}

void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get16(const uint8_t* in) {
    return static_cast<uint32_t>(in[0] | (in[1] << 8));
}

uint32_t get24(const uint8_t* in) {
    return static_cast<uint32_t>(in[0] | (in[1] << 8) | (in[2] << 16));
}

uint32_t get32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint32_t cutUtf8(const char* text, size_t length, uint32_t maximum) {
    if (length <= maximum) {
        return static_cast<uint32_t>(length);
    }
    uint32_t cut = maximum;
    // A continuation byte (10xxxxxx) at the cut means the character began earlier: leave it out whole
    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) {
        cut--;
    }
    return cut;
}

/** Compares two byte strings the way Python compares bytes: by content, then the shorter first. */
int compareBytes(const uint8_t* a, uint32_t aLength, const uint8_t* b, uint32_t bLength) {
    uint32_t common = aLength < bLength ? aLength : bLength;
    int result = common > 0 ? memcmp(a, b, common) : 0;
    if (result != 0) {
        return result;
    }
    return aLength < bLength ? -1 : (aLength > bLength ? 1 : 0);
}

void appendBytes(GrowArray<uint8_t>& array, const void* data, size_t size) {
    size_t at = array.size();
    if (size > 0 && array.resize(at + size)) {
        memcpy(array.begin() + at, data, size);
    }
}

} // namespace

uint32_t truncateKey(const char* key, size_t length) {
    return cutUtf8(key, length, SIDX_MAX_KEY);
}

uint32_t truncateName(const char* name, size_t length) {
    return cutUtf8(name, length, SIDX_MAX_NAME);
}

// ------------------------------------------------------------------------------------------------ encoders

void encodeHeader(const SearchHeader& header, uint8_t out[SIDX_HEADER_SIZE]) {
    memset(out, 0, SIDX_HEADER_SIZE);
    memcpy(out, "SIDX", 4);
    put16(out + 4, SIDX_VERSION);
    put16(out + 6, header.pointBytes);
    put32(out + 8, static_cast<uint32_t>(header.originX));
    put32(out + 12, static_cast<uint32_t>(header.originY));
    put32(out + 16, header.classCount);
    put32(out + 20, header.fenceCount);
    put32(out + 24, header.pageCount);
    put32(out + 28, header.pointCount);
    put32(out + 32, header.streetCount);
    put32(out + 36, header.addressCount);
    put32(out + 40, 0);
    for (uint32_t i = 0; i < SIDX_SECTION_COUNT; i++) {
        put32(out + 44 + 8 * i, header.sectionOffset[i]);
        put32(out + 48 + 8 * i, header.sectionSize[i]);
    }
}

uint32_t encodePt(uint8_t* out, uint32_t pointBytes, int32_t relX, int32_t relY) {
    if (pointBytes == 8) {
        put32(out, static_cast<uint32_t>(relX));
        put32(out + 4, static_cast<uint32_t>(relY));
    } else {
        put16(out, static_cast<uint32_t>(relX));
        put16(out + 2, static_cast<uint32_t>(relY));
    }
    return pointBytes;
}

bool encodeKeyPage(uint8_t* page, uint32_t& used, const uint8_t* key, uint32_t keyLength, uint32_t ngOffset) {
    uint32_t size = 1 + keyLength + 3;
    if (keyLength == 0 || used + size + 1 > SIDX_PAGE_SIZE) {
        return false;
    }
    page[used] = static_cast<uint8_t>(keyLength);
    memcpy(page + used + 1, key, keyLength);
    put24(page + used + 1 + keyLength, ngOffset);
    used += size;
    return true;
}

uint32_t encodeNameGroup(uint8_t* out, const char* name, uint32_t nameLength, SearchKind kind, uint32_t first, uint16_t count) {
    out[0] = static_cast<uint8_t>(nameLength);
    memcpy(out + 1, name, nameLength);
    out[1 + nameLength] = static_cast<uint8_t>(kind);
    put32(out + 2 + nameLength, first);
    put16(out + 6 + nameLength, count);
    return 1 + nameLength + 1 + 4 + 2;
}

uint32_t encodePoint(uint8_t* out, uint32_t pointBytes, int32_t relX, int32_t relY, uint8_t pair, uint32_t ngOffset) {
    encodePt(out, pointBytes, relX, relY);
    out[pointBytes] = pair;
    put24(out + pointBytes + 1, ngOffset);
    return pointBytes + 4;
}

uint32_t encodeStreet(uint8_t* out, uint32_t pointBytes, const int32_t rep[2], const int32_t boxMin[2], const int32_t boxMax[2],
                      uint32_t firstAddress, uint16_t addressCount) {
    encodePt(out, pointBytes, rep[0], rep[1]);
    encodePt(out + pointBytes, pointBytes, boxMin[0], boxMin[1]);
    encodePt(out + 2 * pointBytes, pointBytes, boxMax[0], boxMax[1]);
    put32(out + 3 * pointBytes, firstAddress);
    put16(out + 3 * pointBytes + 4, addressCount);
    return 3 * pointBytes + 6;
}

uint32_t encodeAddress(uint8_t* out, uint32_t pointBytes, uint16_t number, uint16_t aux, uint8_t suffix, uint8_t flags, int32_t relX,
                       int32_t relY) {
    put16(out, number);
    put16(out + 2, aux);
    out[4] = suffix;
    out[5] = flags;
    encodePt(out + 6, pointBytes, relX, relY);
    return 6 + pointBytes;
}

uint32_t encodeTextAddress(uint8_t* out, uint32_t pointBytes, uint32_t textOffset, int32_t relX, int32_t relY) {
    return encodeAddress(out, pointBytes, 0xFFFF, static_cast<uint16_t>(textOffset & 0xFFFF), static_cast<uint8_t>(textOffset >> 16),
                         HOUSE_TEXT, relX, relY);
}

// ------------------------------------------------------------------------------------------------ writer

namespace {

enum Category : uint8_t { CAT_CATEGORY = 0, CAT_PLACE = 1, CAT_STREET = 2, CAT_OTHER = 3, CAT_POI = 4 };

struct NameGroup {
    const char* name;
    uint32_t nameLength;
    const char* sortKey;
    uint32_t sortLength;
    uint8_t kind;
    uint8_t category;
    uint32_t pair;
    /** Order of creation, the tie-breaker of the sort (sidx.py sorts stably). */
    uint32_t creation;
    /** Point group: its first point (index into the input). Street: the street. Category: unused. */
    uint32_t source;
    uint32_t keysSource;
    uint32_t first;
    uint32_t count;
    uint32_t offset;
    /** Category: the run of pairs. */
    uint32_t pairFirst;
    uint32_t pairCount;
};

struct CategoryRun {
    uint32_t rawOffset;
    uint32_t rawLength;
    uint32_t firstPair;
    uint32_t lastPair;
    uint32_t count;
    bool contiguous;
};

struct PointRecord {
    uint32_t pair;
    uint32_t unnamed;
    uint32_t ng;
    int32_t tileY;
    int32_t tileX;
    int32_t x;
    int32_t y;
};

struct KeyEntry {
    uint32_t offset;
    uint32_t length;
    uint32_t ng;
    uint8_t priority;
};

struct AddressTemp {
    uint16_t number;
    uint16_t aux;
    uint8_t suffix;
    uint8_t flags;
    int32_t x, y;
    uint32_t index;
};

struct StreetTemp {
    const SearchClusterInput* cluster;
    uint32_t firstAddress;
    uint32_t count;
};

struct Failure {
    char* text;
    size_t size;
    bool fail(const char* what, uint32_t a = 0, uint32_t b = 0) {
        snprintf(text, size, what, a, b);
        return false;
    }
};

/** A very small hash set of overflow strings: offsets into the overflow bytes. */
class OverflowTable {
public:
    /** Returns the offset of the text, adding it when new. UINT32_MAX when out of memory. */
    uint32_t find(const char* text, size_t length) {
        if ((used + 1) * 2 > slots.size()) {
            if (!grow()) {
                return UINT32_MAX;
            }
        }
        uint32_t hash = hashOf(text, length);
        size_t mask = slots.size() - 1;
        for (size_t i = hash & mask;; i = (i + 1) & mask) {
            uint32_t slot = slots[i];
            if (slot == 0) {
                uint32_t offset = static_cast<uint32_t>(bytes.size());
                size_t at = bytes.size();
                if (!bytes.resize(at + length + 1)) {
                    return UINT32_MAX;
                }
                memcpy(bytes.begin() + at, text, length);
                slots[i] = offset + 1;
                used++;
                count++;
                return offset;
            }
            const char* stored = bytes.begin() + (slot - 1);
            if (strlen(stored) == length && memcmp(stored, text, length) == 0) {
                return slot - 1;
            }
        }
    }
    GrowArray<char> bytes;
    uint32_t count = 0;

private:
    static uint32_t hashOf(const char* text, size_t length) {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < length; i++) {
            hash = (hash ^ static_cast<uint8_t>(text[i])) * 16777619u;
        }
        return hash;
    }
    bool grow() {
        GrowArray<uint32_t> next;
        size_t size = slots.size() < 64 ? 64 : slots.size() * 2;
        if (!next.resize(size)) {
            return false;
        }
        size_t mask = size - 1;
        for (uint32_t slot : slots) {
            if (slot == 0) {
                continue;
            }
            const char* stored = bytes.begin() + (slot - 1);
            size_t i = hashOf(stored, strlen(stored)) & mask;
            while (next[i] != 0) {
                i = (i + 1) & mask;
            }
            next[i] = slot;
        }
        slots.swap(next);
        return true;
    }
    GrowArray<uint32_t> slots;
    size_t used = 0;
};

/** Folds a category name (an OSM class made of ASCII): lowercase, anything but letters and digits is a space, runs of spaces are one. */
uint32_t foldCategory(const char* name, char* out, size_t capacity) {
    uint32_t length = 0;
    bool space = false;
    for (const char* p = name; *p != 0; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        char value;
        if (c >= 'A' && c <= 'Z') {
            value = static_cast<char>(c + 32);
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
            value = static_cast<char>(c);
        } else {
            space = length > 0;
            continue;
        }
        if (space && length + 2 < capacity) {
            out[length++] = ' ';
        }
        space = false;
        if (length + 1 < capacity) {
            out[length++] = value;
        }
    }
    out[length] = 0;
    return length;
}

/** The number text as the index stores it. Text that the grammar does not cover, or would store wrongly, becomes overflow. */
bool parseNumber(const char* text, bool slashSecond, uint16_t& number, uint16_t& aux, uint8_t& suffix, uint8_t& flags) {
    size_t length = strlen(text);
    bool letter = false;
    uint32_t run = 0;
    for (size_t i = 0; i <= length; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= '0' && c <= '9') {
            run = run * 10 + (c - '0');
            if (run >= 0xFFFF) {
                return false;
            }
            continue;
        }
        run = 0;
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            letter = true;
        }
    }
    HouseNumber parsed;
    if (!parseHouseNumber(text, length, parsed)) {
        return false;
    }
    // A letter that parseHouseNumber could not turn into an ASCII letter would be lost: keep the text instead
    if (letter && parsed.suffix == 0) {
        return false;
    }
    number = parsed.number;
    aux = parsed.aux;
    suffix = static_cast<uint8_t>(parsed.suffix);
    flags = parsed.flags;
    if (slashSecond && flags == HOUSE_SLASH && aux != 0) {
        uint16_t first = number;
        number = aux;
        aux = first;
    }
    return true;
}

} // namespace

int compareSearchBytes(const uint8_t* a, uint32_t aLength, const uint8_t* b, uint32_t bLength) {
    return compareBytes(a, aLength, b, bLength);
}

uint32_t foldCategoryName(const char* name, char* out, size_t capacity) {
    return foldCategory(name, out, capacity);
}

bool indexHouseNumber(const char* text, bool slashSecond, uint16_t& number, uint16_t& aux, uint8_t& suffix, uint8_t& flags) {
    return parseNumber(text, slashSecond, number, aux, suffix, flags);
}

bool SearchIndexWriter::build(const SearchInput& in, ByteSink& sink, SearchBuildStats* stats) {
    errorText[0] = 0;
    Failure failure = { errorText, sizeof(errorText) };
    const SearchPointInput* points = in.points;
    const uint32_t pointCount = in.pointCount;
    const SearchStreetInput* streets = in.streets;
    const uint32_t streetCount = in.streetCount;

    // ---- origin and point size
    bool anyPoint = false;
    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    auto include = [&](int32_t x, int32_t y) {
        if (!anyPoint) {
            minX = maxX = x;
            minY = maxY = y;
            anyPoint = true;
            return;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    };
    for (uint32_t i = 0; i < pointCount; i++) {
        include(points[i].x, points[i].y);
    }
    for (uint32_t s = 0; s < streetCount; s++) {
        for (uint32_t c = 0; c < streets[s].clusterCount; c++) {
            const SearchClusterInput& cl = streets[s].clusters[c];
            include(cl.repX, cl.repY);
            include(cl.minX, cl.minY);
            include(cl.maxX, cl.maxY);
            for (uint32_t a = 0; a < cl.addressCount; a++) {
                include(cl.addresses[a].x, cl.addresses[a].y);
            }
        }
    }
    const int32_t originX = minX, originY = minY;
    const int64_t span = std::max<int64_t>(static_cast<int64_t>(maxX) - minX, static_cast<int64_t>(maxY) - minY);
    const bool wide = span >= 65536;
    const uint32_t pts = wide ? 8 : 4;

    // ---- class pairs: unique labels (sorted by text), folded into "<class>/(all)" when there are more than 255
    GrowArray<uint32_t> order;
    order.resize(pointCount);
    for (uint32_t i = 0; i < pointCount; i++) {
        if (points[i].label == nullptr) {
            return failure.fail("point %u has no label", i);
        }
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        int c = strcmp(points[a].label, points[b].label);
        return c != 0 ? c < 0 : a < b;
    });
    GrowArray<uint32_t> uniqueOf, uniqueFirst, uniqueCount;
    uniqueOf.resize(pointCount);
    for (uint32_t k = 0; k < pointCount; k++) {
        uint32_t i = order[k];
        if (k == 0 || strcmp(points[order[k - 1]].label, points[i].label) != 0) {
            uniqueFirst.push(i);
            uniqueCount.push(0);
        }
        uniqueOf[i] = static_cast<uint32_t>(uniqueFirst.size() - 1);
        uniqueCount[uniqueCount.size() - 1]++;
        if (i < uniqueFirst[uniqueFirst.size() - 1]) {
            uniqueFirst[uniqueFirst.size() - 1] = i;
        }
    }
    const uint32_t uniqueTotal = static_cast<uint32_t>(uniqueFirst.size());
    GrowArray<uint8_t> keep;
    keep.resize(uniqueTotal);
    uint32_t collapsed = 0;
    if (uniqueTotal > 255) {
        GrowArray<uint32_t> byCount;
        byCount.resize(uniqueTotal);
        for (uint32_t u = 0; u < uniqueTotal; u++) {
            byCount[u] = u;
        }
        std::sort(byCount.begin(), byCount.end(), [&](uint32_t a, uint32_t b) {
            return uniqueCount[a] != uniqueCount[b] ? uniqueCount[a] > uniqueCount[b] : uniqueFirst[a] < uniqueFirst[b];
        });
        for (uint32_t k = 0; k < 110; k++) {
            keep[byCount[k]] = 1;
        }
        collapsed = uniqueTotal - 110;
    } else {
        for (uint32_t u = 0; u < uniqueTotal; u++) {
            keep[u] = 1;
        }
    }
    GrowArray<char> labelBytes;
    GrowArray<uint32_t> labelOffset;
    labelOffset.resize(uniqueTotal);
    for (uint32_t u = 0; u < uniqueTotal; u++) {
        const char* label = points[uniqueFirst[u]].label;
        size_t length = strlen(label);
        size_t keepLength = length;
        const char* suffix = "";
        if (!keep[u]) {
            const char* slash = strchr(label, '/');
            const char* second = slash != nullptr ? strchr(slash + 1, '/') : nullptr;
            keepLength = second != nullptr ? static_cast<size_t>(second - label) : length;
            suffix = "/*";
        }
        size_t at = labelBytes.size();
        labelOffset[u] = static_cast<uint32_t>(at);
        labelBytes.resize(at + keepLength + strlen(suffix) + 1);
        memcpy(labelBytes.begin() + at, label, keepLength);
        memcpy(labelBytes.begin() + at + keepLength, suffix, strlen(suffix));
    }
    if (labelBytes.failed()) {
        return failure.fail("out of memory");
    }
    GrowArray<uint32_t> labelOrder;
    labelOrder.resize(uniqueTotal);
    for (uint32_t u = 0; u < uniqueTotal; u++) {
        labelOrder[u] = u;
    }
    const char* labelBase = labelBytes.begin();
    std::sort(labelOrder.begin(), labelOrder.end(), [&](uint32_t a, uint32_t b) {
        int c = strcmp(labelBase + labelOffset[a], labelBase + labelOffset[b]);
        return c != 0 ? c < 0 : a < b;
    });
    GrowArray<uint32_t> pairOfUnique, pairLabel;   // pairLabel: a unique label with that pair's text
    pairOfUnique.resize(uniqueTotal);
    for (uint32_t k = 0; k < uniqueTotal; k++) {
        uint32_t u = labelOrder[k];
        if (k == 0 || strcmp(labelBase + labelOffset[labelOrder[k - 1]], labelBase + labelOffset[u]) != 0) {
            pairLabel.push(u);
        }
        pairOfUnique[u] = static_cast<uint32_t>(pairLabel.size() - 1);
    }
    const uint32_t pairTotal = static_cast<uint32_t>(pairLabel.size());
    if (pairTotal > 255) {
        return failure.fail("%u class pairs do not fit a byte", pairTotal);
    }
    auto pairText = [&](uint32_t pair) { return labelBase + labelOffset[pairLabel[pair]]; };
    for (uint32_t pair = 0; pair < pairTotal; pair++) {
        if (strlen(pairText(pair)) > 255) {
            return failure.fail("class label %u is longer than 255 bytes", pair);
        }
    }
    GrowArray<uint8_t> pairOf;
    pairOf.resize(pointCount);
    for (uint32_t i = 0; i < pointCount; i++) {
        pairOf[i] = static_cast<uint8_t>(pairOfUnique[uniqueOf[i]]);
    }

    // ---- category runs: class and subclass names of "poi/<class>/<subclass>" pairs
    GrowArray<char> catChars;
    GrowArray<CategoryRun> classRuns, subRuns;
    uint32_t notContiguous = 0;
    auto addRun = [&](GrowArray<CategoryRun>& runs, const char* raw, size_t rawLength, uint32_t pair) {
        for (CategoryRun& run : runs) {
            if (run.rawLength == rawLength && memcmp(catChars.begin() + run.rawOffset, raw, rawLength) == 0) {
                if (pair != run.lastPair + 1) {
                    run.contiguous = false;
                }
                run.lastPair = pair;
                run.count++;
                return;
            }
        }
        CategoryRun run = {};
        run.rawOffset = static_cast<uint32_t>(catChars.size());
        run.rawLength = static_cast<uint32_t>(rawLength);
        run.firstPair = run.lastPair = pair;
        run.count = 1;
        run.contiguous = true;
        size_t at = catChars.size();
        catChars.resize(at + rawLength + 1);
        memcpy(catChars.begin() + at, raw, rawLength);
        runs.push(run);
    };
    for (uint32_t pair = 0; pair < pairTotal; pair++) {
        const char* label = pairText(pair);
        if (strncmp(label, "poi/", 4) != 0) {
            continue;
        }
        const char* classStart = label + 4;
        const char* classEnd = strchr(classStart, '/');
        size_t classLength = classEnd != nullptr ? static_cast<size_t>(classEnd - classStart) : strlen(classStart);
        addRun(classRuns, classStart, classLength, pair);
        if (classEnd != nullptr) {
            const char* subStart = classEnd + 1;
            const char* subEnd = strchr(subStart, '/');
            size_t subLength = subEnd != nullptr ? static_cast<size_t>(subEnd - subStart) : strlen(subStart);
            if (!(subLength == classLength && memcmp(subStart, classStart, subLength) == 0)) {
                addRun(subRuns, subStart, subLength, pair);
            }
        }
    }
    // display names ("pet_shop" -> "pet shop") and their sort keys, kept after the raw names
    struct CategoryText {
        uint32_t nameOffset, nameLength, sortOffset, sortLength;
        CategoryRun run;
    };
    GrowArray<CategoryText> categories;
    for (int which = 0; which < 2; which++) {
        for (const CategoryRun& run : which == 0 ? classRuns : subRuns) {
            if (!run.contiguous) {
                notContiguous++;
                continue;
            }
            CategoryText text = {};
            text.run = run;
            char name[128];
            uint32_t length = std::min<uint32_t>(run.rawLength, sizeof(name) - 1);
            memcpy(name, catChars.begin() + run.rawOffset, length);
            for (uint32_t i = 0; i < length; i++) {
                if (name[i] == '_') {
                    name[i] = ' ';
                }
            }
            name[length] = 0;
            char folded[128];
            uint32_t foldedLength = foldCategory(name, folded, sizeof(folded));
            for (uint32_t o = 0; o < in.categoryOverrides; o++) {
                if (strcmp(in.categoryNames[o], name) == 0) {
                    foldedLength = std::min<uint32_t>(static_cast<uint32_t>(strlen(in.categoryKeys[o])), sizeof(folded) - 1);
                    memcpy(folded, in.categoryKeys[o], foldedLength);
                    folded[foldedLength] = 0;
                    break;
                }
            }
            text.nameOffset = static_cast<uint32_t>(catChars.size());
            text.nameLength = length;
            catChars.resize(catChars.size() + length + 1);
            memcpy(catChars.begin() + text.nameOffset, name, length);
            text.sortOffset = static_cast<uint32_t>(catChars.size());
            text.sortLength = foldedLength;
            catChars.resize(catChars.size() + foldedLength + 1);
            memcpy(catChars.begin() + text.sortOffset, folded, foldedLength);
            categories.push(text);
        }
    }
    if (catChars.failed() || categories.failed()) {
        return failure.fail("out of memory");
    }

    // ---- name groups
    GrowArray<NameGroup> created;
    GrowArray<uint32_t> groupOfPoint;   // created index of a named point's group
    groupOfPoint.resize(pointCount);
    {
        GrowArray<uint32_t> named;
        for (uint32_t i = 0; i < pointCount; i++) {
            if (points[i].name != nullptr) {
                named.push(i);
            }
        }
        std::sort(named.begin(), named.end(), [&](uint32_t a, uint32_t b) {
            int c = strcmp(points[a].name, points[b].name);
            if (c != 0) {
                return c < 0;
            }
            return pairOf[a] != pairOf[b] ? pairOf[a] < pairOf[b] : a < b;
        });
        for (size_t k = 0; k < named.size(); k++) {
            uint32_t i = named[k];
            bool head = k == 0 || strcmp(points[named[k - 1]].name, points[i].name) != 0 || pairOf[named[k - 1]] != pairOf[i];
            if (head) {
                NameGroup group = {};
                group.name = points[i].name;
                group.nameLength = truncateName(group.name, strlen(group.name));
                group.sortKey = points[i].sortKey != nullptr ? points[i].sortKey : "";
                group.sortLength = static_cast<uint32_t>(strlen(group.sortKey));
                group.kind = static_cast<uint8_t>(SearchKind::Point);
                const char* label = points[i].label;
                group.category = strncmp(label, "poi/", 4) == 0 ? CAT_POI : (strncmp(label, "place/", 6) == 0 ? CAT_PLACE : CAT_OTHER);
                group.pair = pairOf[i];
                group.creation = i;
                group.source = i;
                created.push(group);
            }
            groupOfPoint[i] = static_cast<uint32_t>(created.size() - 1);
        }
    }
    for (uint32_t s = 0; s < streetCount; s++) {
        NameGroup group = {};
        group.name = streets[s].name;
        group.nameLength = truncateName(group.name, strlen(group.name));
        group.sortKey = streets[s].sortKey != nullptr ? streets[s].sortKey : "";
        group.sortLength = static_cast<uint32_t>(strlen(group.sortKey));
        group.kind = static_cast<uint8_t>(SearchKind::Street);
        group.category = CAT_STREET;
        group.creation = pointCount + s;
        group.source = s;
        created.push(group);
    }
    for (uint32_t c = 0; c < categories.size(); c++) {
        NameGroup group = {};
        group.name = catChars.begin() + categories[c].nameOffset;
        group.nameLength = truncateName(group.name, categories[c].nameLength);
        group.sortKey = catChars.begin() + categories[c].sortOffset;
        group.sortLength = categories[c].sortLength;
        group.kind = static_cast<uint8_t>(SearchKind::Category);
        group.category = CAT_CATEGORY;
        group.creation = pointCount + streetCount + c;
        group.source = c;
        group.pairFirst = categories[c].run.firstPair;
        group.pairCount = categories[c].run.count;
        created.push(group);
    }
    if (created.failed()) {
        return failure.fail("out of memory");
    }
    const uint32_t groupTotal = static_cast<uint32_t>(created.size());
    GrowArray<uint32_t> groupOrder;
    groupOrder.resize(groupTotal);
    for (uint32_t g = 0; g < groupTotal; g++) {
        groupOrder[g] = g;
    }
    std::sort(groupOrder.begin(), groupOrder.end(), [&](uint32_t a, uint32_t b) {
        const NameGroup& x = created[a];
        const NameGroup& y = created[b];
        if (x.kind != y.kind) {
            return x.kind < y.kind;
        }
        int c = compareBytes(reinterpret_cast<const uint8_t*>(x.sortKey), x.sortLength, reinterpret_cast<const uint8_t*>(y.sortKey),
                             y.sortLength);
        if (c != 0) {
            return c < 0;
        }
        if (x.pair != y.pair) {
            return x.pair < y.pair;
        }
        return x.creation < y.creation;
    });
    GrowArray<NameGroup> groups;
    GrowArray<uint32_t> newIndex;
    groups.resize(groupTotal);
    newIndex.resize(groupTotal);
    uint32_t groupBytes = 0;
    for (uint32_t k = 0; k < groupTotal; k++) {
        groups[k] = created[groupOrder[k]];
        newIndex[groupOrder[k]] = k;
        groups[k].offset = groupBytes;
        groupBytes += 1 + groups[k].nameLength + 1 + 4 + 2;
    }
    if (groupBytes >= SIDX_MAX_OFFSET24) {
        return failure.fail("the name groups take %u bytes, more than 16 MB", groupBytes);
    }

    // ---- point records, sorted by (pair, named first, name group, tile)
    GrowArray<PointRecord> records;
    records.resize(pointCount);
    for (uint32_t i = 0; i < pointCount; i++) {
        PointRecord& r = records[i];
        r.pair = pairOf[i];
        r.unnamed = points[i].name == nullptr ? 1 : 0;
        r.ng = r.unnamed ? 0 : newIndex[groupOfPoint[i]];
        r.tileY = points[i].y >> 12;
        r.tileX = points[i].x >> 12;
        r.x = points[i].x;
        r.y = points[i].y;
    }
    std::sort(records.begin(), records.end(), [](const PointRecord& a, const PointRecord& b) {
        if (a.pair != b.pair) return a.pair < b.pair;
        if (a.unnamed != b.unnamed) return a.unnamed < b.unnamed;
        if (a.ng != b.ng) return a.ng < b.ng;
        if (a.tileY != b.tileY) return a.tileY < b.tileY;
        if (a.tileX != b.tileX) return a.tileX < b.tileX;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    GrowArray<uint32_t> firstOf, countOf, pairStart, pairCount;
    firstOf.resize(groupTotal);
    countOf.resize(groupTotal);
    pairStart.resize(pairTotal);
    pairCount.resize(pairTotal);
    for (uint32_t i = 0; i < pointCount; i++) {
        const PointRecord& r = records[i];
        if (!r.unnamed) {
            if (countOf[r.ng] == 0) {
                firstOf[r.ng] = i;
            }
            countOf[r.ng]++;
        }
        if (pairCount[r.pair] == 0) {
            pairStart[r.pair] = i;
        }
        pairCount[r.pair]++;
    }

    // ---- street records and addresses, in name group order
    GrowArray<StreetTemp> streetRecords;
    GrowArray<uint8_t> addressBlob;
    OverflowTable overflow;
    uint32_t addressTotal = 0;
    for (uint32_t k = 0; k < groupTotal; k++) {
        NameGroup& g = groups[k];
        if (g.kind != static_cast<uint8_t>(SearchKind::Street)) {
            continue;
        }
        const SearchStreetInput& street = streets[g.source];
        if (street.clusterCount > 0xFFFF) {
            return failure.fail("a street has %u clusters, the limit is 65535", street.clusterCount);
        }
        g.first = static_cast<uint32_t>(streetRecords.size());
        g.count = street.clusterCount;
        for (uint32_t c = 0; c < street.clusterCount; c++) {
            const SearchClusterInput& cluster = street.clusters[c];
            if (cluster.addressCount > 0xFFFF) {
                return failure.fail("a street cluster has %u addresses, the limit is 65535", cluster.addressCount);
            }
            GrowArray<AddressTemp> temp;
            for (uint32_t a = 0; a < cluster.addressCount; a++) {
                const SearchAddressInput& in2 = cluster.addresses[a];
                AddressTemp t = {};
                t.x = in2.x - originX;
                t.y = in2.y - originY;
                t.index = a;
                if (!parseNumber(in2.number, in.slashSecond, t.number, t.aux, t.suffix, t.flags)) {
                    uint32_t at = overflow.find(in2.number, strlen(in2.number));
                    if (at == UINT32_MAX || at >= SIDX_MAX_OFFSET24) {
                        return failure.fail("the overflow strings pass 16 MB");
                    }
                    t.number = 0xFFFF;
                    t.aux = static_cast<uint16_t>(at & 0xFFFF);
                    t.suffix = static_cast<uint8_t>(at >> 16);
                    t.flags = HOUSE_TEXT;
                }
                t.flags = static_cast<uint8_t>(t.flags | in2.flags);
                temp.push(t);
            }
            std::sort(temp.begin(), temp.end(), [](const AddressTemp& a, const AddressTemp& b) {
                if (a.number != b.number) return a.number < b.number;
                if (a.aux != b.aux) return a.aux < b.aux;
                if (a.suffix != b.suffix) return a.suffix < b.suffix;
                return a.index < b.index;
            });
            StreetTemp rec = { &cluster, addressTotal, cluster.addressCount };
            streetRecords.push(rec);
            for (const AddressTemp& t : temp) {
                uint8_t bytes[16];
                uint32_t size = encodeAddress(bytes, pts, t.number, t.aux, t.suffix, t.flags, t.x, t.y);
                appendBytes(addressBlob, bytes, size);
            }
            addressTotal += cluster.addressCount;
        }
    }

    // ---- name group counts, then the name group area and the point and street records
    for (uint32_t k = 0; k < groupTotal; k++) {
        NameGroup& g = groups[k];
        if (g.kind == static_cast<uint8_t>(SearchKind::Point)) {
            g.first = firstOf[k];
            g.count = countOf[k];
        } else if (g.kind == static_cast<uint8_t>(SearchKind::Category)) {
            g.first = pairStart[g.pairFirst];
            g.count = 0;
            for (uint32_t p = 0; p < g.pairCount; p++) {
                g.count += pairCount[g.pairFirst + p];
            }
        }
        if (g.count > 0xFFFF) {
            return failure.fail("name group %u has more than 65535 records", k);
        }
    }
    GrowArray<uint8_t> groupBlob;
    for (uint32_t k = 0; k < groupTotal; k++) {
        uint8_t bytes[SIDX_MAX_NAME + 8];
        uint32_t size = encodeNameGroup(bytes, groups[k].name, groups[k].nameLength, static_cast<SearchKind>(groups[k].kind), groups[k].first,
                                        static_cast<uint16_t>(groups[k].count));
        appendBytes(groupBlob, bytes, size);
    }
    GrowArray<uint8_t> pointBlob;
    for (uint32_t i = 0; i < pointCount; i++) {
        const PointRecord& r = records[i];
        uint8_t bytes[16];
        uint32_t size = encodePoint(bytes, pts, r.x - originX, r.y - originY, static_cast<uint8_t>(r.pair),
                                    r.unnamed ? SIDX_UNNAMED : groups[r.ng].offset);
        appendBytes(pointBlob, bytes, size);
    }
    GrowArray<uint8_t> streetBlob;
    for (const StreetTemp& s : streetRecords) {
        const SearchClusterInput& c = *s.cluster;
        int32_t rep[2] = { c.repX - originX, c.repY - originY };
        int32_t lo[2] = { c.minX - originX, c.minY - originY };
        int32_t hi[2] = { c.maxX - originX, c.maxY - originY };
        uint8_t bytes[32];
        uint32_t size = encodeStreet(bytes, pts, rep, lo, hi, s.firstAddress, static_cast<uint16_t>(s.count));
        appendBytes(streetBlob, bytes, size);
    }

    // ---- keys, key pages and fence
    GrowArray<char> keyBytes;
    GrowArray<KeyEntry> entries;
    for (uint32_t k = 0; k < groupTotal; k++) {
        const NameGroup& g = groups[k];
        const char* const* keys = nullptr;
        uint32_t keyCount = 0;
        const char* categoryKey[1];
        if (g.kind == static_cast<uint8_t>(SearchKind::Point)) {
            keys = points[g.source].keys;
            keyCount = points[g.source].keyCount;
        } else if (g.kind == static_cast<uint8_t>(SearchKind::Street)) {
            keys = streets[g.source].keys;
            keyCount = streets[g.source].keyCount;
        } else {
            categoryKey[0] = g.sortKey;
            keys = categoryKey;
            keyCount = 1;
        }
        for (uint32_t i = 0; i < keyCount; i++) {
            size_t length = strlen(keys[i]);
            if (length == 0) {
                continue;
            }
            KeyEntry e = {};
            e.length = truncateKey(keys[i], length);
            e.offset = static_cast<uint32_t>(keyBytes.size());
            e.ng = g.offset;
            e.priority = g.category;
            size_t at = keyBytes.size();
            keyBytes.resize(at + e.length);
            memcpy(keyBytes.begin() + at, keys[i], e.length);
            entries.push(e);
        }
    }
    if (keyBytes.failed() || entries.failed()) {
        return failure.fail("out of memory");
    }
    const uint8_t* keyBase = reinterpret_cast<const uint8_t*>(keyBytes.begin());
    std::sort(entries.begin(), entries.end(), [&](const KeyEntry& a, const KeyEntry& b) {
        int c = compareBytes(keyBase + a.offset, a.length, keyBase + b.offset, b.length);
        if (c != 0) return c < 0;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.ng < b.ng;
    });
    GrowArray<uint8_t> keyBlob, fenceBlob;
    uint32_t pageCount = 0, fenceCount = 0;
    {
        uint8_t page[SIDX_PAGE_SIZE];
        uint32_t used = 0;
        memset(page, 0, sizeof(page));
        for (const KeyEntry& e : entries) {
            if (!encodeKeyPage(page, used, keyBase + e.offset, e.length, e.ng)) {
                appendBytes(keyBlob, page, SIDX_PAGE_SIZE);
                pageCount++;
                memset(page, 0, sizeof(page));
                used = 0;
                if (!encodeKeyPage(page, used, keyBase + e.offset, e.length, e.ng)) {
                    return failure.fail("a key does not fit a page");
                }
            }
            if (used == 1 + e.length + 3) {
                uint8_t length = static_cast<uint8_t>(e.length);
                appendBytes(fenceBlob, &length, 1);
                appendBytes(fenceBlob, keyBase + e.offset, e.length);
                fenceCount++;
            }
        }
        if (used > 0) {
            appendBytes(keyBlob, page, SIDX_PAGE_SIZE);
            pageCount++;
        }
    }
    GrowArray<uint8_t> classBlob;
    for (uint32_t pair = 0; pair < pairTotal; pair++) {
        const char* label = pairText(pair);
        uint8_t length = static_cast<uint8_t>(strlen(label));
        uint8_t tail[8];
        put32(tail, pairStart[pair]);
        put32(tail + 4, pairCount[pair]);
        appendBytes(classBlob, &length, 1);
        appendBytes(classBlob, label, length);
        appendBytes(classBlob, tail, 8);
    }
    if (pointBlob.failed() || streetBlob.failed() || addressBlob.failed() || groupBlob.failed() || keyBlob.failed() || fenceBlob.failed() ||
        classBlob.failed() || overflow.bytes.failed() || groups.failed()) {
        return failure.fail("out of memory");
    }

    // ---- assemble
    const GrowArray<uint8_t>* parts[SIDX_SECTION_COUNT] = { &classBlob, &fenceBlob, &keyBlob, &groupBlob, &pointBlob, &streetBlob, &addressBlob, nullptr };
    SearchHeader header = {};
    header.pointBytes = static_cast<uint16_t>(pts);
    header.originX = originX;
    header.originY = originY;
    header.classCount = pairTotal;
    header.fenceCount = fenceCount;
    header.pageCount = pageCount;
    header.pointCount = pointCount;
    header.streetCount = static_cast<uint32_t>(streetRecords.size());
    header.addressCount = addressTotal;
    uint64_t offset = SIDX_HEADER_SIZE;
    for (uint32_t i = 0; i < SIDX_SECTION_COUNT; i++) {
        uint64_t size = parts[i] != nullptr ? parts[i]->size() : overflow.bytes.size();
        header.sectionOffset[i] = static_cast<uint32_t>(offset);
        header.sectionSize[i] = static_cast<uint32_t>(size);
        offset += size;
    }
    if (offset > 0xFFFFFFFFull) {
        return failure.fail("the index is larger than 4 GB");
    }
    uint8_t headerBytes[SIDX_HEADER_SIZE];
    encodeHeader(header, headerBytes);
    bool ok = sink.append(headerBytes, SIDX_HEADER_SIZE);
    for (uint32_t i = 0; i < SIDX_SECTION_COUNT && ok; i++) {
        const void* data = parts[i] != nullptr ? static_cast<const void*>(parts[i]->begin()) : static_cast<const void*>(overflow.bytes.begin());
        if (header.sectionSize[i] > 0) {
            ok = sink.append(data, header.sectionSize[i]);
        }
    }
    if (!ok) {
        return failure.fail("the sink failed");
    }
    if (stats != nullptr) {
        *stats = {};
        stats->totalBytes = static_cast<uint32_t>(offset);
        stats->header = header;
        stats->keyCount = static_cast<uint32_t>(entries.size());
        stats->ngCount = groupTotal;
        for (uint32_t i = 0; i < pointCount; i++) {
            if (points[i].name != nullptr) {
                stats->namedPoints++;
            } else {
                stats->unnamedPoints++;
            }
        }
        stats->overflowStrings = overflow.count;
        stats->collapsedPairs = collapsed;
        stats->categoryNotContiguous = notContiguous;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ reader

uint32_t SearchIndexReader::readSection(SidxSection section, uint32_t offset, void* out, uint32_t size) {
    uint32_t index = static_cast<uint32_t>(section);
    uint32_t length = header.sectionSize[index];
    if (offset >= length || size == 0) {
        return 0;
    }
    if (size > length - offset) {
        size = length - offset;
    }
    callCount++;
    if (section == SidxSection::Ng && ngResident) {
        memcpy(out, residentNg.begin() + offset, size);
        return size;
    }
    if (section == SidxSection::Keys && keysResident) {
        memcpy(out, residentKeys.begin() + offset, size);
        return size;
    }
    uint32_t absolute = header.sectionOffset[index] + offset;
    touch(absolute, size);
    return file->read(absolute, out, size) ? size : 0;
}

void SearchIndexReader::touch(uint32_t offset, uint32_t size) {
    uint32_t last = (offset + (size > 0 ? size : 1) - 1) / SIDX_PAGE_SIZE;
    for (uint32_t b = offset / SIDX_PAGE_SIZE; b <= last && b < blockSeen.size(); b++) {
        blockSeen[b] = 1;
    }
}

void SearchIndexReader::resetIo() {
    callCount = 0;
    for (uint8_t& seen : blockSeen) {
        seen = 0;
    }
}

SearchIoStats SearchIndexReader::io() const {
    SearchIoStats stats = {};
    stats.calls = callCount;
    bool previous = false;
    for (uint8_t seen : blockSeen) {
        if (seen) {
            stats.blocks++;
            if (!previous) {
                stats.runs++;
            }
        }
        previous = seen != 0;
    }
    return stats;
}

bool SearchIndexReader::open(SearchFile* source) {
    file = source;
    uint8_t raw[SIDX_HEADER_SIZE];
    if (file == nullptr || !file->read(0, raw, SIDX_HEADER_SIZE)) {
        return false;
    }
    if (memcmp(raw, "SIDX", 4) != 0 || get16(raw + 4) != SIDX_VERSION) {
        return false;
    }
    header = {};
    header.pointBytes = static_cast<uint16_t>(get16(raw + 6));
    if (header.pointBytes != 4 && header.pointBytes != 8) {
        return false;
    }
    pointBytes = header.pointBytes;
    header.originX = static_cast<int32_t>(get32(raw + 8));
    header.originY = static_cast<int32_t>(get32(raw + 12));
    header.classCount = get32(raw + 16);
    header.fenceCount = get32(raw + 20);
    header.pageCount = get32(raw + 24);
    header.pointCount = get32(raw + 28);
    header.streetCount = get32(raw + 32);
    header.addressCount = get32(raw + 36);
    uint64_t end = SIDX_HEADER_SIZE;
    for (uint32_t i = 0; i < SIDX_SECTION_COUNT; i++) {
        header.sectionOffset[i] = get32(raw + 44 + 8 * i);
        header.sectionSize[i] = get32(raw + 48 + 8 * i);
        end = std::max<uint64_t>(end, static_cast<uint64_t>(header.sectionOffset[i]) + header.sectionSize[i]);
    }
    ngResident = keysResident = false;
    residentNg.release();
    residentKeys.release();
    blockSeen.release();
    blockSeen.resize(static_cast<size_t>(end / SIDX_PAGE_SIZE + 1));
    classBlob.release();
    classOffsets.release();
    fenceBlob.release();
    fenceOffsets.release();
    if (!classBlob.resize(header.sectionSize[0]) || !fenceBlob.resize(header.sectionSize[1])) {
        return false;
    }
    if (header.sectionSize[0] > 0 && !file->read(header.sectionOffset[0], classBlob.begin(), header.sectionSize[0])) {
        return false;
    }
    if (header.sectionSize[1] > 0 && !file->read(header.sectionOffset[1], fenceBlob.begin(), header.sectionSize[1])) {
        return false;
    }
    for (uint32_t at = 0, i = 0; i < header.classCount; i++) {
        if (at >= classBlob.size() || at + 1 + classBlob[at] + 8 > classBlob.size()) {
            return false;
        }
        classOffsets.push(at);
        at += 1 + classBlob[at] + 8;
    }
    for (uint32_t at = 0; at < fenceBlob.size();) {
        if (at + 1 + fenceBlob[at] > fenceBlob.size()) {
            return false;
        }
        fenceOffsets.push(at);
        at += 1 + fenceBlob[at];
    }
    if (classOffsets.failed() || fenceOffsets.failed() || fenceOffsets.size() != header.fenceCount) {
        return false;
    }
    resetIo();
    return true;
}

bool SearchIndexReader::loadResident(bool nameGroups, bool keyPages) {
    if (file == nullptr) {
        return false;
    }
    auto load = [&](SidxSection section, GrowArray<uint8_t>& into, bool& flag) {
        flag = false;
        into.release();
        uint32_t index = static_cast<uint32_t>(section);
        if (header.sectionSize[index] == 0) {
            return true;
        }
        if (!into.resize(header.sectionSize[index]) || !file->read(header.sectionOffset[index], into.begin(), header.sectionSize[index])) {
            into.release();
            return false;
        }
        flag = true;
        return true;
    };
    bool ok = true;
    if (nameGroups) {
        ok = load(SidxSection::Ng, residentNg, ngResident) && ok;
    } else {
        ngResident = false;
        residentNg.release();
    }
    if (keyPages) {
        ok = load(SidxSection::Keys, residentKeys, keysResident) && ok;
    } else {
        keysResident = false;
        residentKeys.release();
    }
    return ok;
}

bool SearchIndexReader::classPair(uint32_t index, char* label, size_t labelSize, uint32_t& first, uint32_t& count) const {
    if (index >= classOffsets.size()) {
        return false;
    }
    const uint8_t* entry = classBlob.begin() + classOffsets[index];
    uint32_t length = entry[0];
    if (labelSize > 0) {
        size_t copy = std::min<size_t>(length, labelSize - 1);
        memcpy(label, entry + 1, copy);
        label[copy] = 0;
    }
    first = get32(entry + 1 + length);
    count = get32(entry + 5 + length);
    return true;
}

bool SearchIndexReader::readPage(uint32_t page, uint8_t* out) {
    return readSection(SidxSection::Keys, page * SIDX_PAGE_SIZE, out, SIDX_PAGE_SIZE) == SIDX_PAGE_SIZE;
}

uint32_t SearchIndexReader::prefix(const char* key, uint32_t limit, uint32_t* ngOffsets) {
    const uint8_t* query = reinterpret_cast<const uint8_t*>(key);
    uint32_t queryLength = truncateKey(key, strlen(key));
    // bisect_left over the fence
    size_t lo = 0, hi = fenceOffsets.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint8_t* fence = fenceBlob.begin() + fenceOffsets[mid];
        if (compareBytes(fence + 1, fence[0], query, queryLength) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    uint32_t page = lo > 0 ? static_cast<uint32_t>(lo - 1) : 0;
    uint32_t found = 0;
    bool done = limit == 0;
    for (; page < header.pageCount && !done; page++) {
        if (!readPage(page, pageBuffer)) {
            break;
        }
        uint32_t i = 0;
        while (i < SIDX_PAGE_SIZE && pageBuffer[i] != 0) {
            uint32_t length = pageBuffer[i];
            if (i + 4 + length > SIDX_PAGE_SIZE) {
                break;
            }
            const uint8_t* entry = pageBuffer + i + 1;
            uint32_t reference = get24(entry + length);
            i += 4 + length;
            if (length >= queryLength && memcmp(entry, query, queryLength) == 0) {
                bool seen = false;
                for (uint32_t r = 0; r < found; r++) {
                    seen = seen || ngOffsets[r] == reference;
                }
                if (!seen) {
                    ngOffsets[found++] = reference;
                }
                if (found >= limit) {
                    done = true;
                    break;
                }
            } else if (compareBytes(entry, length, query, queryLength) > 0) {
                done = true;
                break;
            }
        }
    }
    return found;
}

bool SearchIndexReader::ng(uint32_t offset, SearchNg& out) {
    uint8_t bytes[SIDX_MAX_NAME + 8];
    uint32_t got = readSection(SidxSection::Ng, offset, bytes, 80);
    if (got < 1) {
        return false;
    }
    uint32_t length = bytes[0];
    if (got < length + 8) {
        got = readSection(SidxSection::Ng, offset, bytes, length + 8);
        if (got < length + 8) {
            return false;
        }
    }
    memcpy(out.name, bytes + 1, length);
    out.name[length] = 0;
    out.kind = static_cast<SearchKind>(bytes[length + 1]);
    out.first = get32(bytes + length + 2);
    out.count = static_cast<uint16_t>(get16(bytes + length + 6));
    out.offset = offset;
    return true;
}

void SearchIndexReader::decodePt(const uint8_t* bytes, int32_t& x, int32_t& y) const {
    if (pointBytes == 8) {
        x = static_cast<int32_t>(get32(bytes)) + header.originX;
        y = static_cast<int32_t>(get32(bytes + 4)) + header.originY;
    } else {
        x = static_cast<int32_t>(get16(bytes)) + header.originX;
        y = static_cast<int32_t>(get16(bytes + 2)) + header.originY;
    }
}

bool SearchIndexReader::pointRecords(uint32_t first, uint32_t count, GrowArray<SearchPointRecord>& out) {
    out.clear();
    if (count == 0) {
        return true;
    }
    uint32_t size = pointRecordBytes(pointBytes);
    GrowArray<uint8_t> raw;
    if (!raw.resize(static_cast<size_t>(count) * size) ||
        readSection(SidxSection::Points, first * size, raw.begin(), count * size) != count * size) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* b = raw.begin() + static_cast<size_t>(i) * size;
        SearchPointRecord r;
        decodePt(b, r.x, r.y);
        r.pair = b[pointBytes];
        r.ng = get24(b + pointBytes + 1);
        out.push(r);
    }
    return !out.failed();
}

bool SearchIndexReader::streetRecords(uint32_t first, uint32_t count, GrowArray<SearchStreetRecord>& out) {
    out.clear();
    if (count == 0) {
        return true;
    }
    uint32_t size = streetRecordBytes(pointBytes);
    GrowArray<uint8_t> raw;
    if (!raw.resize(static_cast<size_t>(count) * size) ||
        readSection(SidxSection::Streets, first * size, raw.begin(), count * size) != count * size) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* b = raw.begin() + static_cast<size_t>(i) * size;
        SearchStreetRecord r;
        decodePt(b, r.x, r.y);
        decodePt(b + pointBytes, r.minX, r.minY);
        decodePt(b + 2 * pointBytes, r.maxX, r.maxY);
        r.firstAddress = get32(b + 3 * pointBytes);
        r.addressCount = static_cast<uint16_t>(get16(b + 3 * pointBytes + 4));
        out.push(r);
    }
    return !out.failed();
}

void SearchIndexReader::decodeAddress(const uint8_t* b, SearchAddress& out) const {
    out.number = static_cast<uint16_t>(get16(b));
    out.aux = static_cast<uint16_t>(get16(b + 2));
    out.flags = b[5];
    out.isText = (out.flags & HOUSE_TEXT) != 0;
    out.suffix = out.isText ? 0 : static_cast<char>(b[4]);
    out.textOffset = out.isText ? (static_cast<uint32_t>(out.aux) | (static_cast<uint32_t>(b[4]) << 16)) : 0;
    decodePt(b + 6, out.x, out.y);
}

bool SearchIndexReader::addressAt(uint32_t index, SearchAddress& out) {
    uint32_t size = addressRecordBytes(pointBytes);
    uint8_t bytes[16];
    if (readSection(SidxSection::Addresses, index * size, bytes, size) != size) {
        return false;
    }
    decodeAddress(bytes, out);
    return true;
}

bool SearchIndexReader::addressFind(uint32_t first, uint32_t count, uint16_t number, int suffix, GrowArray<SearchAddress>& out) {
    out.clear();
    uint32_t lo = 0, hi = count;
    SearchAddress a;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (!addressAt(first + mid, a)) {
            return false;
        }
        if (a.number < number) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (uint32_t i = lo; i < count; i++) {
        if (!addressAt(first + i, a)) {
            return false;
        }
        if (a.number != number) {
            break;
        }
        if (suffix < 0 || a.suffix == suffix) {
            out.push(a);
        }
    }
    return !out.failed();
}

bool SearchIndexReader::overflowText(uint32_t offset, char* out, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    size_t length = 0;
    while (true) {
        char chunk[32];
        uint32_t got = readSection(SidxSection::Overflow, offset + static_cast<uint32_t>(length), chunk, sizeof(chunk));
        if (got == 0) {
            out[length < outSize ? length : outSize - 1] = 0;
            return length > 0;
        }
        for (uint32_t i = 0; i < got; i++) {
            if (chunk[i] == 0) {
                out[std::min(length, outSize - 1)] = 0;
                return true;
            }
            if (length + 1 < outSize) {
                out[length] = chunk[i];
            }
            length++;
        }
    }
}

bool SearchIndexReader::searchName(const char* key, uint32_t limit, uint32_t show, GrowArray<SearchNg>& out) {
    out.clear();
    GrowArray<uint32_t> refs;
    if (!refs.resize(limit)) {
        return false;
    }
    uint32_t found = prefix(key, limit, refs.begin());
    for (uint32_t i = 0; i < found; i++) {
        SearchNg group;
        if (!ng(refs[i], group)) {
            return false;
        }
        out.push(group);
    }
    for (uint32_t i = 0; i < out.size() && i < show; i++) {
        if (out[i].kind == SearchKind::Point) {
            GrowArray<SearchPointRecord> records;
            pointRecords(out[i].first, std::min<uint32_t>(out[i].count, 64), records);
        } else if (out[i].kind == SearchKind::Street) {
            GrowArray<SearchStreetRecord> records;
            streetRecords(out[i].first, std::min<uint32_t>(out[i].count, 16), records);
        }
    }
    return !out.failed();
}

bool SearchIndexReader::searchAddress(const char* streetKey, uint16_t number, int suffix, uint32_t limit,
                                      GrowArray<SearchAddressHit>& out) {
    out.clear();
    GrowArray<uint32_t> refs;
    if (!refs.resize(limit)) {
        return false;
    }
    uint32_t found = prefix(streetKey, limit, refs.begin());
    for (uint32_t i = 0; i < found; i++) {
        SearchNg group;
        if (!ng(refs[i], group)) {
            return false;
        }
        if (group.kind != SearchKind::Street) {
            continue;
        }
        GrowArray<SearchStreetRecord> records;
        if (!streetRecords(group.first, group.count, records)) {
            return false;
        }
        for (const SearchStreetRecord& record : records) {
            if (record.addressCount == 0) {
                continue;
            }
            GrowArray<SearchAddress> matches;
            if (!addressFind(record.firstAddress, record.addressCount, number, suffix, matches)) {
                return false;
            }
            for (const SearchAddress& a : matches) {
                SearchAddressHit hit = {};
                snprintf(hit.street, sizeof(hit.street), "%s", group.name);
                hit.address = a;
                out.push(hit);
            }
        }
        if (out.size() >= 8) {
            break;
        }
    }
    return !out.failed();
}

bool SearchIndexReader::category(const SearchNg& group, GrowArray<SearchPointRecord>& out) {
    return pointRecords(group.first, group.count, out);
}

} // namespace maps
