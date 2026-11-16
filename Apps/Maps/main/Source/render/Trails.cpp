// SPDX-License-Identifier: GPL-3.0-or-later
#include "Trails.h"

#include "../platform/Platform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace maps {

namespace {

/** strnlen, which Tactility does not export (host/check_symbols.py). */
int boundedLength(const char* text, size_t size) {
    size_t length = 0;
    while (length < size && text[length] != '\0') {
        length++;
    }
    return static_cast<int>(length);
}

constexpr int MARGIN_PIXELS = 4;
constexpr double WORLD_TILE = 256.0;
constexpr uint32_t NO_ROUTE = 0xFFFFFFFFu;
constexpr int MAX_RUN_POINTS = 60000;
constexpr size_t LINE_HEADER_BYTES = 6;

constexpr uint16_t rgb(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

constexpr uint16_t COLOR_RED = rgb(210, 20, 20);
constexpr uint16_t COLOR_BLUE = rgb(30, 70, 220);
constexpr uint16_t COLOR_GREEN = rgb(20, 150, 40);
constexpr uint16_t COLOR_YELLOW = rgb(228, 186, 0);
constexpr uint16_t COLOR_BLACK = rgb(20, 20, 20);
constexpr uint16_t COLOR_WHITE = rgb(200, 200, 200);
constexpr uint16_t COLOR_ORANGE = rgb(240, 130, 0);
constexpr uint16_t COLOR_PURPLE = rgb(140, 40, 170);
constexpr uint16_t COLOR_BROWN = rgb(130, 80, 30);
constexpr uint16_t COLOR_BICYCLE = rgb(160, 0, 160);
constexpr uint16_t COLOR_MTB = rgb(190, 100, 20);
constexpr uint16_t COLOR_MONO = 0x4208;

uint16_t get16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t get32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

void put16(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* bytes, uint32_t value) {
    put16(bytes, value & 0xFFFF);
    put16(bytes + 2, value >> 16);
}

/** Cuts a NUL terminated UTF-8 string that ends inside a character at the character before. */
void trimUtf8(char* text, int& length) {
    if (length <= 0) {
        length = 0;
        text[0] = 0;
        return;
    }
    int i = length - 1;
    int back = 0;
    while (i >= 0 && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80 && back < 3) {
        i--;
        back++;
    }
    if (i < 0) {
        length = 0;
        text[0] = 0;
        return;
    }
    const uint8_t lead = static_cast<uint8_t>(text[i]);
    const int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && length - i < need) {
        length = i;
        text[length] = 0;
    }
}

/** Copies into a fixed field: truncated at a character boundary, NUL padded. */
void copyField(char* destination, size_t capacity, const char* source, int length) {
    int n = length < static_cast<int>(capacity) - 1 ? length : static_cast<int>(capacity) - 1;
    if (n < 0) {
        n = 0;
    }
    memcpy(destination, source, static_cast<size_t>(n));
    destination[n] = 0;
    trimUtf8(destination, n);
    memset(destination + n, 0, capacity - static_cast<size_t>(n));
}

bool equalsIgnoreCase(const char* text, size_t length, const char* lower) {
    if (strlen(lower) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return false;
        }
    }
    return true;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** A colour name or #rgb / #rrggbb up to length characters. */
bool parseColor(const char* text, size_t length, uint16_t& color) {
    if (text == nullptr || length == 0) {
        return false;
    }
    if (text[0] == '#') {
        int v[6];
        if (length != 7 && length != 4) {
            return false;
        }
        const size_t digits = length - 1;
        for (size_t i = 0; i < digits; i++) {
            v[i] = hexValue(text[i + 1]);
            if (v[i] < 0) {
                return false;
            }
        }
        if (digits == 3) {
            color = rgb(v[0] * 17, v[1] * 17, v[2] * 17);
        } else {
            color = rgb(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
        }
        return true;
    }
    struct Named {
        const char* name;
        uint16_t color;
    };
    static const Named NAMES[] = {
        { "red", COLOR_RED }, { "blue", COLOR_BLUE }, { "green", COLOR_GREEN }, { "yellow", COLOR_YELLOW },
        { "black", COLOR_BLACK }, { "white", COLOR_WHITE }, { "orange", COLOR_ORANGE }, { "purple", COLOR_PURPLE },
        { "brown", COLOR_BROWN },
    };
    for (const Named& named : NAMES) {
        if (equalsIgnoreCase(text, length, named.name)) {
            color = named.color;
            return true;
        }
    }
    return false;
}

uint16_t kindColor(TrailKind kind) {
    switch (kind) {
        case TrailKind::Bicycle:
            return COLOR_BICYCLE;
        case TrailKind::Mtb:
            return COLOR_MTB;
        default:
            return COLOR_RED;
    }
}

/** Percent-encodes into a buffer. */
struct Encoder {
    char* out;
    size_t size;
    size_t length;
    bool ok;

    void raw(const char* text) {
        for (; *text != 0; text++) {
            push(*text);
        }
    }
    void encoded(const char* text) {
        static const char HEX[] = "0123456789ABCDEF";
        for (; *text != 0; text++) {
            const uint8_t c = static_cast<uint8_t>(*text);
            const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            if (plain) {
                push(static_cast<char>(c));
            } else {
                push('%');
                push(HEX[c >> 4]);
                push(HEX[c & 15]);
            }
        }
    }
    void push(char c) {
        if (length + 1 < size) {
            out[length] = c;
        } else {
            ok = false;
        }
        length++;
    }
};

} // namespace

bool trailKindFromRoute(const char* route, TrailKind& kind) {
    if (route == nullptr) {
        return false;
    }
    if (strcmp(route, "hiking") == 0) {
        kind = TrailKind::Hiking;
    } else if (strcmp(route, "foot") == 0) {
        kind = TrailKind::Foot;
    } else if (strcmp(route, "bicycle") == 0) {
        kind = TrailKind::Bicycle;
    } else if (strcmp(route, "mtb") == 0) {
        kind = TrailKind::Mtb;
    } else {
        return false;
    }
    return true;
}

uint16_t trailColor(TrailKind kind, const char* osmcSymbol, const char* colourTag) {
    uint16_t color;
    if (osmcSymbol != nullptr) {
        size_t length = 0;
        while (osmcSymbol[length] != 0 && osmcSymbol[length] != ':') {
            length++;
        }
        if (parseColor(osmcSymbol, length, color)) {
            return color;
        }
    }
    if (colourTag != nullptr && parseColor(colourTag, strlen(colourTag), color)) {
        return color;
    }
    return kindColor(kind);
}

bool formatTrailQuery(const BoundingBox& box, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    out[0] = 0;
    if (!std::isfinite(box.south) || !std::isfinite(box.west) || !std::isfinite(box.north) || !std::isfinite(box.east) || box.south >= box.north ||
        box.west >= box.east || box.south < -90.0 || box.north > 90.0 || box.west < -180.0 || box.east > 180.0) {
        return false;
    }
    char area[96];
    const int written = snprintf(area, sizeof(area), "%.5f,%.5f,%.5f,%.5f", box.south, box.west, box.north, box.east);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(area)) {
        return false;
    }
    Encoder encoder { out, size, 0, true };
    encoder.raw("data=");
    encoder.encoded("[out:json][timeout:60];relation[\"route\"~\"^(hiking|foot|bicycle|mtb)$\"](");
    encoder.encoded(area);
    encoder.encoded(");out geom(");
    encoder.encoded(area);
    encoder.encoded(");");
    if (!encoder.ok) {
        out[0] = 0;
        return false;
    }
    out[encoder.length] = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// TrailTiler

bool TrailTiler::begin(const BoundingBox& box) {
    end();
    if (!std::isfinite(box.south) || !std::isfinite(box.west) || !std::isfinite(box.north) || !std::isfinite(box.east) || box.south >= box.north ||
        box.west >= box.east || box.south < -MAX_LATITUDE || box.north > MAX_LATITUDE || box.west < -180.0 || box.east > 180.0) {
        return false;
    }
    range = tileRange(box, TRAIL_ZOOM);
    const int64_t count = range.count();
    if (count <= 0 || count > TRAIL_MAX_TILES) {
        return false;
    }
    void* memory = largeAlloc(static_cast<size_t>(count) * sizeof(GrowArray<uint8_t>));
    if (memory == nullptr) {
        return false;
    }
    tiles = static_cast<GrowArray<uint8_t>*>(memory);
    for (int64_t i = 0; i < count; i++) {
        new (&tiles[i]) GrowArray<uint8_t>();
    }
    tileTotal = static_cast<int>(count);
    return true;
}

void TrailTiler::end() {
    if (tiles != nullptr) {
        for (int i = 0; i < tileTotal; i++) {
            tiles[i].~GrowArray<uint8_t>();
        }
        largeFree(tiles);
        tiles = nullptr;
    }
    tileTotal = 0;
    routes.release();
    pendingLines.release();
    pendingPoints.release();
    work.release();
    run.release();
    largeFree(ways);
    ways = nullptr;
    wayCapacity = 0;
    wayCount = 0;
    open = -1;
    error = false;
}

TileKey TrailTiler::tileAt(int index) const {
    const int width = range.maxX - range.minX + 1;
    if (index < 0 || index >= tileTotal || width <= 0) {
        return TileKey { TRAIL_ZOOM, 0, 0 };
    }
    return TileKey { TRAIL_ZOOM, range.minX + index % width, range.minY + index / width };
}

int TrailTiler::beginRoute() {
    if (tiles == nullptr || open >= 0 || routes.size() >= 65535) {
        return -1;
    }
    TrailRoute blank {};
    routes.push(blank);
    if (routes.failed()) {
        error = true;
        routes.clear();
        return -1;
    }
    pendingLines.clear();
    pendingPoints.clear();
    open = static_cast<int>(routes.size()) - 1;
    return open;
}

bool TrailTiler::addLine(int route, uint64_t wayId, const double* lonLat, int pointCount) {
    if (route < 0 || route != open || lonLat == nullptr) {
        return false;
    }
    if (pointCount < 2) {
        return true;
    }
    PendingLine line {};
    line.way = wayId;
    line.offset = static_cast<uint32_t>(pendingPoints.size());
    line.count = static_cast<uint32_t>(pointCount);
    if (!pendingPoints.reserve(pendingPoints.size() + static_cast<size_t>(pointCount) * 2)) {
        error = true;
        return false;
    }
    for (int i = 0; i < pointCount * 2; i++) {
        pendingPoints.push(lonLat[i]);
    }
    pendingLines.push(line);
    if (pendingLines.failed() || pendingPoints.failed()) {
        error = true;
        return false;
    }
    return true;
}

bool TrailTiler::growWays() {
    const size_t next = wayCapacity == 0 ? 256 : wayCapacity * 2;
    auto* table = static_cast<WayEntry*>(largeAlloc(next * sizeof(WayEntry)));
    if (table == nullptr) {
        return false;
    }
    for (size_t i = 0; i < next; i++) {
        table[i] = WayEntry { 0, 0, NO_ROUTE };
    }
    for (size_t i = 0; i < wayCapacity; i++) {
        if (ways[i].lastRoute == NO_ROUTE) {
            continue;
        }
        size_t slot = static_cast<size_t>((ways[i].way * 0x9E3779B97F4A7C15ull) >> 32) & (next - 1);
        while (table[slot].lastRoute != NO_ROUTE) {
            slot = (slot + 1) & (next - 1);
        }
        table[slot] = ways[i];
    }
    largeFree(ways);
    ways = table;
    wayCapacity = next;
    return true;
}

int TrailTiler::slotFor(uint64_t way, uint32_t route) {
    if ((wayCount + 1) * 2 > wayCapacity && !growWays()) {
        error = true;
        return 0;
    }
    size_t slot = static_cast<size_t>((way * 0x9E3779B97F4A7C15ull) >> 32) & (wayCapacity - 1);
    while (ways[slot].lastRoute != NO_ROUTE && ways[slot].way != way) {
        slot = (slot + 1) & (wayCapacity - 1);
    }
    WayEntry& entry = ways[slot];
    if (entry.lastRoute == NO_ROUTE) {
        entry.way = way;
        entry.count = 0;
        wayCount++;
    }
    uint32_t place;
    if (entry.lastRoute != route) {
        place = entry.count;
        entry.count++;
        entry.lastRoute = route;
    } else {
        place = entry.count > 0 ? entry.count - 1 : 0;
    }
    return place > static_cast<uint32_t>(TRAIL_MAX_SLOT) ? TRAIL_MAX_SLOT : static_cast<int>(place);
}

void TrailTiler::endRoute(int route, const TrailRoute& style, bool keep) {
    if (route < 0 || route != open) {
        return;
    }
    open = -1;
    if (keep) {
        routes[static_cast<size_t>(route)] = style;
        for (size_t i = 0; i < pendingLines.size(); i++) {
            const PendingLine line = pendingLines[i];
            const int slot = slotFor(line.way, static_cast<uint32_t>(route));
            clipToTiles(&pendingPoints[line.offset], static_cast<int>(line.count), route, slot);
        }
    } else {
        routes.pop();
    }
    pendingLines.clear();
    pendingPoints.clear();
}

void TrailTiler::clipToTiles(const double* lonLat, int count, int route, int slot) {
    if (!work.resize(static_cast<size_t>(count) * 2)) {
        error = true;
        return;
    }
    double minX = 1e30;
    double minY = 1e30;
    double maxX = -1e30;
    double maxY = -1e30;
    for (int i = 0; i < count; i++) {
        double lat = lonLat[i * 2 + 1];
        lat = lat < -MAX_LATITUDE ? -MAX_LATITUDE : lat > MAX_LATITUDE ? MAX_LATITUDE : lat;
        const double x = lonToWorldX(lonLat[i * 2], TRAIL_ZOOM);
        const double y = latToWorldY(lat, TRAIL_ZOOM);
        work[static_cast<size_t>(i) * 2] = x;
        work[static_cast<size_t>(i) * 2 + 1] = y;
        minX = x < minX ? x : minX;
        maxX = x > maxX ? x : maxX;
        minY = y < minY ? y : minY;
        maxY = y > maxY ? y : maxY;
    }
    int firstX = static_cast<int>(std::floor((minX - MARGIN_PIXELS) / WORLD_TILE));
    int lastX = static_cast<int>(std::floor((maxX + MARGIN_PIXELS) / WORLD_TILE));
    int firstY = static_cast<int>(std::floor((minY - MARGIN_PIXELS) / WORLD_TILE));
    int lastY = static_cast<int>(std::floor((maxY + MARGIN_PIXELS) / WORLD_TILE));
    firstX = firstX < range.minX ? range.minX : firstX;
    lastX = lastX > range.maxX ? range.maxX : lastX;
    firstY = firstY < range.minY ? range.minY : firstY;
    lastY = lastY > range.maxY ? range.maxY : lastY;
    const int width = range.maxX - range.minX + 1;
    for (int y = firstY; y <= lastY; y++) {
        for (int x = firstX; x <= lastX; x++) {
            clipToTile((y - range.minY) * width + (x - range.minX), &work[0], count, route, slot);
        }
    }
}

namespace {

/** One edge of Liang-Barsky clipping: false when the segment lies outside. */
bool clipEdge(double p, double q, double& t0, double& t1) {
    if (p == 0.0) {
        return q >= 0.0;
    }
    const double r = q / p;
    if (p < 0.0) {
        if (r > t1) {
            return false;
        }
        if (r > t0) {
            t0 = r;
        }
    } else {
        if (r < t0) {
            return false;
        }
        if (r < t1) {
            t1 = r;
        }
    }
    return true;
}

int16_t quantize(double value) {
    return static_cast<int16_t>(static_cast<int>(std::floor(value * 64.0 + 0.5)));
}

} // namespace

void TrailTiler::clipToTile(int index, const double* worldXY, int count, int route, int slot) {
    const TileKey tile = tileAt(index);
    const double originX = static_cast<double>(tile.x) * WORLD_TILE;
    const double originY = static_cast<double>(tile.y) * WORLD_TILE;
    const double low = -MARGIN_PIXELS;
    const double high = WORLD_TILE + MARGIN_PIXELS;
    run.clear();
    auto addPoint = [&](double x, double y) {
        const int16_t qx = quantize(x);
        const int16_t qy = quantize(y);
        const size_t n = run.size();
        if (n >= 2 && run[n - 2] == qx && run[n - 1] == qy) {
            return;
        }
        run.push(qx);
        run.push(qy);
    };
    auto flush = [&]() {
        if (run.size() >= 4) {
            appendLine(index, route, slot, run.begin(), static_cast<int>(run.size() / 2));
        }
        run.clear();
    };
    for (int i = 0; i + 1 < count; i++) {
        const double x0 = worldXY[i * 2] - originX;
        const double y0 = worldXY[i * 2 + 1] - originY;
        const double dx = worldXY[i * 2 + 2] - originX - x0;
        const double dy = worldXY[i * 2 + 3] - originY - y0;
        double t0 = 0.0;
        double t1 = 1.0;
        if (!clipEdge(-dx, x0 - low, t0, t1) || !clipEdge(dx, high - x0, t0, t1) || !clipEdge(-dy, y0 - low, t0, t1) || !clipEdge(dy, high - y0, t0, t1)) {
            flush();
            continue;
        }
        if (run.size() == 0) {
            addPoint(x0 + t0 * dx, y0 + t0 * dy);
        }
        addPoint(x0 + t1 * dx, y0 + t1 * dy);
        if (t1 < 1.0) {
            flush();
        } else if (static_cast<int>(run.size() / 2) >= MAX_RUN_POINTS) {
            const int16_t lastX = run[run.size() - 2];
            const int16_t lastY = run[run.size() - 1];
            flush();
            run.push(lastX);
            run.push(lastY);
        }
    }
    flush();
    if (run.failed()) {
        error = true;
    }
}

void TrailTiler::appendLine(int index, int route, int slot, const int16_t* points, int count) {
    GrowArray<uint8_t>& buffer = tiles[index];
    const size_t at = buffer.size();
    const size_t bytes = LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
    if (!buffer.resize(at + bytes)) {
        error = true;
        return;
    }
    uint8_t* p = buffer.begin() + at;
    put16(p, static_cast<uint32_t>(route));
    p[2] = static_cast<uint8_t>(slot);
    p[3] = 0;
    put16(p + 4, static_cast<uint32_t>(count));
    p += LINE_HEADER_BYTES;
    for (int i = 0; i < count * 2; i++) {
        put16(p + i * 2, static_cast<uint16_t>(points[i]));
    }
}

bool TrailTiler::buildTile(int index, GrowArray<uint8_t>& out) {
    out.clear();
    if (tiles == nullptr || index < 0 || index >= tileTotal || tiles[index].size() == 0) {
        return false;
    }
    const GrowArray<uint8_t>& buffer = tiles[index];
    const size_t routeTotal = routes.size();
    GrowArray<uint16_t> remap;
    if (!remap.resize(routeTotal)) {
        return false;
    }
    // Pass 1: which routes the tile uses
    uint32_t lineCount = 0;
    uint32_t pointCount = 0;
    size_t position = 0;
    while (position < buffer.size()) {
        if (position + LINE_HEADER_BYTES > buffer.size()) {
            return false;
        }
        const uint8_t* p = buffer.begin() + position;
        const uint32_t route = get16(p);
        const uint32_t count = get16(p + 4);
        if (route >= routeTotal || count < 2) {
            return false;
        }
        remap[route] = 1;
        lineCount++;
        pointCount += count;
        position += LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
    }
    if (position != buffer.size()) {
        return false;
    }
    uint32_t usedRoutes = 0;
    for (size_t r = 0; r < routeTotal; r++) {
        if (remap[r] != 0) {
            remap[r] = static_cast<uint16_t>(usedRoutes++);
        } else {
            remap[r] = 0xFFFF;
        }
    }
    const size_t total = TRAIL_HEADER_BYTES + static_cast<size_t>(usedRoutes) * TRAIL_ROUTE_BYTES + buffer.size();
    if (!out.resize(total)) {
        return false;
    }
    uint8_t* file = out.begin();
    memcpy(file, "TMTR", 4);
    file[4] = TRAIL_FILE_VERSION;
    file[5] = TRAIL_ZOOM;
    put16(file + 6, usedRoutes);
    put32(file + 8, lineCount);
    put32(file + 12, pointCount);
    const TileKey tile = tileAt(index);
    put16(file + 16, static_cast<uint32_t>(tile.x));
    put16(file + 18, static_cast<uint32_t>(tile.y));
    put32(file + 20, 0);
    uint8_t* entry = file + TRAIL_HEADER_BYTES;
    for (size_t r = 0; r < routeTotal; r++) {
        if (remap[r] == 0xFFFF) {
            continue;
        }
        const TrailRoute& route = routes[r];
        memset(entry, 0, TRAIL_ROUTE_BYTES);
        entry[0] = static_cast<uint8_t>(route.kind);
        put16(entry + 2, route.color);
        copyField(reinterpret_cast<char*>(entry + 4), 16, route.ref, boundedLength(route.ref, sizeof(route.ref)));
        copyField(reinterpret_cast<char*>(entry + 20), 64, route.name, boundedLength(route.name, sizeof(route.name)));
        entry += TRAIL_ROUTE_BYTES;
    }
    memcpy(entry, buffer.begin(), buffer.size());
    position = 0;
    while (position < buffer.size()) {
        uint8_t* p = entry + position;
        const uint32_t count = get16(p + 4);
        put16(p, remap[get16(p)]);
        position += LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
    }
    return true;
}

size_t TrailTiler::memoryBytes() const {
    size_t bytes = static_cast<size_t>(tileTotal) * sizeof(GrowArray<uint8_t>);
    for (int i = 0; i < tileTotal; i++) {
        bytes += tiles[i].allocated();
    }
    return bytes + routes.allocated() + pendingLines.allocated() + pendingPoints.allocated() + work.allocated() + run.allocated() +
        wayCapacity * sizeof(WayEntry);
}

// ---------------------------------------------------------------------------------------------------------------
// TrailParser

bool TrailParser::begin(TrailTiler* tilerToFill) {
    pointBuffer.release();
    tiler = tilerToFill;
    failedFlag = tilerToFill == nullptr;
    started = false;
    memset(containerIsObject, 0, sizeof(containerIsObject));
    for (Role& role : roles) {
        role = Role::Skip;
    }
    depth = 0;
    expect = Expect::Value;
    pendingRole = Role::Skip;
    pendingKey = Key::Other;
    pendingTag = Tag::None;
    lex = Lex::None;
    target = Target::None;
    textLength = 0;
    textFull = false;
    hex = 0;
    hexDigits = 0;
    highSurrogate = 0;
    literal = nullptr;
    literalIndex = 0;
    numberState = NumberState::Zero;
    numberKeep = false;
    numberLength = 0;
    numberOverflow = false;
    numberKey = Key::Other;
    numberRole = Role::Skip;
    routeIndex = -1;
    elementIsRelation = true;
    memberIsWay = false;
    haveRef = false;
    memberRef = 0;
    havePointLat = false;
    havePointLon = false;
    kept = 0;
    dropped = 0;
    lines = 0;
    points = 0;
    nulls = 0;
    return !failedFlag;
}

bool TrailParser::fail() {
    failedFlag = true;
    return false;
}

bool TrailParser::feed(const uint8_t* bytes, size_t size) {
    if (failedFlag) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        if (!step(bytes[i])) {
            failedFlag = true;
            return false;
        }
    }
    return true;
}

