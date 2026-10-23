#include "MidiController.h"
#include "LcdDisplay.h"
#include "GroveLookup.h"
#include <UnitPaHub.h>
#include <Unit8Encoder.h>

#include <lvgl.h>
#include <cstring>
#include <esp_log.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// M5Stack 8Encoder (V2, addr 0x41) + LCD Unit (addr 0x3E)
// Both are on PaHub (addr 0x70) on the grove port in I2C mode (Tab5 Port A, SDA=53 SCL=54).
// Looked up via findGroveI2cDevice() (GroveLookup.h) rather than a hardcoded device name: the
// grove port device itself ("grove0") is a mode-switchable I2C/UART router (GROVE_TYPE), not an
// I2C controller - passing it directly into i2c_controller_*() calls crashes (wrong driver data
// layout). The actual I2C-master child device it spawns in I2C mode is named "grove0_i2c", but
// even that name isn't hardcoded here since grove port naming isn't guaranteed.
//
//   Usually 8Encoder on PaHub CH4 and LCD on CH5, but encoderInit() probes
//   every channel (0-5) and accepts either unit on any free port.
//
// Per-mode encoder → parameter mapping:
//   EUCLID     : knob 0 → sel.steps; knob 1 → sel.events; knob 2 → sel.rot; knob 3 → sel.note
//                knob 4 → BPM; knobs 5-7 → steps for voices 6-8; button i → select voice i
//   BABY8      : knob i (0-7) → note for step i;    button i → toggle gate i
//   DIMENSIONS : knob 0-3 → A/B/C/D; knob 4 → equation; knob 5 → BPM; knob 6 → oct; knob 7 → rnd
//                button 0 → play/stop; button 1 → reset
//   TB3PO      : knob 0 → density; knob 1 → BPM; knob 2 → key; knob 3 → octave
//                knob 4 → scale; knob 5 → regen-on-turn; button 0 → play/stop; button 1 → regen
//   RAGA       : knob 0 → raga; knob 1 → tala; knob 2 → root; knob 3 → BPM
//                button 0 → play/stop; button 1 → drone toggle
// ---------------------------------------------------------------------------

static constexpr auto* TAG = "EncoderInput";
static constexpr int POLL_MS = 100;

static UnitPaHub s_hub;
static Unit8Encoder s_enc;
static Device* s_i2cDev = nullptr;
// Discovered in encoderInit() by probing every PaHub channel (0-5), the 8Encoder can be plugged into any port on the hub.
static uint8_t s_encChannel = UnitPaHub::NO_CHANNEL;

// ---------------------------------------------------------------------------
// LED colour tables
// ---------------------------------------------------------------------------

// flushLeds() expects LED_COUNT (9) entries: 8 encoder LEDs + the switch LED.
// The app leaves the switch LED off, so the ninth entry is always 0.
static constexpr uint32_t EUCLID_COLORS[Unit8Encoder::LED_COUNT] = {
    0xFF4040, 0xFF9900, 0x44DD44, 0x44BBFF,
    0xFF44FF, 0xFFDD00, 0x00FFCC, 0xFF8888,
    0x000000
};
static constexpr uint32_t RAGA_COLORS[Unit8Encoder::LED_COUNT] = {
    0xFF8800, 0xFF6600, 0x00FFCC, 0xFFE000,
    0x000000, 0x000000, 0x000000, 0x000000,
    0x000000
};
static constexpr uint32_t DIM_COLORS[Unit8Encoder::LED_COUNT] = {
    0x88DDFF, 0x88FFCC, 0xFFCC88, 0xFF88CC,
    0xFF8844, 0xFFE000,
    0xAAFF88, 0xFFAA44,
    0x000000
};

// ---------------------------------------------------------------------------
// PaHub channel helpers
// ---------------------------------------------------------------------------

static bool selectEncoder() {
    return s_encChannel != UnitPaHub::NO_CHANNEL && s_hub.select(s_encChannel);
}

