// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchRegion.h"

#include "QueryParse.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t NONE = UINT32_MAX;
constexpr int TILE_UNITS = 4096;
constexpr double PI_D = 3.14159265358979323846;
constexpr size_t KEY_BUFFER = 2048;
/** Fields of the index: a cluster's address count. */
constexpr uint32_t MAX_CLUSTER_ADDRESSES = 0xFFFF;
/** Lines are added up to this far outside a tile (the 100 m of matching plus the 60 m of the neighbours' vote). */
constexpr double LINE_BUFFER_M = 160.0;

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

bool sameText(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

int32_t floorDiv(int32_t value, int32_t by) {
    return value >= 0 ? value / by : -((-value + by - 1) / by);
}

bool startsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/** The characters of a UTF-8 word (data.py len() of a Python string). */
size_t characterCount(const char* s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) {
        if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

uint32_t tileKey(int x, int y) {
    return static_cast<uint32_t>(x) << 16 | static_cast<uint32_t>(y);
}

} // namespace

double metresPerUnitAtTileY(double tileY) {
    const double n = PI_D * (1.0 - 2.0 * tileY / 16384.0);
    const double lat = std::atan(std::sinh(n));
    return 40075016.686 * std::cos(lat) / (16384.0 * 4096.0);
}

// ------------------------------------------------------------------------------------------------ HashIndex

void HashIndex::clear() {
    full = false;
    values.clear();
    hashes.clear();
    used = 0;
}

void HashIndex::release() {
    full = false;
    values.release();
    hashes.release();
    used = 0;
}

void HashIndex::put(uint64_t hash, uint32_t value) {
    const size_t mask = values.size() - 1;
    size_t i = static_cast<size_t>(hash) & mask;
    while (values[i] != NONE) {
        i = (i + 1) & mask;
    }
    values[i] = value;
    hashes[i] = hash;
}

void HashIndex::insert(uint64_t hash, uint32_t value) {
    if (full) {
        return;
    }
    if ((used + 1) * 2 > values.size()) {
        // Both new arrays first: without memory for both the old table stays as it was (find() keeps working on it)
        // and the index is full, which failed() reports
        const size_t capacity = values.size() < 1024 ? 1024 : values.size() * 2;
        GrowArray<uint32_t> grownValues;
        GrowArray<uint64_t> grownHashes;
        if (!grownValues.resize(capacity) || !grownHashes.resize(capacity)) {
            full = true;
            return;
        }
        for (size_t i = 0; i < capacity; i++) {
            grownValues[i] = NONE;
        }
        grownValues.swap(values);
        grownHashes.swap(hashes);
        // The old table is in the grown* arrays now
        for (size_t i = 0; i < grownValues.size(); i++) {
            if (grownValues[i] != NONE) {
                put(grownHashes[i], grownValues[i]);
            }
        }
    }
    put(hash, value);
    used++;
}

// ------------------------------------------------------------------------------------------------ builder

SearchRegionBuilder::SearchRegionBuilder() = default;
SearchRegionBuilder::~SearchRegionBuilder() = default;

bool SearchRegionBuilder::fail(const char* message) {
    snprintf(errorText, sizeof(errorText), "%s", message);
    return false;
}

bool SearchRegionBuilder::anyFailed() const {
    return text.failed() || points.failed() || lines.failed() || linePoints.failed() || houses.failed() || labelIndex.failed() ||
        nameIndex.failed() || pointIndex.failed() || lineIndex.failed() || houseIndex.failed() || tileIndex.failed() || labelOffsets.failed() ||
        nameOffsets.failed() || clusters.failed() || streets.failed() || addressRecs.failed() || keyPool.failed() || keyOffsets.failed() ||
        pointKeys.failed() || streetKeys.failed() || pointInputs.failed() || streetInputs.failed() || clusterInputs.failed() ||
        addressInputs.failed() || keyPointers.failed() || pseudos.failed() || settlements.failed();
}

uint32_t SearchRegionBuilder::copyText(const char* s) {
    if (s == nullptr) {
        return EXTRACT_NONE;
    }
    const auto offset = static_cast<uint32_t>(text.size());
    const size_t length = strlen(s);
    // s may point into text (a settlement's name): grow first, so the pushes below never move it
    const bool inside = s >= text.begin() && s < text.begin() + text.size();
    const size_t from = inside ? static_cast<size_t>(s - text.begin()) : 0;
    if (!text.reserve(text.size() + length + 1)) {
        text.push(0);   // marks the failure (failed())
        return offset;
    }
    const char* source = inside ? text.begin() + from : s;
    for (size_t i = 0; i <= length; i++) {
        text.push(source[i]);
    }
    return offset;
}

uint32_t SearchRegionBuilder::internLabel(const char* label) {
    const uint64_t hash = mix(hashText(1469598103934665603ULL, label));
    const uint32_t found = labelIndex.find(hash, [&](uint32_t id) { return strcmp(&text[labelOffsets[id]], label) == 0; });
    if (found != NONE) {
        return found;
    }
    const auto id = static_cast<uint32_t>(labelOffsets.size());
    labelOffsets.push(copyText(label));
    labelIndex.insert(hash, id);
    return id;
}

uint32_t SearchRegionBuilder::internName(const char* name) {
    const uint64_t hash = mix(hashText(1469598103934665603ULL, name));
    const uint32_t found = nameIndex.find(hash, [&](uint32_t id) { return strcmp(&text[nameOffsets[id]], name) == 0; });
    if (found != NONE) {
        return found;
    }
    const auto id = static_cast<uint32_t>(nameOffsets.size());
    nameOffsets.push(copyText(name));
    nameIndex.insert(hash, id);
    return id;
}

bool SearchRegionBuilder::mergeTile(const ExtractedTile& tile, uint32_t tileNumber) {
    for (size_t i = 0; i < tile.points.size(); i++) {
        const ExtractPoint& p = tile.points[i];
        const char* name = tile.str(p.name);
        const uint32_t labelId = internLabel(tile.str(p.label));
        const uint64_t hash = mix(hashInt(hashInt(hashText(hashInt(1469598103934665603ULL, labelId), name), p.x), p.y));
        const uint32_t found = pointIndex.find(hash, [&](uint32_t c) {
            const Pt& q = points[c];
            return q.label == labelId && q.x == p.x && q.y == p.y && sameText(textAt(q.name), name);
        });
        if (found != NONE) {
            continue;
        }
        Pt q = {};
        q.label = labelId;
        q.name = copyText(name);
        q.latin = copyText(tile.str(p.latin));
        q.en = copyText(tile.str(p.en));
        q.x = p.x;
        q.y = p.y;
        q.settlement = p.settlement;
        q.useEn = p.useEn;
        pointIndex.insert(hash, static_cast<uint32_t>(points.size()));
        points.push(q);
    }
    for (size_t i = 0; i < tile.lines.size(); i++) {
        const ExtractLine& l = tile.lines[i];
        const uint32_t nameId = internName(tile.str(l.name));
        const int32_t* xy = &tile.linePoints[2 * static_cast<size_t>(l.first)];
        const uint64_t hash = mix(hashBytes(hashInt(1469598103934665603ULL, nameId), xy, 2 * sizeof(int32_t) * l.count));
        const uint32_t found = lineIndex.find(hash, [&](uint32_t c) {
            const Ln& q = lines[c];
            return q.nameId == nameId && q.count == l.count && memcmp(&linePoints[2 * static_cast<size_t>(q.first)], xy, 2 * sizeof(int32_t) * l.count) == 0;
        });
        if (found != NONE) {
            continue;
        }
        Ln q = {};
        q.nameId = nameId;
        q.latin = copyText(tile.str(l.latin));
        q.first = static_cast<uint32_t>(linePoints.size() / 2);
        q.count = l.count;
        q.cluster = NONE;
        q.pathClass = l.pathClass;
        q.minX = q.maxX = xy[0];
        q.minY = q.maxY = xy[1];
        for (uint32_t k = 0; k < l.count; k++) {
            q.minX = std::min(q.minX, xy[2 * k]);
            q.maxX = std::max(q.maxX, xy[2 * k]);
            q.minY = std::min(q.minY, xy[2 * k + 1]);
            q.maxY = std::max(q.maxY, xy[2 * k + 1]);
            linePoints.push(xy[2 * k]);
            linePoints.push(xy[2 * k + 1]);
        }
        lineIndex.insert(hash, static_cast<uint32_t>(lines.size()));
        lines.push(q);
    }
    for (size_t i = 0; i < tile.houses.size(); i++) {
        const ExtractHouse& h = tile.houses[i];
        const char* number = tile.str(h.text);
        const uint64_t hash = mix(hashInt(hashInt(hashText(1469598103934665603ULL, number), h.x), h.y));
        const uint32_t found = houseIndex.find(hash, [&](uint32_t c) {
            const Hs& q = houses[c];
            return q.x == h.x && q.y == h.y && strcmp(&text[q.text], number) == 0;
        });
        if (found != NONE) {
            // data.py: the copy inside its own tile wins
            if (h.inside != 0 && houses[found].inside == 0) {
                houses[found].inside = 1;
                houses[found].tile = tileNumber;
                houses[found].local = static_cast<uint32_t>(i);
            }
            continue;
        }
        Hs q = {};
        q.text = copyText(number);
        q.x = h.x;
        q.y = h.y;
        q.tile = tileNumber;
        q.local = static_cast<uint32_t>(i);
        q.inside = h.inside;
        q.cluster = NONE;
        q.village = NONE;
        houseIndex.insert(hash, static_cast<uint32_t>(houses.size()));
        houses.push(q);
    }
    return !anyFailed();
}

bool SearchRegionBuilder::addTile(MvtDecoder& decoder, ByteInput* input, int tileX, int tileY) {
    if (!extractor.extract(decoder, input, tileX, tileY, tileBuffer)) {
        return fail("cannot read a tile");
    }
    const uint64_t hash = mix(tileKey(tileX, tileY));
    const auto tileNumber = static_cast<uint32_t>(tiles.size());
    tileIndex.insert(hash, tileNumber);
    tiles.push({ tileX, tileY });
    statistics.refOnlyLines += tileBuffer.refOnlyLines;
    statistics.junkPoisSkipped += tileBuffer.junkPois;
    return mergeTile(tileBuffer, tileNumber) || fail("no memory");
}

HouseView SearchRegionBuilder::house(size_t index) const {
    const Hs& h = houses[index];
    HouseView v = {};
    v.text = &text[h.text];
    v.x = h.x;
    v.y = h.y;
    v.tier = static_cast<AddressTier>(h.tier);
    v.streetM = h.streetM;
    v.villageStyle = h.villageStyle != 0;
    if (h.cluster != NONE) {
        v.street = textAt(nameOffsets[streets[clusters[h.cluster].street].nameId]);
    }
    if (h.village != NONE) {
        v.village = textAt(nameOffsets[settlements[h.village].nameId]);
    }
    return v;
}

// ------------------------------------------------------------------------------------------------ clusters

namespace {

struct CellEntry {
    uint32_t nameId;
    int32_t cx, cy;
    uint32_t line;
};

struct LineRef {
    uint32_t nameId;
    uint32_t root;
    uint32_t line;
};

struct Group {
    uint32_t nameId;
    uint32_t begin;
    uint32_t end;
    int32_t minX, minY;
};

} // namespace

bool SearchRegionBuilder::makeClusters(double metresPerUnit) {
    const size_t count = lines.size();
    const auto cell = std::max<int32_t>(1, static_cast<int32_t>(150.0 / metresPerUnit));
    GrowArray<uint32_t> parent;
    parent.resize(count);
    for (size_t i = 0; i < count; i++) {
        parent[i] = static_cast<uint32_t>(i);
    }
    auto find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    HashIndex cells;
    GrowArray<CellEntry> entries;
    auto cellHash = [](uint32_t nameId, int32_t cx, int32_t cy) {
        return mix((static_cast<uint64_t>(nameId) * 0x9E3779B97F4A7C15ULL) ^ (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32 | static_cast<uint32_t>(cy)));
    };
    for (size_t i = 0; i < count; i++) {
        const Ln& line = lines[i];
        int32_t previousX = INT32_MIN;
        int32_t previousY = INT32_MIN;
        for (uint32_t k = 0; k < line.count; k++) {
            const int32_t cx = floorDiv(linePoints[2 * (static_cast<size_t>(line.first) + k)], cell);
            const int32_t cy = floorDiv(linePoints[2 * (static_cast<size_t>(line.first) + k) + 1], cell);
            if (cx == previousX && cy == previousY) {
                continue;
            }
            previousX = cx;
            previousY = cy;
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    const uint32_t found = cells.find(cellHash(line.nameId, cx + dx, cy + dy), [&](uint32_t e) {
                        return entries[e].nameId == line.nameId && entries[e].cx == cx + dx && entries[e].cy == cy + dy;
                    });
                    if (found != NONE) {
                        const uint32_t a = find(static_cast<uint32_t>(i));
                        const uint32_t b = find(entries[found].line);
                        if (a != b) {
                            parent[a] = b;
                        }
                    }
                }
            }
            const uint64_t hash = cellHash(line.nameId, cx, cy);
            if (cells.find(hash, [&](uint32_t e) { return entries[e].nameId == line.nameId && entries[e].cx == cx && entries[e].cy == cy; }) == NONE) {
                cells.insert(hash, static_cast<uint32_t>(entries.size()));
                entries.push({ line.nameId, cx, cy, static_cast<uint32_t>(i) });
            }
        }
    }
    cells.release();
    entries.release();
    GrowArray<LineRef> refs;
    refs.reserve(count);
    for (size_t i = 0; i < count; i++) {
        refs.push({ lines[i].nameId, find(static_cast<uint32_t>(i)), static_cast<uint32_t>(i) });
    }
    std::sort(refs.begin(), refs.end(), [](const LineRef& a, const LineRef& b) {
        if (a.nameId != b.nameId) return a.nameId < b.nameId;
        if (a.root != b.root) return a.root < b.root;
        return a.line < b.line;
    });
    GrowArray<Group> groups;
    for (size_t i = 0; i < refs.size();) {
        size_t j = i;
        Group g = { refs[i].nameId, static_cast<uint32_t>(i), 0, INT32_MAX, INT32_MAX };
        while (j < refs.size() && refs[j].nameId == refs[i].nameId && refs[j].root == refs[i].root) {
            const Ln& line = lines[refs[j].line];
            const int32_t fx = linePoints[2 * static_cast<size_t>(line.first)];
            const int32_t fy = linePoints[2 * static_cast<size_t>(line.first) + 1];
            if (fx < g.minX || (fx == g.minX && fy < g.minY)) {
                g.minX = fx;
                g.minY = fy;
            }
            j++;
        }
        g.end = static_cast<uint32_t>(j);
        groups.push(g);
        i = j;
    }
    // Streets in the byte order of their names, clusters of a street by their smallest first vertex
    std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
        if (a.nameId != b.nameId) return strcmp(&text[nameOffsets[a.nameId]], &text[nameOffsets[b.nameId]]) < 0;
        if (a.minX != b.minX) return a.minX < b.minX;
        if (a.minY != b.minY) return a.minY < b.minY;
        return a.begin < b.begin;
    });
    clusters.clear();
    streets.clear();
    for (const Group& g : groups) {
        if (streets.size() == 0 || streets[streets.size() - 1].nameId != g.nameId) {
            Street s = {};
            s.nameId = g.nameId;
            s.latin = EXTRACT_NONE;
            s.en = EXTRACT_NONE;
            s.firstCluster = static_cast<uint32_t>(clusters.size());
            streets.push(s);
        }
        Street& street = streets[streets.size() - 1];
        Cluster c = {};
        c.street = static_cast<uint32_t>(streets.size() - 1);
        c.minX = c.minY = INT32_MAX;
        c.maxX = c.maxY = INT32_MIN;
        double sumX = 0;
        double sumY = 0;
        uint64_t total = 0;
        for (uint32_t r = g.begin; r < g.end; r++) {
            Ln& line = lines[refs[r].line];
            line.cluster = static_cast<uint32_t>(clusters.size());
            if (street.latin == EXTRACT_NONE && line.latin != EXTRACT_NONE) {
                street.latin = line.latin;
            }
            for (uint32_t k = 0; k < line.count; k++) {
                const int32_t x = linePoints[2 * (static_cast<size_t>(line.first) + k)];
                const int32_t y = linePoints[2 * (static_cast<size_t>(line.first) + k) + 1];
                sumX += x;
                sumY += y;
                total++;
                c.minX = std::min(c.minX, x);
                c.maxX = std::max(c.maxX, x);
                c.minY = std::min(c.minY, y);
                c.maxY = std::max(c.maxY, y);
            }
        }
        const double cx = sumX / static_cast<double>(total);
        const double cy = sumY / static_cast<double>(total);
        double bestD = 0;
        bool have = false;
        for (uint32_t r = g.begin; r < g.end; r++) {
            const Ln& line = lines[refs[r].line];
            for (uint32_t k = 0; k < line.count; k++) {
                const int32_t x = linePoints[2 * (static_cast<size_t>(line.first) + k)];
                const int32_t y = linePoints[2 * (static_cast<size_t>(line.first) + k) + 1];
                const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (!have || d < bestD || (d == bestD && (x < c.repX || (x == c.repX && y < c.repY)))) {
                    have = true;
                    bestD = d;
                    c.repX = x;
                    c.repY = y;
                }
            }
        }
        street.clusterCount++;
        clusters.push(c);
    }
    return !(clusters.failed() || streets.failed() || parent.failed() || refs.failed() || groups.failed());
}

