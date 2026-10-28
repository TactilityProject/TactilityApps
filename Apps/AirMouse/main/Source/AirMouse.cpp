#include "AirMouse.h"

#include <tactility/drivers/i2c_controller.h>
#include <tactility/file_ops.h>
#include <tactility/log.h>
#include <tactility/preferences.h>
#include <tactility/concurrent/task_event_group.h>
#include <app/paths.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

namespace {

const char* TAG = "AirMouse";
const char* DEVICE_NAME = "Tactility Air Mouse";

/** Must match manifest.properties' app.id */
constexpr const char* APP_ID = "tactility.airmouse";
constexpr const char* SETTINGS_FILE = "settings.properties";
// Motion settings keys
constexpr const char* PREF_SENSITIVITY = "sens";
constexpr const char* PREF_INVERT = "inv";
constexpr const char* PREF_ORIENTATION = "orient";
constexpr const char* PREF_AUTOCLUTCH = "autoclutch";
constexpr const char* PREF_MOUNT = "mm"; // mm0..mm8

// ── IMU discovery (device tree, or a bare external MPU6886 on any I2C bus) ─────

constexpr TickType_t kExtTimeout = pdMS_TO_TICKS(10);
constexpr float kExtAccelScale = 8.0f / 32768.0f;    // g per LSB (±8g)
constexpr float kExtGyroScale  = 2000.0f / 32768.0f; // °/s per LSB (±2000°/s)

// Default orientation of a bare external MPU6886: a module has no devicetree entry, so there is
// nothing else to tell us how it is mounted. This maps the module's axes onto the device frame
// (+X right, +Y to the top of the screen, +Z out of the screen toward the user).
//
// It assumes the module lies flat against the back of the device - on a device whose screen faces
// up, that is the bottom, so the sticker ends up facing the floor - which puts the module's Z
// through the device, away from the screen (a right-handed frame fixes it: Z = X x Y, and with X to
// the device's left and Y towards the top of the screen, Z can only point that way). Read from
// below, the sticker then shows its Y towards the top of the screen and its X to the device's left,
// which from the front reads as X to the left. A 180 degree turn about Y.
//
// Change this constant if the module is mounted differently, or per device by setting mm0..mm8:
// the settings file overrides this default whenever it has the keys.
constexpr airmouse::MountMatrix kDefaultExtMountMatrix = {{ -1, 0, 0, 0, 1, 0, 0, 0, -1 }};

int16_t extToI16(uint8_t hi, uint8_t lo) {
    return static_cast<int16_t>(static_cast<uint16_t>(hi) << 8 | lo);
}

void initExtMpu6886(Device* i2c, uint8_t addr) {
    // Wake from sleep (clear all PWR_MGMT_1 bits)
    if (i2c_controller_register8_set(i2c, addr, 0x6B, 0x00, kExtTimeout) != ERROR_NONE) return;
    vTaskDelay(pdMS_TO_TICKS(10));

    // Device reset (bit 7)
    if (i2c_controller_register8_set(i2c, addr, 0x6B, 0x80, kExtTimeout) != ERROR_NONE) return;
    vTaskDelay(pdMS_TO_TICKS(10));

    // Select auto clock (CLKSEL=1: PLL with gyro reference when available)
    if (i2c_controller_register8_set(i2c, addr, 0x6B, 0x01, kExtTimeout) != ERROR_NONE) return;
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configure accel: ±8g
    if (i2c_controller_register8_set(i2c, addr, 0x1C, 0x10, kExtTimeout) != ERROR_NONE) return;
    // Configure gyro: ±2000°/s
    if (i2c_controller_register8_set(i2c, addr, 0x1B, 0x18, kExtTimeout) != ERROR_NONE) return;
    // DLPF: gyro BW=176Hz
    if (i2c_controller_register8_set(i2c, addr, 0x1A, 0x01, kExtTimeout) != ERROR_NONE) return;
    // Sample rate: 1kHz / (1+5) = 166Hz
    if (i2c_controller_register8_set(i2c, addr, 0x19, 0x05, kExtTimeout) != ERROR_NONE) return;
    // Clear interrupt enables before reconfiguring
    if (i2c_controller_register8_set(i2c, addr, 0x38, 0x00, kExtTimeout) != ERROR_NONE) return;
    // Accel low-pass filter: ACCEL_FCHOICE_B=0, A_DLPF_CFG=0
    if (i2c_controller_register8_set(i2c, addr, 0x1D, 0x00, kExtTimeout) != ERROR_NONE) return;
    // Disable DMP and FIFO
    if (i2c_controller_register8_set(i2c, addr, 0x6A, 0x00, kExtTimeout) != ERROR_NONE) return;
    if (i2c_controller_register8_set(i2c, addr, 0x23, 0x00, kExtTimeout) != ERROR_NONE) return;
    // Interrupt: active-high (ACTL=0), push-pull (OPEN=0), latched (LATCH_INT_EN=1), cleared on any read (INT_ANYRD_2CLEAR=1) → 0x30
    if (i2c_controller_register8_set(i2c, addr, 0x37, 0x30, kExtTimeout) != ERROR_NONE) return;
    // Enable DATA_RDY interrupt
    if (i2c_controller_register8_set(i2c, addr, 0x38, 0x01, kExtTimeout) != ERROR_NONE) return;
    vTaskDelay(pdMS_TO_TICKS(100));
}

error_t readExtMpu6886Gyro(Device* i2c, uint8_t addr, const airmouse::MountMatrix& matrix, ImuGyroData* out) {
    uint8_t buf[6] = {};
    error_t err = i2c_controller_read_register(i2c, addr, 0x43, buf, sizeof(buf), kExtTimeout);
    if (err != ERROR_NONE) return err;
    float gx = extToI16(buf[0], buf[1]) * kExtGyroScale;
    float gy = extToI16(buf[2], buf[3]) * kExtGyroScale;
    float gz = extToI16(buf[4], buf[5]) * kExtGyroScale;
    matrix.apply(gx, gy, gz);
    out->gx = gx;
    out->gy = gy;
    out->gz = gz;
    return ERROR_NONE;
}

error_t readExtMpu6886Accel(Device* i2c, uint8_t addr, const airmouse::MountMatrix& matrix, ImuAccelData* out) {
    uint8_t buf[6] = {};
    error_t err = i2c_controller_read_register(i2c, addr, 0x3B, buf, sizeof(buf), kExtTimeout);
    if (err != ERROR_NONE) return err;
    float ax = extToI16(buf[0], buf[1]) * kExtAccelScale;
    float ay = extToI16(buf[2], buf[3]) * kExtAccelScale;
    float az = extToI16(buf[4], buf[5]) * kExtAccelScale;
    matrix.apply(ax, ay, az);
    out->ax = ax;
    out->ay = ay;
    out->az = az;
    return ERROR_NONE;
}

// Reads accel + gyro in the device frame. For a devicetree IMU the driver has
// already applied its mount-matrix; for the external module we apply the one
// stored in Context (identity by default).
//
// Gyro is mandatory; accel only feeds orientation mapping, so if a driver has no
// accel-only read path we zero it and the pipeline falls back to the fixed
// mapping rather than failing.
error_t ctxReadMotion(Context* ctx, ImuAccelData* accel, ImuGyroData* gyro) {
    *accel = ImuAccelData {};
    if (ctx->extI2cCtrl) {
        error_t err = readExtMpu6886Gyro(ctx->extI2cCtrl, ctx->extImuAddr, ctx->extMountMatrix, gyro);
        if (err != ERROR_NONE) return err;
        readExtMpu6886Accel(ctx->extI2cCtrl, ctx->extImuAddr, ctx->extMountMatrix, accel);
        return ERROR_NONE;
    }
    if (ctx->imuDevice) {
        error_t err = imu_read_gyro(ctx->imuDevice, gyro);
        if (err != ERROR_NONE) return err;
        imu_read_accel(ctx->imuDevice, accel);
        return ERROR_NONE;
    }
    return ERROR_NOT_FOUND;
}

void discoverImu(Context* ctx) {
    Device* found = nullptr;
    if (device_get_first_by_type(&IMU_TYPE, &found) == ERROR_NONE) {
        ctx->imuDevice = found;
        return;
    }

    // No device-tree IMU - scan I2C buses for a bare external MPU6886 module. Retried a few
    // times: a module can still be settling on the bus (or the bus itself still coming up) right
    // at app-launch time, when this runs - the real mpu6886 driver hits the same thing at power-up
    // and retries its own WHO_AM_I read for the same reason (see mpu6886.cpp's start()).
    struct ScanCtx { Device* ctrl; uint8_t addr; };
    ScanCtx scanCtx = { nullptr, 0x68 };
    static constexpr int kScanAttempts = 5;
    for (int attempt = 0; attempt < kScanAttempts && !scanCtx.ctrl; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(20));
        device_for_each_of_type(&I2C_CONTROLLER_TYPE, &scanCtx, [](Device* dev, void* c) -> bool {
            auto* sc = static_cast<ScanCtx*>(c);
            if (!device_is_ready(dev)) return true;
            if (i2c_controller_has_device_at_address(dev, sc->addr, pdMS_TO_TICKS(10)) == ERROR_NONE) {
                uint8_t whoami = 0;
                if (i2c_controller_read_register(dev, sc->addr, 0x75, &whoami, 1, pdMS_TO_TICKS(10)) == ERROR_NONE && whoami == 0x19) {
                    sc->ctrl = dev;
                    return false;
                }
            }
            return true;
        });
    }
    if (scanCtx.ctrl) {
        ctx->extI2cCtrl = scanCtx.ctrl;
        ctx->extImuAddr = scanCtx.addr;
        initExtMpu6886(ctx->extI2cCtrl, ctx->extImuAddr);
        LOG_I(TAG, "External MPU6886 found at 0x%02X", ctx->extImuAddr);
    }
}

