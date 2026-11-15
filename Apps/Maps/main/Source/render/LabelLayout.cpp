// SPDX-License-Identifier: GPL-3.0-or-later
#include "LabelLayout.h"

#include "Raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

namespace {

/** Whether the app's label font (assets/fonts/LabelSans.ttf, see host/make_label_font.py) has letters for this codepoint's script. */
bool labelFontCovers(uint32_t codepoint) {
    return codepoint < 0x0530 || (codepoint >= 0x1E00 && codepoint <= 0x1EFF) || (codepoint >= 0x2000 && codepoint <= 0x20CF) ||
        codepoint == 0x2116;
}

/**
 * Whether every character of a UTF-8 name can be drawn with the label font: asked of the font itself, since the app's
 * font has only the characters that map names use (tools/label_codepoints.txt); without a font, by script.
 */
bool labelFontCoversText(LabelFont* font, const char* text, uint16_t length) {
    // Plain ASCII (every house number, most names) is always in the font: no glyph lookups
    bool ascii = true;
    for (uint16_t i = 0; i < length && ascii; i++) {
        ascii = static_cast<uint8_t>(text[i]) < 0x80;
    }
    if (ascii) {
        return true;
    }
    for (uint16_t i = 0; i < length;) {
        const auto first = static_cast<uint8_t>(text[i]);
        uint32_t codepoint;
        int bytes;
        if (first < 0x80) {
            codepoint = first;
            bytes = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            bytes = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            bytes = 3;
        } else {
            codepoint = first & 0x07;
            bytes = 4;
        }
        if (i + bytes > length) {
            return false;
        }
        for (int k = 1; k < bytes; k++) {
            codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
        }
        LabelGlyph glyph {};
        if (font != nullptr ? !font->glyph(codepoint, glyph, false) : !labelFontCovers(codepoint)) {
            return false;
        }
        i = static_cast<uint16_t>(i + bytes);
    }
    return true;
}

/** The collision grid's cell in pixels. */
constexpr int CELL = 4;
/** The grid reaches this far beyond the screen: a label partly outside keeps its place, so nothing changes at the edge. */
constexpr int MARGIN = 40;
/** Map positions that must mean the same at every view zoom (a label's identity) are in pixels of this zoom. */
constexpr int WORLD_ZOOM = 24;
constexpr int MAX_LINE_POINTS = 6000;
/** A line stretch is straight enough when no segment turns more than this from the stretch's direction. */
constexpr float MAX_BEND = 0.42f;
constexpr float PI = 3.14159265f;
/** Space kept free around a label, so neighbours do not touch. */
constexpr float GAP = 2.0f;
/** Icons may stand closer together than names. */
constexpr float ICON_GAP = 1.0f;
constexpr int SIZE_SMALL = 0;
constexpr int SIZE_NORMAL = 1;
constexpr int SIZE_LARGE = 2;

/** Priorities, the most important first. */
enum Priority : uint8_t {
    PRIORITY_REGION,
    PRIORITY_CITY,
    PRIORITY_TOWN,
    /** States and provinces: after the cities and towns, whose names matter more when both do not fit. */
    PRIORITY_STATE,
    PRIORITY_VILLAGE,
    PRIORITY_SUBURB,
    PRIORITY_MAJOR_ROAD,
    PRIORITY_MINOR_ROAD,
    PRIORITY_HAMLET,
    PRIORITY_WATER,
    PRIORITY_PEAK,
    PRIORITY_POI,
    PRIORITY_HOUSE_NUMBER
};

uint32_t nextCodepoint(const char*& text, const char* end) {
    const auto first = static_cast<uint8_t>(*text++);
    if (first < 0x80) {
        return first;
    }
    if ((first & 0xE0) == 0xC0 && text < end) {
        return ((first & 0x1Fu) << 6) | (static_cast<uint8_t>(*text++) & 0x3Fu);
    }
    if ((first & 0xF0) == 0xE0 && text + 1 < end) {
        const uint32_t second = static_cast<uint8_t>(*text++) & 0x3Fu;
        const uint32_t third = static_cast<uint8_t>(*text++) & 0x3Fu;
        return ((first & 0x0Fu) << 12) | (second << 6) | third;
    }
    while (text < end && (static_cast<uint8_t>(*text) & 0xC0) == 0x80) {
        text++;
    }
    return '?';
}

/** The first zoom level a road name of a class shows at. */
int roadZoom(Value value) {
    switch (value) {
        case Value::motorway:
        case Value::trunk:
        case Value::primary:
            return 12;
        case Value::secondary:
        case Value::tertiary:
            return 14;
        default:
            return 15;
    }
}

} // namespace

LabelLayout::~LabelLayout() {
    end();
}

bool LabelLayout::begin(void* (*allocate)(size_t), void (*release)(void*)) {
    allocateMemory = allocate;
    releaseMemory = release;
    arena = static_cast<uint8_t*>(allocate(ARENA_BYTES));
    texts = static_cast<char*>(allocate(TEXT_BYTES));
    candidates = static_cast<Candidate*>(allocate(sizeof(Candidate) * MAX_CANDIDATES));
    linePoints = static_cast<float*>(allocate(sizeof(float) * 2 * MAX_LINE_POINTS));
    widths = static_cast<WidthEntry*>(allocate(sizeof(WidthEntry) * WIDTH_ENTRIES));
    if (widths != nullptr) {
        memset(widths, 0, sizeof(WidthEntry) * WIDTH_ENTRIES);
    }
    if (arena == nullptr || texts == nullptr || candidates == nullptr || linePoints == nullptr || widths == nullptr) {
        end();
        return false;
    }
    return true;
}

void LabelLayout::end() {
    if (releaseMemory != nullptr) {
        for (void* block : { static_cast<void*>(arena), static_cast<void*>(texts), static_cast<void*>(candidates), static_cast<void*>(linePoints),
                 static_cast<void*>(widths) }) {
            if (block != nullptr) {
                releaseMemory(block);
            }
        }
    }
    arena = nullptr;
    texts = nullptr;
    candidates = nullptr;
    linePoints = nullptr;
    widths = nullptr;
    placedCount = 0;
    candidateCount = 0;
}

void LabelLayout::setFonts(LabelFont* small, LabelFont* normal, LabelFont* large) {
    // New fonts measure differently
    if (widths != nullptr) {
        memset(widths, 0, sizeof(WidthEntry) * WIDTH_ENTRIES);
    }
    fonts[SIZE_SMALL] = small;
    fonts[SIZE_NORMAL] = normal != nullptr ? normal : small;
    fonts[SIZE_LARGE] = large != nullptr ? large : fonts[SIZE_NORMAL];
}

LabelFont* LabelLayout::fontFor(int size) const {
    return fonts[std::clamp(size, 0, 2)];
}

void LabelLayout::clear() {
    placedCount = 0;
}

