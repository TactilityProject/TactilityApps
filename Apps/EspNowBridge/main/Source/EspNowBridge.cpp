#include "EspNowBridge.h"

#include <app/paths.h>
#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/wifi_auto_scan.h>
#include <tactility/firmware/firmware.h>

#include <tt_app_fileselection.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_system.h>

#include <tactility/log.h>

constexpr TickType_t LVGL_DEFAULT_LOCK_TIME = 500; // 500 ticks = 500 ms

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

static constexpr auto* TAG = "EspNowBridge";
static constexpr size_t CHUNK_SIZE = 1500;
static constexpr uint32_t TRANSPORT_WAIT_TIMEOUT_MS = 5000;
static constexpr uint32_t UPDATE_TASK_STACK_SIZE = 8192;

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "one.tactility.espnowbridge";

AutoScanPauseGuard::AutoScanPauseGuard() { wifi_auto_scan_set_paused(true); }
AutoScanPauseGuard::~AutoScanPauseGuard() { wifi_auto_scan_set_paused(false); }

// Binary partition table format (gen_esp32part.py STRUCT_FORMAT '<2sBBLL16sL'): a flat array of
// 32-byte little-endian records starting at flash offset PARTITION_TABLE_OFFSET, terminated by
// an all-0xFF entry or an MD5-checksum record (magic 0xEBEB). Not exposed as a C header by
// ESP-IDF (only the Python generator knows the format) - this is a hand-ported minimal reader,
// just enough to locate the app partition inside a merged/factory bin.
static constexpr size_t PARTITION_TABLE_OFFSET = 0x8000;
static constexpr size_t PARTITION_TABLE_MAX_ENTRIES = 128; // covers the largest partition table IDF supports (0x1000 / 32)
static constexpr uint16_t PARTITION_ENTRY_MAGIC = 0x50AA; // little-endian bytes 0xAA, 0x50
static constexpr uint16_t PARTITION_MD5_MAGIC = 0xEBEB;
static constexpr uint8_t PARTITION_TYPE_APP = 0x00;
static constexpr uint8_t PARTITION_SUBTYPE_FACTORY = 0x00;
static constexpr uint8_t PARTITION_SUBTYPE_OTA_0 = 0x10;

struct __attribute__((packed)) PartitionEntry {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    char name[16];
    uint32_t flags;
};
static_assert(sizeof(PartitionEntry) == 32, "partition table entry must be 32 bytes");

/**
 * Scans the partition table embedded in a merged/factory bin (at PARTITION_TABLE_OFFSET) for
 * the app partition to flash: prefers "factory" if present, otherwise the first OTA slot
 * (ota_0) - matches what a real M5Stack ESP-Hosted factory image contains.
 * @return true if an app partition was found, with appOffset/appSize set to its location
 * within the file (these are the same as the absolute flash offsets the merged bin preserves).
 */
static bool findAppPartitionInMergedBin(FILE* file, size_t& appOffset, size_t& appSize) {
    if (fseek(file, static_cast<long>(PARTITION_TABLE_OFFSET), SEEK_SET) != 0) {
        return false;
    }

    bool foundFactory = false;
    bool foundOta0 = false;
    size_t factoryOffset = 0, factorySize = 0;
    size_t ota0Offset = 0, ota0Size = 0;

    for (size_t i = 0; i < PARTITION_TABLE_MAX_ENTRIES; i++) {
        PartitionEntry entry;
        if (fread(&entry, 1, sizeof(entry), file) != sizeof(entry)) {
            break;
        }
        if (entry.magic == PARTITION_MD5_MAGIC) {
            break;
        }
        if (entry.magic != PARTITION_ENTRY_MAGIC) {
            break;
        }
        if (entry.type == PARTITION_TYPE_APP) {
            if (entry.subtype == PARTITION_SUBTYPE_FACTORY) {
                foundFactory = true;
                factoryOffset = entry.offset;
                factorySize = entry.size;
            } else if (entry.subtype == PARTITION_SUBTYPE_OTA_0 && !foundOta0) {
                foundOta0 = true;
                ota0Offset = entry.offset;
                ota0Size = entry.size;
            }
        }
    }

    if (foundFactory) {
        appOffset = factoryOffset;
        appSize = factorySize;
        return true;
    }
    if (foundOta0) {
        appOffset = ota0Offset;
        appSize = ota0Size;
        return true;
    }
    return false;
}