// ------------------------------------------------------------------------------------------------ matching

bool SearchRegionBuilder::matchHouses(const RegionConfig& config, double metresPerUnit, bool villageMode, bool slashSecond) {
    const size_t tileCount = tiles.size();
    const size_t houseCount = houses.size();
    // Own houses per tile
    GrowArray<uint32_t> houseStart;
    GrowArray<uint32_t> houseOrder;
    houseStart.resize(tileCount + 1);
    for (size_t i = 0; i < houseCount; i++) {
        houseStart[houses[i].tile + 1]++;
    }
    for (size_t t = 0; t < tileCount; t++) {
        houseStart[t + 1] += houseStart[t];
    }
    houseOrder.resize(houseCount);
    {
        GrowArray<uint32_t> fill;
        fill.resize(tileCount);
        for (size_t i = 0; i < houseCount; i++) {
            const uint32_t t = houses[i].tile;
            houseOrder[houseStart[t] + fill[t]++] = static_cast<uint32_t>(i);
        }
    }
    // A tile's houses in the order of its own extract (the order a streaming build sees them in)
    for (size_t t = 0; t < tileCount; t++) {
        std::sort(&houseOrder[houseStart[t]], &houseOrder[houseStart[t]] + (houseStart[t + 1] - houseStart[t]),
                  [&](uint32_t a, uint32_t b) { return houses[a].local < houses[b].local; });
    }
    // Lines per tile, with the buffer
    const auto lineBuffer = static_cast<int32_t>(LINE_BUFFER_M / metresPerUnit) + 1;
    GrowArray<uint32_t> lineStart;
    GrowArray<uint32_t> lineOrder;
    lineStart.resize(tileCount + 1);
    auto forTiles = [&](const Ln& line, auto visit) {
        const int32_t x0 = floorDiv(line.minX - lineBuffer, TILE_UNITS);
        const int32_t x1 = floorDiv(line.maxX + lineBuffer, TILE_UNITS);
        const int32_t y0 = floorDiv(line.minY - lineBuffer, TILE_UNITS);
        const int32_t y1 = floorDiv(line.maxY + lineBuffer, TILE_UNITS);
        for (int32_t tx = x0; tx <= x1; tx++) {
            for (int32_t ty = y0; ty <= y1; ty++) {
                const uint32_t t = tileIndex.find(mix(tileKey(tx, ty)), [&](uint32_t c) { return tiles[c].x == tx && tiles[c].y == ty; });
                if (t != NONE) {
                    visit(t);
                }
            }
        }
    };
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].pathClass != 0 && !config.matchPathLines) {
            continue;
        }
        forTiles(lines[i], [&](uint32_t t) { lineStart[t + 1]++; });
    }
    for (size_t t = 0; t < tileCount; t++) {
        lineStart[t + 1] += lineStart[t];
    }
    lineOrder.resize(lineStart[tileCount]);
    {
        GrowArray<uint32_t> fill;
        fill.resize(tileCount);
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].pathClass != 0 && !config.matchPathLines) {
                continue;
            }
            forTiles(lines[i], [&](uint32_t t) { lineOrder[lineStart[t] + fill[t]++] = static_cast<uint32_t>(i); });
        }
    }
    settlementPseudo.resize(settlements.size());
    for (size_t i = 0; i < settlementPseudo.size(); i++) {
        settlementPseudo[i] = NONE;
    }
    nameStreet.resize(nameOffsets.size());
    for (size_t i = 0; i < nameStreet.size(); i++) {
        nameStreet[i] = NONE;
    }
    for (size_t i = 0; i < streets.size(); i++) {
        nameStreet[streets[i].nameId] = static_cast<uint32_t>(i);
    }

    GrowArray<uint32_t> contextOf;
    for (size_t t = 0; t < tileCount; t++) {
        if (houseStart[t + 1] == houseStart[t]) {
            continue;
        }
        const double mpu = metresPerUnitAtTileY(tiles[t].y + 0.5);
        const int32_t rx0 = tiles[t].x * TILE_UNITS;
        const int32_t ry0 = tiles[t].y * TILE_UNITS;
        const int32_t rx1 = rx0 + TILE_UNITS - 1;
        const int32_t ry1 = ry0 + TILE_UNITS - 1;
        matcher.begin(mpu);
        matcher.clipToTile(tiles[t].x, tiles[t].y);
        for (uint32_t k = lineStart[t]; k < lineStart[t + 1]; k++) {
            const Ln& line = lines[lineOrder[k]];
            matcher.addLine(line.cluster, &linePoints[2 * static_cast<size_t>(line.first)], line.count);
        }
        if (villageMode) {
            const auto placeBuffer = static_cast<int32_t>(AddressMatcher::PLACE_M / mpu) + 1;
            for (size_t s = 0; s < settlements.size(); s++) {
                const Pt& p = points[settlements[s].point];
                if (p.x >= rx0 - placeBuffer && p.x <= rx1 + placeBuffer && p.y >= ry0 - placeBuffer && p.y <= ry1 + placeBuffer) {
                    matcher.addPlace(static_cast<uint32_t>(s), static_cast<PlaceClass>(settlements[s].placeClass), p.x, p.y);
                }
            }
        }
        auto streetNumberOf = [&](const char* number) { return houseVoteNumber(number, slashSecond && config.voteSlashSecond); };
        const uint32_t own = houseStart[t + 1] - houseStart[t];
        for (uint32_t k = houseStart[t]; k < houseStart[t + 1]; k++) {
            Hs& h = houses[houseOrder[k]];
            const char* number = &text[h.text];
            const bool villageStyle = villageMode && houseNumberStyle(number).plain;
            h.villageStyle = villageStyle ? 1 : 0;
            matcher.addHouse(h.x, h.y, streetNumberOf(number), villageStyle);
        }
        // The neighbours' houses near the tile edge take part in the vote
        const auto voteBuffer = static_cast<int32_t>(AddressMatcher::VOTE_M / mpu) + 1;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const int nx = tiles[t].x + dx;
                const int ny = tiles[t].y + dy;
                const uint32_t n = tileIndex.find(mix(tileKey(nx, ny)), [&](uint32_t c) { return tiles[c].x == nx && tiles[c].y == ny; });
                if (n == NONE) {
                    continue;
                }
                for (uint32_t k = houseStart[n]; k < houseStart[n + 1]; k++) {
                    const Hs& h = houses[houseOrder[k]];
                    if (h.x >= rx0 - voteBuffer && h.x <= rx1 + voteBuffer && h.y >= ry0 - voteBuffer && h.y <= ry1 + voteBuffer) {
                        matcher.addHouse(h.x, h.y, streetNumberOf(&text[h.text]), false);
                    }
                }
            }
        }
        if (!matcher.run()) {
            return fail("no memory for the house matching");
        }
        for (uint32_t k = 0; k < own; k++) {
            const uint32_t index = houseOrder[houseStart[t] + k];
            Hs& h = houses[index];
            const AddressMatch& m = matcher.result(k);
            h.tier = static_cast<uint8_t>(m.tier);
            h.cluster = m.tier == AddressTier::None ? NONE : m.street;
            h.streetM = m.streetM;
            h.village = (h.villageStyle != 0 && m.villages[0] != NONE) ? m.villages[0] : NONE;
            if (h.cluster != NONE) {
                addressRecs.push({ h.cluster, index, static_cast<uint8_t>(m.tier == AddressTier::LowStreet ? ADDRESS_LOW : 0) });
                if (m.tier == AddressTier::Street) {
                    statistics.pathStreet50++;
                } else {
                    statistics.pathStreet100++;
                }
                if (h.village != NONE) {
                    statistics.alsoVillage++;
                }
            } else if (h.village != NONE) {
                statistics.pathVillage++;
            } else {
                statistics.pathNone++;
            }
            if (h.village != NONE) {
                uint32_t& pseudo = settlementPseudo[h.village];
                if (pseudo == NONE) {
                    const Settlement& s = settlements[h.village];
                    uint32_t street = nameStreet[s.nameId];
                    if (street == NONE) {
                        const Pt& p = points[s.point];
                        Street created = {};
                        created.nameId = s.nameId;
                        created.latin = p.latin;
                        created.en = p.en;
                        created.useEn = 1;
                        street = static_cast<uint32_t>(streets.size());
                        nameStreet[s.nameId] = street;
                        streets.push(created);
                    }
                    pseudo = static_cast<uint32_t>(pseudos.size());
                    pseudos.push({ street, h.village });
                }
                addressRecs.push({ static_cast<uint32_t>(clusters.size()) + pseudo, index, ADDRESS_VILLAGE });
            }
        }
    }
    matcher.release();
    statistics.pseudoStreets = static_cast<uint32_t>(pseudos.size());
    return !anyFailed();
}

