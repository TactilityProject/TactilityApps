// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/GnssModel.h"
#include "../core/Settings.h"

#include <app/instance.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>

namespace gpsviewer {

constexpr const char* APP_ID = "tactility.gpsviewer";
constexpr int PATH_LENGTH = 192;

enum class SourceStatus : uint8_t {
    NoReceiver,
    LocationOff,
    ReceiverStarting,
    Receiving,
    ReceiverLost,
};

// Bits for AppContext::requests, set by the UI and handled by the app task
enum Request : uint32_t {
    REQUEST_SAVE_SETTINGS = 1U << 0,
    REQUEST_RESET_SPEED = 1U << 1,
    // Opens Tactility's location settings, where GPS receivers are set up and location is turned on
    REQUEST_OPEN_LOCATION_SETTINGS = 1U << 4,
};

struct AppContext;

// LVGL thread only. Defined in Ui.cpp.
struct UiState;

struct AppContext {
    AppInstanceId appInstanceId = 0;
    char dataDirectory[PATH_LENGTH] = "";

    // Guards everything up to the end of this block, between the app task and the LVGL thread
    SemaphoreHandle_t mutex = nullptr;
    GnssModel model;
    Settings settings;
    SourceStatus status = SourceStatus::NoReceiver;
    // Counts receiver connections. Events from an earlier connection are dropped.
    uint32_t session = 0;
    char receiverName[32] = "";
    bool recording = false;
    char recordingName[40] = "";
    uint32_t recordedLines = 0;

    std::atomic<uint32_t> requests {0};
    // Start/stop presses not handled yet. Each one toggles recording, so two quick presses cancel out.
    std::atomic<uint32_t> recordingToggles {0};

    // LVGL thread only
    UiState* ui = nullptr;
};

class ContextLock {
public:
    explicit ContextLock(AppContext& context) : context(context) { xSemaphoreTake(context.mutex, portMAX_DELAY); }
    ~ContextLock() { xSemaphoreGive(context.mutex); }
    ContextLock(const ContextLock&) = delete;
    ContextLock& operator=(const ContextLock&) = delete;

private:
    AppContext& context;
};

} // namespace gpsviewer
