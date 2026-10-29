// SPDX-License-Identifier: GPL-3.0-or-later
#include "Style.h"

#include <cmath>
#include <cstdio>

namespace maps {

namespace {

constexpr uint16_t rgb(uint32_t hex) {
    return static_cast<uint16_t>(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

constexpr Palette COLOR_PALETTE = {
    .background = rgb(0xF2EFE9),
    .placeholder = rgb(0xE4E1DB),
    .water = rgb(0xAAD3DF),
    .wood = rgb(0xADD19E),
    .grass = rgb(0xCDEBB0),
    .farmland = rgb(0xEEF0D5),
    .wetland = rgb(0xD6E8D2),
    .sand = rgb(0xF5E9C6),
    .ice = rgb(0xDDECEC),
    .rock = rgb(0xE2DED8),
    .residential = rgb(0xE0DFDF),
    .commercial = rgb(0xF2DAD9),
    .industrial = rgb(0xEBDBE8),
    .cemetery = rgb(0xAACBAF),
    .hospital = rgb(0xFFF0F0),
    .school = rgb(0xFFFFE5),
    .pitch = rgb(0xAAE0CB),
    .military = rgb(0xF3E2E1),
    .park = rgb(0xC8FACC),
    .building = rgb(0xD9D0C9),
    .aerodrome = rgb(0xE9E7E2),
    .runway = rgb(0xBBBBCC),
    .boundary = rgb(0x8D618B),
    .motorway = rgb(0xE892A2),
    .motorwayCasing = rgb(0xC24E6B),
    .trunk = rgb(0xF9B29C),
    .trunkCasing = rgb(0xC84E2F),
    .primary = rgb(0xFCD6A4),
    .primaryCasing = rgb(0xA06B00),
    .secondary = rgb(0xF7FABF),
    .secondaryCasing = rgb(0x707D05),
    .minor = rgb(0xFFFFFF),
    .minorCasing = rgb(0xB8B4AE),
    .path = rgb(0xC08A5A),
    .rail = rgb(0x707070),
    .pier = rgb(0xF2EFE9),
    .text = rgb(0x202020),
    .halo = rgb(0xFFFFFF),
    .marker = rgb(0x1A73E8),
};

constexpr Palette MONOCHROME_PALETTE = {
    .background = rgb(0xFFFFFF),
    .placeholder = rgb(0xEEEEEE),
    .water = rgb(0xA0A0A0),
    .wood = rgb(0xC8C8C8),
    .grass = rgb(0xE0E0E0),
    .farmland = rgb(0xFFFFFF),
    .wetland = rgb(0xD8D8D8),
    .sand = rgb(0xF0F0F0),
    .ice = rgb(0xF0F0F0),
    .rock = rgb(0xE8E8E8),
    .residential = rgb(0xF4F4F4),
    .commercial = rgb(0xF0F0F0),
    .industrial = rgb(0xECECEC),
    .cemetery = rgb(0xD0D0D0),
    .hospital = rgb(0xF4F4F4),
    .school = rgb(0xF4F4F4),
    .pitch = rgb(0xE0E0E0),
    .military = rgb(0xECECEC),
    .park = rgb(0xDCDCDC),
    .building = rgb(0xB8B8B8),
    .aerodrome = rgb(0xF0F0F0),
    .runway = rgb(0x909090),
    .boundary = rgb(0x404040),
    .motorway = rgb(0xFFFFFF),
    .motorwayCasing = rgb(0x000000),
    .trunk = rgb(0xFFFFFF),
    .trunkCasing = rgb(0x000000),
    .primary = rgb(0xFFFFFF),
    .primaryCasing = rgb(0x000000),
    .secondary = rgb(0xFFFFFF),
    .secondaryCasing = rgb(0x202020),
    .minor = rgb(0xFFFFFF),
    .minorCasing = rgb(0x404040),
    .path = rgb(0x303030),
    .rail = rgb(0x000000),
    .pier = rgb(0xFFFFFF),
    .text = rgb(0x000000),
    .halo = rgb(0xFFFFFF),
    .marker = rgb(0x000000),
};

enum class RoadKind : uint8_t {
    None,
    Motorway,
    Trunk,
    Primary,
    Secondary,
    Tertiary,
    Minor,
    Service,
    Path,
    Rail,
    Pier
};

struct RoadSpec {
    float widthAt14;
    int8_t minZoom;
    int8_t casingMinZoom;
};

constexpr RoadSpec ROAD_SPECS[] = {
    { 0.0f, 99, 99 },  // None
    { 4.5f, 0, 10 },   // Motorway
    { 4.0f, 5, 11 },   // Trunk
    { 3.5f, 8, 12 },   // Primary
    { 3.0f, 9, 12 },   // Secondary
    { 2.6f, 11, 13 },  // Tertiary
    { 1.8f, 13, 14 },  // Minor
    { 1.1f, 14, 15 },  // Service
    { 0.8f, 14, 99 },  // Path
    { 1.0f, 10, 99 },  // Rail
    { 1.6f, 14, 99 },  // Pier
};

RoadKind roadKind(Value featureClass) {
    switch (featureClass) {
        case Value::motorway:
            return RoadKind::Motorway;
        case Value::trunk:
            return RoadKind::Trunk;
        case Value::primary:
            return RoadKind::Primary;
        case Value::secondary:
            return RoadKind::Secondary;
        case Value::tertiary:
            return RoadKind::Tertiary;
        case Value::minor:
        case Value::raceway:
        case Value::busway:
            return RoadKind::Minor;
        case Value::service:
            return RoadKind::Service;
        case Value::path:
        case Value::track:
            return RoadKind::Path;
        case Value::rail:
        case Value::transit:
            return RoadKind::Rail;
        case Value::pier:
            return RoadKind::Pier;
        default:
            return RoadKind::None;
    }
}

/** Line widths grow slower than the map below z14 and close to the map above it. */
float zoomScale(int zoom) {
    const float steps = static_cast<float>(zoom - 14);
    return std::pow(2.0f, steps * (zoom < 14 ? 0.55f : 0.85f));
}

bool roadStyle(const FeatureInfo& info, Stage stage, int zoom, const Palette& palette, DrawStyle& style) {
    const RoadKind kind = roadKind(info.featureClass);
    if (kind == RoadKind::None) {
        return false;
    }
    const RoadSpec& spec = ROAD_SPECS[static_cast<int>(kind)];
    if (zoom < spec.minZoom) {
        return false;
    }
    if (info.geometryType == GEOMETRY_POLYGON) {
        if (kind != RoadKind::Pier || stage != Stage::RoadFill) {
            return false;
        }
        style = { palette.pier, 255, 0.0f };
        return true;
    }
    if (info.geometryType != GEOMETRY_LINE) {
        return false;
    }
    const bool tunnel = info.brunnel == Value::tunnel;
    float width = std::fmax(0.6f, spec.widthAt14 * zoomScale(zoom));
    if (kind == RoadKind::Path) {
        width = std::fmin(1.4f, width);
    }
    uint16_t fill;
    uint16_t casing;
    switch (kind) {
        case RoadKind::Motorway:
            fill = palette.motorway;
            casing = palette.motorwayCasing;
            break;
        case RoadKind::Trunk:
            fill = palette.trunk;
            casing = palette.trunkCasing;
            break;
        case RoadKind::Primary:
            fill = palette.primary;
            casing = palette.primaryCasing;
            break;
        case RoadKind::Secondary:
            fill = palette.secondary;
            casing = palette.secondaryCasing;
            break;
        case RoadKind::Path:
            fill = palette.path;
            casing = palette.path;
            break;
        case RoadKind::Rail:
            fill = palette.rail;
            casing = palette.rail;
            break;
        case RoadKind::Pier:
            fill = palette.pier;
            casing = palette.minorCasing;
            break;
        default:
            fill = palette.minor;
            casing = palette.minorCasing;
            break;
    }
    if (stage == Stage::RoadCasing) {
        if (tunnel || zoom < spec.casingMinZoom) {
            return false;
        }
        style = { casing, 255, width + std::fmax(1.0f, width * 0.35f) };
        return true;
    }
    // Roads without a casing at this zoom level use the casing colour, so white roads stay visible
    const bool cased = zoom >= spec.casingMinZoom && kind != RoadKind::Path && kind != RoadKind::Rail;
    uint8_t opacity = tunnel ? 110 : 255;
    if (kind == RoadKind::Path) {
        opacity = zoom < 15 ? 120 : 190;
    }
    style = { cased || kind == RoadKind::Path || kind == RoadKind::Rail ? fill : casing, opacity, width };
    return true;
}

bool waterwayStyle(const FeatureInfo& info, int zoom, const Palette& palette, DrawStyle& style) {
    if (info.geometryType != GEOMETRY_LINE) {
        return false;
    }
    float width;
    int minZoom;
    switch (info.featureClass) {
        case Value::river:
            width = 2.2f;
            minZoom = 8;
            break;
        case Value::canal:
            width = 1.8f;
            minZoom = 11;
            break;
        case Value::stream:
            width = 0.9f;
            minZoom = 13;
            break;
        default:
            width = 0.7f;
            minZoom = 14;
            break;
    }
    if (zoom < minZoom) {
        return false;
    }
    style = { palette.water, static_cast<uint8_t>(info.brunnel == Value::tunnel ? 90 : 255), std::fmax(0.6f, width * zoomScale(zoom)) };
    return true;
}

bool areaColor(const FeatureInfo& info, const Palette& palette, uint16_t& color, uint8_t& opacity) {
    opacity = 255;
    switch (info.layer) {
        case Layer::Water:
            color = palette.water;
            return true;
        case Layer::Park:
            color = palette.park;
            opacity = 140;
            return true;
        case Layer::Building:
            color = palette.building;
            return true;
        case Layer::Landcover:
            switch (info.featureClass) {
                case Value::wood:
                    color = palette.wood;
                    return true;
                case Value::grass:
                    color = info.subclass == Value::park || info.subclass == Value::garden ? palette.park : palette.grass;
                    return true;
                case Value::farmland:
                    color = palette.farmland;
                    return true;
                case Value::wetland:
                    color = palette.wetland;
                    return true;
                case Value::sand:
                    color = palette.sand;
                    return true;
                case Value::ice:
                    color = palette.ice;
                    return true;
                case Value::rock:
                    color = palette.rock;
                    return true;
                default:
                    return false;
            }
        case Layer::Landuse:
            switch (info.featureClass) {
                case Value::residential:
                case Value::suburb:
                case Value::quarter:
                case Value::neighbourhood:
                    color = palette.residential;
                    opacity = 150;
                    return true;
                case Value::commercial:
                case Value::retail:
                    color = palette.commercial;
                    return true;
                case Value::industrial:
                case Value::garages:
                case Value::railway:
                case Value::dam:
                    color = palette.industrial;
                    return true;
                case Value::cemetery:
                    color = palette.cemetery;
                    return true;
                case Value::hospital:
                    color = palette.hospital;
                    return true;
                case Value::school:
                case Value::university:
                case Value::college:
                case Value::kindergarten:
                    color = palette.school;
                    return true;
                case Value::pitch:
                case Value::stadium:
                case Value::playground:
                case Value::track:
                case Value::theme_park:
                case Value::zoo:
                    color = palette.pitch;
                    return true;
                case Value::military:
                    color = palette.military;
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

} // namespace

Layer stageLayer(Stage stage) {
    switch (stage) {
        case Stage::Landcover:
            return Layer::Landcover;
        case Stage::Landuse:
            return Layer::Landuse;
        case Stage::Park:
            return Layer::Park;
        case Stage::Water:
            return Layer::Water;
        case Stage::Waterway:
            return Layer::Waterway;
        case Stage::Aeroway:
            return Layer::Aeroway;
        case Stage::Boundary:
            return Layer::Boundary;
        case Stage::Building:
            return Layer::Building;
        case Stage::RoadCasing:
        case Stage::RoadFill:
            return Layer::Transportation;
        default:
            return Layer::Unknown;
    }
}

void styleCacheId(bool monochrome, char* out, size_t size) {
    snprintf(out, size, "%c%u", monochrome ? 'm' : 'c', static_cast<unsigned>(STYLE_VERSION));
}

const Palette& colorPalette() {
    return COLOR_PALETTE;
}

const Palette& monochromePalette() {
    return MONOCHROME_PALETTE;
}

bool styleFor(const FeatureInfo& info, Stage stage, int zoom, const Palette& palette, DrawStyle& style) {
    switch (stage) {
        case Stage::Landcover:
        case Stage::Landuse:
        case Stage::Park:
        case Stage::Water:
        case Stage::Building: {
            if (info.geometryType != GEOMETRY_POLYGON) {
                return false;
            }
            if (stage == Stage::Building && zoom < 13) {
                return false;
            }
            uint16_t color;
            uint8_t opacity;
            if (!areaColor(info, palette, color, opacity)) {
                return false;
            }
            style = { color, opacity, 0.0f };
            return true;
        }
        case Stage::Waterway:
            return waterwayStyle(info, zoom, palette, style);
        case Stage::Aeroway:
            if (info.geometryType == GEOMETRY_POLYGON) {
                const bool paved = info.featureClass == Value::runway || info.featureClass == Value::taxiway || info.featureClass == Value::apron;
                style = { paved ? palette.runway : palette.aerodrome, 255, 0.0f };
                return info.featureClass != Value::None;
            }
            if (info.geometryType == GEOMETRY_LINE && (info.featureClass == Value::runway || info.featureClass == Value::taxiway)) {
                const float width = info.featureClass == Value::runway ? 6.0f : 2.0f;
                style = { palette.runway, 255, std::fmax(0.8f, width * zoomScale(zoom)) };
                return zoom >= 11;
            }
            return false;
        case Stage::Boundary:
            if (info.geometryType != GEOMETRY_LINE || info.maritime || info.adminLevel < 0 || info.adminLevel > 4) {
                return false;
            }
            style = { palette.boundary, 170, info.adminLevel <= 2 ? 1.4f : 0.9f };
            return true;
        case Stage::RoadCasing:
        case Stage::RoadFill:
            return roadStyle(info, stage, zoom, palette, style);
        default:
            return false;
    }
}

} // namespace maps