// ── Settings (persisted) ──────────────────────────────────────────────────────

bool getSettingsPath(char* buf, size_t size) {
    return app_paths_get_user_data_path(APP_ID, SETTINGS_FILE, buf, size) == ERROR_NONE;
}

// True when the device has a pointer/touch input device. Without one, the user
// can't press the on-screen click pads at all - let alone both at once for the
// manual clutch - so the auto-clutch is the only way to freeze the pointer, and
// we default it on for those devices (its own settings toggle would likely be
// just as unreachable).
bool hasTouchInput() {
    // The LVGL indev list is static after boot in practice, but this runs from the
    // app's own task (via loadSettings), not the LVGL task, so take the lock.
    const bool locked = lvgl_try_lock(1000);
    bool found = false;
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) { found = true; break; }
    }
    if (locked) lvgl_unlock();
    return found;
}

void loadSettings(Context* ctx) {
    ctx->sensitivityPercent = 100;
    ctx->invertMode = 0;
    ctx->useOrientation = true;
    ctx->autoClutch = !hasTouchInput(); // no touch screen = can't use the manual clutch
    ctx->extMountMatrix = kDefaultExtMountMatrix;

    char path[192];
    if (getSettingsPath(path, sizeof(path))) {
        Preferences* prefs = preferences_open(path);
        if (prefs) {
            int32_t value = 0;
            if (preferences_opt_int32(prefs, PREF_SENSITIVITY, &value)) ctx->sensitivityPercent = value;
            if (preferences_opt_int32(prefs, PREF_INVERT, &value)) ctx->invertMode = value;
            bool enabled = true;
            if (preferences_opt_bool(prefs, PREF_ORIENTATION, &enabled)) ctx->useOrientation = enabled;
            if (preferences_opt_bool(prefs, PREF_AUTOCLUTCH, &enabled)) ctx->autoClutch = enabled;
            for (int i = 0; i < 9; i++) {
                char key[8];
                snprintf(key, sizeof(key), "%s%d", PREF_MOUNT, i);
                if (preferences_opt_int32(prefs, key, &value)) ctx->extMountMatrix.m[i] = value;
            }
            preferences_close(prefs);
        }
    }

    if (ctx->sensitivityPercent < 25) ctx->sensitivityPercent = 25;
    if (ctx->sensitivityPercent > 300) ctx->sensitivityPercent = 300;
    ctx->invertMode &= 0x03;
    applySettings(ctx);
}