void LabelLayout::screenToMap(double sx, double sy, double& mx, double& my) const {
    if (viewSin == 0.0f && viewCos == 1.0f) {
        // North up: the screen's corner is the origin (kept exactly as before, so unturned placements do not change)
        mx = sx + originX;
        my = sy + originY;
        return;
    }
    const double dx = sx - screenWidth / 2.0;
    const double dy = sy - screenHeight / 2.0;
    mx = viewCenterX + viewCos * dx - viewSin * dy;
    my = viewCenterY + viewSin * dx + viewCos * dy;
}

void LabelLayout::mapToScreen(double mx, double my, double& sx, double& sy) const {
    if (viewSin == 0.0f && viewCos == 1.0f) {
        sx = mx - originX;
        sy = my - originY;
        return;
    }
    const double dx = mx - viewCenterX;
    const double dy = my - viewCenterY;
    sx = screenWidth / 2.0 + viewCos * dx + viewSin * dy;
    sy = screenHeight / 2.0 - viewSin * dx + viewCos * dy;
}

void LabelLayout::beginView(int width, int height, int viewZoom, double worldLeft, double worldTop) {
    beginTurnedView(width, height, viewZoom, worldLeft, worldTop, worldLeft + width / 2.0, worldTop + height / 2.0, 1.0f, 0.0f);
}

void LabelLayout::beginTurnedView(int width, int height, int viewZoom, double worldLeft, double worldTop, double centerX, double centerY,
    float cosine, float sine) {
    if (placedCount > 0) {
        // What is on screen now is remembered (by name and map position), so it keeps its place in the next placement
        const double scale = std::ldexp(1.0, WORLD_ZOOM - zoom);
        keptCount = 0;
        for (int i = 0; i < placedCount; i++) {
            const Placed& label = placed[i];
            double mx;
            double my;
            screenToMap(label.x + label.width / 2.0, label.y + label.height / 2.0, mx, my);
            kept[keptCount++] = { label.id, mx * scale, my * scale };
        }
    }
    screenWidth = width;
    screenHeight = height;
    zoom = viewZoom;
    originX = worldLeft;
    originY = worldTop;
    viewCenterX = centerX;
    viewCenterY = centerY;
    viewCos = cosine;
    viewSin = sine;
    gridBaseX = std::floor((worldLeft - MARGIN) / CELL);
    gridBaseY = std::floor((worldTop - MARGIN) / CELL);
    pendingCount = 0;
    candidateCount = 0;
    recordCount = 0;
    linePointCount = 0;
    textsUsed = 0;
    gridColumns = (width + 2 * MARGIN + CELL - 1) / CELL + 1;
    gridRows = (height + 2 * MARGIN + CELL - 1) / CELL + 1;
    if (gridColumns * gridRows > static_cast<int>(sizeof(grid) * 8)) {
        gridRows = static_cast<int>(sizeof(grid) * 8) / gridColumns;
    }
}

bool LabelLayout::wants(const LabelRecord& record, uint8_t& priority, uint8_t& fontSize, uint16_t& color) const {
    if (density == LabelDensity::Off) {
        return false;
    }
    // Few shows each kind one or two zoom levels later and leaves out the least important ones
    const int later = density == LabelDensity::Few ? 1 : 0;
    fontSize = SIZE_SMALL;
    switch (record.kind) {
        case LabelKind::Place:
            color = style.place;
            switch (record.labelClass) {
                case Value::continent:
                case Value::country:
                    priority = PRIORITY_REGION;
                    // Large while whole countries fill the screen; from z5 the middle size, so the capitals and
                    // large cities fit between the names of the countries (central Europe at z5 had no room for them)
                    fontSize = zoom <= 4 ? SIZE_LARGE : SIZE_NORMAL;
                    return zoom <= 7;
                case Value::state:
                case Value::province:
                    priority = PRIORITY_STATE;
                    return zoom >= 5 && zoom <= 8 && density == LabelDensity::Normal;
                case Value::city: {
                    priority = PRIORITY_CITY;
                    // Zoomed out, only the cities that matter at that scale, by their rank (capitals and the largest
                    // have 1 to 3, Bratislava or Krakow 5, regional centres 6 to 8): the same rule everywhere, so a
                    // small city never shows where a capital next to it does not. All of them from z8.
                    const int rank = record.rank < 0 ? 99 : record.rank;
                    const int first = rank <= 2 ? 4 : (rank <= 3 ? 5 : (rank <= 5 ? 6 : (rank <= 7 ? 7 : 8)));
                    // Zoomed out the cities are small next to the countries' names, and grow as the countries leave
                    fontSize = zoom <= 6 ? SIZE_SMALL : (zoom == 7 ? SIZE_NORMAL : SIZE_LARGE);
                    return zoom >= std::max(4, first);
                }
                case Value::town:
                    priority = PRIORITY_TOWN;
                    fontSize = SIZE_NORMAL;
                    return zoom >= 8 + 2 * later;
                case Value::village:
                    priority = PRIORITY_VILLAGE;
                    return zoom >= 11 + later;
                case Value::suburb:
                case Value::quarter:
                    priority = PRIORITY_SUBURB;
                    return zoom >= 12 + later;
                case Value::neighbourhood:
                case Value::hamlet:
                    priority = PRIORITY_HAMLET;
                    return zoom >= 14 && density == LabelDensity::Normal;
                case Value::isolated_dwelling:
                    priority = PRIORITY_HAMLET;
                    return zoom >= 16 && density == LabelDensity::Normal;
                case Value::island:
                    priority = PRIORITY_WATER;
                    return zoom >= 12;
                default:
                    priority = PRIORITY_HAMLET;
                    return zoom >= 14 && density == LabelDensity::Normal;
            }
        case LabelKind::Road: {
            color = style.road;
            const int first = roadZoom(record.labelClass);
            priority = first == 12 ? PRIORITY_MAJOR_ROAD : PRIORITY_MINOR_ROAD;
            if (density == LabelDensity::Few && first > 14) {
                return false;
            }
            return zoom >= first + later;
        }
        case LabelKind::Water:
            color = style.water;
            priority = PRIORITY_WATER;
            if (record.labelClass == Value::ocean || record.labelClass == Value::sea) {
                return zoom >= 3;
            }
            return zoom >= 9 + 2 * later;
        case LabelKind::Waterway:
            color = style.water;
            priority = PRIORITY_WATER;
            if (record.labelClass == Value::river) {
                return zoom >= 12 + later;
            }
            return zoom >= 15 && density == LabelDensity::Normal;
        case LabelKind::Peak:
            color = style.peak;
            priority = PRIORITY_PEAK;
            return zoom >= 12 + later;
        case LabelKind::Poi: {
            PoiAppearance look {};
            if (poiLookup == nullptr || !poiLookup(static_cast<uint8_t>(record.labelClass), look) || look.icon == nullptr) {
                return false;
            }
            color = style.binary ? style.place : look.color;
            priority = PRIORITY_POI;
            // From the type's zoom (15 for most); the most important ones (rank 1-2) one level earlier, at least z14
            int first = look.minZoom;
            if (record.rank >= 0 && record.rank <= 2) {
                first = std::min(first, 14);
            }
            return zoom >= std::max(14, first) + later;
        }
        case LabelKind::HouseNumber:
            color = style.houseNumber;
            priority = PRIORITY_HOUSE_NUMBER;
            return zoom >= 17 + later;
    }
    return false;
}

