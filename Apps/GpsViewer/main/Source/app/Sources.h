// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppContext.h"
#include "Recorder.h"

#include <gps/gps.h>
#include <location/location.h>
#include <tactility/device.h>

#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

namespace gpsviewer {

// Live data from the first started GPS_TYPE device.
// A GPS subscription holds only the latest event, and the driver's thread runs at a high priority.
// So a reader task with a higher priority copies every event into a queue right away, otherwise
// the parts of a GSV set get overwritten before they are read. The app task drains the queue.
class GpsSource {
public:
    // Subscribes to the location service, which starts the receiver and shows the location icon,
    // and starts the reader task. App task only.
    void start(AppContext& context, TaskEventGroup* eventGroup);
    // Feeds queued events into the model and the recorder, waiting at most timeoutMs for the first. App task only.
    void poll(AppContext& context, uint32_t timeoutMs, Recorder& recorder);
    // Stops the reader task, which drops the GPS subscription, then the location subscription. App task only.
    void stop();

private:
    static void readerMain(void* argument);
    void readerLoop();
    // Reader task only
    void tryConnect(uint32_t now);
    void disconnect();

    AppContext* context = nullptr;
    QueueHandle_t queue = nullptr;
    TaskHandle_t reader = nullptr;
    std::atomic<bool> stopRequested {false};
    std::atomic<bool> readerDone {true};
    LocationSubscription location {};
    bool locationSubscribed = false;


    // Reader task only
    Device* device = nullptr;
    GpsSubscription subscription {};
    bool subscribed = false;
    uint32_t nextAttemptMs = 0;
    bool lost = false;
};

} // namespace gpsviewer