// ── Motion processing ─────────────────────────────────────────────────────────

void onMotionTimer(lv_timer_t* t) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(t));
    if (!ctx) return;
    // Widgets only exist while this window is topmost - skip otherwise (buried by another app
    // started non-modally; window_manager deletes a buried window's widgets, so touching them
    // here would use-after-free them). The timer itself isn't destroyed by burial, only by
    // airMouseTeardown(), so it can still fire while buried.
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    ImuAccelData accel {};
    ImuGyroData gyro {};
    if (ctxReadMotion(ctx, &accel, &gyro) != ERROR_NONE) return;

    if (ctx->gyroLabel) {
        char buf[48];
        if (ctx->motion.isCalibrating()) {
            lv_label_set_text(ctx->gyroLabel, "Keep still: calibrating...");
        } else if (ctx->motion.isAutoClutching()) {
            lv_label_set_text(ctx->gyroLabel, "Repositioning - pointer frozen");
        } else {
            snprintf(buf, sizeof(buf), "bias:%.2f  %s", ctx->motion.biasMagnitude(),
                     ctx->motion.orientationActive() ? "tilt-aware" : "fixed");
            lv_label_set_text(ctx->gyroLabel, buf);
        }
    }

    uint32_t now = lv_tick_get();
    float dt = (ctx->lastMotionTick == 0)
        ? (1.0f / 33.0f)
        : static_cast<float>(now - ctx->lastMotionTick) / 1000.0f;
    ctx->lastMotionTick = now;
    if (dt > 0.2f) dt = 0.2f; // long stall (app was buried): don't lurch on resume

    // Always run the pipeline while the app is on screen - even with the pointer off or the
    // clutch held - so gyro calibration, the bias drift tracker and the gravity estimate stay
    // current. Only the *sending* is gated below; the pipeline drains its own sub-pixel carry
    // every tick, so discarding the output can't leave pent-up motion to "catch up" later.
    int dx = 0;
    int dy = 0;
    bool moved = ctx->motion.update(accel.ax, accel.ay, accel.az,
                                    gyro.gx, gyro.gy, gyro.gz,
                                    dt, dx, dy);

    // Only actually move the pointer while this window is topmost, enabled and connected.
    if (!ctx->isEnabled || !ctx->hidDevice) return;

    // Clutch: holding both buttons freezes the pointer so the user can reposition the device
    // without moving the cursor. The pipeline above still runs, so nothing lurches on release.
    if ((ctx->buttons & 0x03) == 0x03) return;

    if (!moved) return;
    sendMouseReport(ctx, static_cast<int8_t>(dx), static_cast<int8_t>(dy));
}

