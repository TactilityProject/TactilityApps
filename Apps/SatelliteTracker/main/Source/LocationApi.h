#pragma once

#if __has_include(<location/location.h>)
#include <location/location.h>
#else

// Declarations from Tactility's location-module (Modules/location-module/include/location/location.h, Apache-2.0),
// used while the app SDK does not ship that header yet. They must match the firmware.

#include <stdbool.h>
#include <stdint.h>

#include <tactility/concurrent/task_event_group.h>
#include <tactility/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOCATION_PROVIDER_ID_MAX_LENGTH 15

struct Location {
    double latitude;
    double longitude;
    float horizontal_accuracy;
    float altitude;
    float ellipsoidal_altitude;
    float vertical_accuracy;
    float speed;
    float speed_accuracy;
    float course;
    float course_accuracy;
    uint8_t satellites;
    int64_t time_ms;
    uint64_t uptime_ms;
    char provider[LOCATION_PROVIDER_ID_MAX_LENGTH + 1];
};

#define LOCATION_SUBSCRIBER_DESCRIPTION_MAX_LENGTH 24

struct LocationSubscription {
    uint32_t bit;

    struct {
        char description[LOCATION_SUBSCRIBER_DESCRIPTION_MAX_LENGTH + 1];
        struct TaskEventGroup* event_group;
        struct LocationSubscription* next;
    } internal;
};

bool location_is_enabled(void);
error_t location_get(struct Location* location);
error_t location_subscribe(struct LocationSubscription* sub, struct TaskEventGroup* event_group, const char* description);
error_t location_unsubscribe(struct LocationSubscription* sub);

#ifdef __cplusplus
}
#endif

#endif