// ---------------------------------------------------------------------------
// Per-mode dispatch (encoder side only - LED flush goes through Unit8Encoder)
// ---------------------------------------------------------------------------

static void dispatchEuclid(MidiController* mc) {
    auto& es = mc->euclid_;
    int32_t deltas[8] = {};
    uint8_t buttons[8] = {};
    if (!selectEncoder()) return;
    if (!s_enc.readAll(deltas, buttons)) return;

    bool changed = false;
    auto& sv = es.voice[es.selectedVoice];

    if (deltas[0]) { int s = sv.steps   + deltas[0]; s = std::max(1,std::min(32,s)); sv.steps = s; if (sv.events > sv.steps) sv.events = sv.steps; changed = true; }
    if (deltas[1]) { int e = sv.events  + deltas[1]; e = std::max(0,std::min(sv.steps,e)); sv.events = e; changed = true; }
    if (deltas[2]) { int safeSteps = std::max(1, sv.steps); int r = sv.rotation + deltas[2]; r = ((r % safeSteps) + safeSteps) % safeSteps; sv.rotation = r; changed = true; }
    if (deltas[3]) { int n = sv.note    + deltas[3]; n = std::max(0,std::min(127,n)); sv.note = n; changed = true; }
    if (deltas[4]) { int b = es.bpm     + deltas[4]; b = std::max(40,std::min(280,b)); es.bpm = b; if (es.bpmLbl) { char buf[8]; snprintf(buf,sizeof(buf),"%d",b); lv_label_set_text(es.bpmLbl,buf); } }
    for (int i = 5; i <= 7; i++) {
        if (deltas[i]) { auto& v = es.voice[i]; int s = v.steps + deltas[i]; s = std::max(1,std::min(32,s)); v.steps = s; if (v.events > v.steps) v.events = v.steps; changed = true; }
    }
    for (int i = 0; i < 8; i++) {
        bool btn = (buttons[i] != 0);
        if (btn && !mc->encoder_.btnPrev[i]) { es.selectedVoice = i; changed = true; }
        mc->encoder_.btnPrev[i] = btn;
    }
    if (changed) euclidOnEncoderUpdate(mc);

    uint32_t pending[Unit8Encoder::LED_COUNT] = {};
    for (int i = 0; i < 8; i++) {
        uint32_t c = EUCLID_COLORS[i];
        pending[i] = (i == es.selectedVoice) ? c : (c >> 2) & 0x3F3F3F;
    }
    selectEncoder();
    s_enc.flushLeds(pending);
    lcdUpdate(mc);
}

