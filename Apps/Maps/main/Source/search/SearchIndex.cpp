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

// The writer (SearchIndexWriter::build) is the PC's reference and lives in host/oracle/SearchIndexWriter.cpp; the
// device builds its index with SearchStreamBuilder. What both share stays here.

namespace {

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