// ------------------------------------------------------------------------------------------------ keys

SearchRegionBuilder::KeyRef SearchRegionBuilder::makeKeys(const char* name, const char* latin, const char* en, bool useEn, Translit profile) {
    KeyRef ref = {};
    keyMaker.make(name, latin, en, useEn, profile);
    auto copy = [&](const char* key) {
        const auto offset = static_cast<uint32_t>(keyPool.size());
        for (const char* c = key;; c++) {
            keyPool.push(*c);
            if (*c == '\0') {
                break;
            }
        }
        return offset;
    };
    ref.sortKey = copy(keyMaker.sortKey());
    ref.first = static_cast<uint32_t>(keyOffsets.size());
    for (uint32_t i = 0; i < keyMaker.count(); i++) {
        keyOffsets.push(copy(keyMaker.key(i)));
    }
    ref.count = keyMaker.count();
    return ref;
}

// ------------------------------------------------------------------------------------------------ KeyMaker

void KeyMaker::sortKeyOf(const char* name, Translit profile, char* out, size_t outSize) {
    if (!hasNonLatin(name)) {
        foldLatin(name, out, outSize);
    } else {
        transliterate(name, profile, out, outSize);
    }
}

bool KeyMaker::has(const char* key) const {
    for (size_t i = 0; i < offsets.size(); i++) {
        if (strcmp(&pool[offsets[i]], key) == 0) {
            return true;
        }
    }
    return false;
}

