#pragma once

#include <atomic>
#include <optional>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>

#include <tactility/drivers/wifi.h>

/** RAII guard: pauses WifiService's background auto-connect scan for the guard's lifetime. See
 *  tactility/wifi_auto_scan.h - belt-and-suspenders measure, not sufficient on its own (see the
 *  REBOOT comment in EspNowBridge.cpp). */
class AutoScanPauseGuard {
public:
    AutoScanPauseGuard();
    ~AutoScanPauseGuard();
    AutoScanPauseGuard(const AutoScanPauseGuard&) = delete;
    AutoScanPauseGuard& operator=(const AutoScanPauseGuard&) = delete;
};

struct Context {
    uint32_t appInstanceId;

    uint32_t pickFileLaunchId = 0;
    std::string pendingUpdateFilePath;
    Device* wifiDevice = nullptr;

    // Resolved once per createWidgets() call via wifi_get_firmware_ops() - null on a WiFi
    // device with no updatable co-processor (e.g. a native, non-hosted chip). All OTA/
    // version-query calls go through this generic interface, not any esp_hosted-specific
    // API directly.
    const FirmwareOps* firmwareOps = nullptr;
    void* firmwareCtx = nullptr;

    // Set once widgets exist (end of espNowBridgeCreateWidgets()), false again the moment
    // they don't (start of a rebuild, or final teardown) - checked (via dispatchToUi(), below)
    // before touching any lv_obj_t*, since the OTA worker task and the WiFi-event callback can
    // both outlive the window being buried by a modal child (e.g. the file picker) or the app
    // closing entirely.
    std::atomic<bool> isShown{false};

    TaskHandle_t updateTask = nullptr;

    // Number of background tasks (updateTaskEntry, waitForTransportTaskEntry) currently running
    // against this instance's members. espNowBridgeTeardown() must wait for this to hit 0 before
    // returning - main() frees this Context shortly after, so any task still touching it past
    // that point is a use-after-free.
    std::atomic<int> outstandingTasks{0};
    SemaphoreHandle_t taskDoneSemaphore = nullptr;

    // Outlives performUpdate() deliberately, so auto-scan stays paused across the async gap
    // between performUpdate() returning and the automatic restart - see performUpdate().
    std::optional<AutoScanPauseGuard> heldAutoScanPauseGuard;

    lv_obj_t* currentVersionLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* progressBar = nullptr;
    lv_obj_t* updateButton = nullptr;
    lv_obj_t* updateBundledButton = nullptr;
    lv_obj_t* enableWifiButton = nullptr;
};

/** Sets up state that must exist for the whole app instance lifetime, regardless of how many
 *  times the window is (re)built. Call once, right after constructing the Context. */
void espNowBridgeInit(Context* ctx);

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void espNowBridgeCreateWidgets(lv_obj_t* parent, void* userData);

/** Starts the update task if ctx->pendingUpdateFilePath is set (consuming it). Called both from
 *  within espNowBridgeCreateWidgets() and directly by main()'s event loop right after a picked
 *  path arrives - the file-selection child's window teardown (and so this window's rebuild)
 *  happens before its APP_EVENT_RESULT is delivered here, so by the time the result arrives
 *  espNowBridgeCreateWidgets() has typically already run and found the path not yet set. */
void espNowBridgeApplyPendingUpdate(Context* ctx);

/** Waits for any outstanding background task to finish, then releases resources. Call once,
 *  after the window has been torn down and right before the Context itself is freed. */
void espNowBridgeTeardown(Context* ctx);
