// SPDX-License-Identifier: GPL-3.0-or-later
// The debug script's steps (Debug.h): the app driven from a file on the card, for tests on the device.
#include "MapView.h"
#include "MapViewInternal.h"

#include "../platform/Platform.h"
#include "../ui/Metrics.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>

namespace maps {

void MapView::pollDebug(uint32_t now) {
    if (waitingRoute && navigator.state() != Navigator::State::Searching) {
        waitingRoute = false;
    }
    if (waitingSearchIndex && searchIndexJob.state() != SearchIndexJob::State::Busy) {
        waitingSearchIndex = false;
    }
    if (!waitingIdle && !waitingRoute && !waitingSearchIndex) {
        runDebugStep(script.next(now), now);
    } else if (viewTimed && !dirty) {
        waitingIdle = false;
    }
}

void MapView::runDebugStep(const DebugScript::Step& step, uint32_t now) {
    switch (step.action) {
        case DebugScript::Action::Key:
            markInput(now);
            if (step.key == keys::ENTER) {
                handleEnter(true, now);
                handleEnter(false, now);
            } else if (step.key == 'c') {
                // "key c" in a debug script is a trackball click in mouse mode
                click(true);
            } else {
                handleKey(step.key);
            }
            break;
        case DebugScript::Action::Shot:
            render();
            saveShot(step.name);
            break;
        case DebugScript::Action::Goto:
            centerOn(step.lat, step.lon, step.zoom);
            break;
        case DebugScript::Action::WaitIdle:
            waitingIdle = true;
            break;
        case DebugScript::Action::Exit:
            exitRequested = true;
            break;
        case DebugScript::Action::Touch:
            syntheticTouch = step;
            syntheticTouchStart = now;
            syntheticTouchActive = true;
            syntheticTouchEnded = false;
            break;
        case DebugScript::Action::Squeeze:
            squeezeMemory(static_cast<size_t>(step.zoom) * 1024);
            break;
        case DebugScript::Action::RouteBench: {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", rootPath, step.name);
            navigator.bench(path, step.zoom >= 3 ? RouteProfile::Count : static_cast<RouteProfile>(step.zoom));
            break;
        }
        case DebugScript::Action::Navigate:
            navigateTo(step.lat, step.lon, static_cast<RouteProfile>(std::min(step.zoom, 2)), false);
            break;
        case DebugScript::Action::WaitRoute:
            waitingRoute = true;
            break;
        case DebugScript::Action::Replay:
            startReplay(step.name, step.values[0], step.values[1], static_cast<uint32_t>(step.values[2]));
            break;
        case DebugScript::Action::SearchIndex:
            startSearchIndex(step.zoom != 0, step.name[0] != '\0' ? step.name : nullptr);
            break;
        case DebugScript::Action::WaitSearchIndex:
            waitingSearchIndex = true;
            break;
        case DebugScript::Action::FileCheck:
            checkFile(step.name);
            break;
        case DebugScript::Action::Heights:
            downloader.downloadTerrain(step.name);
            break;
        case DebugScript::Action::Trails:
            downloader.downloadTrails(step.name);
            break;
        case DebugScript::Action::OffRoute:
            if (setOffRouteTuning(step.name, step.values[0])) {
                logInfo("Off route: %s = %g", step.name, static_cast<double>(step.values[0]));
            } else {
                logError("Off route: no value named %s", step.name);
            }
            break;
        case DebugScript::Action::Pointer:
            cursorX = std::clamp(static_cast<int>(step.values[0]), 0, width - 1);
            cursorY = std::clamp(static_cast<int>(step.values[1]), 0, height - 1);
            cursorShown = true;
            cursorMovedAt = nowMs();
            if (mainMenuOpen()) {
                menuGrid.hover(cursorX, cursorY);
            }
            dirty = true;
            break;
        case DebugScript::Action::FollowGpx: {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", rootPath, step.name);
            followGpxFile(path);
            break;
        }
        case DebugScript::Action::Restart:
            logInfo("debug: restarting the board");
            saveView();
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
            break;
        case DebugScript::Action::Memory: {
            logMemory(step.name);
            // The tasks that run now, by their handles (the task list of FreeRTOS is not exported to apps)
            struct Named {
                const char* name;
                TaskHandle_t task;
            };
            const Named tasks[] = { { "io", io.taskHandle() }, { "render0", worker.laneTask(0) }, { "render1", worker.laneTask(1) },
                { "download", downloader.controlTaskHandle() }, { "browse", downloader.browseTaskHandle() },
                { "route", navigator.taskHandle() }, { "search", searchTask.taskHandle() }, { "sidx", searchIndexJob.taskHandle() } };
            char line[240];
            int at = snprintf(line, sizeof(line), "Stack left %s: main %u", step.name,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            for (const Named& named : tasks) {
                if (named.task != nullptr && at < static_cast<int>(sizeof(line)) - 24) {
                    at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), ", %s %u", named.name,
                        static_cast<unsigned>(uxTaskGetStackHighWaterMark(named.task)));
                }
            }
            logInfo("%s", line);
            // Into the script log too: the serial line is not always there (silent after a restart, 13 Nov)
            scriptLog("Memory %s: internal %u free (largest %u), PSRAM %u free (largest %u)", step.name,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            scriptLog("%s", line);
            break;
        }
        case DebugScript::Action::TouchOnly:
            input.setTouchOnly(true);
            cursorShown = false;
            controls.setInputs({ input.hasKeyboard(), input.hasTouch(), input.hasPointer() });
            metrics::setKeyboard(input.hasKeyboard());
            if (downloadCard.isOpen()) {
                fillDownloadCard();
            }
            dirty = true;
            break;
        default:
            break;
    }
}

} // namespace maps
