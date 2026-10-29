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
    X(tunnel) X(ford) X(aerodrome) X(heliport) X(runway) X(helipad) X(taxiway) X(apron) X(gate)

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