/**
 * Validates the app image at the given file offset and extracts its version string. The actual
 * transfer size used for the OTA loop is just the real remaining file size from appOffset (see
 * performUpdate) - hand-computing the image's "logical" size from segment headers + checksum/
 * hash padding drifts a bit short of the real length, so we just use the file size instead.
 */
static bool parseImageHeader(FILE* file, size_t appOffset, char* versionOut, size_t versionOutLen, std::string* errorOut = nullptr) {
    esp_image_header_t imageHeader;
    if (fseek(file, static_cast<long>(appOffset), SEEK_SET) != 0 ||
        fread(&imageHeader, 1, sizeof(imageHeader), file) != sizeof(imageHeader)) {
        if (errorOut != nullptr) {
            *errorOut = "Failed to read image header";
        }
        return false;
    }

    if (imageHeader.magic != ESP_IMAGE_HEADER_MAGIC) {
        if (errorOut != nullptr) {
            *errorOut = "Selected file is not a valid firmware image (bad magic)";
        }
        return false;
    }

    // Fail fast on a wrong-chip image (e.g. an ESP32 or S3 binary picked by mistake) before
    // streaming the whole file over the paced, slow bridge link - esp_hosted_slave_ota_end()
    // would eventually catch this too, but only after the entire transfer already completed.
    if (imageHeader.chip_id != ESP_CHIP_ID_ESP32C6) {
        if (errorOut != nullptr) {
            char buf[96];
            snprintf(buf, sizeof(buf), "Wrong chip: image targets chip id %u, expected ESP32-C6",
                (unsigned)imageHeader.chip_id);
            *errorOut = buf;
        }
        return false;
    }

    esp_image_segment_header_t segmentHeader;
    size_t firstSegmentOffset = appOffset + sizeof(imageHeader);
    if (fseek(file, static_cast<long>(firstSegmentOffset), SEEK_SET) != 0 ||
        fread(&segmentHeader, 1, sizeof(segmentHeader), file) != sizeof(segmentHeader)) {
        if (errorOut != nullptr) {
            *errorOut = "Failed to read first segment header";
        }
        return false;
    }

    esp_app_desc_t appDesc;
    size_t appDescOffset = appOffset + sizeof(imageHeader) + sizeof(segmentHeader);
    if (fseek(file, static_cast<long>(appDescOffset), SEEK_SET) == 0 && fread(&appDesc, 1, sizeof(appDesc), file) == sizeof(appDesc)) {
        strncpy(versionOut, appDesc.version, versionOutLen - 1);
        versionOut[versionOutLen - 1] = '\0';
    } else {
        strncpy(versionOut, "unknown", versionOutLen - 1);
        versionOut[versionOutLen - 1] = '\0';
    }

    return true;
}

static bool getCurrentVersionString(const FirmwareOps* ops, void* ctx, char* versionOut, size_t versionOutLen) {
    FirmwareInfo info = {};
    if (ops == nullptr || ops->get_info(ctx, &info) != ERROR_NONE) {
        return false;
    }

    if (info.name[0] != '\0') {
        snprintf(versionOut, versionOutLen, "%u.%u.%u (%s)",
            (unsigned)info.fw_major, (unsigned)info.fw_minor, (unsigned)info.fw_patch, info.name);
    } else {
        snprintf(versionOut, versionOutLen, "%u.%u.%u",
            (unsigned)info.fw_major, (unsigned)info.fw_minor, (unsigned)info.fw_patch);
    }
    return true;
}

/** Only slave firmware >= v2.6.0 implements esp_hosted_slave_ota_activate() - older slaves
 *  reject/lack the RPC entirely. Matches upstream's host_performs_slave_ota example. */
static bool activateSupported(uint32_t major, uint32_t minor) {
    return (major > 2) || (major == 2 && minor > 5);
}

// Only one EspNowBridge instance is ever live at a time (app-module owns a single instance per
// running app), so a single static "is this instance still current" pointer, guarded by an
// atomic, lets the OTA worker task and dispatchToUi()'s lv_async_call closures check
// liveInstance == ctx before touching any member, instead of needing a shared-ownership lifetime
// guard.
static std::atomic<Context*> liveInstance{nullptr};

