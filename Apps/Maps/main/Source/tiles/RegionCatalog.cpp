// SPDX-License-Identifier: GPL-3.0-or-later
#include "RegionCatalog.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

constexpr double UNIT = 10000.0;
// Small enough for a task stack (about 1.3 KB together); an outline crossing a tile row more often counts the whole row
constexpr int MAX_CROSSINGS = 128;
constexpr int MAX_INTERVALS = 96;

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readI32(const uint8_t* p) {
    return static_cast<int32_t>(readU32(p));
}

int32_t zigzagDecode(uint64_t value) {
    return static_cast<int32_t>((value >> 1) ^ (0 - (value & 1)));
}

struct Cursor {
    const uint8_t* data;
    size_t size;
    size_t pos;

    bool read(uint64_t& value) {
        value = 0;
        unsigned shift = 0;
        while (pos < size) {
            const uint8_t byte = data[pos++];
            if (shift < 64) {
                value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            }
            if ((byte & 0x80) == 0) {
                return true;
            }
            shift += 7;
            if (shift >= 64) {
                return false;
            }
        }
        return false;
    }
};

int floorToInt(double value) {
    const double floored = std::floor(value);
    if (floored <= -2147483648.0) {
        return -2147483647 - 1;
    }
    if (floored >= 2147483647.0) {
        return 2147483647;
    }
    return static_cast<int>(floored);
}

int ceilToInt(double value) {
    return -floorToInt(-value);
}

} // namespace

bool RegionCatalog::open(const uint8_t* data, size_t size) {
    data_ = nullptr;
    size_ = 0;
    count_ = 0;
    outlineOffset_ = 0;
    nameOffset_ = 0;
    open_ = false;

    if (data == nullptr || size < 16) {
        return false;
    }
    if (data[0] != 'T' || data[1] != 'M' || data[2] != 'R' || data[3] != 'C') {
        return false;
    }
    if (readU16(data + 4) != 1) {
        return false;
    }

    const uint16_t count = readU16(data + 6);
    const uint32_t outlineOffset = readU32(data + 8);
    const uint32_t nameOffset = readU32(data + 12);
    const size_t recordsEnd = 16 + static_cast<size_t>(count) * 32;
    if (recordsEnd > size || outlineOffset > size || nameOffset > size) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        const uint8_t* rec = data + 16 + static_cast<size_t>(i) * 32;
        const uint16_t parent = readU16(rec);
        if (parent != 0xFFFF && parent >= count) {
            return false;
        }
        const uint8_t nameLength = rec[3];
        const uint32_t nameRelative = readU32(rec + 4);
        const uint32_t outlineRelative = readU32(rec + 24);
        const uint64_t nameEnd = static_cast<uint64_t>(nameOffset) + nameRelative + nameLength;
        const uint64_t outlineBegin = static_cast<uint64_t>(outlineOffset) + outlineRelative;
        if (nameEnd > size || outlineBegin > size) {
            return false;
        }
    }

    data_ = data;
    size_ = size;
    count_ = count;
    outlineOffset_ = outlineOffset;
    nameOffset_ = nameOffset;
    open_ = true;
    return true;
}

bool RegionCatalog::isOpen() const {
    return open_;
}

int RegionCatalog::count() const {
    return count_;
}

const uint8_t* RegionCatalog::record(int index) const {
    return data_ + 16 + static_cast<size_t>(index) * 32;
}

const uint8_t* RegionCatalog::outlineStart(int index, size_t& length) const {
    const uint8_t* rec = record(index);
    const size_t start = static_cast<size_t>(outlineOffset_) + readU32(rec + 24);
    if (start > size_) {
        return nullptr;
    }
    size_t end = size_;
    if (nameOffset_ >= start && nameOffset_ <= size_) {
        end = nameOffset_;
    }
    length = end - start;
    return data_ + start;
}