static void dispatchBaby8(MidiController* mc) {
    auto& bs = mc->baby8_;
    int32_t deltas[8] = {};
    uint8_t buttons[8] = {};
    if (!selectEncoder()) return;
    if (!s_enc.readAll(deltas, buttons)) return;

    static const char* NOTE_NAMES[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    bool stepChanged = false;
    for (int i = 0; i < 8; i++) {
        if (deltas[i]) {
            int n = bs.steps[i].note + deltas[i];
            n = std::max(0, std::min(127, n));
            bs.steps[i].note = n;
            if (bs.noteLbl[i]) {
                char buf[8]; snprintf(buf,sizeof(buf),"%s%d",NOTE_NAMES[n%12],n/12-1);
                lv_label_set_text(bs.noteLbl[i], buf);
            }
            stepChanged = true;
        }
        bool btn = (buttons[i] != 0);
        if (btn && !mc->encoder_.btnPrev[i]) {
            bs.steps[i].gate = !bs.steps[i].gate;
            stepChanged = true;
        }
        mc->encoder_.btnPrev[i] = btn;
    }
    // Encoder edits must repaint the step buttons immediately; the timer only
    // refreshes them while the sequencer is running.
    if (stepChanged) baby8UpdateStepColors(mc);

    uint32_t pending[Unit8Encoder::LED_COUNT] = {};
    for (int i = 0; i < 8; i++)
        pending[i] = (bs.playing && i == bs.currentStep) ? 0xFFFFFF
                                                         : (bs.steps[i].gate ? 0x004400 : 0x440000);
    selectEncoder();
    s_enc.flushLeds(pending);
    lcdUpdate(mc);
}

static void dispatchDimensions(MidiController* mc) {
    auto& ds = mc->dim_;
    int32_t deltas[8] = {};
    uint8_t buttons[8] = {};
    if (!selectEncoder()) return;
    if (!s_enc.readAll(deltas, buttons)) return;

    float* params[4] = {&ds.a,&ds.b,&ds.c,&ds.d};
    lv_obj_t** lbls[4] = {&ds.aLbl,&ds.bLbl,&ds.cLbl,&ds.dLbl};
    static const char* NAMES[4] = {"A","B","C","D"};
    for (int i = 0; i < 4; i++) {
        if (deltas[i]) {
            float v = *params[i] + deltas[i];
            v = std::max(0.0f, std::min(100.0f, v));
            *params[i] = v;
            if (*lbls[i]) { char b[12]; snprintf(b,sizeof(b),"%s:%.0f",NAMES[i],v); lv_label_set_text(*lbls[i],b); }
        }
    }
    if (deltas[4]) { int eq = ds.equation + deltas[4]; eq = std::max(1,std::min(20,eq)); ds.equation = eq; if (ds.eqLbl) { char b[12]; snprintf(b,sizeof(b),"Eq:%d",eq); lv_label_set_text(ds.eqLbl,b); } }
    if (deltas[5]) { int bpm = ds.bpm + deltas[5]; bpm = std::max(40,std::min(280,bpm)); ds.bpm = bpm; if (ds.bpmLbl) { char b[8]; snprintf(b,sizeof(b),"%d",bpm); lv_label_set_text(ds.bpmLbl,b); } }
    if (deltas[6]) dimChangeOctave(mc, deltas[6]);
    if (deltas[7]) dimChangeRandom(mc, deltas[7]);

    for (int i = 0; i < 2; i++) {
        bool btn = (buttons[i] != 0);
        if (btn && !mc->encoder_.btnPrev[i]) { if (i==0) dimTogglePlay(mc); else dimReset(mc); }
        mc->encoder_.btnPrev[i] = btn;
    }
    selectEncoder();
    s_enc.flushLeds(DIM_COLORS);
    lcdUpdate(mc);
}

static void dispatchTb3po(MidiController* mc) {
    auto& ts = mc->tb3po_;
    int32_t deltas[8] = {};
    uint8_t buttons[8] = {};
    if (!selectEncoder()) return;
    if (!s_enc.readAll(deltas, buttons)) return;

    if (deltas[0]) {
        int d = ts.density + deltas[0];
        d = std::max(0, std::min(16, d));
        if (d != ts.density) {
            ts.density = d;
            if (ts.densityLbl) { char b[8]; snprintf(b, sizeof(b), "D:%d", d); lv_label_set_text(ts.densityLbl, b); }
            // Match the touch UI: a density change also draws a fresh seed and pattern.
            tb3Regen(mc);
        }
    }
    if (deltas[1]) { int bpm = ts.bpm + deltas[1]; bpm = std::max(40,std::min(280,bpm)); ts.bpm = bpm; if (ts.bpmLbl) { char b[8]; snprintf(b,sizeof(b),"%d",bpm); lv_label_set_text(ts.bpmLbl,b); } }
    bool keyOrOctaveChanged = false;
    if (deltas[2]) { ts.key = ((ts.key + deltas[2]) % 12 + 12) % 12; static const char* KS[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}; if (ts.keyLbl) lv_label_set_text(ts.keyLbl, KS[ts.key]); keyOrOctaveChanged = true; }
    if (deltas[3]) { int oct = ts.octave + deltas[3]; oct = std::max(1,std::min(6,oct)); ts.octave = oct; keyOrOctaveChanged = true; }
    if (deltas[4]) tb3ChangeScale(mc, deltas[4]);
    if (deltas[5]) tb3Regen(mc);
    // The piano roll shows resolved note names; key/octave changes need a repaint
    // even when the sequencer is stopped.
    if (keyOrOctaveChanged) tb3Redraw(mc);
    for (int i = 0; i < 2; i++) {
        bool btn = (buttons[i] != 0);
        if (btn && !mc->encoder_.btnPrev[i]) { if (i==0) tb3TogglePlay(mc); else tb3Regen(mc); }
        mc->encoder_.btnPrev[i] = btn;
    }

    uint32_t pending[Unit8Encoder::LED_COUNT] = {};
    for (int i = 0; i < 8; i++)
        pending[i] = (ts.playing && i == (ts.currentStep / 4)) ? 0xFF6600 : 0x221100;
    selectEncoder();
    s_enc.flushLeds(pending);
    lcdUpdate(mc);
}

static void dispatchRaga(MidiController* mc) {
    auto& rs = mc->raga_;
    int32_t deltas[8] = {};
    uint8_t buttons[8] = {};
    if (!selectEncoder()) return;
    if (!s_enc.readAll(deltas, buttons)) return;

    static const char* RAGA_NAMES[] = {"Bhairavi","Lalit","Bhupali","Todi","Madhuvanti","Meghmalhar","Yaman","Malkauns"};
    static const char* TALA_NAMES[] = {"Teental","Rupak","Jhaptal"};

    if (deltas[0]) { rs.raga = ((rs.raga + deltas[0]) % 8 + 8) % 8; if (rs.ragaLbl) lv_label_set_text(rs.ragaLbl, RAGA_NAMES[rs.raga]); if (rs.ragaBigLbl) lv_label_set_text(rs.ragaBigLbl, RAGA_NAMES[rs.raga]); }
    if (deltas[1]) { rs.tala = ((rs.tala + deltas[1]) % 3 + 3) % 3; rs.talaBeat = 0; if (rs.talaLbl) lv_label_set_text(rs.talaLbl, TALA_NAMES[rs.tala]); if (rs.talaBigLbl) lv_label_set_text(rs.talaBigLbl, TALA_NAMES[rs.tala]); }
    if (deltas[2]) { int r = rs.rootNote + deltas[2]; r = std::max(24,std::min(96,r)); rs.rootNote = r; if (rs.rootLbl) { static const char* N[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}; char b[8]; snprintf(b,sizeof(b),"%s%d",N[r%12],r/12-1); lv_label_set_text(rs.rootLbl,b); } }
    if (deltas[3]) { int bpm = rs.bpm + deltas[3]; bpm = std::max(40,std::min(240,bpm)); rs.bpm = bpm; if (rs.bpmLbl) { char b[8]; snprintf(b,sizeof(b),"%d",bpm); lv_label_set_text(rs.bpmLbl,b); } }
    for (int i = 0; i < 2; i++) {
        bool btn = (buttons[i] != 0);
        if (btn && !mc->encoder_.btnPrev[i]) { if (i==0) ragaTogglePlay(mc); else ragaToggleDrone(mc); }
        mc->encoder_.btnPrev[i] = btn;
    }
    selectEncoder();
    s_enc.flushLeds(RAGA_COLORS);
    lcdUpdate(mc);
}

// ---------------------------------------------------------------------------
// Poll timer
// ---------------------------------------------------------------------------

static void cbEncoderPoll(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    // With no encoder, this timer still refreshes a detected LCD unit.
    if (!mc->encoder_.present) {
        lcdUpdate(mc);
        return;
    }
    if (!s_hub.isPresent()) return;

    // After a mode switch, do one silent read to seed btnPrev with the live
    // button state - prevents a physically-held button from registering as a
    // fresh press in the new mode's dispatch.
    if (mc->encoder_.btnSeedNext) {
        mc->encoder_.btnSeedNext = false;
        int32_t deltas[8] = {};
        uint8_t buttons[8] = {};
        if (selectEncoder()) {
            s_enc.readAll(deltas, buttons);
            for (int i = 0; i < 8; i++)
                mc->encoder_.btnPrev[i] = (buttons[i] != 0);
        }
        return;
    }

    switch (mc->currentMode_) {
        case MidiMode::EUCLID:     dispatchEuclid    (mc); break;
        case MidiMode::BABY8:      dispatchBaby8     (mc); break;
        case MidiMode::DIMENSIONS: dispatchDimensions(mc); break;
        case MidiMode::TB3PO:      dispatchTb3po     (mc); break;
        case MidiMode::RAGA:       dispatchRaga      (mc); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void encoderSetLed(MidiController* mc, uint8_t idx, uint32_t rgb) {
    if (!mc->encoder_.present || idx >= 8) return;
    if (!s_hub.isPresent()) return;
    selectEncoder();
    s_enc.setLed(idx, rgb);
    // sync cache
    mc->encoder_.ledColor[idx] = rgb;
}

void encoderSetAllLeds(MidiController* mc, uint32_t rgb) {
    if (!mc->encoder_.present) return;
    if (!s_hub.isPresent()) return;
    selectEncoder();
    s_enc.setAllLeds(rgb);
    for (int i = 0; i < 8; i++) mc->encoder_.ledColor[i] = rgb;
}

void encoderInit(MidiController* mc) {
    if (mc->encoderTimer_) return;

    Device* dev = findGroveI2cDevice();
    if (!dev) {
        ESP_LOGE(TAG, "grove0_i2c not ready");
        mc->encoder_.present = false;
        return;
    }
    s_i2cDev = dev;

    // Init PaHub
    if (!s_hub.begin(dev)) {
        ESP_LOGW(TAG, "PaHub not found - no encoder or LCD");
        mc->encoder_.present = false;
        s_i2cDev = nullptr;
        return;
    }

    // Find the 8Encoder by probing every PaHub channel - Unit8Encoder::begin()
    // itself probes the I2C address and returns false if nothing answers, so trying each
    // channel in turn and keeping the first hit is safe/cheap (6 channels, worst case).
    s_encChannel = UnitPaHub::NO_CHANNEL;
    for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS; ch++) {
        if (!s_hub.select(ch)) continue;
        if (s_enc.begin(dev)) {
            s_encChannel = ch;
            break;
        }
    }
    if (s_encChannel == UnitPaHub::NO_CHANNEL) {
        ESP_LOGW(TAG, "8Encoder not found on any PaHub channel");
        mc->encoder_.present = false;
        // Still try LCD even without encoder
    } else {
        mc->encoder_.present = true;
        memset(mc->encoder_.ledColor, 0, sizeof(mc->encoder_.ledColor));
        ESP_LOGI(TAG, "8Encoder ready on PaHub CH%d", (int)s_encChannel);
    }

    // Find the LCD the same way (LcdDisplay.cpp's own channel scan).
    lcdInit(&s_hub);
    lcdSetMode(mc->currentMode_);

    if (mc->encoder_.present || lcdIsPresent()) {
        mc->encoderTimer_ = lv_timer_create(cbEncoderPoll, POLL_MS, mc);
    }
}

void encoderStop(MidiController* mc) {
    if (mc->encoderTimer_) {
        lv_timer_delete(mc->encoderTimer_);
        mc->encoderTimer_ = nullptr;
    }
    if (mc->encoder_.present && s_hub.isPresent()) {
        selectEncoder();
        s_enc.setAllLeds(0x000000);
    }
    lcdStop();
    memset(mc->encoder_.ledColor, 0, sizeof(mc->encoder_.ledColor));
    mc->encoder_.present = false;
    s_i2cDev = nullptr;
    s_encChannel = UnitPaHub::NO_CHANNEL;
}
