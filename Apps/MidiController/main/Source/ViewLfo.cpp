#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_timer.h>
#include <cstdio>
#include <cmath>

// ---------------------------------------------------------------------------
// LFO view - Low-frequency oscillator for CC/Pitch modulation
//
// Layout (flex column):
//   [row 1: < | LFO MOD | status dot]
//   [row 2: START/STOP | Rate -/val/Hz/+  | WAVE]
//   [row 3: Amt -/val/+  | Tgt CC -/val/+ | PITCH]
//   [waveform area (LV_EVENT_DRAW_MAIN)]
//   [value readout (flex_grow)]
// ---------------------------------------------------------------------------

static const char* WAVE_NAMES[] = { "SINE", "TRI", "SQR", "SAW" };


// ---------------------------------------------------------------------------
// Waveform computation
// ---------------------------------------------------------------------------

static float computeWave(int waveform, float phase) {
    switch (waveform) {
        case 0: return sinf(phase * (float)(2.0 * M_PI));
        case 1: return (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
        case 2: return (phase < 0.5f) ? 1.0f : -1.0f;
        case 3: return (2.0f * phase) - 1.0f;
        default: return 0.0f;
    }
}

static int waveToCCValue(float wave, int amount, int center = 64) {
    int val = center + (int)(wave * (float)amount * 0.5f);
    return val < 0 ? 0 : (val > 127 ? 127 : val);
}

// ---------------------------------------------------------------------------
// Waveform draw event
// ---------------------------------------------------------------------------

static void cbWaveDraw(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& lf = mc->lfo_;

    lv_obj_t*   obj   = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t ox = area.x1, oy = area.y1;
    int32_t cW = lv_area_get_width(&area);
    int32_t cH = lv_area_get_height(&area);

    lf.canvasW = (int)cW;
    lf.canvasH = (int)cH;

    // Background
    lv_draw_fill_dsc_t bg;
    lv_draw_fill_dsc_init(&bg);
    bg.color = lv_color_hex(0x080C12);
    bg.opa   = LV_OPA_COVER;
    lv_draw_fill(layer, &bg, &area);

    if (cW <= 1 || cH <= 1) return;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.opa = LV_OPA_COVER;

    // Center line
    line.color = lv_color_hex(0x1A2A40);
    line.width = 1;
    line.p1.x = (lv_value_precise_t)ox;
    line.p1.y = (lv_value_precise_t)(oy + cH / 2);
    line.p2.x = (lv_value_precise_t)(ox + cW - 1);
    line.p2.y = (lv_value_precise_t)(oy + cH / 2);
    lv_draw_line(layer, &line);

    // Waveform - initialize prevY from waveform value at phase 0 (not hardcoded cH/2)
    line.color = lv_color_hex(0x00DFFF);
    line.width = 1;
    float v0 = computeWave(lf.waveform, 0.0f);
    int prevY = (int)((1.0f - (v0 + 1.0f) * 0.5f) * (float)(cH - 1));
    prevY = prevY < 0 ? 0 : (prevY >= cH ? cH - 1 : prevY);
    for (int x = 1; x < cW; x++) {
        float ph  = (float)x / (float)cW;
        float val = computeWave(lf.waveform, ph);
        int y = (int)((1.0f - (val + 1.0f) * 0.5f) * (float)(cH - 1));
        y = y < 0 ? 0 : (y >= cH ? cH - 1 : y);
        line.p1.x = (lv_value_precise_t)(ox + x - 1);
        line.p1.y = (lv_value_precise_t)(oy + prevY);
        line.p2.x = (lv_value_precise_t)(ox + x);
        line.p2.y = (lv_value_precise_t)(oy + y);
        lv_draw_line(layer, &line);
        prevY = y;
    }
}

// ---------------------------------------------------------------------------
// LFO timer
// ---------------------------------------------------------------------------

static void cbLfoTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc || !mc->lfo_.running) return;
    auto& lf = mc->lfo_;

    uint64_t nowUs   = (uint64_t)esp_timer_get_time();
    uint64_t deltaUs = nowUs - lf.lastUpdateUs;
    lf.lastUpdateUs  = nowUs;

    float phaseInc = lf.rate * (float)deltaUs / 1000000.0f;
    lf.phase = fmodf(lf.phase + phaseInc, 1.0f);

    float wave = computeWave(lf.waveform, lf.phase);

    if (lf.pitchMode) {
        int bend = 8192 + (int)(wave * (float)lf.amount * 64.0f);
        bend = bend < 0 ? 0 : (bend > 16383 ? 16383 : bend);
        uint8_t lsb = bend & 0x7F;
        uint8_t msb = (bend >> 7) & 0x7F;
        if (lf.lastValue != bend / 128) {
            mc->sendMidi(0xE0, lsb, msb);
            lf.lastValue = bend / 128;
        }
    } else {
        int ccVal = waveToCCValue(wave, lf.amount);
        if (ccVal != lf.lastValue) {
            mc->sendMidi(0xB0, (uint8_t)lf.ccTarget, (uint8_t)ccVal);
            lf.lastValue = ccVal;
        }
    }

    lf.updateCounter++;
    if (lf.updateCounter >= 5) {
        lf.updateCounter = 0;
        if (lf.valueLbl) {
            char buf[12]; snprintf(buf, sizeof(buf), "Val: %d", lf.lastValue);
            lv_label_set_text(lf.valueLbl, buf);
        }
        if (lf.statusLbl) {
            lv_label_set_text(lf.statusLbl,
                lf.pitchMode ? "RUN PITCH" : "RUN CC");
        }
    }
}

