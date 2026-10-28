#include "MidiController.h"

#include <lvgl/devices/pointer.h>
#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// KEYS view - single-octave chromatic keyboard
//
// Layout (flex column):
//   [control row: < | OCT- | octave | OCT+ | SCALE | KEY- | key | KEY+]
//   [piano area: 7 white keys with 5 black keys overlaid]
//   [status label]
// ---------------------------------------------------------------------------

static constexpr int WHITE_OFF[7] = { 0, 2, 4, 5, 7, 9, 11 };
static constexpr int BLACK_OFF[5] = { 1, 3, 6, 8, 10 };
// Center X of each black key as fraction of white-key-width × 100
static constexpr int BLACK_FRAC[5] = { 65, 165, 365, 465, 565 };

static const char* WHITE_NAME[7] = { "C","D","E","F","G","A","B" };

// Key colors by role
static lv_color_t keyColorWhiteDefault() { return lv_color_hex(0xE8E8D4); }
static lv_color_t keyColorBlackDefault() { return lv_color_hex(0x111111); }

static inline lv_coord_t dispW() {
    return (lv_coord_t)lv_display_get_horizontal_resolution(nullptr);
}

static void resetKeyColors(MidiController* mc) {
    auto& ks = mc->keys_;

    for (int i = 0; i < 7; i++) {
        if (!ks.whiteKey[i]) continue;
        lv_obj_set_style_bg_color(ks.whiteKey[i], keyColorWhiteDefault(), LV_PART_MAIN);
    }

    for (int i = 0; i < 5; i++) {
        if (!ks.blackKey[i]) continue;
        lv_obj_set_style_bg_color(ks.blackKey[i], keyColorBlackDefault(), LV_PART_MAIN);
    }
}

// Rebuild note assignments after an octave change, and refresh the scale/key labels
static void rebuildKeyNotes(MidiController* mc) {
    auto& ks = mc->keys_;

    // White keys
    for (int k = 0; k < 7; k++) {
        if (!ks.whiteKey[k]) continue;
        int note = std::max(0, std::min(127, (ks.octave + 1) * 12 + WHITE_OFF[k]));
        lv_obj_set_user_data(ks.whiteKey[k], (void*)(intptr_t)note);

        // Update C note label
        if (k == 0) {
            lv_obj_t* lbl = lv_obj_get_child(ks.whiteKey[k], 0);
            if (lbl) {
                char buf[16]; snprintf(buf, sizeof(buf), "C%d", ks.octave);
                lv_label_set_text(lbl, buf);
            }
        }
    }

    // Black keys
    for (int k = 0; k < 5; k++) {
        if (!ks.blackKey[k]) continue;
        int note = std::max(0, std::min(127, (ks.octave + 1) * 12 + BLACK_OFF[k]));
        lv_obj_set_user_data(ks.blackKey[k], (void*)(intptr_t)note);
    }

    // Update status label
    if (ks.statusLbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Oct %d | %s | Key %s",
                 ks.octave,
                 MIDI_SCALES[ks.scale % NUM_MIDI_SCALES].name,
                 KEY_NAMES[ks.key % 12]);
        lv_label_set_text(ks.statusLbl, buf);
    }
    if (ks.octLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "Oct %d", ks.octave);
        lv_label_set_text(ks.octLbl, buf);
    }
    if (ks.scaleLbl) {
        lv_label_set_text(ks.scaleLbl, MIDI_SCALES[ks.scale % NUM_MIDI_SCALES].name);
    }
    if (ks.keyLbl) {
        lv_label_set_text(ks.keyLbl, KEY_NAMES[ks.key % 12]);
    }

    resetKeyColors(mc);
}

// ---------------------------------------------------------------------------
// Touch handling - drag across keys (pressing on pianoArea)
// ---------------------------------------------------------------------------

