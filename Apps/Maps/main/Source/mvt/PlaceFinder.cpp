// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaceFinder.h"

#include "../platform/Platform.h"

#include <cmath>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t WIRE_VARINT = 0;
constexpr uint32_t WIRE_64BIT = 1;
constexpr uint32_t WIRE_LENGTH = 2;
constexpr uint32_t WIRE_32BIT = 5;

constexpr uint32_t COMMAND_MOVE_TO = 1;
constexpr uint32_t COMMAND_LINE_TO = 2;
constexpr uint32_t COMMAND_CLOSE_PATH = 7;

constexpr int32_t zigzag(uint32_t value) {
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}

bool streamEnd(size_t start, uint64_t length, size_t& end) {
    if (length > static_cast<uint64_t>(SIZE_MAX - start)) {
        return false;
    }
    end = start + static_cast<size_t>(length);
    return true;
}

} // namespace

PlaceFinder::~PlaceFinder() {
    largeFree(valueOffsets);
    largeFree(valueLengths);
    largeFree(valueRanks);
}

bool PlaceFinder::init() {
    if (valueOffsets == nullptr) {
        valueOffsets = static_cast<uint32_t*>(largeAlloc(VALUE_TABLE_SIZE * sizeof(uint32_t)));
    }
    if (valueLengths == nullptr) {
        valueLengths = static_cast<uint16_t*>(largeAlloc(VALUE_TABLE_SIZE * sizeof(uint16_t)));
    }
    if (valueRanks == nullptr) {
        valueRanks = static_cast<uint8_t*>(largeAlloc(VALUE_TABLE_SIZE * sizeof(uint8_t)));
    }
    return reader.init() && valueOffsets != nullptr && valueLengths != nullptr && valueRanks != nullptr;
}

