// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchExtract.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int TILE_UNITS = 4096;

bool textIs(const char* text, size_t length, const char* value) {
    return text != nullptr && strlen(value) == length && memcmp(text, value, length) == 0;
}

/** Unnamed POI classes that are not worth an index entry (study.py JUNK). */
bool isJunkClass(const char* text, size_t length) {
    static const char* const JUNK[] = { "gate", "bollard", "lift_gate", "waste_basket", "bicycle_parking", "information", "shelter", "recycling" };
    for (const char* junk : JUNK) {
        if (textIs(text, length, junk)) {
            return true;
        }
    }
    return false;
}

/** Classes the study's street matching left out (data.py PATHCLS). */
bool isPathClass(const char* text, size_t length) {
    static const char* const PATH[] = { "path", "track", "aerialway", "ferry", "rail" };
    for (const char* path : PATH) {
        if (textIs(text, length, path)) {
            return true;
        }
    }
    return false;
}

uint8_t settlementClass(const char* text, size_t length) {
    static const char* const NAMES[] = { "city", "town", "village", "hamlet", "isolated_dwelling", "suburb", "quarter", "neighbourhood" };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (textIs(text, length, NAMES[i])) {
            return static_cast<uint8_t>(i);
        }
    }
    return EXTRACT_NOT_SETTLEMENT;
}

} // namespace

void ExtractedTile::clear() {
    text.clear();
    points.clear();
    lines.clear();
    linePoints.clear();
    houses.clear();
    refOnlyLines = 0;
    junkPois = 0;
}

void ExtractedTile::release() {
    text.release();
    points.release();
    lines.release();
    linePoints.release();
    houses.release();
    refOnlyLines = 0;
    junkPois = 0;
}

uint32_t TileExtractor::addText(const char* text, size_t length) {
    const auto offset = static_cast<uint32_t>(out->text.size());
    for (size_t i = 0; i < length; i++) {
        out->text.push(text[i]);
    }
    out->text.push('\0');
    return offset;
}

uint32_t TileExtractor::addLabel(const char* kind, const char* classText, size_t classLength, const char* subclassText, size_t subclassLength) {
    char buffer[200];
    const bool isPoi = strcmp(kind, "poi") == 0;
    char cls[80];
    size_t n = classLength < sizeof(cls) - 1 ? classLength : sizeof(cls) - 1;
    if (classText != nullptr && n > 0) {
        memcpy(cls, classText, n);
    } else {
        n = 0;
    }
    cls[n] = '\0';
    if (n == 0) {
        strcpy(cls, "other");
    }
    if (isPoi) {
        char sub[80];
        size_t m = subclassLength < sizeof(sub) - 1 ? subclassLength : sizeof(sub) - 1;
        if (subclassText != nullptr && m > 0) {
            memcpy(sub, subclassText, m);
        } else {
            m = 0;
        }
        sub[m] = '\0';
        snprintf(buffer, sizeof(buffer), "poi/%s/%s", cls, m > 0 ? sub : cls);
    } else {
        snprintf(buffer, sizeof(buffer), "%s/%s", kind, cls);
    }
    return addText(buffer, strlen(buffer));
}

int32_t TileExtractor::scaled(int32_t value, int32_t origin) const {
    if (extent == TILE_UNITS) {
        return origin + value;
    }
    const double units = static_cast<double>(value) * TILE_UNITS / extent;
    return origin + static_cast<int32_t>(std::floor(units + 0.5));
}

bool TileExtractor::extract(MvtDecoder& decoder, ByteInput* input, int tileX, int tileY, ExtractedTile& tile, ExtractConsumer* featureConsumer) {
    out = &tile;
    consumer = featureConsumer;
    consumerOk = true;
    arrayFailed = false;
    tile.clear();
    tile.tileX = tileX;
    tile.tileY = tileY;
    originX = tileX * TILE_UNITS;
    originY = tileY * TILE_UNITS;
    shape = Shape::None;
    if (!decoder.scan(input, true)) {
        decoder.shrinkPools();
        return false;
    }
    for (int i = 0; i < static_cast<int>(Layer::Count); i++) {
        extents[i] = decoder.extent(static_cast<Layer>(i));
    }
    static const Layer LAYERS[] = { Layer::Place, Layer::Poi, Layer::TransportationName, Layer::Housenumber, Layer::WaterName, Layer::MountainPeak,
        Layer::Park };
    const bool ok = decoder.sweep(LAYERS, 7, *this);
    decoder.shrinkPools();
    consumer = nullptr;
    return ok && consumerOk && !arrayFailed && !tile.failed();
}

