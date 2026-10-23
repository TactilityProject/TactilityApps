#include "MidiController.h"
#include "LcdDisplay.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// EUCLID view - 8-voice Euclidean rhythm sequencer
//
// (toolbar ~40px, statusbar ~20px → content ~1280×660)
//
// Layout (flex column, fills content area):
//   TOP ROW  (88px, full width):  < | PLAY | BPM- N BPM+ | TRIP ON/OFF
//   BODY     (flex_grow, flex row):
//     LEFT  (flex_grow):   canvas - 8 concentric rings + sweep line
//     RIGHT (500px):       controls stacked in flex column
//       ├ voice tab row: 8 equal tabs
//       ├ voice name label (large, colored)
//       └ param rows (flex_grow each): Steps / Events / Rotation / Note
// ---------------------------------------------------------------------------

// Fixed pixel constants
static constexpr int ROW_H    = 88;   // matches ViewKeys rowH
static constexpr int COL_W    = 500;
static constexpr int CHROME_H = 60;   // toolbar (~40px) + statusbar (~20px)
static constexpr int BTN_H    = 64;   // matches ViewKeys btnH (rowH-24)
static constexpr int BTN_W_SM = 88;   // small button (< >)
static constexpr int BTN_W_MD = 144;  // medium button (PLAY)
static constexpr int BTN_W_LG = 88;   // back button
static constexpr int LBL_W    = 100;  // value label width
static constexpr int PAD      = 12;   // row padding / gap - matches ViewKeys

// Per-voice accent colors (8 voices)
static constexpr uint32_t VOICE_COLORS[8] = {
    0xFF4040, 0xFF9900, 0x44DD44, 0x44BBFF,
    0xFF44FF, 0xFFDD00, 0x00FFCC, 0xFF8888
};

// ---------------------------------------------------------------------------
// Bucket (Bresenham-style) Euclidean pattern generator
// ---------------------------------------------------------------------------