// ── Toolbar callbacks ─────────────────────────────────────────────────────────

// Toolbar action callbacks: the Tactility toolbar registers these for
// LV_EVENT_SHORT_CLICKED, so they must not filter on the event code.
void onSettingsClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    showView(ctx, AirMouseView::Settings);
}

void onBackClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    airMouseRequestSaveSettings(ctx); // persist the sensitivity (the sliderbox only reports value changes)
    showView(ctx, AirMouseView::Main);
}

void onSwitchToggled(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    airMouseRequestToggle(ctx, enabled);
}

// Widgets can't be rebuilt from inside a widget's own event callback (the rebuild
// deletes that widget mid-dispatch), so the switch happens on the next LVGL tick.
void onViewTimer(lv_timer_t* t) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(t));
    ctx->viewTimer = nullptr;
    buildView(ctx);
}

// ── BT lifecycle ──────────────────────────────────────────────────────────────

// Restores the radio to the state we found it in when we turned it on ourselves. Does not touch
// the BT event subscription - that's app-lifetime, set up by airMouseInitBt()/torn down by
// teardownBt() at app exit, not per toggle.
void restoreRadioIfNeeded(Context* ctx) {
    if (ctx->btDevice && ctx->radioWasOff) bluetooth_set_radio_enabled(ctx->btDevice, false);
    ctx->radioWasOff = false;
}

// Takes the LVGL lock. Everything in this file that runs on the app task (BT events, requests)
// needs it before touching a widget or ctx->hidDevice, which the motion timer reads under it.
bool lockLvgl() {
    for (int i = 0; i < 5; i++) {
        if (lvgl_try_lock(1000)) return true;
    }
    LOG_W(TAG, "Failed to acquire LVGL lock");
    return false;
}

void setSwitchChecked(Context* ctx, bool checked) {
    if (!lockLvgl()) return;
    if (ctx->switchWidget) {
        if (checked) lv_obj_add_state(ctx->switchWidget, LV_STATE_CHECKED);
        else lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
    }
    lvgl_unlock();
}