static void cbPianoPressing(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ks = mc->keys_;

    // lv_event_get_indev() (the indev that actually fired THIS event), not lv_indev_active() -
    // the latter is a process-global "whichever indev is being processed right now" and would
    // make every finger's hit-test collapse onto whichever touch happened to be read last, once
    // multiple simultaneous touch slots exist (see lvgl-module's pointer pool). slotIdx indexes
    // ks.lastNote so each finger tracks its own held note independently.
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    int8_t slotIdx = lvgl_pointer_get_slot_index(indev);
    if (slotIdx < 0) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (!ks.pianoArea) return;
    lv_area_t rowArea;
    lv_obj_get_coords(ks.pianoArea, &rowArea);
    if (pt.y < rowArea.y1 || pt.y > rowArea.y2) return;

    // Black keys take priority - check them first
    int newNote = -1;
    for (int i = 0; i < 5; i++) {
        if (!ks.blackKey[i]) continue;
        lv_area_t area;
        lv_obj_get_coords(ks.blackKey[i], &area);
        if (pt.x >= area.x1 && pt.x <= area.x2 && pt.y >= area.y1 && pt.y <= area.y2) {
            newNote = (int)(intptr_t)lv_obj_get_user_data(ks.blackKey[i]);
            break;
        }
    }

    // Then white keys
    if (newNote < 0) {
        for (int i = 0; i < 7; i++) {
            if (!ks.whiteKey[i]) continue;
            lv_area_t area;
            lv_obj_get_coords(ks.whiteKey[i], &area);
            if (pt.x >= area.x1 && pt.x <= area.x2) {
                newNote = (int)(intptr_t)lv_obj_get_user_data(ks.whiteKey[i]);
                break;
            }
        }
    }

    if (newNote < 0) return;
    if (newNote == ks.lastNote[slotIdx]) return;

    // Slide: off old note, on new note (only this finger's note - other slots' held notes are
    // untouched)
    if (ks.lastNote[slotIdx] >= 0) {
        mc->sendMidi(0x80, (uint8_t)ks.lastNote[slotIdx], 0);
    }
    mc->sendMidi(0x90, (uint8_t)newNote, 100);
    ks.lastNote[slotIdx] = newNote;
}

static void cbPianoReleased(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ks = mc->keys_;

    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    int8_t slotIdx = lvgl_pointer_get_slot_index(indev);
    if (slotIdx < 0) return;

    if (ks.lastNote[slotIdx] >= 0) {
        mc->sendMidi(0x80, (uint8_t)ks.lastNote[slotIdx], 0);
        ks.lastNote[slotIdx] = -1;
    }
}

// ---------------------------------------------------------------------------
// Control callbacks
// ---------------------------------------------------------------------------

static void cbOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->keys_.octave > 1) {
        mc->keys_.octave--;
        rebuildKeyNotes(mc);
    }
}

static void cbOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->keys_.octave < 8) {
        mc->keys_.octave++;
        rebuildKeyNotes(mc);
    }
}

static void cbScaleCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->keys_.scale = (mc->keys_.scale + 1) % NUM_MIDI_SCALES;
    rebuildKeyNotes(mc);
}

static void cbKeyDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->keys_.key = (mc->keys_.key + 11) % 12;
    rebuildKeyNotes(mc);
}