void KeyMaker::add(const char* key) {
    offsets.push(static_cast<uint32_t>(pool.size()));
    for (const char* c = key;; c++) {
        pool.push(*c);
        if (*c == '\0') {
            break;
        }
    }
}

bool KeyMaker::make(const char* name, const char* latin, const char* en, bool useEn, Translit profile) {
    pool.clear();
    offsets.clear();
    // The sort key first in the pool (offset 0), not one of the keys
    sortKeyOf(name, profile, scratch[4], BUFFER);
    for (const char* c = scratch[4];; c++) {
        pool.push(*c);
        if (*c == '\0') {
            break;
        }
    }
    // fold.search_keys
    int n = 0;
    if (!hasNonLatin(name)) {
        foldLatin(name, scratch[n++], BUFFER);
        if (latin != nullptr && !hasNonLatin(latin)) {
            foldLatin(latin, scratch[n++], BUFFER);
        }
    } else {
        transliterate(name, profile, scratch[n++], BUFFER);
        foldOriginal(name, scratch[n++], BUFFER);
        if (latin != nullptr) {
            foldLatin(latin, scratch[n++], BUFFER);
        }
    }
    if (useEn && en != nullptr && !hasNonLatin(en)) {
        foldLatin(en, scratch[n++], BUFFER);
    }
    for (int i = 0; i < n; i++) {
        if (scratch[i][0] != '\0' && !has(scratch[i])) {
            add(scratch[i]);
        }
    }
    // fold.suffix_keys of each base key: the text from each later word of at least 3 characters
    const size_t baseCount = offsets.size();
    for (size_t b = 0; b < baseCount; b++) {
        // The pool may move while keys are added: work on a copy
        strncpy(scratch[4], &pool[offsets[b]], BUFFER - 1);
        scratch[4][BUFFER - 1] = '\0';
        const char* word = scratch[4];
        bool first = true;
        while (true) {
            const char* space = strchr(word, ' ');
            const size_t length = space != nullptr ? static_cast<size_t>(space - word) : strlen(word);
            if (!first && characterCount(word, length) >= 3 && !has(word)) {
                add(word);
            }
            first = false;
            if (space == nullptr) {
                break;
            }
            word = space + 1;
        }
    }
    return !pool.failed() && !offsets.failed();
}