void LabelLayout::addSection(const uint8_t* section, size_t size, int count, const LabelTransform& transform, const LabelCells* cells) {
    if (candidates == nullptr) {
        return;
    }
    if (cells == nullptr) {
        addRecords(section, size, count, transform);
        return;
    }
    addRecords(section, cells->generalBytes, static_cast<int>(cells->generalCount), transform);
    // POIs, then house numbers, of the cells near the screen only: at z17 that is about a tenth of a z14 tile
    for (int kind = 0; kind < 2; kind++) {
        const uint32_t* offsets = kind == 0 ? cells->poiOffset : cells->numberOffset;
        const uint32_t* counts = kind == 0 ? cells->poiCount : cells->numberCount;
        for (int cell = 0; cell < LABEL_CELLS; cell++) {
            if (counts[cell] != 0 && offsets[cell] < size && cellNearScreen(cell, transform)) {
                addRecords(section + offsets[cell], size - offsets[cell], static_cast<int>(counts[cell]), transform);
            }
        }
    }
}

bool LabelLayout::cellNearScreen(int cell, const LabelTransform& transform) const {
    const auto u0 = static_cast<float>((cell % LABEL_GRID) * LABEL_CELL_UNITS);
    const auto v0 = static_cast<float>((cell / LABEL_GRID) * LABEL_CELL_UNITS);
    const auto span = static_cast<float>(LABEL_CELL_UNITS);
    float minX = 1e9f;
    float minY = 1e9f;
    float maxX = -1e9f;
    float maxY = -1e9f;
    for (int corner = 0; corner < 4; corner++) {
        const float u = u0 + ((corner & 1) != 0 ? span : 0.0f);
        const float v = v0 + ((corner & 2) != 0 ? span : 0.0f);
        const float x = transform.a * u + transform.b * v + transform.x;
        const float y = transform.c * u + transform.d * v + transform.y;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    // The same margins as the test for single points below
    return maxX >= -40.0f && maxY >= -20.0f && minX <= screenWidth + 40.0f && minY <= screenHeight + 20.0f;
}

void LabelLayout::addRecords(const uint8_t* bytes, size_t size, int count, const LabelTransform& transform) {
    LabelReader reader;
    reader.begin(bytes, size, count);
    LabelRecord record {};
    while (candidateCount < MAX_CANDIDATES && reader.next(record)) {
        recordCount++;
        Candidate candidate {};
        // POIs and house numbers never crowd out place and street names: they stop before the candidates are full
        if ((record.kind == LabelKind::Poi && candidateCount >= POI_CANDIDATE_LIMIT) ||
            (record.kind == LabelKind::HouseNumber && candidateCount >= NUMBER_CANDIDATE_LIMIT)) {
            continue;
        }
        if (!wants(record, candidate.priority, candidate.fontSize, candidate.color)) {
            continue;
        }
        candidate.kind = record.kind;
        if (record.kind == LabelKind::Poi) {
            PoiAppearance look {};
            poiLookup(static_cast<uint8_t>(record.labelClass), look);
            candidate.poiType = static_cast<uint8_t>(record.labelClass);
            candidate.icon = look.icon;
            candidate.iconSize = static_cast<uint8_t>(look.iconSize);
        }
        // The Latin-script name when asked for, and for names in scripts the label font has no letters for (Japanese,
        // Chinese, Arabic and others)
        const bool hasLatin = record.nameLatin != nullptr && record.nameLatinLength > 0;
        const bool latin = hasLatin && (preferLatin || !labelFontCoversText(fonts[SIZE_NORMAL], record.name, record.nameLength));
        const char* name = latin ? record.nameLatin : record.name;
        const uint16_t length = latin ? record.nameLatinLength : record.nameLength;
        // A POI may have no name: its icon is the label, the sheet names its type
        if ((length == 0 && record.kind != LabelKind::Poi) || textsUsed + length + 1 > TEXT_BYTES) {
            continue;
        }
        candidate.line = record.pointCount >= 2;
        candidate.rank = record.rank < 0 ? INT32_MAX : record.rank;
        if (candidate.line) {
            if (linePointCount + record.pointCount > MAX_LINE_POINTS) {
                continue;
            }
            candidate.firstPoint = static_cast<uint16_t>(std::min(linePointCount, 65535));
            candidate.pointCount = static_cast<uint16_t>(record.pointCount);
            float minX = 1e9f;
            float minY = 1e9f;
            float maxX = -1e9f;
            float maxY = -1e9f;
            for (int i = 0; i < record.pointCount; i++) {
                const auto u = static_cast<float>(record.points[2 * i]);
                const auto v = static_cast<float>(record.points[2 * i + 1]);
                const float x = transform.a * u + transform.b * v + transform.x;
                const float y = transform.c * u + transform.d * v + transform.y;
                linePoints[2 * (linePointCount + i)] = x;
                linePoints[2 * (linePointCount + i) + 1] = y;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
            // A line that does not reach the screen keeps none of its points
            if (maxX < 0.0f || maxY < 0.0f || minX > screenWidth || minY > screenHeight) {
                continue;
            }
            linePointCount += record.pointCount;
        } else {
            const auto u = static_cast<float>(record.points[0]);
            const auto v = static_cast<float>(record.points[1]);
            candidate.x = transform.a * u + transform.b * v + transform.x;
            candidate.y = transform.c * u + transform.d * v + transform.y;
            // Points off screen cannot show their label
            if (candidate.x < -40.0f || candidate.y < -20.0f || candidate.x > screenWidth + 40.0f || candidate.y > screenHeight + 20.0f) {
                continue;
            }
        }
        {
            // The label's own order among equals: its name and where it is on the map (to two pixels, so the same
            // label from two tiles gets the same number)
            uint32_t hash = 2166136261u;
            for (uint16_t i = 0; i < length; i++) {
                hash = (hash ^ static_cast<uint8_t>(name[i])) * 16777619u;
            }
            const float anchorX = candidate.line ? linePoints[2 * candidate.firstPoint] : candidate.x;
            const float anchorY = candidate.line ? linePoints[2 * candidate.firstPoint + 1] : candidate.y;
            // In pixels of WORLD_ZOOM, so the label has the same number at every view zoom
            const double scale = std::ldexp(1.0, WORLD_ZOOM - zoom);
            double mapX;
            double mapY;
            screenToMap(anchorX, anchorY, mapX, mapY);
            const auto worldX = static_cast<int64_t>(std::floor(mapX * scale / 16.0 + 0.5));
            const auto worldY = static_cast<int64_t>(std::floor(mapY * scale / 16.0 + 0.5));
            hash = (hash ^ static_cast<uint32_t>(worldX)) * 16777619u;
            hash = (hash ^ static_cast<uint32_t>(worldY)) * 16777619u;
            candidate.order = hash;
        }
        candidate.textOffset = static_cast<uint32_t>(textsUsed);
        candidate.textLength = length;
        memcpy(texts + textsUsed, name, length);
        texts[textsUsed + length] = '\0';
        textsUsed += length + 1u;
        candidates[candidateCount++] = candidate;
    }
}

int LabelLayout::candidateWidth(LabelFont& font, const Candidate& candidate) {
    const char* text = texts + candidate.textOffset;
    if (widths == nullptr) {
        return textWidth(font, text, candidate.textLength);
    }
    // FNV-1a over the font size and the text; 0 marks an empty slot
    uint32_t hash = 2166136261u ^ candidate.fontSize;
    for (int i = 0; i < candidate.textLength; i++) {
        hash = (hash ^ static_cast<uint8_t>(text[i])) * 16777619u;
    }
    hash = hash == 0 ? 1 : hash;
    WidthEntry& entry = widths[hash & (WIDTH_ENTRIES - 1)];
    if (entry.hash == hash && entry.used != 0) {
        return entry.width;
    }
    const int width = textWidth(font, text, candidate.textLength);
    entry = { hash, static_cast<uint16_t>(std::clamp(width, 0, 65535)), 1 };
    return width;
}

int LabelLayout::textWidth(LabelFont& font, const char* text, int length) {
    const char* end = text + length;
    int widthX16 = 0;
    uint32_t previous = 0;
    bool hasPrevious = false;
    while (text < end) {
        LabelGlyph glyph {};
        if (!font.glyph(nextCodepoint(text, end), glyph, false)) {
            continue;
        }
        if (hasPrevious) {
            widthX16 += font.kerningX16(previous, glyph.id);
        }
        widthX16 += glyph.advanceX16;
        previous = glyph.id;
        hasPrevious = true;
    }
    return (widthX16 + 8) >> 4;
}

bool LabelLayout::gridRange(float cx, float cy, float halfWidth, float halfHeight, int& left, int& top, int& right, int& bottom) const {
    // Cells are fixed to the map, not to the screen: a label covers the same cells however the view is panned, so a
    // pan never changes which labels fit (they jumped when the cells moved with the screen)
    left = static_cast<int>(std::floor((static_cast<double>(cx - halfWidth) + originX) / CELL) - gridBaseX);
    right = static_cast<int>(std::floor((static_cast<double>(cx + halfWidth) + originX) / CELL) - gridBaseX);
    top = static_cast<int>(std::floor((static_cast<double>(cy - halfHeight) + originY) / CELL) - gridBaseY);
    bottom = static_cast<int>(std::floor((static_cast<double>(cy + halfHeight) + originY) / CELL) - gridBaseY);
    // What lies beyond the margin around the screen is not tracked: nothing can be seen there
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, gridColumns - 1);
    bottom = std::min(bottom, gridRows - 1);
    return left <= right && top <= bottom;
}

bool LabelLayout::onScreen(float cx, float cy, float halfWidth, float halfHeight) const {
    return cx + halfWidth > 0.0f && cy + halfHeight > 0.0f && cx - halfWidth < screenWidth && cy - halfHeight < screenHeight;
}

bool LabelLayout::gridFree(float cx, float cy, float halfWidth, float halfHeight, float angle) const {
    if (angle == 0.0f) {
        // Upright: exactly the box (as squares along the axis it reached half a height further at both ends)
        int left, top, right, bottom;
        if (!gridRange(cx, cy, halfWidth, halfHeight, left, top, right, bottom)) {
            return true;
        }
        for (int row = top; row <= bottom; row++) {
            for (int column = left; column <= right; column++) {
                const int cell = row * gridColumns + column;
                if ((grid[cell >> 5] >> (cell & 31)) & 1u) {
                    return false;
                }
            }
        }
        return true;
    }
    // A turned box is checked as squares of its height along its axis, the end squares inside the box
    const float ux = std::cos(angle);
    const float uy = std::sin(angle);
    const float step = std::max(2.0f, halfHeight);
    const float reach = std::max(0.0f, halfWidth - halfHeight);
    for (float t = -reach; t <= reach + 0.01f; t += step) {
        int left, top, right, bottom;
        if (!gridRange(cx + ux * t, cy + uy * t, halfHeight, halfHeight, left, top, right, bottom)) {
            continue;
        }
        for (int row = top; row <= bottom; row++) {
            for (int column = left; column <= right; column++) {
                const int cell = row * gridColumns + column;
                if ((grid[cell >> 5] >> (cell & 31)) & 1u) {
                    return false;
                }
            }
        }
    }
    return true;
}

void LabelLayout::gridMark(float cx, float cy, float halfWidth, float halfHeight, float angle, bool taken) {
    const float ux = std::cos(angle);
    const float uy = std::sin(angle);
    const float step = std::max(2.0f, halfHeight);
    const float reach = angle == 0.0f ? 0.0f : std::max(0.0f, halfWidth - halfHeight);
    for (float t = -reach; t <= reach + 0.01f; t += step) {
        int left, top, right, bottom;
        if (!gridRange(cx + ux * t, cy + uy * t, angle == 0.0f ? halfWidth : halfHeight, halfHeight, left, top, right, bottom)) {
            continue;
        }
        for (int row = top; row <= bottom; row++) {
            for (int column = left; column <= right; column++) {
                const int cell = row * gridColumns + column;
                if (taken) {
                    grid[cell >> 5] |= 1u << (cell & 31);
                } else {
                    grid[cell >> 5] &= ~(1u << (cell & 31));
                }
            }
        }
    }
}

bool LabelLayout::alreadyPlaced(const Candidate& candidate, float x, float y) const {
    // The same name from a neighbouring tile, or the same street again too soon
    const char* name = texts + candidate.textOffset;
    // House numbers repeat in every street: only the same number at the same spot (two tiles' copies) is one
    const float reach = candidate.line ? static_cast<float>(LINE_REPEAT_PIXELS) : (candidate.kind == LabelKind::HouseNumber ? 6.0f : 120.0f);
    for (int i = 0; i < placedCount; i++) {
        const Placed& other = placed[i];
        if (other.line != candidate.line || other.kind != candidate.kind || strcmp(texts + other.textOffset, name) != 0) {
            continue;
        }
        const float dx = other.x + other.width / 2.0f - x;
        const float dy = other.y + other.height / 2.0f - y;
        if (dx * dx + dy * dy < reach * reach) {
            return true;
        }
    }
    return false;
}

bool LabelLayout::placePoint(const Candidate& candidate) {
    LabelFont* font = fontFor(candidate.fontSize);
    if (font == nullptr || alreadyPlaced(candidate, candidate.x, candidate.y)) {
        return false;
    }
    const int width = candidateWidth(*font, candidate);
    const float halfWidth = width / 2.0f + GAP;
    const float halfHeight = font->lineHeight() / 2.0f + GAP;
    // Peaks get their name above the point, places centred on it
    float centerX = candidate.x;
    float centerY = candidate.priority == PRIORITY_PEAK ? candidate.y - halfHeight - 3.0f : candidate.y;
    if (!gridFree(candidate.x, centerY, halfWidth, halfHeight, 0.0f)) {
        // Countries, cities and towns try just above, below and beside their point before they give up: a capital next
        // to its country's name (Budapest under Magyarorszag) no longer loses to a small city with free space
        if (candidate.priority > PRIORITY_TOWN) {
            return false;
        }
        const float shift = 2.0f * halfHeight - GAP;
        const float aside = halfWidth + 4.0f;
        if (gridFree(candidate.x, candidate.y - shift, halfWidth, halfHeight, 0.0f)) {
            centerY = candidate.y - shift;
        } else if (gridFree(candidate.x, candidate.y + shift, halfWidth, halfHeight, 0.0f)) {
            centerY = candidate.y + shift;
        } else if (gridFree(candidate.x + aside, candidate.y, halfWidth, halfHeight, 0.0f)) {
            centerX = candidate.x + aside;
        } else if (gridFree(candidate.x - aside, candidate.y, halfWidth, halfHeight, 0.0f)) {
            centerX = candidate.x - aside;
        } else {
            return false;
        }
    }
    // A label wholly in the margin beyond the screen takes its place (so nothing changes when it comes into view) but
    // costs no sprite
    if (onScreen(centerX, centerY, halfWidth, halfHeight) && !drawSprite(candidate, centerX, centerY, 0.0f, width)) {
        return false;
    }
    gridMark(centerX, centerY, halfWidth, halfHeight, 0.0f);
    return true;
}

bool LabelLayout::placePoiIcon(const Candidate& candidate) {
    if (candidate.icon == nullptr) {
        return false;
    }
    // Chains repeat names (two shops of the same name a street apart): only a copy right next to it is the same POI
    const char* name = texts + candidate.textOffset;
    for (int i = 0; i < pendingCount; i++) {
        const Candidate& other = candidates[pending[i]];
        if (other.poiType == candidate.poiType && strcmp(texts + other.textOffset, name) == 0 && std::fabs(other.x - candidate.x) < 6.0f &&
            std::fabs(other.y - candidate.y) < 6.0f) {
            return false;
        }
    }
    const float iconHalf = candidate.iconSize / 2.0f + ICON_GAP;
    if (!gridFree(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f)) {
        return false;
    }
    gridMark(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f);
    return true;
}

void LabelLayout::finishPois() {
    // Every icon that fits has its place now. Names come second, where room is left next to an icon: a name never
    // costs a neighbour its icon (the sheet of a POI shows its name anyway).
    for (int i = 0; i < pendingCount && placedCount < MAX_LABELS; i++) {
        const Candidate& candidate = candidates[pending[i]];
        LabelFont* font = fontFor(candidate.fontSize);
        const float iconHalf = candidate.iconSize / 2.0f + ICON_GAP;
        if (font != nullptr && zoom >= POI_NAME_ZOOM && candidate.textLength > 0) {
            const int textPixels = candidateWidth(*font, candidate);
            const float contentWidth = candidate.iconSize + 3.0f + textPixels;
            const float centerX = candidate.x - candidate.iconSize / 2.0f + contentWidth / 2.0f;
            const float halfHeight = std::max(iconHalf, font->lineHeight() / 2.0f + GAP);
            // Its own icon's cells are its own: taken out for the test of the icon and name together
            gridMark(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f, false);
            if (gridFree(centerX, candidate.y, contentWidth / 2.0f + GAP, halfHeight, 0.0f) &&
                (!onScreen(centerX, candidate.y, contentWidth / 2.0f, halfHeight) ||
                    drawSprite(candidate, centerX, candidate.y, 0.0f, textPixels, true))) {
                gridMark(centerX, candidate.y, contentWidth / 2.0f + GAP, halfHeight, 0.0f);
                continue;
            }
            gridMark(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f);
        }
        if (!onScreen(candidate.x, candidate.y, iconHalf, iconHalf)) {
            continue;   // in the margin: its place is kept, nothing is drawn
        }
        if (!drawSprite(candidate, candidate.x, candidate.y, 0.0f, 0, false)) {
            // The sprite arena is full: the place is given back
            gridMark(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f, false);
        }
    }
    pendingCount = 0;
}

int LabelLayout::placeLine(const Candidate& candidate) {
    LabelFont* font = fontFor(candidate.fontSize);
    if (font == nullptr) {
        return 0;
    }
    const float* points = linePoints + 2 * candidate.firstPoint;
    const int count = candidate.pointCount;
    const int width = candidateWidth(*font, candidate);
    const float needed = static_cast<float>(width) + 8.0f;
    const float halfHeight = font->lineHeight() / 2.0f + GAP;
    // Distance along the line at each point
    // Not on the stack (1 KB): the map loop's task has little of it left
    float* lengths = lineLengths;
    lengths[0] = 0.0f;
    const int used = std::min(count, LabelReader::MAX_POINTS);
    for (int i = 1; i < used; i++) {
        const float dx = points[2 * i] - points[2 * i - 2];
        const float dy = points[2 * i + 1] - points[2 * i - 1];
        lengths[i] = lengths[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    const float total = lengths[used - 1];
    // A damaged label file can give points far away: beyond this the 16 px steps below no longer add up in a float
    // and the loop never ended (found by host/fuzz_tests). No real line on screen is that long.
    if (total < needed || !(total < 200000.0f)) {
        return 0;
    }
    auto pointAt = [&](float distance, float& x, float& y, int& segment) {
        segment = 1;
        while (segment < used - 1 && lengths[segment] < distance) {
            segment++;
        }
        const float span = std::max(1e-3f, lengths[segment] - lengths[segment - 1]);
        const float t = std::clamp((distance - lengths[segment - 1]) / span, 0.0f, 1.0f);
        x = points[2 * segment - 2] + (points[2 * segment] - points[2 * segment - 2]) * t;
        y = points[2 * segment - 1] + (points[2 * segment + 1] - points[2 * segment - 1]) * t;
    };
    // One stretch of the line starting at a distance along it: placed there when it is straight, on screen and free
    auto tryAt = [&](float start, const float* nearX, const float* nearY) {
        float x0;
        float y0;
        float x1;
        float y1;
        int first;
        int last;
        pointAt(start, x0, y0, first);
        pointAt(start + needed, x1, y1, last);
        const float centerX = (x0 + x1) / 2.0f;
        const float centerY = (y0 + y1) / 2.0f;
        if (centerX < halfHeight || centerY < halfHeight || centerX > screenWidth - halfHeight || centerY > screenHeight - halfHeight) {
            return false;
        }
        if (nearX != nullptr && (std::fabs(centerX - *nearX) > 10.0f || std::fabs(centerY - *nearY) > 10.0f)) {
            return false;
        }
        const float chordX = x1 - x0;
        const float chordY = y1 - y0;
        const float chord = std::sqrt(chordX * chordX + chordY * chordY);
        bool straight = chord >= needed * 0.92f;
        // Every segment inside the stretch must run close to the chord's direction
        const float angle = std::atan2(chordY, chordX);
        for (int i = first; i <= last && straight; i++) {
            const float sx = points[2 * i] - points[2 * i - 2];
            const float sy = points[2 * i + 1] - points[2 * i - 1];
            if (sx * sx + sy * sy < 4.0f) {
                continue;
            }
            float bend = std::fabs(std::atan2(sy, sx) - angle);
            if (bend > PI) {
                bend = 2.0f * PI - bend;
            }
            straight = bend <= MAX_BEND;
        }
        // Upright: text never runs from right to left
        float upright = angle;
        if (upright > PI / 2.0f) {
            upright -= PI;
        } else if (upright < -PI / 2.0f) {
            upright += PI;
        }
        if (straight && !alreadyPlaced(candidate, centerX, centerY) && gridFree(centerX, centerY, width / 2.0f + GAP, halfHeight, upright) &&
            drawSprite(candidate, centerX, centerY, upright, width)) {
            gridMark(centerX, centerY, width / 2.0f + GAP, halfHeight, upright);
            return true;
        }
        return false;
    };
    int placedHere = 0;
    // A name that was on screen keeps its stretch while that is still good: otherwise it slid along its street with
    // every pan, to the first stretch on screen, and pushed other labels away
    if (candidate.sticky) {
        const double scale = std::ldexp(1.0, zoom - WORLD_ZOOM);
        for (int k = 0; k < keptCount && placedCount < MAX_LABELS; k++) {
            if (kept[k].id != candidate.order) {
                continue;
            }
            double screenX;
            double screenY;
            mapToScreen(kept[k].worldX * scale, kept[k].worldY * scale, screenX, screenY);
            const auto nearX = static_cast<float>(screenX);
            const auto nearY = static_cast<float>(screenY);
            for (float start = 0.0f; start + needed <= total; start += 16.0f) {
                if (tryAt(start, &nearX, &nearY)) {
                    placedHere++;
                    break;
                }
            }
        }
    }
    for (float start = 0.0f; start + needed <= total && placedCount < MAX_LABELS;) {
        if (tryAt(start, nullptr, nullptr)) {
            placedHere++;
            start += needed + LINE_REPEAT_PIXELS;
        } else {
            start += 16.0f;
        }
    }
    return placedHere;
}

bool LabelLayout::drawSprite(const Candidate& candidate, float centerX, float centerY, float angle, int textPixels, bool withText) {
    LabelFont* font = fontFor(candidate.fontSize);
    if (placedCount >= MAX_LABELS || font == nullptr) {
        return false;
    }
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    // The text box, with room for the halo, turned
    // A POI's icon comes first, its name (if drawn) after a small gap
    const int iconSize = candidate.icon != nullptr ? candidate.iconSize : 0;
    const int iconWidth = iconSize > 0 ? iconSize + (withText ? 3 : 0) : 0;
    const float boxWidth = static_cast<float>(iconWidth + (withText ? textPixels : 0)) + 6.0f;
    const float boxHeight = static_cast<float>(std::max(withText ? font->lineHeight() : 0, iconSize)) + 4.0f;
    const int width = static_cast<int>(std::ceil(std::fabs(boxWidth * cosine) + std::fabs(boxHeight * sine))) + 2;
    const int height = static_cast<int>(std::ceil(std::fabs(boxWidth * sine) + std::fabs(boxHeight * cosine))) + 2;
    if (candidate.line && (centerX - width / 2.0f < -0.5f || centerY - height / 2.0f < -0.5f || centerX + width / 2.0f > screenWidth + 0.5f ||
                              centerY + height / 2.0f > screenHeight + 0.5f)) {
        return false; // a steep line label's turned box would stick out of the screen
    }
    const size_t bytes = static_cast<size_t>(width) * height * 2;
    // An icon without a name looks the same wherever it is: the sprite drawn for the first one serves the others
    const bool shared = !withText && iconSize > 0 && angle == 0.0f;
    int sharedAt = -1;
    for (int i = 0; shared && i < iconSpriteCount && sharedAt < 0; i++) {
        if (iconSprites[i].icon == candidate.icon && iconSprites[i].size == iconSize) {
            sharedAt = i;
        }
    }
    auto finish = [&](uint32_t spriteOffset) {
        Placed& label = placed[placedCount++];
        label.textOffset = candidate.textOffset;
        label.x = static_cast<int16_t>(std::floor(centerX - width / 2.0f + 0.5f));
        label.y = static_cast<int16_t>(std::floor(centerY - height / 2.0f + 0.5f));
        label.width = static_cast<int16_t>(width);
        label.height = static_cast<int16_t>(height);
        label.color = candidate.color;
        label.line = candidate.line;
        label.spriteOffset = spriteOffset;
        label.kind = candidate.kind;
        label.poiType = candidate.poiType;
        label.id = candidate.order;
        label.anchorX = static_cast<int16_t>(std::clamp(candidate.x, -30000.0f, 30000.0f));
        label.anchorY = static_cast<int16_t>(std::clamp(candidate.y, -30000.0f, 30000.0f));
    };
    if (sharedAt >= 0) {
        finish(iconSprites[sharedAt].offset);
        return true;
    }
    // The sprite's two planes, two more to work in and two numbers a row (the halo below)
    if (arenaUsed + bytes + 2 * static_cast<size_t>(width) * height + 4 * static_cast<size_t>(height) + 2 > ARENA_BYTES) {
        return false;
    }
    const uint32_t started = clock != nullptr ? clock() : 0;
    uint8_t* textPlane = arena + arenaUsed;
    uint8_t* haloPlane = textPlane + static_cast<size_t>(width) * height;
    memset(textPlane, 0, bytes);
    // Glyphs along the baseline, the text's centre at the sprite's centre
    const float spriteCenterX = width / 2.0f;
    const float spriteCenterY = height / 2.0f;
    const float baseline = font->ascent() - font->lineHeight() / 2.0f;
    const char* text = texts + candidate.textOffset;
    const char* end = withText ? text + candidate.textLength : text;
    const int contentWidth = iconWidth + (withText ? textPixels : 0);
    // The text starts after the icon; without one this is -textPixels * 8, the text centred
    int penX16 = static_cast<int>((static_cast<float>(iconWidth) - contentWidth / 2.0f) * 16.0f);
    if (iconSize > 0) {
        // The icon's mask at the left end, centred on the line; icons are only drawn level
        const int iconX = static_cast<int>(std::floor(spriteCenterX - contentWidth / 2.0f + 0.5f));
        const int iconY = static_cast<int>(std::floor(spriteCenterY - iconSize / 2.0f + 0.5f));
        for (int gy = 0; gy < iconSize; gy++) {
            const int y = iconY + gy;
            for (int gx = 0; gx < iconSize && y >= 0 && y < height; gx++) {
                const int x = iconX + gx;
                if (x >= 0 && x < width) {
                    uint8_t& target = textPlane[y * width + x];
                    target = std::max(target, candidate.icon[gy * iconSize + gx]);
                }
            }
        }
    }
    uint32_t previous = 0;
    bool hasPrevious = false;
    while (text < end) {
        LabelGlyph glyph {};
        if (!font->glyph(nextCodepoint(text, end), glyph, true)) {
            continue;
        }
        if (hasPrevious) {
            penX16 += font->kerningX16(previous, glyph.id);
        }
        previous = glyph.id;
        hasPrevious = true;
        if (glyph.alpha != nullptr && glyph.width > 0 && glyph.height > 0 && angle == 0.0f) {
            // Level text: the mask is copied to the nearest whole pixel
            const int originX = static_cast<int>(std::floor(spriteCenterX + penX16 / 16.0f + glyph.left + 0.5f));
            const int originY = static_cast<int>(std::floor(spriteCenterY + baseline + glyph.top + 0.5f));
            for (int gy = 0; gy < glyph.height; gy++) {
                const int y = originY + gy;
                if (y < 0 || y >= height) {
                    continue;
                }
                for (int gx = 0; gx < glyph.width; gx++) {
                    const int x = originX + gx;
                    if (x >= 0 && x < width) {
                        uint8_t& target = textPlane[y * width + x];
                        target = std::max(target, glyph.alpha[gy * glyph.width + gx]);
                    }
                }
            }
        } else if (glyph.alpha != nullptr && glyph.width > 0 && glyph.height > 0) {
            // The glyph's top left corner in text coordinates, then each sprite pixel near it sampled back
            const float originX = penX16 / 16.0f + glyph.left;
            const float originY = baseline + glyph.top;
            const float cornersX[4] = { originX, originX + glyph.width, originX, originX + glyph.width };
            const float cornersY[4] = { originY, originY, originY + glyph.height, originY + glyph.height };
            float minX = 1e9f;
            float minY = 1e9f;
            float maxX = -1e9f;
            float maxY = -1e9f;
            for (int i = 0; i < 4; i++) {
                const float sx = spriteCenterX + cornersX[i] * cosine - cornersY[i] * sine;
                const float sy = spriteCenterY + cornersX[i] * sine + cornersY[i] * cosine;
                minX = std::min(minX, sx);
                minY = std::min(minY, sy);
                maxX = std::max(maxX, sx);
                maxY = std::max(maxY, sy);
            }
            const int left = std::max(0, static_cast<int>(std::floor(minX)) - 1);
            const int right = std::min(width - 1, static_cast<int>(std::ceil(maxX)) + 1);
            const int top = std::max(0, static_cast<int>(std::floor(minY)) - 1);
            const int bottom = std::min(height - 1, static_cast<int>(std::ceil(maxY)) + 1);
            for (int y = top; y <= bottom; y++) {
                for (int x = left; x <= right; x++) {
                    // Back into the glyph's mask, sampled at the pixel centre with bilinear filtering
                    const float dx = x + 0.5f - spriteCenterX;
                    const float dy = y + 0.5f - spriteCenterY;
                    const float gx = dx * cosine + dy * sine - originX - 0.5f;
                    const float gy = -dx * sine + dy * cosine - originY - 0.5f;
                    if (gx < -1.0f || gy < -1.0f || gx >= glyph.width || gy >= glyph.height) {
                        continue;
                    }
                    const int ix = static_cast<int>(std::floor(gx));
                    const int iy = static_cast<int>(std::floor(gy));
                    const float fx = gx - ix;
                    const float fy = gy - iy;
                    auto at = [&](int px, int py) -> float {
                        return px < 0 || py < 0 || px >= glyph.width || py >= glyph.height ? 0.0f : glyph.alpha[py * glyph.width + px];
                    };
                    const float value = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                    uint8_t& target = textPlane[y * width + x];
                    target = std::max(target, static_cast<uint8_t>(std::min(255.0f, value + 0.5f)));
                }
            }
        }
        penX16 += glyph.advanceX16;
    }
    // The halo: the text grown by one pixel, and half as strong one more pixel out. That is the largest value within
    // one pixel, or half the largest within two, whichever is more. Both are found in two passes, along the rows and
    // then down the columns, and only where a row has text: most of a sprite is empty (all of it but a band, for a
    // name turned along its street), and going over every pixel five times was more than half the time of drawing
    // the labels of a view (0.43 us a pixel on the T-Deck, 13-21 ms a view, 15 Nov).
    const size_t plane = static_cast<size_t>(width) * height;
    uint8_t* near = haloPlane + plane;   // the largest of a pixel and its two neighbours in the row
    uint8_t* far = near + plane;         // and of the two on each side
    // (at an even address: the arena's sprites have any size)
    auto* rowLow = reinterpret_cast<int16_t*>(far + plane + ((reinterpret_cast<uintptr_t>(far) + plane) & 1));
    int16_t* rowHigh = rowLow + height;
    memset(near, 0, 2 * plane);
    memset(haloPlane, 0, plane);
    for (int y = 0; y < height; y++) {
        const uint8_t* in = textPlane + y * width;
        int low = 0;
        while (low < width && in[low] == 0) {
            low++;
        }
        int high = width - 1;
        while (high >= low && in[high] == 0) {
            high--;
        }
        if (high < low) {
            rowLow[y] = static_cast<int16_t>(width);
            rowHigh[y] = -1;
            continue;
        }
        low = std::max(0, low - 2);
        high = std::min(width - 1, high + 2);
        rowLow[y] = static_cast<int16_t>(low);
        rowHigh[y] = static_cast<int16_t>(high);
        uint8_t* out3 = near + y * width;
        uint8_t* out5 = far + y * width;
        for (int x = low; x <= high; x++) {
            uint8_t three = in[x];
            if (x > 0 && in[x - 1] > three) {
                three = in[x - 1];
            }
            if (x + 1 < width && in[x + 1] > three) {
                three = in[x + 1];
            }
            uint8_t five = three;
            if (x > 1 && in[x - 2] > five) {
                five = in[x - 2];
            }
            if (x + 2 < width && in[x + 2] > five) {
                five = in[x + 2];
            }
            out3[x] = three;
            out5[x] = five;
        }
    }
    for (int y = 0; y < height; y++) {
        // The columns that any of the five rows around this one has text in
        int low = width;
        int high = -1;
        for (int row = std::max(0, y - 2); row <= std::min(height - 1, y + 2); row++) {
            low = std::min<int>(low, rowLow[row]);
            high = std::max<int>(high, rowHigh[row]);
        }
        if (high < low) {
            continue;
        }
        const uint8_t* n0 = near + (y > 0 ? y - 1 : y) * width;
        const uint8_t* n1 = near + y * width;
        const uint8_t* n2 = near + (y + 1 < height ? y + 1 : y) * width;
        const uint8_t* f0 = far + std::max(0, y - 2) * width;
        const uint8_t* f1 = far + std::max(0, y - 1) * width;
        const uint8_t* f2 = far + y * width;
        const uint8_t* f3 = far + std::min(height - 1, y + 1) * width;
        const uint8_t* f4 = far + std::min(height - 1, y + 2) * width;
        uint8_t* out = haloPlane + y * width;
        for (int x = low; x <= high; x++) {
            uint8_t one = n1[x];
            if (n0[x] > one) {
                one = n0[x];
            }
            if (n2[x] > one) {
                one = n2[x];
            }
            uint8_t two = f2[x];
            if (f0[x] > two) {
                two = f0[x];
            }
            if (f1[x] > two) {
                two = f1[x];
            }
            if (f3[x] > two) {
                two = f3[x];
            }
            if (f4[x] > two) {
                two = f4[x];
            }
            two = static_cast<uint8_t>(two / 2);
            out[x] = one > two ? one : two;
        }
    }
    finish(static_cast<uint32_t>(arenaUsed));
    if (shared && iconSpriteCount < ICON_SPRITES) {
        iconSprites[iconSpriteCount++] = { candidate.icon, static_cast<uint32_t>(arenaUsed), static_cast<int16_t>(iconSize) };
    }
    arenaUsed += bytes;
    if (clock != nullptr) {
        spriteTime += clock() - started;
    }
    return true;
}

int LabelLayout::place() {
    placedCount = 0;
    arenaUsed = 0;
    iconSpriteCount = 0;
    spriteTime = 0;
    memset(grid, 0, sizeof(grid));
    if (candidates == nullptr || density == LabelDensity::Off) {
        return 0;
    }
    // The most important first; within a kind the lower rank first
    // The most important first; within a kind the lower rank first. Equal ones in an order that depends only on the
    // label itself (its name and its place on the map), never on the order the tiles were read in: the same view
    // always gives the same labels
    // Among the small things (suburbs, streets, POIs, house numbers) what was on screen in the last placement comes
    // first: it stays where it is while the map moves or zooms, and new ones fill the room that is left. Countries,
    // cities, towns and villages always go by rank.
    for (int i = 0; i < candidateCount; i++) {
        Candidate& candidate = candidates[i];
        candidate.sticky = false;
        if (candidate.priority >= PRIORITY_SUBURB) {
            for (int k = 0; k < keptCount; k++) {
                if (kept[k].id == candidate.order) {
                    candidate.sticky = true;
                    break;
                }
            }
        }
    }
    std::sort(candidates, candidates + candidateCount, [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.sticky != b.sticky) {
            return a.sticky;
        }
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        if (a.order != b.order) {
            return a.order < b.order;
        }
        return a.textOffset < b.textOffset;
    });
    bool poisOpen = false;
    for (int i = 0; i < candidateCount && placedCount + pendingCount < MAX_LABELS; i++) {
        if (candidates[i].kind == LabelKind::Poi) {
            // Icons first, all of them; their names when the last POI had its turn
            poisOpen = true;
            if (placePoiIcon(candidates[i])) {
                pending[pendingCount++] = static_cast<uint16_t>(i);
            }
            continue;
        }
        if (poisOpen) {
            finishPois();
            poisOpen = false;
        }
        if (candidates[i].line) {
            placeLine(candidates[i]);
        } else {
            placePoint(candidates[i]);
        }
    }
    finishPois();
    return placedCount;
}

void LabelLayout::draw(Canvas& canvas, int offsetX, int offsetY, bool lineLabels) const {
    for (int i = 0; i < placedCount; i++) {
        const Placed& label = placed[i];
        if (label.line && !lineLabels) {
            continue;
        }
        const uint8_t* textPlane = arena + label.spriteOffset;
        const uint8_t* haloPlane = textPlane + static_cast<size_t>(label.width) * label.height;
        const int originX = label.x + offsetX;
        const int originY = label.y + offsetY;
        const int top = std::max(0, -originY);
        const int bottom = std::min(static_cast<int>(label.height), canvas.height - originY);
        const int left = std::max(0, -originX);
        const int right = std::min(static_cast<int>(label.width), canvas.width - originX);
        for (int y = top; y < bottom; y++) {
            uint16_t* row = canvas.pixels + (originY + y) * canvas.width + originX;
            const uint8_t* halo = haloPlane + y * label.width;
            const uint8_t* ink = textPlane + y * label.width;
            for (int x = left; x < right; x++) {
                if (style.binary) {
                    // Black and white displays: no blending, the halo first
                    if (ink[x] >= 128) {
                        row[x] = label.color;
                    } else if (halo[x] >= 128) {
                        row[x] = style.halo;
                    }
                    continue;
                }
                if (halo[x] != 0) {
                    const uint32_t alpha = (halo[x] + 4u) >> 3;
                    row[x] = alpha >= 32 ? style.halo : blendRgb565(style.halo, row[x], alpha);
                }
                if (ink[x] != 0) {
                    const uint32_t alpha = (ink[x] + 4u) >> 3;
                    row[x] = alpha >= 32 ? label.color : blendRgb565(label.color, row[x], alpha);
                }
            }
        }
    }
}

bool LabelLayout::item(int index, PlacedItem& out) const {
    if (index < 0 || index >= placedCount) {
        return false;
    }
    const Placed& label = placed[index];
    if (label.kind != LabelKind::Poi && label.kind != LabelKind::HouseNumber) {
        return false;
    }
    out = { label.kind, label.poiType, texts + label.textOffset, label.anchorX, label.anchorY };
    return true;
}

int LabelLayout::pick(int x, int y, int radius, int offsetX, int offsetY) const {
    int best = -1;
    int bestDistance = radius * radius + 1;
    for (int i = 0; i < placedCount; i++) {
        const Placed& label = placed[i];
        if (label.kind != LabelKind::Poi && label.kind != LabelKind::HouseNumber) {
            continue;
        }
        // Inside the label's box counts as on it; otherwise the distance to its point
        const int left = label.x + offsetX;
        const int top = label.y + offsetY;
        const bool inside = x >= left && y >= top && x < left + label.width && y < top + label.height;
        const int dx = label.anchorX + offsetX - x;
        const int dy = label.anchorY + offsetY - y;
        const int distance = inside ? 0 : dx * dx + dy * dy;
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

int LabelLayout::byDistance(int x, int y, int* out, int max, int offsetX, int offsetY) const {
    int count = 0;
    int distances[MAX_LABELS];
    const int capacity = max < MAX_LABELS ? max : MAX_LABELS;
    for (int i = 0; i < placedCount && capacity > 0; i++) {
        const Placed& label = placed[i];
        if (label.kind != LabelKind::Poi && label.kind != LabelKind::HouseNumber) {
            continue;
        }
        const int dx = label.anchorX + offsetX - x;
        const int dy = label.anchorY + offsetY - y;
        // Insertion into the sorted list, nearest first. Every label is looked at, because the
        // nearest one can come late in placement order; when the list is full the farthest drops out.
        const int distance = dx * dx + dy * dy;
        if (count == capacity) {
            if (distance >= distances[count - 1]) {
                continue;
            }
            count--;
        }
        int at = count++;
        while (at > 0 && distances[at - 1] > distance) {
            distances[at] = distances[at - 1];
            out[at] = out[at - 1];
            at--;
        }
        distances[at] = distance;
        out[at] = i;
    }
    return count;
}

void LabelLayout::bounds(int index, int& x, int& y, int& w, int& h) const {
    const Placed& label = placed[index];
    x = label.x;
    y = label.y;
    w = label.width;
    h = label.height;
}

} // namespace maps