static void cbKeyUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->keys_.key = (mc->keys_.key + 1) % 12;
    rebuildKeyNotes(mc);
}

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildKeysView(MidiController* mc, lv_obj_t* parent) {
    auto& ks = mc->keys_;
    // A note held when the view was left is already silenced by the mode-switch
    // All Notes Off; clear the per-slot state so the next tap always sends Note On.
    for (auto& n : ks.lastNote) n = -1;

    lv_coord_t W = dispW();
    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int rowH    = 88;   // single combined control row height
    int statusH = 56;

    // Root container
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

    // -- Single control row: [<] [OCT-] [OCT] [OCT+] | [SCALE] [KEY-] [KEY] [KEY+] --
    lv_obj_t* ctrl = mcRow(root, rowH);

    int btnH = MC_BTN_H;

    // Back button
    mcBackBtn(ctrl, cbBack, mc, MC_W_S);

    // OCT -
    lv_obj_t* octDownBtn = mcButton(ctrl);
    lv_obj_set_size(octDownBtn, 144, btnH);
    lv_obj_add_event_cb(octDownBtn, cbOctDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* odLbl = lv_label_create(octDownBtn);
    lv_label_set_text(odLbl, "OCT-");
    lv_obj_set_style_text_font(odLbl, fontSmall, 0);
    lv_obj_center(odLbl);

    // OCT label
    ks.octLbl = lv_label_create(ctrl);
    char obuf[16]; snprintf(obuf, sizeof(obuf), "Oct %d", ks.octave);
    lv_label_set_text(ks.octLbl, obuf);
    lv_obj_set_style_text_font(ks.octLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ks.octLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_width(ks.octLbl, 144);
    lv_obj_set_style_text_align(ks.octLbl, LV_TEXT_ALIGN_CENTER, 0);

    // OCT +
    lv_obj_t* octUpBtn = mcButton(ctrl);
    lv_obj_set_size(octUpBtn, 144, btnH);
    lv_obj_add_event_cb(octUpBtn, cbOctUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* ouLbl = lv_label_create(octUpBtn);
    lv_label_set_text(ouLbl, "OCT+");
    lv_obj_set_style_text_font(ouLbl, fontSmall, 0);
    lv_obj_center(ouLbl);

    // Divider label
    lv_obj_t* div = lv_label_create(ctrl);
    lv_label_set_text(div, "|");
    lv_obj_set_style_text_color(div, lv_color_hex(0x334455), 0);
    lv_obj_set_style_text_font(div, fontSmall, 0);

    // Scale cycle button
    lv_obj_t* scaleBtn = mcButton(ctrl);
    lv_obj_set_size(scaleBtn, 224, btnH);
    lv_obj_add_event_cb(scaleBtn, cbScaleCycle, LV_EVENT_CLICKED, mc);
    ks.scaleLbl = lv_label_create(scaleBtn);
    lv_label_set_text(ks.scaleLbl, MIDI_SCALES[ks.scale % NUM_MIDI_SCALES].name);
    lv_obj_set_style_text_font(ks.scaleLbl, fontSmall, 0);
    lv_obj_center(ks.scaleLbl);

    // KEY -
    lv_obj_t* keyDownBtn = mcButton(ctrl);
    lv_obj_set_size(keyDownBtn, 112, btnH);
    lv_obj_add_event_cb(keyDownBtn, cbKeyDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* kdLbl = lv_label_create(keyDownBtn);
    lv_label_set_text(kdLbl, "KEY-");
    lv_obj_set_style_text_font(kdLbl, fontSmall, 0);
    lv_obj_center(kdLbl);

    // Key label
    ks.keyLbl = lv_label_create(ctrl);
    lv_label_set_text(ks.keyLbl, KEY_NAMES[ks.key % 12]);
    lv_obj_set_style_text_font(ks.keyLbl, fontNormal, 0);
    lv_obj_set_style_text_color(ks.keyLbl, MC_VALUE(), 0);
    lv_obj_set_width(ks.keyLbl, 96);
    lv_obj_set_style_text_align(ks.keyLbl, LV_TEXT_ALIGN_CENTER, 0);

    // KEY +
    lv_obj_t* keyUpBtn = mcButton(ctrl);
    lv_obj_set_size(keyUpBtn, 112, btnH);
    lv_obj_add_event_cb(keyUpBtn, cbKeyUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* kuLbl = lv_label_create(keyUpBtn);
    lv_label_set_text(kuLbl, "KEY+");
    lv_obj_set_style_text_font(kuLbl, fontSmall, 0);
    lv_obj_center(kuLbl);

    // -- Piano area (single row/octave, flex_grow fills remaining height) -----
    lv_obj_t* pianoArea = mcContainer(root);
    lv_obj_set_width(pianoArea, LV_PCT(100));
    lv_obj_set_flex_grow(pianoArea, 1);
    lv_obj_set_style_bg_color(pianoArea, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(pianoArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pianoArea, 0, 0);
    lv_obj_set_style_radius(pianoArea, 0, 0);
    lv_obj_set_style_pad_all(pianoArea, 0, 0);
    lv_obj_set_scrollable(pianoArea, false);

    lv_obj_add_event_cb(pianoArea, cbPianoPressing, LV_EVENT_PRESSING, mc);
    lv_obj_add_event_cb(pianoArea, cbPianoReleased, LV_EVENT_RELEASED, mc);

    ks.pianoArea = pianoArea;

    int wkGap = 2;              // seam between white keys (dark bg shows through)
    int wkw   = W / 7;          // white key cell width
    int bkw   = (wkw * 3) / 5;  // black key width (~60%)

    // White keys (7), full height, thin dark seam between each - no border stroke
    for (int k = 0; k < 7; k++) {
        int midiNote = std::max(0, std::min(127, (ks.octave + 1) * 12 + WHITE_OFF[k]));

        lv_obj_t* btn = mcButton(pianoArea);
        lv_obj_set_pos(btn, k * wkw, 0);
        lv_obj_set_size(btn, wkw - wkGap, LV_PCT(100));
        lv_obj_set_style_bg_color(btn, keyColorWhiteDefault(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        // Without this, LVGL locks the press onto the first key touched and never
        // re-detects the object under the finger while sliding (see lv_indev.c
        // indev_proc_press: press-locked objects skip re-search) - breaks piano slide.
        lv_obj_set_press_lock(btn, false);
        lv_obj_set_user_data(btn, (void*)(intptr_t)midiNote);
        // Note On/Off via the row container's PRESSING/RELEASED handlers
        // but also handle individual taps via PRESSED/RELEASED on each key
        lv_obj_add_event_cb(btn, cbPianoPressing, LV_EVENT_PRESSED, mc);
        lv_obj_add_event_cb(btn, cbPianoReleased, LV_EVENT_RELEASED, mc);

        // Note label (C note shows octave, others show letter)
        lv_obj_t* lbl = lv_label_create(btn);
        if (k == 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "C%d", ks.octave);
            lv_label_set_text(lbl, buf);
        } else {
            lv_label_set_text(lbl, WHITE_NAME[k]);
        }
        lv_obj_set_style_text_font(lbl, fontSmall, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x555566), 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

        ks.whiteKey[k] = btn;
    }

    // Black keys (5), shorter, straddling the seam between white keys, on top
    for (int k = 0; k < 5; k++) {
        int midiNote = std::max(0, std::min(127, (ks.octave + 1) * 12 + BLACK_OFF[k]));
        int centerX  = (BLACK_FRAC[k] * wkw) / 100;
        int leftX    = centerX - bkw / 2;

        lv_obj_t* btn = mcButton(pianoArea);
        lv_obj_set_pos(btn, leftX, 0);
        lv_obj_set_size(btn, bkw, LV_PCT(60));
        lv_obj_set_style_bg_color(btn, keyColorBlackDefault(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_press_lock(btn, false);
        lv_obj_set_user_data(btn, (void*)(intptr_t)midiNote);
        lv_obj_add_event_cb(btn, cbPianoPressing, LV_EVENT_PRESSED, mc);
        lv_obj_add_event_cb(btn, cbPianoReleased, LV_EVENT_RELEASED, mc);
        lv_obj_move_foreground(btn);  // render on top of white keys

        ks.blackKey[k] = btn;
    }

    // -- Status bar ------------------------------------------------------------
    lv_obj_t* statusBar = mcContainer(root);
    lv_obj_set_size(statusBar, LV_PCT(100), statusH);
    lv_obj_set_style_bg_color(statusBar, lv_color_hex(0x0A0C14), 0);
    lv_obj_set_style_bg_opa(statusBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(statusBar, 0, 0);
    lv_obj_set_style_pad_all(statusBar, 8, 0);
    lv_obj_set_scrollable(statusBar, false);

    ks.statusLbl = lv_label_create(statusBar);
    lv_obj_set_style_text_font(ks.statusLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ks.statusLbl, lv_color_hex(0x666688), 0);
    lv_obj_center(ks.statusLbl);

    // Apply initial labels and default key colours
    rebuildKeyNotes(mc);
}
