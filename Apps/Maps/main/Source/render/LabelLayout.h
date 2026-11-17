// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PreparedLabels.h"

#include "../tiles/TileMath.h"
#include "../ui/Canvas.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** One glyph of a label font: its alpha mask (one byte per pixel, rows of width bytes) relative to the pen. */
struct LabelGlyph {
    uint32_t id;
    int advanceX16;
    /** Left edge of the mask from the pen, and its top edge from the baseline (negative is above it). */
    int left;
    int top;
    int width;
    int height;
    const uint8_t* alpha;
};

/** A font that gives glyph masks, so labels can be drawn with a halo and turned along a line. */
class LabelFont {
public:
    virtual ~LabelFont() = default;
    virtual int ascent() const = 0;
    virtual int lineHeight() const = 0;
    /**
     * The glyph of a codepoint; with withMask its alpha is decoded and stays valid until the next call, otherwise alpha
     * is nullptr (measuring only). False when the font has none.
     */
    virtual bool glyph(uint32_t codepoint, LabelGlyph& out, bool withMask) = 0;
    virtual int kerningX16(uint32_t leftId, uint32_t rightId) = 0;
};

/** How many labels: Off, Few (the important places and roads only, from higher zoom levels), Normal. */
enum class LabelDensity : uint8_t {
    Off,
    Few,
    Normal
};

/** Colours of the labels in the current palette, and whether the display only has black and white. */
struct LabelStyle {
    uint16_t place;
    uint16_t road;
    uint16_t water;
    uint16_t peak;
    uint16_t halo;
    bool binary;
    /** House numbers: small and quiet. */
    uint16_t houseNumber = 0x7BEF;
    /**
     * Peaks, saddles, cliffs and volcanoes: only in the outdoor style. They go with the POIs: their icons first, their
     * names only where room is left.
     */
    bool peaks = true;
};

/** How a POI type looks: its icon (an alpha mask, iconSize x iconSize), colour, and the first zoom it shows at. */
struct PoiAppearance {
    const uint8_t* icon;
    int iconSize;
    uint16_t color;
    uint8_t minZoom;
};

/** Gives the appearance of a POI type; false for types that are never shown. */
using PoiLookup = bool (*)(uint8_t poiType, PoiAppearance& out);

/** A placed POI or house number, for the sheet: its kind, POI type, name ("" when it has none) and screen point. */
struct PlacedItem {
    LabelKind kind;
    uint8_t poiType;
    const char* name;
    int x;
    int y;
};

/**
 * Where the tile a label section belongs to is on screen: screen = (a * u + b * v + x, c * u + d * v + y) for a point
 * u, v in the tile's 2048 extent. Scale and, later, rotation of the view are in a, b, c, d.
 */
struct LabelTransform {
    float a;
    float b;
    float c;
    float d;
    float x;
    float y;
};

/**
 * Labels placed in screen space for one view: names of places, peaks and water at their points, street and river
 * names along straight stretches of their lines. Labels stay upright, never overlap (a collision grid, the most
 * important first: city, town, village, suburb, major road, minor road, water, peak) and a name that several tiles
 * carry shows once, a line name at most once per LINE_REPEAT_PIXELS.
 *
 * Each placed label is drawn once into a small sprite with its halo, in a fixed arena, so drawing a frame only blends
 * the sprites, and a pan moves them without placing again.
 */
class LabelLayout {
public:
    static constexpr int MAX_LABELS = 160;
    static constexpr int MAX_CANDIDATES = 640;
    /** POIs and house numbers stop being taken in once the candidates are this full, so names always find room. */
    static constexpr int POI_CANDIDATE_LIMIT = MAX_CANDIDATES * 3 / 4;
    static constexpr int NUMBER_CANDIDATE_LIMIT = MAX_CANDIDATES / 2;
    /** POI names show next to their icon from this zoom. */
    static constexpr int POI_NAME_ZOOM = 17;
    /**
     * From this zoom nothing is left out: every POI of the tiles on screen is a candidate, named or not, whatever its
     * rank. Two icons that collide both stay: the later one moves to the nearest free spot within ICON_MOVE_PIXELS,
     * or shrinks to a dot when there is none. Only names give way. Barriers (types that start at BARRIER_ZOOM) show
     * from the zoom above.
     */
    static constexpr int POI_ALL_ZOOM = 18;
    static constexpr int BARRIER_ZOOM = 19;
    static constexpr int ICON_MOVE_PIXELS = 12;
    /** The dot that stands for an icon that has no room, a small alpha mask. */
    static constexpr int DOT_SIZE = 5;
    static constexpr int LINE_REPEAT_PIXELS = 300;
    /** Sprite pixels for all labels of a view, two bytes each (text and halo coverage). */
    static constexpr size_t ARENA_BYTES = 192 * 1024;
    /** Room for the names of the candidates of one view. */
    static constexpr size_t TEXT_BYTES = 24 * 1024;