static void euclidGenPattern(MidiController::EuclidState::Voice& v) {
    memset(v.pattern, 0, sizeof(v.pattern));
    if (v.events == 0 || v.steps == 0) return;
    int events = std::min(v.events, v.steps);
    // Pre-fill the accumulator so the first event lands on step 0. Starting at
    // zero instead delays the first event and produces uneven gaps for some
    // pairs (for example E(3,8) became 00100110 rather than 10010010).
    int bucket = v.steps - events;
    for (int i = 0; i < v.steps; i++) {
        bucket += events;
        if (bucket >= v.steps) {
            bucket -= v.steps;
            int pos = (i + v.rotation) % v.steps;
            if (pos < 0) pos += v.steps;
            v.pattern[pos] = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Canvas draw
// ---------------------------------------------------------------------------

static void euclidDrawCanvas(MidiController::EuclidState& es) {
    if (!es.canvas) return;
    lv_obj_clean(es.canvas);

    int W = lv_obj_get_width(es.canvas);
    int H = lv_obj_get_height(es.canvas);
    if (W <= 4 || H <= 4) {
        // Layout hasn't settled yet - use stored estimates
        W = es.canvasW;
        H = es.canvasH;
    }
    if (W <= 0 || H <= 0) return;

    int cx = W / 2;
    int cy = H / 2;
    int maxR = std::min(cx, cy) - 8;
    if (maxR < 20) return;
    int ringSpacing = maxR / 9;  // 8 rings + small gap from center

    for (int vi = 0; vi < 8; vi++) {
        const auto& v = es.voice[vi];
        if (v.steps == 0) continue;
        int r = maxR - vi * ringSpacing;
        if (r < 8) continue;

        // Selected voice: draw a subtle ring outline behind the dots
        if (vi == es.selectedVoice) {
            lv_obj_t* ring = mcContainer(es.canvas);
            lv_obj_set_size(ring, r * 2 + 8, r * 2 + 8);
            lv_obj_set_pos(ring, cx - r - 4, cy - r - 4);
            lv_obj_set_ignore_layout(ring, true);
            lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(ring, 2, 0);
            lv_obj_set_style_border_color(ring, lv_color_hex(VOICE_COLORS[vi]), 0);
            lv_obj_set_style_border_opa(ring, LV_OPA_50, 0);
            lv_obj_set_scrollable(ring, false);
            lv_obj_set_clickable(ring, false);
        }

        for (int step = 0; step < v.steps; step++) {
            float angle = (float)(2.0 * M_PI * step) / v.steps - (float)(M_PI / 2.0);
            int px = cx + (int)(cosf(angle) * r);
            int py = cy + (int)(sinf(angle) * r);

            bool active    = v.pattern[step];
            bool isCurrent = (es.playing && step == (es.currentStep % v.steps));

            uint32_t col;
            if (isCurrent && active)   col = 0xFFFFFF;
            else if (isCurrent)        col = 0x44BBFF;
            else if (active)           col = VOICE_COLORS[vi];
            else                       col = 0x1A2030;

            int dotR = active ? 7 : 4;
            lv_obj_t* dot = mcContainer(es.canvas);
            lv_obj_set_size(dot, dotR * 2, dotR * 2);
            lv_obj_set_pos(dot, px - dotR, py - dotR);
            lv_obj_set_ignore_layout(dot, true);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(col), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_scrollable(dot, false);
            lv_obj_set_clickable(dot, false);
        }
    }

    // Sweep line - radial from center to edge
    if (es.playing && es.voice[0].steps > 0) {
        int steps = es.voice[0].steps;
        float angle = (float)(2.0 * M_PI * (es.currentStep % steps)) / steps - (float)(M_PI / 2.0);
        int ex = cx + (int)(cosf(angle) * maxR);
        int ey = cy + (int)(sinf(angle) * maxR);
        static lv_point_precise_t pts[2];
        pts[0] = {(lv_value_precise_t)cx, (lv_value_precise_t)cy};
        pts[1] = {(lv_value_precise_t)ex, (lv_value_precise_t)ey};
        lv_obj_t* ln = lv_line_create(es.canvas);
        lv_line_set_points(ln, pts, 2);
        lv_obj_set_ignore_layout(ln, true);
        lv_obj_set_style_line_color(ln, lv_color_hex(0x2244AA), 0);
        lv_obj_set_style_line_opa(ln, LV_OPA_60, 0);
        lv_obj_set_style_line_width(ln, 2, 0);
        lv_obj_set_clickable(ln, false);
    }
}

// ---------------------------------------------------------------------------
// Update param labels for selected voice
// ---------------------------------------------------------------------------

static void euclidUpdateParamLabels(MidiController::EuclidState& es) {
    const auto& v = es.voice[es.selectedVoice];
    if (es.stepsLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "S:%d", v.steps);
        lv_label_set_text(es.stepsLbl, buf);
    }
    if (es.eventsLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "E:%d", v.events);
        lv_label_set_text(es.eventsLbl, buf);
    }
    if (es.rotLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "R:%d", v.rotation);
        lv_label_set_text(es.rotLbl, buf);
    }
    if (es.noteLbl) {
        char buf[8]; midiNoteName(v.note, buf, sizeof(buf));
        lv_label_set_text(es.noteLbl, buf);
    }
    if (es.voiceLbl) {
        char buf[24]; snprintf(buf, sizeof(buf), "Voice %d", es.selectedVoice + 1);
        lv_label_set_text(es.voiceLbl, buf);
        lv_obj_set_style_text_color(es.voiceLbl, lv_color_hex(VOICE_COLORS[es.selectedVoice]), 0);
    }
}

// ---------------------------------------------------------------------------
// Called by EncoderInput after modifying voice params via knob/button
// ---------------------------------------------------------------------------

