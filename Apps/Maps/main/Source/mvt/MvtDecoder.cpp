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

} // namespace

MvtDecoder::~MvtDecoder() {
    largeFree(valuePool);
    largeFree(chunk);
}

bool MvtDecoder::init() {
    if (valuePool == nullptr) {
        valuePool = static_cast<uint16_t*>(largeAlloc(VALUE_POOL_SIZE * sizeof(uint16_t)));
    }
    if (chunk == nullptr) {
        chunk = static_cast<TilePoint*>(largeAlloc(CHUNK_POINTS * sizeof(TilePoint)));
    }
    return reader.init() && valuePool != nullptr && chunk != nullptr;
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

uint16_t MvtDecoder::valueCode(const LayerTable& table, uint32_t index) const {
    if (index >= table.valueCount) {
        return 0;
    }
    return valuePool[table.valueOffset + index];
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

bool MvtDecoder::scanValue(size_t valueEnd, uint16_t& code) {
    code = 0;
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
            char text[32];
            if (length < sizeof(text)) {
                if (reader.read(reinterpret_cast<uint8_t*>(text), static_cast<size_t>(length)) != length) {
                    return false;
                }
                code = static_cast<uint16_t>(valueFromString(text, static_cast<size_t>(length)));
            } else if (!reader.skip(static_cast<size_t>(length))) {
                return false;
            }
        } else if ((field == 4 || field == 5 || field == 7) && wire == WIRE_VARINT) {
            if (!readVarint(number)) {
                return false;
            }
            code = integerCode(static_cast<int64_t>(number));
        } else if (field == 6 && wire == WIRE_VARINT) {
            if (!readVarint(number)) {
                return false;
            }
            code = integerCode(zigzag(static_cast<uint32_t>(number)));
        } else if (!skipField(wire)) {
            return false;
        }
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
            uint16_t code;
            if (!scanValue(reader.tell() + static_cast<size_t>(length), code)) {
                return false;
            }
            if (valuePoolUsed < VALUE_POOL_SIZE) {
                valuePool[valuePoolUsed++] = code;
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

bool MvtDecoder::scan(ByteInput* source) {
    input = source;
    valuePoolUsed = 0;
    totalSize = 0;
    for (auto& table : tables) {
        table = { -1, -1, -1, -1, -1, -1, -1, 0, 0, 4096 };
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
    const uint16_t code = valueCode(table, value);
    const bool isInteger = (code & INTEGER_FLAG) != 0;
    if (keyIndex == table.classKey && !isInteger) {
        info.featureClass = static_cast<Value>(code);
    } else if (keyIndex == table.subclassKey && !isInteger) {
        info.subclass = static_cast<Value>(code);
    } else if (keyIndex == table.brunnelKey && !isInteger) {
        info.brunnel = static_cast<Value>(code);
    } else if (keyIndex == table.adminLevelKey && isInteger) {
        info.adminLevel = static_cast<int8_t>(code & 0x7F);
    } else if (keyIndex == table.maritimeKey && isInteger) {
        info.maritime = (code & 0x7FFF) != 0;
    } else if (keyIndex == table.rampKey && isInteger) {
        info.ramp = (code & 0x7FFF) != 0;
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
