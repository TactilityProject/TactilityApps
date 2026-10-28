#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// CHORD view - Diatonic chord player
//
// 8 chord buttons (I-VII + I octave up) filling the main area.
// Control row at bottom: OCT- | OCT+ | SCALE | CLEAR
// ---------------------------------------------------------------------------

// Diatonic chord Roman numeral labels
static const char* CHORD_LABELS[] = { "I", "ii", "iii", "IV", "V", "vi", "vii\xc2\xb0", "I+" };
// Chord type per diatonic degree: 0=MAJ 1=MIN 2=DIM
static const int CHORD_TYPE[8]    = { 0, 1, 1, 0, 0, 1, 2, 0 };
// Diatonic scale degree index for getNoteInScale (degree 0-6, degree 7 = degree 0 +1 octave)
static const int DEGREE_IDX[8]    = { 0, 1, 2, 3, 4, 5, 6, 0 };
// Octave offset per degree (0=same octave, 1=octave up for I+)
static const int DEGREE_OCT[8]    = { 0, 0, 0, 0, 0, 0, 0, 1 };
// Per-degree color palette
static const uint32_t DEGREE_COLORS[8] = {
    0xFF3333, 0xFF8800, 0xFFFF00, 0x33CC33,
    0x3399FF, 0xAA33FF, 0xFF33CC, 0xFF3333
};

// Chord voicing intervals (up to 4 notes, -1 = unused)
static const int CHORD_VOICING[3][4] = {
    { 0, 4, 7, -1 },   // MAJ
    { 0, 3, 7, -1 },   // MIN
    { 0, 3, 6, -1 },   // DIM
};


static void playChord(MidiController* mc, int degree) {
    auto& ch = mc->chord_;
    const int* voicing = CHORD_VOICING[CHORD_TYPE[degree]];
    int rootNote = getNoteInScale(ch.scale, DEGREE_IDX[degree], ch.octave + DEGREE_OCT[degree], ch.key);
    rootNote = std::max(0, std::min(127, rootNote));

    for (int n = 0; n < 4; n++) {
        if (voicing[n] < 0) {
            ch.chordNotes[degree][n] = -1;
            continue;
        }
        int note = std::max(0, std::min(127, rootNote + voicing[n]));
        ch.chordNotes[degree][n] = note;
        mc->sendMidi(0x90, (uint8_t)note, 100);
    }
}

static void refreshChordBtn(MidiController* mc, int degree) {
    lv_obj_t* btn = mc->chord_.chordBtn[degree];
    if (!btn) return;
    bool pressed = mc->chord_.chordPlaying[degree];
    lv_color_t col = lv_color_hex(DEGREE_COLORS[degree]);
    if (pressed) {
        lv_obj_set_style_bg_color(btn, col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        // Dark text on bright bg
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x080C12), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2A40), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, col, LV_PART_MAIN);
    }
}

static void clearAllChordState(MidiController* mc) {
    for (int i = 0; i < 8; i++) {
        mc->chord_.chordPlaying[i] = false;
        refreshChordBtn(mc, i);
    }
}

static void rebuildChordBtnLabels(MidiController* mc) {
    auto& ch = mc->chord_;
    for (int i = 0; i < 8; i++) {
        if (!ch.chordBtn[i]) continue;
        lv_obj_t* lblObj = lv_obj_get_child(ch.chordBtn[i], 0);
        if (lblObj) {
            int rootNote = std::max(0, std::min(127, getNoteInScale(ch.scale, DEGREE_IDX[i], ch.octave + DEGREE_OCT[i], ch.key)));
            char noteName[8]; midiNoteName(rootNote, noteName, sizeof(noteName));
            char buf[16]; snprintf(buf, sizeof(buf), "%s\n%s", CHORD_LABELS[i], noteName);
            lv_label_set_text(lblObj, buf);
        }
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbChordPressed(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    lv_obj_t* btn = lv_event_get_target_obj(e);
    int degree = (int)(intptr_t)lv_obj_get_user_data(btn);
    mc->chord_.chordPlaying[degree] = true;
    playChord(mc, degree);
    refreshChordBtn(mc, degree);
}

static void cbChordReleased(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    lv_obj_t* btn = lv_event_get_target_obj(e);
    int degree = (int)(intptr_t)lv_obj_get_user_data(btn);
    mc->chord_.chordPlaying[degree] = false;
    // Use stored notes from press to avoid note-off mismatch if octave/scale changed
    for (int n = 0; n < 4; n++) {
        int note = mc->chord_.chordNotes[degree][n];
        if (note >= 0) mc->sendMidi(0x80, (uint8_t)note, 0);
    }
    refreshChordBtn(mc, degree);
}

static void cbOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->stopAllNotes();
    clearAllChordState(mc);
    if (mc->chord_.octave > 1) mc->chord_.octave--;
    rebuildChordBtnLabels(mc);
    if (mc->chord_.octLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "Oct %d", mc->chord_.octave);
        lv_label_set_text(mc->chord_.octLbl, buf);
    }
}
static void cbOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->stopAllNotes();
    clearAllChordState(mc);
    if (mc->chord_.octave < 7) mc->chord_.octave++;
    rebuildChordBtnLabels(mc);
    if (mc->chord_.octLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "Oct %d", mc->chord_.octave);
        lv_label_set_text(mc->chord_.octLbl, buf);
    }
}

