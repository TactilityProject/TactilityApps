// SPDX-License-Identifier: GPL-3.0-or-later
#include "MvtDecoder.h"

#include "../platform/Platform.h"

#include <cstring>

namespace maps {

namespace {

constexpr uint32_t WIRE_VARINT = 0;
constexpr uint32_t WIRE_64BIT = 1;
constexpr uint32_t WIRE_LENGTH = 2;
constexpr uint32_t WIRE_32BIT = 5;

constexpr uint16_t INTEGER_FLAG = 0x8000;

constexpr uint32_t COMMAND_MOVE_TO = 1;
constexpr uint32_t COMMAND_LINE_TO = 2;
constexpr uint32_t COMMAND_CLOSE_PATH = 7;

constexpr int32_t zigzag(uint32_t value) {
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}

uint16_t integerCode(int64_t value) {
    if (value < 0) {
        return 0;
    }
    return static_cast<uint16_t>(INTEGER_FLAG | (value > 0x7FFF ? 0x7FFF : value));
}

int32_t clampRank(int64_t value) {
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(value);
}

} // namespace

MvtDecoder::~MvtDecoder() {
    largeFree(valuePool);
    largeFree(namePool);
    largeFree(chunk);
}

bool MvtDecoder::init() {
    if (valuePool == nullptr) {
        valuePool = static_cast<ValueEntry*>(largeAlloc(VALUE_POOL_SIZE * sizeof(ValueEntry)));
        valuePoolCapacity = valuePool != nullptr ? VALUE_POOL_SIZE : 0;
    }
    if (namePool == nullptr) {
        namePool = static_cast<char*>(largeAlloc(NAME_POOL_SIZE));
        namePoolCapacity = namePool != nullptr ? NAME_POOL_SIZE : 0;
    }
    if (chunk == nullptr) {
        chunk = static_cast<TilePoint*>(largeAlloc(CHUNK_POINTS * sizeof(TilePoint)));
    }
    return reader.init() && valuePool != nullptr && namePool != nullptr && chunk != nullptr;
}

bool MvtDecoder::growPools() {
    // Without memory for the bigger pools the normal ones stay: more POI strings go without text, nothing fails
    if (valuePoolCapacity < POI_VALUE_POOL_SIZE) {
        if (auto* bigger = static_cast<ValueEntry*>(largeAlloc(POI_VALUE_POOL_SIZE * sizeof(ValueEntry)))) {
            largeFree(valuePool);
            valuePool = bigger;
            valuePoolCapacity = POI_VALUE_POOL_SIZE;
        }
    }
    if (namePoolCapacity < POI_NAME_POOL_SIZE) {
        if (auto* bigger = static_cast<char*>(largeAlloc(POI_NAME_POOL_SIZE))) {
            largeFree(namePool);
            namePool = bigger;
            namePoolCapacity = POI_NAME_POOL_SIZE;
        }
    }
    return valuePool != nullptr && namePool != nullptr;
}

void MvtDecoder::shrinkPools() {
    if (valuePoolCapacity > VALUE_POOL_SIZE) {
        if (auto* smaller = static_cast<ValueEntry*>(largeAlloc(VALUE_POOL_SIZE * sizeof(ValueEntry)))) {
            largeFree(valuePool);
            valuePool = smaller;
            valuePoolCapacity = VALUE_POOL_SIZE;
        }
    }
    if (namePoolCapacity > NAME_POOL_SIZE) {
        if (auto* smaller = static_cast<char*>(largeAlloc(NAME_POOL_SIZE))) {
            largeFree(namePool);
            namePool = smaller;
            namePoolCapacity = NAME_POOL_SIZE;
        }
    }
    valuePoolUsed = 0;
    namePoolUsed = 0;
}

bool MvtDecoder::readVarint(uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const int byte = reader.get();
        if (byte < 0) {
            return false;
        }
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

bool MvtDecoder::skipField(uint32_t wireType) {
    uint64_t value;
    switch (wireType) {
        case WIRE_VARINT:
            return readVarint(value);
        case WIRE_64BIT:
            return reader.skip(8);
        case WIRE_LENGTH:
            return readVarint(value) && reader.skip(static_cast<size_t>(value));
        case WIRE_32BIT:
            return reader.skip(4);
        default:
            return false;
    }
}

const MvtDecoder::ValueEntry* MvtDecoder::valueEntry(const LayerTable& table, uint32_t index) const {
    if (index >= table.valueCount) {
        return nullptr;
    }
    return &valuePool[table.valueOffset + index];
}

bool MvtDecoder::readLayerName(size_t layerEnd, Layer& layer) {
    layer = Layer::Unknown;
    while (reader.tell() < layerEnd) {
        uint64_t key;
        if (!readVarint(key)) {
            return false;
        }
        if ((key >> 3) == 1 && (key & 7) == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            char name[24];
            if (length >= sizeof(name)) {
                return reader.skip(static_cast<size_t>(length));
            }
            if (reader.read(reinterpret_cast<uint8_t*>(name), static_cast<size_t>(length)) != length) {
                return false;
            }
            layer = layerFromName(name, static_cast<size_t>(length));
            return true;
        }
        if (!skipField(static_cast<uint32_t>(key & 7))) {
            return false;
        }
    }
    return true;
}

bool MvtDecoder::scanValue(size_t valueEnd, ValueEntry& entry) {
    entry = {};
    entry.rank = -1;
    uint16_t code = 0;
    int32_t rank = -1;
    uint32_t textOffset = 0;
    uint8_t textLength = 0;
    bool hasText = false;
    while (reader.tell() < valueEnd) {
        uint64_t key;
        if (!readVarint(key)) {
            return false;
        }
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 7);
        uint64_t number;
        if (field == 1 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            char text[256];
            size_t stored = 0;
            if (length > 0) {
                const size_t wanted = static_cast<size_t>(length);
                stored = wanted < sizeof(text) ? wanted : sizeof(text) - 1;
                if (reader.read(reinterpret_cast<uint8_t*>(text), stored) != stored) {
                    return false;
                }
                if (wanted > stored) {
                    if (!reader.skip(wanted - stored)) {
                        return false;
                    }
                    while (stored > 0 && (static_cast<uint8_t>(text[stored - 1]) & 0xC0) == 0x80) {
                        stored--;
                    }
                    if (stored > 0 && (static_cast<uint8_t>(text[stored - 1]) & 0xC0) == 0xC0) {
                        stored--;
                    }
                }
                code = static_cast<uint16_t>(valueFromString(text, stored));
                if (stored > 0 && namePool != nullptr && namePoolUsed + stored <= namePoolCapacity) {
                    memcpy(namePool + namePoolUsed, text, stored);
                    textOffset = namePoolUsed;
                    textLength = static_cast<uint8_t>(stored);
                    hasText = true;
                    namePoolUsed += static_cast<uint32_t>(stored);
                }
            }
        } else if ((field == 4 || field == 5) && wire == WIRE_VARINT) {
            if (!readVarint(number)) {
                return false;
            }
            const int64_t value = static_cast<int64_t>(number);
            code = integerCode(value);
            rank = clampRank(value);
        } else if (field == 6 && wire == WIRE_VARINT) {
            if (!readVarint(number)) {
                return false;
            }
            const int64_t value = zigzag(static_cast<uint32_t>(number));
            code = integerCode(value);
            rank = clampRank(value);
        } else if (field == 7 && wire == WIRE_VARINT) {
            if (!readVarint(number)) {
                return false;
            }
            code = integerCode(static_cast<int64_t>(number));
        } else if (!skipField(wire)) {
            return false;
        }
    }
    entry.code = code;
    entry.textLength = textLength;
    entry.hasText = static_cast<uint8_t>(hasText ? 1 : 0);
    if (hasText) {
        entry.textOffset = textOffset;
    } else {
        entry.rank = rank;
    }
    return true;
}

bool MvtDecoder::scanLayer(size_t layerEnd, LayerTable& table, Layer layer) {
    (void)layer;
    uint32_t keyIndex = 0;
    table.valueOffset = valuePoolUsed;
    table.valueCount = 0;
    while (reader.tell() < layerEnd) {
        uint64_t key;
        if (!readVarint(key)) {
            return false;
        }
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 7);
        if (field == 3 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            char name[16];
            if (length < sizeof(name)) {
                if (reader.read(reinterpret_cast<uint8_t*>(name), static_cast<size_t>(length)) != length) {
                    return false;
                }
                name[length] = '\0';
                const auto index = static_cast<int16_t>(keyIndex);
                if (strcmp(name, "class") == 0) {
                    table.classKey = index;
                } else if (strcmp(name, "subclass") == 0) {
                    table.subclassKey = index;
                } else if (strcmp(name, "brunnel") == 0) {
                    table.brunnelKey = index;
                } else if (strcmp(name, "admin_level") == 0) {
                    table.adminLevelKey = index;
                } else if (strcmp(name, "maritime") == 0) {
                    table.maritimeKey = index;
                } else if (strcmp(name, "ramp") == 0) {
                    table.rampKey = index;
                } else if (strcmp(name, "name") == 0) {
                    table.nameKey = index;
                } else if (strcmp(name, "name:latin") == 0) {
                    table.nameLatinKey = index;
                } else if (strcmp(name, "rank") == 0) {
                    table.rankKey = index;
                } else if (strcmp(name, "housenumber") == 0) {
                    table.houseNumberKey = index;
                }
            } else if (!reader.skip(static_cast<size_t>(length))) {
                return false;
            }
            keyIndex++;
        } else if (field == 4 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            ValueEntry entry;
            if (!scanValue(reader.tell() + static_cast<size_t>(length), entry)) {
                return false;
            }
            if (valuePoolUsed < valuePoolCapacity) {
                valuePool[valuePoolUsed++] = entry;
                table.valueCount++;
            }
        } else if (field == 5 && wire == WIRE_VARINT) {
            uint64_t extent;
            if (!readVarint(extent)) {
                return false;
            }
            table.extent = static_cast<int32_t>(extent);
        } else if (!skipField(wire)) {
            return false;
        }
    }
    return true;
}

