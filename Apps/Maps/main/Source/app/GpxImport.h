// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../tiles/TilePaths.h"
#include "../ui/GpxLayer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace maps {

/**
 * Reads one GPX file at a time in the background into a GpxLayer: from its .gpc cache next to it when that is
 * current, otherwise by parsing the GPX in 8 KB chunks through the I/O task and writing a new cache. The map polls
 * for the result and takes the layer.
 */
class GpxImport {
public:
    enum class State : uint8_t {
        Idle,
        Busy,
        Done,
        Failed
    };

    ~GpxImport();
    /** Starts reading path; false while another file is still being read. */
    bool start(IoTask& io, const char* path);
    State state() const { return currentState.load(); }
    /** After Done: the layer, now owned by the caller; after Failed: why. Both reset the state to Idle. */
    GpxLayer* takeLayer();
    const char* error() const { return message; }
    void acknowledge();
    const char* path() const { return filePath; }

private:
    static void taskMain(void* context);
    void run();
    void finish();

    IoTask* io = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<State> currentState = State::Idle;
    char filePath[PATH_MAX_LENGTH] = "";
    char message[64] = "";
    GpxLayer* layer = nullptr;
};

} // namespace maps
