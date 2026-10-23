#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// BABY8 view - 8-step melodic sequencer with swing
//
// Layout (flex column):
//   [ctrl row: < | PLAY | BPM- | N | BPM+  |  SWING- | N% | SWING+]
//   [step grid row (flex_grow): 8 step cells]
//   [edit row: NOTE- | noteName | NOTE+   VEL- | vel | VEL+   GATE  PAT- | P:N | PAT+]
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void baby8UpdateStepColors(MidiController* mc) {
    auto& bs = mc->baby8_;
    for (int i = 0; i < 8; i++) {
        if (!bs.stepBtn[i]) continue;
        const auto& s = bs.steps[i];
        uint32_t col;
        if (i == bs.currentStep && bs.playing) {
            col = s.gate ? 0xFFFFFF : 0x4488FF;
        } else if (i == bs.selectedStep) {
            col = s.gate ? 0xFF8800 : 0x886633;
        } else {
            col = s.gate ? 0x006622 : 0x222222;
        }
        lv_obj_set_style_bg_color(bs.stepBtn[i], lv_color_hex(col), 0);
        // Update note label
        if (bs.noteLbl[i] && s.gate) {
            char buf[8]; midiNoteName(s.note, buf, sizeof(buf));
            lv_label_set_text(bs.noteLbl[i], buf);
        } else if (bs.noteLbl[i]) {
            lv_label_set_text(bs.noteLbl[i], "--");
        }
    }
}

static void baby8UpdateEditRow(MidiController* mc) {
    auto& bs = mc->baby8_;
    const auto& s = bs.steps[bs.selectedStep];
    if (bs.editNoteLbl) {
        char buf[8]; midiNoteName(s.note, buf, sizeof(buf));
        lv_label_set_text(bs.editNoteLbl, buf);
    }
    if (bs.editVelLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "%d", s.velocity);
        lv_label_set_text(bs.editVelLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static void cbBaby8Timer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& bs = mc->baby8_;
    if (!bs.playing) return;

    uint32_t now = lv_tick_get();

    // Note-off
    if (bs.lastNote >= 0 && bs.nextStepMs >= 10 && now >= bs.nextStepMs - 10) {
        mc->sendMidi(0x80, (uint8_t)bs.lastNote, 0);
        bs.lastNote = -1;
    }

    if (now < bs.nextStepMs) return;

    // Advance step
    bs.currentStep = (bs.currentStep + 1) % 8;
    bool even = (bs.currentStep % 2 == 0);

    // BPM → 16th note duration
    uint32_t beatMs   = 60000u / (uint32_t)bs.bpm;
    uint32_t sixteenth = beatMs / 4;

    // Swing: lengthen even steps and shorten odd steps by the same amount so a
    // pair still fills two 16th notes and the overall tempo does not drift.
    // swing=50 → no swing (both equal); swing>50 → even step longer.
    uint32_t stepDur = sixteenth;
    if (bs.swing > 50) {
        if (even) stepDur = (uint32_t)(sixteenth * bs.swing / 50u);
        else      stepDur = (uint32_t)(sixteenth * (100u - bs.swing) / 50u);
        if (stepDur < 20) stepDur = 20;
    }

    bs.nextStepMs = now + stepDur;

    // Play current step
    const auto& step = bs.steps[bs.currentStep];
    if (step.gate) {
        mc->sendMidi(0x90, (uint8_t)step.note, (uint8_t)step.velocity);
        bs.lastNote = step.note;
    }

    // Update step highlight
    baby8UpdateStepColors(mc);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbBaby8Back(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbBaby8Play(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    bs.playing = !bs.playing;
    if (bs.playing) {
        bs.currentStep = 7;  // will advance to 0 on first tick
        uint32_t beatMs   = 60000u / (uint32_t)bs.bpm;
        bs.nextStepMs = lv_tick_get() + beatMs / 4;
    } else {
        if (bs.lastNote >= 0) {
            mc->sendMidi(0x80, (uint8_t)bs.lastNote, 0);
            bs.lastNote = -1;
        }
        baby8UpdateStepColors(mc);
    }
    if (bs.playLbl) lv_label_set_text(bs.playLbl, bs.playing ? "STOP" : "PLAY");
}

static void cbBaby8BpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    if (bs.bpm > 40) bs.bpm -= 5;
    if (bs.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", bs.bpm);
        lv_label_set_text(bs.bpmLbl, buf);
    }
}
static void cbBaby8BpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    if (bs.bpm < 280) bs.bpm += 5;
    if (bs.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", bs.bpm);
        lv_label_set_text(bs.bpmLbl, buf);
    }
}

static void cbBaby8SwingDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    if (bs.swing > 50) bs.swing -= 5;
    if (bs.swingLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", bs.swing);
        lv_label_set_text(bs.swingLbl, buf);
    }
}
static void cbBaby8SwingUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    if (bs.swing < 80) bs.swing += 5;
    if (bs.swingLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", bs.swing);
        lv_label_set_text(bs.swingLbl, buf);
    }
}

