// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/** The OpenMapTiles layers that are drawn. Every other layer is skipped. */
enum class Layer : uint8_t {
    Unknown,
    Aeroway,
    Boundary,
    Building,
    Landcover,
    Landuse,
    Park,
    Transportation,
    Water,
    Waterway,
    Place,
    TransportationName,
    WaterName,
    MountainPeak,
    /** Not drawn. Read only when a caller asks the decoder to scan them (MvtDecoder::scan), for the label records. */
    Poi,
    Housenumber,
    Count
};

#define MAPS_SCHEMA_VALUES(X) \
    X(ocean) X(lake) X(river) X(pond) X(dock) X(swimming_pool) X(stream) X(canal) X(drain) X(ditch) \
    X(farmland) X(ice) X(wood) X(rock) X(grass) X(wetland) X(sand) \
    X(residential) X(suburb) X(quarter) X(neighbourhood) X(commercial) X(industrial) X(retail) X(railway) \
    X(cemetery) X(military) X(hospital) X(school) X(university) X(college) X(kindergarten) X(stadium) \
    X(pitch) X(playground) X(track) X(theme_park) X(zoo) X(garages) X(dam) X(park) X(garden) \
    X(national_park) X(nature_reserve) X(protected_area) \
    X(motorway) X(trunk) X(primary) X(secondary) X(tertiary) X(minor) X(service) X(path) X(raceway) \
    X(busway) X(bus_guideway) X(ferry) X(rail) X(transit) X(cable_car) X(gondola) X(pier) X(bridge) \
    X(tunnel) X(ford) X(aerodrome) X(heliport) X(runway) X(helipad) X(taxiway) X(apron) X(gate) \
    X(city) X(town) X(village) X(hamlet) X(isolated_dwelling) X(continent) X(country) X(state) \
    X(province) X(island) X(peak) X(volcano) X(saddle) X(sea) X(bay) X(strait) X(lagoon)

/** String values the style needs. Value::None means a string that is not in this list. */
enum class Value : uint16_t {
    None,
#define MAPS_VALUE_ENUM(name) name,
    MAPS_SCHEMA_VALUES(MAPS_VALUE_ENUM)
#undef MAPS_VALUE_ENUM
    Count
};

/** Feature attributes the style needs, resolved from the tags of one feature. */
struct FeatureInfo {
    Layer layer = Layer::Unknown;
    uint8_t geometryType = 0;
    Value featureClass = Value::None;
    Value subclass = Value::None;
    Value brunnel = Value::None;
    int8_t adminLevel = -1;
    bool maritime = false;
    bool ramp = false;
    /** UTF-8 name of the feature from "name", not null-terminated, nullptr when absent. Valid during beginFeature(). */
    const char* name = nullptr;
    uint16_t nameLength = 0;
    /** The same from "name:latin" (OpenMapTiles' Latin-script name), nullptr when absent. */
    const char* nameLatin = nullptr;
    uint16_t nameLatinLength = 0;
    /** "rank" as an integer, -1 when absent (place, mountain_peak and poi layers). */
    int32_t rank = -1;
    /** The raw "class" and "subclass" strings of any layer, nullptr when absent. Valid during beginFeature(). */
    const char* classText = nullptr;
    uint16_t classTextLength = 0;
    const char* subclassText = nullptr;
    uint16_t subclassTextLength = 0;
    /** The "housenumber" tag (housenumber layer), nullptr when absent. */
    const char* houseNumber = nullptr;
    uint16_t houseNumberLength = 0;
};

enum GeometryType : uint8_t {
    GEOMETRY_UNKNOWN = 0,
    GEOMETRY_POINT = 1,
    GEOMETRY_LINE = 2,
    GEOMETRY_POLYGON = 3
};

Layer layerFromName(const char* name, size_t length);
Value valueFromString(const char* text, size_t length);
const char* valueName(Value value);

} // namespace maps