// ---------------------------------------------------------------------------
// Control callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbStartStop(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& lf = mc->lfo_;
    lf.running = !lf.running;
    lf.lastUpdateUs = (uint64_t)esp_timer_get_time();
    if (lf.running) {
        if (!mc->lfoTimer_) {
            mc->lfoTimer_ = lv_timer_create(cbLfoTimer, 20, mc);
        }
    } else {
        if (mc->lfoTimer_) { lv_timer_delete(mc->lfoTimer_); mc->lfoTimer_ = nullptr; }
        if (lf.pitchMode) mc->sendMidi(0xE0, 0x00, 0x40);
    }
    if (lf.startBtn) {
        lv_obj_set_style_bg_color(lf.startBtn,
            lf.running ? lv_color_hex(0xFF2020) : lv_color_hex(0x00AA44), 0);
    }
    if (lf.startLbl) lv_label_set_text(lf.startLbl, lf.running ? "STOP" : "START");
    if (lf.statusLbl) lv_label_set_text(lf.statusLbl, lf.running ? "RUN" : "IDLE");
}

static void cbRateDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->lfo_.rate -= 0.1f;
    if (mc->lfo_.rate < 0.1f) mc->lfo_.rate = 0.1f;
    if (mc->lfo_.rateLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%.1f", mc->lfo_.rate);
        lv_label_set_text(mc->lfo_.rateLbl, buf);
    }
}
static void cbRateUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->lfo_.rate += 0.1f;
    if (mc->lfo_.rate > 10.0f) mc->lfo_.rate = 10.0f;
    if (mc->lfo_.rateLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%.1f", mc->lfo_.rate);
        lv_label_set_text(mc->lfo_.rateLbl, buf);
    }
}

static void cbWaveCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->lfo_.waveform = (mc->lfo_.waveform + 1) % 4;
    if (mc->lfo_.waveLbl) lv_label_set_text(mc->lfo_.waveLbl, WAVE_NAMES[mc->lfo_.waveform]);
    // Invalidate canvas to trigger redraw
    if (mc->lfo_.canvas) lv_obj_invalidate(mc->lfo_.canvas);
}

static void cbAmountDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->lfo_.amount -= 5; if (mc->lfo_.amount < 0) mc->lfo_.amount = 0;
    if (mc->lfo_.amountLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "Amt:%d", mc->lfo_.amount);
        lv_label_set_text(mc->lfo_.amountLbl, buf);
    }
}
static void cbAmountUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->lfo_.amount += 5; if (mc->lfo_.amount > 127) mc->lfo_.amount = 127;
    if (mc->lfo_.amountLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "Amt:%d", mc->lfo_.amount);
        lv_label_set_text(mc->lfo_.amountLbl, buf);
    }
}

static void cbCCDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || mc->lfo_.pitchMode) return;
    mc->lfo_.ccTarget = (mc->lfo_.ccTarget - 1 + 128) % 128;
    if (mc->lfo_.ccLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "CC %d", mc->lfo_.ccTarget);
        lv_label_set_text(mc->lfo_.ccLbl, buf);
    }
}
static void cbCCUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || mc->lfo_.pitchMode) return;
    mc->lfo_.ccTarget = (mc->lfo_.ccTarget + 1) % 128;
    if (mc->lfo_.ccLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "CC %d", mc->lfo_.ccTarget);
        lv_label_set_text(mc->lfo_.ccLbl, buf);
    }
}