bool TileExtractor::beginFeature(const FeatureInfo& info) {
    if (consumer != nullptr) {
        // Texts of a skipped feature before this one are not needed
        out->text.clear();
    }
    shape = Shape::None;
    rings = 0;
    havePoint = false;
    label = name = latin = en = EXTRACT_NONE;
    settlement = EXTRACT_NOT_SETTLEMENT;
    useEn = 0;
    pathClass = 0;
    extent = extents[static_cast<int>(info.layer)];
    if (extent <= 0) {
        extent = TILE_UNITS;
    }
    if (info.layer == Layer::Housenumber) {
        if (info.houseNumber == nullptr || info.houseNumberLength == 0) {
            return false;
        }
        name = addText(info.houseNumber, info.houseNumberLength);
        shape = Shape::House;
        return true;
    }
    // The display name is name, else name:latin, else name_en (data.py nm_of, without name_int)
    const char* shown = nullptr;
    size_t shownLength = 0;
    if (info.name != nullptr && info.nameLength > 0) {
        shown = info.name;
        shownLength = info.nameLength;
    } else if (info.nameLatin != nullptr && info.nameLatinLength > 0) {
        shown = info.nameLatin;
        shownLength = info.nameLatinLength;
    } else if (info.nameEn != nullptr && info.nameEnLength > 0) {
        shown = info.nameEn;
        shownLength = info.nameEnLength;
    }
    if (info.layer == Layer::TransportationName) {
        if (shown == nullptr) {
            out->refOnlyLines++;
            return false;
        }
        name = addText(shown, shownLength);
        if (info.nameLatin != nullptr && info.nameLatinLength > 0) {
            latin = addText(info.nameLatin, info.nameLatinLength);
        }
        pathClass = isPathClass(info.classText, info.classTextLength) ? 1 : 0;
        shape = Shape::Line;
        return true;
    }
    if (shown == nullptr) {
        if (info.layer != Layer::Poi) {
            return false;
        }
        if (isJunkClass(info.classText, info.classTextLength)) {
            out->junkPois++;
            return false;
        }
    }
    switch (info.layer) {
        case Layer::Place:
            label = addLabel("place", info.classText, info.classTextLength, nullptr, 0);
            settlement = settlementClass(info.classText, info.classTextLength);
            useEn = 1;
            break;
        case Layer::Poi:
            label = addLabel("poi", info.classText, info.classTextLength, info.subclassText, info.subclassTextLength);
            break;
        case Layer::WaterName:
            label = addLabel("water", info.classText, info.classTextLength, nullptr, 0);
            useEn = 1;
            break;
        case Layer::MountainPeak:
            label = addLabel("peak", info.classText, info.classTextLength, nullptr, 0);
            useEn = 1;
            break;
        case Layer::Park:
            label = addLabel("park", info.classText, info.classTextLength, nullptr, 0);
            useEn = 1;
            break;
        default:
            return false;
    }
    if (shown != nullptr) {
        name = addText(shown, shownLength);
        if (info.nameLatin != nullptr && info.nameLatinLength > 0) {
            latin = addText(info.nameLatin, info.nameLatinLength);
        }
        if (info.nameEn != nullptr && info.nameEnLength > 0) {
            en = addText(info.nameEn, info.nameEnLength);
        }
    }
    shape = Shape::Point;
    return true;
}

void TileExtractor::beginRing() {
    rings++;
    chunks = 0;
    if (shape == Shape::Line) {
        lineStart = static_cast<uint32_t>(out->linePoints.size() / 2);
    } else if (rings == 1) {
        ring.clear();
    }
}

void TileExtractor::points(const TilePoint* chunk, int count) {
    // The decoder starts each chunk after the first with the last point of the one before: skip that repeat
    const int skip = chunks > 0 ? 1 : 0;
    chunks++;
    if (shape == Shape::Line) {
        for (int i = skip; i < count; i++) {
            out->linePoints.push(scaled(chunk[i].x, originX));
            out->linePoints.push(scaled(chunk[i].y, originY));
        }
    } else if ((shape == Shape::Point || shape == Shape::House) && rings == 1) {
        for (int i = skip; i < count; i++) {
            ring.push(chunk[i]);
        }
    }
}

void TileExtractor::endRing(bool closed) {
    if (shape == Shape::Line) {
        const auto count = static_cast<uint32_t>(out->linePoints.size() / 2 - lineStart);
        if (count >= 2) {
            out->lines.push({ name, latin, lineStart, count, pathClass });
        } else {
            // Not a line: take its points back
            while (out->linePoints.size() / 2 > lineStart) {
                out->linePoints.pop();
                out->linePoints.pop();
            }
        }
        return;
    }
    if (ring.failed()) {
        arrayFailed = true;
        return;
    }
    if (rings != 1 || ring.size() == 0) {
        return;
    }
    // data.py first_pt: the middle vertex of a ring of more than 2 points (a closed ring repeats its first point there)
    const size_t length = ring.size() + (closed ? 1 : 0);
    size_t index = length > 2 ? length / 2 : 0;
    if (index >= ring.size()) {
        index = ring.size() - 1;
    }
    havePoint = true;
    pointX = ring[index].x;
    pointY = ring[index].y;
}

void TileExtractor::endFeature() {
    if (havePoint) {
        const int32_t gx = scaled(pointX, originX);
        const int32_t gy = scaled(pointY, originY);
        if (shape == Shape::Point) {
            out->points.push({ label, name, latin, en, gx, gy, settlement, useEn });
        } else if (shape == Shape::House) {
            const int32_t lx = gx - originX;
            const int32_t ly = gy - originY;
            out->houses.push({ name, gx, gy, static_cast<uint8_t>(lx >= 0 && lx < TILE_UNITS && ly >= 0 && ly < TILE_UNITS ? 1 : 0) });
        }
    }
    shape = Shape::None;
    if (out->failed()) {
        // Never hand partial arrays on; clear() below resets the flags, so remember it
        arrayFailed = true;
        consumerOk = false;
    }
    if (consumer != nullptr) {
        if (consumerOk && (out->points.size() > 0 || out->lines.size() > 0 || out->houses.size() > 0)) {
            consumerOk = consumer->take(*out);
        }
        out->text.clear();
        out->points.clear();
        out->lines.clear();
        out->linePoints.clear();
        out->houses.clear();
    }
}

} // namespace maps
