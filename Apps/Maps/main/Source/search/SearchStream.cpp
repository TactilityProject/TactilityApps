// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchStream.h"

#include "../platform/Platform.h"
#include "QueryParse.h"
#include "TextFold.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr uint32_t NONE = UINT32_MAX;
constexpr int32_t TILE_UNITS = 4096;
/** Lines are matched up to this far outside a tile (the 100 m of matching plus the 60 m of the neighbours' vote). */
constexpr double LINE_BUFFER_M = 160.0;
constexpr uint32_t IO_BUFFER = 4096;
/** An address record's cluster reference with this bit is a settlement's (village) cluster. */
constexpr uint32_t PSEUDO = 0x80000000u;
/** Texts in sort records are cut to this many bytes (names are far shorter; a record has at most four texts). */
constexpr uint32_t MAX_TEXT = 500;
constexpr uint32_t MAX_CLUSTER_ADDRESSES = 0xFFFF;
/** Records between two keepGoing() calls in the long loops. */
constexpr uint32_t CHECK_EVERY = 2048;

enum Category : uint8_t { CAT_CATEGORY = 0, CAT_PLACE = 1, CAT_STREET = 2, CAT_OTHER = 3, CAT_POI = 4 };

int32_t floorDiv(int32_t value, int32_t by) {
    return value >= 0 ? value / by : -((-value + by - 1) / by);
}

uint64_t mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

uint64_t hashBytes(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001b3ULL;
    }
    return h;
}

uint64_t hashText(uint64_t h, const char* s) {
    return s == nullptr ? hashBytes(h, "\1", 1) : hashBytes(h, s, strlen(s) + 1);
}

uint64_t hashInt(uint64_t h, int64_t v) {
    return hashBytes(h, &v, sizeof(v));
}

constexpr uint64_t SEED = 1469598103934665603ULL;

void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8;
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
        static_cast<uint32_t>(p[3]) << 24;
}

