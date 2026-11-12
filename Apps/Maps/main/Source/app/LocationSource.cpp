// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocationSource.h"

#include "../platform/Platform.h"

namespace maps {

namespace {

/** location_subscribe() of newer firmware, which takes what the subscriber is. Older firmware ignores the argument. */
using SubscribeWithDescription = error_t (*)(LocationSubscription* sub, TaskEventGroup* eventGroup, const char* description);

} // namespace

bool LocationSource::subscribe(TaskEventGroup* eventGroup) {
    if (subscribed) {
        return true;
    }
    storage = {};
    // Through a function type without arguments, which GCC accepts as a deliberate cast between function types
    const auto subscribeCall = reinterpret_cast<SubscribeWithDescription>(reinterpret_cast<void (*)()>(&location_subscribe));
    subscribed = subscribeCall(&storage.subscription, eventGroup, "Maps") == ERROR_NONE;
    logInfo("Location services %s, %s", location_is_enabled() ? "on" : "off", subscribed ? "subscribed" : "not subscribed");
    return subscribed;
}

void LocationSource::unsubscribe() {
    if (subscribed) {
        location_unsubscribe(&storage.subscription);
        subscribed = false;
    }
}

bool LocationSource::isEnabled() {
    return location_is_enabled();
}

bool LocationSource::setEnabled(bool enabled) {
    // The subscription stays registered while the service is stopped, new fixes signal it again once it runs
    const error_t error = location_set_enabled(enabled);
    logInfo("Location services switched %s: %s", enabled ? "on" : "off", error == ERROR_NONE ? "done" : "failed");
    return error == ERROR_NONE;
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