    ~LabelLayout();
    /** Allocates the arena; allocate() comes from the platform so the arena can be in PSRAM. */
    bool begin(void* (*allocate)(size_t), void (*release)(void*));
    void end();

    /** Fonts by size: small (roads, water, peaks, villages), normal (towns), large (cities, countries). */
    void setFonts(LabelFont* small, LabelFont* normal, LabelFont* large);
    void setStyle(const LabelStyle& value) { style = value; }
    void setDensity(LabelDensity value) { density = value; }
    void setPreferLatin(bool value) { preferLatin = value; }
    /** Without a lookup no POIs are shown. */
    void setPoiLookup(PoiLookup value) { poiLookup = value; }

    /** Starts a new placement for a screen of width x height at a view zoom level. */
    /**
     * worldLeft and worldTop: the map position of the screen's top left corner in pixels of the view's zoom. With
     * them the placement is the same wherever the view is panned to; without (0, 0) it is tied to the screen.
     */
    void beginView(int width, int height, int viewZoom, double worldLeft = 0.0, double worldTop = 0.0);
    /**
     * The same for a turned map: the screen centre's map position and the bearing's cosine and sine (screen offset =
     * [cos sin; -sin cos] x map offset, as the label transforms). Labels keep their identity and place across pans.
     */
    void beginTurnedView(int width, int height, int viewZoom, double worldLeft, double worldTop, double centerX, double centerY,
        float cosine, float sine);
    /**
     * Adds the labels of one source tile's label section. With cells (a split tile's label file) only the POI and house
     * number cells near the screen are read.
     */
    void addSection(const uint8_t* section, size_t size, int count, const LabelTransform& transform, const LabelCells* cells = nullptr);
    /** Places the candidates and draws their sprites. Returns the number of labels placed. */
    int place();
    /** Drops all labels, for example when the view changed so much that they would be wrong. */
    void clear();

    /** Draws the placed labels moved by offset pixels; lineLabels false leaves street and river names out. */
    void draw(Canvas& canvas, int offsetX, int offsetY, bool lineLabels) const;
    int count() const { return placedCount; }
    int candidateTotal() const { return candidateCount; }
    /** Tests: the identity (name and map position hash) of candidate i of the last view. */
    uint32_t candidateOrder(int index) const { return candidates[index].order; }
    /** Records read by the addSection() calls since beginView(), for the log. */
    int recordsRead() const { return recordCount; }
    /** Time the last place() spent drawing sprites, set through setClock(). */
    uint32_t spriteMicros() const { return spriteTime; }
    void setClock(uint32_t (*micros)()) { clock = micros; }
    /** For tests: the box a placed label covers on screen. */
    void bounds(int index, int& x, int& y, int& w, int& h) const;
    bool isLineLabel(int index) const { return placed[index].line; }

    /** A placed label that can be selected (a POI or a house number); false for the others. */
    bool item(int index, PlacedItem& out) const;
    /** The selectable label nearest to (x, y) within radius pixels (offset: how far the labels moved since placed); -1 if none. */
    int pick(int x, int y, int radius, int offsetX = 0, int offsetY = 0) const;
    /**
     * The selectable labels by distance from (x, y), nearest first, at most max of them, into out. Returns how many.
     * Enter walks this list: the first press takes the nearest, the next the one after it.
     */
    int byDistance(int x, int y, int* out, int max, int offsetX = 0, int offsetY = 0) const;
    const char* text(int index) const { return texts + placed[index].textOffset; }

private:
    struct Candidate {
        uint32_t textOffset;
        uint16_t textLength;
        uint8_t priority;
        uint8_t fontSize;
        uint16_t color;
        bool line;
        /** Point labels: the point; line labels: index of the first point in linePoints. */
        float x;
        float y;
        uint16_t firstPoint;
        uint16_t pointCount;
        int32_t rank;
        LabelKind kind;
        uint8_t poiType;
        uint8_t iconSize;
        const uint8_t* icon;
        /** Where the icon is drawn: the point, or the free spot it moved to (POIs from POI_ALL_ZOOM). */
        float iconX;
        float iconY;
        /** The icon is the dot: no free spot was near, and no name is drawn. */
        bool dot;
        /** Order among candidates of the same priority and rank, from the name and the map position: the label's identity. */
        uint32_t order;
        /** It was on screen in the last placement. */
        bool sticky;
    };
    struct Placed {
        uint32_t textOffset;
        int16_t x;
        int16_t y;
        int16_t width;
        int16_t height;
        uint16_t color;
        bool line;
        uint32_t spriteOffset;
        LabelKind kind;
        uint8_t poiType;
        /** The labelled point on screen when placed (for POIs and house numbers). */
        int16_t anchorX;
        int16_t anchorY;
        /** The candidate's order number. */
        uint32_t id;
    };
    /** A label of the last placement: its identity and its centre on the map (pixels of zoom 24). */
    struct Kept {
        uint32_t id;
        double worldX;
        double worldY;
    };

