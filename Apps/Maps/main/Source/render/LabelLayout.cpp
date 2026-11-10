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

constexpr int CELL = 8;
constexpr int MAX_LINE_POINTS = 6000;
/** A line stretch is straight enough when no segment turns more than this from the stretch's direction. */
constexpr float MAX_BEND = 0.42f;
constexpr float PI = 3.14159265f;
/** Space kept free around a label, so neighbours do not touch. */
constexpr float GAP = 2.0f;
constexpr int SIZE_SMALL = 0;
constexpr int SIZE_NORMAL = 1;
constexpr int SIZE_LARGE = 2;

/** Priorities, the most important first. */
enum Priority : uint8_t {
    PRIORITY_REGION,
    PRIORITY_CITY,
    PRIORITY_TOWN,
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
    if (arena == nullptr || texts == nullptr || candidates == nullptr || linePoints == nullptr) {
        end();
        return false;
    }
    return true;
}

void LabelLayout::end() {
    if (releaseMemory != nullptr) {
        for (void* block : { static_cast<void*>(arena), static_cast<void*>(texts), static_cast<void*>(candidates), static_cast<void*>(linePoints) }) {
            if (block != nullptr) {
                releaseMemory(block);
            }
        }
    }
    arena = nullptr;
    texts = nullptr;
    candidates = nullptr;
    linePoints = nullptr;
    placedCount = 0;
    candidateCount = 0;
}