bool PlaceFinder::readVarint(uint64_t& value) {
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

bool PlaceFinder::skipField(uint32_t wireType) {
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

uint8_t PlaceFinder::valueRank(uint32_t index) const {
    if (index >= layer.valueCount || index >= VALUE_TABLE_SIZE) {
        return 0;
    }
    return valueRanks[index];
}

bool PlaceFinder::scanValue(size_t valueEnd, uint32_t index) {
    uint32_t offset = 0;
    uint16_t length = 0;
    uint8_t rank = 0;
    bool stored = false;
    while (reader.tell() < valueEnd) {
        uint64_t key;
        if (!readVarint(key)) {
            return false;
        }
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 7);
        if (field == 1 && wire == WIRE_LENGTH) {
            uint64_t stringLength;
            if (!readVarint(stringLength)) {
                return false;
            }
            const size_t stringOffset = reader.tell();
            if (stringLength < 16) {
                char text[16];
                if (reader.read(reinterpret_cast<uint8_t*>(text), static_cast<size_t>(stringLength)) != stringLength) {
                    return false;
                }
                text[stringLength] = '\0';
                if (strcmp(text, "city") == 0) {
                    rank = 1;
                } else if (strcmp(text, "town") == 0) {
                    rank = 2;
                } else if (strcmp(text, "village") == 0) {
                    rank = 3;
                }
            } else if (!reader.skip(static_cast<size_t>(stringLength))) {
                return false;
            }
            if (stringOffset <= 0xFFFFFFFFu && stringLength <= 0xFFFFFFFFu) {
                offset = static_cast<uint32_t>(stringOffset);
                length = static_cast<uint16_t>(stringLength > 0xFFFF ? 0xFFFF : stringLength);
                stored = true;
            }
        } else if (!skipField(wire)) {
            return false;
        }
        if (reader.tell() > valueEnd) {
            return false;
        }
    }
    if (index < VALUE_TABLE_SIZE) {
        valueOffsets[index] = stored ? offset : 0;
        valueLengths[index] = stored ? length : 0;
        valueRanks[index] = rank;
    }
    return true;
}

bool PlaceFinder::scanTile(ByteInput* tile) {
    layer = {};
    layer.extent = 4096;
    layer.classKey = -1;
    layer.nameKey = -1;
    layer.latinKey = -1;
    if (!reader.begin(tile)) {
        return false;
    }
    while (true) {
        uint64_t key;
        if (!readVarint(key)) {
            break;
        }
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 7);
        if (field == 3 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            const size_t layerStart = reader.tell();
            size_t layerEnd;
            if (!streamEnd(layerStart, length, layerEnd)) {
                return false;
            }
            bool isPlace = false;
            bool nameSeen = false;
            int16_t classKey = -1;
            int16_t nameKey = -1;
            int16_t latinKey = -1;
            int32_t extent = 4096;
            uint32_t keyIndex = 0;
            uint32_t valueIndex = 0;
            while (reader.tell() < layerEnd) {
                uint64_t layerKey;
                if (!readVarint(layerKey)) {
                    return false;
                }
                const uint32_t layerField = static_cast<uint32_t>(layerKey >> 3);
                const uint32_t layerWire = static_cast<uint32_t>(layerKey & 7);
                if (layerField == 1 && layerWire == WIRE_LENGTH) {
                    uint64_t nameLength;
                    if (!readVarint(nameLength)) {
                        return false;
                    }
                    char name[16];
                    if (nameLength < sizeof(name)) {
                        if (reader.read(reinterpret_cast<uint8_t*>(name), static_cast<size_t>(nameLength)) != nameLength) {
                            return false;
                        }
                        name[nameLength] = '\0';
                        isPlace = nameLength == 5 && strcmp(name, "place") == 0;
                    } else if (!reader.skip(static_cast<size_t>(nameLength))) {
                        return false;
                    }
                    nameSeen = true;
                    if (!isPlace) {
                        if (reader.tell() < layerEnd && !reader.skip(layerEnd - reader.tell())) {
                            return false;
                        }
                        break;
                    }
                } else if (layerField == 3 && layerWire == WIRE_LENGTH) {
                    uint64_t keyLength;
                    if (!readVarint(keyLength)) {
                        return false;
                    }
                    char text[16];
                    if (keyLength < sizeof(text)) {
                        if (reader.read(reinterpret_cast<uint8_t*>(text), static_cast<size_t>(keyLength)) != keyLength) {
                            return false;
                        }
                        text[keyLength] = '\0';
                        if (strcmp(text, "class") == 0) {
                            classKey = static_cast<int16_t>(keyIndex);
                        } else if (strcmp(text, "name") == 0) {
                            nameKey = static_cast<int16_t>(keyIndex);
                        } else if (strcmp(text, "name:latin") == 0) {
                            latinKey = static_cast<int16_t>(keyIndex);
                        }
                    } else if (!reader.skip(static_cast<size_t>(keyLength))) {
                        return false;
                    }
                    keyIndex++;
                } else if (layerField == 4 && layerWire == WIRE_LENGTH) {
                    uint64_t valueLength;
                    if (!readVarint(valueLength)) {
                        return false;
                    }
                    const size_t valueStart = reader.tell();
                    size_t valueEnd;
                    if (!streamEnd(valueStart, valueLength, valueEnd) || valueEnd > layerEnd) {
                        return false;
                    }
                    if (isPlace || !nameSeen) {
                        if (!scanValue(valueEnd, valueIndex)) {
                            return false;
                        }
                    } else if (!reader.skip(valueEnd - valueStart)) {
                        return false;
                    }
                    valueIndex++;
                } else if (layerField == 5 && layerWire == WIRE_VARINT) {
                    uint64_t value;
                    if (!readVarint(value)) {
                        return false;
                    }
                    extent = static_cast<int32_t>(value);
                } else if (!skipField(layerWire)) {
                    return false;
                }
                if (reader.tell() > layerEnd) {
                    return false;
                }
            }
            if (isPlace && nameSeen) {
                layer.found = true;
                layer.start = layerStart;
                layer.length = static_cast<size_t>(length);
                layer.extent = extent > 0 ? extent : 4096;
                layer.classKey = classKey;
                layer.nameKey = nameKey;
                layer.latinKey = latinKey;
                layer.valueCount = valueIndex;
                break;
            }
            if (reader.tell() < layerEnd && !reader.skip(layerEnd - reader.tell())) {
                return false;
            }
        } else if (!skipField(wire)) {
            return false;
        }
    }
    return !reader.failed();
}

bool PlaceFinder::readGeometry(size_t geometryEnd, int32_t& px, int32_t& py, bool& hasPoint) {
    int32_t x = 0;
    int32_t y = 0;
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
                if (command == COMMAND_MOVE_TO && !hasPoint) {
                    px = x;
                    py = y;
                    hasPoint = true;
                }
            }
        } else if (command != COMMAND_CLOSE_PATH) {
            return false;
        }
        if (reader.tell() > geometryEnd) {
            return false;
        }
    }
    return true;
}

