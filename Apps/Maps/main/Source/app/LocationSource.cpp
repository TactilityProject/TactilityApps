// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocationSource.h"

#include "../platform/Platform.h"

namespace maps {

bool LocationSource::subscribe(TaskEventGroup* eventGroup) {
    if (subscribed) {
        return true;
    }
    subscription = {};
    subscribed = location_subscribe(&subscription, eventGroup) == ERROR_NONE;
    logInfo("Location services %s, %s", location_is_enabled() ? "on" : "off", subscribed ? "subscribed" : "not subscribed");
    return subscribed;
}

void LocationSource::unsubscribe() {
    if (subscribed) {
        location_unsubscribe(&subscription);
        subscribed = false;
    }
}

PositionRead LocationSource::read(PositionFix& fix) {
    if (!location_is_enabled()) {
        return PositionRead::Off;
    }
    Location location {};
    if (location_get(&location) != ERROR_NONE) {
        return PositionRead::NoFix;
    }
    fix.latitude = location.latitude;
    fix.longitude = location.longitude;
    fix.accuracy = location.horizontal_accuracy;
    fix.course = location.course;
    fix.speed = location.speed;
    fix.timeMs = location.time_ms;
    return PositionRead::Fix;
}

} // namespace maps