static void cbBaby8StepTap(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    // Find which step button was tapped by matching lv_obj
    lv_obj_t* obj = lv_event_get_target_obj(e);
    for (int i = 0; i < 8; i++) {
        if (bs.stepBtn[i] == obj) {
            bs.selectedStep = i;
            break;
        }
    }
    baby8UpdateStepColors(mc);
    baby8UpdateEditRow(mc);
}

static void cbBaby8NoteDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    auto& s = bs.steps[bs.selectedStep];
    if (s.note > 0) s.note--;
    baby8UpdateEditRow(mc);
    baby8UpdateStepColors(mc);
}
static void cbBaby8NoteUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    auto& s = bs.steps[bs.selectedStep];
    if (s.note < 127) s.note++;
    baby8UpdateEditRow(mc);
    baby8UpdateStepColors(mc);
}

static void cbBaby8VelDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    auto& s = bs.steps[bs.selectedStep];
    if (s.velocity > 10) s.velocity -= 10;
    else s.velocity = 1;
    baby8UpdateEditRow(mc);
}
static void cbBaby8VelUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    auto& s = bs.steps[bs.selectedStep];
    if (s.velocity <= 117) s.velocity += 10;
    else s.velocity = 127;
    baby8UpdateEditRow(mc);
}

static void cbBaby8Gate(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    bs.steps[bs.selectedStep].gate = !bs.steps[bs.selectedStep].gate;
    baby8UpdateEditRow(mc);
    baby8UpdateStepColors(mc);
}