static void refreshCurrentVersion(Context* ctx) {
    char versionStr[32];
    if (getCurrentVersionString(ctx->firmwareOps, ctx->firmwareCtx, versionStr, sizeof(versionStr))) {
        lv_label_set_text_fmt(ctx->currentVersionLabel, "Co-processor firmware: %s", versionStr);
    } else {
        lv_label_set_text(ctx->currentVersionLabel, "Co-processor firmware: unknown (link not up)");
    }
}

static bool isWifiRadioOn(Context* ctx) {
    if (ctx->wifiDevice == nullptr) {
        return false;
    }
    WifiRadioState radioState = WIFI_RADIO_STATE_OFF;
    if (wifi_get_radio_state(ctx->wifiDevice, &radioState) != ERROR_NONE) {
        return false;
    }
    // ON with any station state (disconnected/pending/connected) is fine - the ESP-NOW bridge
    // just needs the radio + esp_hosted transport up, not a completed AP connection.
    return radioState == WIFI_RADIO_STATE_ON;
}

static void setUpdateButtonsDisabled(Context* ctx, bool disabled) {
    if (disabled) {
        lv_obj_add_state(ctx->updateButton, LV_STATE_DISABLED);
        lv_obj_add_state(ctx->updateBundledButton, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ctx->updateButton, LV_STATE_DISABLED);
        lv_obj_clear_state(ctx->updateBundledButton, LV_STATE_DISABLED);
    }
}

static void refreshWifiPrompt(Context* ctx) {
    if (isWifiRadioOn(ctx)) {
        lv_obj_add_flag(ctx->enableWifiButton, LV_OBJ_FLAG_HIDDEN);
        setUpdateButtonsDisabled(ctx, false);
    } else {
        lv_obj_clear_flag(ctx->enableWifiButton, LV_OBJ_FLAG_HIDDEN);
        setUpdateButtonsDisabled(ctx, true);
    }
}

static void setStatus(Context* ctx, const std::string& text) {
    lv_label_set_text(ctx->statusLabel, text.c_str());
}

static void setProgress(Context* ctx, int percent) {
    lv_bar_set_value(ctx->progressBar, percent, LV_ANIM_OFF);
}

namespace {
struct UiDispatchPayload {
    Context* instance;
    void (*work)(Context&, void*);
    void* context;
    void (*freeContext)(void*);
};
}

/** Marshal a UI-touching closure onto the LVGL task. Only ever invoked if liveInstance is still
 *  @a ctx (checked at dispatch time and again right before running, on the LVGL task) and
 *  ctx->isShown is true (this app's widget tree exists). */
static void dispatchToUi(Context* ctx, void (*work)(Context&, void*), void* context, void (*freeContext)(void*)) {
    auto* payload = new UiDispatchPayload{ctx, work, context, freeContext};
    // lv_async_call() itself is an LVGL operation and must be lock-guarded when called from a
    // non-LVGL task (see lvgl_lock()'s doc comment) - the OTA worker task calls dispatchToUi()
    // repeatedly during the transfer, and without this lock most of those calls were silently
    // racing LVGL's own task and getting lost (only the very last status update, right before
    // esp_restart(), happened to land - everything else stayed stuck at "Waiting for
    // co-processor link...").
    bool locked = lvgl_try_lock(LVGL_DEFAULT_LOCK_TIME);
    if (!locked) {
        // Without the lock, lv_async_call() itself would be touching LVGL's internal timer list
        // unguarded - and if it happened to still enqueue successfully, the callback below would
        // later fire against `payload` after we've already freed it here. Drop the update instead.
        if (freeContext != nullptr) {
            freeContext(context);
        }
        delete payload;
        return;
    }

    lv_result_t result = lv_async_call([](void* userData) {
        auto* payload = static_cast<UiDispatchPayload*>(userData);
        if (liveInstance.load() == payload->instance && payload->instance->isShown.load()) {
            payload->work(*payload->instance, payload->context);
        }
        if (payload->freeContext != nullptr) {
            payload->freeContext(payload->context);
        }
        delete payload;
    }, payload);
    lvgl_unlock();

    if (result != LV_RESULT_OK) {
        if (freeContext != nullptr) {
            freeContext(context);
        }
        delete payload;
    }
}

namespace {

void workSetStatus(Context& app, void* context) {
    setStatus(&app, *static_cast<std::string*>(context));
}
void freeString(void* context) { delete static_cast<std::string*>(context); }

void workSetProgress(Context& app, void* context) {
    setProgress(&app, *static_cast<int*>(context));
}
void freeInt(void* context) { delete static_cast<int*>(context); }

} // namespace

