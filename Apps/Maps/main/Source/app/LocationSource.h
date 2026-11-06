// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Position.h"

#include <location/location.h>

namespace maps {

/**
 * The position from Tactility's location module, the app's only position source. Subscribing makes the
 * location service run its providers (GPS, IP lookup) while the map is open, and signals a bit of the app's
 * event group for each new fix. Location settings belong to the user: only the menu's Location switch turns the
 * service on or off, and the precision is never changed.
 */
class LocationSource final : public PositionSource {
public:
    ~LocationSource() override { unsubscribe(); }

    bool subscribe(TaskEventGroup* eventGroup);
    void unsubscribe();
    /** The event group bit that is signalled for a new fix, 0 when not subscribed. */
    uint32_t bit() const { return subscribed ? storage.subscription.bit : 0; }

    PositionRead read(PositionFix& fix) override;

    static bool isEnabled();
    /** Starts or stops Tactility's location service and stores the choice in its settings. */
    static bool setEnabled(bool enabled);

private:
    /**
     * Firmware from November 2026 on keeps a 25-byte description in the subscription's internal part, so its
     * subscription is larger than the one of the SDK this app is built with. The service owns those internal fields
     * and the app only reads bit (the first field in both), so room for either layout keeps both firmwares working.
     */
    union Storage {
        LocationSubscription subscription;
        uint8_t bytes[64];
    };
    Storage storage {};
    bool subscribed = false;
};

} // namespace maps