static void cbBaby8PatDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    // Save current pattern before switching
    memcpy(bs.patterns[bs.patternIdx], bs.steps, sizeof(bs.steps));
    bs.patternIdx = (bs.patternIdx + 7) % 8;
    memcpy(bs.steps, bs.patterns[bs.patternIdx], sizeof(bs.steps));
    if (bs.patLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "P:%d", bs.patternIdx + 1);
        lv_label_set_text(bs.patLbl, buf);
    }
    baby8UpdateStepColors(mc);
    baby8UpdateEditRow(mc);
}
static void cbBaby8PatUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& bs = mc->baby8_;
    // Save current pattern before switching
    memcpy(bs.patterns[bs.patternIdx], bs.steps, sizeof(bs.steps));
    bs.patternIdx = (bs.patternIdx + 1) % 8;
    memcpy(bs.steps, bs.patterns[bs.patternIdx], sizeof(bs.steps));
    if (bs.patLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "P:%d", bs.patternIdx + 1);
        lv_label_set_text(bs.patLbl, buf);
    }
    baby8UpdateStepColors(mc);
    baby8UpdateEditRow(mc);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void buildBaby8View(MidiController* mc, lv_obj_t* parent) {
    auto& bs = mc->baby8_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int sBtnW = 88;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x070A10), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 8, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    auto makeRow = [&]() -> lv_obj_t* { return mcRow(root); };

    auto makeBtn = [&](lv_obj_t* par, const char* txt, int w, lv_event_cb_t cb, void* ud) -> lv_obj_t* {
        return mcSmallBtn(par, txt, w, cb, ud);
    };

    auto makeLbl = [&](lv_obj_t* par, const char* txt, int w, lv_color_t col) -> lv_obj_t* {
        return mcValue(par, txt, w, col);
    };

    auto makeSep = [&](lv_obj_t* par) {
        lv_obj_t* sep = lv_label_create(par);
        lv_label_set_text(sep, "|");
        lv_obj_set_style_text_font(sep, fontSmall, 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(0x334455), 0);
    };

    // -- Control row -----------------------------------------------------------
    {
        lv_obj_t* row = makeRow();
        mcBackBtn(row, cbBaby8Back, mc, MC_W_S);
        lv_obj_t* titleLbl = lv_label_create(row);
        lv_label_set_text(titleLbl, "BABY-8");
        lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
        lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44BBFF), 0);
        lv_obj_set_flex_grow(titleLbl, 1);
        lv_obj_t* playBtn = makeBtn(row, bs.playing ? "STOP" : "PLAY", 176, cbBaby8Play, mc);
        bs.playLbl = lv_obj_get_child(playBtn, 0);
        makeSep(row);
        makeBtn(row, "<", sBtnW, cbBaby8BpmDown, mc);
        char bufBpm[8]; snprintf(bufBpm, sizeof(bufBpm), "%d", bs.bpm);
        bs.bpmLbl = makeLbl(row, bufBpm, 136, lv_color_hex(0xFFE000));
        makeBtn(row, ">", sBtnW, cbBaby8BpmUp, mc);
        makeSep(row);
        makeBtn(row, "<", sBtnW, cbBaby8SwingDown, mc);
        char bufSw[8]; snprintf(bufSw, sizeof(bufSw), "%d%%", bs.swing);
        bs.swingLbl = makeLbl(row, bufSw, 128, lv_color_hex(0xAADDFF));
        makeBtn(row, ">", sBtnW, cbBaby8SwingUp, mc);
    }

    // -- Step grid (flex_grow) -----------------------------------------------------------
    {
        lv_obj_t* gridArea = mcContainer(root);
        lv_obj_set_width(gridArea, LV_PCT(100));
        lv_obj_set_flex_grow(gridArea, 1);
        lv_obj_set_flex_flow(gridArea, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(gridArea, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(gridArea, lv_color_hex(0x050810), 0);
        lv_obj_set_style_bg_opa(gridArea, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(gridArea, 0, 0);
        lv_obj_set_style_pad_all(gridArea, 16, 0);
        lv_obj_set_style_pad_gap(gridArea, 12, 0);
        lv_obj_set_scrollable(gridArea, false);

        // Compute cell size from available space
        int cellW = (lv_display_get_horizontal_resolution(nullptr) - 32 - 7 * 12) / 8;

        for (int i = 0; i < 8; i++) {
            lv_obj_t* cell = mcContainer(gridArea);
            lv_obj_set_size(cell, cellW, LV_PCT(100));
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_radius(cell, 16, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(0x334455), 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(bs.steps[i].gate ? 0x006622u : 0x222222u), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(cell, 8, 0);
            lv_obj_set_style_pad_gap(cell, 8, 0);
            lv_obj_set_scrollable(cell, false);
            lv_obj_set_clickable(cell, true);
            lv_obj_add_event_cb(cell, cbBaby8StepTap, LV_EVENT_CLICKED, mc);
            bs.stepBtn[i] = cell;

            // Step number
            lv_obj_t* numLbl = lv_label_create(cell);
            char nbuf[4]; snprintf(nbuf, sizeof(nbuf), "%d", i + 1);
            lv_label_set_text(numLbl, nbuf);
            lv_obj_set_style_text_font(numLbl, fontSmall, 0);
            lv_obj_set_style_text_color(numLbl, lv_color_hex(0x888888), 0);

            // Note name
            lv_obj_t* noteLbl = lv_label_create(cell);
            char noteBuf[8]; midiNoteName(bs.steps[i].note, noteBuf, sizeof(noteBuf));
            lv_label_set_text(noteLbl, bs.steps[i].gate ? noteBuf : "--");
            lv_obj_set_style_text_font(noteLbl, fontSmall, 0);
            lv_obj_set_style_text_color(noteLbl, lv_color_hex(0xFFFFFF), 0);
            bs.noteLbl[i] = noteLbl;
        }
    }

    // -- Edit row -----------------------------------------------------------
    {
        lv_obj_t* row = makeRow();
        // Note
        makeBtn(row, "<", sBtnW, cbBaby8NoteDown, mc);
        char noteBuf[8]; midiNoteName(bs.steps[bs.selectedStep].note, noteBuf, sizeof(noteBuf));
        bs.editNoteLbl = makeLbl(row, noteBuf, 128, lv_color_hex(0x88DDFF));
        makeBtn(row, ">", sBtnW, cbBaby8NoteUp, mc);
        // Velocity
        makeBtn(row, "<", sBtnW, cbBaby8VelDown, mc);
        char velBuf[6]; snprintf(velBuf, sizeof(velBuf), "%d", bs.steps[bs.selectedStep].velocity);
        bs.editVelLbl = makeLbl(row, velBuf, 112, lv_color_hex(0xFFCC88));
        makeBtn(row, ">", sBtnW, cbBaby8VelUp, mc);
        // Gate
        makeBtn(row, "GATE", 160, cbBaby8Gate, mc);
        // Pattern
        makeBtn(row, "<", sBtnW, cbBaby8PatDown, mc);
        char patBuf[16]; snprintf(patBuf, sizeof(patBuf), "P:%d", bs.patternIdx + 1);
        bs.patLbl = makeLbl(row, patBuf, 112, lv_color_hex(0xAA88FF));
        makeBtn(row, ">", sBtnW, cbBaby8PatUp, mc);
    }

    // -- Start timer -----------------------------------------------------------
    bs.nextStepMs = lv_tick_get() + 60000u / (uint32_t)bs.bpm / 4u;
    if (mc->baby8Timer_) {
        lv_timer_delete(mc->baby8Timer_);
    }
    mc->baby8Timer_ = lv_timer_create(cbBaby8Timer, 10, mc);
}