static void cbScaleCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->stopAllNotes();
    clearAllChordState(mc);
    mc->chord_.scale = (mc->chord_.scale + 1) % NUM_MIDI_SCALES;
    rebuildChordBtnLabels(mc);
    if (mc->chord_.scaleLbl) {
        lv_label_set_text(mc->chord_.scaleLbl, MIDI_SCALES[mc->chord_.scale].name);
    }
}

static void cbClear(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->stopAllNotes();
    for (int i = 0; i < 8; i++) {
        mc->chord_.chordPlaying[i] = false;
        refreshChordBtn(mc, i);
    }
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildChordView(MidiController* mc, lv_obj_t* parent) {
    auto& ch = mc->chord_;
    // Mode switching sends All Notes Off, so any held-chord flags from the
    // previous visit are stale; rebuild the buttons in the released state.
    for (auto& playing : ch.chordPlaying) playing = false;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);
    int ctrlH = 88;
    int btnH  = MC_BTN_H;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    // -- Control row (TOP) ----------------------------------------------------
    lv_obj_t* ctrl = mcRow(root, ctrlH);

    // Back
    mcBackBtn(ctrl, cbBack, mc, 96);

    lv_obj_t* titleLbl = lv_label_create(ctrl);
    lv_label_set_text(titleLbl, "CHORD");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44BBFF), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    // OCT -
    lv_obj_t* octDown = mcButton(ctrl);
    lv_obj_set_size(octDown, 128, btnH);
    lv_obj_add_event_cb(octDown, cbOctDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* odl = lv_label_create(octDown);
    lv_label_set_text(odl, "OCT-");
    lv_obj_set_style_text_font(odl, fontSmall, 0);
    lv_obj_center(odl);

    // OCT label
    ch.octLbl = lv_label_create(ctrl);
    char obuf[8]; snprintf(obuf, sizeof(obuf), "Oct %d", ch.octave);
    lv_label_set_text(ch.octLbl, obuf);
    lv_obj_set_style_text_font(ch.octLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ch.octLbl, lv_color_hex(0x666688), 0);
    lv_obj_set_width(ch.octLbl, 144);
    lv_obj_set_style_text_align(ch.octLbl, LV_TEXT_ALIGN_CENTER, 0);

    // OCT +
    lv_obj_t* octUp = mcButton(ctrl);
    lv_obj_set_size(octUp, 128, btnH);
    lv_obj_add_event_cb(octUp, cbOctUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* oul = lv_label_create(octUp); lv_label_set_text(oul, "OCT+");
    lv_obj_set_style_text_font(oul, fontSmall, 0); lv_obj_center(oul);

    // SCALE
    lv_obj_t* scaleBtn = mcButton(ctrl);
    lv_obj_set_size(scaleBtn, 224, btnH);
    lv_obj_add_event_cb(scaleBtn, cbScaleCycle, LV_EVENT_CLICKED, mc);
    ch.scaleLbl = lv_label_create(scaleBtn);
    lv_label_set_text(ch.scaleLbl, MIDI_SCALES[ch.scale].name);
    lv_obj_set_style_text_font(ch.scaleLbl, fontSmall, 0);
    lv_obj_center(ch.scaleLbl);

    // CLEAR
    lv_obj_t* clearBtn = mcButton(ctrl);
    lv_obj_set_size(clearBtn, 176, btnH);
    lv_obj_set_style_bg_color(clearBtn, lv_color_hex(0x331A00), 0);
    lv_obj_add_event_cb(clearBtn, cbClear, LV_EVENT_CLICKED, mc);
    lv_obj_t* clearLbl = lv_label_create(clearBtn);
    lv_label_set_text(clearLbl, "CLEAR");
    lv_obj_set_style_text_font(clearLbl, fontSmall, 0);
    lv_obj_center(clearLbl);

    // -- Chord button row (fills remaining space) -----------------------------
    lv_obj_t* chordRow = mcContainer(root);
    lv_obj_set_width(chordRow, LV_PCT(100));
    lv_obj_set_flex_grow(chordRow, 1);
    lv_obj_set_flex_flow(chordRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chordRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(chordRow, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(chordRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chordRow, 0, 0);
    lv_obj_set_style_pad_all(chordRow, 8, 0);
    lv_obj_set_style_pad_gap(chordRow, 8, 0);
    lv_obj_set_scrollable(chordRow, false);

    for (int i = 0; i < 8; i++) {
        lv_obj_t* btn = mcButton(chordRow);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2A40), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(DEGREE_COLORS[i]), 0);
        lv_obj_set_style_border_width(btn, 8, 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, cbChordPressed,  LV_EVENT_PRESSED,  mc);
        lv_obj_add_event_cb(btn, cbChordReleased, LV_EVENT_RELEASED, mc);

        // Label: Roman numeral + root note name with octave (two lines)
        int rootNote = std::max(0, std::min(127, getNoteInScale(ch.scale, DEGREE_IDX[i], ch.octave + DEGREE_OCT[i], ch.key)));
        char rootName[8]; midiNoteName(rootNote, rootName, sizeof(rootName));
        char lbuf[16]; snprintf(lbuf, sizeof(lbuf), "%s\n%s", CHORD_LABELS[i], rootName);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, lbuf);
        lv_obj_set_style_text_font(lbl, fontSmall, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(DEGREE_COLORS[i]), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        lv_obj_set_clickable(lbl, false);

        ch.chordBtn[i] = btn;
    }

}