// ------------------------------------------------------------------------------------------------ assembly

bool SearchRegionBuilder::assemble(const RegionConfig& config) {
    const size_t realClusters = clusters.size();
    const size_t unified = realClusters + pseudos.size();
    // Streets in the byte order of their names (the settlements' own streets were added at the end)
    GrowArray<uint32_t> streetOrder;
    GrowArray<uint32_t> streetRank;
    for (size_t i = 0; i < streets.size(); i++) {
        streetOrder.push(static_cast<uint32_t>(i));
    }
    std::sort(streetOrder.begin(), streetOrder.end(), [&](uint32_t a, uint32_t b) {
        return strcmp(&text[nameOffsets[streets[a].nameId]], &text[nameOffsets[streets[b].nameId]]) < 0;
    });
    streetRank.resize(streets.size());
    for (size_t k = 0; k < streetOrder.size(); k++) {
        streetRank[streetOrder[k]] = static_cast<uint32_t>(k);
    }
    // Pseudo clusters in street order, then in the order of their settlements (the points' order)
    GrowArray<uint32_t> pseudoOrder;
    for (size_t i = 0; i < pseudos.size(); i++) {
        pseudoOrder.push(static_cast<uint32_t>(i));
    }
    std::sort(pseudoOrder.begin(), pseudoOrder.end(), [&](uint32_t a, uint32_t b) {
        if (pseudos[a].street != pseudos[b].street) return streetRank[pseudos[a].street] < streetRank[pseudos[b].street];
        return pseudos[a].settlement < pseudos[b].settlement;
    });
    // Output order of the clusters: per street, the real ones, then the places'
    GrowArray<uint32_t> finalOf;
    finalOf.resize(unified);
    GrowArray<uint32_t> unifiedAt;
    size_t cursor = 0;
    for (size_t k = 0; k < streetOrder.size(); k++) {
        const uint32_t s = streetOrder[k];
        for (uint32_t c = 0; c < streets[s].clusterCount; c++) {
            const uint32_t id = streets[s].firstCluster + c;
            finalOf[id] = static_cast<uint32_t>(unifiedAt.size());
            unifiedAt.push(id);
        }
        while (cursor < pseudoOrder.size() && pseudos[pseudoOrder[cursor]].street == s) {
            const auto id = static_cast<uint32_t>(realClusters + pseudoOrder[cursor]);
            finalOf[id] = static_cast<uint32_t>(unifiedAt.size());
            unifiedAt.push(id);
            cursor++;
        }
    }
    // Addresses per output cluster (counting sort), capped at the field's width
    GrowArray<uint32_t> start;
    start.resize(unified + 1);
    for (const AddressRec& r : addressRecs) {
        start[finalOf[r.cluster] + 1]++;
    }
    for (size_t i = 0; i < unified; i++) {
        start[i + 1] += start[i];
    }
    GrowArray<uint32_t> order;
    order.resize(addressRecs.size());
    {
        GrowArray<uint32_t> fill;
        fill.resize(unified);
        for (size_t i = 0; i < addressRecs.size(); i++) {
            const uint32_t f = finalOf[addressRecs[i].cluster];
            order[start[f] + fill[f]++] = static_cast<uint32_t>(i);
        }
    }
    addressInputs.clear();
    clusterInputs.clear();
    GrowArray<uint32_t> addressFirst;
    GrowArray<uint32_t> numberOffsets;
    for (size_t f = 0; f < unified; f++) {
        const uint32_t id = unifiedAt[f];
        SearchClusterInput c = {};
        if (id < realClusters) {
            const Cluster& k = clusters[id];
            c.repX = k.repX;
            c.repY = k.repY;
            c.minX = k.minX;
            c.minY = k.minY;
            c.maxX = k.maxX;
            c.maxY = k.maxY;
        } else {
            const Pt& p = points[settlements[pseudos[id - realClusters].settlement].point];
            c.repX = c.minX = c.maxX = p.x;
            c.repY = c.minY = c.maxY = p.y;
        }
        addressFirst.push(static_cast<uint32_t>(addressInputs.size()));
        uint32_t taken = 0;
        for (uint32_t k = start[f]; k < start[f + 1]; k++) {
            if (taken >= MAX_CLUSTER_ADDRESSES) {
                statistics.droppedAddresses++;
                continue;
            }
            const AddressRec& r = addressRecs[order[k]];
            const Hs& h = houses[r.house];
            // The text pointer is set when the pool is final (below): keep the offset for now
            addressInputs.push({ nullptr, h.x, h.y, r.flags });
            numberOffsets.push(h.text);
            taken++;
        }
        c.addressCount = taken;
        clusterInputs.push(c);
    }
    for (size_t f = 0; f < unified; f++) {
        clusterInputs[f].addresses = clusterInputs[f].addressCount > 0 ? &addressInputs[addressFirst[f]] : nullptr;
    }
    statistics.addresses = static_cast<uint32_t>(addressInputs.size());
    // Number texts: the offsets become pointers (the pool does not grow any more)
    for (size_t i = 0; i < addressInputs.size(); i++) {
        addressInputs[i].number = &text[numberOffsets[i]];
    }

    // Streets and their keys
    streetInputs.clear();
    streetKeys.clear();
    keyPool.clear();
    keyOffsets.clear();
    uint32_t clusterAt = 0;
    for (size_t k = 0; k < streetOrder.size(); k++) {
        const uint32_t s = streetOrder[k];
        const Street& street = streets[s];
        const KeyRef keys = makeKeys(&text[nameOffsets[street.nameId]], textAt(street.latin), textAt(street.en), street.useEn != 0, config.profile);
        streetKeys.push(keys);
        SearchStreetInput in = {};
        in.name = &text[nameOffsets[street.nameId]];
        in.keyCount = keys.count;
        uint32_t total = street.clusterCount;
        for (size_t p = 0; p < pseudoOrder.size(); p++) {
            if (pseudos[pseudoOrder[p]].street == s) {
                total++;
            }
        }
        in.clusterCount = total;
        in.clusters = total > 0 ? &clusterInputs[clusterAt] : nullptr;
        clusterAt += total;
        streetInputs.push(in);
    }
    // Points and their keys
    pointInputs.clear();
    pointKeys.clear();
    keyShare.clear();
    for (size_t i = 0; i < points.size(); i++) {
        const Pt& p = points[i];
        KeyRef keys = {};
        keys.sortKey = NONE;
        if (p.name != EXTRACT_NONE) {
            const char* name = &text[p.name];
            const uint64_t hash = mix(hashInt(hashText(hashText(hashText(1469598103934665603ULL, name), textAt(p.latin)), textAt(p.en)), p.useEn));
            const uint32_t found = keyShare.find(hash, [&](uint32_t c) {
                const Pt& q = points[c];
                return q.useEn == p.useEn && sameText(textAt(q.name), name) && sameText(textAt(q.latin), textAt(p.latin)) && sameText(textAt(q.en), textAt(p.en));
            });
            if (found != NONE) {
                keys = pointKeys[found];
            } else {
                keys = makeKeys(name, textAt(p.latin), textAt(p.en), p.useEn != 0, config.profile);
                keyShare.insert(hash, static_cast<uint32_t>(i));
            }
        }
        pointKeys.push(keys);
    }
    // Category names that are not ASCII get a key of their own
    GrowArray<uint32_t> categoryNameOffsets;
    GrowArray<uint32_t> categoryKeyOffsets;
    for (size_t i = 0; i < labelOffsets.size(); i++) {
        const char* label = &text[labelOffsets[i]];
        if (!startsWith(label, "poi/")) {
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
            if (ascii) {
                continue;
            }
            bool known = false;
            for (size_t c = 0; c < categoryNameOffsets.size(); c++) {
                known = known || strcmp(&keyPool[categoryNameOffsets[c]], display) == 0;
            }
            if (known) {
                continue;
            }
            categoryNameOffsets.push(static_cast<uint32_t>(keyPool.size()));
            for (const char* c = display;; c++) {
                keyPool.push(*c);
                if (*c == '\0') {
                    break;
                }
            }
            KeyMaker::sortKeyOf(display, config.profile, categoryScratch, sizeof(categoryScratch));
            categoryKeyOffsets.push(static_cast<uint32_t>(keyPool.size()));
            for (const char* c = categoryScratch;; c++) {
                keyPool.push(*c);
                if (*c == '\0') {
                    break;
                }
            }
        }
    }
    if (anyFailed()) {
        return false;
    }
    // The pools are final: pointers
    keyPointers.clear();
    for (size_t i = 0; i < keyOffsets.size(); i++) {
        keyPointers.push(&keyPool[keyOffsets[i]]);
    }
    for (size_t i = 0; i < points.size(); i++) {
        const Pt& p = points[i];
        SearchPointInput in = {};
        in.label = &text[labelOffsets[p.label]];
        in.x = p.x;
        in.y = p.y;
        if (p.name != EXTRACT_NONE) {
            in.name = &text[p.name];
            in.sortKey = &keyPool[pointKeys[i].sortKey];
            in.keys = pointKeys[i].count > 0 ? &keyPointers[pointKeys[i].first] : nullptr;
            in.keyCount = pointKeys[i].count;
        }
        pointInputs.push(in);
    }
    for (size_t s = 0; s < streets.size(); s++) {
        streetInputs[s].sortKey = &keyPool[streetKeys[s].sortKey];
        streetInputs[s].keys = streetKeys[s].count > 0 ? &keyPointers[streetKeys[s].first] : nullptr;
    }
    categoryNames.clear();
    categoryKeys.clear();
    for (size_t i = 0; i < categoryNameOffsets.size(); i++) {
        categoryNames.push(&keyPool[categoryNameOffsets[i]]);
        categoryKeys.push(&keyPool[categoryKeyOffsets[i]]);
    }
    searchInput = {};
    searchInput.points = pointInputs.begin();
    searchInput.pointCount = static_cast<uint32_t>(pointInputs.size());
    searchInput.streets = streetInputs.begin();
    searchInput.streetCount = static_cast<uint32_t>(streetInputs.size());
    searchInput.slashSecond = statistics.slashSecond;
    searchInput.categoryNames = categoryNames.begin();
    searchInput.categoryKeys = categoryKeys.begin();
    searchInput.categoryOverrides = static_cast<uint32_t>(categoryNames.size());
    return !anyFailed();
}