static void cbPitchToggle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& lf = mc->lfo_;
    bool wasPitch = lf.pitchMode;
    lf.pitchMode = !lf.pitchMode;
    // Leaving pitch mode must release the bend, otherwise the receiver stays
    // detuned. Force the next CC update as well so the CC target is refreshed.
    if (wasPitch) mc->sendMidi(0xE0, 0x00, 0x40);
    lf.lastValue = -1;
    if (mc->lfo_.ccLbl) {
        if (mc->lfo_.pitchMode) {
            lv_label_set_text(mc->lfo_.ccLbl, "PITCH");
            lv_obj_set_style_text_color(mc->lfo_.ccLbl, lv_color_hex(0xFF8000), 0);
        } else {
            char buf[8]; snprintf(buf, sizeof(buf), "CC %d", mc->lfo_.ccTarget);
            lv_label_set_text(mc->lfo_.ccLbl, buf);
            lv_obj_set_style_text_color(mc->lfo_.ccLbl, lv_color_hex(0x00DFFF), 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

static void cbLfoRootDelete(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& lf = mc->lfo_;

    // Stop and delete the LFO timer so it cannot fire after widgets are gone
    if (mc->lfoTimer_) {
        lv_timer_delete(mc->lfoTimer_);
        mc->lfoTimer_ = nullptr;
    }
    // Do not leave a pitch-bend receiver detuned when leaving the view.
    if (lf.pitchMode) mc->sendMidi(0xE0, 0x00, 0x40);
    lf.running = false;

    // Null all stored LVGL pointers
    lf.canvas    = nullptr;
    lf.statusLbl = nullptr;
    lf.rateLbl   = nullptr;
    lf.amountLbl = nullptr;
    lf.ccLbl     = nullptr;
    lf.waveLbl   = nullptr;
    lf.valueLbl  = nullptr;
    lf.startBtn  = nullptr;
    lf.startLbl  = nullptr;
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildLfoView(MidiController* mc, lv_obj_t* parent) {
    auto& lf = mc->lfo_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int rowH    = 88;
    int waveH   = 160;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_add_event_cb(root, cbLfoRootDelete, LV_EVENT_DELETE, mc);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 1, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    auto makeRow = [&]() -> lv_obj_t* { return mcRow(root); };

    auto makeBtn = [&](lv_obj_t* par, const char* txt, int w, lv_event_cb_t cb) -> lv_obj_t* {
        return mcSmallBtn(par, txt, w, cb, mc);
    };

    auto makeLbl = [&](lv_obj_t* par, const char* txt, int w, lv_color_t col) -> lv_obj_t* {
        return mcValue(par, txt, w, col);
    };

    // -- Row 1: [<] [LFO MOD] [status] ----------------------------------------
    lv_obj_t* row1 = makeRow();
    mcBackBtn(row1, cbBack, mc, MC_W_M);
    lv_obj_t* title = lv_label_create(row1);
    lv_label_set_text(title, "LFO MOD");
    lv_obj_set_style_text_font(title, fontNormal, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xCC44FF), 0);
    lv_obj_set_flex_grow(title, 1);
    lf.statusLbl = lv_label_create(row1);
    lv_label_set_text(lf.statusLbl, "IDLE");
    lv_obj_set_style_text_font(lf.statusLbl, fontSmall, 0);
    lv_obj_set_style_text_color(lf.statusLbl, MC_LABEL(), 0);

    // -- Row 2: START/STOP | Rate -/val/Hz/+ | WAVE ---------------------------
    lv_obj_t* row2 = makeRow();
    lf.startBtn = mcButton(row2);
    lv_obj_set_size(lf.startBtn, 224, MC_BTN_H);
    lv_obj_set_style_bg_color(lf.startBtn, lv_color_hex(0x00AA44), 0);
    lv_obj_add_event_cb(lf.startBtn, cbStartStop, LV_EVENT_CLICKED, mc);
    lf.startLbl = lv_label_create(lf.startBtn);
    lv_label_set_text(lf.startLbl, "START");
    lv_obj_set_style_text_font(lf.startLbl, fontSmall, 0);
    lv_obj_center(lf.startLbl);

    lv_obj_t* rHdr = lv_label_create(row2);
    lv_label_set_text(rHdr, " Rate:");
    lv_obj_set_style_text_font(rHdr, fontSmall, 0);
    lv_obj_set_style_text_color(rHdr, lv_color_hex(0x666688), 0);
    makeBtn(row2, "-", 96, cbRateDown);
    char rbuf[8]; snprintf(rbuf, sizeof(rbuf), "%.1f", lf.rate);
    lf.rateLbl = makeLbl(row2, rbuf, 144, lv_color_hex(0x00DFFF));
    lv_obj_t* rHz = lv_label_create(row2);
    lv_label_set_text(rHz, "Hz");
    lv_obj_set_style_text_font(rHz, fontSmall, 0);
    lv_obj_set_style_text_color(rHz, lv_color_hex(0x666688), 0);
    makeBtn(row2, "+", 96, cbRateUp);

    lv_obj_t* waveBtn = mcButton(row2);
    lv_obj_set_size(waveBtn, 208, MC_BTN_H);
    lv_obj_add_event_cb(waveBtn, cbWaveCycle, LV_EVENT_CLICKED, mc);
    lf.waveLbl = lv_label_create(waveBtn);
    lv_label_set_text(lf.waveLbl, WAVE_NAMES[lf.waveform]);
    lv_obj_set_style_text_font(lf.waveLbl, fontSmall, 0);
    lv_obj_center(lf.waveLbl);

    // -- Row 3: Amount -/val/+ | CC -/val/+ | PITCH ---------------------------
    lv_obj_t* row3 = makeRow();

    lv_obj_t* aHdr = lv_label_create(row3);
    lv_label_set_text(aHdr, "Amt:");
    lv_obj_set_style_text_font(aHdr, fontSmall, 0);
    lv_obj_set_style_text_color(aHdr, lv_color_hex(0x666688), 0);
    makeBtn(row3, "-", 96, cbAmountDown);
    char abuf[10]; snprintf(abuf, sizeof(abuf), "Amt:%d", lf.amount);
    lf.amountLbl = makeLbl(row3, abuf, 208, lv_color_hex(0xFFE000));
    makeBtn(row3, "+", 96, cbAmountUp);

    lv_obj_t* cHdr = lv_label_create(row3);
    lv_label_set_text(cHdr, " Tgt:");
    lv_obj_set_style_text_font(cHdr, fontSmall, 0);
    lv_obj_set_style_text_color(cHdr, lv_color_hex(0x666688), 0);
    makeBtn(row3, "-", 96, cbCCDown);
    char ccbuf[10]; snprintf(ccbuf, sizeof(ccbuf), "CC %d", lf.ccTarget);
    lf.ccLbl = makeLbl(row3, ccbuf, 192, lv_color_hex(0x00DFFF));
    makeBtn(row3, "+", 96, cbCCUp);

    lv_obj_t* pitchBtn = mcButton(row3);
    lv_obj_set_size(pitchBtn, 192, MC_BTN_H);
    lv_obj_set_style_bg_color(pitchBtn, lv_color_hex(0x220033), 0);
    lv_obj_add_event_cb(pitchBtn, cbPitchToggle, LV_EVENT_CLICKED, mc);
    lv_obj_t* pitchLbl = lv_label_create(pitchBtn);
    lv_label_set_text(pitchLbl, "PITCH");
    lv_obj_set_style_text_font(pitchLbl, fontSmall, 0);
    lv_obj_center(pitchLbl);

    // -- Spacer row (pushes waveform area down) --------------------------------
    lv_obj_t* spacer = mcContainer(root);
    lv_obj_set_size(spacer, LV_PCT(100), rowH);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    // -- Waveform display (draw event) -----------------------------------------
    lv_obj_t* waveArea = mcContainer(root);
    lv_obj_set_size(waveArea, LV_PCT(100), waveH);
    lv_obj_set_style_bg_color(waveArea, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(waveArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(waveArea, 0, 0);
    lv_obj_set_style_pad_all(waveArea, 0, 0);
    lv_obj_set_scrollable(waveArea, false);
    lv_obj_add_event_cb(waveArea, cbWaveDraw, LV_EVENT_DRAW_MAIN, mc);
    lf.canvas = waveArea;

    // -- Value readout ---------------------------------------------------------
    lv_obj_t* footer = mcContainer(root);
    lv_obj_set_width(footer, LV_PCT(100));
    lv_obj_set_flex_grow(footer, 1);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 16, 0);
    lv_obj_set_scrollable(footer, false);

    lf.valueLbl = lv_label_create(footer);
    lv_label_set_text(lf.valueLbl, "Val: 64");
    lv_obj_set_style_text_font(lf.valueLbl, fontNormal, 0);
    lv_obj_set_style_text_color(lf.valueLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_center(lf.valueLbl);
}