int compareInt(int64_t a, int64_t b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

int compareText(const char* a, const char* b) {
    return compareSearchBytes(reinterpret_cast<const uint8_t*>(a), static_cast<uint32_t>(strlen(a)), reinterpret_cast<const uint8_t*>(b),
                              static_cast<uint32_t>(strlen(b)));
}

/** A record being built for a sort or a scratch file. Texts end with a zero byte and are cut to MAX_TEXT bytes. */
class Rec {
public:
    void clear() { used = 0; }
    void u8(uint32_t v) { bytes(&v, 1); }
    void u16(uint32_t v) {
        uint8_t b[2];
        put16(b, v);
        bytes(b, 2);
    }
    void u32(uint32_t v) {
        uint8_t b[4];
        put32(b, v);
        bytes(b, 4);
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void text(const char* s) {
        size_t n = s != nullptr ? strlen(s) : 0;
        if (n > MAX_TEXT) {
            n = MAX_TEXT;
            while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) {
                n--;
            }
        }
        bytes(s, n);
        const uint8_t zero = 0;
        bytes(&zero, 1);
    }
    void bytes(const void* data, size_t size) {
        if (used + size > sizeof(buffer)) {
            full = true;
            return;
        }
        if (size > 0) {
            memcpy(buffer + used, data, size);
        }
        used += static_cast<uint32_t>(size);
    }
    const uint8_t* data() const { return buffer; }
    uint32_t size() const { return used; }
    bool overflowed() const { return full; }

private:
    uint8_t buffer[ExternalSorter::MAX_RECORD] = {};
    uint32_t used = 0;
    bool full = false;
};

/** Reads the fields of a record in the order they were written. */
class Cursor {
public:
    Cursor(const uint8_t* data, uint32_t size) : p(data), end(data + size) {}
    uint32_t u8() { return p < end ? *p++ : 0; }
    uint32_t u16() {
        const uint32_t v = get16(p);
        p += 2;
        return v;
    }
    uint32_t u32() {
        const uint32_t v = get32(p);
        p += 4;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    /** A text inside the record (it ends with its zero byte). */
    const char* text() {
        const char* s = reinterpret_cast<const char*>(p);
        p += strlen(s) + 1;
        return s;
    }

private:
    const uint8_t* p;
    const uint8_t* end;
};

/** A scratch file written in IO_BUFFER pieces. */
class OutFile {
public:
    bool open(ScratchFiles* scratch, const char* path) {
        files = scratch;
        used = 0;
        total = 0;
        good = true;
        handle = files->create(path);
        good = handle >= 0;
        return good;
    }
    bool write(const void* data, uint32_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        while (size > 0 && good) {
            const uint32_t n = std::min(size, IO_BUFFER - used);
            memcpy(buffer + used, p, n);
            used += n;
            p += n;
            size -= n;
            total += n;
            if (used == IO_BUFFER) {
                good = files->write(handle, buffer, used);
                used = 0;
            }
        }
        return good;
    }
    bool write(const Rec& rec) { return write(rec.data(), rec.size()); }
    bool close() {
        if (handle < 0) {
            return good;
        }
        if (good && used > 0) {
            good = files->write(handle, buffer, used);
        }
        used = 0;
        good = files->close(handle) && good;
        handle = -1;
        return good;
    }
    uint32_t size() const { return total; }

private:
    ScratchFiles* files = nullptr;
    int handle = -1;
    uint8_t buffer[IO_BUFFER] = {};
    uint32_t used = 0;
    uint32_t total = 0;
    bool good = false;
};

/** A scratch file read in IO_BUFFER pieces, with seek. */
class InFile {
public:
    bool open(ScratchFiles* scratch, const char* path) {
        files = scratch;
        filled = 0;
        at = 0;
        base = 0;
        handle = files->openRead(path);
        return handle >= 0;
    }
    void close() {
        if (handle >= 0) {
            files->close(handle);
            handle = -1;
        }
    }
    bool seek(uint32_t offset) {
        if (offset >= base && offset <= base + filled) {
            at = offset - base;
            return true;
        }
        filled = 0;
        at = 0;
        base = offset;
        return files->seek(handle, offset);
    }
    /** Exactly size bytes; false at the end or on an error. */
    bool read(void* out, uint32_t size) {
        auto* p = static_cast<uint8_t*>(out);
        while (size > 0) {
            if (at == filled && !refill()) {
                return false;
            }
            const uint32_t n = std::min(size, filled - at);
            memcpy(p, buffer + at, n);
            at += n;
            p += n;
            size -= n;
        }
        return true;
    }
    bool u8(uint32_t& v) {
        uint8_t b = 0;
        const bool ok = read(&b, 1);
        v = b;
        return ok;
    }
    bool u32(uint32_t& v) {
        uint8_t b[4];
        const bool ok = read(b, 4);
        v = get32(b);
        return ok;
    }
    bool i32(int32_t& v) {
        uint32_t u = 0;
        const bool ok = u32(u);
        v = static_cast<int32_t>(u);
        return ok;
    }
    /** A text up to its zero byte (cut to capacity - 1). */
    bool text(char* out, uint32_t capacity) {
        uint32_t n = 0;
        while (true) {
            if (at == filled && !refill()) {
                return false;
            }
            const char c = static_cast<char>(buffer[at++]);
            if (c == '\0') {
                break;
            }
            if (n + 1 < capacity) {
                out[n++] = c;
            }
        }
        out[n] = '\0';
        return true;
    }
    bool atEnd() {
        return at == filled && !refill();
    }
    uint32_t position() const { return base + at; }

private:
    bool refill() {
        base += filled;
        filled = 0;
        at = 0;
        const int n = files->read(handle, buffer, IO_BUFFER);
        if (n <= 0) {
            return false;
        }
        filled = static_cast<uint32_t>(n);
        return true;
    }
    ScratchFiles* files = nullptr;
    int handle = -1;
    uint8_t buffer[IO_BUFFER] = {};
    uint32_t filled = 0;
    uint32_t at = 0;
    uint32_t base = 0;
};

/** Interned strings: id by text. */
class Strings {
public:
    uint32_t intern(const char* s) {
        const uint64_t hash = mix(hashText(SEED, s));
        const uint32_t found = index.find(hash, [&](uint32_t id) { return strcmp(&pool[offsets[id]], s) == 0; });
        if (found != HashIndex::NONE) {
            return found;
        }
        const auto id = static_cast<uint32_t>(offsets.size());
        offsets.push(static_cast<uint32_t>(pool.size()));
        for (const char* c = s;; c++) {
            pool.push(*c);
            if (*c == '\0') {
                break;
            }
        }
        index.insert(hash, id);
        return id;
    }
    uint32_t find(const char* s) const {
        const uint64_t hash = mix(hashText(SEED, s));
        return index.find(hash, [&](uint32_t id) { return strcmp(&pool[offsets[id]], s) == 0; });
    }
    const char* at(uint32_t id) const { return &pool[offsets[id]]; }
    uint32_t count() const { return static_cast<uint32_t>(offsets.size()); }
    size_t bytes() const { return pool.allocated() + offsets.allocated() + 12 * offsets.size(); }
    bool failed() const { return pool.failed() || offsets.failed() || index.failed(); }
    void release() {
        pool.release();
        offsets.release();
        index.release();
    }

private:
    GrowArray<char> pool;
    GrowArray<uint32_t> offsets;
    HashIndex index;
};

struct TileInfo {
    int32_t x, y;
    uint32_t lineFirst;
    uint32_t lineCount;
    uint32_t lineOffset;
    uint32_t rawHouseOffset;
    uint32_t rawHouseCount;
    uint32_t houseOffset;
    uint32_t houseCount;
};

struct LabelInfo {
    uint32_t count;
    uint32_t minSeq;
    uint32_t pair;
    uint8_t category;
};

struct SettlementInfo {
    uint32_t label;
    uint32_t nameId;
    int32_t x, y;
    uint8_t placeClass;
    /** Offsets into the extra texts, NONE for none. */
    uint32_t latin;
    uint32_t en;
    uint32_t pseudo;
};

struct Comp {
    uint32_t nameId;
    int32_t firstX, firstY;
    int32_t minX, minY, maxX, maxY;
    int64_t sumX, sumY;
    uint32_t count;
    uint32_t latin;
    int32_t repX, repY;
    double best;
    bool haveRep;
    uint32_t addresses;
    uint32_t finalIndex;
};

/** A street cluster as the index needs it after clustering. */
struct ClusterInfo {
    int32_t repX, repY;
    int32_t minX, minY, maxX, maxY;
    uint32_t addresses;
    uint32_t finalIndex;
};

struct StreetInfo {
    uint32_t nameId;
    uint32_t latin;
    uint32_t en;
    uint8_t useEn;
    uint32_t firstCluster;
    uint32_t clusterCount;
    uint32_t pseudoCount;
    /** Position in name order (the writer's creation order). */
    uint32_t order;
    uint32_t sortKey;
    uint32_t firstRecord;
};

struct PseudoInfo {
    uint32_t settlement;
    uint32_t street;
    uint32_t addresses;
    uint32_t finalIndex;
};

struct CategoryRun {
    uint32_t raw;
    uint32_t firstPair;
    uint32_t lastPair;
    uint32_t count;
    bool contiguous;
};

struct CategoryNg {
    uint32_t name;
    uint32_t sortKey;
    uint32_t firstPair;
    uint32_t pairCount;
    uint32_t creation;
};

/** A house's match, kept until the tile's addresses are written. */
struct OwnResult {
    uint32_t cluster;
    uint32_t village;
    uint8_t tier;
};

struct ColumnCell {
    int32_t cy;
    uint32_t line;
};

// ---------------------------------------------------------------- sort orders

/** S_PT: label u32, named u8, name, x, y, seq | latin, en, settlement u8, useEn u8. */
int comparePoints(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void*) {
    Cursor x(a, aSize);
    Cursor y(b, bSize);
    int c = compareInt(x.u32(), y.u32());
    if (c != 0) return c;
    c = compareInt(x.u8(), y.u8());
    if (c != 0) return c;
    c = strcmp(x.text(), y.text());
    if (c != 0) return c;
    c = compareInt(x.i32(), y.i32());
    if (c != 0) return c;
    c = compareInt(x.i32(), y.i32());
    if (c != 0) return c;
    return compareInt(x.u32(), y.u32());
}

/** S_CELL: name u32, cx, cy, line u32 (fixed). */
int compareCells(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void*) {
    Cursor x(a, aSize);
    Cursor y(b, bSize);
    int c = compareInt(x.u32(), y.u32());
    if (c != 0) return c;
    c = compareInt(x.i32(), y.i32());
    if (c != 0) return c;
    c = compareInt(x.i32(), y.i32());
    if (c != 0) return c;
    return compareInt(x.u32(), y.u32());
}

/** Fixed records compared by their first n u32/i32 fields: the field kinds per sort below. */
int compareU32Fields(const uint8_t* a, const uint8_t* b, int fields, const bool* isSigned) {
    for (int i = 0; i < fields; i++) {
        const uint32_t ua = get32(a + 4 * i);
        const uint32_t ub = get32(b + 4 * i);
        if (ua != ub) {
            if (isSigned[i]) {
                return static_cast<int32_t>(ua) < static_cast<int32_t>(ub) ? -1 : 1;
            }
            return ua < ub ? -1 : 1;
        }
    }
    return 0;
}

/** S_A1: final u32, seq u32 | flags, x, y, text. */
int compareAddressSeq(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[2] = { false, false };
    return compareU32Fields(a, b, 2, kinds);
}

/** S_A2: final u32, number u32, aux u32, suffix u32, seq u32 | flags, x, y. */
int compareAddressNumber(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[5] = { false, false, false, false, false };
    return compareU32Fields(a, b, 5, kinds);
}

/** S_GRP: name, pair u8, seq u32 | ... */
int compareGroups(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void*) {
    Cursor x(a, aSize);
    Cursor y(b, bSize);
    int c = strcmp(x.text(), y.text());
    if (c != 0) return c;
    c = compareInt(x.u8(), y.u8());
    if (c != 0) return c;
    return compareInt(x.u32(), y.u32());
}

/** S_NG: sortKey, pair u8, headSeq u32 | ... (the writer's name group order for points) */
int compareNameGroups(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void*) {
    Cursor x(a, aSize);
    Cursor y(b, bSize);
    int c = compareText(x.text(), y.text());
    if (c != 0) return c;
    c = compareInt(x.u8(), y.u8());
    if (c != 0) return c;
    return compareInt(x.u32(), y.u32());
}

/** S_HS: hash high, hash low, not inside, tile, local (all u32): copies of one house together, the winner first. */
int compareHouseCopies(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[5] = { false, false, false, false, false };
    return compareU32Fields(a, b, 5, kinds);
}

/** S_DROP: tile, local. */
int compareFirstTwo(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[2] = { false, false };
    return compareU32Fields(a, b, 2, kinds);
}

/** S_G2R and S_FC: the first u32. */
int compareFirst(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[1] = { false };
    return compareU32Fields(a, b, 1, kinds);
}

/** S_REC: pair, unnamed, rank, tileY, tileX, x, y | offset (the writer's point record order). */
int comparePointRecords(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    static const bool kinds[7] = { false, false, false, true, true, true, true };
    return compareU32Fields(a, b, 7, kinds);
}

/** S_KEY: length u8, key, priority u8, offset u32 (the writer's key order). */
int compareKeys(const uint8_t* a, uint32_t, const uint8_t* b, uint32_t, void*) {
    const uint32_t la = a[0];
    const uint32_t lb = b[0];
    int c = compareSearchBytes(a + 1, la, b + 1, lb);
    if (c != 0) return c;
    c = compareInt(a[1 + la], b[1 + lb]);
    if (c != 0) return c;
    return compareInt(get32(a + 2 + la), get32(b + 2 + lb));
}

} // namespace

// ------------------------------------------------------------------------------------------------ number helpers

HouseNumberStyle houseNumberStyle(const char* text) {
    HouseNumberStyle k = {};
    k.parsed = parseHouseNumber(text, strlen(text), k.number);
    if (k.parsed) {
        k.plain = k.number.aux == 0 && k.number.suffix == 0 && k.number.flags == 0;
        k.slash = (k.number.flags & HOUSE_SLASH) != 0 && k.number.aux != 0;
        k.secondSmaller = k.slash && k.number.aux < k.number.number;
    }
    return k;
}

int32_t houseVoteNumber(const char* text, bool slashSecond) {
    const HouseNumberStyle kind = houseNumberStyle(text);
    if (kind.parsed) {
        if (kind.slash && slashSecond) {
            return static_cast<int32_t>(kind.number.aux);
        }
        return static_cast<int32_t>(kind.number.number);
    }
    int32_t value = 0;
    int digits = 0;
    while (text[digits] >= '0' && text[digits] <= '9' && digits < 9) {
        value = value * 10 + (text[digits] - '0');
        digits++;
    }
    return digits > 0 ? value : -1;
}

// ------------------------------------------------------------------------------------------------ the builder

struct SearchStreamBuilder::Impl {
    SearchStreamBuilder* owner = nullptr;
    ScratchFiles* files = nullptr;
    StreamTileSource* source = nullptr;
    MvtDecoder* decoder = nullptr;
    const StreamBuildOptions* options = nullptr;
    RegionConfig config;
    StreamBuildStats* st = nullptr;
    char dir[128] = {};
    char path[160] = {};
    StreamProgress progress = {};

    // Tiles
    GrowArray<TileInfo> tiles;
    HashIndex tileIndex;
    double regionMpu = 0;

    // Phase 1: the extractor hands over feature by feature (extract holds one feature)
    struct Consumer final : ExtractConsumer {
        Impl* impl = nullptr;
        bool take(const ExtractedTile& feature) override { return impl->takeFeature(feature); }
    };
    Consumer consumer;
    uint32_t currentTile = 0;
    ExtractedTile extract;
    TileExtractor extractor;
    Strings names;
    Strings labelNames;
    GrowArray<LabelInfo> labels;
    GrowArray<uint32_t> lineName;
    GrowArray<uint64_t> lineHash;
    GrowArray<SettlementInfo> settlements;
    GrowArray<char> extraText;
    uint32_t pointSeq = 0;
    uint32_t pointCount = 0;
    uint32_t namedCount = 0;
    bool villageMode = false;
    bool slashSecond = false;

    // Bounds of everything the index stores (the writer's origin)
    bool anyPoint = false;
    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;

    // Pairs and categories
    Strings pairTexts;
    uint32_t pairTotal = 0;
    GrowArray<uint32_t> pairStart;
    GrowArray<uint32_t> pairCount;
    GrowArray<char> categoryChars;
    GrowArray<CategoryNg> categories;
    uint32_t collapsed = 0;
    uint32_t notContiguous = 0;

    // Clusters, streets, villages
    GrowArray<uint32_t> lineCluster;
    GrowArray<ClusterInfo> clusters;
    GrowArray<StreetInfo> streets;
    GrowArray<uint32_t> nameStreet;
    GrowArray<PseudoInfo> pseudos;
    GrowArray<uint32_t> streetNgOrder;
    GrowArray<char> sortKeys;
    uint32_t streetRecordTotal = 0;
    uint32_t addressTotal = 0;

    // Name groups
    uint32_t pointNgCount = 0;
    uint32_t pointNgBytes = 0;
    uint32_t streetNgBytes = 0;
    uint32_t ngTotal = 0;
    uint32_t keyCount = 0;
    uint32_t overflowCount = 0;
    uint32_t overflowBytes = 0;
    GrowArray<uint8_t> fence;
    uint32_t fenceCount = 0;
    uint32_t pageCount = 0;

    // Work objects (big: kept here, not on the stack)
    AddressMatcher matcher;
    KeyMaker keys;
    ExternalSorter sortA;
    ExternalSorter sortB;
    Rec rec;
    OutFile outA;
    OutFile outB;
    InFile inA;
    InFile inB;
    GrowArray<int32_t> points;
    GrowArray<char> tileText;
    GrowArray<uint32_t> tileTextAt;
    GrowArray<int32_t> houseXY;
    char textA[MAX_TEXT + 8] = {};
    char textB[MAX_TEXT + 8] = {};
    char textC[MAX_TEXT + 8] = {};
    char keyScratch[2048] = {};

    // ------------------------------------------------------------ helpers

    const char* file(const char* name) {
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        return path;
    }

    bool fail(const char* message) {
        if (owner->errorText[0] == '\0') {
            snprintf(owner->errorText, sizeof(owner->errorText), "%s", message);
        }
        return false;
    }

    bool fail(const char* format, uint32_t a, uint32_t b = 0) {
        if (owner->errorText[0] == '\0') {
            snprintf(owner->errorText, sizeof(owner->errorText), format, a, b);
        }
        return false;
    }

    bool keepGoing() {
        if (options->keepGoing != nullptr && !options->keepGoing(progress, options->context)) {
            owner->wasCancelled = true;
            return fail("cancelled");
        }
        return true;
    }

    void step() {
        progress.done++;
    }

    void include(int32_t x, int32_t y) {
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
    }

    uint32_t tileAt(int32_t x, int32_t y) const {
        const uint64_t hash = mix(static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(y));
        return tileIndex.find(hash, [&](uint32_t t) { return tiles[t].x == x && tiles[t].y == y; });
    }

    uint32_t addExtra(const char* s) {
        if (s == nullptr) {
            return NONE;
        }
        const auto offset = static_cast<uint32_t>(extraText.size());
        for (const char* c = s;; c++) {
            extraText.push(*c);
            if (*c == '\0') {
                break;
            }
        }
        return offset;
    }
    const char* extra(uint32_t offset) const { return offset == NONE ? nullptr : &extraText[offset]; }

    bool sortBegin(ExternalSorter& sorter, const char* name, ExternalSorter::Compare compare) {
        char prefix[160];
        snprintf(prefix, sizeof(prefix), "%s/%s", dir, name);
        if (!sorter.begin(files, prefix, options->sortBufferBytes, compare, nullptr)) {
            return fail(sorter.errorText());
        }
        return true;
    }

    bool sortAdd(ExternalSorter& sorter) {
        if (rec.overflowed()) {
            return fail("a record is too long");
        }
        if (!sorter.add(rec.data(), rec.size())) {
            return fail(sorter.errorText());
        }
        return true;
    }

    void noteSort(const ExternalSorter& sorter) {
        st->scratchBytes += sorter.bytesWritten();
        st->sortPeakBytes = std::max(st->sortPeakBytes, sorter.peakBytes());
    }

    void noteTables() {
        size_t bytes = names.bytes() + labelNames.bytes() + labels.allocated() + lineName.allocated() + lineHash.allocated() +
            settlements.allocated() + extraText.allocated() + tiles.allocated() + lineCluster.allocated() + clusters.allocated() +
            streets.allocated() + nameStreet.allocated() + pseudos.allocated() + streetNgOrder.allocated() + sortKeys.allocated() +
            categories.allocated() + categoryChars.allocated() +
            fence.allocated() + pairStart.allocated() + pairCount.allocated();
        st->tableBytes = std::max(st->tableBytes, bytes);
    }

    // ------------------------------------------------------------ 1. tiles

    bool readTiles() {
        const uint32_t started = nowMs();
        if (!outA.open(files, file("l.tmp")) || !outB.open(files, file("h.tmp"))) {
            return fail("cannot create the scratch files");
        }
        if (!sortBegin(sortA, "s_pt", comparePoints) || !sortBegin(sortB, "s_hs", compareHouseCopies)) {
            return false;
        }
        RegionStats& rs = st->region;
        for (uint32_t t = 0; t < tiles.size(); t++) {
            if (!keepGoing()) {
                return false;
            }
            TileInfo& tile = tiles[t];
            ByteInput* input = source->open(tile.x, tile.y);
            if (input == nullptr) {
                return fail("cannot read tile %u/%u", static_cast<uint32_t>(tile.x), static_cast<uint32_t>(tile.y));
            }
            const uint32_t tileStart = nowMs();
            currentTile = t;
            tile.lineFirst = static_cast<uint32_t>(lineName.size());
            tile.lineOffset = outA.size();
            tile.rawHouseOffset = outB.size();
            tile.rawHouseCount = 0;
            consumer.impl = this;
            if (!extractor.extract(*decoder, input, tile.x, tile.y, extract, &consumer)) {
                return owner->errorText[0] != '\0' ? false
                                                    : fail("cannot decode tile %u/%u", static_cast<uint32_t>(tile.x), static_cast<uint32_t>(tile.y));
            }
            tile.lineCount = static_cast<uint32_t>(lineName.size()) - tile.lineFirst;
            rs.refOnlyLines += extract.refOnlyLines;
            rs.junkPoisSkipped += extract.junkPois;
            const uint32_t ms = nowMs() - tileStart;
            if (ms > st->slowestExtractMs) {
                st->slowestExtractMs = ms;
                st->slowestExtractTile = { tile.x, tile.y };
            }
            step();
        }
        if (!outA.close() || !outB.close()) {
            return fail("cannot write the scratch files");
        }
        st->scratchBytes += outA.size() + outB.size();
        extract.release();
        source->tilesDone();
        lineHash.release();
        rs.tiles = static_cast<uint32_t>(tiles.size());
        rs.lines = static_cast<uint32_t>(lineName.size());
        rs.settlements = static_cast<uint32_t>(settlements.size());
        noteTables();
        st->phaseMs[0] = nowMs() - started;
        return !names.failed() && !lineName.failed() && !settlements.failed() ? true : fail("no memory for the tile tables");
    }

    /** One decoded feature of the current tile: its lines, house number or point. */
    bool takeFeature(const ExtractedTile& feature) {
        return addLines(currentTile, feature) && addHouses(currentTile, feature) && addPoints(feature);
    }

    bool addLines(uint32_t t, const ExtractedTile& extract) {
        const TileInfo& tile = tiles[t];
        for (size_t i = 0; i < extract.lines.size(); i++) {
            const ExtractLine& l = extract.lines[i];
            const char* name = extract.str(l.name);
            const int32_t* xy = &extract.linePoints[2 * static_cast<size_t>(l.first)];
            const uint64_t hash = mix(hashBytes(hashText(SEED, name), xy, 2 * sizeof(int32_t) * l.count));
            // A copy from a neighbour's buffer: the same name and points in this tile or a neighbour read before
            bool duplicate = false;
            for (uint32_t k = tile.lineFirst; k < lineName.size() && !duplicate; k++) {
                duplicate = lineHash[k] == hash;
            }
            for (int dx = -1; dx <= 1 && !duplicate; dx++) {
                for (int dy = -1; dy <= 1 && !duplicate; dy++) {
                    const uint32_t n = (dx == 0 && dy == 0) ? NONE : tileAt(tile.x + dx, tile.y + dy);
                    if (n == NONE || n >= t) {
                        continue;
                    }
                    for (uint32_t k = tiles[n].lineFirst; k < tiles[n].lineFirst + tiles[n].lineCount && !duplicate; k++) {
                        duplicate = lineHash[k] == hash;
                    }
                }
            }
            if (duplicate) {
                continue;
            }
            const uint32_t nameId = names.intern(name);
            lineName.push(nameId);
            lineHash.push(hash);
            rec.clear();
            rec.u32(nameId);
            rec.u8(l.pathClass);
            const char* latin = extract.str(l.latin);
            rec.u8(latin != nullptr ? 1 : 0);
            rec.u32(l.count);
            if (!outA.write(rec) || (latin != nullptr && !outA.write(latin, static_cast<uint32_t>(strlen(latin)) + 1)) ||
                !outA.write(xy, 2 * sizeof(int32_t) * l.count)) {
                return fail("cannot write the lines");
            }
        }
        return true;
    }

    bool addHouses(uint32_t t, const ExtractedTile& extract) {
        TileInfo& tile = tiles[t];
        for (size_t i = 0; i < extract.houses.size(); i++) {
            const ExtractHouse& h = extract.houses[i];
            const char* text = extract.str(h.text);
            // Its index among the tile's house numbers (the tile's own order)
            const uint32_t local = tile.rawHouseCount++;
            rec.clear();
            rec.u32(local);
            rec.text(text);
            rec.i32(h.x);
            rec.i32(h.y);
            if (!outB.write(rec)) {
                return fail("cannot write the house numbers");
            }
            // Every copy goes into the sort that picks one per (text, position): see filterHouses()
            const uint64_t hash = mix(hashInt(hashInt(hashText(SEED, text), h.x), h.y));
            rec.clear();
            rec.u32(static_cast<uint32_t>(hash >> 32));
            rec.u32(static_cast<uint32_t>(hash));
            rec.u32(h.inside != 0 ? 0 : 1);
            rec.u32(t);
            rec.u32(local);
            if (!sortAdd(sortB)) {
                return false;
            }
        }
        return true;
    }

    /**
     * One copy per house number (text and position), like SearchRegionBuilder: the copy inside its own tile, else the
     * first copy read (tile order, then the tile's own order). The others are dropped while h.tmp becomes i.tmp; the
     * region's number statistics and modes come from what is kept.
     */
    bool filterHouses() {
        const uint32_t started = nowMs();
        RegionStats& rs = st->region;
        if (!sortB.finish() || !sortBegin(sortA, "s_drop", compareFirstTwo)) {
            return fail("cannot sort the house numbers");
        }
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        uint32_t lastHi = 0;
        uint32_t lastLo = 0;
        bool have = false;
        while (sortB.next(data, size)) {
            const uint32_t hi = get32(data);
            const uint32_t lo = get32(data + 4);
            if (have && hi == lastHi && lo == lastLo) {
                rec.clear();
                rec.u32(get32(data + 12));
                rec.u32(get32(data + 16));
                if (!sortAdd(sortA)) {
                    return false;
                }
                continue;
            }
            have = true;
            lastHi = hi;
            lastLo = lo;
        }
        if (sortB.failed() || !sortA.finish()) {
            return fail(sortB.failed() ? sortB.errorText() : sortA.errorText());
        }
        noteSort(sortB);
        sortB.end();
        if (!inB.open(files, file("h.tmp")) || !outB.open(files, file("i.tmp"))) {
            return fail("cannot filter the house numbers");
        }
        bool dropPending = sortA.next(data, size);
        uint32_t dropTile = dropPending ? get32(data) : NONE;
        uint32_t dropLocal = dropPending ? get32(data + 4) : NONE;
        for (uint32_t t = 0; t < tiles.size(); t++) {
            TileInfo& tile = tiles[t];
            tile.houseOffset = outB.size();
            tile.houseCount = 0;
            for (uint32_t k = 0; k < tile.rawHouseCount; k++) {
                uint32_t local = 0;
                int32_t x = 0;
                int32_t y = 0;
                if (!inB.u32(local) || !inB.text(textA, sizeof(textA)) || !inB.i32(x) || !inB.i32(y)) {
                    return fail("cannot read the house numbers");
                }
                if (dropPending && dropTile == t && dropLocal == local) {
                    dropPending = sortA.next(data, size);
                    dropTile = dropPending ? get32(data) : NONE;
                    dropLocal = dropPending ? get32(data + 4) : NONE;
                    continue;
                }
                rec.clear();
                rec.text(textA);
                rec.i32(x);
                rec.i32(y);
                if (!outB.write(rec)) {
                    return fail("cannot write the house numbers");
                }
                tile.houseCount++;
                rs.houses++;
                const HouseNumberStyle kind = houseNumberStyle(textA);
                rs.textNumbers += kind.parsed ? 0 : 1;
                rs.plainNumbers += kind.plain ? 1 : 0;
                rs.slashNumbers += kind.slash ? 1 : 0;
                rs.slashSecondSmaller += kind.secondSmaller ? 1 : 0;
            }
            if (t % 16 == 0 && !keepGoing()) {
                return false;
            }
        }
        inB.close();
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        noteSort(sortA);
        sortA.end();
        if (!outB.close()) {
            return fail("cannot write the house numbers");
        }
        st->scratchBytes += outB.size();
        files->remove(file("h.tmp"));
        rs.plainShare = rs.houses > 0 ? static_cast<double>(rs.plainNumbers) / static_cast<double>(rs.houses) : 0.0;
        rs.villageMode = rs.plainShare >= config.villageShare && rs.houses > 0;
        rs.slashShare = rs.slashNumbers > 0 ? static_cast<double>(rs.slashSecondSmaller) / static_cast<double>(rs.slashNumbers) : 0.0;
        rs.slashSecond = rs.slashShare >= config.slashSecondShare && rs.slashNumbers > 0;
        villageMode = rs.villageMode;
        slashSecond = rs.slashSecond;
        st->phaseMs[1] += nowMs() - started;
        step();
        return true;
    }

    bool addPoints(const ExtractedTile& extract) {
        for (size_t i = 0; i < extract.points.size(); i++) {
            const ExtractPoint& p = extract.points[i];
            const uint32_t seq = pointSeq++;
            const char* labelText = extract.str(p.label);
            const uint32_t label = labelNames.intern(labelText);
            if (label == labels.size()) {
                LabelInfo info = {};
                info.minSeq = NONE;
                info.category = strncmp(labelText, "poi/", 4) == 0 ? CAT_POI : (strncmp(labelText, "place/", 6) == 0 ? CAT_PLACE : CAT_OTHER);
                labels.push(info);
            }
            const char* name = extract.str(p.name);
            rec.clear();
            rec.u32(label);
            rec.u8(name != nullptr ? 1 : 0);
            rec.text(name);
            rec.i32(p.x);
            rec.i32(p.y);
            rec.u32(seq);
            rec.text(extract.str(p.latin));
            rec.u8(p.latin != EXTRACT_NONE ? 1 : 0);
            rec.text(extract.str(p.en));
            rec.u8(p.en != EXTRACT_NONE ? 1 : 0);
            rec.u8(p.useEn);
            if (!sortAdd(sortA)) {
                return false;
            }
            // Settlements for the village rule, in the order they are first seen (the points' order)
            if (p.settlement != EXTRACT_NOT_SETTLEMENT && name != nullptr) {
                const uint32_t nameId = names.intern(name);
                bool known = false;
                for (const SettlementInfo& s : settlements) {
                    known = known || (s.label == label && s.nameId == nameId && s.x == p.x && s.y == p.y);
                }
                if (!known) {
                    SettlementInfo s = {};
                    s.label = label;
                    s.nameId = nameId;
                    s.x = p.x;
                    s.y = p.y;
                    s.placeClass = p.settlement;
                    s.latin = addExtra(extract.str(p.latin));
                    s.en = addExtra(extract.str(p.en));
                    s.pseudo = NONE;
                    settlements.push(s);
                }
            }
        }
        return true;
    }

    // ------------------------------------------------------------ 2. points

    bool dedupPoints() {
        const uint32_t started = nowMs();
        if (!sortA.finish()) {
            return fail(sortA.errorText());
        }
        if (!outA.open(files, file("p.tmp"))) {
            return fail("cannot create the point file");
        }
        RegionStats& rs = st->region;
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        uint32_t lastLabel = NONE;
        uint32_t lastNamed = 0;
        int32_t lastX = 0, lastY = 0;
        bool have = false;
        uint32_t n = 0;
        while (sortA.next(data, size)) {
            if (++n % CHECK_EVERY == 0 && !keepGoing()) {
                return false;
            }
            Cursor c(data, size);
            const uint32_t label = c.u32();
            const uint32_t named = c.u8();
            const char* name = c.text();
            const int32_t x = c.i32();
            const int32_t y = c.i32();
            const uint32_t seq = c.u32();
            if (have && label == lastLabel && named == lastNamed && x == lastX && y == lastY && strcmp(name, textA) == 0) {
                continue;   // a later copy of the same point
            }
            have = true;
            lastLabel = label;
            lastNamed = named;
            lastX = x;
            lastY = y;
            snprintf(textA, sizeof(textA), "%s", name);
            const char* latin = c.text();
            const uint32_t hasLatin = c.u8();
            const char* en = c.text();
            const uint32_t hasEn = c.u8();
            const uint32_t useEn = c.u8();
            LabelInfo& info = labels[label];
            info.count++;
            info.minSeq = std::min(info.minSeq, seq);
            pointCount++;
            include(x, y);
            if (info.category == CAT_PLACE) {
                rs.places++;
            } else if (info.category == CAT_POI) {
                if (named != 0) {
                    rs.namedPois++;
                } else {
                    rs.unnamedPois++;
                }
            } else {
                rs.otherNamed++;
            }
            namedCount += named != 0 ? 1 : 0;
            rec.clear();
            rec.u32(label);
            rec.u8(named);
            rec.text(name);
            rec.text(hasLatin != 0 ? latin : nullptr);
            rec.u8(hasLatin);
            rec.text(hasEn != 0 ? en : nullptr);
            rec.u8(hasEn);
            rec.u8(useEn);
            rec.i32(x);
            rec.i32(y);
            rec.u32(seq);
            if (!outA.write(rec)) {
                return fail("cannot write the points");
            }
        }
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        noteSort(sortA);
        sortA.end();
        if (!outA.close()) {
            return fail("cannot write the points");
        }
        st->scratchBytes += outA.size();
        if (!makePairs()) {
            return false;
        }
        st->phaseMs[1] = nowMs() - started;
        step();
        return true;
    }

    /** Class pairs as the writer makes them: labels by text; beyond 255 the rare ones fold to their kind and class plus a star (top 110 kept). */
    bool makePairs() {
        const uint32_t total = static_cast<uint32_t>(labels.size());
        GrowArray<uint32_t> byText;
        for (uint32_t u = 0; u < total; u++) {
            byText.push(u);
        }
        std::sort(byText.begin(), byText.end(), [&](uint32_t a, uint32_t b) { return strcmp(labelNames.at(a), labelNames.at(b)) < 0; });
        GrowArray<uint8_t> keep;
        keep.resize(total);
        if (total > 255) {
            GrowArray<uint32_t> byCount;
            for (uint32_t u = 0; u < total; u++) {
                byCount.push(u);
            }
            std::sort(byCount.begin(), byCount.end(), [&](uint32_t a, uint32_t b) {
                return labels[a].count != labels[b].count ? labels[a].count > labels[b].count : labels[a].minSeq < labels[b].minSeq;
            });
            for (uint32_t k = 0; k < 110; k++) {
                keep[byCount[k]] = 1;
            }
            collapsed = total - 110;
        } else {
            for (uint32_t u = 0; u < total; u++) {
                keep[u] = 1;
            }
        }
        // The folded texts, then the pairs in their text order
        Strings folded;
        GrowArray<uint32_t> foldedOf;
        foldedOf.resize(total);
        for (uint32_t u = 0; u < total; u++) {
            const char* label = labelNames.at(u);
            char text[300];
            if (keep[u]) {
                snprintf(text, sizeof(text), "%s", label);
            } else {
                const char* slash = strchr(label, '/');
                const char* second = slash != nullptr ? strchr(slash + 1, '/') : nullptr;
                const size_t keepLength = second != nullptr ? static_cast<size_t>(second - label) : strlen(label);
                snprintf(text, sizeof(text), "%.*s/*", static_cast<int>(keepLength), label);
            }
            foldedOf[u] = folded.intern(text);
        }
        GrowArray<uint32_t> foldedOrder;
        for (uint32_t f = 0; f < folded.count(); f++) {
            foldedOrder.push(f);
        }
        std::sort(foldedOrder.begin(), foldedOrder.end(), [&](uint32_t a, uint32_t b) { return strcmp(folded.at(a), folded.at(b)) < 0; });
        pairTotal = folded.count();
        if (pairTotal > 255) {
            return fail("%u class pairs do not fit a byte", pairTotal);
        }
        GrowArray<uint32_t> pairOfFolded;
        pairOfFolded.resize(pairTotal);
        for (uint32_t k = 0; k < pairTotal; k++) {
            pairOfFolded[foldedOrder[k]] = k;
            const char* text = folded.at(foldedOrder[k]);
            if (strlen(text) > 255) {
                return fail("class label %u is longer than 255 bytes", k);
            }
            pairTexts.intern(text);   // pair k has id k
        }
        for (uint32_t u = 0; u < total; u++) {
            labels[u].pair = pairOfFolded[foldedOf[u]];
        }
        pairStart.resize(pairTotal);
        pairCount.resize(pairTotal);
        return makeCategories();
    }

    /** Category name groups as the writer makes them: class and subclass names of the poi pairs, when contiguous. */
    bool makeCategories() {
        GrowArray<CategoryRun> classRuns;
        GrowArray<CategoryRun> subRuns;
        auto addRun = [&](GrowArray<CategoryRun>& runs, const char* raw, size_t length, uint32_t pair) {
            for (CategoryRun& run : runs) {
                const char* stored = &categoryChars[run.raw];
                if (strlen(stored) == length && memcmp(stored, raw, length) == 0) {
                    if (pair != run.lastPair + 1) {
                        run.contiguous = false;
                    }
                    run.lastPair = pair;
                    run.count++;
                    return;
                }
            }
            CategoryRun run = {};
            run.raw = static_cast<uint32_t>(categoryChars.size());
            for (size_t i = 0; i < length; i++) {
                categoryChars.push(raw[i]);
            }
            categoryChars.push('\0');
            run.firstPair = run.lastPair = pair;
            run.count = 1;
            run.contiguous = true;
            runs.push(run);
        };
        for (uint32_t pair = 0; pair < pairTotal; pair++) {
            const char* label = pairTexts.at(pair);
            if (strncmp(label, "poi/", 4) != 0) {
                continue;
            }
            const char* classStart = label + 4;
            const char* classEnd = strchr(classStart, '/');
            const size_t classLength = classEnd != nullptr ? static_cast<size_t>(classEnd - classStart) : strlen(classStart);
            addRun(classRuns, classStart, classLength, pair);
            if (classEnd != nullptr) {
                const char* subStart = classEnd + 1;
                const char* subEnd = strchr(subStart, '/');
                const size_t subLength = subEnd != nullptr ? static_cast<size_t>(subEnd - subStart) : strlen(subStart);
                if (!(subLength == classLength && memcmp(subStart, classStart, subLength) == 0)) {
                    addRun(subRuns, subStart, subLength, pair);
                }
            }
        }
        // Keys of category names that are not ASCII (SearchRegionBuilder's overrides, from every raw label)
        Strings overrideNames;
        GrowArray<uint32_t> overrideKeys;
        for (uint32_t u = 0; u < labels.size(); u++) {
            const char* label = labelNames.at(u);
            if (strncmp(label, "poi/", 4) != 0) {
                continue;
            }
            const char* part = label + 4;
            for (int k = 0; k < 2 && part != nullptr; k++) {
                const char* slash = strchr(part, '/');
                const size_t length = slash != nullptr ? static_cast<size_t>(slash - part) : strlen(part);
                char display[200];
                const size_t n = std::min(length, sizeof(display) - 1);
                bool ascii = true;
                for (size_t c = 0; c < n; c++) {
                    display[c] = part[c] == '_' ? ' ' : part[c];
                    ascii = ascii && static_cast<uint8_t>(part[c]) < 0x80;
                }
                display[n] = '\0';
                part = slash != nullptr ? slash + 1 : nullptr;
                if (ascii || overrideNames.find(display) != HashIndex::NONE) {
                    continue;
                }
                overrideNames.intern(display);
                KeyMaker::sortKeyOf(display, config.profile, keyScratch, sizeof(keyScratch));
                overrideKeys.push(addExtra(keyScratch));
            }
        }
        uint32_t creation = 0;
        for (int which = 0; which < 2; which++) {
            for (const CategoryRun& run : which == 0 ? classRuns : subRuns) {
                if (!run.contiguous) {
                    notContiguous++;
                    continue;
                }
                char name[128];
                const char* raw = &categoryChars[run.raw];
                const size_t length = std::min(strlen(raw), sizeof(name) - 1);
                memcpy(name, raw, length);
                for (size_t i = 0; i < length; i++) {
                    if (name[i] == '_') {
                        name[i] = ' ';
                    }
                }
                name[length] = '\0';
                char key[128];
                foldCategoryName(name, key, sizeof(key));
                const uint32_t o = overrideNames.find(name);
                if (o != HashIndex::NONE) {
                    snprintf(key, sizeof(key), "%s", extra(overrideKeys[o]));
                }
                CategoryNg ng = {};
                ng.name = addExtra(name);
                ng.sortKey = addExtra(key);
                ng.firstPair = run.firstPair;
                ng.pairCount = run.count;
                ng.creation = creation++;
                categories.push(ng);
            }
        }
        std::sort(categories.begin(), categories.end(), [&](const CategoryNg& a, const CategoryNg& b) {
            const int c = compareText(extra(a.sortKey), extra(b.sortKey));
            return c != 0 ? c < 0 : a.creation < b.creation;
        });
        return !categories.failed() && !categoryChars.failed() && !extraText.failed() ? true : fail("no memory for the categories");
    }

    // ------------------------------------------------------------ 3. street clusters

    /** Reads one line record of l.tmp: name, path class, latin (into textC, "" for none), points. */
    bool readLine(InFile& in, uint32_t& nameId, uint32_t& pathClass, bool& hasLatin, GrowArray<int32_t>& xy) {
        uint32_t latinFlag = 0;
        uint32_t count = 0;
        if (!in.u32(nameId) || !in.u8(pathClass) || !in.u8(latinFlag) || !in.u32(count)) {
            return false;
        }
        hasLatin = latinFlag != 0;
        textC[0] = '\0';
        if (hasLatin && !in.text(textC, sizeof(textC))) {
            return false;
        }
        xy.resize(2 * static_cast<size_t>(count));
        return !xy.failed() && in.read(xy.begin(), 2 * sizeof(int32_t) * count);
    }

    bool makeClusters() {
        const uint32_t started = nowMs();
        GrowArray<Comp> comps;
        const uint32_t lineTotal = static_cast<uint32_t>(lineName.size());
        const auto cell = std::max<int32_t>(1, static_cast<int32_t>(150.0 / regionMpu));
        if (!sortBegin(sortA, "s_cell", compareCells) || !inA.open(files, file("l.tmp"))) {
            return fail("cannot read the lines");
        }
        for (uint32_t i = 0; i < lineTotal; i++) {
            uint32_t nameId = 0;
            uint32_t pathClass = 0;
            bool hasLatin = false;
            if (!readLine(inA, nameId, pathClass, hasLatin, points)) {
                return fail("cannot read line %u", i);
            }
            int32_t previousX = INT32_MIN;
            int32_t previousY = INT32_MIN;
            for (size_t k = 0; k < points.size() / 2; k++) {
                const int32_t cx = floorDiv(points[2 * k], cell);
                const int32_t cy = floorDiv(points[2 * k + 1], cell);
                if (cx == previousX && cy == previousY) {
                    continue;
                }
                previousX = cx;
                previousY = cy;
                rec.clear();
                rec.u32(nameId);
                rec.i32(cx);
                rec.i32(cy);
                rec.u32(i);
                if (!sortAdd(sortA)) {
                    return false;
                }
            }
            if (i % CHECK_EVERY == 0 && !keepGoing()) {
                return false;
            }
        }
        inA.close();
        if (!sortA.finish()) {
            return fail(sortA.errorText());
        }
        // Union-find over the lines: the same cell, or touching cells (8 neighbours), of the same name join
        GrowArray<uint32_t>& parent = lineCluster;
        parent.resize(lineTotal);
        for (uint32_t i = 0; i < lineTotal; i++) {
            parent[i] = i;
        }
        auto find = [&](uint32_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        auto join = [&](uint32_t a, uint32_t b) {
            a = find(a);
            b = find(b);
            if (a != b) {
                parent[a] = b;
            }
        };
        GrowArray<ColumnCell> previous;
        GrowArray<ColumnCell> current;
        uint32_t name = NONE;
        int32_t column = 0;
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        while (sortA.next(data, size)) {
            Cursor c(data, size);
            const uint32_t n = c.u32();
            const int32_t cx = c.i32();
            const int32_t cy = c.i32();
            const uint32_t line = c.u32();
            if (n != name) {
                name = n;
                previous.clear();
                current.clear();
                column = cx;
            } else if (cx != column) {
                if (cx == column + 1) {
                    previous.swap(current);
                } else {
                    previous.clear();
                }
                current.clear();
                column = cx;
            }
            if (current.size() > 0 && current[current.size() - 1].cy == cy) {
                join(line, current[current.size() - 1].line);
                continue;
            }
            if (current.size() > 0 && current[current.size() - 1].cy == cy - 1) {
                join(line, current[current.size() - 1].line);
            }
            // The column to the left: cells cy - 1 .. cy + 1 (sorted by cy)
            const ColumnCell* lo = std::lower_bound(previous.begin(), previous.end(), cy - 1,
                                                    [](const ColumnCell& cell, int32_t value) { return cell.cy < value; });
            for (const ColumnCell* p = lo; p != previous.end() && p->cy <= cy + 1; p++) {
                join(line, p->line);
            }
            current.push({ cy, line });
        }
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        noteSort(sortA);
        sortA.end();
        previous.release();
        current.release();
        // Components: one per root, numbered in line order
        GrowArray<uint32_t> compOfRoot;
        compOfRoot.resize(lineTotal);
        for (uint32_t i = 0; i < lineTotal; i++) {
            compOfRoot[i] = NONE;
        }
        GrowArray<uint32_t> compOfLine;
        compOfLine.resize(lineTotal);
        for (uint32_t i = 0; i < lineTotal; i++) {
            const uint32_t root = find(i);
            if (compOfRoot[root] == NONE) {
                compOfRoot[root] = static_cast<uint32_t>(comps.size());
                Comp comp = {};
                comp.nameId = lineName[i];
                comp.minX = comp.minY = INT32_MAX;
                comp.maxX = comp.maxY = INT32_MIN;
                comp.firstX = comp.firstY = INT32_MAX;
                comp.latin = NONE;
                comps.push(comp);
            }
            compOfLine[i] = compOfRoot[root];
        }
        compOfRoot.release();   // reused below as the map from a component to its sorted cluster
        if (comps.failed() || compOfLine.failed()) {
            return fail("no memory for the street clusters");
        }
        // Their sums, boxes and smallest first vertex
        if (!inA.open(files, file("l.tmp"))) {
            return fail("cannot read the lines");
        }
        for (uint32_t i = 0; i < lineTotal; i++) {
            uint32_t nameId = 0;
            uint32_t pathClass = 0;
            bool hasLatin = false;
            if (!readLine(inA, nameId, pathClass, hasLatin, points)) {
                return fail("cannot read line %u", i);
            }
            Comp& comp = comps[compOfLine[i]];
            const int32_t fx = points[0];
            const int32_t fy = points[1];
            if (fx < comp.firstX || (fx == comp.firstX && fy < comp.firstY)) {
                comp.firstX = fx;
                comp.firstY = fy;
            }
            if (comp.latin == NONE && hasLatin) {
                comp.latin = addExtra(textC);
            }
            for (size_t k = 0; k < points.size() / 2; k++) {
                const int32_t x = points[2 * k];
                const int32_t y = points[2 * k + 1];
                comp.sumX += x;
                comp.sumY += y;
                comp.count++;
                comp.minX = std::min(comp.minX, x);
                comp.maxX = std::max(comp.maxX, x);
                comp.minY = std::min(comp.minY, y);
                comp.maxY = std::max(comp.maxY, y);
            }
        }
        inA.close();
        // The representative vertex: the one nearest the centroid (ties: the smallest x, then y)
        if (!inA.open(files, file("l.tmp"))) {
            return fail("cannot read the lines");
        }
        for (uint32_t i = 0; i < lineTotal; i++) {
            uint32_t nameId = 0;
            uint32_t pathClass = 0;
            bool hasLatin = false;
            if (!readLine(inA, nameId, pathClass, hasLatin, points)) {
                return fail("cannot read line %u", i);
            }
            Comp& comp = comps[compOfLine[i]];
            const double cx = static_cast<double>(comp.sumX) / static_cast<double>(comp.count);
            const double cy = static_cast<double>(comp.sumY) / static_cast<double>(comp.count);
            for (size_t k = 0; k < points.size() / 2; k++) {
                const int32_t x = points[2 * k];
                const int32_t y = points[2 * k + 1];
                const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (!comp.haveRep || d < comp.best || (d == comp.best && (x < comp.repX || (x == comp.repX && y < comp.repY)))) {
                    comp.haveRep = true;
                    comp.best = d;
                    comp.repX = x;
                    comp.repY = y;
                }
            }
        }
        inA.close();
        lineName.release();
        // Cluster order: name (bytes), then the smallest first vertex; the compact clusters in that order
        GrowArray<uint32_t> order;
        if (!order.reserve(comps.size()) || !clusters.reserve(comps.size())) {
            return fail("no memory for the street clusters");
        }
        for (uint32_t c = 0; c < comps.size(); c++) {
            order.push(c);
        }
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const Comp& x = comps[a];
            const Comp& y = comps[b];
            if (x.nameId != y.nameId) {
                return strcmp(names.at(x.nameId), names.at(y.nameId)) < 0;
            }
            if (x.firstX != y.firstX) return x.firstX < y.firstX;
            return x.firstY < y.firstY;
        });
        nameStreet.resize(names.count());
        for (uint32_t n = 0; n < names.count(); n++) {
            nameStreet[n] = NONE;
        }
        for (uint32_t k = 0; k < order.size(); k++) {
            const Comp& comp = comps[order[k]];
            clusters.push({ comp.repX, comp.repY, comp.minX, comp.minY, comp.maxX, comp.maxY, 0, 0 });
            include(comp.repX, comp.repY);
            include(comp.minX, comp.minY);
            include(comp.maxX, comp.maxY);
            if (streets.size() == 0 || streets[streets.size() - 1].nameId != comp.nameId) {
                StreetInfo street = {};
                street.nameId = comp.nameId;
                street.latin = NONE;
                street.en = NONE;
                street.firstCluster = k;
                nameStreet[comp.nameId] = static_cast<uint32_t>(streets.size());
                streets.push(street);
            }
            StreetInfo& street = streets[streets.size() - 1];
            street.clusterCount++;
            if (street.latin == NONE && comp.latin != NONE) {
                street.latin = comp.latin;
            }
        }
        // Lines point at the sorted clusters (the order array becomes the inverse map)
        compOfRoot.resize(comps.size());
        for (uint32_t k = 0; k < order.size(); k++) {
            compOfRoot[order[k]] = k;
        }
        for (uint32_t i = 0; i < lineTotal; i++) {
            lineCluster[i] = compOfRoot[compOfLine[i]];
        }
        compOfRoot.release();
        compOfLine.release();
        order.release();
        st->region.streetNames = static_cast<uint32_t>(streets.size());
        st->region.clusters = static_cast<uint32_t>(comps.size());
        comps.release();
        noteTables();
        st->phaseMs[2] = nowMs() - started;
        step();
        return !streets.failed() && !nameStreet.failed() && !extraText.failed() ? true : fail("no memory for the streets");
    }

    // ------------------------------------------------------------ 4. matching

    /** Reads the house numbers of a tile: texts into tileText, positions into houseXY. */
    bool readHouses(uint32_t t, bool keepText) {
        houseXY.clear();
        tileText.clear();
        tileTextAt.clear();
        const TileInfo& tile = tiles[t];
        if (tile.houseCount == 0) {
            return true;
        }
        if (!inB.seek(tile.houseOffset)) {
            return false;
        }
        for (uint32_t k = 0; k < tile.houseCount; k++) {
            int32_t x = 0;
            int32_t y = 0;
            if (!inB.text(textA, sizeof(textA)) || !inB.i32(x) || !inB.i32(y)) {
                return false;
            }
            houseXY.push(x);
            houseXY.push(y);
            if (keepText) {
                tileTextAt.push(static_cast<uint32_t>(tileText.size()));
                for (const char* c = textA;; c++) {
                    tileText.push(*c);
                    if (*c == '\0') {
                        break;
                    }
                }
            } else {
                // Only the vote number is needed: keep it in place of the text
                tileTextAt.push(static_cast<uint32_t>(houseVoteNumber(textA, slashSecond && config.voteSlashSecond)));
            }
        }
        return !houseXY.failed() && !tileText.failed() && !tileTextAt.failed();
    }

    /** Adds the lines of the tile and its neighbours that SearchRegionBuilder would add, in line order (clipped by the matcher). */
    bool addTileLines(const TileInfo& tile, int32_t lineBuffer) {
        // Each tile's lines are one range of l.tmp, in line order
        uint32_t ranges[9];
        int rangeCount = 0;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                const uint32_t n = tileAt(tile.x + dx, tile.y + dy);
                if (n != NONE && tiles[n].lineCount > 0) {
                    ranges[rangeCount++] = n;
                }
            }
        }
        // Insertion sort: GCC's array-bounds check misreads std::sort on this small array
        for (int i = 1; i < rangeCount; i++) {
            const uint32_t value = ranges[i];
            int j = i;
            while (j > 0 && tiles[ranges[j - 1]].lineFirst > tiles[value].lineFirst) {
                ranges[j] = ranges[j - 1];
                j--;
            }
            ranges[j] = value;
        }
        for (int r = 0; r < rangeCount; r++) {
            const TileInfo& from = tiles[ranges[r]];
            if (!inA.seek(from.lineOffset)) {
                return fail("cannot read the lines");
            }
            for (uint32_t k = 0; k < from.lineCount; k++) {
                uint32_t nameId = 0;
                uint32_t pathClass = 0;
                bool hasLatin = false;
                if (!readLine(inA, nameId, pathClass, hasLatin, points)) {
                    return fail("cannot read the lines");
                }
                if (pathClass != 0 && !config.matchPathLines) {
                    continue;
                }
                int32_t lx0 = INT32_MAX, ly0 = INT32_MAX, lx1 = INT32_MIN, ly1 = INT32_MIN;
                for (size_t p = 0; p < points.size() / 2; p++) {
                    lx0 = std::min(lx0, points[2 * p]);
                    lx1 = std::max(lx1, points[2 * p]);
                    ly0 = std::min(ly0, points[2 * p + 1]);
                    ly1 = std::max(ly1, points[2 * p + 1]);
                }
                // SearchRegionBuilder's test: the tiles the line's box (plus the buffer) touches include this one
                if (floorDiv(lx0 - lineBuffer, TILE_UNITS) > tile.x || floorDiv(lx1 + lineBuffer, TILE_UNITS) < tile.x ||
                    floorDiv(ly0 - lineBuffer, TILE_UNITS) > tile.y || floorDiv(ly1 + lineBuffer, TILE_UNITS) < tile.y) {
                    continue;
                }
                matcher.addLine(lineCluster[from.lineFirst + k], points.begin(), points.size() / 2);
            }
        }
        return true;
    }

    /**
     * House numbers to streets and settlements, tile by tile (SearchRegionBuilder's rules and results). A dense tile is
     * matched in blocks to keep the matcher small: a block's houses with the lines within reach and, as voters, every
     * other house within VOTE_M of the block. A result depends only on segments within LOW_STREET_M of the house and on
     * voters within VOTE_M (whose own nearest streets depend on segments within LOW_STREET_M of them), so blocks give
     * the same results as the whole tile at once; the addresses are written in the tile's own order afterwards.
     */
    bool matchHouses() {
        const uint32_t started = nowMs();
        RegionStats& rs = st->region;
        const auto lineBuffer = static_cast<int32_t>(LINE_BUFFER_M / regionMpu) + 1;
        if (!inA.open(files, file("l.tmp")) || !inB.open(files, file("i.tmp")) || !outA.open(files, file("a.tmp"))) {
            return fail("cannot open the matching files");
        }
        GrowArray<int32_t> ownXY;
        GrowArray<int32_t> ownVote;
        GrowArray<uint8_t> ownPlain;
        GrowArray<OwnResult> ownResult;
        GrowArray<int32_t> voterXY;
        GrowArray<int32_t> voterVote;
        GrowArray<uint32_t> members;
        for (uint32_t t = 0; t < tiles.size(); t++) {
            if (!keepGoing()) {
                return false;
            }
            const TileInfo& tile = tiles[t];
            step();
            if (tile.houseCount == 0) {
                continue;
            }
            const uint32_t tileStart = nowMs();
            const double mpu = metresPerUnitAtTileY(tile.y + 0.5);
            const int32_t rx0 = tile.x * TILE_UNITS;
            const int32_t ry0 = tile.y * TILE_UNITS;
            const int32_t rx1 = rx0 + TILE_UNITS - 1;
            const int32_t ry1 = ry0 + TILE_UNITS - 1;
            const auto voteBuffer = static_cast<int32_t>(AddressMatcher::VOTE_M / mpu) + 1;
            const auto reach = static_cast<int32_t>((AddressMatcher::VOTE_M + AddressMatcher::LOW_STREET_M + 20.0) / mpu) + 2;
            // The tile's own houses (position, vote number, village style), and the neighbours' near the tile that vote
            const uint32_t own = tile.houseCount;
            ownXY.clear();
            ownVote.clear();
            ownPlain.clear();
            if (!ownXY.reserve(2 * static_cast<size_t>(own)) || !ownVote.reserve(own) || !ownPlain.reserve(own) || !inB.seek(tile.houseOffset)) {
                return fail("no memory for the house matching");
            }
            for (uint32_t k = 0; k < own; k++) {
                int32_t x = 0;
                int32_t y = 0;
                if (!inB.text(textA, sizeof(textA)) || !inB.i32(x) || !inB.i32(y)) {
                    return fail("cannot read the house numbers");
                }
                ownXY.push(x);
                ownXY.push(y);
                ownVote.push(houseVoteNumber(textA, slashSecond && config.voteSlashSecond));
                ownPlain.push(houseNumberStyle(textA).plain ? 1 : 0);
            }
            voterXY.clear();
            voterVote.clear();
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const uint32_t n = tileAt(tile.x + dx, tile.y + dy);
                    if (n == NONE || tiles[n].houseCount == 0) {
                        continue;
                    }
                    if (!inB.seek(tiles[n].houseOffset)) {
                        return fail("cannot read the house numbers");
                    }
                    for (uint32_t k = 0; k < tiles[n].houseCount; k++) {
                        int32_t x = 0;
                        int32_t y = 0;
                        if (!inB.text(textA, sizeof(textA)) || !inB.i32(x) || !inB.i32(y)) {
                            return fail("cannot read the house numbers");
                        }
                        if (x >= rx0 - voteBuffer && x <= rx1 + voteBuffer && y >= ry0 - voteBuffer && y <= ry1 + voteBuffer) {
                            voterXY.push(x);
                            voterXY.push(y);
                            voterVote.push(houseVoteNumber(textA, slashSecond && config.voteSlashSecond));
                        }
                    }
                }
            }
            if (!ownResult.resize(own) || ownXY.failed() || ownVote.failed() || ownPlain.failed() || voterXY.failed() || voterVote.failed()) {
                return fail("no memory for the house matching");
            }
            // Blocks per side: one for most tiles, more for the dense ones (each block reads the lines again)
            const int32_t blocks = own <= 200 ? 1 : (own <= 800 ? 2 : (own <= 1800 ? 3 : 4));
            const int32_t blockUnits = (TILE_UNITS + blocks - 1) / blocks;
            for (int32_t by = 0; by < blocks; by++) {
                for (int32_t bx = 0; bx < blocks; bx++) {
                    // Members: own houses whose position (held to the tile) falls in the block, in the tile's order
                    members.clear();
                    int32_t mx0 = INT32_MAX, my0 = INT32_MAX, mx1 = INT32_MIN, my1 = INT32_MIN;
                    for (uint32_t k = 0; k < own; k++) {
                        const int32_t x = ownXY[2 * k];
                        const int32_t y = ownXY[2 * k + 1];
                        const int32_t cx = std::min(std::max(x, rx0), rx1) - rx0;
                        const int32_t cy = std::min(std::max(y, ry0), ry1) - ry0;
                        if (cx / blockUnits != bx || cy / blockUnits != by) {
                            continue;
                        }
                        members.push(k);
                        mx0 = std::min(mx0, x);
                        mx1 = std::max(mx1, x);
                        my0 = std::min(my0, y);
                        my1 = std::max(my1, y);
                    }
                    if (members.size() == 0) {
                        continue;
                    }
                    matcher.begin(mpu);
                    if (blocks == 1) {
                        matcher.clipToTile(tile.x, tile.y);
                    } else {
                        matcher.clip(mx0 - reach, my0 - reach, mx1 + reach, my1 + reach);
                    }
                    if (!addTileLines(tile, lineBuffer)) {
                        return false;
                    }
                    if (villageMode) {
                        const auto placeBuffer = static_cast<int32_t>(AddressMatcher::PLACE_M / mpu) + 1;
                        for (uint32_t s = 0; s < settlements.size(); s++) {
                            const SettlementInfo& p = settlements[s];
                            if (p.x >= rx0 - placeBuffer && p.x <= rx1 + placeBuffer && p.y >= ry0 - placeBuffer && p.y <= ry1 + placeBuffer) {
                                matcher.addPlace(s, static_cast<PlaceClass>(p.placeClass), p.x, p.y);
                            }
                        }
                    }
                    for (uint32_t m : members) {
                        matcher.addHouse(ownXY[2 * m], ownXY[2 * m + 1], ownVote[m], villageMode && ownPlain[m] != 0);
                    }
                    // Voters: the tile's other houses and the neighbours' near this block (all of them for one block)
                    auto near = [&](int32_t x, int32_t y) {
                        return blocks == 1 || (x >= mx0 - voteBuffer && x <= mx1 + voteBuffer && y >= my0 - voteBuffer && y <= my1 + voteBuffer);
                    };
                    if (blocks > 1) {
                        size_t next = 0;
                        for (uint32_t k = 0; k < own; k++) {
                            if (next < members.size() && members[next] == k) {
                                next++;
                                continue;
                            }
                            if (near(ownXY[2 * k], ownXY[2 * k + 1])) {
                                matcher.addHouse(ownXY[2 * k], ownXY[2 * k + 1], ownVote[k], false);
                            }
                        }
                    }
                    for (size_t k = 0; k < voterVote.size(); k++) {
                        if (near(voterXY[2 * k], voterXY[2 * k + 1])) {
                            matcher.addHouse(voterXY[2 * k], voterXY[2 * k + 1], voterVote[k], false);
                        }
                    }
                    if (!matcher.run()) {
                        return fail("no memory for the house matching");
                    }
                    for (size_t q = 0; q < members.size(); q++) {
                        const AddressMatch& m = matcher.result(static_cast<uint32_t>(q));
                        OwnResult& r = ownResult[members[q]];
                        r.cluster = m.tier == AddressTier::None ? NONE : m.street;
                        r.village = (villageMode && ownPlain[members[q]] != 0 && m.villages[0] != NONE) ? m.villages[0] : NONE;
                        r.tier = static_cast<uint8_t>(m.tier);
                    }
                }
            }
            // The addresses in the tile's own order, with the texts read again
            if (!inB.seek(tile.houseOffset)) {
                return fail("cannot read the house numbers");
            }
            for (uint32_t k = 0; k < own; k++) {
                int32_t x = 0;
                int32_t y = 0;
                if (!inB.text(textA, sizeof(textA)) || !inB.i32(x) || !inB.i32(y)) {
                    return fail("cannot read the house numbers");
                }
                const char* text = textA;
                const OwnResult& m = ownResult[k];
                const uint32_t cluster = m.cluster;
                const uint32_t village = m.village;
                if (cluster != NONE) {
                    if (!writeAddress(cluster, m.tier == static_cast<uint8_t>(AddressTier::LowStreet) ? ADDRESS_LOW : 0, x, y, text)) {
                        return false;
                    }
                    if (m.tier == static_cast<uint8_t>(AddressTier::Street)) {
                        rs.pathStreet50++;
                    } else {
                        rs.pathStreet100++;
                    }
                    if (village != NONE) {
                        rs.alsoVillage++;
                    }
                } else if (village != NONE) {
                    rs.pathVillage++;
                } else {
                    rs.pathNone++;
                }
                if (village != NONE) {
                    SettlementInfo& s = settlements[village];
                    if (s.pseudo == NONE) {
                        uint32_t street = nameStreet[s.nameId];
                        if (street == NONE) {
                            StreetInfo created = {};
                            created.nameId = s.nameId;
                            created.latin = s.latin;
                            created.en = s.en;
                            created.useEn = 1;
                            created.firstCluster = NONE;
                            street = static_cast<uint32_t>(streets.size());
                            nameStreet[s.nameId] = street;
                            streets.push(created);
                        }
                        s.pseudo = static_cast<uint32_t>(pseudos.size());
                        pseudos.push({ village, street, 0, 0 });
                        streets[street].pseudoCount++;
                    }
                    if (!writeAddress(PSEUDO | s.pseudo, ADDRESS_VILLAGE, x, y, text)) {
                        return false;
                    }
                }
            }
            const uint32_t ms = nowMs() - tileStart;
            if (ms > st->slowestMatchMs) {
                st->slowestMatchMs = ms;
                st->slowestMatchTile = { tile.x, tile.y };
            }
            st->matcherPeakBytes = std::max(st->matcherPeakBytes, matcher.allocated());
        }
        matcher.release();
        inA.close();
        inB.close();
        if (!outA.close()) {
            return fail("cannot write the addresses");
        }
        st->scratchBytes += outA.size();
        rs.pseudoStreets = static_cast<uint32_t>(pseudos.size());
        noteTables();
        st->phaseMs[3] = nowMs() - started;
        return !streets.failed() && !pseudos.failed() ? true : fail("no memory for the village streets");
    }

    bool writeAddress(uint32_t cluster, uint32_t flags, int32_t x, int32_t y, const char* text) {
        rec.clear();
        rec.u32(cluster);
        rec.u8(flags);
        rec.i32(x);
        rec.i32(y);
        rec.text(text);
        return outA.write(rec) ? true : fail("cannot write the addresses");
    }

    // ------------------------------------------------------------ 5. streets and addresses

    /** Street records in name group order: per street (sort key, then name order) its clusters, then its villages. */
    bool orderStreets() {
        GrowArray<uint32_t> byName;
        for (uint32_t s = 0; s < streets.size(); s++) {
            byName.push(s);
        }
        std::sort(byName.begin(), byName.end(), [&](uint32_t a, uint32_t b) { return strcmp(names.at(streets[a].nameId), names.at(streets[b].nameId)) < 0; });
        for (uint32_t k = 0; k < byName.size(); k++) {
            StreetInfo& s = streets[byName[k]];
            s.order = k;
            KeyMaker::sortKeyOf(names.at(s.nameId), config.profile, keyScratch, sizeof(keyScratch));
            s.sortKey = static_cast<uint32_t>(sortKeys.size());
            for (const char* c = keyScratch;; c++) {
                sortKeys.push(*c);
                if (*c == '\0') {
                    break;
                }
            }
        }
        streetNgOrder.clear();
        for (uint32_t k = 0; k < byName.size(); k++) {
            streetNgOrder.push(byName[k]);
        }
        std::sort(streetNgOrder.begin(), streetNgOrder.end(), [&](uint32_t a, uint32_t b) {
            const int c = compareText(&sortKeys[streets[a].sortKey], &sortKeys[streets[b].sortKey]);
            return c != 0 ? c < 0 : streets[a].order < streets[b].order;
        });
        // The villages of a street in the order of their settlements
        GrowArray<uint32_t> pseudoOrder;
        for (uint32_t p = 0; p < pseudos.size(); p++) {
            pseudoOrder.push(p);
        }
        std::sort(pseudoOrder.begin(), pseudoOrder.end(), [&](uint32_t a, uint32_t b) {
            if (pseudos[a].street != pseudos[b].street) return pseudos[a].street < pseudos[b].street;
            return pseudos[a].settlement < pseudos[b].settlement;
        });
        GrowArray<uint32_t> pseudoFirst;
        pseudoFirst.resize(streets.size());
        for (auto k = static_cast<uint32_t>(pseudoOrder.size()); k > 0; k--) {
            pseudoFirst[pseudos[pseudoOrder[k - 1]].street] = k - 1;
        }
        uint32_t record = 0;
        for (uint32_t street : streetNgOrder) {
            StreetInfo& s = streets[street];
            if (s.clusterCount + s.pseudoCount > 0xFFFF) {
                return fail("a street has %u clusters, the limit is 65535", s.clusterCount + s.pseudoCount);
            }
            s.firstRecord = record;
            for (uint32_t c = 0; c < s.clusterCount; c++) {
                clusters[s.firstCluster + c].finalIndex = record++;
            }
            for (uint32_t p = 0; p < s.pseudoCount; p++) {
                pseudos[pseudoOrder[pseudoFirst[street] + p]].finalIndex = record++;
            }
        }
        streetRecordTotal = record;
        noteTables();
        return !streetNgOrder.failed() && !sortKeys.failed() ? true : fail("no memory for the street order");
    }

    /** The addresses by street record (capped at 65535 each), then sorted by number: a.tmp -> d.tmp, o.tmp. */
    bool makeAddresses() {
        const uint32_t started = nowMs();
        RegionStats& rs = st->region;
        if (!orderStreets() || !sortBegin(sortA, "s_a1", compareAddressSeq) || !inA.open(files, file("a.tmp"))) {
            return fail("cannot read the addresses");
        }
        uint32_t seq = 0;
        while (!inA.atEnd()) {
            uint32_t cluster = 0;
            uint32_t flags = 0;
            int32_t x = 0;
            int32_t y = 0;
            if (!inA.u32(cluster) || !inA.u8(flags) || !inA.i32(x) || !inA.i32(y) || !inA.text(textA, sizeof(textA))) {
                return fail("cannot read the addresses");
            }
            uint32_t& count = (cluster & PSEUDO) != 0 ? pseudos[cluster & ~PSEUDO].addresses : clusters[cluster].addresses;
            const uint32_t final = (cluster & PSEUDO) != 0 ? pseudos[cluster & ~PSEUDO].finalIndex : clusters[cluster].finalIndex;
            if (count >= MAX_CLUSTER_ADDRESSES) {
                rs.droppedAddresses++;
                continue;
            }
            count++;
            include(x, y);
            rec.clear();
            rec.u32(final);
            rec.u32(seq++);
            rec.u8(flags);
            rec.i32(x);
            rec.i32(y);
            rec.text(textA);
            if (!sortAdd(sortA) || (seq % CHECK_EVERY == 0 && !keepGoing())) {
                return false;
            }
        }
        inA.close();
        addressTotal = seq;
        rs.addresses = seq;
        if (!sortA.finish()) {
            return fail(sortA.errorText());
        }
        // Numbers as the records store them; text numbers into the overflow strings in this order (the writer's)
        if (!sortBegin(sortB, "s_a2", compareAddressNumber) || !outA.open(files, file("o.tmp"))) {
            return fail("cannot start the address sort");
        }
        HashIndex overflow;
        GrowArray<uint64_t> overflowHashes;
        GrowArray<uint32_t> overflowOffsets;
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        while (sortA.next(data, size)) {
            Cursor c(data, size);
            const uint32_t final = c.u32();
            const uint32_t order = c.u32();
            const uint32_t flags = c.u8();
            const int32_t x = c.i32();
            const int32_t y = c.i32();
            const char* text = c.text();
            uint16_t number = 0;
            uint16_t aux = 0;
            uint8_t suffix = 0;
            uint8_t numberFlags = 0;
            if (!indexHouseNumber(text, slashSecond, number, aux, suffix, numberFlags)) {
                const uint64_t hash = mix(hashText(SEED, text));
                uint32_t at = overflow.find(hash, [&](uint32_t k) { return overflowHashes[k] == hash; });
                uint32_t offset = 0;
                if (at != HashIndex::NONE) {
                    offset = overflowOffsets[at];
                } else {
                    offset = overflowBytes;
                    overflow.insert(hash, static_cast<uint32_t>(overflowHashes.size()));
                    overflowHashes.push(hash);
                    overflowOffsets.push(offset);
                    const auto length = static_cast<uint32_t>(strlen(text));
                    if (!outA.write(text, length + 1)) {
                        return fail("cannot write the overflow strings");
                    }
                    overflowBytes += length + 1;
                    overflowCount++;
                }
                if (offset >= SIDX_MAX_OFFSET24) {
                    return fail("the overflow strings pass 16 MB");
                }
                number = 0xFFFF;
                aux = static_cast<uint16_t>(offset & 0xFFFF);
                suffix = static_cast<uint8_t>(offset >> 16);
                numberFlags = HOUSE_TEXT;
            }
            rec.clear();
            rec.u32(final);
            rec.u32(number);
            rec.u32(aux);
            rec.u32(suffix);
            rec.u32(order);
            rec.u8(numberFlags | flags);
            rec.i32(x);
            rec.i32(y);
            if (!sortAdd(sortB)) {
                return false;
            }
        }
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        noteSort(sortA);
        sortA.end();
        overflow.release();
        if (!outA.close() || !sortB.finish()) {
            return fail("cannot write the overflow strings");
        }
        st->scratchBytes += outA.size();
        st->phaseMs[4] = nowMs() - started;
        step();
        return true;
    }

    /** The address and street records, once the origin is known: d.tmp and t.tmp. */
    bool writeAddressRecords(uint32_t pts) {
        if (!outA.open(files, file("d.tmp"))) {
            return fail("cannot create the address records");
        }
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        uint8_t bytes[32];
        uint32_t n = 0;
        while (sortB.next(data, size)) {
            Cursor c(data, size);
            c.u32();
            const uint32_t number = c.u32();
            const uint32_t aux = c.u32();
            const uint32_t suffix = c.u32();
            c.u32();
            const uint32_t flags = c.u8();
            const int32_t x = c.i32();
            const int32_t y = c.i32();
            const uint32_t length = encodeAddress(bytes, pts, static_cast<uint16_t>(number), static_cast<uint16_t>(aux), static_cast<uint8_t>(suffix),
                                                  static_cast<uint8_t>(flags), x - minX, y - minY);
            if (!outA.write(bytes, length)) {
                return fail("cannot write the address records");
            }
            if (++n % CHECK_EVERY == 0 && !keepGoing()) {
                return false;
            }
        }
        if (sortB.failed()) {
            return fail(sortB.errorText());
        }
        noteSort(sortB);
        sortB.end();
        if (!outA.close() || !outB.open(files, file("t.tmp"))) {
            return fail("cannot write the address records");
        }
        st->scratchBytes += outA.size();
        uint32_t firstAddress = 0;
        for (uint32_t street : streetNgOrder) {
            const StreetInfo& s = streets[street];
            auto put = [&](int32_t rx, int32_t ry, int32_t lx, int32_t ly, int32_t hx, int32_t hy, uint32_t count) {
                const int32_t rep[2] = { rx - minX, ry - minY };
                const int32_t lo[2] = { lx - minX, ly - minY };
                const int32_t hi[2] = { hx - minX, hy - minY };
                const uint32_t length = encodeStreet(bytes, pts, rep, lo, hi, firstAddress, static_cast<uint16_t>(count));
                firstAddress += count;
                return outB.write(bytes, length);
            };
            for (uint32_t c = 0; c < s.clusterCount; c++) {
                const ClusterInfo& comp = clusters[s.firstCluster + c];
                if (!put(comp.repX, comp.repY, comp.minX, comp.minY, comp.maxX, comp.maxY, comp.addresses)) {
                    return fail("cannot write the street records");
                }
            }
            // The villages, in their final order (finalIndex follows the street's real clusters)
            for (uint32_t k = 0; k < s.pseudoCount; k++) {
                const uint32_t wanted = s.firstRecord + s.clusterCount + k;
                for (const PseudoInfo& p : pseudos) {
                    if (p.street == street && p.finalIndex == wanted) {
                        const SettlementInfo& place = settlements[p.settlement];
                        if (!put(place.x, place.y, place.x, place.y, place.x, place.y, p.addresses)) {
                            return fail("cannot write the street records");
                        }
                        break;
                    }
                }
            }
        }
        if (!outB.close()) {
            return fail("cannot write the street records");
        }
        st->scratchBytes += outB.size();
        return true;
    }

    // ------------------------------------------------------------ 6. name groups, point records, keys

    bool pointGroups() {
        const uint32_t started = nowMs();
        // Named points by (name, pair, seq); unnamed ones straight to their record list
        if (!sortBegin(sortA, "s_grp", compareGroups) || !inA.open(files, file("p.tmp")) || !outA.open(files, file("u.tmp"))) {
            return fail("cannot read the points");
        }
        for (uint32_t i = 0; i < pointCount; i++) {
            uint32_t label = 0, named = 0, hasLatin = 0, hasEn = 0, useEn = 0, seq = 0;
            int32_t x = 0, y = 0;
            if (!inA.u32(label) || !inA.u8(named) || !inA.text(textA, sizeof(textA)) || !inA.text(textB, sizeof(textB)) || !inA.u8(hasLatin) ||
                !inA.text(textC, sizeof(textC)) || !inA.u8(hasEn) || !inA.u8(useEn) || !inA.i32(x) || !inA.i32(y) || !inA.u32(seq)) {
                return fail("cannot read the points");
            }
            const uint32_t pair = labels[label].pair;
            rec.clear();
            if (named == 0) {
                rec.u8(pair);
                rec.i32(x);
                rec.i32(y);
                if (!outA.write(rec)) {
                    return fail("cannot write the points");
                }
                continue;
            }
            rec.text(textA);
            rec.u8(pair);
            rec.u32(seq);
            rec.i32(x);
            rec.i32(y);
            rec.text(textB);
            rec.u8(hasLatin);
            rec.text(textC);
            rec.u8(hasEn);
            rec.u8(useEn);
            rec.u8(labels[label].category);
            if (!sortAdd(sortA) || (i % CHECK_EVERY == 0 && !keepGoing())) {
                return false;
            }
        }
        inA.close();
        if (!outA.close() || !sortA.finish()) {
            return fail("cannot sort the points");
        }
        st->scratchBytes += outA.size();
        // Groups (name, pair): the head (first seen) gives the group its keys; members listed by group number
        if (!sortBegin(sortB, "s_ng", compareNameGroups) || !outA.open(files, file("m.tmp"))) {
            return fail("cannot start the name group sort");
        }
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        uint32_t group = NONE;
        uint32_t lastPair = NONE;
        while (sortA.next(data, size)) {
            Cursor c(data, size);
            const char* name = c.text();
            const uint32_t pair = c.u8();
            const uint32_t seq = c.u32();
            const int32_t x = c.i32();
            const int32_t y = c.i32();
            if (group == NONE || pair != lastPair || strcmp(name, textA) != 0) {
                group = group == NONE ? 0 : group + 1;
                lastPair = pair;
                snprintf(textA, sizeof(textA), "%s", name);
                const char* latin = c.text();
                const uint32_t hasLatin = c.u8();
                const char* en = c.text();
                const uint32_t hasEn = c.u8();
                const uint32_t useEn = c.u8();
                const uint32_t category = c.u8();
                KeyMaker::sortKeyOf(name, config.profile, keyScratch, sizeof(keyScratch));
                rec.clear();
                rec.text(keyScratch);
                rec.u8(pair);
                rec.u32(seq);
                rec.u32(group);
                rec.text(name);
                rec.text(latin);
                rec.u8(hasLatin);
                rec.text(en);
                rec.u8(hasEn);
                rec.u8(useEn);
                rec.u8(category);
                if (!sortAdd(sortB)) {
                    return false;
                }
            }
            rec.clear();
            rec.u32(group);
            rec.u8(pair);
            rec.i32(x);
            rec.i32(y);
            if (!outA.write(rec)) {
                return fail("cannot write the group members");
            }
        }
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        noteSort(sortA);
        sortA.end();
        if (!outA.close() || !sortB.finish()) {
            return fail("cannot sort the name groups");
        }
        st->scratchBytes += outA.size();
        // Ranks and offsets of the point groups in the writer's order; n.tmp keeps them in that order
        if (!sortBegin(sortA, "s_g2r", compareFirst) || !outA.open(files, file("n.tmp"))) {
            return fail("cannot start the rank sort");
        }
        uint32_t rank = 0;
        uint32_t offset = 0;
        while (sortB.next(data, size)) {
            Cursor c(data, size);
            c.text();
            c.u8();
            c.u32();
            const uint32_t g = c.u32();
            const char* name = c.text();
            const char* latin = c.text();
            const uint32_t hasLatin = c.u8();
            const char* en = c.text();
            const uint32_t hasEn = c.u8();
            const uint32_t useEn = c.u8();
            const uint32_t category = c.u8();
            rec.clear();
            rec.u32(g);
            rec.u32(rank);
            rec.u32(offset);
            if (!sortAdd(sortA)) {
                return false;
            }
            rec.clear();
            rec.text(name);
            rec.text(latin);
            rec.u8(hasLatin);
            rec.text(en);
            rec.u8(hasEn);
            rec.u8(useEn);
            rec.u8(category);
            if (!outA.write(rec)) {
                return fail("cannot write the name groups");
            }
            offset += 1 + truncateName(name, strlen(name)) + 1 + 4 + 2;
            rank++;
        }
        if (sortB.failed()) {
            return fail(sortB.errorText());
        }
        noteSort(sortB);
        sortB.end();
        pointNgCount = rank;
        pointNgBytes = offset;
        if (!outA.close() || !sortA.finish()) {
            return fail("cannot sort the ranks");
        }
        st->scratchBytes += outA.size();
        // Point records: members get their group's rank and offset (both lists in group order)
        if (!sortBegin(sortB, "s_rec", comparePointRecords) || !inA.open(files, file("m.tmp"))) {
            return fail("cannot start the point record sort");
        }
        uint32_t currentGroup = NONE;
        uint32_t currentRank = 0;
        uint32_t currentOffset = 0;
        for (uint32_t i = 0; i < namedCount; i++) {
            uint32_t g = 0, pair = 0;
            int32_t x = 0, y = 0;
            if (!inA.u32(g) || !inA.u8(pair) || !inA.i32(x) || !inA.i32(y)) {
                return fail("cannot read the group members");
            }
            while (currentGroup != g) {
                if (!sortA.next(data, size)) {
                    return fail("the group ranks are short");
                }
                currentGroup = get32(data);
                currentRank = get32(data + 4);
                currentOffset = get32(data + 8);
            }
            if (!addPointRecord(pair, 0, currentRank, x, y, currentOffset)) {
                return false;
            }
        }
        inA.close();
        noteSort(sortA);
        sortA.end();
        if (!inA.open(files, file("u.tmp"))) {
            return fail("cannot read the unnamed points");
        }
        for (uint32_t i = namedCount; i < pointCount; i++) {
            uint32_t pair = 0;
            int32_t x = 0, y = 0;
            if (!inA.u8(pair) || !inA.i32(x) || !inA.i32(y)) {
                return fail("cannot read the unnamed points");
            }
            if (!addPointRecord(pair, 1, 0, x, y, SIDX_UNNAMED)) {
                return false;
            }
        }
        inA.close();
        if (!sortB.finish()) {
            return fail(sortB.errorText());
        }
        st->phaseMs[5] = nowMs() - started;
        step();
        return true;
    }

    bool addPointRecord(uint32_t pair, uint32_t unnamed, uint32_t rank, int32_t x, int32_t y, uint32_t offset) {
        rec.clear();
        rec.u32(pair);
        rec.u32(unnamed);
        rec.u32(rank);
        rec.i32(y >> 12);
        rec.i32(x >> 12);
        rec.i32(x);
        rec.i32(y);
        rec.u32(offset);
        return sortAdd(sortB);
    }

    /** Point records (r.tmp) and the name group area (g.tmp): point groups, streets, categories. */
    bool writeGroups(uint32_t pts) {
        const uint32_t started = nowMs();
        if (!outA.open(files, file("r.tmp")) || !sortBegin(sortA, "s_fc", compareFirst)) {
            return fail("cannot create the point records");
        }
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        uint8_t bytes[SIDX_MAX_NAME + 16];
        uint32_t index = 0;
        uint32_t runRank = NONE;
        uint32_t runFirst = 0;
        uint32_t runCount = 0;
        auto flushRun = [&]() {
            if (runRank == NONE) {
                return true;
            }
            rec.clear();
            rec.u32(runRank);
            rec.u32(runFirst);
            rec.u32(runCount);
            return sortAdd(sortA);
        };
        while (sortB.next(data, size)) {
            const uint32_t pair = get32(data);
            const uint32_t unnamed = get32(data + 4);
            const uint32_t rank = get32(data + 8);
            const auto x = static_cast<int32_t>(get32(data + 20));
            const auto y = static_cast<int32_t>(get32(data + 24));
            const uint32_t offset = get32(data + 28);
            const uint32_t length = encodePoint(bytes, pts, x - minX, y - minY, static_cast<uint8_t>(pair), offset);
            if (!outA.write(bytes, length)) {
                return fail("cannot write the point records");
            }
            if (pairCount[pair] == 0) {
                pairStart[pair] = index;
            }
            pairCount[pair]++;
            if (unnamed == 0) {
                if (rank != runRank) {
                    if (!flushRun()) {
                        return false;
                    }
                    runRank = rank;
                    runFirst = index;
                    runCount = 0;
                }
                runCount++;
            }
            index++;
            if (index % CHECK_EVERY == 0 && !keepGoing()) {
                return false;
            }
        }
        if (sortB.failed() || !flushRun()) {
            return fail(sortB.failed() ? sortB.errorText() : "cannot sort the group runs");
        }
        noteSort(sortB);
        sortB.end();
        if (!outA.close() || !sortA.finish()) {
            return fail("cannot write the point records");
        }
        st->scratchBytes += outA.size();
        // The name group area
        if (!outB.open(files, file("g.tmp")) || !inA.open(files, file("n.tmp"))) {
            return fail("cannot create the name groups");
        }
        uint32_t groupBytes = 0;
        for (uint32_t r = 0; r < pointNgCount; r++) {
            uint32_t hasLatin = 0, hasEn = 0, useEn = 0, category = 0;
            if (!inA.text(textA, sizeof(textA)) || !inA.text(textB, sizeof(textB)) || !inA.u8(hasLatin) || !inA.text(textC, sizeof(textC)) ||
                !inA.u8(hasEn) || !inA.u8(useEn) || !inA.u8(category)) {
                return fail("cannot read the name groups");
            }
            if (!sortA.next(data, size) || get32(data) != r) {
                return fail("the group runs do not match the groups");
            }
            const uint32_t first = get32(data + 4);
            const uint32_t count = get32(data + 8);
            if (count > 0xFFFF) {
                return fail("name group %u has more than 65535 records", r);
            }
            const uint32_t nameLength = truncateName(textA, strlen(textA));
            const uint32_t length = encodeNameGroup(bytes, textA, nameLength, SearchKind::Point, first, static_cast<uint16_t>(count));
            if (!outB.write(bytes, length)) {
                return fail("cannot write the name groups");
            }
            groupBytes += length;
        }
        inA.close();
        noteSort(sortA);
        sortA.end();
        for (uint32_t street : streetNgOrder) {
            const StreetInfo& s = streets[street];
            const char* name = names.at(s.nameId);
            const uint32_t nameLength = truncateName(name, strlen(name));
            const uint32_t length = encodeNameGroup(bytes, name, nameLength, SearchKind::Street, s.firstRecord,
                                                    static_cast<uint16_t>(s.clusterCount + s.pseudoCount));
            if (!outB.write(bytes, length)) {
                return fail("cannot write the name groups");
            }
            groupBytes += length;
        }
        streetNgBytes = groupBytes - pointNgBytes;
        uint32_t k = pointNgCount + static_cast<uint32_t>(streets.size());
        for (const CategoryNg& c : categories) {
            uint32_t count = 0;
            for (uint32_t p = 0; p < c.pairCount; p++) {
                count += pairCount[c.firstPair + p];
            }
            if (count > 0xFFFF) {
                return fail("name group %u has more than 65535 records", k);
            }
            const char* name = extra(c.name);
            const uint32_t nameLength = truncateName(name, strlen(name));
            const uint32_t length = encodeNameGroup(bytes, name, nameLength, SearchKind::Category, pairStart[c.firstPair], static_cast<uint16_t>(count));
            if (!outB.write(bytes, length)) {
                return fail("cannot write the name groups");
            }
            groupBytes += length;
            k++;
        }
        ngTotal = k;
        if (groupBytes >= SIDX_MAX_OFFSET24) {
            return fail("the name groups take %u bytes, more than 16 MB", groupBytes);
        }
        if (!outB.close()) {
            return fail("cannot write the name groups");
        }
        st->scratchBytes += outB.size();
        st->phaseMs[5] += nowMs() - started;
        return true;
    }

    bool addKey(const char* key, uint32_t priority, uint32_t offset) {
        const size_t length = strlen(key);
        if (length == 0) {
            return true;
        }
        const uint32_t cut = truncateKey(key, length);
        rec.clear();
        rec.u8(cut);
        rec.bytes(key, cut);
        rec.u8(priority);
        rec.u32(offset);
        keyCount++;
        return sortAdd(sortA);
    }

    /** Keys of every name group, sorted, into key pages (k.tmp) and the fence table. */
    bool writeKeys() {
        const uint32_t started = nowMs();
        if (!sortBegin(sortA, "s_key", compareKeys) || !inA.open(files, file("n.tmp"))) {
            return fail("cannot start the key sort");
        }
        uint32_t offset = 0;
        for (uint32_t r = 0; r < pointNgCount; r++) {
            uint32_t hasLatin = 0, hasEn = 0, useEn = 0, category = 0;
            if (!inA.text(textA, sizeof(textA)) || !inA.text(textB, sizeof(textB)) || !inA.u8(hasLatin) || !inA.text(textC, sizeof(textC)) ||
                !inA.u8(hasEn) || !inA.u8(useEn) || !inA.u8(category)) {
                return fail("cannot read the name groups");
            }
            if (!keys.make(textA, hasLatin != 0 ? textB : nullptr, hasEn != 0 ? textC : nullptr, useEn != 0, config.profile)) {
                return fail("no memory for the keys");
            }
            for (uint32_t k = 0; k < keys.count(); k++) {
                if (!addKey(keys.key(k), category, offset)) {
                    return false;
                }
            }
            offset += 1 + truncateName(textA, strlen(textA)) + 1 + 4 + 2;
            if (r % CHECK_EVERY == 0 && !keepGoing()) {
                return false;
            }
        }
        inA.close();
        for (uint32_t street : streetNgOrder) {
            const StreetInfo& s = streets[street];
            const char* name = names.at(s.nameId);
            if (!keys.make(name, extra(s.latin), extra(s.en), s.useEn != 0, config.profile)) {
                return fail("no memory for the keys");
            }
            for (uint32_t k = 0; k < keys.count(); k++) {
                if (!addKey(keys.key(k), CAT_STREET, offset)) {
                    return false;
                }
            }
            offset += 1 + truncateName(name, strlen(name)) + 1 + 4 + 2;
        }
        for (const CategoryNg& c : categories) {
            if (!addKey(extra(c.sortKey), CAT_CATEGORY, offset)) {
                return false;
            }
            const char* name = extra(c.name);
            offset += 1 + truncateName(name, strlen(name)) + 1 + 4 + 2;
        }
        keys.release();
        if (!sortA.finish() || !outA.open(files, file("k.tmp"))) {
            return fail("cannot sort the keys");
        }
        uint8_t page[SIDX_PAGE_SIZE];
        uint32_t used = 0;
        memset(page, 0, sizeof(page));
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        while (sortA.next(data, size)) {
            const uint32_t length = data[0];
            const uint8_t* key = data + 1;
            const uint32_t ng = get32(data + 2 + length);
            if (!encodeKeyPage(page, used, key, length, ng)) {
                if (!outA.write(page, SIDX_PAGE_SIZE)) {
                    return fail("cannot write the key pages");
                }
                pageCount++;
                memset(page, 0, sizeof(page));
                used = 0;
                if (!encodeKeyPage(page, used, key, length, ng)) {
                    return fail("a key does not fit a page");
                }
            }
            if (used == 1 + length + 3) {
                fence.push(static_cast<uint8_t>(length));
                for (uint32_t i = 0; i < length; i++) {
                    fence.push(key[i]);
                }
                fenceCount++;
            }
        }
        if (sortA.failed()) {
            return fail(sortA.errorText());
        }
        if (used > 0) {
            if (!outA.write(page, SIDX_PAGE_SIZE)) {
                return fail("cannot write the key pages");
            }
            pageCount++;
        }
        noteSort(sortA);
        sortA.end();
        if (!outA.close() || fence.failed()) {
            return fail("cannot write the key pages");
        }
        st->scratchBytes += outA.size();
        st->phaseMs[6] = nowMs() - started;
        step();
        return true;
    }

    // ------------------------------------------------------------ 7. the file

    bool copyInto(OutFile& out, const char* name, uint32_t expected) {
        if (expected == 0) {
            return true;
        }
        if (!inA.open(files, file(name))) {
            return fail("cannot read a section");
        }
        uint8_t chunk[1024];
        uint32_t left = expected;
        while (left > 0) {
            const uint32_t n = std::min<uint32_t>(left, sizeof(chunk));
            if (!inA.read(chunk, n) || !out.write(chunk, n)) {
                inA.close();
                return fail("cannot copy a section");
            }
            left -= n;
        }
        inA.close();
        return true;
    }

    bool writeFile(const char* outPath, const char* temporaryPath, uint32_t pts) {
        const uint32_t started = nowMs();
        GrowArray<uint8_t> classBlob;
        for (uint32_t pair = 0; pair < pairTotal; pair++) {
            const char* label = pairTexts.at(pair);
            const auto length = static_cast<uint8_t>(strlen(label));
            uint8_t tail[8];
            put32(tail, pairStart[pair]);
            put32(tail + 4, pairCount[pair]);
            classBlob.push(length);
            for (uint32_t i = 0; i < length; i++) {
                classBlob.push(static_cast<uint8_t>(label[i]));
            }
            for (uint8_t b : tail) {
                classBlob.push(b);
            }
        }
        const uint32_t addressBytes = addressTotal * addressRecordBytes(pts);
        const uint32_t sizes[SIDX_SECTION_COUNT] = {
            static_cast<uint32_t>(classBlob.size()),
            static_cast<uint32_t>(fence.size()),
            pageCount * SIDX_PAGE_SIZE,
            pointNgBytes + streetNgBytes + categoryBytes(),
            pointCount * pointRecordBytes(pts),
            streetRecordTotal * streetRecordBytes(pts),
            addressBytes,
            overflowBytes,
        };
        SearchHeader header = {};
        header.pointBytes = static_cast<uint16_t>(pts);
        header.originX = minX;
        header.originY = minY;
        header.classCount = pairTotal;
        header.fenceCount = fenceCount;
        header.pageCount = pageCount;
        header.pointCount = pointCount;
        header.streetCount = streetRecordTotal;
        header.addressCount = addressTotal;
        uint64_t offset = SIDX_HEADER_SIZE;
        for (uint32_t i = 0; i < SIDX_SECTION_COUNT; i++) {
            header.sectionOffset[i] = static_cast<uint32_t>(offset);
            header.sectionSize[i] = sizes[i];
            offset += sizes[i];
        }
        if (offset > 0xFFFFFFFFull) {
            return fail("the index is larger than 4 GB");
        }
        uint8_t headerBytes[SIDX_HEADER_SIZE];
        encodeHeader(header, headerBytes);
        OutFile& out = outB;
        if (!out.open(files, temporaryPath)) {
            return fail("cannot create the index file");
        }
        bool ok = out.write(headerBytes, SIDX_HEADER_SIZE) && out.write(classBlob.begin(), static_cast<uint32_t>(classBlob.size())) &&
            out.write(fence.begin(), static_cast<uint32_t>(fence.size()));
        ok = ok && copyInto(out, "k.tmp", sizes[2]) && copyInto(out, "g.tmp", sizes[3]) && copyInto(out, "r.tmp", sizes[4]) &&
            copyInto(out, "t.tmp", sizes[5]) && copyInto(out, "d.tmp", sizes[6]) && copyInto(out, "o.tmp", sizes[7]);
        ok = out.close() && ok;
        if (!ok) {
            files->remove(temporaryPath);
            return fail("cannot write the index file");
        }
        if (!files->rename(temporaryPath, outPath)) {
            files->remove(temporaryPath);
            return fail("cannot rename the index file");
        }
        SearchBuildStats& is = st->index;
        is.totalBytes = static_cast<uint32_t>(offset);
        is.header = header;
        is.keyCount = keyCount;
        is.ngCount = ngTotal;
        is.namedPoints = namedCount;
        is.unnamedPoints = pointCount - namedCount;
        is.overflowStrings = overflowCount;
        is.collapsedPairs = collapsed;
        is.categoryNotContiguous = notContiguous;
        st->phaseMs[7] = nowMs() - started;
        step();
        return true;
    }

    uint32_t categoryBytes() const {
        uint32_t bytes = 0;
        for (const CategoryNg& c : categories) {
            const char* name = extra(c.name);
            bytes += 1 + truncateName(name, strlen(name)) + 1 + 4 + 2;
        }
        return bytes;
    }

    void removeScratch() {
        static const char* const NAMES[] = { "l.tmp", "h.tmp", "i.tmp", "p.tmp", "a.tmp", "o.tmp", "d.tmp", "t.tmp", "u.tmp",
                                             "m.tmp", "n.tmp", "r.tmp", "g.tmp", "k.tmp" };
        inA.close();
        inB.close();
        outA.close();
        outB.close();
        sortA.end();
        sortB.end();
        for (const char* name : NAMES) {
            files->remove(file(name));
        }
    }
};

