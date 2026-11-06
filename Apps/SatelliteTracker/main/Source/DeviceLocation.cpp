#include "DeviceLocation.h"

void DeviceLocation::update(TaskEventGroup* eventGroup, bool wanted) {
    bool shouldRun = wanted && location_is_enabled();
    if (shouldRun && !subscribed) {
        subscription = LocationSubscription {};
        subscribed = location_subscribe(&subscription, eventGroup, "Satellite Tracker") == ERROR_NONE;
    } else if (!shouldRun && subscribed) {
        stop();
    }
}

void DeviceLocation::stop() {
    if (subscribed) {
        location_unsubscribe(&subscription);
        subscribed = false;
    }
    lastUptimeMs = 0;
}

bool DeviceLocation::poll(Location& out) {
    if (!subscribed) return false;
    Location fix {};
    if (location_get(&fix) != ERROR_NONE || fix.uptime_ms == lastUptimeMs) return false;
    lastUptimeMs = fix.uptime_ms;
    out = fix;
    return true;
}