static void performUpdate(Context* ctx, const std::string& filePath) {
    dispatchToUi(ctx, [](Context& app, void*) {
        setUpdateButtonsDisabled(&app, true);
        setProgress(&app, 0);
        setStatus(&app, "Waiting for co-processor link...");
    }, nullptr, nullptr);

    if (ctx->firmwareOps == nullptr) {
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "This WiFi device has no updatable co-processor");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    if (!ctx->firmwareOps->wait_ready(ctx->firmwareCtx, TRANSPORT_WAIT_TIMEOUT_MS)) {
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Co-processor link not available - update cancelled");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    FILE* file = fopen(filePath.c_str(), "rb");
    if (file == nullptr) {
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Failed to open selected file");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    fseek(file, 0, SEEK_END);
    long fileSizeSigned = ftell(file);
    if (fileSizeSigned <= 0) {
        fclose(file);
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Failed to determine file size");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }
    size_t fileSize = static_cast<size_t>(fileSizeSigned);

    // Support both a plain app image (starting with the app image header at offset 0) and a
    // merged/factory bin (e.g. M5Stack's official ESP-Hosted factory image) - detected by whether
    // a valid partition table is found at PARTITION_TABLE_OFFSET.
    size_t appOffset = 0;
    size_t partitionSize = 0;
    bool isMergedBin = findAppPartitionInMergedBin(file, appOffset, partitionSize);
    if (isMergedBin && appOffset >= fileSize) {
        fclose(file);
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Merged bin's app partition is outside the file - selected file looks truncated");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    char newVersion[32];
    std::string parseError;
    if (!parseImageHeader(file, appOffset, newVersion, sizeof(newVersion), &parseError)) {
        fclose(file);
        dispatchToUi(ctx, workSetStatus, new std::string(parseError), freeString);
        dispatchToUi(ctx, [](Context& app, void*) {
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    // Merged bins pad the app partition to its declared size; a plain app image is exactly as
    // long as the app itself. Transfer whichever is smaller.
    size_t remainingInFile = fileSize - appOffset;
    size_t firmwareSize = isMergedBin ? std::min(partitionSize, remainingInFile) : remainingInFile;

    std::string versionStr(newVersion);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Pushing firmware %s...", versionStr.c_str());
        dispatchToUi(ctx, workSetStatus, new std::string(buf), freeString);
    }

    // Held on the Context (not a local variable) so it outlives this function - see
    // Context::heldAutoScanPauseGuard's declaration for why. Released when the host actually
    // restarts (moot, since esp_restart() doesn't return) or if the update fails early below.
    ctx->heldAutoScanPauseGuard.emplace();

    FirmwareUpdateRequest updateRequest = {};
    updateRequest.image_size = firmwareSize;
    FirmwareUpdateHandle* handle = nullptr;
    if (ctx->firmwareOps->begin(ctx->firmwareCtx, &updateRequest, &handle) != ERROR_NONE) {
        fclose(file);
        ctx->heldAutoScanPauseGuard.reset();
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Failed to start OTA on co-processor");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    if (fseek(file, static_cast<long>(appOffset), SEEK_SET) != 0) {
        fclose(file);
        ctx->firmwareOps->abort(handle);
        ctx->heldAutoScanPauseGuard.reset();
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Failed to seek to firmware start");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    uint8_t chunk[CHUNK_SIZE];
    size_t sent = 0;
    bool writeFailed = false;
    int lastReportedPercent = -1;

    while (sent < firmwareSize) {
        size_t toRead = (firmwareSize - sent > CHUNK_SIZE) ? CHUNK_SIZE : (firmwareSize - sent);
        size_t actuallyRead = fread(chunk, 1, toRead, file);
        if (actuallyRead != toRead) {
            LOG_E(TAG, "Failed to read file at offset %zu", sent);
            writeFailed = true;
            break;
        }

        if (ctx->firmwareOps->write(handle, chunk, actuallyRead) != ERROR_NONE) {
            LOG_E(TAG, "firmwareOps->write() failed at offset %zu", sent);
            writeFailed = true;
            break;
        }

        // Pace the transfer - esp_hosted's SDIO driver only retries a write twice with no
        // backoff before giving up and restarting the host. Back-to-back chunk writes with zero
        // gap were observed to saturate the bus enough to trigger a genuine SDIO timeout
        // mid-transfer, not just around the post-activate reboot.
        vTaskDelay(pdMS_TO_TICKS(5));

        sent += actuallyRead;

        // Only touch LVGL every couple of percent, not every 1500-byte chunk - frequent
        // display-bus activity during the transfer was implicated in SDIO transport crashes
        // under sustained OTA write load.
        int percent = (int)((sent * 100) / firmwareSize);
        if (percent != lastReportedPercent) {
            dispatchToUi(ctx, workSetProgress, new int(percent), freeInt);
            lastReportedPercent = percent;
        }
    }

    fclose(file);

    if (writeFailed) {
        ctx->firmwareOps->abort(handle);
        ctx->heldAutoScanPauseGuard.reset();
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Update failed while transferring firmware");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    if (ctx->firmwareOps->finish(handle) != ERROR_NONE) {
        ctx->heldAutoScanPauseGuard.reset();
        dispatchToUi(ctx, [](Context& app, void*) {
            setStatus(&app, "Failed to finalize OTA on co-processor");
            setUpdateButtonsDisabled(&app, false);
        }, nullptr, nullptr);
        return;
    }

    // Check the *currently running* (pre-update) slave version - the new image isn't running
    // yet - and skip straight to the required host restart for older slaves.
    FirmwareInfo runningInfo = {};
    bool canActivate = ctx->firmwareOps->get_info(ctx->firmwareCtx, &runningInfo) == ERROR_NONE
        && activateSupported(runningInfo.fw_major, runningInfo.fw_minor);

    if (canActivate) {
        if (ctx->firmwareOps->activate(ctx->firmwareCtx) != ERROR_NONE) {
            ctx->heldAutoScanPauseGuard.reset();
            dispatchToUi(ctx, [](Context& app, void*) {
                setStatus(&app, "Failed to activate new firmware - co-processor still running old firmware");
                setUpdateButtonsDisabled(&app, false);
            }, nullptr, nullptr);
            return;
        }
    }

    // heldAutoScanPauseGuard is deliberately left held (never explicitly released) - the host
    // restarts itself immediately below, and there's no safe window to resume normal WiFi
    // activity before that.
    {
        char buf[80];
        if (canActivate) {
            snprintf(buf, sizeof(buf), "Firmware %s activated - restarting...", versionStr.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Firmware %s pushed - restarting to apply...", versionStr.c_str());
        }
        dispatchToUi(ctx, workSetStatus, new std::string(buf), freeString);
    }

    // Give the status message above a moment to actually be seen before the restart cuts the
    // display, then restart.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void updateTaskEntry(void* arg) {
    auto* ctx = static_cast<Context*>(arg);
    performUpdate(ctx, ctx->pendingUpdateFilePath);
    ctx->updateTask = nullptr;
    if (ctx->outstandingTasks.fetch_sub(1) == 1 && ctx->taskDoneSemaphore != nullptr) {
        xSemaphoreGive(ctx->taskDoneSemaphore);
    }
    vTaskDelete(nullptr);
}

static void startUpdateTask(Context* ctx, const std::string& filePath) {
    if (ctx->updateTask != nullptr) {
        return;
    }
    ctx->pendingUpdateFilePath = filePath;
    ctx->outstandingTasks.fetch_add(1);
    if (xTaskCreate(updateTaskEntry, "espnow_bridge_ota", UPDATE_TASK_STACK_SIZE / sizeof(StackType_t), ctx, tskIDLE_PRIORITY + 1, &ctx->updateTask) != pdPASS) {
        ctx->outstandingTasks.fetch_sub(1);
    }
}

static void onUpdateButtonClicked(lv_event_t* /*event*/) {
    auto* ctx = liveInstance.load();
    if (ctx == nullptr || !isWifiRadioOn(ctx)) {
        return;
    }
    ctx->pickFileLaunchId = tt_app_fileselection_start_for_existing_file(ctx->appInstanceId);
}

// Name of the slave bridge firmware bundled in this app's assets/ folder
// lets users flash the known-good bridge firmware without needing to source/copy a
// .bin onto the SD card themselves. The SD-card picker (onUpdateButtonClicked above) stays
// available too, for factory-image downgrades or custom builds.
static constexpr auto* BUNDLED_FIRMWARE_ASSET_NAME = "espnow_bridge_slave_c6.bin";

static void onUpdateBundledButtonClicked(lv_event_t* /*event*/) {
    auto* ctx = liveInstance.load();
    if (ctx == nullptr || !isWifiRadioOn(ctx)) {
        return;
    }
    char assetPath[256] = {};
    if (app_paths_get_assets_path(APP_ID, BUNDLED_FIRMWARE_ASSET_NAME, assetPath, sizeof(assetPath)) != ERROR_NONE) {
        LOG_E(TAG, "Failed to resolve bundled firmware asset path");
        return;
    }
    startUpdateTask(ctx, assetPath);
}

static void waitForTransportTaskEntry(void* arg) {
    auto* ctx = static_cast<Context*>(arg);
    constexpr uint32_t WAIT_TIMEOUT_MS = 10000;
    // liveInstance must be checked before touching any member of ctx - if espNowBridgeTeardown()
    // already ran, `ctx` may be freed, and dereferencing ctx->firmwareOps first would be a
    // use-after-free even just to read the pointer.
    if (liveInstance.load() == ctx && ctx->firmwareOps != nullptr
            && ctx->firmwareOps->wait_ready(ctx->firmwareCtx, WAIT_TIMEOUT_MS)
            && liveInstance.load() == ctx) {
        dispatchToUi(ctx, [](Context& app, void*) {
            refreshCurrentVersion(&app);
        }, nullptr, nullptr);
    }
    if (ctx->outstandingTasks.fetch_sub(1) == 1 && ctx->taskDoneSemaphore != nullptr) {
        xSemaphoreGive(ctx->taskDoneSemaphore);
    }
    vTaskDelete(nullptr);
}

static void onWifiEvent(Device* /*device*/, void* callbackContext, WifiEvent /*event*/) {
    auto* ctx = static_cast<Context*>(callbackContext);
    if (liveInstance.load() != ctx) {
        return;
    }
    dispatchToUi(ctx, [](Context& app, void*) {
        refreshWifiPrompt(&app);
        refreshCurrentVersion(&app);
    }, nullptr, nullptr);
}

static void onEnableWifiButtonClicked(lv_event_t* /*event*/) {
    auto* ctx = liveInstance.load();
    if (ctx == nullptr || ctx->wifiDevice == nullptr) {
        return;
    }
    device_start(ctx->wifiDevice);
    // start_device() allocates a fresh driver context (Platforms/platform-esp32's
    // esp32_wifi.cpp), which wipes any event callback registered before the device was started -
    // re-register now that it's actually running. Also refresh once directly rather than relying
    // solely on the next WifiEvent, so the "WiFi on" prompt updates immediately even though the
    // co-processor firmware version below isn't available yet.
    wifi_add_event_callback(ctx->wifiDevice, ctx, onWifiEvent);
    refreshWifiPrompt(ctx);
    refreshCurrentVersion(ctx);

    // The co-processor RPC transport isn't up the instant device_start() returns - it comes up
    // asynchronously (~1-2s later) - so firmwareOps->get_info() above reliably fails right after
    // enabling WiFi. Nothing else reliably re-triggers a version refresh once the transport
    // actually comes up (the WiFi event callback only covers radio/station state, not transport
    // readiness), so wait for it explicitly on a background task and refresh once it's ready.
    if (ctx->firmwareOps != nullptr) {
        ctx->outstandingTasks.fetch_add(1);
        if (xTaskCreate(waitForTransportTaskEntry, "espnow_bridge_wait", 4096 / sizeof(StackType_t), ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
            ctx->outstandingTasks.fetch_sub(1);
        }
    }
}

void espNowBridgeInit(Context* ctx) {
    ctx->taskDoneSemaphore = xSemaphoreCreateBinary();
    liveInstance = ctx;
}

void espNowBridgeCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    // Tear down whatever the previous build wired up. The first call has nothing to tear down
    // (wifiDevice is null); a rebuild - after e.g. the file picker (a modal child) closes and
    // this window resurfaces - does, since there's no separate "window buried" callback in this
    // app framework to have done it already (unlike the old one's onHide()).
    ctx->isShown = false;
    if (ctx->wifiDevice != nullptr) {
        wifi_remove_event_callback(ctx->wifiDevice, onWifiEvent);
        ctx->wifiDevice = nullptr;
    }

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "ESP-NOW Bridge");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrapper, 8, LV_STATE_DEFAULT);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(wrapper, 1);

    ctx->currentVersionLabel = lv_label_create(wrapper);
    lv_obj_set_style_pad_bottom(ctx->currentVersionLabel, 12, LV_STATE_DEFAULT);

    ctx->enableWifiButton = lv_button_create(wrapper);
    lv_obj_add_event_cb(ctx->enableWifiButton, onEnableWifiButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* enableWifiButtonLabel = lv_label_create(ctx->enableWifiButton);
    lv_label_set_text(enableWifiButtonLabel, "Enable WiFi (required for co-processor link)");
    lv_obj_set_style_pad_bottom(ctx->enableWifiButton, 12, LV_STATE_DEFAULT);

    ctx->updateBundledButton = lv_button_create(wrapper);
    lv_obj_add_event_cb(ctx->updateBundledButton, onUpdateBundledButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* updateBundledButtonLabel = lv_label_create(ctx->updateBundledButton);
    lv_label_set_text(updateBundledButtonLabel, "Update to bundled firmware");
    lv_obj_set_style_pad_bottom(ctx->updateBundledButton, 12, LV_STATE_DEFAULT);

    ctx->updateButton = lv_button_create(wrapper);
    lv_obj_add_event_cb(ctx->updateButton, onUpdateButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* updateButtonLabel = lv_label_create(ctx->updateButton);
    lv_label_set_text(updateButtonLabel, "Update from SD card...");
    lv_obj_set_style_pad_bottom(ctx->updateButton, 12, LV_STATE_DEFAULT);

    ctx->progressBar = lv_bar_create(wrapper);
    lv_obj_set_size(ctx->progressBar, LV_PCT(100), LV_PCT(6));
    lv_bar_set_range(ctx->progressBar, 0, 100);
    lv_bar_set_value(ctx->progressBar, 0, LV_ANIM_OFF);

    ctx->statusLabel = lv_label_create(wrapper);
    lv_label_set_text(ctx->statusLabel, "Ready");

    ctx->wifiDevice = wifi_find_first_registered_device();
    if (ctx->wifiDevice != nullptr) {
        wifi_add_event_callback(ctx->wifiDevice, ctx, onWifiEvent);
        if (wifi_get_firmware_ops(ctx->wifiDevice, &ctx->firmwareOps, &ctx->firmwareCtx) != ERROR_NONE) {
            ctx->firmwareOps = nullptr;
            ctx->firmwareCtx = nullptr;
        }
    }

    refreshCurrentVersion(ctx);
    refreshWifiPrompt(ctx);

    ctx->isShown = true;

    // If an SD-card file was picked before this ran, perform the update now that widgets are
    // valid again. In practice this rarely fires - see espNowBridgeApplyPendingUpdate()'s doc
    // comment - but it's a harmless no-op otherwise and stays as a defensive fallback.
    espNowBridgeApplyPendingUpdate(ctx);
}

void espNowBridgeApplyPendingUpdate(Context* ctx) {
    if (ctx->pendingUpdateFilePath.empty()) {
        return;
    }
    std::string path = std::move(ctx->pendingUpdateFilePath);
    ctx->pendingUpdateFilePath.clear();
    startUpdateTask(ctx, path);
}

void espNowBridgeTeardown(Context* ctx) {
    ctx->isShown = false;
    if (ctx->wifiDevice != nullptr) {
        wifi_remove_event_callback(ctx->wifiDevice, onWifiEvent);
        ctx->wifiDevice = nullptr;
    }

    // Clear liveInstance first so any task still running bails out at its next liveInstance
    // check instead of continuing to touch this instance's members.
    liveInstance = nullptr;

    // Wait for any outstanding background task (OTA update, transport-wait) to actually finish -
    // main() frees this Context shortly after this function returns, so a task that outlives it
    // would dereference freed memory.
    while (ctx->outstandingTasks.load() > 0) {
        if (ctx->taskDoneSemaphore != nullptr) {
            xSemaphoreTake(ctx->taskDoneSemaphore, pdMS_TO_TICKS(1000));
        }
    }

    if (ctx->taskDoneSemaphore != nullptr) {
        vSemaphoreDelete(ctx->taskDoneSemaphore);
        ctx->taskDoneSemaphore = nullptr;
    }
}
