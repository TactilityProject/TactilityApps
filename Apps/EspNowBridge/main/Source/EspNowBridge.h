#pragma once

#include <TactilityCpp/App.h>

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

class EspNowBridge final : public App {
public:
    EspNowBridge() = default;
    EspNowBridge(const EspNowBridge&) = delete;
    EspNowBridge& operator=(const EspNowBridge&) = delete;

    void onCreate(AppHandle app) override;
    void onDestroy(AppHandle app) override;
    void onShow(AppHandle app, lv_obj_t* parent) override;
    void onHide(AppHandle app) override;
    void onResult(AppHandle app, void* data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;

    // Public so the free-function dispatchToUi() work callbacks in EspNowBridge.cpp (which run
    // outside any member-function's lexical scope, unlike the inline lambdas in performUpdate())
    // can call them.
    void setStatus(const std::string& text);
    void setProgress(int percent);

private:
    AppHandle appHandle_ = nullptr;
    AppLaunchId pickFileLaunchId_ = 0;
    std::string pendingUpdateFilePath_;
    Device* wifiDevice_ = nullptr;

    // Resolved once in onShow() via wifi_get_firmware_ops() - null on a WiFi device with no
    // updatable co-processor (e.g. a native, non-hosted chip). All OTA/version-query calls go
    // through this generic interface, not any esp_hosted-specific API directly.
    const FirmwareOps* firmwareOps_ = nullptr;
    void* firmwareCtx_ = nullptr;

    // Set once in onShow(), false once onHide() tears the widget tree down - checked (via
    // dispatchToUi(), below) before touching any lv_obj_t*, since the OTA worker task and the
    // WiFi-event callback can both outlive a hide/app-switch.
    std::atomic<bool> isShown_{false};

    // Only one EspNowBridge instance is ever live at a time (app loader owns a single instance
    // per running app), so a single static "is this instance still current" pointer, guarded by
    // an atomic, substitutes for the internal app's shared_ptr-based lifetime guard - the OTA
    // worker task and dispatchToUi()'s lv_async_call closures check liveInstance_ == this before
    // touching any member, instead of holding a shared_ptr to keep `this` alive.
    static std::atomic<EspNowBridge*> liveInstance_;

    TaskHandle_t updateTask_ = nullptr;

    // Number of background tasks (updateTaskEntry, waitForTransportTaskEntry) currently running
    // against this instance's members. onDestroy() must wait for this to hit 0 before returning -
    // the app framework frees this instance shortly after onDestroy() returns (see Loader.cpp),
    // so any task still touching `this` past that point is a use-after-free.
    std::atomic<int> outstandingTasks_{0};
    SemaphoreHandle_t taskDoneSemaphore_ = nullptr;

    // Outlives performUpdate() deliberately, so auto-scan stays paused across the async gap
    // between performUpdate() returning and the automatic restart - see performUpdate().
    std::optional<AutoScanPauseGuard> heldAutoScanPauseGuard_;

    lv_obj_t* currentVersionLabel_ = nullptr;
    lv_obj_t* statusLabel_ = nullptr;
    lv_obj_t* progressBar_ = nullptr;
    lv_obj_t* updateButton_ = nullptr;
    lv_obj_t* updateBundledButton_ = nullptr;
    lv_obj_t* enableWifiButton_ = nullptr;

    void refreshCurrentVersion();
    bool isWifiRadioOn();
    void refreshWifiPrompt();
    /** Enables/disables both update-trigger buttons together - only one performUpdate() can run
     *  at a time (see updateTask_), regardless of which button started it. */
    void setUpdateButtonsDisabled(bool disabled);
    /** Marshal a UI-touching closure onto the LVGL task. Only ever invoked if liveInstance_ is
     *  still this instance (checked at dispatch time and again right before running, on the LVGL
     *  task) and isShown_ is true (this app's widget tree exists). */
    void dispatchToUi(void (*work)(EspNowBridge&, void*), void* context, void (*freeContext)(void*));
    void performUpdate(const std::string& filePath);
    void startUpdateTask(const std::string& filePath);

    static void updateTaskEntry(void* arg);
    static void onUpdateButtonClicked(lv_event_t* event);
    static void onUpdateBundledButtonClicked(lv_event_t* event);
    static void onEnableWifiButtonClicked(lv_event_t* event);
    static void onWifiEvent(Device* device, void* callbackContext, WifiEvent event);
    static void waitForTransportTaskEntry(void* arg);
};