void euclidOnEncoderUpdate(MidiController* mc) {
    auto& es = mc->euclid_;
    // Regenerate pattern and refresh UI for all 8 voices (knobs may have
    // changed any of them, and the canvas shows all voices simultaneously)
    for (int i = 0; i < 8; i++) {
        euclidGenPattern(es.voice[i]);
    }
    euclidUpdateParamLabels(es);
    euclidDrawCanvas(es);
    // Update voice tab button opacities (selected = full, others = dim)
    if (es.voiceTabRow) {
        uint32_t childCnt = lv_obj_get_child_count(es.voiceTabRow);
        for (uint32_t i = 0; i < childCnt && i < 8; i++) {
            lv_obj_t* t = lv_obj_get_child(es.voiceTabRow, i);
            lv_obj_set_style_bg_opa(t, (i == (uint32_t)es.selectedVoice) ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static void cbEuclidTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& es = mc->euclid_;
    if (!es.playing) return;

    uint32_t now = lv_tick_get();
    if (now < es.nextStepMs) return;

    mc->sendMidi(0xB0, 123, 0);  // all-notes-off

    for (int vi = 0; vi < 8; vi++) {
        auto& v = es.voice[vi];
        if (v.steps == 0) continue;
        int stepIdx = es.currentStep % v.steps;
        if (v.pattern[stepIdx]) {
            mc->sendMidi(0x90, (uint8_t)v.note, 110);
        }
    }

    // Free-running clock: each voice takes currentStep modulo its own length.
    // Wrapping at 64 would restart voices whose step count does not divide 64.
    es.currentStep++;

    uint32_t stepMs = 60000u / (uint32_t)es.bpm / 4u;
    if (es.triplet) stepMs = stepMs * 2 / 3;
    if (stepMs < 20) stepMs = 20;
    es.nextStepMs = now + stepMs;

    if (es.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", es.bpm);
        lv_label_set_text(es.bpmLbl, buf);
    }

    euclidDrawCanvas(es);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbEuclidBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbEuclidPlay(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& es = mc->euclid_;
    es.playing = !es.playing;
    if (!es.playing) {
        mc->sendMidi(0xB0, 123, 0);
        es.currentStep = 0;
    } else {
        es.nextStepMs = lv_tick_get();
    }
    if (es.playLbl) lv_label_set_text(es.playLbl, es.playing ? "STOP" : "PLAY");
    // Stopping resets the playhead; repaint the rings so it is not left on the
    // previous step until the next timer tick (which no longer runs when stopped).
    euclidDrawCanvas(es);
    lcdUpdate(mc);
}

static void cbEuclidTrip(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& es = mc->euclid_;
    es.triplet = !es.triplet;
    if (es.tripLbl) lv_label_set_text(es.tripLbl, es.triplet ? "TRIP ON" : "TRIP OFF");
}

static void cbEuclidBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& es = mc->euclid_;
    if (es.bpm > 40) es.bpm -= 5;
    if (es.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", es.bpm);
        lv_label_set_text(es.bpmLbl, buf);
    }
}
static void cbEuclidBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& es = mc->euclid_;
    if (es.bpm < 280) es.bpm += 5;
    if (es.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", es.bpm);
        lv_label_set_text(es.bpmLbl, buf);
    }
}

// Voice tab - mc is stored as object user_data, voice index as event user_data
static void cbEuclidVoiceTab(lv_event_t* e) {
    lv_obj_t* tab = lv_event_get_target_obj(e);
    auto* mc = static_cast<MidiController*>(lv_obj_get_user_data(tab));
    if (!mc) return;
    auto& es = mc->euclid_;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    es.selectedVoice = (int)idx;

    // Update tab button opacities: selected = full, others = dim
    lv_obj_t* tabRow = lv_obj_get_parent(tab);
    if (tabRow) {
        uint32_t childCnt = lv_obj_get_child_count(tabRow);
        for (uint32_t i = 0; i < childCnt && i < 8; i++) {
            lv_obj_t* t = lv_obj_get_child(tabRow, i);
            lv_obj_set_style_bg_opa(t, (i == (uint32_t)idx) ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }

    euclidUpdateParamLabels(es);
    euclidDrawCanvas(es);
    lcdUpdate(mc);
}

static void cbEuclidStepsDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.steps > 1) v.steps--;
    if (v.events > v.steps) v.events = v.steps;
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}
static void cbEuclidStepsUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.steps < 32) v.steps++;
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}

static void cbEuclidEvtsDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.events > 0) v.events--;
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}
static void cbEuclidEvtsUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.events < v.steps) v.events++;
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}

static void cbEuclidRotDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    v.rotation = (v.rotation - 1 + v.steps) % std::max(1, v.steps);
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}
static void cbEuclidRotUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    v.rotation = (v.rotation + 1) % std::max(1, v.steps);
    euclidGenPattern(v);
    euclidUpdateParamLabels(mc->euclid_);
    euclidDrawCanvas(mc->euclid_);
}