bool MvtDecoder::scan(ByteInput* source, bool withPoiLayers) {
    input = source;
    if (withPoiLayers && !growPools()) {
        return false;
    }
    valuePoolUsed = 0;
    namePoolUsed = 0;
    totalSize = 0;
    for (auto& table : tables) {
        table = {};
    }
    if (!reader.begin(input)) {
        return false;
    }
    int8_t layerIndex = 0;
    while (true) {
        uint64_t key;
        if (!readVarint(key)) {
            break;
        }
        if ((key >> 3) == 3 && (key & 7) == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            const size_t layerEnd = reader.tell() + static_cast<size_t>(length);
            Layer layer;
            if (!readLayerName(layerEnd, layer)) {
                return false;
            }
            if (!withPoiLayers && (layer == Layer::Poi || layer == Layer::Housenumber)) {
                layer = Layer::Unknown;
            }
            if (layer != Layer::Unknown) {
                LayerTable& table = tables[static_cast<int>(layer)];
                table.fileIndex = layerIndex;
                if (!scanLayer(layerEnd, table, layer)) {
                    return false;
                }
            }
            if (reader.tell() < layerEnd && !reader.skip(layerEnd - reader.tell())) {
                return false;
            }
            layerIndex++;
        } else if (!skipField(static_cast<uint32_t>(key & 7))) {
            return false;
        }
    }
    totalSize = reader.tell();
    return !reader.failed();
}

