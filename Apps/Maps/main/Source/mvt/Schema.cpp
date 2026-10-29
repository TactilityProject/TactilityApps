// SPDX-License-Identifier: GPL-3.0-or-later
#include "Schema.h"

#include <cstring>

namespace maps {

namespace {

const char* const VALUE_NAMES[] = {
    "",
#define MAPS_VALUE_NAME(name) #name,
    MAPS_SCHEMA_VALUES(MAPS_VALUE_NAME)
#undef MAPS_VALUE_NAME
};

struct LayerName {
    const char* name;
    Layer layer;
};

constexpr LayerName LAYER_NAMES[] = {
    { "aeroway", Layer::Aeroway },
    { "boundary", Layer::Boundary },
    { "building", Layer::Building },
    { "landcover", Layer::Landcover },
    { "landuse", Layer::Landuse },
    { "park", Layer::Park },
    { "transportation", Layer::Transportation },
    { "water", Layer::Water },
    { "waterway", Layer::Waterway },
};

bool equals(const char* text, size_t length, const char* name) {
    return strlen(name) == length && memcmp(text, name, length) == 0;
}

} // namespace

Layer layerFromName(const char* name, size_t length) {
    for (const auto& entry : LAYER_NAMES) {
        if (equals(name, length, entry.name)) {
            return entry.layer;
        }
    }
    return Layer::Unknown;
}

Value valueFromString(const char* text, size_t length) {
    for (uint16_t i = 1; i < static_cast<uint16_t>(Value::Count); i++) {
        if (equals(text, length, VALUE_NAMES[i])) {
            return static_cast<Value>(i);
        }
    }
    return Value::None;
}

const char* valueName(Value value) {
    const auto index = static_cast<uint16_t>(value);
    return index < static_cast<uint16_t>(Value::Count) ? VALUE_NAMES[index] : "";
}

} // namespace maps
