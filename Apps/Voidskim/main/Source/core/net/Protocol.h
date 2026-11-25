#pragma once

#include "../RaceSetup.h"

#include <cstddef>
#include <cstdint>

namespace hd::net {

/**
 * Messages exchanged over a NetSession. Little-endian, packed by hand so the format doesn't depend
 * on the compiler or architecture. Every message starts with its type and the protocol version.
 */
constexpr uint8_t PROTOCOL_VERSION = 1;

enum class MessageType : uint8_t {
    StartRace = 1, // host -> all: the RaceSetup to start
    Input = 2, // anyone -> all: one racer's input for one tick
    Snapshot = 3, // host -> all: authoritative state of one racer
};

/** The largest encoded message, for receive buffers. */
constexpr size_t MAX_MESSAGE_SIZE = 256;

struct InputMessage {
    uint32_t tick = 0;
    uint8_t racer = 0;
    RacerInput input;
};

struct SnapshotMessage {
    uint32_t tick = 0;
    uint8_t racer = 0;
    float x = 0, z = 0, height = 0;
    float vx = 0, vz = 0, verticalSpeed = 0;
    float heading = 0;
    float boostEnergy = 0;
    int8_t lapsCompleted = 0;
    uint8_t mode = 0;
    uint8_t flags = 0; // bit 0: airborne, bit 1: finished, bit 2: passed halfway
};

/** @return the encoded size, or 0 when it doesn't fit */
size_t encode(const InputMessage& message, uint8_t* buffer, size_t capacity);
size_t encode(const SnapshotMessage& message, uint8_t* buffer, size_t capacity);
/** Racer names longer than 15 characters are cut short. */
size_t encode(const RaceSetup& setup, uint8_t* buffer, size_t capacity);

/** @return the type of an encoded message, or 0 when it isn't one of ours */
uint8_t peekType(const uint8_t* buffer, size_t size);

bool decode(const uint8_t* buffer, size_t size, InputMessage& out);
bool decode(const uint8_t* buffer, size_t size, SnapshotMessage& out);
bool decode(const uint8_t* buffer, size_t size, RaceSetup& out);

/** Captures a racer for a SnapshotMessage. */
SnapshotMessage snapshotOf(const Racer& racer, uint32_t tick);

/** Corrects a racer to a received snapshot. */
void applySnapshot(const SnapshotMessage& snapshot, Racer& racer);

} // namespace hd::net