void MvtDecoder::applyTag(const LayerTable& table, uint32_t key, uint32_t value, FeatureInfo& info) const {
    const auto keyIndex = static_cast<int16_t>(key);
    const ValueEntry* entry = valueEntry(table, value);
    if (entry == nullptr) {
        return;
    }
    const uint16_t code = entry->code;
    const bool isInteger = (code & INTEGER_FLAG) != 0;
    if (keyIndex == table.classKey && !isInteger) {
        info.featureClass = static_cast<Value>(code);
        if (entry->hasText != 0) {
            info.classText = namePool + entry->textOffset;
            info.classTextLength = entry->textLength;
        }
    } else if (keyIndex == table.subclassKey && !isInteger) {
        info.subclass = static_cast<Value>(code);
        if (entry->hasText != 0) {
            info.subclassText = namePool + entry->textOffset;
            info.subclassTextLength = entry->textLength;
        }
    } else if (keyIndex == table.brunnelKey && !isInteger) {
        info.brunnel = static_cast<Value>(code);
    } else if (keyIndex == table.adminLevelKey && isInteger) {
        info.adminLevel = static_cast<int8_t>(code & 0x7F);
    } else if (keyIndex == table.maritimeKey && isInteger) {
        info.maritime = (code & 0x7FFF) != 0;
    } else if (keyIndex == table.rampKey && isInteger) {
        info.ramp = (code & 0x7FFF) != 0;
    } else if (keyIndex == table.nameKey && entry->hasText != 0) {
        info.name = namePool + entry->textOffset;
        info.nameLength = entry->textLength;
    } else if (keyIndex == table.nameLatinKey && entry->hasText != 0) {
        info.nameLatin = namePool + entry->textOffset;
        info.nameLatinLength = entry->textLength;
    } else if (keyIndex == table.rankKey && isInteger) {
        info.rank = entry->rank;
    } else if (keyIndex == table.houseNumberKey && entry->hasText != 0) {
        info.houseNumber = namePool + entry->textOffset;
        info.houseNumberLength = entry->textLength;
    }
}