bool SearchRegionBuilder::buildSearchInput(const RegionConfig& config) {
    errorText[0] = '\0';
    if (tiles.size() == 0) {
        return fail("no tiles");
    }
    int minY = tiles[0].y;
    int maxY = tiles[0].y;
    for (const TileRef& t : tiles) {
        minY = std::min(minY, t.y);
        maxY = std::max(maxY, t.y);
    }
    const double mpu = config.metresPerUnit > 0 ? config.metresPerUnit : metresPerUnitAtTileY((minY + maxY + 1) / 2.0);
    RegionStats& st = statistics;
    st.tiles = static_cast<uint32_t>(tiles.size());
    st.lines = static_cast<uint32_t>(lines.size());
    st.houses = static_cast<uint32_t>(houses.size());
    for (const Pt& p : points) {
        const char* label = &text[labelOffsets[p.label]];
        if (startsWith(label, "place/")) {
            st.places++;
        } else if (startsWith(label, "poi/")) {
            if (p.name != EXTRACT_NONE) {
                st.namedPois++;
            } else {
                st.unnamedPois++;
            }
        } else {
            st.otherNamed++;
        }
    }
    // Settlements for the village rule
    settlements.clear();
    for (size_t i = 0; i < points.size(); i++) {
        if (points[i].settlement != EXTRACT_NOT_SETTLEMENT && points[i].name != EXTRACT_NONE) {
            settlements.push({ static_cast<uint32_t>(i), internName(&text[points[i].name]), points[i].settlement });
        }
    }
    st.settlements = static_cast<uint32_t>(settlements.size());
    // The region's number style
    for (const Hs& h : houses) {
        const HouseNumberStyle kind = houseNumberStyle(&text[h.text]);
        if (!kind.parsed) {
            st.textNumbers++;
        }
        if (kind.plain) {
            st.plainNumbers++;
        }
        if (kind.slash) {
            st.slashNumbers++;
            if (kind.secondSmaller) {
                st.slashSecondSmaller++;
            }
        }
    }
    st.plainShare = houses.size() > 0 ? static_cast<double>(st.plainNumbers) / static_cast<double>(houses.size()) : 0.0;
    st.villageMode = st.plainShare >= config.villageShare && houses.size() > 0;
    st.slashShare = st.slashNumbers > 0 ? static_cast<double>(st.slashSecondSmaller) / static_cast<double>(st.slashNumbers) : 0.0;
    st.slashSecond = st.slashShare >= config.slashSecondShare && st.slashNumbers > 0;
    if (anyFailed() || !makeClusters(mpu)) {
        return fail("no memory for the street clusters");
    }
    st.streetNames = static_cast<uint32_t>(streets.size());
    st.clusters = static_cast<uint32_t>(clusters.size());
    if (!matchHouses(config, mpu, st.villageMode, st.slashSecond)) {
        return errorText[0] != '\0' ? false : fail("no memory for the house matching");
    }
    if (!assemble(config)) {
        return fail("no memory for the index input");
    }
    return true;
}

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

} // namespace maps
