// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/GrowArray.h"
#include "../tiles/TileMath.h"
#include "Raster.h"
#include "Style.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Marked trails: hiking, foot, bicycle and mountain bike route relations of OpenStreetMap, from the Overpass API,
 * drawn over the map.
 *
 * Trail files: one per z12 tile, "<region>/12/<x>/<y>.trl" next to the tile's .pbf. All numbers little-endian.
 *   header, 24 bytes: "TMTR", version (1), zoom (12), routeCount u16, lineCount u32, pointCount u32 (all lines),
 *                     tile x u16, tile y u16, reserved u32 (0)
 *   route table, routeCount entries of 84 bytes: kind u8 (TrailKind), reserved u8 (0), colour u16 (RGB565),
 *                     ref[16] and name[64], UTF-8, NUL padded, the last byte always NUL
 *   lines, lineCount entries: route index u16, slot u8 (0..3), reserved u8 (0), point count u16 (at least 2),
 *                     then point count pairs of x i16, y i16
 * Points are in 1/64 pixel of the z12 tile, relative to its top-left corner. A line is clipped to the tile with a
 * margin of 4 pixels outside it (coordinates -256..16640), so that neighbouring tiles join without gaps.
 * A slot is the place of a route on a way that several routes share (0 for the first route that has the way):
 * the lines are drawn side by side, so that a path with a red and a blue mark shows both.
 * The file size must be exactly the sum of its parts: readers check every count against it.
 *
 * The query asks for the relations in the region's box with their geometry ("out geom"): the answer is a JSON
 * document of the order TrailParser expects (members before tags is fine, any order works). Members outside the
 * box come with null points, which break the line there.
 */
constexpr int TRAIL_ZOOM = 12;
constexpr int TRAIL_MAX_SLOT = 3;
constexpr size_t TRAIL_HEADER_BYTES = 24;
constexpr size_t TRAIL_ROUTE_BYTES = 84;
constexpr uint8_t TRAIL_FILE_VERSION = 1;
/** Most z12 tiles one region may have (a region is at most 300 tiles a download). */
constexpr int TRAIL_MAX_TILES = 1024;

enum class TrailKind : uint8_t { Hiking, Foot, Bicycle, Mtb };

struct TrailRoute {
    TrailKind kind;
    /** RGB565 */
    uint16_t color;
    char ref[16];
    char name[64];
};

/** The POST body for the region's query: "data=" + URL-encoded query, with the box in the order south, west, north, east, 5 decimals. */
bool formatTrailQuery(const BoundingBox& box, char* out, size_t size);

/**
 * The colour of a route from its tags: the first field of osmc:symbol, else colour, else by kind. Each may be
 * nullptr or empty. A colour is a name (red, blue, green, yellow, black, white, orange, purple, brown) or #rrggbb.
 */
uint16_t trailColor(TrailKind kind, const char* osmcSymbol, const char* colourTag);
/** The kind of a route tag value ("hiking", "foot", "bicycle", "mtb"), false for any other. */
bool trailKindFromRoute(const char* route, TrailKind& kind);

/**
 * Collects the lines of a region per z12 tile in PSRAM, then gives each tile's .trl file. One route is open at a
 * time: beginRoute, addLine for its member ways, endRoute. The lines of a route wait until endRoute, because its
 * tags (and so whether it is kept and in which colour) come after the members in the answer.
 */
class TrailTiler {
public:
    TrailTiler() = default;
    ~TrailTiler() { end(); }
    TrailTiler(const TrailTiler&) = delete;
    TrailTiler& operator=(const TrailTiler&) = delete;

    /** The z12 tiles touching the box only. False for an invalid box or more than TRAIL_MAX_TILES tiles, or no memory. */
    bool begin(const BoundingBox& box);
    void end();
    /** Index of a new route (its tags come later), -1 when there is no room (65535 routes) or one is open. */
    int beginRoute();
    /** One stretch of a member way (no nulls inside: the parser splits at them), lon/lat pairs. */
    bool addLine(int route, uint64_t wayId, const double* lonLat, int pointCount);
    /** keep=false drops the lines of the route (and its index is given out again). */
    void endRoute(int route, const TrailRoute& style, bool keep);
    int tileCount() const { return tileTotal; }
    TileKey tileAt(int index) const;
    /**
     * The .trl bytes of one tile into out (replacing its content). False when the tile has no lines, unless evenEmpty:
     * then a file without routes (the header only) says that the tile was asked for.
     */
    bool buildTile(int index, GrowArray<uint8_t>& out, bool evenEmpty = false);
    size_t memoryBytes() const;
    /** Routes kept so far. */
    int routeCount() const { return static_cast<int>(routes.size()); }
    bool failed() const { return error; }

private:
    struct WayEntry {
        uint64_t way;
        uint32_t count;
        uint32_t lastRoute;
    };
    struct PendingLine {
        uint64_t way;
        uint32_t offset;
        uint32_t count;
    };

    int slotFor(uint64_t way, uint32_t route);
    bool growWays();
    void clipToTiles(const double* lonLat, int count, int route, int slot);
    void clipToTile(int index, const double* worldXY, int count, int route, int slot);
    void appendLine(int index, int route, int slot, const int16_t* points, int count);