bool MvtDecoder::decodeGeometry(size_t geometryEnd, GeometrySink& sink) {
    int32_t x = 0;
    int32_t y = 0;
    int count = 0;
    bool ringOpen = false;
    while (reader.tell() < geometryEnd) {
        uint64_t commandInteger;
        if (!readVarint(commandInteger)) {
            return false;
        }
        const uint32_t command = static_cast<uint32_t>(commandInteger & 7);
        const uint32_t repeat = static_cast<uint32_t>(commandInteger >> 3);
        if (command == COMMAND_MOVE_TO || command == COMMAND_LINE_TO) {
            for (uint32_t i = 0; i < repeat; i++) {
                uint64_t dx;
                uint64_t dy;
                if (!readVarint(dx) || !readVarint(dy)) {
                    return false;
                }
                x += zigzag(static_cast<uint32_t>(dx));
                y += zigzag(static_cast<uint32_t>(dy));
                if (command == COMMAND_MOVE_TO) {
                    if (ringOpen) {
                        sink.points(chunk, count);
                        sink.endRing(false);
                    }
                    sink.beginRing();
                    ringOpen = true;
                    count = 0;
                } else if (count == CHUNK_POINTS) {
                    sink.points(chunk, count);
                    chunk[0] = chunk[count - 1];
                    count = 1;
                }
                chunk[count++] = { x, y };
            }
        } else if (command == COMMAND_CLOSE_PATH) {
            if (ringOpen) {
                sink.points(chunk, count);
                sink.endRing(true);
                ringOpen = false;
                count = 0;
            }
        } else {
            return false;
        }
    }
    if (ringOpen) {
        sink.points(chunk, count);
        sink.endRing(false);
    }
    return true;
}

bool MvtDecoder::decodeFeature(size_t featureEnd, Layer layer, GeometrySink& sink) {
    const LayerTable& table = tables[static_cast<int>(layer)];
    FeatureInfo info;
    info.layer = layer;
    while (reader.tell() < featureEnd) {
        uint64_t key;
        if (!readVarint(key)) {
            return false;
        }
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 7);
        if (field == 2 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            const size_t tagsEnd = reader.tell() + static_cast<size_t>(length);
            while (reader.tell() < tagsEnd) {
                uint64_t tagKey;
                uint64_t tagValue;
                if (!readVarint(tagKey) || !readVarint(tagValue)) {
                    return false;
                }
                applyTag(table, static_cast<uint32_t>(tagKey), static_cast<uint32_t>(tagValue), info);
            }
        } else if (field == 3 && wire == WIRE_VARINT) {
            uint64_t type;
            if (!readVarint(type)) {
                return false;
            }
            info.geometryType = static_cast<uint8_t>(type);
        } else if (field == 4 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            const size_t geometryEnd = reader.tell() + static_cast<size_t>(length);
            if (sink.beginFeature(info)) {
                if (!decodeGeometry(geometryEnd, sink)) {
                    return false;
                }
                sink.endFeature();
            } else if (!reader.skip(static_cast<size_t>(length))) {
                return false;
            }
        } else if (!skipField(wire)) {
            return false;
        }
    }
    return true;
}

bool MvtDecoder::inflate(ByteInput* source, uint8_t* out, size_t size) {
    if (!reader.begin(source)) {
        return false;
    }
    if (reader.read(out, size) != size || reader.failed()) {
        return false;
    }
    // Nothing may follow
    return reader.get() < 0 && !reader.failed();
}

bool MvtDecoder::sweep(const Layer* layers, int count, GeometrySink& sink) {
    int remaining = 0;
    bool wanted[static_cast<int>(Layer::Count)] = {};
    for (int i = 0; i < count; i++) {
        const int index = static_cast<int>(layers[i]);
        if (tables[index].fileIndex >= 0 && !wanted[index]) {
            wanted[index] = true;
            remaining++;
        }
    }
    if (remaining == 0) {
        return true;
    }
    if (!reader.begin(input)) {
        return false;
    }
    while (remaining > 0) {
        uint64_t key;
        if (!readVarint(key)) {
            break;
        }
        if ((key >> 3) != 3 || (key & 7) != WIRE_LENGTH) {
            if (!skipField(static_cast<uint32_t>(key & 7))) {
                return false;
            }
            continue;
        }
        uint64_t length;
        if (!readVarint(length)) {
            return false;
        }
        const size_t layerEnd = reader.tell() + static_cast<size_t>(length);
        Layer layer;
        if (!readLayerName(layerEnd, layer)) {
            return false;
        }
        if (layer != Layer::Unknown && wanted[static_cast<int>(layer)]) {
            while (reader.tell() < layerEnd) {
                uint64_t layerKey;
                if (!readVarint(layerKey)) {
                    return false;
                }
                if ((layerKey >> 3) == 2 && (layerKey & 7) == WIRE_LENGTH) {
                    if (sink.shouldStop()) {
                        return false;
                    }
                    uint64_t featureLength;
                    if (!readVarint(featureLength)) {
                        return false;
                    }
                    if (!decodeFeature(reader.tell() + static_cast<size_t>(featureLength), layer, sink)) {
                        return false;
                    }
                } else if (!skipField(static_cast<uint32_t>(layerKey & 7))) {
                    return false;
                }
            }
            remaining--;
        } else if (reader.tell() < layerEnd && !reader.skip(layerEnd - reader.tell())) {
            return false;
        }
    }
    return !reader.failed();
}

} // namespace maps
