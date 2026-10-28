#include "MidiController.h"
#include "LcdDisplay.h"

#include <lvgl/widgets/toolbar.h>
#include <lvgl/icons/shared.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <app/paths.h>
#include <tactility/drivers/bluetooth_midi.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/file_ops.h>
#include <tactility/preferences.h>
#include <lvgl.h>
#include <cstring>
#include <cstdio>
#include <esp_timer.h>
#include <esp_log.h>

static constexpr auto* TAG = "MidiController";

// App-scoped settings: only which transport was chosen, never whether it was running.
static constexpr auto* APP_ID          = "tactility.midicontroller";
static constexpr auto* SETTINGS_FILE   = "settings.properties";
static constexpr auto* PREF_TRANSPORT  = "transport"; // 0 = BLE, 1 = USB
// BLE advertised name. It is also the default BLE identity, whose hash selects the address and
// bond slot, so it must stay stable across releases or hosts have to pair again.
static constexpr auto* BLE_DEVICE_NAME = "Tactility MIDI";

MidiController* MidiController::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Theme colors
// ---------------------------------------------------------------------------

static lv_color_t MC_WARNING()   { return lv_color_hex(0xFFE000); }
static lv_color_t MC_ERROR()     { return lv_color_hex(0xFF2020); }

// MIDI note numbers for the four drum tracks (KICK, SNARE, HIHAT, OPEN)
static constexpr uint8_t DRUM_NOTES[] = { 36, 38, 42, 46 };

// Per-track colors for BEATS mode (KICK, SNARE, HIHAT, OPEN)
static lv_color_t TRACK_COLOR(int t) {
    switch (t) {
        case 0:  return MC_ERROR();
        case 1:  return MC_WARNING();
        case 2:  return MC_PRIMARY();
        case 3:  return lv_color_hex(0xCC44FF);
        default: return MC_SURFACE();
    }
}

// ---------------------------------------------------------------------------
// Scaling helper - always based on display width, never parent
// ---------------------------------------------------------------------------


static inline lv_coord_t dispW() {
    return (lv_coord_t)lv_display_get_horizontal_resolution(nullptr);
}

static inline lv_coord_t dispH() {
    return (lv_coord_t)lv_display_get_vertical_resolution(nullptr);
}

// ---------------------------------------------------------------------------
// Style helpers
// ---------------------------------------------------------------------------