CatalogRegion RegionCatalog::region(int index) const {
    CatalogRegion result { -1, 0, nullptr, 0, { 0.0, 0.0, 0.0, 0.0 }, 0 };
    if (!open_ || index < 0 || index >= count_) {
        return result;
    }
    const uint8_t* rec = record(index);
    const uint16_t parent = readU16(rec);
    result.parent = parent == 0xFFFF ? -1 : static_cast<int>(parent);
    result.level = rec[2];
    result.nameLength = rec[3];
    result.name = reinterpret_cast<const char*>(data_ + nameOffset_ + readU32(rec + 4));
    result.box.west = static_cast<double>(readI32(rec + 8)) / UNIT;
    result.box.south = static_cast<double>(readI32(rec + 12)) / UNIT;
    result.box.east = static_cast<double>(readI32(rec + 16)) / UNIT;
    result.box.north = static_cast<double>(readI32(rec + 20)) / UNIT;
    result.childCount = readU16(rec + 30);
    return result;
}

void RegionCatalog::copyName(int index, char* out, size_t size) const {
    if (out == nullptr || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!open_ || index < 0 || index >= count_) {
        return;
    }
    const uint8_t* rec = record(index);
    const size_t length = rec[3];
    const uint8_t* name = data_ + nameOffset_ + readU32(rec + 4);
    size_t copy = length;
    if (copy > size - 1) {
        copy = size - 1;
    }
    for (size_t i = 0; i < copy; i++) {
        out[i] = static_cast<char>(name[i]);
    }
    out[copy] = '\0';
}

int RegionCatalog::children(int parent, int* out, int max) const {
    if (!open_ || out == nullptr || max <= 0) {
        return 0;
    }
    int written = 0;
    for (int i = 0; i < count_ && written < max; i++) {
        const uint16_t value = readU16(record(i));
        const int regionParent = value == 0xFFFF ? -1 : static_cast<int>(value);
        if (regionParent == parent) {
            out[written++] = i;
        }
    }
    return written;
}

bool RegionCatalog::contains(int index, double lat, double lon) const {
    if (!open_ || index < 0 || index >= count_) {
        return false;
    }
    size_t end = 0;
    const uint8_t* start = outlineStart(index, end);
    if (start == nullptr) {
        return false;
    }
    const uint16_t ringCount = readU16(record(index) + 28);
    Cursor cursor { start, end, 0 };
    bool inside = false;
    for (uint16_t r = 0; r < ringCount; r++) {
        uint64_t pointCount = 0;
        if (!cursor.read(pointCount)) {
            return false;
        }
        if (pointCount == 0) {
            continue;
        }
        uint64_t vx = 0;
        uint64_t vy = 0;
        if (!cursor.read(vx) || !cursor.read(vy)) {
            return false;
        }
        int64_t lonUnits = zigzagDecode(vx);
        int64_t latUnits = zigzagDecode(vy);
        const double firstLon = static_cast<double>(lonUnits) / UNIT;
        const double firstLat = static_cast<double>(latUnits) / UNIT;
        double prevLon = firstLon;
        double prevLat = firstLat;
        if (pointCount < 2) {
            continue;
        }
        for (uint64_t i = 1; i < pointCount; i++) {
            uint64_t dx = 0;
            uint64_t dy = 0;
            if (!cursor.read(dx) || !cursor.read(dy)) {
                return false;
            }
            lonUnits += zigzagDecode(dx);
            latUnits += zigzagDecode(dy);
            const double x = static_cast<double>(lonUnits) / UNIT;
            const double y = static_cast<double>(latUnits) / UNIT;
            if ((prevLat > lat) != (y > lat)) {
                const double cross = prevLon + (lat - prevLat) * (x - prevLon) / (y - prevLat);
                if (lon < cross) {
                    inside = !inside;
                }
            }
            prevLon = x;
            prevLat = y;
        }
        if ((prevLat > lat) != (firstLat > lat)) {
            const double cross = prevLon + (lat - prevLat) * (firstLon - prevLon) / (firstLat - prevLat);
            if (lon < cross) {
                inside = !inside;
            }
        }
    }
    return inside;
}