void LabelLayout::setFonts(LabelFont* small, LabelFont* normal, LabelFont* large) {
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

void LabelLayout::beginView(int width, int height, int viewZoom) {
    screenWidth = width;
    screenHeight = height;
    zoom = viewZoom;
    candidateCount = 0;
    linePointCount = 0;
    textsUsed = 0;
    gridColumns = (width + CELL - 1) / CELL;
    gridRows = (height + CELL - 1) / CELL;
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
                    fontSize = SIZE_LARGE;
                    return zoom <= 7;
                case Value::state:
                case Value::province:
                    priority = PRIORITY_REGION;
                    return zoom >= 5 && zoom <= 8 && density == LabelDensity::Normal;
                case Value::city:
                    priority = PRIORITY_CITY;
                    fontSize = SIZE_LARGE;
                    return zoom >= 4;
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

void LabelLayout::addSection(const uint8_t* section, size_t size, int count, const LabelTransform& transform) {
    if (candidates == nullptr) {
        return;
    }
    LabelReader reader;
    reader.begin(section, size, count);
    LabelRecord record {};
    while (candidateCount < MAX_CANDIDATES && reader.next(record)) {
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
        candidate.textOffset = static_cast<uint32_t>(textsUsed);
        candidate.textLength = length;
        memcpy(texts + textsUsed, name, length);
        texts[textsUsed + length] = '\0';
        textsUsed += length + 1u;
        candidates[candidateCount++] = candidate;
    }
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

bool LabelLayout::gridFree(float cx, float cy, float halfWidth, float halfHeight, float angle) const {
    // A turned box is checked as squares of its height along its axis
    const float ux = std::cos(angle);
    const float uy = std::sin(angle);
    const float step = std::max(2.0f, halfHeight);
    for (float t = -halfWidth; t <= halfWidth + 0.01f; t += step) {
        const float px = cx + ux * t;
        const float py = cy + uy * t;
        const int left = static_cast<int>(std::floor((px - halfHeight) / CELL));
        const int right = static_cast<int>(std::floor((px + halfHeight) / CELL));
        const int top = static_cast<int>(std::floor((py - halfHeight) / CELL));
        const int bottom = static_cast<int>(std::floor((py + halfHeight) / CELL));
        for (int row = top; row <= bottom; row++) {
            for (int column = left; column <= right; column++) {
                if (row < 0 || column < 0 || row >= gridRows || column >= gridColumns) {
                    // Labels stay inside the screen
                    return false;
                }
                const int cell = row * gridColumns + column;
                if ((grid[cell >> 5] >> (cell & 31)) & 1u) {
                    return false;
                }
            }
        }
    }
    return true;
}

void LabelLayout::gridMark(float cx, float cy, float halfWidth, float halfHeight, float angle) {
    const float ux = std::cos(angle);
    const float uy = std::sin(angle);
    const float step = std::max(2.0f, halfHeight);
    for (float t = -halfWidth; t <= halfWidth + 0.01f; t += step) {
        const float px = cx + ux * t;
        const float py = cy + uy * t;
        const int left = std::max(0, static_cast<int>(std::floor((px - halfHeight) / CELL)));
        const int right = std::min(gridColumns - 1, static_cast<int>(std::floor((px + halfHeight) / CELL)));
        const int top = std::max(0, static_cast<int>(std::floor((py - halfHeight) / CELL)));
        const int bottom = std::min(gridRows - 1, static_cast<int>(std::floor((py + halfHeight) / CELL)));
        for (int row = top; row <= bottom; row++) {
            for (int column = left; column <= right; column++) {
                const int cell = row * gridColumns + column;
                grid[cell >> 5] |= 1u << (cell & 31);
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
    const int width = textWidth(*font, texts + candidate.textOffset, candidate.textLength);
    const float halfWidth = width / 2.0f + GAP;
    const float halfHeight = font->lineHeight() / 2.0f + GAP;
    // Peaks get their name above the point, places centred on it
    const float centerY = candidate.priority == PRIORITY_PEAK ? candidate.y - halfHeight - 3.0f : candidate.y;
    if (!gridFree(candidate.x, centerY, halfWidth, halfHeight, 0.0f)) {
        return false;
    }
    if (!drawSprite(candidate, candidate.x, centerY, 0.0f, width)) {
        return false;
    }
    gridMark(candidate.x, centerY, halfWidth, halfHeight, 0.0f);
    return true;
}

bool LabelLayout::placePoi(const Candidate& candidate) {
    LabelFont* font = fontFor(candidate.fontSize);
    if (font == nullptr || candidate.icon == nullptr) {
        return false;
    }
    // Chains repeat names (two shops of the same name a street apart): only a copy right next to it is the same POI
    const char* name = texts + candidate.textOffset;
    for (int i = 0; i < placedCount; i++) {
        const Placed& other = placed[i];
        if (other.kind == LabelKind::Poi && other.poiType == candidate.poiType && strcmp(texts + other.textOffset, name) == 0 &&
            std::fabs(other.anchorX - candidate.x) < 6.0f && std::fabs(other.anchorY - candidate.y) < 6.0f) {
            return false;
        }
    }
    const float iconHalf = candidate.iconSize / 2.0f + GAP;
    // Where even the icon has no room, nothing is measured or drawn
    if (!gridFree(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f)) {
        return false;
    }
    // The name to the right of the icon when zoomed in and there is room, else the icon alone
    if (zoom >= POI_NAME_ZOOM && candidate.textLength > 0) {
        const int textPixels = textWidth(*font, name, candidate.textLength);
        const float contentWidth = candidate.iconSize + 3.0f + textPixels;
        const float centerX = candidate.x - candidate.iconSize / 2.0f + contentWidth / 2.0f;
        const float halfHeight = std::max(iconHalf, font->lineHeight() / 2.0f + GAP);
        if (gridFree(centerX, candidate.y, contentWidth / 2.0f + GAP, halfHeight, 0.0f) &&
            drawSprite(candidate, centerX, candidate.y, 0.0f, textPixels, true)) {
            gridMark(centerX, candidate.y, contentWidth / 2.0f + GAP, halfHeight, 0.0f);
            return true;
        }
    }
    if (!drawSprite(candidate, candidate.x, candidate.y, 0.0f, 0, false)) {
        return false;
    }
    gridMark(candidate.x, candidate.y, iconHalf, iconHalf, 0.0f);
    return true;
}

int LabelLayout::placeLine(const Candidate& candidate) {
    LabelFont* font = fontFor(candidate.fontSize);
    if (font == nullptr) {
        return 0;
    }
    const float* points = linePoints + 2 * candidate.firstPoint;
    const int count = candidate.pointCount;
    const int width = textWidth(*font, texts + candidate.textOffset, candidate.textLength);
    const float needed = static_cast<float>(width) + 8.0f;
    const float halfHeight = font->lineHeight() / 2.0f + GAP;
    // Distance along the line at each point
    float lengths[LabelReader::MAX_POINTS];
    lengths[0] = 0.0f;
    const int used = std::min(count, LabelReader::MAX_POINTS);
    for (int i = 1; i < used; i++) {
        const float dx = points[2 * i] - points[2 * i - 2];
        const float dy = points[2 * i + 1] - points[2 * i - 1];
        lengths[i] = lengths[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    const float total = lengths[used - 1];
    if (total < needed) {
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
    int placedHere = 0;
    for (float start = 0.0f; start + needed <= total && placedCount < MAX_LABELS;) {
        float x0;
        float y0;
        float x1;
        float y1;
        int first;
        int last;
        pointAt(start, x0, y0, first);
        pointAt(start + needed, x1, y1, last);
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
        const float centerX = (x0 + x1) / 2.0f;
        const float centerY = (y0 + y1) / 2.0f;
        if (centerX < halfHeight || centerY < halfHeight || centerX > screenWidth - halfHeight || centerY > screenHeight - halfHeight) {
            start += 16.0f;
            continue;
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
    if (arenaUsed + bytes + 2 * static_cast<size_t>(width) * height > ARENA_BYTES) {
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
    // The halo: the text grown by one pixel, and half as strong one more pixel out
    uint8_t* scratch = haloPlane + static_cast<size_t>(width) * height;
    uint8_t* scratch2 = scratch + static_cast<size_t>(width) * height;
    // A 3 x 3 maximum: by rows into rows, then by columns into to
    auto grow = [&](const uint8_t* from, uint8_t* to, uint8_t* rows) {
        for (int y = 0; y < height; y++) {
            const uint8_t* in = from + y * width;
            uint8_t* out = rows + y * width;
            for (int x = 0; x < width; x++) {
                uint8_t value = in[x];
                if (x > 0 && in[x - 1] > value) {
                    value = in[x - 1];
                }
                if (x + 1 < width && in[x + 1] > value) {
                    value = in[x + 1];
                }
                out[x] = value;
            }
        }
        for (int y = 0; y < height; y++) {
            const uint8_t* above = rows + (y > 0 ? y - 1 : y) * width;
            const uint8_t* here = rows + y * width;
            const uint8_t* below = rows + (y + 1 < height ? y + 1 : y) * width;
            uint8_t* out = to + y * width;
            for (int x = 0; x < width; x++) {
                out[x] = std::max(here[x], std::max(above[x], below[x]));
            }
        }
    };
    grow(textPlane, haloPlane, scratch);
    grow(haloPlane, scratch, scratch2);
    for (int i = 0; i < width * height; i++) {
        haloPlane[i] = std::max(haloPlane[i], static_cast<uint8_t>(scratch[i] / 2));
    }
    Placed& label = placed[placedCount++];
    label.textOffset = candidate.textOffset;
    label.x = static_cast<int16_t>(std::floor(centerX - width / 2.0f + 0.5f));
    label.y = static_cast<int16_t>(std::floor(centerY - height / 2.0f + 0.5f));
    label.width = static_cast<int16_t>(width);
    label.height = static_cast<int16_t>(height);
    label.color = candidate.color;
    label.line = candidate.line;
    label.spriteOffset = static_cast<uint32_t>(arenaUsed);
    label.kind = candidate.kind;
    label.poiType = candidate.poiType;
    label.anchorX = static_cast<int16_t>(std::clamp(candidate.x, -30000.0f, 30000.0f));
    label.anchorY = static_cast<int16_t>(std::clamp(candidate.y, -30000.0f, 30000.0f));
    arenaUsed += bytes;
    if (clock != nullptr) {
        spriteTime += clock() - started;
    }
    return true;
}

int LabelLayout::place() {
    placedCount = 0;
    arenaUsed = 0;
    spriteTime = 0;
    memset(grid, 0, sizeof(grid));
    if (candidates == nullptr || density == LabelDensity::Off) {
        return 0;
    }
    // The most important first; within a kind the lower rank first
    std::sort(candidates, candidates + candidateCount, [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.rank < b.rank;
    });
    for (int i = 0; i < candidateCount && placedCount < MAX_LABELS; i++) {
        if (candidates[i].line) {
            placeLine(candidates[i]);
        } else if (candidates[i].kind == LabelKind::Poi) {
            placePoi(candidates[i]);
        } else {
            placePoint(candidates[i]);
        }
    }
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
    for (int i = 0; i < placedCount && count < max && count < MAX_LABELS; i++) {
        const Placed& label = placed[i];
        if (label.kind != LabelKind::Poi && label.kind != LabelKind::HouseNumber) {
            continue;
        }
        const int dx = label.anchorX + offsetX - x;
        const int dy = label.anchorY + offsetY - y;
        // Insertion into the sorted list, nearest first
        const int distance = dx * dx + dy * dy;
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