static void cbEuclidNoteDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.note > 0) v.note--;
    euclidUpdateParamLabels(mc->euclid_);
}
static void cbEuclidNoteUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& v = mc->euclid_.voice[mc->euclid_.selectedVoice];
    if (v.note < 127) v.note++;
    euclidUpdateParamLabels(mc->euclid_);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void buildEuclidView(MidiController* mc, lv_obj_t* parent) {
    auto& es = mc->euclid_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);
    const lv_font_t* fontLarge  = lvgl_get_text_font(FONT_SIZE_LARGE);

    // Default voice params if not yet set
    static const int DEFAULT_STEPS[8]  = {16, 16, 16, 16, 16, 16, 16, 16};
    static const int DEFAULT_EVENTS[8] = {4, 4, 8, 5, 3, 6, 7, 2};
    static const int DEFAULT_ROT[8]    = {0, 2, 0, 1, 0, 1, 3, 2};
    static const int DEFAULT_NOTE[8]   = {36, 38, 42, 39, 45, 47, 49, 51};
    for (int i = 0; i < 8; i++) {
        auto& v = es.voice[i];
        if (v.steps == 0) {
            v.steps    = DEFAULT_STEPS[i];
            v.events   = DEFAULT_EVENTS[i];
            v.rotation = DEFAULT_ROT[i];
            v.note     = DEFAULT_NOTE[i];
        }
        euclidGenPattern(v);
    }

    // -- Root: flex COLUMN - full content area --------------------------------
    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(root, MC_BG(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    // Shared helpers (used in both transport row and right column)
    auto makeBtn = [&](lv_obj_t* par, const char* txt, int w,
                       lv_event_cb_t cb, void* ud) -> lv_obj_t* {
        return mcSmallBtn(par, txt, w, cb, ud);
    };

    auto makeLbl = [&](lv_obj_t* par, const char* txt, int w,
                       lv_color_t col, const lv_font_t* font = nullptr) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(par);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, font ? font : fontSmall, 0);
        lv_obj_set_style_text_color(lbl, col, 0);
        lv_obj_set_width(lbl, w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        return lbl;
    };

    // -- Row 1: Full-width transport (ROW_H) ----------------------------------
    //   < | PLAY | BPM- N BPM+ | TRIP ON/OFF
    {
        lv_obj_t* topRow = mcRow(root);

        mcBackBtn(topRow, cbEuclidBack, mc, BTN_W_LG);

        lv_obj_t* title = lv_label_create(topRow);
        lv_label_set_text(title, "EUCLID");
        lv_obj_set_style_text_font(title, fontNormal, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x44BBFF), 0);
        lv_obj_set_flex_grow(title, 1);

        lv_obj_t* playBtn = makeBtn(topRow, es.playing ? "STOP" : "PLAY",
                                    BTN_W_MD, cbEuclidPlay, mc);
        es.playLbl = lv_obj_get_child(playBtn, 0);

        lv_obj_t* sep = lv_label_create(topRow);
        lv_label_set_text(sep, "|");
        lv_obj_set_style_text_font(sep, fontSmall, 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(0x334455), 0);

        makeBtn(topRow, "<", BTN_W_SM, cbEuclidBpmDown, mc);
        char bpmBuf[8]; snprintf(bpmBuf, sizeof(bpmBuf), "%d", es.bpm);
        es.bpmLbl = makeLbl(topRow, bpmBuf, LBL_W, lv_color_hex(0xFFE000));
        makeBtn(topRow, ">", BTN_W_SM, cbEuclidBpmUp, mc);

        sep = lv_label_create(topRow);
        lv_label_set_text(sep, "|");
        lv_obj_set_style_text_font(sep, fontSmall, 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(0x334455), 0);

        lv_obj_t* tripBtn = makeBtn(topRow, es.triplet ? "TRIP ON" : "TRIP OFF",
                                    120, cbEuclidTrip, mc);
        es.tripLbl = lv_obj_get_child(tripBtn, 0);
    }

    // -- Body: flex ROW (flex_grow) - canvas LEFT + controls RIGHT ------------
    {
        lv_obj_t* body = mcContainer(root);
        lv_obj_set_size(body, LV_PCT(100), 0);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(body, 0, 0);
        lv_obj_set_style_pad_all(body, 0, 0);
        lv_obj_set_style_pad_gap(body, 0, 0);
        lv_obj_set_scrollable(body, false);

        // -- LEFT: canvas area (flex_grow, LV_PCT(100) height fills body) ----
        {
            lv_obj_t* canvasArea = mcContainer(body);
            lv_obj_set_flex_grow(canvasArea, 1);
            lv_obj_set_height(canvasArea, LV_PCT(100));
            lv_obj_set_style_bg_color(canvasArea, lv_color_hex(0x040610), 0);
            lv_obj_set_style_bg_opa(canvasArea, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(canvasArea, 0, 0);
            lv_obj_set_style_pad_all(canvasArea, 0, 0);
            lv_obj_set_scrollable(canvasArea, false);
            lv_obj_set_layout(canvasArea, LV_LAYOUT_NONE);  // children use absolute lv_obj_set_pos()
            es.canvas = canvasArea;
            // Estimates for first draw before layout settles
            es.canvasW = lv_display_get_horizontal_resolution(nullptr) - COL_W;
            es.canvasH = lv_display_get_vertical_resolution(nullptr) - CHROME_H - ROW_H;
        }

        // -- RIGHT: controls column (500px wide) ------------------------------
        {

            lv_obj_t* rightCol = mcContainer(body);
            lv_obj_set_size(rightCol, COL_W, LV_PCT(100));
            lv_obj_set_flex_flow(rightCol, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(rightCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_bg_color(rightCol, MC_PANEL(), 0);
            lv_obj_set_style_bg_opa(rightCol, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(rightCol, 0, 0);
            lv_obj_set_style_pad_all(rightCol, PAD, 0);
            lv_obj_set_style_pad_gap(rightCol, PAD, 0);
            lv_obj_set_scrollable(rightCol, false);

            auto makeRow = [&](int h) -> lv_obj_t* {
                lv_obj_t* row = mcContainer(rightCol);
                lv_obj_set_size(row, LV_PCT(100), h);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_pad_all(row, 0, 0);
                lv_obj_set_style_pad_gap(row, PAD, 0);
                lv_obj_set_scrollable(row, false);
                return row;
            };

            // -- Voice tabs (8 equal, full width) ----------------------------
            {
                lv_obj_t* row = makeRow(ROW_H);
                es.voiceTabRow = row;
                int tabW = (COL_W - PAD * 2 - PAD * 7) / 8;
                for (int i = 0; i < 8; i++) {
                    lv_obj_t* tab = mcButton(row);
                    lv_obj_set_size(tab, tabW, BTN_H);
                    lv_obj_set_user_data(tab, mc);
                    lv_obj_add_event_cb(tab, cbEuclidVoiceTab, LV_EVENT_CLICKED, (void*)(intptr_t)i);
                    lv_obj_set_style_bg_color(tab, lv_color_hex(VOICE_COLORS[i]), 0);
                    lv_obj_set_style_bg_opa(tab, (i == es.selectedVoice) ? LV_OPA_COVER : LV_OPA_30, 0);
                    lv_obj_t* lbl = lv_label_create(tab);
                    char vbuf[4]; snprintf(vbuf, sizeof(vbuf), "%d", i + 1);
                    lv_label_set_text(lbl, vbuf);
                    lv_obj_set_style_text_font(lbl, fontSmall, 0);
                    lv_obj_center(lbl);
                }
            }

            // -- Voice name (large, colored) ----------------------------------
            {
                lv_obj_t* row = makeRow(ROW_H);
                char vbuf[24]; snprintf(vbuf, sizeof(vbuf), "Voice %d", es.selectedVoice + 1);
                es.voiceLbl = makeLbl(row, vbuf, COL_W - PAD * 2,
                                      lv_color_hex(VOICE_COLORS[es.selectedVoice]), fontLarge);
            }

            // Helper: param row with colored header + < value > buttons
            // flex_grow=1 so all 4 rows share remaining space equally
            auto makeParamRow = [&](const char* name, lv_color_t col) -> lv_obj_t* {
                lv_obj_t* row = mcContainer(rightCol);
                lv_obj_set_width(row, LV_PCT(100));
                lv_obj_set_flex_grow(row, 1);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_bg_color(row, lv_color_hex(0x0C0E1A), 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_color(row, lv_color_hex(0x1A2030), 0);
                lv_obj_set_style_pad_all(row, PAD, 0);
                lv_obj_set_style_pad_gap(row, PAD, 0);
                lv_obj_set_scrollable(row, false);
                lv_obj_t* hdr = lv_label_create(row);
                lv_label_set_text(hdr, name);
                lv_obj_set_style_text_font(hdr, fontSmall, 0);
                lv_obj_set_style_text_color(hdr, col, 0);
                lv_obj_set_width(hdr, 120);
                return row;
            };

            // -- Steps --------------------------------------------------------
            {
                lv_obj_t* row = makeParamRow("Steps", lv_color_hex(0xFFCC88));
                makeBtn(row, "<", BTN_W_SM, cbEuclidStepsDown, mc);
                char sBuf[8]; snprintf(sBuf, sizeof(sBuf), "S:%d", es.voice[es.selectedVoice].steps);
                es.stepsLbl = makeLbl(row, sBuf, LBL_W, lv_color_hex(0xFFCC88));
                makeBtn(row, ">", BTN_W_SM, cbEuclidStepsUp, mc);
            }

            // -- Events -------------------------------------------------------
            {
                lv_obj_t* row = makeParamRow("Events", lv_color_hex(0x88DDFF));
                makeBtn(row, "<", BTN_W_SM, cbEuclidEvtsDown, mc);
                char eBuf[8]; snprintf(eBuf, sizeof(eBuf), "E:%d", es.voice[es.selectedVoice].events);
                es.eventsLbl = makeLbl(row, eBuf, LBL_W, lv_color_hex(0x88DDFF));
                makeBtn(row, ">", BTN_W_SM, cbEuclidEvtsUp, mc);
            }

            // -- Rotation -----------------------------------------------------
            {
                lv_obj_t* row = makeParamRow("Rotation", lv_color_hex(0xAAFF88));
                makeBtn(row, "<", BTN_W_SM, cbEuclidRotDown, mc);
                char rBuf[8]; snprintf(rBuf, sizeof(rBuf), "R:%d", es.voice[es.selectedVoice].rotation);
                es.rotLbl = makeLbl(row, rBuf, LBL_W, lv_color_hex(0xAAFF88));
                makeBtn(row, ">", BTN_W_SM, cbEuclidRotUp, mc);
            }

            // -- Note ---------------------------------------------------------
            {
                lv_obj_t* row = makeParamRow("Note", lv_color_hex(0xFF88CC));
                makeBtn(row, "<", BTN_W_SM, cbEuclidNoteDown, mc);
                char nBuf[8]; midiNoteName(es.voice[es.selectedVoice].note, nBuf, sizeof(nBuf));
                es.noteLbl = makeLbl(row, nBuf, LBL_W, lv_color_hex(0xFF88CC));
                makeBtn(row, ">", BTN_W_SM, cbEuclidNoteUp, mc);
            }
        }
    }

    // -- Redraw when canvas gets its real size (first layout pass) ------------
    // At build time LV_PCT(100) hasn't resolved yet, so estimates are used.
    // LV_EVENT_SIZE_CHANGED fires once the layout pass completes.
    lv_obj_add_event_cb(es.canvas, [](lv_event_t* ev) {
        auto* mc2 = static_cast<MidiController*>(lv_event_get_user_data(ev));
        if (!mc2) return;
        auto& es2 = mc2->euclid_;
        int w = lv_obj_get_width(es2.canvas);
        int h = lv_obj_get_height(es2.canvas);
        if (w > 4 && h > 4) {
            es2.canvasW = w;
            es2.canvasH = h;
            euclidDrawCanvas(es2);
        }
    }, LV_EVENT_SIZE_CHANGED, mc);

    // -- Start timer ----------------------------------------------------------
    mc->euclidTimer_ = lv_timer_create(cbEuclidTimer, 10, mc);
    es.nextStepMs = lv_tick_get();
}