int RegionCatalog::smallestContaining(double lat, double lon) const {
    if (!open_) {
        return -1;
    }
    int best = -1;
    int bestLevel = -1;
    double bestArea = 0.0;
    for (int i = 0; i < count_; i++) {
        if (!contains(i, lat, lon)) {
            continue;
        }
        const CatalogRegion info = region(i);
        double width = info.box.east - info.box.west;
        double height = info.box.north - info.box.south;
        if (width < 0.0) {
            width = -width;
        }
        if (height < 0.0) {
            height = -height;
        }
        const double area = width * height;
        if (info.level > bestLevel || (info.level == bestLevel && area < bestArea)) {
            best = i;
            bestLevel = info.level;
            bestArea = area;
        }
    }
    return best;
}

int64_t RegionCatalog::forEachTile(int index, int z, bool (*visit)(const TileKey& tile, void* context), void* context, int64_t limit) const {
    if (!open_ || index < 0 || index >= count_ || z < 0 || z > 30) {
        return 0;
    }
    const uint8_t* rec = record(index);
    const uint16_t ringCount = readU16(rec + 28);
    size_t outlineEnd = 0;
    const uint8_t* outline = outlineStart(index, outlineEnd);
    if (outline == nullptr) {
        return 0;
    }

    const CatalogRegion info = region(index);
    const TileRange rows = tileRange(info.box, z);
    const int maxTile = (1 << z) - 1;
    int64_t visited = 0;

    struct Interval {
        int lo;
        int hi;
    };

    for (int ty = rows.minY; ty <= rows.maxY; ty++) {
        const double yc = static_cast<double>(ty) + 0.5;
        float crossings[MAX_CROSSINGS];
        int crossingCount = 0;
        bool crossingOverflow = false;

        Interval intervals[MAX_INTERVALS];
        int intervalCount = 0;
        bool intervalOverflow = false;
        int spanMin = 0;
        int spanMax = -1;

        auto addInterval = [&](int lo, int hi) {
            if (lo > hi) {
                return;
            }
            if (spanMax < spanMin) {
                spanMin = lo;
                spanMax = hi;
            } else {
                if (lo < spanMin) {
                    spanMin = lo;
                }
                if (hi > spanMax) {
                    spanMax = hi;
                }
            }
            if (intervalCount < MAX_INTERVALS) {
                intervals[intervalCount].lo = lo;
                intervals[intervalCount].hi = hi;
                intervalCount++;
            } else {
                intervalOverflow = true;
            }
        };

        auto addEdge = [&](double x1, double y1, double x2, double y2) {
            if ((y1 <= yc) != (y2 <= yc)) {
                const double t = (yc - y1) / (y2 - y1);
                const double x = x1 + t * (x2 - x1);
                if (crossingCount < MAX_CROSSINGS) {
                    crossings[crossingCount++] = static_cast<float>(x);
                } else {
                    crossingOverflow = true;
                }
            }

            const double dx = x2 - x1;
            const double dy = y2 - y1;
            const double bandTop = static_cast<double>(ty);
            const double bandBottom = bandTop + 1.0;
            if (dy == 0.0) {
                if (y1 >= bandTop && y1 < bandBottom) {
                    addInterval(floorToInt(std::min(x1, x2)), floorToInt(std::max(x1, x2)));
                }
                return;
            }
            const double tA = (bandTop - y1) / dy;
            const double tB = (bandBottom - y1) / dy;
            const double tMin = std::min(tA, tB);
            const double tMax = std::max(tA, tB);
            if (tMax < 0.0 || tMin > 1.0) {
                return;
            }
            const double tLo = std::max(0.0, tMin);
            const double tHi = std::min(1.0, tMax);
            if (tLo > tHi) {
                return;
            }
            if (tLo == tHi) {
                const double onlyY = y1 + tLo * dy;
                if (onlyY < bandTop || onlyY >= bandBottom) {
                    return;
                }
            }
            const double xa = x1 + tLo * dx;
            const double xb = x1 + tHi * dx;
            addInterval(floorToInt(std::min(xa, xb)), floorToInt(std::max(xa, xb)));
        };

        Cursor cursor { outline, outlineEnd, 0 };
        bool decodeOk = true;
        for (uint16_t r = 0; r < ringCount && decodeOk; r++) {
            uint64_t pointCount = 0;
            if (!cursor.read(pointCount)) {
                decodeOk = false;
                break;
            }
            if (pointCount == 0) {
                continue;
            }
            uint64_t vx = 0;
            uint64_t vy = 0;
            if (!cursor.read(vx) || !cursor.read(vy)) {
                decodeOk = false;
                break;
            }
            int64_t lonUnits = zigzagDecode(vx);
            int64_t latUnits = zigzagDecode(vy);
            const double firstX = lonToWorldX(static_cast<double>(lonUnits) / UNIT, z) / TILE_SIZE;
            const double firstY = latToWorldY(static_cast<double>(latUnits) / UNIT, z) / TILE_SIZE;
            double prevX = firstX;
            double prevY = firstY;
            if (pointCount < 2) {
                continue;
            }
            for (uint64_t i = 1; i < pointCount; i++) {
                uint64_t dx = 0;
                uint64_t dy = 0;
                if (!cursor.read(dx) || !cursor.read(dy)) {
                    decodeOk = false;
                    break;
                }
                lonUnits += zigzagDecode(dx);
                latUnits += zigzagDecode(dy);
                const double x = lonToWorldX(static_cast<double>(lonUnits) / UNIT, z) / TILE_SIZE;
                const double y = latToWorldY(static_cast<double>(latUnits) / UNIT, z) / TILE_SIZE;
                addEdge(prevX, prevY, x, y);
                prevX = x;
                prevY = y;
            }
            if (!decodeOk) {
                break;
            }
            addEdge(prevX, prevY, firstX, firstY);
        }
        if (!decodeOk) {
            return visited;
        }

        if (crossingOverflow) {
            addInterval(rows.minX, rows.maxX);
        } else {
            for (int i = 1; i < crossingCount; i++) {
                const float value = crossings[i];
                int j = i - 1;
                while (j >= 0 && crossings[j] > value) {
                    crossings[j + 1] = crossings[j];
                    j--;
                }
                crossings[j + 1] = value;
            }
            for (int i = 0; i + 1 < crossingCount; i += 2) {
                const double xa = crossings[i];
                const double xb = crossings[i + 1];
                const int txMin = ceilToInt(xa - 0.5);
                const int txMax = ceilToInt(xb - 0.5) - 1;
                addInterval(txMin, txMax);
            }
        }

        auto emit = [&](int lo, int hi) -> bool {
            if (lo < 0) {
                lo = 0;
            }
            if (hi > maxTile) {
                hi = maxTile;
            }
            if (lo > hi) {
                return true;
            }
            for (int tx = lo; tx <= hi; tx++) {
                visited++;
                if (visit != nullptr) {
                    const TileKey tile { z, tx, ty };
                    if (!visit(tile, context)) {
                        return false;
                    }
                } else if (limit != INT64_MAX && visited > limit) {
                    return false;
                }
            }
            return true;
        };

        if (intervalOverflow) {
            if (!emit(spanMin, spanMax)) {
                return visited;
            }
        } else {
            for (int i = 1; i < intervalCount; i++) {
                const Interval current = intervals[i];
                int j = i - 1;
                while (j >= 0 && intervals[j].lo > current.lo) {
                    intervals[j + 1] = intervals[j];
                    j--;
                }
                intervals[j + 1] = current;
            }
            int merged = 0;
            for (int i = 0; i < intervalCount; i++) {
                if (merged == 0) {
                    intervals[merged++] = intervals[i];
                } else {
                    Interval& last = intervals[merged - 1];
                    if (intervals[i].lo <= last.hi + 1) {
                        if (intervals[i].hi > last.hi) {
                            last.hi = intervals[i].hi;
                        }
                    } else {
                        intervals[merged++] = intervals[i];
                    }
                }
            }
            for (int i = 0; i < merged; i++) {
                if (!emit(intervals[i].lo, intervals[i].hi)) {
                    return visited;
                }
            }
        }
    }
    return visited;
}

} // namespace maps
