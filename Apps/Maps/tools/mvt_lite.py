# SPDX-License-Identifier: GPL-3.0-or-later
"""A small pure-Python reader for Mapbox Vector Tiles (MVT 2.1), gzip or plain, for the PC tools.

    for layer in read_tile(path_or_bytes):
        layer.name, layer.extent
        for feature in layer.features:
            feature.type        # 1 point, 2 line, 3 polygon
            feature.tags        # dict: key -> value (str, float, int or bool)
            feature.geometry()  # list of parts, each a list of (x, y) in tile units (0..extent)

No dependencies beyond the standard library.
"""

import gzip
import zlib


def _varint(data, pos):
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if byte < 0x80:
            return result, pos
        shift += 7


def _fields(data):
    """Yields (field number, wire type, value) where value is an int or a bytes slice."""
    pos = 0
    end = len(data)
    while pos < end:
        key, pos = _varint(data, pos)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = _varint(data, pos)
        elif wire == 2:
            length, pos = _varint(data, pos)
            value = data[pos:pos + length]
            pos += length
        elif wire == 1:
            value = data[pos:pos + 8]
            pos += 8
        elif wire == 5:
            value = data[pos:pos + 4]
            pos += 4
        else:
            raise ValueError("unsupported wire type %d" % wire)
        yield number, wire, value


def _packed(data):
    values = []
    pos = 0
    while pos < len(data):
        value, pos = _varint(data, pos)
        values.append(value)
    return values


def _zigzag(n):
    return (n >> 1) ^ -(n & 1)


def _value(data):
    import struct
    for number, wire, value in _fields(data):
        if number == 1:
            return bytes(value).decode("utf-8", "replace")
        if number == 2:
            return struct.unpack("<f", value)[0]
        if number == 3:
            return struct.unpack("<d", value)[0]
        if number == 4:
            return value if value < (1 << 63) else value - (1 << 64)
        if number == 5:
            return value
        if number == 6:
            return _zigzag(value)
        if number == 7:
            return bool(value)
    return None


class Feature:
    __slots__ = ("id", "type", "tags", "_geometry")

    def __init__(self):
        self.id = None
        self.type = 0
        self.tags = {}
        self._geometry = []

    def geometry(self):
        parts = []
        current = None
        x = y = 0
        commands = self._geometry
        i = 0
        while i < len(commands):
            command, count = commands[i] & 7, commands[i] >> 3
            i += 1
            if command == 7:  # ClosePath
                if current:
                    current.append(current[0])
                continue
            for _ in range(count):
                x += _zigzag(commands[i])
                y += _zigzag(commands[i + 1])
                i += 2
                if command == 1:  # MoveTo
                    current = [(x, y)]
                    parts.append(current)
                else:  # LineTo
                    current.append((x, y))
        return parts


class Layer:
    __slots__ = ("name", "extent", "features")

    def __init__(self):
        self.name = ""
        self.extent = 4096
        self.features = []


def read_tile(source):
    """source: a file path or the tile's bytes (gzip, zlib or plain)."""
    if not isinstance(source, (bytes, bytearray, memoryview)):
        with open(source, "rb") as f:
            data = f.read()
    else:
        data = bytes(source)
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    elif data[:1] == b"\x78":
        data = zlib.decompress(data)
    data = memoryview(data)
    layers = []
    for number, _, value in _fields(data):
        if number != 3:
            continue
        layer = Layer()
        keys, values, raw_features = [], [], []
        for n, _, v in _fields(value):
            if n == 1:
                layer.name = bytes(v).decode("utf-8")
            elif n == 2:
                raw_features.append(v)
            elif n == 3:
                keys.append(bytes(v).decode("utf-8"))
            elif n == 4:
                values.append(_value(v))
            elif n == 5:
                layer.extent = v
        for raw in raw_features:
            feature = Feature()
            for n, _, v in _fields(raw):
                if n == 1:
                    feature.id = v
                elif n == 2:
                    tags = _packed(v)
                    for k in range(0, len(tags) - 1, 2):
                        feature.tags[keys[tags[k]]] = values[tags[k + 1]]
                elif n == 3:
                    feature.type = v
                elif n == 4:
                    feature._geometry = _packed(v)
            layer.features.append(feature)
        layers.append(layer)
    return layers


if __name__ == "__main__":
    import sys
    for layer in read_tile(sys.argv[1]):
        print("%-20s %5d features, extent %d" % (layer.name, len(layer.features), layer.extent))