bool PlaceFinder::readFeature(size_t featureEnd, double fx, double fy) {
    uint8_t type = 0;
    int32_t px = 0;
    int32_t py = 0;
    bool hasPoint = false;
    uint32_t nameIndex = 0;
    bool hasName = false;
    uint32_t latinIndex = 0;
    bool hasLatin = false;
    int rank = -1;
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
            const size_t tagsStart = reader.tell();
            size_t tagsEnd;
            if (!streamEnd(tagsStart, length, tagsEnd) || tagsEnd > featureEnd) {
                return false;
            }
            while (reader.tell() < tagsEnd) {
                uint64_t tagKey;
                uint64_t tagValue;
                if (!readVarint(tagKey) || !readVarint(tagValue)) {
                    return false;
                }
                const uint32_t tagKeyIndex = static_cast<uint32_t>(tagKey);
                const uint32_t tagValueIndex = static_cast<uint32_t>(tagValue);
                if (layer.classKey >= 0 && tagKeyIndex == static_cast<uint32_t>(layer.classKey)) {
                    const uint8_t value = valueRank(tagValueIndex);
                    if (value > 0) {
                        rank = static_cast<int>(value) - 1;
                    }
                } else if (layer.nameKey >= 0 && tagKeyIndex == static_cast<uint32_t>(layer.nameKey)) {
                    nameIndex = tagValueIndex;
                    hasName = true;
                } else if (layer.latinKey >= 0 && tagKeyIndex == static_cast<uint32_t>(layer.latinKey)) {
                    latinIndex = tagValueIndex;
                    hasLatin = true;
                }
            }
        } else if (field == 3 && wire == WIRE_VARINT) {
            uint64_t value;
            if (!readVarint(value)) {
                return false;
            }
            type = static_cast<uint8_t>(value);
        } else if (field == 4 && wire == WIRE_LENGTH) {
            uint64_t length;
            if (!readVarint(length)) {
                return false;
            }
            size_t geometryEnd;
            if (!streamEnd(reader.tell(), length, geometryEnd) || geometryEnd > featureEnd) {
                return false;
            }
            if (!readGeometry(geometryEnd, px, py, hasPoint)) {
                return false;
            }
        } else if (!skipField(wire)) {
            return false;
        }
        if (reader.tell() > featureEnd) {
            return false;
        }
    }
    if (hasLatin && latinIndex < layer.valueCount && latinIndex < VALUE_TABLE_SIZE && valueLengths[latinIndex] > 0) {
        nameIndex = latinIndex;
        hasName = true;
    }
    if (type != 1 || !hasPoint || !hasName || rank < 0) {
        return true;
    }
    if (nameIndex >= layer.valueCount || nameIndex >= VALUE_TABLE_SIZE || valueLengths[nameIndex] == 0) {
        return true;
    }
    const int32_t extent = layer.extent > 0 ? layer.extent : 4096;
    const double dx = static_cast<double>(px) / extent - fx;
    const double dy = static_cast<double>(py) / extent - fy;
    const double distance = sqrt(dx * dx + dy * dy);
    const double score = distance * (1 + rank);
    if (bestRank < 0 || score < bestScore) {
        bestScore = score;
        bestDistance = distance * extent;
        bestRank = rank;
        bestName = nameIndex;
    }
    return true;
}

bool PlaceFinder::findFeatures(ByteInput* tile, double fx, double fy) {
    bestScore = 0.0;
    bestDistance = 0.0;
    bestRank = -1;
    bestName = 0;
    if (!reader.begin(tile)) {
        return false;
    }
    if (!reader.skip(layer.start)) {
        return false;
    }
    const size_t layerEnd = layer.start + layer.length;
    while (reader.tell() < layerEnd) {
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
            const size_t featureStart = reader.tell();
            size_t featureEnd;
            if (!streamEnd(featureStart, length, featureEnd) || featureEnd > layerEnd) {
                return false;
            }
            if (!readFeature(featureEnd, fx, fy)) {
                return false;
            }
            if (reader.tell() < featureEnd && !reader.skip(featureEnd - reader.tell())) {
                return false;
            }
        } else if (!skipField(wire)) {
            return false;
        }
        if (reader.tell() > layerEnd) {
            return false;
        }
    }
    return !reader.failed();
}

bool PlaceFinder::readName(ByteInput* tile, uint32_t index, PlaceName& out) {
    if (index >= layer.valueCount || index >= VALUE_TABLE_SIZE) {
        return false;
    }
    const uint16_t length = valueLengths[index];
    if (length == 0) {
        return false;
    }
    if (!reader.begin(tile)) {
        return false;
    }
    if (!reader.skip(valueOffsets[index])) {
        return false;
    }
    const size_t count = length < 47 ? length : 47;
    if (reader.read(reinterpret_cast<uint8_t*>(out.name), count) != count) {
        return false;
    }
    size_t cut = count;
    if (length > count) {
        while (cut > 0 && (static_cast<uint8_t>(out.name[cut - 1]) & 0xC0) == 0x80) {
            cut--;
        }
        if (cut > 0 && (static_cast<uint8_t>(out.name[cut - 1]) & 0xC0) == 0xC0) {
            cut--;
        }
    }
    out.name[cut] = '\0';
    out.rank = bestRank;
    out.distance = bestDistance;
    return true;
}

bool PlaceFinder::nearest(ByteInput* tile, double fx, double fy, PlaceName& out) {
    if (!init()) {
        return false;
    }
    if (!scanTile(tile)) {
        return false;
    }
    if (!layer.found) {
        return false;
    }
    if (!findFeatures(tile, fx, fy)) {
        return false;
    }
    if (bestRank < 0) {
        return false;
    }
    return readName(tile, bestName, out);
}

} // namespace maps