    bool wants(const LabelRecord& record, uint8_t& priority, uint8_t& fontSize, uint16_t& color) const;
    LabelFont* fontFor(int size) const;
    int textWidth(LabelFont& font, const char* text, int length);
    /** textWidth of a candidate's name, remembered across placements (the same names come back view after view). */
    int candidateWidth(LabelFont& font, const Candidate& candidate);
    struct WidthEntry {
        uint32_t hash;
        uint16_t width;
        uint16_t used;
    };
    static constexpr int WIDTH_ENTRIES = 1024;
    WidthEntry* widths = nullptr;
    /** The cells a box covers, cut to the grid; false when it lies outside it. */
    bool gridRange(float cx, float cy, float halfWidth, float halfHeight, int& left, int& top, int& right, int& bottom) const;
    /** Whether any part of a box is on the screen. */
    bool onScreen(float cx, float cy, float halfWidth, float halfHeight) const;
    bool gridFree(float cx, float cy, float halfWidth, float halfHeight, float angle) const;
    void gridMark(float cx, float cy, float halfWidth, float halfHeight, float angle, bool taken = true);
    bool placePoint(const Candidate& candidate);
    /** Takes the place of a POI's icon; its sprite (with the name where there is room) is drawn by finishPois(). */
    bool placePoiIcon(Candidate& candidate);
    /** Whether a placed label can be picked (a tap, Enter): POIs, house numbers, peaks. */
    static bool selectable(const Placed& label);
    /** Whether a candidate is placed as an icon with the name beside it: POIs, and peaks that have an icon. */
    static bool hasIcon(const Candidate& candidate);
    void finishPois();
    int placeLine(const Candidate& candidate);
    /** Draws text turned by angle (radians) around the centre of a new sprite; false when the arena is full. */
    bool drawSprite(const Candidate& candidate, float centerX, float centerY, float angle, int textPixels, bool withText = true);
    bool alreadyPlaced(const Candidate& candidate, float x, float y) const;

    void* (*allocateMemory)(size_t) = nullptr;
    void (*releaseMemory)(void*) = nullptr;
    uint8_t* arena = nullptr;
    size_t arenaUsed = 0;
    /**
     * The sprites of POI icons drawn without a name in this pass: every POI of a kind shows the same icon, so its
     * sprite (mask and halo) is drawn once and shared (at zoom 17 most labels are such icons).
     */
    struct IconSprite {
        const uint8_t* icon;
        uint32_t offset;
        int16_t size;
    };
    static constexpr int ICON_SPRITES = 48;
    IconSprite iconSprites[ICON_SPRITES] = {};
    int iconSpriteCount = 0;
    char* texts = nullptr;
    size_t textsUsed = 0;
    Candidate* candidates = nullptr;
    int candidateCount = 0;
    float* linePoints = nullptr;
    int linePointCount = 0;
    Placed placed[MAX_LABELS] {};
    int placedCount = 0;
    /** One bit per cell of the screen and a margin around it; the cells are fixed to the map (see gridRange). */
    uint32_t grid[1024] {};
    double originX = 0.0;
    double originY = 0.0;
    /** The view's turn (cos 1, sin 0 when north is up) and its centre on the map, for screenToMap / mapToScreen. */
    double viewCenterX = 0.0;
    double viewCenterY = 0.0;
    float viewCos = 1.0f;
    float viewSin = 0.0f;
    /** Map pixels (of this view's zoom) of a screen point, and back; the same frame for every pan at one bearing. */
    void screenToMap(double sx, double sy, double& mx, double& my) const;
    void mapToScreen(double mx, double my, double& sx, double& sy) const;
    double gridBaseX = 0.0;
    double gridBaseY = 0.0;
    /** POIs whose icon has a place, as candidate indexes, until finishPois() draws them. */
    uint16_t pending[MAX_LABELS] {};
    int pendingCount = 0;
    Kept kept[MAX_LABELS] {};
    /** placeLine's distances along the line it is placing. */
    float lineLengths[LabelReader::MAX_POINTS] {};
    int keptCount = 0;
    int gridColumns = 0;
    int gridRows = 0;
    int screenWidth = 0;
    int screenHeight = 0;
    int zoom = 0;
    LabelFont* fonts[3] {};
    LabelStyle style {};
    LabelDensity density = LabelDensity::Normal;
    bool preferLatin = false;
    PoiLookup poiLookup = nullptr;
    uint32_t (*clock)() = nullptr;
    uint32_t spriteTime = 0;
    int recordCount = 0;
    void addRecords(const uint8_t* bytes, size_t size, int count, const LabelTransform& transform);
    bool cellNearScreen(int cell, const LabelTransform& transform) const;
};

} // namespace maps