static void styleContainer(lv_obj_t* obj, lv_color_t bg = {}, bool transparent = true) {
    if (transparent) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(obj, bg, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollable(obj, false);
}

// ---------------------------------------------------------------------------
// MIDI transport - BLE (MediaKeys-style radio management) or USB, chosen from the transport
// selector overlay before the toolbar switch is turned on. sendMidi() dispatches to whichever
// one is currently running (only one of bleRunning_/usbRunning_ is ever true).
// ---------------------------------------------------------------------------

// Only reached via handleTransportToggle() (radio already on) or handleBtEvent() (radio just
// turned on after handleTransportToggle() requested it) - btDevice_ is always the ref'd pointer
// device_get_first_by_type() gave handleTransportToggle() in both cases, so every path that
// clears it here must device_put() first.
void MidiController::startBleMidi() {
    radioEnabling_ = false;
    struct Device* midiDev = bluetooth_midi_get();
    if (!midiDev) {
        ESP_LOGE(TAG, "No BLE MIDI device after radio on");
        if (btDevice_) {
            unsubscribeBtEvents();
            device_put(btDevice_);
        }
        btDevice_ = nullptr;
        bleRunning_ = false;
        failTransportStart();
        return;
    }
    error_t startErr = bluetooth_midi_start(midiDev);
    device_put(midiDev);  // bluetooth_midi_get() returns a referenced device
    if (startErr != ERROR_NONE) {
        ESP_LOGW(TAG, "bluetooth_midi_start failed");
        if (btDevice_) {
            unsubscribeBtEvents();
            device_put(btDevice_);
        }
        btDevice_ = nullptr;
        bleRunning_ = false;
        failTransportStart();
        return;
    }
    bleRunning_ = true;
    ESP_LOGI(TAG, "BLE MIDI started");
}

void MidiController::stopBleMidi() {
    if (bleRunning_) {
        sendMidi(0xB0, 123, 0);  // CC 123 = All Notes Off
        struct Device* midiDev = bluetooth_midi_get();
        if (midiDev) {
            bluetooth_midi_stop(midiDev);
            device_put(midiDev);  // bluetooth_midi_get() returns a referenced device
        }
        bleRunning_ = false;
    }
}

void MidiController::startUsbMidi() {
    struct Device* dev = usb_midi_device_get();
    if (!dev) {
        ESP_LOGE(TAG, "No USB MIDI device available");
        failTransportStart();
        return;
    }
    usb_midi_device_set_name(dev, "Tactility MIDI");
    error_t err = usb_midi_device_start(dev);
    device_put(dev);
    if (err != ERROR_NONE) {
        ESP_LOGW(TAG, "usb_midi_device_start failed: %d (slot busy?)", (int)err);
        failTransportStart();
        return;
    }
    usbRunning_ = true;
    ESP_LOGI(TAG, "USB MIDI started");
}

void MidiController::stopUsbMidi() {
    if (usbRunning_) {
        sendMidi(0xB0, 123, 0);  // CC 123 = All Notes Off
        struct Device* dev = usb_midi_device_get();
        if (dev) {
            usb_midi_device_stop(dev);
            device_put(dev);
        }
        usbRunning_ = false;
    }
}

void MidiController::handleBtEvent(const struct BtEvent& event) {
    if (event.type == BT_EVENT_RADIO_STATE_CHANGED) {
        ESP_LOGI(TAG, "BT radio state: %d", (int)event.radio_state);
        if (event.radio_state == BT_RADIO_STATE_ON && radioEnabling_) {
            startBleMidi();
        } else if (event.radio_state == BT_RADIO_STATE_OFF && bleRunning_) {
            ESP_LOGI(TAG, "BT radio turned off externally, stopping MIDI");
            sendMidi(0xB0, 123, 0);  // best-effort All Notes Off; the radio may already be down
            bleRunning_ = false;
            failTransportStart();
        }
    }
}

void MidiController::pollBtEvents() {
    if (!btEventSubscribed_) return;
    struct BtEvent event {};
    while (bluetooth_event_poll(&btEventSub_, &event) == ERROR_NONE) {
        handleBtEvent(event);
    }
    // If the radio dropped externally, the event handler only updated the UI.
    // Do not unsubscribe from inside the poll callback (that would destroy the
    // subscription while the loop is still using it); clean up once the queue
    // is drained and no enable request is in flight.
    if (!bleRunning_ && !radioEnabling_ && btEventSubscribed_) {
        unsubscribeBtEvents();
        if (btDevice_) {
            device_put(btDevice_);
            btDevice_ = nullptr;
        }
        radioWasOff_ = false;
    }
}

void MidiController::subscribeBtEvents() {
    if (btEventSubscribed_ || btDevice_ == nullptr) return;
    btEventSub_ = {};
    bluetooth_event_subscribe(btDevice_, &btEventSub_, eventGroup_);
    btEventSubscribed_ = true;
}

void MidiController::unsubscribeBtEvents() {
    if (!btEventSubscribed_) return;
    if (btDevice_ != nullptr) {
        bluetooth_event_unsubscribe(btDevice_, &btEventSub_);
    }
    btEventSubscribed_ = false;
}

void MidiController::failTransportStart() {
    lvgl_lock();
    if (switchWidget_) lv_obj_remove_state(switchWidget_, LV_STATE_CHECKED);
    if (transportBtn_) lv_obj_remove_state(transportBtn_, LV_STATE_DISABLED);
    lvgl_unlock();
}

void MidiController::enableTransportButton() {
    lvgl_lock();
    if (transportBtn_) lv_obj_remove_state(transportBtn_, LV_STATE_DISABLED);
    lvgl_unlock();
}

// Runs on the LVGL task: only the UI changes here. Starting a transport can write to flash or NVS,
// which isn't possible on the LVGL task, so the app's task does that in applyTransport().
void MidiController::handleTransportToggle(bool enabled) {
    ESP_LOGI(TAG, "Transport toggle: %s", enabled ? "ON" : "OFF");
    if (enabled) {
        // Close the transport chooser if it is still open
        if (transportOverlay_) {
            lv_obj_del(transportOverlay_);
            transportOverlay_ = nullptr;
            // First launch: the chooser was the only content. Open HOME before starting,
            // otherwise the switch would leave the content area empty.
            if (currentViewRoot_ == nullptr) {
                currentMode_ = MidiMode::HOME;
                buildHomeView(contentArea_);
            }
        }
        if (transportBtn_) lv_obj_add_state(transportBtn_, LV_STATE_DISABLED);
    }
    transportRequest_.store(enabled ? 1 : 0);
    task_event_group_signal(eventGroup_, transportRequestBit_);
}

// Runs on the app's task
void MidiController::applyTransport(bool enabled) {
    radioEnabling_ = false;
    if (enabled) {
        if (activeTransport_ == MidiTransport::USB) {
            startUsbMidi();
            return;
        }

        // bluetooth_find_first_ready_device() only finds an already-started device, so it can't
        // be used here: on a cold boot the BT device may not be started yet so this toggle would silently do nothing.
        // device_get_first_by_type() finds the device regardless of ready state; start it on demand.
        if (device_get_first_by_type(&BLUETOOTH_TYPE, &btDevice_) != ERROR_NONE) {
            ESP_LOGE(TAG, "No Bluetooth device found");
            failTransportStart();
            return;
        }
        if (!device_is_ready(btDevice_) && device_start(btDevice_) != ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to start Bluetooth device");
            device_put(btDevice_);
            btDevice_ = nullptr;
            failTransportStart();
            return;
        }
        bluetooth_set_device_name(btDevice_, BLE_DEVICE_NAME);
        subscribeBtEvents();

        enum BtRadioState radioState;
        bluetooth_get_radio_state(btDevice_, &radioState);
        if (radioState == BT_RADIO_STATE_ON) {
            radioWasOff_ = false;
            startBleMidi();
        } else {
            ESP_LOGI(TAG, "BT radio not on (state=%d), enabling...", (int)radioState);
            radioWasOff_ = true;
            radioEnabling_ = true;
            bluetooth_set_radio_enabled(btDevice_, true);
        }
    } else {
        if (activeTransport_ == MidiTransport::USB) {
            stopUsbMidi();
        } else {
            stopBleMidi();
            if (btDevice_) {
                unsubscribeBtEvents();
                if (radioWasOff_) bluetooth_set_radio_enabled(btDevice_, false);
                device_put(btDevice_);
                btDevice_ = nullptr;
            }
            radioWasOff_ = false;
        }
        enableTransportButton();
    }
}

void MidiController::attachEventGroup(struct TaskEventGroup* group) {
    eventGroup_ = group;
    task_event_group_claim_bit(group, &transportRequestBit_);
}

void MidiController::processEvents() {
    const int request = transportRequest_.exchange(-1);
    if (request >= 0) {
        applyTransport(request == 1);
    }
    pollBtEvents();
}

void MidiController::sendMidi(uint8_t cmd, uint8_t d1, uint8_t d2) {
    uint8_t msg[3] = { cmd, d1, d2 };
    if (bleRunning_) {
        struct Device* dev = bluetooth_midi_get();
        if (dev) {
            bluetooth_midi_send(dev, msg, 3);
            device_put(dev);  // bluetooth_midi_get() returns a referenced device
        }
    } else if (usbRunning_) {
        struct Device* dev = usb_midi_device_get();
        if (dev) {
            usb_midi_device_send(dev, msg, 3);
            device_put(dev);
        }
    }
}

void MidiController::stopAllNotes() {
    for (int i = 0; i < 128; i++) {
        sendMidi(0x80, (uint8_t)i, 0);
    }
}

// ---------------------------------------------------------------------------
// Transport selector
// ---------------------------------------------------------------------------

void MidiController::updateTransportButton() {
    if (!transportIconLbl_) return;
    lv_label_set_text(transportIconLbl_,
        activeTransport_ == MidiTransport::USB
            ? LVGL_ICON_SHARED_USB : LVGL_ICON_SHARED_BLUETOOTH);
}

void MidiController::showTransportSelect() {
    if (transportOverlay_ || bleRunning_ || usbRunning_) return;
    transportOverlay_ = buildTransportSelectOverlay(contentArea_);
}

lv_obj_t* MidiController::buildTransportSelectOverlay(lv_obj_t* parent) {
    const lv_font_t* fontSmall   = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontDefault = lvgl_get_text_font(FONT_SIZE_DEFAULT);
    const lv_font_t* fontLarge   = lvgl_get_text_font(FONT_SIZE_LARGE);

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(root, MC_BG(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 32, 0);
    lv_obj_set_style_pad_gap(root, 18, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_clickable(root, true);  // swallow taps on empty space
    lv_obj_move_foreground(root);

    mcLabel(root, "SELECT MIDI TRANSPORT", fontLarge, MC_PRIMARY());
    mcLabel(root, "Choose how this app sends MIDI.", fontDefault, MC_LABEL());

    lv_obj_t* row = mcContainer(root);
    lv_obj_set_size(row, LV_PCT(100), 280);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 24, 0);

    struct CardSpec {
        const char* title;
        const char* subtitle;
        MidiTransport transport;
    };
    static const CardSpec CARDS[] = {
        { "BLUETOOTH LE", "Wireless MIDI", MidiTransport::BLE },
        { "USB", "Wired MIDI device", MidiTransport::USB },
    };

    for (const CardSpec& spec : CARDS) {
        bool selected = (activeTransport_ == spec.transport);

        lv_obj_t* card = mcButton(row);
        lv_obj_set_size(card, 420, 260);
        lv_obj_set_style_bg_color(card, selected ? MC_SURFACE() : MC_PANEL(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(card, selected ? MC_PRIMARY() : lv_color_hex(0x223344), 0);
        lv_obj_set_style_pad_all(card, 20, 0);
        lv_obj_set_style_pad_gap(card, 10, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(card, (void*)(intptr_t)(spec.transport == MidiTransport::USB ? 1 : 0));
        lv_obj_add_event_cb(card, cbTransportCard, LV_EVENT_CLICKED, this);

        mcLabel(card, spec.title, fontLarge, selected ? MC_PRIMARY() : MC_TEXT());
        mcLabel(card, spec.subtitle, fontSmall, MC_LABEL());
        if (selected) mcLabel(card, "CURRENT", fontSmall, MC_PRIMARY());
    }

    if (currentViewRoot_ != nullptr) {
        mcLabel(root, "The toolbar switch starts and stops sending.", fontSmall, MC_LABEL());
        mcBtn(root, "CANCEL", 160, MC_BTN_H, cbTransportCancel, this);
    } else {
        mcLabel(root, "Choose a transport to open the controller.", fontSmall, MC_LABEL());
    }

    return root;
}

void MidiController::cbTransportSelect(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->showTransportSelect();
}

// The write runs on the kernel's file_ops task: cbTransportCard() runs on the LVGL task, whose
// stack is in PSRAM and must not touch flash. The job owns its data so it can outlive the app.
struct SaveTransportJob {
    char path[192];
    int32_t transport;
};

static void saveTransportTask(void* context) {
    auto* job = static_cast<SaveTransportJob*>(context);
    Preferences* prefs = preferences_open(job->path);
    if (prefs) {
        preferences_put_int32(prefs, PREF_TRANSPORT, job->transport);
        preferences_close(prefs);
    }
    delete job;
}

void MidiController::saveTransportSelection() {
    auto* job = new SaveTransportJob {};
    if (app_paths_get_user_data_path(APP_ID, SETTINGS_FILE, job->path, sizeof(job->path)) != ERROR_NONE) {
        delete job;
        return;
    }
    job->transport = (activeTransport_ == MidiTransport::USB) ? 1 : 0;
    if (file_ops_queue(saveTransportTask, job, nullptr) != ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to queue transport selection save");
        delete job;
    }
}

void MidiController::cbTransportCard(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    lv_obj_t* card = lv_event_get_target_obj(e);
    if (!mc || !card) return;

    intptr_t id = (intptr_t)lv_obj_get_user_data(card);
    MidiTransport selected = (id == 1) ? MidiTransport::USB : MidiTransport::BLE;
    // Persist an explicit choice - including the very first one, even when it's the default BLE,
    // so the chooser doesn't come back on the next launch.
    bool shouldSave = (selected != mc->activeTransport_) || !mc->hasSavedTransport_;
    mc->activeTransport_ = selected;
    mc->hasSavedTransport_ = true;
    mc->updateTransportButton();
    if (shouldSave) mc->saveTransportSelection();

    if (mc->transportOverlay_) {
        lv_obj_del(mc->transportOverlay_);
        mc->transportOverlay_ = nullptr;
    }

    // First launch: there is no view behind the overlay yet - open the home grid.
    if (mc->currentViewRoot_ == nullptr) {
        mc->currentMode_ = MidiMode::HOME;
        mc->buildHomeView(mc->contentArea_);
    }
}

void MidiController::cbTransportCancel(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || !mc->transportOverlay_) return;
    lv_obj_del(mc->transportOverlay_);
    mc->transportOverlay_ = nullptr;
}

// ---------------------------------------------------------------------------
// Mode timer management
// ---------------------------------------------------------------------------

void MidiController::stopAllModeTimers() {
    if (arpTimer_)   { lv_timer_delete(arpTimer_);   arpTimer_   = nullptr; }
    if (lfoTimer_)   { lv_timer_delete(lfoTimer_);   lfoTimer_   = nullptr; }
    if (zenTimer_)   { lv_timer_delete(zenTimer_);   zenTimer_   = nullptr; }
    if (dropTimer_)  { lv_timer_delete(dropTimer_);  dropTimer_  = nullptr; }
    if (rngTimer_)   { lv_timer_delete(rngTimer_);   rngTimer_   = nullptr; }
    if (ragaTimer_)  { lv_timer_delete(ragaTimer_);  ragaTimer_  = nullptr; }
    if (dimTimer_)   { lv_timer_delete(dimTimer_);   dimTimer_   = nullptr; }
    if (baby8Timer_) { lv_timer_delete(baby8Timer_); baby8Timer_ = nullptr; }
    if (tb3poTimer_) { lv_timer_delete(tb3poTimer_); tb3poTimer_ = nullptr; }
    if (euclidTimer_){ lv_timer_delete(euclidTimer_);euclidTimer_= nullptr; }
    // The UI timer only services BEATS' step cursor; don't leave it running
    // every 20 ms while another view is open.
    if (uiTimer_)    { lv_timer_delete(uiTimer_);    uiTimer_    = nullptr; }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void MidiController::switchToMode(MidiMode mode) {
    // Close the transport chooser if it is still open
    if (transportOverlay_) {
        lv_obj_del(transportOverlay_);
        transportOverlay_ = nullptr;
    }

    // Stop active mode timers
    stopAllModeTimers();

    // Stop sequencer if leaving BEATS
    if (currentMode_ == MidiMode::BEATS && seq_ && seq_->isPlaying()) {
        seq_->stop();
        beats_.playing = false;
    }

    // Send All-Notes-Off (CC 123) - single packet instead of 128 note-offs
    sendMidi(0xB0, 123, 0);

    // Null all LVGL widget pointers BEFORE deleting the tree, so any callbacks
    // fired during deletion (LV_EVENT_DELETE, pending timers) see null pointers.
    // Data fields (octave, bpm, scale, etc.) are preserved across mode switches.
    zen_.canvas = nullptr; zen_.statusLbl = nullptr; zen_.ballCntLbl = nullptr; zen_.octLbl = nullptr;
    for (auto& b : zen_.ballObj) b = nullptr;

    drop_.canvas = nullptr; drop_.statusLbl = nullptr;
    drop_.modeLbl = nullptr; drop_.octLbl = nullptr; drop_.modeBtn = nullptr;
    for (auto& b : drop_.ballObj) b = nullptr;
    for (auto& l : drop_.platLbl)  l = nullptr;

    xy_.canvas = nullptr; xy_.indicator = nullptr;
    xy_.xCCLbl = nullptr; xy_.yCCLbl = nullptr; xy_.valLbl = nullptr;

    for (auto& k : keys_.whiteKey) k = nullptr;
    for (auto& k : keys_.blackKey) k = nullptr;
    keys_.pianoArea = nullptr;
    keys_.octLbl = nullptr; keys_.keyLbl = nullptr;
    keys_.scaleLbl = nullptr; keys_.statusLbl = nullptr;

    beats_.playBtn = nullptr; beats_.playLbl = nullptr; beats_.bpmLbl = nullptr;
    for (int t = 0; t < Sequencer::NUM_TRACKS; t++)
        for (int s = 0; s < Sequencer::NUM_STEPS; s++)
            beats_.stepBtn[t][s] = nullptr;

    for (auto& k : arp_.pianoKey) k = nullptr;
    arp_.patternLbl  = nullptr; arp_.chordLbl    = nullptr;
    arp_.speedLbl    = nullptr; arp_.bpmLbl      = nullptr;
    arp_.octLbl      = nullptr; arp_.pianoOctLbl = nullptr;
    arp_.statusLbl   = nullptr; arp_.noteLbl     = nullptr;

    for (auto& b : chord_.chordBtn) b = nullptr;
    chord_.octLbl = nullptr; chord_.scaleLbl = nullptr;

    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 8; c++)
            grid_.cell[r][c] = nullptr;
    grid_.octLbl = nullptr; grid_.noteLbl = nullptr;

    lfo_.canvas     = nullptr; lfo_.startBtn  = nullptr; lfo_.startLbl  = nullptr;
    lfo_.statusLbl  = nullptr; lfo_.rateLbl   = nullptr; lfo_.waveLbl   = nullptr;
    lfo_.amountLbl  = nullptr; lfo_.ccLbl     = nullptr; lfo_.valueLbl  = nullptr;

    rng_.playBtn    = nullptr; rng_.playLbl   = nullptr; rng_.keyLbl    = nullptr;
    rng_.scaleLbl   = nullptr; rng_.minOctLbl = nullptr; rng_.maxOctLbl = nullptr;
    rng_.probLbl    = nullptr; rng_.bpmLbl    = nullptr; rng_.subdivLbl = nullptr;
    rng_.noteLbl    = nullptr;
    rng_.playing    = false;   rng_.currentNote = -1;

    raga_.ragaLbl   = nullptr; raga_.talaLbl    = nullptr; raga_.rootLbl  = nullptr;
    raga_.bpmLbl    = nullptr; raga_.statusLbl  = nullptr; raga_.playBtn  = nullptr;
    raga_.playLbl   = nullptr; raga_.droneBtn   = nullptr; raga_.droneLbl = nullptr;
    raga_.ragaBigLbl= nullptr; raga_.talaBigLbl = nullptr;
    raga_.playing   = false;   raga_.droneOn  = false;
    if (raga_.droneActive) { sendMidi(0x80, raga_.droneNote, 0); raga_.droneActive = false; }
    if (raga_.noteActive)  { sendMidi(0x80, raga_.currentNote, 0); raga_.noteActive = false; }

    dim_.eqLbl  = nullptr; dim_.aLbl   = nullptr; dim_.bLbl   = nullptr;
    dim_.cLbl   = nullptr; dim_.dLbl   = nullptr; dim_.outLbl = nullptr;
    dim_.octLbl = nullptr; dim_.rndLbl = nullptr; dim_.bpmLbl = nullptr; dim_.playLbl = nullptr;
    dim_.playing = false;
    if (dim_.lastNote >= 0) { sendMidi(0x80, (uint8_t)dim_.lastNote, 0); dim_.lastNote = -1; }

    for (int i = 0; i < 8; i++) { baby8_.stepBtn[i] = nullptr; baby8_.noteLbl[i] = nullptr; }
    baby8_.bpmLbl = nullptr; baby8_.swingLbl = nullptr; baby8_.patLbl = nullptr;
    baby8_.editNoteLbl = nullptr; baby8_.editVelLbl = nullptr; baby8_.playLbl = nullptr;
    baby8_.playing = false;
    if (baby8_.lastNote >= 0) { sendMidi(0x80, (uint8_t)baby8_.lastNote, 0); baby8_.lastNote = -1; }

    tb3po_.canvas = nullptr; tb3po_.densityLbl = nullptr; tb3po_.seedLbl = nullptr;
    tb3po_.bpmLbl = nullptr; tb3po_.scaleLbl   = nullptr; tb3po_.keyLbl  = nullptr;
    tb3po_.playLbl = nullptr;
    tb3po_.playing = false;
    if (tb3po_.lastNote >= 0) { sendMidi(0x80, (uint8_t)tb3po_.lastNote, 0); tb3po_.lastNote = -1; }
    tb3po_.lastNoteStep = -1;

    euclid_.canvas   = nullptr; euclid_.bpmLbl   = nullptr; euclid_.stepsLbl  = nullptr;
    euclid_.eventsLbl= nullptr; euclid_.rotLbl   = nullptr; euclid_.noteLbl   = nullptr;
    euclid_.voiceLbl = nullptr; euclid_.playLbl  = nullptr; euclid_.tripLbl = nullptr;
    euclid_.voiceTabRow = nullptr;
    euclid_.playing = false;

    // Turn off encoder LEDs before building the new view
    // (each view's dispatch sets its own colors on first poll)
    encoderSetAllLeds(this, 0x000000);

    // On the next encoder poll, seed btnPrev from the live button state
    // without firing any actions - prevents a held button from registering
    // as a fresh press in the new view.
    encoder_.btnSeedNext = true;

    // Discard any touch still in progress so the tap that triggered this mode
    // switch doesn't land as a click on a button in the newly built view.
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        lv_indev_wait_release(indev);
        indev = lv_indev_get_next(indev);
    }

    // Destroy current view tree
    if (currentViewRoot_) {
        lv_obj_del(currentViewRoot_);
        currentViewRoot_ = nullptr;
    }

    currentMode_ = mode;
    lcdSetMode(mode);      // marks LCD dirty
    lcdUpdate(this);       // paint immediately (no-op if LCD not present)

    if (!contentArea_) return;

    switch (mode) {
        case MidiMode::HOME:  buildHomeView(contentArea_); break;
        case MidiMode::KEYS:  buildKeysView (this, contentArea_); break;
        case MidiMode::BEATS: buildBeatsView(this, contentArea_); break;
        case MidiMode::ZEN:   buildZenView  (this, contentArea_); break;
        case MidiMode::DROP:  buildDropView (this, contentArea_); break;
        case MidiMode::RNG:   buildRngView  (this, contentArea_); break;
        case MidiMode::XY:    buildXYPadView(this, contentArea_); break;
        case MidiMode::ARP:   buildArpView  (this, contentArea_); break;
        case MidiMode::GRID:  buildGridView (this, contentArea_); break;
        case MidiMode::CHORD:      buildChordView     (this, contentArea_); break;
        case MidiMode::LFO:        buildLfoView       (this, contentArea_); break;
        case MidiMode::RAGA:       buildRagaView      (this, contentArea_); break;
        case MidiMode::DIMENSIONS: buildDimensionsView(this, contentArea_); break;
        case MidiMode::BABY8:      buildBaby8View     (this, contentArea_); break;
        case MidiMode::TB3PO:      buildTb3poView     (this, contentArea_); break;
        case MidiMode::EUCLID:     buildEuclidView    (this, contentArea_); break;
        default: buildHomeView(contentArea_); break;
    }

    // BEATS is the only view that needs the shared UI timer.
    if (mode == MidiMode::BEATS) {
        uiTimer_ = lv_timer_create(cbUiTimer, 20, this);
    }

    // Re-arm wait_release after view construction so any touch that was in
    // flight when we entered this function cannot land on buttons in the new view.
    {
        lv_indev_t* indev = lv_indev_get_next(nullptr);
        while (indev) {
            lv_indev_wait_release(indev);
            indev = lv_indev_get_next(indev);
        }
    }
}

// ---------------------------------------------------------------------------
// HOME view - 2×5 mode selection grid
// ---------------------------------------------------------------------------

static const char* MODE_LABELS[] = {
    "KEYS", "BEATS", "ZEN", "DROP", "RNG",
    "XY", "ARP", "GRID", "CHORD", "LFO",
    "RAGA", "DIM", "BABY8", "TB3PO", "EUCLID"
};

static lv_color_t MODE_COLOR(int idx) {
    static const uint32_t COLORS[] = {
        0xFF3333, 0xFF8800, 0xFFFF00, 0x33CC33, 0x3399FF,
        0xAA33FF, 0xFF33CC, 0x00FFFF, 0xFF9944, 0x99FF44,
        0xFF8844, 0x44BBFF, 0xFF88FF, 0x88FF44, 0xFF4488
    };
    if (idx >= 0 && idx < 15) return lv_color_hex(COLORS[idx]);
    return lv_color_hex(0x888888);
}

void MidiController::cbModeBtn(lv_event_t* e) {
    if (!s_instance) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < (int)MidiMode::COUNT) {
        s_instance->switchToMode((MidiMode)idx);
    }
}

void MidiController::buildHomeView(lv_obj_t* parent) {
    lv_coord_t W = dispW();
    lv_coord_t H = dispH();

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    styleContainer(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, MC_BG(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 16, 0);
    lv_obj_set_style_pad_gap(root, 16, 0);
    currentViewRoot_ = root;

    // Button grid - 3 rows of 5, each row flex_grow=1 so buttons always fill height
    lv_coord_t gap  = 8;
    // Root has pad_all 16 on each side; 5 buttons share the rest minus 4 gaps.
    lv_coord_t btnW = (W - 16 * 2 - gap * 4) / 5;
    const lv_font_t* btnFont = W < 400 ? lvgl_get_text_font(FONT_SIZE_SMALL)
                                       : lvgl_get_text_font(FONT_SIZE_DEFAULT);

    for (int row = 0; row < 3; row++) {
        lv_obj_t* rowObj = mcContainer(root);
        lv_obj_set_width(rowObj, LV_PCT(100));
        lv_obj_set_flex_grow(rowObj, 1);
        lv_obj_set_flex_flow(rowObj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowObj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        styleContainer(rowObj);
        lv_obj_set_style_bg_color(rowObj, MC_BG(), 0);
        lv_obj_set_style_bg_opa(rowObj, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_gap(rowObj, gap, 0);
        lv_obj_set_style_pad_all(rowObj, 0, 0);

        for (int col = 0; col < 5; col++) {
            int i = row * 5 + col;
            lv_obj_t* btn = mcButton(rowObj);
            lv_obj_set_size(btn, btnW, LV_PCT(100));
            lv_obj_set_style_bg_color(btn, MC_SURFACE(), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(btn, MODE_COLOR(i), 0);
            lv_obj_set_style_border_width(btn, 8, 0);
            lv_obj_set_style_radius(btn, 24, 0);
            lv_obj_set_style_pad_all(btn, 8, 0);
            lv_obj_add_event_cb(btn, cbModeBtn, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);

            // Color accent bar at top of button
            lv_obj_t* bar = mcContainer(btn);
            lv_obj_set_size(bar, LV_PCT(100), 16);
            lv_obj_set_style_bg_color(bar, MODE_COLOR(i), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 0, 0);
            lv_obj_set_clickable(bar, false);
            lv_obj_set_align(bar, LV_ALIGN_TOP_MID);
            lv_obj_set_scrollable(bar, false);

            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, MODE_LABELS[i]);
            lv_obj_set_style_text_color(lbl, MC_TEXT(), 0);
            lv_obj_set_style_text_font(lbl, btnFont, 0);
            lv_obj_center(lbl);
            lv_obj_set_clickable(lbl, false);
        }
    }
    (void)H;
}

// ---------------------------------------------------------------------------
// UI timer - BEATS step indicator
// ---------------------------------------------------------------------------

void MidiController::cbTransportSwitch(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    mc->handleTransportToggle(on);
}

void MidiController::cbUiTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;

    // BEATS: update step highlight
    if (mc->currentMode_ == MidiMode::BEATS && mc->seq_) {
        int step = mc->seqPlayStep.load();
        int lastStep = mc->seqLastStep.load();
        if (step != lastStep) {
            int prev = lastStep;
            mc->seqLastStep.store(step);

            // De-highlight previous step across all tracks
            if (prev >= 0 && prev < Sequencer::NUM_STEPS) {
                for (int t = 0; t < Sequencer::NUM_TRACKS; t++) {
                    lv_obj_t* btn = mc->beats_.stepBtn[t][prev];
                    if (btn) {
                        bool active = mc->seq_->getStep(t, prev);
                        lv_obj_set_style_bg_color(btn,
                            active ? TRACK_COLOR(t) : MC_SURFACE(), LV_PART_MAIN);
                        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                    }
                }
            }
            // Highlight current step
            if (step >= 0 && step < Sequencer::NUM_STEPS) {
                for (int t = 0; t < Sequencer::NUM_TRACKS; t++) {
                    lv_obj_t* btn = mc->beats_.stepBtn[t][step];
                    if (btn) {
                        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                        lv_obj_set_style_border_width(btn, 4, LV_PART_MAIN);
                        lv_obj_set_style_border_color(btn, MC_PRIMARY(), LV_PART_MAIN);
                    }
                }
            }
        }

        // Check note-offs
        uint64_t now = (uint64_t)esp_timer_get_time();
        for (int t = 0; t < Sequencer::NUM_TRACKS; t++) {
            if (mc->beats_.noteOffAtUs[t] > 0 && now >= mc->beats_.noteOffAtUs[t]) {
                mc->sendMidi(0x80, DRUM_NOTES[t], 0);
                mc->beats_.noteOffAtUs[t] = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

void MidiController::loadSettings() {
    char path[192];
    if (app_paths_get_user_data_path(APP_ID, SETTINGS_FILE, path, sizeof(path)) != ERROR_NONE) {
        return;
    }
    Preferences* prefs = preferences_open(path);
    if (!prefs) return;
    int32_t value = 0;
    if (preferences_opt_int32(prefs, PREF_TRANSPORT, &value) && (value == 0 || value == 1)) {
        activeTransport_ = (value == 1) ? MidiTransport::USB : MidiTransport::BLE;
        hasSavedTransport_ = true;
    }
    preferences_close(prefs);
}

void MidiController::detectBleState() {
    // Check if BLE MIDI is already active (e.g. system BT was on before app launched).
    // If so, mark bleRunning_ so the app is ready to send. No USB
    // equivalent - unlike BLE's system-wide radio, USB device mode has no "already running from
    // some other app" case worth auto-detecting (only one class can hold the slot at a time, and
    // if MIDI already holds it, that's almost certainly this same app from a previous session).
    //
    // Uses the same ref-counted device_get_first_by_type() lookup handleTransportToggle() uses
    // (rather than bluetooth_find_first_ready_device(), which would also work here since this
    // check only fires if the radio's already on) so btDevice_ is always ref'd whenever non-null,
    // uniformly - every btDevice_ = nullptr elsewhere in this file can then unconditionally
    // device_put() first without needing to track which code path originally acquired it.
    if (device_get_first_by_type(&BLUETOOTH_TYPE, &btDevice_) == ERROR_NONE) {
        enum BtRadioState radioState;
        bluetooth_get_radio_state(btDevice_, &radioState);
        if (radioState == BT_RADIO_STATE_ON) {
            // Radio is already on - register callback and start MIDI immediately.
            radioWasOff_ = false;
            bluetooth_set_device_name(btDevice_, BLE_DEVICE_NAME);
            subscribeBtEvents();
            struct Device* midiDev = bluetooth_midi_get();
            bool midiStarted = false;
            if (midiDev) {
                midiStarted = (bluetooth_midi_start(midiDev) == ERROR_NONE);
                device_put(midiDev);  // bluetooth_midi_get() returns a referenced device
            }
            if (midiStarted) {
                bleRunning_ = true;
                activeTransport_ = MidiTransport::BLE;
            } else {
                // MIDI start failed - clean up subscription
                unsubscribeBtEvents();
                device_put(btDevice_);
                btDevice_ = nullptr;
            }
        } else {
            device_put(btDevice_);
            btDevice_ = nullptr;
        }
    }
}

void MidiController::createWidgets(lv_obj_t* parent, void* userData) {
    auto* mc = static_cast<MidiController*>(userData);
    s_instance = mc;
    mc->createWidgetsImpl(parent);
}

void MidiController::createWidgetsImpl(lv_obj_t* parent) {
    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(parent, MC_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Toolbar
    toolbar_ = lvgl_toolbar_create(parent, "Midi Controller");
    lv_obj_align(toolbar_, LV_ALIGN_TOP_MID, 0, 0);

    // Transport selector button (BLE/USB), added before the switch so it sits to its left within
    // the toolbar's action area. Opens the full-screen chooser; disabled while the switch is on so
    // the transport can't change mid-stream.
    transportBtn_ = lvgl_toolbar_add_text_button_action(toolbar_, "BLE", cbTransportSelect, this);

    // Make the action look like a button (the toolbar's default text action has
    // no background). Reuse the action's existing label as the icon label: the
    // toolbar owns that child and may be drawn while the app is open, so deleting
    // it would leave the widget tree with a dangling entry.
    transportIconLbl_ = lv_obj_get_child(transportBtn_, 0);
    if (transportIconLbl_ != nullptr) {
        lv_obj_set_style_text_font(transportIconLbl_,
            lvgl_get_shared_icon_default_font(), 0);
        lv_obj_set_style_text_color(transportIconLbl_, lv_color_white(), 0);
    }
    lv_obj_set_size(transportBtn_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(transportBtn_, 10, 0);
    lv_obj_set_style_bg_color(transportBtn_, lv_color_hex(0x0A4C78), 0);
    lv_obj_set_style_bg_opa(transportBtn_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(transportBtn_, lv_color_hex(0x8FE8FF), 0);
    lv_obj_set_style_border_width(transportBtn_, 2, 0);
    lv_obj_set_style_radius(transportBtn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(transportBtn_, 0, 0);
    lv_obj_set_style_opa(transportBtn_, LV_OPA_60, LV_STATE_DISABLED);
    updateTransportButton();

    switchWidget_ = lvgl_toolbar_add_switch_action(toolbar_);
    lv_obj_add_event_cb(switchWidget_, cbTransportSwitch, LV_EVENT_VALUE_CHANGED, this);

    // Main wrapper
    mainWrapper_ = mcContainer(parent);
    lv_obj_set_width(mainWrapper_, LV_PCT(100));
    lv_obj_set_flex_grow(mainWrapper_, 1);
    styleContainer(mainWrapper_);
    lv_obj_set_style_bg_color(mainWrapper_, MC_BG(), 0);
    lv_obj_set_style_bg_opa(mainWrapper_, LV_OPA_COVER, 0);

    // Content area (fills mainWrapper)
    contentArea_ = mcContainer(mainWrapper_);
    lv_obj_set_size(contentArea_, LV_PCT(100), LV_PCT(100));
    styleContainer(contentArea_);
    lv_obj_set_style_bg_color(contentArea_, MC_BG(), 0);
    lv_obj_set_style_bg_opa(contentArea_, LV_OPA_COVER, 0);

    // Create Sequencer
    seq_ = std::make_unique<Sequencer>();
    seq_->onStep = [](int /*step*/, int track) {
        if (!s_instance) return;
        static const uint32_t NOTE_OFF_US[] = { 200000, 150000, 50000, 300000 };
        s_instance->sendMidi(0x90, DRUM_NOTES[track], 100);
        s_instance->beats_.noteOffAtUs[track] =
            (uint64_t)esp_timer_get_time() + NOTE_OFF_US[track];
    };
    seq_->onTick = [](int step) {
        if (s_instance) s_instance->seqPlayStep.store(step);
    };

    // detectBleState() found a BLE MIDI session that is already running (e.g. system BT was on
    // before the app launched): the switch shows it
    if (bleRunning_) {
        lv_obj_add_state(switchWidget_, LV_STATE_CHECKED);
        lv_obj_add_state(transportBtn_, LV_STATE_DISABLED);
    }

    // Start HOME view, or show the transport chooser first (unless BLE was already running, or a
    // transport was chosen in an earlier session).
    currentMode_ = MidiMode::HOME;
    if (bleRunning_ || hasSavedTransport_) {
        buildHomeView(contentArea_);
    } else {
        showTransportSelect();
    }

    // 8Encoder (optional hardware - graceful no-op if not present)
    encoderInit(this);
}

void MidiController::teardown() {
    // Stop 8Encoder poll + turn off LEDs
    encoderStop(this);

    // Stop mode timers (including the BEATS UI timer)
    stopAllModeTimers();

    // Stop sequencer
    if (seq_) {
        seq_->stop();
        seq_.reset();
    }

    // MIDI transport + BLE radio cleanup (MediaKeys pattern).
    radioEnabling_ = false;
    stopBleMidi();
    stopUsbMidi();
    if (btDevice_) {
        unsubscribeBtEvents();
        if (radioWasOff_) bluetooth_set_radio_enabled(btDevice_, false);
        device_put(btDevice_);
        btDevice_ = nullptr;
    }
    radioWasOff_ = false;

    resetAllState();
    s_instance = nullptr;
}

void MidiController::resetAllState() {
    toolbar_         = nullptr;
    transportBtn_    = nullptr;
    transportIconLbl_ = nullptr;
    transportOverlay_ = nullptr;
    mainWrapper_     = nullptr;
    switchWidget_    = nullptr;
    contentArea_     = nullptr;
    currentViewRoot_ = nullptr;

    keys_  = {};  keys_.octave = 4;
                  for (auto& n : keys_.lastNote) n = -1;
    beats_ = {};  beats_.bpm = 120;
    xy_    = {};  xy_.xCC = 74; xy_.yCC = 71; xy_.xVal = 64; xy_.yVal = 64;
    arp_   = {};  arp_.bpm = 120; arp_.speedIdx = 1; arp_.pianoOctave = 4;
                  arp_.triggeredNote = -1; arp_.lastNote = -1;
    grid_  = {};  for (auto& n : grid_.pressedNote) n = -1;
    chord_ = {};  chord_.octave = 4;
    lfo_   = {};  lfo_.rate = 1.0f; lfo_.amount = 64; lfo_.ccTarget = 1; lfo_.lastValue = 64;
    zen_   = {};  zen_.octave = 4;
    drop_  = {};  drop_.octave = 4;
    rng_   = {};  rng_.bpm = 120; rng_.subdivision = 0;
                  rng_.minOctave = 3; rng_.maxOctave = 6; rng_.probability = 50;
    raga_  = {};  raga_.rootNote = 48; raga_.bpm = 120;
    dim_   = {};  dim_.equation = 1; dim_.a = 50; dim_.b = 50; dim_.c = 50; dim_.d = 50;
                  dim_.bpm = 120; dim_.t2 = 100; dim_.gran = 1.0f;
                  dim_.xOff = 64; dim_.yOff = 64; dim_.lastNote = -1;
    baby8_ = {};  baby8_.bpm = 120; baby8_.swing = 50; baby8_.lastNote = -1;
                  for (int i = 0; i < 8; i++) { baby8_.steps[i].note = 60 + i; baby8_.steps[i].velocity = 100; baby8_.steps[i].gate = true; }
                  for (int p = 0; p < 8; p++) for (int s = 0; s < 8; s++) { baby8_.patterns[p][s].note = 60 + s; baby8_.patterns[p][s].velocity = 100; baby8_.patterns[p][s].gate = true; }
    tb3po_ = {};  tb3po_.density = 8; tb3po_.bpm = 120; tb3po_.octave = 3; tb3po_.lastNote = -1;
    euclid_= {};  euclid_.bpm = 120;
}