    TileRange range {};
    int tileTotal = 0;
    GrowArray<uint8_t>* tiles = nullptr;
    GrowArray<TrailRoute> routes;
    GrowArray<PendingLine> pendingLines;
    GrowArray<double> pendingPoints;
    GrowArray<double> work;
    GrowArray<int16_t> run;
    WayEntry* ways = nullptr;
    size_t wayCapacity = 0;
    size_t wayCount = 0;
    int open = -1;
    bool error = false;
};

/**
 * Streaming JSON parser for the Overpass answer: bytes in pieces of any size (1 byte to 64 KB), never the document
 * in memory. Only one member way is held at a time. The object is about 0.5 KB: keep it off the stack.
 */
class TrailParser {
public:
    TrailParser() = default;
    ~TrailParser() { pointBuffer.release(); }
    TrailParser(const TrailParser&) = delete;
    TrailParser& operator=(const TrailParser&) = delete;

    bool begin(TrailTiler* tiler);
    /** False once the JSON is malformed (then stays false). */
    bool feed(const uint8_t* bytes, size_t size);
    /** False when the document did not end cleanly. */
    bool finish();
    /** Routes kept. */
    int routeCount() const { return kept; }
    /** Relations of other kinds, dropped. */
    int droppedCount() const { return dropped; }
    /** Lines given to the tiler (stretches of at least 2 points between nulls). */
    int lineCount() const { return lines; }
    /** Points of way members, null points not counted. */
    int pointCount() const { return points; }
    int nullCount() const { return nulls; }

private:
    enum class Role : uint8_t { Skip, Root, Elements, Element, Members, Member, Geometry, Point, Tags };
    enum class Expect : uint8_t {
        Value,
        ValueOrEnd,
        KeyOrEnd,
        Key,
        Colon,
        CommaOrEnd,
        Done
    };
    enum class Lex : uint8_t {
        None,
        String,
        Escape,
        Unicode,
        Number,
        Literal
    };
    enum class Key : uint8_t { Other, Elements, Members, Tags, Type, Geometry, Ref, Lat, Lon };
    enum class Tag : uint8_t { None, Route, Symbol, Colour, Ref, Name };
    enum class Target : uint8_t { None, Key, Type, Tag };
    enum class NumberState : uint8_t { Minus, Zero, Integer, Dot, Fraction, ExpMark, ExpSign, ExpDigits };

    static constexpr int MAX_DEPTH = 24;
    static constexpr int MAX_POINTS = 200000;

    bool step(uint8_t byte);
    bool startValue(uint8_t byte);
    bool finishValue();
    bool closeContainer(uint8_t byte);
    bool openContainer(bool object, Role role);
    bool finishString();
    bool finishNumber();
    bool numberByte(uint8_t byte, bool& consumed);
    void appendByte(uint8_t byte);
    void appendCodePoint(uint32_t codePoint);
    bool endElement();
    bool endMember();
    bool fail();

    TrailTiler* tiler = nullptr;
    bool failedFlag = false;
    bool started = false;

    // Grammar
    uint8_t containerIsObject[MAX_DEPTH] = {};
    Role roles[MAX_DEPTH] = {};
    int depth = 0;
    Expect expect = Expect::Value;
    Role pendingRole = Role::Skip;
    Key pendingKey = Key::Other;
    Tag pendingTag = Tag::None;

    // Lexer
    Lex lex = Lex::None;
    Target target = Target::None;
    char text[72] = {};
    int textLength = 0;
    bool textFull = false;
    uint16_t hex = 0;
    int hexDigits = 0;
    uint32_t highSurrogate = 0;
    const char* literal = nullptr;
    int literalIndex = 0;
    NumberState numberState = NumberState::Zero;
    bool numberKeep = false;
    char number[40] = {};
    int numberLength = 0;
    bool numberOverflow = false;
    Key numberKey = Key::Other;
    Role numberRole = Role::Skip;

    // Current element
    int routeIndex = -1;
    bool elementIsRelation = true;
    char tagRoute[16] = {};
    char tagSymbol[48] = {};
    char tagColour[24] = {};
    char tagRef[16] = {};
    char tagName[64] = {};

    // Current member
    bool memberIsWay = false;
    bool haveRef = false;
    uint64_t memberRef = 0;
    GrowArray<double> pointBuffer;
    bool havePointLat = false;
    bool havePointLon = false;
    double pointLat = 0.0;
    double pointLon = 0.0;

    int kept = 0;
    int dropped = 0;
    int lines = 0;
    int points = 0;
    int nulls = 0;
};

/**
 * Draws a .trl file's lines into a rendered 256x256 tile of zoom 12 or more inside that z12 tile. The raster must
 * be at least 256x256 (it is left empty). Returns the number of lines drawn (touching the tile), 0 for a damaged file,
 * a file of another tile or a zoom below 12. Width by zoom: 1.5 px (z12-13), 2.2 px (z14-15), 3 px (z16 and more),
 * over a thin light casing; the monochrome palette draws dark grey, the binary one black without casing.
 */
int drawTrails(const uint8_t* file, size_t size, const TileKey& target, const Palette& palette, Raster& raster, uint16_t* pixels);
/** True for a file that is whole: header, route table and every line add up to exactly size. */
bool checkTrailHeader(const uint8_t* file, size_t size);
/** The z12 tile of a .trl file whose header is whole. */
bool trailFileTile(const uint8_t* file, size_t size, TileKey& tile);

} // namespace maps
