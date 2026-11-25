#pragma once

#include "../Controller.h"
#include "Protocol.h"

namespace hd::net {

/**
 * A racer driven by a person on another device. Inputs arrive from the NetSession (decoded
 * InputMessages); until a newer one arrives, the last known input is repeated, which is what a
 * player holding a button usually means anyway.
 */
class RemoteController final : public Controller {
public:
    /** Called by the network layer for each InputMessage addressed to this racer. */
    void receive(const InputMessage& message) {
        if (message.tick >= latestTick) {
            latestTick = message.tick;
            latest = message.input;
        }
    }

    RacerInput control(const RaceSim&, int) override { return latest; }

private:
    uint32_t latestTick = 0;
    RacerInput latest;
};

} // namespace hd::net
