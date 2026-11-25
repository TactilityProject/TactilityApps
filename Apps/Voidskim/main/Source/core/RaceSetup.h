#pragma once

#include "Controller.h"

#include <string>
#include <vector>

namespace hd {

/** Who drives a racer. */
enum class SlotKind : uint8_t {
    Local, // a person on this device
    AI, // the computer, on the device that hosts the race
    Remote, // a person on another device (see net/NetSession.h)
};

struct RacerSlot {
    SlotKind kind = SlotKind::AI;
    std::string name;
    int vehicle = 0;
    uint8_t peer = 0; // for Remote slots: which peer drives it
};

/**
 * Everything needed to start the same race on every device: the menu builds one for a local race,
 * and a future network lobby would agree on one (the host sends it to the others, see
 * net/Protocol.h). Slot order is grid order.
 */
struct RaceSetup {
    int track = 0; // index into the track list
    uint32_t trackChecksum = 0; // so peers can tell they loaded the same track text
    int laps = 3;
    Difficulty difficulty = Difficulty::Normal;
    uint32_t seed = 1; // drives the AI's choices: same seed, same race
    std::vector<RacerSlot> slots;
};

/** A checksum of a track's text (FNV-1a), for RaceSetup::trackChecksum. */
inline uint32_t textChecksum(const std::string& text) {
    uint32_t hash = 2166136261u;
    for (char c : text) {
        hash = (hash ^ (uint8_t)c) * 16777619u;
    }
    return hash;
}

} // namespace hd
