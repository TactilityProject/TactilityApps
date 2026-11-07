// SPDX-License-Identifier: GPL-3.0-or-later
#include "Sources.h"

#include <tactility/time.h>

#include <cstring>

namespace gpsviewer {

namespace {

// The location service starts the receiver when the app subscribes, so the first checks come quickly
constexpr uint32_t CONNECT_INTERVAL_MS = 250;
constexpr int QUEUE_LENGTH = 48;
constexpr uint32_t READER_IDLE_MS = 100;
constexpr uint32_t READER_STACK_SIZE = 3072;
// Above the GPS driver's thread (THREAD_PRIORITY_HIGH, 5) so no event is overwritten before it's copied
constexpr UBaseType_t READER_PRIORITY = 6;

// Firmware headers without GpsEvent::talker select the second overload: talker unknown,
// and satellites are then told apart by their ID ranges only
template <typename Event>
auto copyTalker(const Event& event, char* out, int) -> decltype(event.talker, void()) {
    memcpy(out, event.talker, 2);
    out[2] = '\0';
}

template <typename Event>
void copyTalker(const Event&, char* out, long) {
    out[0] = '\0';
}

bool toGnssEvent(const GpsEvent& in, GnssEvent& out) {
    memset(&out, 0, sizeof(out));
    copyTalker(in, out.talker, 0);
    switch (in.type) {
        case GPS_EVENT_MESSAGE_RMC:
            out.type = GnssEventType::Rmc;
            out.data.rmc = in.data.rmc;
            return true;
        case GPS_EVENT_MESSAGE_GGA:
            out.type = GnssEventType::Gga;
            out.data.gga = in.data.gga;
            return true;
        case GPS_EVENT_MESSAGE_GSV:
            out.type = GnssEventType::Gsv;
            out.data.gsv = in.data.gsv;
            return true;
        case GPS_EVENT_MESSAGE_GSA:
            out.type = GnssEventType::Gsa;
            out.data.gsa = in.data.gsa;
            return true;
        default:
            return false;
    }
}

void setStatus(AppContext& context, SourceStatus status) {
    ContextLock lock(context);
    context.status = status;
}

} // namespace

// The driver frees its state when the device stops, after it sent GPS_EVENT_UNSUBSCRIBED and
// dropped all subscriptions. So the API is only called on a started device, and never after that event.
void GpsSource::tryConnect(uint32_t now) {
    if (static_cast<int32_t>(now - nextAttemptMs) < 0) {
        return;
    }
    nextAttemptMs = now + CONNECT_INTERVAL_MS;

    if (!location_is_enabled()) {
        setStatus(*context, SourceStatus::LocationOff);
        return;
    }
    Device* found = nullptr;
    if (device_get_first_active_by_type(&GPS_TYPE, &found) != ERROR_NONE) {
        Device* configured = nullptr;
        const bool exists = device_get_first_by_type(&GPS_TYPE, &configured) == ERROR_NONE;
        if (exists) {
            device_put(configured);
        }
        setStatus(*context, lost ? SourceStatus::ReceiverLost : exists ? SourceStatus::ReceiverStarting : SourceStatus::NoReceiver);
        return;
    }
    if (gps_get_state(found) != GPS_STATE_ON) {
        device_put(found);
        setStatus(*context, lost ? SourceStatus::ReceiverLost : SourceStatus::ReceiverStarting);
        return;
    }
    subscription = {};
    if (gps_event_subscribe(found, &subscription) != ERROR_NONE) {
        device_put(found);
        return;
    }
    device = found;
    subscribed = true;
    lost = false;

    char name[32] = "";
    gps_get_model_name(device, name, sizeof(name));
    ContextLock lock(*context);
    memcpy(context->receiverName, name, sizeof(context->receiverName));
    context->receiverName[sizeof(context->receiverName) - 1] = '\0';
    context->status = SourceStatus::Receiving;
    context->model.reset(now);
}

void GpsSource::disconnect() {
    if (subscribed) {
        if (device_is_ready(device)) {
            gps_event_unsubscribe(device, &subscription);
        }
        subscribed = false;
        device_put(device);
        device = nullptr;
    }
    lost = false;
    nextAttemptMs = 0;
}

void GpsSource::readerLoop() {
    while (!stopRequested.load()) {
        if (!subscribed) {
            tryConnect(get_millis());
            if (!subscribed) {
                vTaskDelay(pdMS_TO_TICKS(READER_IDLE_MS));
                continue;
            }
        }
        if (gps_event_await(device, &subscription, pdMS_TO_TICKS(READER_IDLE_MS)) != ERROR_NONE) {
            continue;
        }
        const GpsEvent event = subscription.event;
        if (event.type == GPS_EVENT_UNSUBSCRIBED) {
            subscribed = false;
            lost = true;
            device_put(device);
            device = nullptr;
            setStatus(*context, SourceStatus::ReceiverLost);
            continue;
        }
        GnssEvent gnss;
        if (toGnssEvent(event, gnss)) {
            // A full queue drops the event, the next epoch repeats the data
            xQueueSend(queue, &gnss, 0);
        }
    }
    disconnect();
}

void GpsSource::readerMain(void* argument) {
    auto* source = static_cast<GpsSource*>(argument);
    source->readerLoop();
    source->readerDone.store(true);
    vTaskDelete(nullptr);
}

// Only the location service starts GPS receivers. Its subscription also keeps the receiver running
// while this app reads the raw sentences.
void GpsSource::start(AppContext& appContext, TaskEventGroup* eventGroup) {
    context = &appContext;
    if (!locationSubscribed) {
        location = {};
        locationSubscribed = location_subscribe(&location, eventGroup, "GPS Viewer") == ERROR_NONE;
    }
    if (queue == nullptr) {
        queue = xQueueCreate(QUEUE_LENGTH, sizeof(GnssEvent));
    }
    if (queue == nullptr || !readerDone.load()) {
        return;
    }
    stopRequested.store(false);
    readerDone.store(false);
    nextAttemptMs = 0;
    if (xTaskCreatePinnedToCore(readerMain, "gpsviewer_rx", READER_STACK_SIZE, this, READER_PRIORITY, &reader, tskNO_AFFINITY) != pdPASS) {
        readerDone.store(true);
        reader = nullptr;
    }
}

void GpsSource::poll(AppContext& appContext, uint32_t timeoutMs, Recorder& recorder) {
    if (queue == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(timeoutMs));
        return;
    }
    GnssEvent event;
    if (xQueueReceive(queue, &event, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return;
    }
    const uint32_t now = get_millis();
    int handled = 0;
    do {
        {
            ContextLock lock(appContext);
            appContext.model.onEvent(event, now);
        }
        recorder.write(event, now);
        handled++;
    } while (handled < QUEUE_LENGTH && xQueueReceive(queue, &event, 0) == pdTRUE);
}


void GpsSource::stop() {
    stopRequested.store(true);
    // The reader wakes at least every READER_IDLE_MS
    for (int i = 0; i < 100 && !readerDone.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    reader = nullptr;
    if (locationSubscribed) {
        location_unsubscribe(&location);
        locationSubscribed = false;
    }
    if (queue != nullptr && readerDone.load()) {
        vQueueDelete(queue);
        queue = nullptr;
    }
}

} // namespace gpsviewer
