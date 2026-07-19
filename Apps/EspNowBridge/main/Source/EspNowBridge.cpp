#include "EspNowBridge.h"

#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/wifi_auto_scan.h>
#include <tactility/firmware/firmware.h>

#include <tt_app.h>
#include <tt_app_fileselection.h>
#include <tt_bundle.h>
#include <tt_lock.h>
#include <tt_lvgl.h>
#include <tt_lvgl_toolbar.h>

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_system.h>

#include <tactility/log.h>

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

std::atomic<EspNowBridge*> EspNowBridge::liveInstance_{nullptr};

void EspNowBridge::onCreate(AppHandle app) {
    appHandle_ = app;
    liveInstance_ = this;
}

void EspNowBridge::onDestroy(AppHandle /*app*/) {
    liveInstance_ = nullptr;
}

void EspNowBridge::refreshCurrentVersion() {
    char versionStr[32];
    if (getCurrentVersionString(firmwareOps_, firmwareCtx_, versionStr, sizeof(versionStr))) {
        lv_label_set_text_fmt(currentVersionLabel_, "Co-processor firmware: %s", versionStr);
    } else {
        lv_label_set_text(currentVersionLabel_, "Co-processor firmware: unknown (link not up)");
    }
}

bool EspNowBridge::isWifiRadioOn() {
    if (wifiDevice_ == nullptr) {
        return false;
    }
    WifiRadioState radioState = WIFI_RADIO_STATE_OFF;
    if (wifi_get_radio_state(wifiDevice_, &radioState) != ERROR_NONE) {
        return false;
    }
    // ON with any station state (disconnected/pending/connected) is fine - the ESP-NOW bridge
    // just needs the radio + esp_hosted transport up, not a completed AP connection.
    return radioState == WIFI_RADIO_STATE_ON;
}

void EspNowBridge::refreshWifiPrompt() {
    if (isWifiRadioOn()) {
        lv_obj_add_flag(enableWifiButton_, LV_OBJ_FLAG_HIDDEN);
        setUpdateButtonsDisabled(false);
    } else {
        lv_obj_clear_flag(enableWifiButton_, LV_OBJ_FLAG_HIDDEN);
        setUpdateButtonsDisabled(true);
    }
}