bool TrailParser::finish() {
    if (failedFlag) {
        return false;
    }
    if (lex == Lex::Number) {
        if (numberState == NumberState::Minus || numberState == NumberState::Dot || numberState == NumberState::ExpMark ||
            numberState == NumberState::ExpSign) {
            return fail();
        }
        lex = Lex::None;
        if (!finishNumber()) {
            return fail();
        }
    }
    if (lex != Lex::None || expect != Expect::Done || depth != 0 || !started) {
        return fail();
    }
    return true;
}

void TrailParser::appendByte(uint8_t byte) {
    if (target == Target::None) {
        return;
    }
    if (textLength < static_cast<int>(sizeof(text)) - 1) {
        text[textLength++] = static_cast<char>(byte);
    } else {
        textFull = true;
    }
}

void TrailParser::appendCodePoint(uint32_t codePoint) {
    if (target == Target::None || codePoint == 0) {
        return;
    }
    char encoded[4];
    int n;
    if (codePoint < 0x80) {
        encoded[0] = static_cast<char>(codePoint);
        n = 1;
    } else if (codePoint < 0x800) {
        encoded[0] = static_cast<char>(0xC0 | (codePoint >> 6));
        encoded[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
        n = 2;
    } else if (codePoint < 0x10000) {
        encoded[0] = static_cast<char>(0xE0 | (codePoint >> 12));
        encoded[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        encoded[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
        n = 3;
    } else {
        encoded[0] = static_cast<char>(0xF0 | (codePoint >> 18));
        encoded[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        encoded[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        encoded[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
        n = 4;
    }
    if (textLength + n > static_cast<int>(sizeof(text)) - 1) {
        textFull = true;
        return;
    }
    memcpy(text + textLength, encoded, static_cast<size_t>(n));
    textLength += n;
}

bool TrailParser::step(uint8_t b) {
    switch (lex) {
        case Lex::String:
            if (b == '"') {
                if (highSurrogate != 0) {
                    appendCodePoint(0xFFFD);
                    highSurrogate = 0;
                }
                lex = Lex::None;
                return finishString();
            }
            if (b == '\\') {
                lex = Lex::Escape;
                return true;
            }
            if (b < 0x20) {
                return false;
            }
            if (highSurrogate != 0) {
                appendCodePoint(0xFFFD);
                highSurrogate = 0;
            }
            appendByte(b);
            return true;
        case Lex::Escape: {
            char simple = 0;
            switch (b) {
                case '"':
                case '\\':
                case '/':
                    simple = static_cast<char>(b);
                    break;
                case 'b':
                    simple = '\b';
                    break;
                case 'f':
                    simple = '\f';
                    break;
                case 'n':
                    simple = '\n';
                    break;
                case 'r':
                    simple = '\r';
                    break;
                case 't':
                    simple = '\t';
                    break;
                case 'u':
                    lex = Lex::Unicode;
                    hex = 0;
                    hexDigits = 0;
                    return true;
                default:
                    return false;
            }
            if (highSurrogate != 0) {
                appendCodePoint(0xFFFD);
                highSurrogate = 0;
            }
            appendByte(static_cast<uint8_t>(simple));
            lex = Lex::String;
            return true;
        }
        case Lex::Unicode: {
            const int digit = hexValue(static_cast<char>(b));
            if (digit < 0) {
                return false;
            }
            hex = static_cast<uint16_t>((hex << 4) | digit);
            if (++hexDigits < 4) {
                return true;
            }
            lex = Lex::String;
            const uint32_t unit = hex;
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                if (highSurrogate != 0) {
                    appendCodePoint(0xFFFD);
                }
                highSurrogate = unit;
            } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                if (highSurrogate != 0) {
                    appendCodePoint(0x10000 + ((highSurrogate - 0xD800) << 10) + (unit - 0xDC00));
                    highSurrogate = 0;
                } else {
                    appendCodePoint(0xFFFD);
                }
            } else {
                if (highSurrogate != 0) {
                    appendCodePoint(0xFFFD);
                    highSurrogate = 0;
                }
                appendCodePoint(unit);
            }
            return true;
        }
        case Lex::Number: {
            bool consumed = false;
            if (!numberByte(b, consumed)) {
                return false;
            }
            if (consumed) {
                return true;
            }
            lex = Lex::None;
            if (!finishNumber()) {
                return false;
            }
            break;
        }
        case Lex::Literal:
            if (b != static_cast<uint8_t>(literal[literalIndex])) {
                return false;
            }
            literalIndex++;
            if (literal[literalIndex] != 0) {
                return true;
            }
            lex = Lex::None;
            if (literal[0] == 'n' && depth > 0 && roles[depth - 1] == Role::Geometry) {
                nulls++;
                if (pointBuffer.size() / 2 >= static_cast<size_t>(MAX_POINTS)) {
                    return false;
                }
                pointBuffer.push(std::nan(""));
                pointBuffer.push(std::nan(""));
                if (pointBuffer.failed()) {
                    return false;
                }
            }
            return finishValue();
        case Lex::None:
            break;
    }

    if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
        return true;
    }
    switch (expect) {
        case Expect::Done:
            return false;
        case Expect::Colon:
            if (b != ':') {
                return false;
            }
            expect = Expect::Value;
            return true;
        case Expect::CommaOrEnd:
            if (b == ',') {
                expect = containerIsObject[depth - 1] != 0 ? Expect::Key : Expect::Value;
                return true;
            }
            if (b == '}' || b == ']') {
                return closeContainer(b);
            }
            return false;
        case Expect::KeyOrEnd:
            if (b == '}') {
                return closeContainer(b);
            }
            [[fallthrough]];
        case Expect::Key:
            if (b != '"') {
                return false;
            }
            lex = Lex::String;
            target = Target::Key;
            textLength = 0;
            textFull = false;
            highSurrogate = 0;
            return true;
        case Expect::ValueOrEnd:
            if (b == ']') {
                return closeContainer(b);
            }
            return startValue(b);
        case Expect::Value:
            return startValue(b);
    }
    return false;
}

bool TrailParser::startValue(uint8_t b) {
    started = true;
    // The role this value takes
    Role role = Role::Skip;
    Role parent = Role::Skip;
    const bool object = b == '{';
    const bool array = b == '[';
    if (depth == 0) {
        if (object) {
            role = Role::Root;
        }
    } else {
        parent = roles[depth - 1];
        switch (parent) {
            case Role::Root:
                if (pendingKey == Key::Elements && array) {
                    role = Role::Elements;
                }
                break;
            case Role::Elements:
                if (object) {
                    role = Role::Element;
                }
                break;
            case Role::Element:
                if (pendingKey == Key::Members && array) {
                    role = Role::Members;
                } else if (pendingKey == Key::Tags && object) {
                    role = Role::Tags;
                }
                break;
            case Role::Members:
                if (object) {
                    role = Role::Member;
                }
                break;
            case Role::Member:
                if (pendingKey == Key::Geometry && array) {
                    role = Role::Geometry;
                }
                break;
            case Role::Geometry:
                if (object) {
                    role = Role::Point;
                }
                break;
            default:
                break;
        }
    }
    if (object || array) {
        return openContainer(object, role);
    }
    if (b == '"') {
        lex = Lex::String;
        textLength = 0;
        textFull = false;
        highSurrogate = 0;
        target = Target::None;
        if (parent == Role::Member && pendingKey == Key::Type) {
            target = Target::Type;
        } else if (parent == Role::Element && pendingKey == Key::Type) {
            target = Target::Type;
        } else if (parent == Role::Tags && pendingTag != Tag::None) {
            target = Target::Tag;
        }
        return true;
    }
    if (b == '-' || (b >= '0' && b <= '9')) {
        lex = Lex::Number;
        numberState = b == '-' ? NumberState::Minus : b == '0' ? NumberState::Zero : NumberState::Integer;
        numberKey = depth > 0 ? pendingKey : Key::Other;
        numberRole = parent;
        numberKeep = (parent == Role::Member && numberKey == Key::Ref) || (parent == Role::Point && (numberKey == Key::Lat || numberKey == Key::Lon));
        numberLength = 0;
        numberOverflow = false;
        if (numberKeep) {
            number[numberLength++] = static_cast<char>(b);
        }
        return true;
    }
    const char* word = b == 't' ? "true" : b == 'f' ? "false" : b == 'n' ? "null" : nullptr;
    if (word == nullptr) {
        return false;
    }
    lex = Lex::Literal;
    literal = word;
    literalIndex = 1;
    return true;
}

bool TrailParser::numberByte(uint8_t b, bool& consumed) {
    const bool digit = b >= '0' && b <= '9';
    NumberState next = numberState;
    consumed = true;
    switch (numberState) {
        case NumberState::Minus:
            if (b == '0') {
                next = NumberState::Zero;
            } else if (digit) {
                next = NumberState::Integer;
            } else {
                return false;
            }
            break;
        case NumberState::Zero:
            if (b == '.') {
                next = NumberState::Dot;
            } else if (b == 'e' || b == 'E') {
                next = NumberState::ExpMark;
            } else if (digit) {
                return false;
            } else {
                consumed = false;
            }
            break;
        case NumberState::Integer:
            if (digit) {
                break;
            }
            if (b == '.') {
                next = NumberState::Dot;
            } else if (b == 'e' || b == 'E') {
                next = NumberState::ExpMark;
            } else {
                consumed = false;
            }
            break;
        case NumberState::Dot:
            if (!digit) {
                return false;
            }
            next = NumberState::Fraction;
            break;
        case NumberState::Fraction:
            if (digit) {
                break;
            }
            if (b == 'e' || b == 'E') {
                next = NumberState::ExpMark;
            } else {
                consumed = false;
            }
            break;
        case NumberState::ExpMark:
            if (b == '+' || b == '-') {
                next = NumberState::ExpSign;
            } else if (digit) {
                next = NumberState::ExpDigits;
            } else {
                return false;
            }
            break;
        case NumberState::ExpSign:
            if (!digit) {
                return false;
            }
            next = NumberState::ExpDigits;
            break;
        case NumberState::ExpDigits:
            if (!digit) {
                consumed = false;
            }
            break;
    }
    if (consumed) {
        numberState = next;
        if (numberKeep) {
            if (numberLength < static_cast<int>(sizeof(number)) - 1) {
                number[numberLength++] = static_cast<char>(b);
            } else {
                numberOverflow = true;
            }
        }
    }
    return true;
}

bool TrailParser::finishNumber() {
    if (numberKeep) {
        if (numberOverflow) {
            return false;
        }
        number[numberLength] = 0;
        if (numberRole == Role::Member) {
            uint64_t value = 0;
            for (int i = 0; i < numberLength; i++) {
                const int digit = number[i] - '0';
                if (digit < 0 || digit > 9 || value > (UINT64_MAX - static_cast<uint64_t>(digit)) / 10) {
                    return false;
                }
                value = value * 10 + static_cast<uint64_t>(digit);
            }
            memberRef = value;
            haveRef = true;
        } else {
            char* end = nullptr;
            const double value = strtod(number, &end);
            if (end == number || *end != 0 || !std::isfinite(value)) {
                return false;
            }
            if (numberKey == Key::Lat) {
                if (value < -90.0 || value > 90.0) {
                    return false;
                }
                pointLat = value;
                havePointLat = true;
            } else {
                if (value < -180.0 || value > 180.0) {
                    return false;
                }
                pointLon = value;
                havePointLon = true;
            }
        }
    }
    return finishValue();
}

bool TrailParser::finishValue() {
    expect = depth == 0 ? Expect::Done : Expect::CommaOrEnd;
    return true;
}

bool TrailParser::finishString() {
    text[textLength] = 0;
    int length = textLength;
    trimUtf8(text, length);
    textLength = length;
    const Target finished = target;
    target = Target::None;
    if (finished == Target::Key) {
        const Role role = depth > 0 ? roles[depth - 1] : Role::Skip;
        pendingKey = Key::Other;
        pendingTag = Tag::None;
        if (!textFull) {
            if (role == Role::Root) {
                if (strcmp(text, "elements") == 0) {
                    pendingKey = Key::Elements;
                }
            } else if (role == Role::Element) {
                pendingKey = strcmp(text, "members") == 0 ? Key::Members : strcmp(text, "tags") == 0 ? Key::Tags : strcmp(text, "type") == 0 ? Key::Type : Key::Other;
            } else if (role == Role::Member) {
                pendingKey = strcmp(text, "type") == 0 ? Key::Type : strcmp(text, "geometry") == 0 ? Key::Geometry : strcmp(text, "ref") == 0 ? Key::Ref : Key::Other;
            } else if (role == Role::Point) {
                pendingKey = strcmp(text, "lat") == 0 ? Key::Lat : strcmp(text, "lon") == 0 ? Key::Lon : Key::Other;
            } else if (role == Role::Tags) {
                pendingTag = strcmp(text, "route") == 0 ? Tag::Route
                    : strcmp(text, "osmc:symbol") == 0 ? Tag::Symbol
                    : strcmp(text, "colour") == 0 ? Tag::Colour
                    : strcmp(text, "ref") == 0 ? Tag::Ref
                    : strcmp(text, "name") == 0 ? Tag::Name
                    : Tag::None;
            }
        }
        expect = Expect::Colon;
        return true;
    }
    if (finished == Target::Type) {
        const Role role = roles[depth - 1];
        if (role == Role::Member) {
            memberIsWay = strcmp(text, "way") == 0;
        } else {
            elementIsRelation = strcmp(text, "relation") == 0;
        }
    } else if (finished == Target::Tag) {
        switch (pendingTag) {
            case Tag::Route:
                copyField(tagRoute, sizeof(tagRoute), text, textLength);
                break;
            case Tag::Symbol:
                copyField(tagSymbol, sizeof(tagSymbol), text, textLength);
                break;
            case Tag::Colour:
                copyField(tagColour, sizeof(tagColour), text, textLength);
                break;
            case Tag::Ref:
                copyField(tagRef, sizeof(tagRef), text, textLength);
                break;
            case Tag::Name:
                copyField(tagName, sizeof(tagName), text, textLength);
                break;
            case Tag::None:
                break;
        }
    }
    return finishValue();
}

bool TrailParser::openContainer(bool object, Role role) {
    if (depth >= MAX_DEPTH) {
        return false;
    }
    containerIsObject[depth] = object ? 1 : 0;
    roles[depth] = role;
    depth++;
    expect = object ? Expect::KeyOrEnd : Expect::ValueOrEnd;
    pendingKey = Key::Other;
    pendingTag = Tag::None;
    switch (role) {
        case Role::Element:
            routeIndex = -1;
            elementIsRelation = true;
            memset(tagRoute, 0, sizeof(tagRoute));
            memset(tagSymbol, 0, sizeof(tagSymbol));
            memset(tagColour, 0, sizeof(tagColour));
            memset(tagRef, 0, sizeof(tagRef));
            memset(tagName, 0, sizeof(tagName));
            break;
        case Role::Members:
            if (routeIndex >= 0) {
                return false;
            }
            routeIndex = tiler->beginRoute();
            if (routeIndex < 0) {
                return false;
            }
            break;
        case Role::Member:
            memberIsWay = false;
            haveRef = false;
            memberRef = 0;
            pointBuffer.clear();
            break;
        case Role::Point:
            havePointLat = false;
            havePointLon = false;
            break;
        default:
            break;
    }
    return true;
}

bool TrailParser::closeContainer(uint8_t b) {
    if (depth <= 0 || (containerIsObject[depth - 1] != 0) != (b == '}')) {
        return false;
    }
    const Role role = roles[depth - 1];
    depth--;
    bool ok = true;
    switch (role) {
        case Role::Point:
            if (!havePointLat || !havePointLon || pointBuffer.size() / 2 >= static_cast<size_t>(MAX_POINTS)) {
                return false;
            }
            pointBuffer.push(pointLon);
            pointBuffer.push(pointLat);
            ok = !pointBuffer.failed();
            break;
        case Role::Member:
            ok = endMember();
            break;
        case Role::Element:
            ok = endElement();
            break;
        default:
            break;
    }
    return ok && finishValue();
}

bool TrailParser::endMember() {
    if (!memberIsWay || routeIndex < 0) {
        return true;
    }
    const double* data = pointBuffer.begin();
    const size_t total = pointBuffer.size() / 2;
    size_t start = 0;
    for (size_t i = 0; i <= total; i++) {
        const bool isNull = i < total && std::isnan(data[i * 2]);
        if (i < total && !isNull) {
            points++;
            continue;
        }
        const size_t length = i - start;
        if (length >= 2) {
            if (!tiler->addLine(routeIndex, haveRef ? memberRef : 0, data + start * 2, static_cast<int>(length))) {
                return false;
            }
            lines++;
        }
        start = i + 1;
    }
    pointBuffer.clear();
    return true;
}

bool TrailParser::endElement() {
    if (routeIndex < 0) {
        return true;
    }
    TrailRoute route {};
    TrailKind kind = TrailKind::Hiking;
    const bool keep = elementIsRelation && trailKindFromRoute(tagRoute, kind);
    if (keep) {
        route.kind = kind;
        route.color = trailColor(kind, tagSymbol, tagColour);
        memcpy(route.ref, tagRef, sizeof(route.ref));
        memcpy(route.name, tagName, sizeof(route.name));
        kept++;
    } else {
        dropped++;
    }
    tiler->endRoute(routeIndex, route, keep);
    routeIndex = -1;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Files and drawing

namespace {

struct TrailFile {
    uint32_t routeCount;
    uint32_t lineCount;
    uint32_t pointCount;
    int tileX;
    int tileY;
    const uint8_t* table;
    const uint8_t* lines;
    size_t lineBytes;
};

bool parseTrailFile(const uint8_t* file, size_t size, TrailFile& out) {
    if (file == nullptr || size < TRAIL_HEADER_BYTES || memcmp(file, "TMTR", 4) != 0 || file[4] != TRAIL_FILE_VERSION || file[5] != TRAIL_ZOOM) {
        return false;
    }
    out.routeCount = get16(file + 6);
    out.lineCount = get32(file + 8);
    out.pointCount = get32(file + 12);
    out.tileX = get16(file + 16);
    out.tileY = get16(file + 18);
    if (get32(file + 20) != 0 || out.tileX >= (1 << TRAIL_ZOOM) || out.tileY >= (1 << TRAIL_ZOOM)) {
        return false;
    }
    const uint64_t tableEnd = TRAIL_HEADER_BYTES + static_cast<uint64_t>(out.routeCount) * TRAIL_ROUTE_BYTES;
    if (tableEnd > size) {
        return false;
    }
    out.table = file + TRAIL_HEADER_BYTES;
    for (uint32_t r = 0; r < out.routeCount; r++) {
        const uint8_t* entry = out.table + static_cast<size_t>(r) * TRAIL_ROUTE_BYTES;
        if (entry[0] > static_cast<uint8_t>(TrailKind::Mtb) || entry[1] != 0 || entry[19] != 0 || entry[83] != 0) {
            return false;
        }
    }
    out.lines = file + tableEnd;
    out.lineBytes = size - static_cast<size_t>(tableEnd);
    size_t position = 0;
    uint64_t points = 0;
    for (uint32_t i = 0; i < out.lineCount; i++) {
        if (out.lineBytes - position < LINE_HEADER_BYTES) {
            return false;
        }
        const uint8_t* p = out.lines + position;
        const uint32_t count = get16(p + 4);
        if (get16(p) >= out.routeCount || p[2] > TRAIL_MAX_SLOT || p[3] != 0 || count < 2) {
            return false;
        }
        const size_t bytes = LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
        if (out.lineBytes - position < bytes) {
            return false;
        }
        points += count;
        position += bytes;
    }
    return position == out.lineBytes && points == out.pointCount;
}

constexpr int MAX_COLORS = 64;

struct Painter {
    Raster* raster;
    float scale;
    float originX;
    float originY;
    float step;
    float halfWidth;
    bool dots;
};

struct Vec {
    float x;
    float y;
};

Vec pointAt(const uint8_t* points, int i, const Painter& painter) {
    return Vec { static_cast<float>(static_cast<int16_t>(get16(points + i * 4))) * painter.scale - painter.originX,
        static_cast<float>(static_cast<int16_t>(get16(points + i * 4 + 2))) * painter.scale - painter.originY };
}

bool unitNormal(const uint8_t* points, int segment, const Painter& painter, Vec& normal) {
    const Vec a = pointAt(points, segment, painter);
    const Vec b = pointAt(points, segment + 1, painter);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-4f) {
        return false;
    }
    normal = Vec { dy / length, -dx / length };
    return true;
}

/** The vertex moved sideways by offset, with a mitre at bends. */
Vec offsetVertex(const uint8_t* points, int count, int i, float offset, const Painter& painter) {
    Vec p = pointAt(points, i, painter);
    if (offset == 0.0f) {
        return p;
    }
    Vec before {};
    Vec after {};
    const bool haveBefore = i > 0 && unitNormal(points, i - 1, painter, before);
    const bool haveAfter = i + 1 < count && unitNormal(points, i, painter, after);
    Vec n {};
    float factor = 1.0f;
    if (haveBefore && haveAfter) {
        Vec m { before.x + after.x, before.y + after.y };
        const float length = std::sqrt(m.x * m.x + m.y * m.y);
        if (length < 0.2f) {
            n = after;
        } else {
            n = Vec { m.x / length, m.y / length };
            const float cosine = n.x * after.x + n.y * after.y;
            factor = 1.0f / (cosine > 0.5f ? cosine : 0.5f);
        }
    } else if (haveBefore) {
        n = before;
    } else if (haveAfter) {
        n = after;
    } else {
        return p;
    }
    p.x += n.x * offset * factor;
    p.y += n.y * offset * factor;
    return p;
}

/** Adds one line to the raster. Returns whether any part may touch the tile. */
bool addTrailLine(const uint8_t* points, int count, int slot, const Painter& painter) {
    const float offset = static_cast<float>(slot) * painter.step;
    const float limit = static_cast<float>(TILE_SIZE) + painter.halfWidth + 1.0f;
    const float low = -painter.halfWidth - 1.0f;
    bool touched = false;
    Vec previous = offsetVertex(points, count, 0, offset, painter);
    for (int i = 1; i < count; i++) {
        const Vec current = offsetVertex(points, count, i, offset, painter);
        const bool outside = (previous.x < low && current.x < low) || (previous.y < low && current.y < low) ||
            (previous.x > limit && current.x > limit) || (previous.y > limit && current.y > limit);
        if (!outside) {
            painter.raster->addSegment(previous.x, previous.y, current.x, current.y, painter.halfWidth);
            if (painter.dots) {
                painter.raster->addDot(current.x, current.y, painter.halfWidth);
            }
            touched = true;
        }
        previous = current;
    }
    return touched;
}

uint16_t paletteColor(const Palette& palette, uint16_t color) {
    if (palette.binary) {
        return 0x0000;
    }
    if (&palette == &monochromePalette()) {
        return COLOR_MONO;
    }
    return color;
}

} // namespace

bool checkTrailHeader(const uint8_t* file, size_t size) {
    TrailFile parsed;
    return parseTrailFile(file, size, parsed);
}

bool trailFileTile(const uint8_t* file, size_t size, TileKey& tile) {
    TrailFile parsed;
    if (!parseTrailFile(file, size, parsed)) {
        return false;
    }
    tile = TileKey { TRAIL_ZOOM, parsed.tileX, parsed.tileY };
    return true;
}

int drawTrails(const uint8_t* file, size_t size, const TileKey& target, const Palette& palette, Raster& raster, uint16_t* pixels) {
    if (pixels == nullptr || target.z < TRAIL_ZOOM || target.z > 30 || raster.width() < TILE_SIZE || raster.height() < TILE_SIZE) {
        return 0;
    }
    TrailFile parsed;
    if (!parseTrailFile(file, size, parsed) || parsed.lineCount == 0) {
        return 0;
    }
    const int dz = target.z - TRAIL_ZOOM;
    if (target.x < 0 || target.y < 0 || (target.x >> dz) != parsed.tileX || (target.y >> dz) != parsed.tileY) {
        return 0;
    }
    const float width = target.z < 14 ? 1.5f : target.z < 16 ? 2.2f : 3.0f;
    Painter painter {};
    painter.raster = &raster;
    painter.scale = static_cast<float>(static_cast<uint32_t>(1) << dz) / 64.0f;
    painter.originX = static_cast<float>(target.x - (parsed.tileX << dz)) * static_cast<float>(TILE_SIZE);
    painter.originY = static_cast<float>(target.y - (parsed.tileY << dz)) * static_cast<float>(TILE_SIZE);
    painter.step = width * 1.1f;
    painter.dots = width >= 2.0f;
    raster.clear();
    raster.setAntialias(!palette.binary);

    // The colours in use, in the order of their first line
    uint16_t colors[MAX_COLORS];
    int colorCount = 0;
    {
        const uint8_t* p = parsed.lines;
        for (uint32_t i = 0; i < parsed.lineCount; i++) {
            const uint32_t count = get16(p + 4);
            const uint16_t color = paletteColor(palette, get16(parsed.table + static_cast<size_t>(get16(p)) * TRAIL_ROUTE_BYTES + 2));
            bool known = false;
            for (int c = 0; c < colorCount; c++) {
                known = known || colors[c] == color;
            }
            if (!known && colorCount < MAX_COLORS) {
                colors[colorCount++] = color;
            }
            p += LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
        }
    }
    auto bucketOf = [&](uint16_t color) {
        for (int c = 0; c < colorCount; c++) {
            if (colors[c] == color) {
                return c;
            }
        }
        return colorCount - 1;
    };

    int drawn = 0;
    if (!palette.binary) {
        painter.halfWidth = (width + 1.6f) / 2.0f;
        const uint8_t* p = parsed.lines;
        for (uint32_t i = 0; i < parsed.lineCount; i++) {
            const uint32_t count = get16(p + 4);
            addTrailLine(p + LINE_HEADER_BYTES, static_cast<int>(count), p[2], painter);
            p += LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
        }
        raster.fill(pixels, TILE_SIZE, 0xFFFF, 150);
    }
    painter.halfWidth = width / 2.0f;
    for (int bucket = 0; bucket < colorCount; bucket++) {
        const uint8_t* p = parsed.lines;
        for (uint32_t i = 0; i < parsed.lineCount; i++) {
            const uint32_t count = get16(p + 4);
            const uint16_t color = paletteColor(palette, get16(parsed.table + static_cast<size_t>(get16(p)) * TRAIL_ROUTE_BYTES + 2));
            if (bucketOf(color) == bucket && addTrailLine(p + LINE_HEADER_BYTES, static_cast<int>(count), p[2], painter)) {
                drawn++;
            }
            p += LINE_HEADER_BYTES + static_cast<size_t>(count) * 4;
        }
        raster.fill(pixels, TILE_SIZE, colors[bucket], 255);
    }
    raster.setAntialias(true);
    return drawn;
}

} // namespace maps
