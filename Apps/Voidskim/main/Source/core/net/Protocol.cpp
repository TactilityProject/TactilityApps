#include "Protocol.h"

#include <cstring>

namespace hd::net {

namespace {

/** Appends little-endian values to a buffer; remembers when it ran out of room. */
class Writer {
public:
    Writer(uint8_t* buffer, size_t capacity) : buffer(buffer), capacity(capacity) {}

    void u8(uint8_t v) {
        if (size + 1 > capacity) { overflow = true; return; }
        buffer[size++] = v;
    }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void f32(float v) { uint32_t bits; memcpy(&bits, &v, 4); u32(bits); }
    void str(const std::string& s, size_t maxLength) {
        const size_t n = s.size() < maxLength ? s.size() : maxLength;
        u8((uint8_t)n);
        for (size_t i = 0; i < n; i++) u8((uint8_t)s[i]);
    }
    size_t finish() const { return overflow ? 0 : size; }

private:
    uint8_t* buffer;
    size_t capacity;
    size_t size = 0;
    bool overflow = false;
};

class Reader {
public:
    Reader(const uint8_t* buffer, size_t size) : buffer(buffer), size(size) {}

    uint8_t u8() {
        if (offset + 1 > size) { underflow = true; return 0; }
        return buffer[offset++];
    }
    uint16_t u16() { const uint16_t lo = u8(); return (uint16_t)(lo | (u8() << 8)); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
    float f32() { const uint32_t bits = u32(); float v; memcpy(&v, &bits, 4); return v; }
    std::string str() {
        const uint8_t n = u8();
        std::string s;
        for (uint8_t i = 0; i < n && !underflow; i++) s.push_back((char)u8());
        return s;
    }
    bool ok() const { return !underflow; }

private:
    const uint8_t* buffer;
    size_t size;
    size_t offset = 0;
    bool underflow = false;
};

void header(Writer& w, MessageType type) {
    w.u8((uint8_t)type);
    w.u8(PROTOCOL_VERSION);
}

bool checkHeader(Reader& r, MessageType type) {
    return r.u8() == (uint8_t)type && r.u8() == PROTOCOL_VERSION;
}

} // namespace

uint8_t peekType(const uint8_t* buffer, size_t size) {
    if (size < 2 || buffer[1] != PROTOCOL_VERSION) {
        return 0;
    }
    return buffer[0];
}

size_t encode(const InputMessage& message, uint8_t* buffer, size_t capacity) {
    Writer w(buffer, capacity);
    header(w, MessageType::Input);
    w.u32(message.tick);
    w.u8(message.racer);
    w.u8((uint8_t)(int8_t)(clampf(message.input.steer, -1.0f, 1.0f) * 127.0f));
    w.u8((uint8_t)((message.input.throttle ? 1 : 0) | (message.input.brake ? 2 : 0) | (message.input.boost ? 4 : 0)));
    return w.finish();
}

bool decode(const uint8_t* buffer, size_t size, InputMessage& out) {
    Reader r(buffer, size);
    if (!checkHeader(r, MessageType::Input)) return false;
    out.tick = r.u32();
    out.racer = r.u8();
    out.input.steer = (float)(int8_t)r.u8() / 127.0f;
    const uint8_t buttons = r.u8();
    out.input.throttle = (buttons & 1) != 0;
    out.input.brake = (buttons & 2) != 0;
    out.input.boost = (buttons & 4) != 0;
    return r.ok();
}

size_t encode(const SnapshotMessage& m, uint8_t* buffer, size_t capacity) {
    Writer w(buffer, capacity);
    header(w, MessageType::Snapshot);
    w.u32(m.tick);
    w.u8(m.racer);
    w.f32(m.x); w.f32(m.z); w.f32(m.height);
    w.f32(m.vx); w.f32(m.vz); w.f32(m.verticalSpeed);
    w.f32(m.heading);
    w.f32(m.boostEnergy);
    w.u8((uint8_t)m.lapsCompleted);
    w.u8(m.mode);
    w.u8(m.flags);
    return w.finish();
}

bool decode(const uint8_t* buffer, size_t size, SnapshotMessage& m) {
    Reader r(buffer, size);
    if (!checkHeader(r, MessageType::Snapshot)) return false;
    m.tick = r.u32();
    m.racer = r.u8();
    m.x = r.f32(); m.z = r.f32(); m.height = r.f32();
    m.vx = r.f32(); m.vz = r.f32(); m.verticalSpeed = r.f32();
    m.heading = r.f32();
    m.boostEnergy = r.f32();
    m.lapsCompleted = (int8_t)r.u8();
    m.mode = r.u8();
    m.flags = r.u8();
    return r.ok();
}

size_t encode(const RaceSetup& setup, uint8_t* buffer, size_t capacity) {
    Writer w(buffer, capacity);
    header(w, MessageType::StartRace);
    w.u8((uint8_t)setup.track);
    w.u32(setup.trackChecksum);
    w.u8((uint8_t)setup.laps);
    w.u8((uint8_t)setup.difficulty);
    w.u32(setup.seed);
    w.u8((uint8_t)setup.slots.size());
    for (const auto& slot : setup.slots) {
        w.u8((uint8_t)slot.kind);
        w.u8((uint8_t)slot.vehicle);
        w.u8(slot.peer);
        w.str(slot.name, 15);
    }
    return w.finish();
}

bool decode(const uint8_t* buffer, size_t size, RaceSetup& setup) {
    Reader r(buffer, size);
    if (!checkHeader(r, MessageType::StartRace)) return false;
    setup.track = r.u8();
    setup.trackChecksum = r.u32();
    setup.laps = r.u8();
    setup.difficulty = (Difficulty)r.u8();
    setup.seed = r.u32();
    const uint8_t count = r.u8();
    setup.slots.clear();
    for (uint8_t i = 0; i < count && r.ok(); i++) {
        RacerSlot slot;
        slot.kind = (SlotKind)r.u8();
        slot.vehicle = r.u8();
        slot.peer = r.u8();
        slot.name = r.str();
        setup.slots.push_back(slot);
    }
    return r.ok();
}

SnapshotMessage snapshotOf(const Racer& racer, uint32_t tick) {
    SnapshotMessage m;
    m.tick = tick;
    m.racer = (uint8_t)racer.id;
    m.x = racer.position.x;
    m.z = racer.position.z;
    m.height = racer.height;
    m.vx = racer.velocity.x;
    m.vz = racer.velocity.z;
    m.verticalSpeed = racer.verticalSpeed;
    m.heading = racer.heading;
    m.boostEnergy = racer.boostEnergy;
    m.lapsCompleted = (int8_t)racer.lapsCompleted;
    m.mode = (uint8_t)racer.mode;
    m.flags = (uint8_t)((racer.airborne ? 1 : 0) | (racer.finished ? 2 : 0) | (racer.passedHalfway ? 4 : 0));
    return m;
}

void applySnapshot(const SnapshotMessage& m, Racer& racer) {
    racer.position = { m.x, m.z };
    racer.height = m.height;
    racer.velocity = { m.vx, m.vz };
    racer.verticalSpeed = m.verticalSpeed;
    racer.heading = m.heading;
    racer.boostEnergy = m.boostEnergy;
    racer.lapsCompleted = m.lapsCompleted;
    racer.mode = (RacerMode)m.mode;
    racer.airborne = (m.flags & 1) != 0;
    racer.finished = (m.flags & 2) != 0;
    racer.passedHalfway = (m.flags & 4) != 0;
}

} // namespace hd::net