void startHid(Context* ctx) {
    // Called once the BT radio is confirmed ON (either already was, or just came up).
    // Runs on the app's own task - never on the LVGL task.
    ctx->radioEnabling = false;

    if (ctx->btDevice) {
        // Name before the profile start. The name is also the default BLE identity, and
        // bluetooth_hid_device_start() applies it to an already-running radio.
        bluetooth_set_device_name(ctx->btDevice, DEVICE_NAME);
    }

    ctx->hidDevice = bluetooth_hid_device_get();
    if (!ctx->hidDevice) {
        LOG_E(TAG, "BLE HID device unavailable after radio on");
        ctx->isEnabled = false;
        setSwitchChecked(ctx, false);
        return;
    }

    error_t err = bluetooth_hid_device_start(ctx->hidDevice, BT_HID_DEVICE_MODE_MOUSE);
    if (err != ERROR_NONE) {
        LOG_E(TAG, "Failed to start HID mouse: %d", (int)err);
        device_put(ctx->hidDevice);
        ctx->hidDevice = nullptr;
        ctx->isEnabled = false;
        setSwitchChecked(ctx, false);
    }
}

void handleBtEvent(Context* ctx, const BtEvent& event) {
    if (event.type != BT_EVENT_RADIO_STATE_CHANGED) return;
    LOG_I(TAG, "BT radio state: %d", (int)event.radio_state);

    if (event.radio_state == BT_RADIO_STATE_ON) {
        if (ctx->radioEnabling && window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
            startHid(ctx);
        }
    } else if (event.radio_state == BT_RADIO_STATE_OFF && ctx->isEnabled) {
        LOG_I(TAG, "BT radio turned off, disabling air mouse");
        if (lvgl_try_lock(1000)) {
            if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED && ctx->switchWidget) {
                lv_obj_remove_state(ctx->switchWidget, LV_STATE_CHECKED);
            }
            if (ctx->hidDevice) device_put(ctx->hidDevice);
            ctx->hidDevice     = nullptr;
            ctx->isEnabled     = false;
            ctx->radioEnabling = false;
            ctx->buttons       = 0;
            lvgl_unlock();
        }
    }
}

void teardownBt(Context* ctx) {
    // Unsubscribe FIRST - stops any in-flight BT events from firing against our (possibly
    // already freed) UI widget pointers after this returns.
    if (ctx->btDevice) bluetooth_event_unsubscribe(ctx->btDevice, &ctx->btEventSub);
    // Stop HID before restoring the radio. A profile left active outlives the app: it keeps
    // advertising, so the paired PC reconnects to a device that has no app behind it.
    // bluetooth_hid_device_stop() is safe from this task now (it no longer touches the GATT
    // registry, that is deferred to the host task on disconnect).
    // ctx->isEnabled was cleared by airMouseTeardown(), so the motion timer can't race us
    // for long; the LVGL lock is still the documented way to own ctx->hidDevice here.
    if (lockLvgl()) {
        if (ctx->hidDevice) {
            if (ctx->btDevice && bluetooth_is_radio_on_or_pending(ctx->btDevice)) {
                bluetooth_hid_device_stop(ctx->hidDevice);
            }
            device_put(ctx->hidDevice);
            ctx->hidDevice = nullptr;
        }
        lvgl_unlock();
    } else if (ctx->hidDevice) {
        // Lock not acquired: don't touch the HID session the motion timer may be using,
        // but still release our reference.
        device_put(ctx->hidDevice);
        ctx->hidDevice = nullptr;
    }
    restoreRadioIfNeeded(ctx);
    if (ctx->btDevice) device_put(ctx->btDevice);
    ctx->btDevice = nullptr;
}

} // namespace

// ── Shared state used by more than one view ───────────────────────────────────

void applySettings(Context* ctx) {
    airmouse::MotionConfig config;
    config.sensitivity = static_cast<float>(ctx->sensitivityPercent) / 100.0f;
    config.useOrientation = ctx->useOrientation;
    config.invertX = (ctx->invertMode & 0x01) != 0;
    config.invertY = (ctx->invertMode & 0x02) != 0;
    config.autoClutch = ctx->autoClutch;
    ctx->motion.configure(config);
}

