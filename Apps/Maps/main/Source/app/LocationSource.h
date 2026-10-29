// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Position.h"

#include <location/location.h>

namespace maps {

/**
 * The position from Tactility's location module, the app's only position source. Subscribing makes the
 * location service run its providers (GPS, IP lookup) while the map is open, and signals a bit of the app's
 * event group for each new fix. Location settings belong to the user: this never enables location or
 * changes its precision.
 */
class LocationSource final : public PositionSource {
public:
    ~LocationSource() override { unsubscribe(); }

    bool subscribe(TaskEventGroup* eventGroup);
    void unsubscribe();
    /** The event group bit that is signalled for a new fix, 0 when not subscribed. */
    uint32_t bit() const { return subscribed ? subscription.bit : 0; }

    PositionRead read(PositionFix& fix) override;

private:
    LocationSubscription subscription {};
    bool subscribed = false;
};

} // namespace maps