SearchStreamBuilder::SearchStreamBuilder() = default;

SearchStreamBuilder::~SearchStreamBuilder() {
    if (impl != nullptr) {
        impl->~Impl();
        largeFree(impl);
    }
}

bool SearchStreamBuilder::build(ScratchFiles& files, const char* scratchDir, const StreamTile* tiles, uint32_t tileCount, StreamTileSource& source,
                                MvtDecoder& decoder, const char* outPath, const char* temporaryPath, const StreamBuildOptions& options) {
    statistics = {};
    errorText[0] = '\0';
    wasCancelled = false;
    if (impl != nullptr) {
        impl->~Impl();
        largeFree(impl);
        impl = nullptr;
    }
    if (tileCount == 0) {
        snprintf(errorText, sizeof(errorText), "no tiles");
        return false;
    }
    void* memory = largeAlloc(sizeof(Impl));
    if (memory == nullptr) {
        snprintf(errorText, sizeof(errorText), "no memory for the builder");
        return false;
    }
    impl = new (memory) Impl();
    Impl& b = *impl;
    b.owner = this;
    b.files = &files;
    b.source = &source;
    b.decoder = &decoder;
    b.options = &options;
    b.config = options.config;
    b.st = &statistics;
    snprintf(b.dir, sizeof(b.dir), "%s", scratchDir);
    int32_t minY = tiles[0].y;
    int32_t maxY = tiles[0].y;
    for (uint32_t i = 0; i < tileCount; i++) {
        TileInfo info = {};
        info.x = tiles[i].x;
        info.y = tiles[i].y;
        b.tileIndex.insert(mix(static_cast<uint64_t>(static_cast<uint32_t>(info.x)) << 32 | static_cast<uint32_t>(info.y)), i);
        b.tiles.push(info);
        minY = std::min(minY, tiles[i].y);
        maxY = std::max(maxY, tiles[i].y);
    }
    b.regionMpu = b.config.metresPerUnit > 0 ? b.config.metresPerUnit : metresPerUnitAtTileY((minY + maxY + 1) / 2.0);
    b.progress.total = 2 * tileCount + 7;
    bool ok = !b.tiles.failed() && !b.tileIndex.failed();
    ok = ok && b.readTiles() && b.dedupPoints() && b.filterHouses() && b.makeClusters() && b.matchHouses() && b.makeAddresses();
    uint32_t pts = 4;
    if (ok) {
        const int64_t span = std::max<int64_t>(static_cast<int64_t>(b.maxX) - b.minX, static_cast<int64_t>(b.maxY) - b.minY);
        pts = span >= 65536 ? 8 : 4;
        if (!b.anyPoint) {
            b.minX = b.minY = 0;
        }
    }
    ok = ok && b.writeAddressRecords(pts) && b.pointGroups() && b.writeGroups(pts) && b.writeKeys() &&
        b.writeFile(outPath, temporaryPath, pts);
    b.noteTables();
    b.removeScratch();
    impl->~Impl();
    largeFree(impl);
    impl = nullptr;
    if (!ok && errorText[0] == '\0') {
        snprintf(errorText, sizeof(errorText), "the build failed");
    }
    return ok;
}

} // namespace maps