// Settings are written on the kernel's file_ops task: this is queued from LVGL event handlers,
// whose task stack is in PSRAM and must not touch flash.
struct SaveSettingsJob {
    char path[192];
    int32_t sensitivityPercent;
    int32_t invertMode;
    bool useOrientation;
    bool autoClutch;
    int32_t mountMatrix[9];
};

void saveSettingsTask(void* context) {
    auto* job = static_cast<SaveSettingsJob*>(context);
    Preferences* prefs = preferences_open(job->path);
    if (prefs) {
        preferences_put_int32(prefs, PREF_SENSITIVITY, job->sensitivityPercent);
        preferences_put_int32(prefs, PREF_INVERT, job->invertMode);
        preferences_put_bool(prefs, PREF_ORIENTATION, job->useOrientation);
        preferences_put_bool(prefs, PREF_AUTOCLUTCH, job->autoClutch);
        for (int i = 0; i < 9; i++) {
            char key[8];
            snprintf(key, sizeof(key), "%s%d", PREF_MOUNT, i);
            preferences_put_int32(prefs, key, job->mountMatrix[i]);
        }
        preferences_close(prefs);
    }
    delete job;
}

// Snapshots the settings into a job that owns its data, so the work stays valid even if the app
// is torn down before it runs.
void queueSaveSettings(Context* ctx) {
    auto* job = new SaveSettingsJob {};
    if (!getSettingsPath(job->path, sizeof(job->path))) {
        delete job;
        return;
    }
    job->sensitivityPercent = ctx->sensitivityPercent;
    job->invertMode = ctx->invertMode;
    job->useOrientation = ctx->useOrientation;
    job->autoClutch = ctx->autoClutch;
    for (int i = 0; i < 9; i++) job->mountMatrix[i] = ctx->extMountMatrix.m[i];
    if (file_ops_queue(saveSettingsTask, job, nullptr) != ERROR_NONE) {
        LOG_E(TAG, "Failed to queue settings save");
        delete job;
    }
}

void sendMouseReport(Context* ctx, int8_t dx, int8_t dy) {
    if (!ctx->hidDevice || !bluetooth_hid_device_is_connected(ctx->hidDevice)) return;
    uint8_t report[4] = { ctx->buttons, static_cast<uint8_t>(dx), static_cast<uint8_t>(dy), 0 };
    bluetooth_hid_device_send_mouse(ctx->hidDevice, report, sizeof(report));
}

void handleSwitchToggle(Context* ctx, bool enabled) {
    // Runs on the app's own task (see airMouseRequestToggle()) so the flash access below can't
    // happen on the LVGL task.
    LOG_I(TAG, "Switch: %s", enabled ? "ON" : "OFF");
    ctx->isEnabled = enabled;

    if (enabled) {
        if (!ctx->btDevice) {
            LOG_E(TAG, "No Bluetooth device found");
            ctx->isEnabled = false;
            setSwitchChecked(ctx, false);
            return;
        }

        // Fresh bias calibration each time the pointer is switched on (gyro bias
        // drifts with temperature between sessions). The motion processor is owned by the LVGL
        // motion timer, so this has to happen under its lock now that we run on the app task.
        if (lockLvgl()) {
            ctx->motion.reset();
            ctx->lastMotionTick = 0;
            lvgl_unlock();
        }

        BtRadioState radioState;
        bluetooth_get_radio_state(ctx->btDevice, &radioState);

        // Name before the profile start. In the radio-off path it also has to be set before
        // enabling, because the identity is resolved while the radio comes up.
        bluetooth_set_device_name(ctx->btDevice, DEVICE_NAME);

        if (radioState == BT_RADIO_STATE_ON) {
            // Radio already up - start HID immediately.
            ctx->radioWasOff = false;
            startHid(ctx);
        } else {
            // Turn the radio on; startHid() will be called from handleBtEvent()
            // once BT_RADIO_STATE_ON fires. Our BT event subscription (airMouseInitBt())
            // was already claimed before the app's main loop started waiting, so that
            // wakeup won't be missed.
            LOG_I(TAG, "BT radio not on (state=%d), enabling...", (int)radioState);
            ctx->radioWasOff = true;
            ctx->radioEnabling = true;
            bluetooth_set_radio_enabled(ctx->btDevice, true);
        }
    } else {
        ctx->radioEnabling = false;
        // Explicit user toggle-off: stop HID cleanly (safe here since the user intentionally
        // disabled, so no race with app teardown). The LVGL lock protects ctx->hidDevice, which
        // the motion timer dereferences while sending a report.
        if (lockLvgl()) {
            if (ctx->hidDevice) {
                if (ctx->btDevice && bluetooth_is_radio_on_or_pending(ctx->btDevice)) {
                    bluetooth_hid_device_stop(ctx->hidDevice);
                }
                device_put(ctx->hidDevice);
            }
            ctx->hidDevice = nullptr;
            lvgl_unlock();
        }
        ctx->buttons = 0;
        restoreRadioIfNeeded(ctx);
    }
}