void EspNowBridge::setUpdateButtonsDisabled(bool disabled) {
    if (disabled) {
        lv_obj_add_state(updateButton_, LV_STATE_DISABLED);
        lv_obj_add_state(updateBundledButton_, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(updateButton_, LV_STATE_DISABLED);
        lv_obj_clear_state(updateBundledButton_, LV_STATE_DISABLED);
    }
}

void EspNowBridge::setStatus(const std::string& text) {
    lv_label_set_text(statusLabel_, text.c_str());
}

void EspNowBridge::setProgress(int percent) {
    lv_bar_set_value(progressBar_, percent, LV_ANIM_OFF);
}

namespace {
struct UiDispatchPayload {
    EspNowBridge* instance;
    void (*work)(EspNowBridge&, void*);
    void* context;
    void (*freeContext)(void*);
};
}

void EspNowBridge::dispatchToUi(void (*work)(EspNowBridge&, void*), void* context, void (*freeContext)(void*)) {
    auto* payload = new UiDispatchPayload{this, work, context, freeContext};
    // lv_async_call() itself is an LVGL operation and must be lock-guarded when called from a
    // non-LVGL task (see tt_lvgl_lock()'s doc comment) - the OTA worker task calls dispatchToUi()
    // repeatedly during the transfer, and without this lock most of those calls were silently
    // racing LVGL's own task and getting lost (only the very last status update, right before
    // esp_restart(), happened to land - everything else stayed stuck at "Waiting for
    // co-processor link...").
    bool locked = tt_lvgl_lock(TT_LVGL_DEFAULT_LOCK_TIME);
    lv_result_t result = lv_async_call([](void* userData) {
        auto* payload = static_cast<UiDispatchPayload*>(userData);
        if (EspNowBridge::liveInstance_.load() == payload->instance && payload->instance->isShown_.load()) {
            payload->work(*payload->instance, payload->context);
        }
        if (payload->freeContext != nullptr) {
            payload->freeContext(payload->context);
        }
        delete payload;
    }, payload);
    if (locked) {
        tt_lvgl_unlock();
    }
    if (!locked || result != LV_RESULT_OK) {
        if (freeContext != nullptr) {
            freeContext(context);
        }
        delete payload;
    }
}

namespace {

void workSetStatus(EspNowBridge& app, void* context) {
    app.setStatus(*static_cast<std::string*>(context));
}
void freeString(void* context) { delete static_cast<std::string*>(context); }

void workSetProgress(EspNowBridge& app, void* context) {
    app.setProgress(*static_cast<int*>(context));
}
void freeInt(void* context) { delete static_cast<int*>(context); }

} // namespace

void EspNowBridge::performUpdate(const std::string& filePath) {
    dispatchToUi([](EspNowBridge& app, void*) {
        app.setUpdateButtonsDisabled(true);
        app.setProgress(0);
        app.setStatus("Waiting for co-processor link...");
    }, nullptr, nullptr);

    if (firmwareOps_ == nullptr) {
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("This WiFi device has no updatable co-processor");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    if (!firmwareOps_->wait_ready(firmwareCtx_, TRANSPORT_WAIT_TIMEOUT_MS)) {
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Co-processor link not available - update cancelled");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    FILE* file = fopen(filePath.c_str(), "rb");
    if (file == nullptr) {
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Failed to open selected file");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    fseek(file, 0, SEEK_END);
    long fileSizeSigned = ftell(file);
    if (fileSizeSigned <= 0) {
        fclose(file);
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Failed to determine file size");
            app.setUpdateButtonsDisabled(false);
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
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Merged bin's app partition is outside the file - selected file looks truncated");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    char newVersion[32];
    std::string parseError;
    if (!parseImageHeader(file, appOffset, newVersion, sizeof(newVersion), &parseError)) {
        fclose(file);
        dispatchToUi(workSetStatus, new std::string(parseError), freeString);
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setUpdateButtonsDisabled(false);
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
        dispatchToUi(workSetStatus, new std::string(buf), freeString);
    }

    // Held on the app instance (not a local variable) so it outlives this function - see
    // heldAutoScanPauseGuard_'s declaration for why. Released when the host actually restarts
    // (moot, since esp_restart() doesn't return) or if the update fails early below.
    heldAutoScanPauseGuard_.emplace();

    FirmwareUpdateRequest updateRequest = {};
    updateRequest.image_size = firmwareSize;
    FirmwareUpdateHandle* handle = nullptr;
    if (firmwareOps_->begin(firmwareCtx_, &updateRequest, &handle) != ERROR_NONE) {
        fclose(file);
        heldAutoScanPauseGuard_.reset();
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Failed to start OTA on co-processor");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    if (fseek(file, static_cast<long>(appOffset), SEEK_SET) != 0) {
        fclose(file);
        firmwareOps_->abort(handle);
        heldAutoScanPauseGuard_.reset();
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Failed to seek to firmware start");
            app.setUpdateButtonsDisabled(false);
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

        if (firmwareOps_->write(handle, chunk, actuallyRead) != ERROR_NONE) {
            LOG_E(TAG, "firmwareOps_->write() failed at offset %zu", sent);
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
            dispatchToUi(workSetProgress, new int(percent), freeInt);
            lastReportedPercent = percent;
        }
    }

    fclose(file);

    if (writeFailed) {
        firmwareOps_->abort(handle);
        heldAutoScanPauseGuard_.reset();
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Update failed while transferring firmware");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    if (firmwareOps_->finish(handle) != ERROR_NONE) {
        heldAutoScanPauseGuard_.reset();
        dispatchToUi([](EspNowBridge& app, void*) {
            app.setStatus("Failed to finalize OTA on co-processor");
            app.setUpdateButtonsDisabled(false);
        }, nullptr, nullptr);
        return;
    }

    // Check the *currently running* (pre-update) slave version - the new image isn't running
    // yet - and skip straight to the required host restart for older slaves.
    FirmwareInfo runningInfo = {};
    bool canActivate = firmwareOps_->get_info(firmwareCtx_, &runningInfo) == ERROR_NONE
        && activateSupported(runningInfo.fw_major, runningInfo.fw_minor);

    if (canActivate) {
        if (firmwareOps_->activate(firmwareCtx_) != ERROR_NONE) {
            heldAutoScanPauseGuard_.reset();
            dispatchToUi([](EspNowBridge& app, void*) {
                app.setStatus("Failed to activate new firmware - co-processor still running old firmware");
                app.setUpdateButtonsDisabled(false);
            }, nullptr, nullptr);
            return;
        }
    }

    // heldAutoScanPauseGuard_ is deliberately left held (never explicitly released) - the host
    // restarts itself immediately below, and there's no safe window to resume normal WiFi
    // activity before that.
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Firmware %s activated - restarting...", versionStr.c_str());
        dispatchToUi(workSetStatus, new std::string(buf), freeString);
    }

    // Give the status message above a moment to actually be seen before the restart cuts the
    // display, then restart.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void EspNowBridge::updateTaskEntry(void* arg) {
    auto* self = static_cast<EspNowBridge*>(arg);
    self->performUpdate(self->pendingUpdateFilePath_);
    self->updateTask_ = nullptr;
    vTaskDelete(nullptr);
}

void EspNowBridge::startUpdateTask(const std::string& filePath) {
    if (updateTask_ != nullptr) {
        return;
    }
    pendingUpdateFilePath_ = filePath;
    xTaskCreate(updateTaskEntry, "espnow_bridge_ota", UPDATE_TASK_STACK_SIZE / sizeof(StackType_t), this, tskIDLE_PRIORITY + 1, &updateTask_);
}

void EspNowBridge::onUpdateButtonClicked(lv_event_t* /*event*/) {
    auto* self = liveInstance_.load();
    if (self == nullptr || !self->isWifiRadioOn()) {
        return;
    }
    self->pickFileLaunchId_ = tt_app_fileselection_start_for_existing_file();
}

// Name of the slave bridge firmware bundled in this app's assets/ folder
// lets users flash the known-good bridge firmware without needing to source/copy a
// .bin onto the SD card themselves. The SD-card picker (onUpdateButtonClicked above) stays
// available too, for factory-image downgrades or custom builds.
static constexpr auto* BUNDLED_FIRMWARE_ASSET_NAME = "espnow_bridge_slave_c6.bin";

void EspNowBridge::onUpdateBundledButtonClicked(lv_event_t* /*event*/) {
    auto* self = liveInstance_.load();
    if (self == nullptr || !self->isWifiRadioOn()) {
        return;
    }
    char assetPath[256] = {};
    size_t assetPathSize = sizeof(assetPath);
    tt_app_get_assets_child_path(self->appHandle_, BUNDLED_FIRMWARE_ASSET_NAME, assetPath, &assetPathSize);
    if (assetPath[0] == '\0') {
        LOG_E(TAG, "Failed to resolve bundled firmware asset path");
        return;
    }
    self->startUpdateTask(assetPath);
}

void EspNowBridge::onEnableWifiButtonClicked(lv_event_t* /*event*/) {
    auto* self = liveInstance_.load();
    if (self == nullptr || self->wifiDevice_ == nullptr) {
        return;
    }
    device_start(self->wifiDevice_);
    // start_device() allocates a fresh driver context (Platforms/platform-esp32's
    // esp32_wifi.cpp), which wipes any event callback registered before the device was started -
    // re-register now that it's actually running. Also refresh once directly rather than relying
    // solely on the next WifiEvent, so the "WiFi on" prompt updates immediately even though the
    // co-processor firmware version below isn't available yet.
    wifi_add_event_callback(self->wifiDevice_, self, onWifiEvent);
    self->refreshWifiPrompt();
    self->refreshCurrentVersion();

    // The co-processor RPC transport isn't up the instant device_start() returns - it comes up
    // asynchronously (~1-2s later) - so firmwareOps_->get_info() above reliably fails right after
    // enabling WiFi. Nothing else reliably re-triggers a version refresh once the transport
    // actually comes up (WifiEvent only covers radio/station state, not transport readiness), so
    // wait for it explicitly on a background task and refresh once it's ready.
    if (self->firmwareOps_ != nullptr) {
        xTaskCreate(waitForTransportTaskEntry, "espnow_bridge_wait", 4096 / sizeof(StackType_t), self, tskIDLE_PRIORITY + 1, nullptr);
    }
}

void EspNowBridge::waitForTransportTaskEntry(void* arg) {
    auto* self = static_cast<EspNowBridge*>(arg);
    constexpr uint32_t WAIT_TIMEOUT_MS = 10000;
    if (self->firmwareOps_ != nullptr && self->firmwareOps_->wait_ready(self->firmwareCtx_, WAIT_TIMEOUT_MS)
            && liveInstance_.load() == self) {
        self->dispatchToUi([](EspNowBridge& app, void*) {
            app.refreshCurrentVersion();
        }, nullptr, nullptr);
    }
    vTaskDelete(nullptr);
}

void EspNowBridge::onWifiEvent(Device* /*device*/, void* callbackContext, WifiEvent /*event*/) {
    auto* self = static_cast<EspNowBridge*>(callbackContext);
    if (liveInstance_.load() != self) {
        return;
    }
    self->dispatchToUi([](EspNowBridge& app, void*) {
        app.refreshWifiPrompt();
        app.refreshCurrentVersion();
    }, nullptr, nullptr);
}

void EspNowBridge::onShow(AppHandle app, lv_obj_t* parent) {
    isShown_ = true;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, app);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrapper, 8, LV_STATE_DEFAULT);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(wrapper, 1);

    currentVersionLabel_ = lv_label_create(wrapper);
    lv_obj_set_style_pad_bottom(currentVersionLabel_, 12, LV_STATE_DEFAULT);

    enableWifiButton_ = lv_button_create(wrapper);
    lv_obj_add_event_cb(enableWifiButton_, onEnableWifiButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* enableWifiButtonLabel = lv_label_create(enableWifiButton_);
    lv_label_set_text(enableWifiButtonLabel, "Enable WiFi (required for co-processor link)");
    lv_obj_set_style_pad_bottom(enableWifiButton_, 12, LV_STATE_DEFAULT);

    updateBundledButton_ = lv_button_create(wrapper);
    lv_obj_add_event_cb(updateBundledButton_, onUpdateBundledButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* updateBundledButtonLabel = lv_label_create(updateBundledButton_);
    lv_label_set_text(updateBundledButtonLabel, "Update to bundled firmware");
    lv_obj_set_style_pad_bottom(updateBundledButton_, 12, LV_STATE_DEFAULT);

    updateButton_ = lv_button_create(wrapper);
    lv_obj_add_event_cb(updateButton_, onUpdateButtonClicked, LV_EVENT_CLICKED, nullptr);
    auto* updateButtonLabel = lv_label_create(updateButton_);
    lv_label_set_text(updateButtonLabel, "Update from SD card...");
    lv_obj_set_style_pad_bottom(updateButton_, 12, LV_STATE_DEFAULT);

    progressBar_ = lv_bar_create(wrapper);
    lv_obj_set_size(progressBar_, LV_PCT(100), LV_PCT(6));
    lv_bar_set_range(progressBar_, 0, 100);
    lv_bar_set_value(progressBar_, 0, LV_ANIM_OFF);

    statusLabel_ = lv_label_create(wrapper);
    lv_label_set_text(statusLabel_, "Ready");

    wifiDevice_ = wifi_find_first_registered_device();
    if (wifiDevice_ != nullptr) {
        wifi_add_event_callback(wifiDevice_, this, onWifiEvent);
        if (wifi_get_firmware_ops(wifiDevice_, &firmwareOps_, &firmwareCtx_) != ERROR_NONE) {
            firmwareOps_ = nullptr;
            firmwareCtx_ = nullptr;
        }
    }

    refreshCurrentVersion();
    refreshWifiPrompt();

    // If an SD-card file was picked before this onShow() ran (FileSelection tears down and
    // rebuilds this app's whole widget tree), perform the update now that widgets are valid
    // again. The bundled-firmware button doesn't go through this path - it calls
    // startUpdateTask() directly since there's no separate app launch/result round trip involved.
    if (!pendingUpdateFilePath_.empty()) {
        std::string path = std::move(pendingUpdateFilePath_);
        pendingUpdateFilePath_.clear();
        startUpdateTask(path);
    }
}

void EspNowBridge::onHide(AppHandle /*app*/) {
    isShown_ = false;
    if (wifiDevice_ != nullptr) {
        wifi_remove_event_callback(wifiDevice_, onWifiEvent);
        wifiDevice_ = nullptr;
    }
}

void EspNowBridge::onResult(AppHandle /*app*/, void* /*data*/, AppLaunchId launchId, AppResult result, BundleHandle resultData) {
    if (launchId != pickFileLaunchId_) {
        return;
    }
    pickFileLaunchId_ = 0;

    if (result == APP_RESULT_OK && resultData != nullptr) {
        char pathBuf[256] = {};
        if (tt_app_fileselection_get_result_path(resultData, pathBuf, sizeof(pathBuf))) {
            pendingUpdateFilePath_ = pathBuf;
        }
    }
}
