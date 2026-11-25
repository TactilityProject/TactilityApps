#pragma once

#include <cstddef>
#include <cstdint>

namespace hd {

/**
 * A connection to the other devices in a networked race, independent of the transport (Wi-Fi UDP,
 * ESP-NOW, BLE, ...). Not implemented yet: this is the seam a network mode plugs into.
 *
 * How a networked race is meant to work with the rest of the game:
 * - Lobby: the host builds a RaceSetup (core/RaceSetup.h) and sends it as a StartRace message
 *   (net/Protocol.h). Every device then creates the same RaceSim from it. Slots of kind Local on the
 *   host become Remote slots on the other devices, and the other way round.
 * - Racing: every device runs the full simulation. Each tick, each device sends the inputs of its
 *   Local racers (InputMessage) and feeds the inputs it receives into RemoteControllers
 *   (net/RemoteController.h). AI racers are driven on every device from the shared seed.
 * - Drift: floating point results can differ between chip architectures, so the host also sends a
 *   SnapshotMessage now and then. Other devices correct their racers to it (net/Protocol.h).
 *
 * Messages are small (tens of bytes) and can arrive late, out of order, or not at all, so the
 * receiving side keeps going with the last known input.
 */
class NetSession {
public:
    virtual ~NetSession() = default;

    /** @return this device's peer id (0 is the host) */
    virtual uint8_t localPeer() const = 0;

    /** @return whether this device decides the race setup and sends snapshots */
    virtual bool isHost() const = 0;

    /** Sends a message to every other peer. Unreliable: it may be lost. */
    virtual void broadcast(const uint8_t* data, size_t size) = 0;

    /**
     * Fetches the next received message.
     * @param[out] fromPeer the sender
     * @return the message size, or 0 when there's nothing waiting
     */
    virtual size_t receive(uint8_t* buffer, size_t capacity, uint8_t& fromPeer) = 0;
};

} // namespace hd