// ── Views ─────────────────────────────────────────────────────────────────────

void showView(Context* ctx, AirMouseView view) {
    ctx->view = view;
    if (ctx->viewTimer == nullptr) {
        ctx->viewTimer = lv_timer_create(onViewTimer, 0, ctx);
        lv_timer_set_repeat_count(ctx->viewTimer, 1);
    }
}

// Rebuilds the window widgets for ctx->view: the toolbar (both views need it as the first
// child of the column) then the content container the view fills. Unrelated widget pointers
// are nulled first, so the motion timer and callbacks can never touch a freed widget.
void buildView(Context* ctx) {
    if (!ctx->root) return;

    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    int gap = (w < 280 || h < 180) ? 2 : 4;

    lv_obj_clean(ctx->root);
    ctx->toolbar           = nullptr;
    ctx->content           = nullptr;
    ctx->switchWidget      = nullptr;
    ctx->gyroLabel         = nullptr;
    ctx->leftClickBtn      = nullptr;
    ctx->rightClickBtn     = nullptr;
    ctx->sensitivityBox    = nullptr;
    ctx->invertBtn         = nullptr;
    ctx->calibrateBtn      = nullptr;
    ctx->orientationSwitch = nullptr;
    ctx->autoClutchSwitch  = nullptr;

    if (ctx->view == AirMouseView::Settings) {
        ctx->toolbar = lvgl_toolbar_create(ctx->root, "Settings");
        lvgl_toolbar_set_nav_action(ctx->toolbar, LV_SYMBOL_LEFT, onBackClicked, ctx);
    } else {
        ctx->toolbar = lvgl_toolbar_create(ctx->root, "Air Mouse");
        ctx->switchWidget = lvgl_toolbar_add_switch_action(ctx->toolbar);
        if (ctx->isEnabled) lv_obj_add_state(ctx->switchWidget, LV_STATE_CHECKED);
        lv_obj_add_event_cb(ctx->switchWidget, onSwitchToggled, LV_EVENT_VALUE_CHANGED, ctx);
        lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_SETTINGS, onSettingsClicked, ctx);

        // Reflect an already-running radio instead of showing an idle switch: the app should
        // serve as soon as it is opened, like MediaKeys and MidiController do. btDevice was
        // acquired by airMouseInitBt() before the first view was built. The toggle runs on the
        // app's own task, exactly like a manual one.
        if (!ctx->isEnabled && ctx->btDevice && device_is_ready(ctx->btDevice)) {
            BtRadioState radioState;
            if (bluetooth_get_radio_state(ctx->btDevice, &radioState) == ERROR_NONE &&
                radioState == BT_RADIO_STATE_ON) {
                lv_obj_add_state(ctx->switchWidget, LV_STATE_CHECKED);
                airMouseRequestToggle(ctx, true);
            }
        }
    }

    ctx->content = lv_obj_create(ctx->root);
    lv_obj_set_width(ctx->content, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->content, 1);
    lv_obj_set_flex_flow(ctx->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ctx->content, gap, 0);
    lv_obj_set_style_border_width(ctx->content, 0, 0);
    lv_obj_set_style_radius(ctx->content, 0, 0);

    if (!ctx->imuDevice && !ctx->extI2cCtrl) {
        lv_obj_t* wrapperWidget = lv_obj_create(ctx->content);
        lv_obj_set_width(wrapperWidget, LV_PCT(100));
        lv_obj_set_flex_grow(wrapperWidget, 1);
        lv_obj_set_style_pad_all(wrapperWidget, 0, 0);
        lv_obj_set_style_border_width(wrapperWidget, 0, 0);
        lv_obj_set_style_bg_opa(wrapperWidget, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollable(wrapperWidget, false);
        // Show error in a centered label
        lv_obj_t* errLabel = lv_label_create(wrapperWidget);
        lv_label_set_text(errLabel, "No IMU found");
        lv_obj_center(errLabel);
        return;
    }

    if (ctx->view == AirMouseView::Settings) {
        settingsViewCreate(ctx);
    } else {
        mainViewCreate(ctx);
    }
}

void airMouseCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->root = parent;
    ctx->view = AirMouseView::Main;

    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = w < 280 || h < 180;
    int pad = isSmall ? 2 : 4;
    int gap = isSmall ? 2 : 4;

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, pad, 0);
    lv_obj_set_style_pad_row(parent, gap, 0);
    
    buildView(ctx);

    if (!ctx->motionTimer) {
        ctx->motionTimer = lv_timer_create(onMotionTimer, 30, ctx); // ~33Hz for responsive mouse
    }
}

void airMouseTeardown(Context* ctx) {
    ctx->radioEnabling = false;
    ctx->isEnabled = false;
    // Final snapshot. The queued job owns its data, so it still lands after the app is gone.
    queueSaveSettings(ctx);
    teardownBt(ctx);
    if (ctx->imuDevice) { device_put(ctx->imuDevice); ctx->imuDevice = nullptr; }
    // motionTimer is app-owned, not part of the window's widget subtree, so
    // window_manager_remove() never touches it - still safe (and necessary) to delete here.
    if (ctx->motionTimer) {
        lv_timer_delete(ctx->motionTimer);
        ctx->motionTimer = nullptr;
    }
    if (ctx->viewTimer) {
        lv_timer_delete(ctx->viewTimer);
        ctx->viewTimer = nullptr;
    }

    ctx->toolbar           = nullptr;
    ctx->content           = nullptr;
    ctx->switchWidget      = nullptr;
    ctx->gyroLabel         = nullptr;
    ctx->leftClickBtn      = nullptr;
    ctx->rightClickBtn     = nullptr;
    ctx->sensitivityBox    = nullptr;
    ctx->invertBtn         = nullptr;
    ctx->calibrateBtn      = nullptr;
    ctx->orientationSwitch = nullptr;
    ctx->autoClutchSwitch  = nullptr;
    ctx->root              = nullptr;
}

// ── App lifecycle ──────────────────────────────────────────────────────────────

bool airMouseInitBt(Context* ctx) {
    loadSettings(ctx);
    discoverImu(ctx);
    if (!ctx->imuDevice && !ctx->extI2cCtrl) {
        LOG_W(TAG, "No IMU found");
    }

    // ble0 is started for the process lifetime once enabled in the devicetree - just look it up,
    // no device_start() needed.
    if (device_get_first_by_type(&BLUETOOTH_TYPE, &ctx->btDevice) != ERROR_NONE) {
        LOG_E(TAG, "No Bluetooth device found");
        ctx->btDevice = nullptr;
        return false;
    }
    // Must claim this subscription's bit before the app's main loop makes its first
    // task_event_group_wait_any() call - a bit claimed mid-wait isn't included until the next
    // call, so a radio-on event fired right after a late subscribe could go unnoticed forever.
    if (bluetooth_event_subscribe(ctx->btDevice, &ctx->btEventSub, ctx->eventGroup) != ERROR_NONE) {
        LOG_E(TAG, "Failed to subscribe to BT events");
        device_put(ctx->btDevice);
        ctx->btDevice = nullptr;
        return false;
    }
    return true;
}

void airMouseProcessBtEvents(Context* ctx) {
    if (ctx->btDevice == nullptr) return;
    BtEvent event {};
    while (bluetooth_event_poll(&ctx->btEventSub, &event) == ERROR_NONE) {
        handleBtEvent(ctx, event);
    }
}

void airMouseRequestToggle(Context* ctx, bool enabled) {
    ctx->pendingToggle.store(enabled ? 1 : 0);
    if (ctx->requestBit != 0) task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void airMouseRequestSaveSettings(Context* ctx) {
    queueSaveSettings(ctx);
}

void airMouseProcessRequests(Context* ctx) {
    int toggle = ctx->pendingToggle.exchange(-1);
    if (toggle >= 0) handleSwitchToggle(ctx, toggle != 0);
}
