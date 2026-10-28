#include "MidiController.h"

#include <lvgl/devices/pointer.h>
#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// GRID view - Isomorphic 8×5 Linnstrument-style grid
//
// Layout: perfect-4ths tuning - each row is 5 semitones higher, each column +1.
// Base note = (octave + 1) * 12 - at oct=3: baseNote=48 (C3).
// ---------------------------------------------------------------------------

static inline lv_coord_t dispW() {
    return (lv_coord_t)lv_display_get_horizontal_resolution(nullptr);
}
// Whether a semitone class is a "black" key
static bool isBlackKey(int semitone) {
    semitone = ((semitone % 12) + 12) % 12;
    return (semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10);
}

static lv_color_t cellTextColor(int note) {
    int sc12 = note % 12;
    if (isBlackKey(sc12)) return lv_color_hex(0x808080);  // dark cell - light text
    return lv_color_hex(0x000000);                         // C notes and white keys - dark text
}

static int gridNote(int row, int col, int baseNote) {
    // Row 0 = lowest, row 4 = highest (display top = row 4)
    int note = baseNote + (row * 5) + col;
    return std::max(0, std::min(127, note));
}

static bool isNotePressed(const MidiController::GridState& gs, int note) {
    for (int slot = 0; slot < LVGL_POINTER_MAX_SLOTS; slot++) {
        if (gs.pressedNote[slot] == note) return true;
    }
    return false;
}

static void refreshGridColors(MidiController* mc) {
    if (!mc) return;
    auto& gs = mc->grid_;
    int baseNote = (gs.octave + 1) * 12;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 8; c++) {
            if (!gs.cell[r][c]) continue;
            int note = gridNote(r, c, baseNote);
            int sc12 = note % 12;
            lv_color_t cellColor;
            if (isNotePressed(gs, note)) {
                cellColor = lv_color_hex(0x00DFFF);  // pressed
            } else if (sc12 == 0) {
                cellColor = lv_color_hex(0x00FFCC);  // C notes
            } else if (isBlackKey(sc12)) {
                cellColor = lv_color_hex(0x1A2A40);  // black key
            } else {
                cellColor = lv_color_hex(0x2A3A50);  // white key
            }
            lv_obj_set_style_bg_color(gs.cell[r][c], cellColor, LV_PART_MAIN);
            lv_obj_set_user_data(gs.cell[r][c], (void*)(intptr_t)note);
            // Update note name label (child 0)
            lv_obj_t* lbl = lv_obj_get_child(gs.cell[r][c], 0);
            if (lbl) {
                char buf[8]; midiNoteName(note, buf, sizeof(buf));
                lv_label_set_text(lbl, buf);
                lv_obj_set_style_text_color(lbl, cellTextColor(note), 0);
            }
        }
    }
}

static void refreshOctLabel(MidiController* mc) {
    if (!mc->grid_.octLbl) return;
    char buf[10]; snprintf(buf, sizeof(buf), "Oct %d", mc->grid_.octave);
    lv_label_set_text(mc->grid_.octLbl, buf);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbCellPressed(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& gs = mc->grid_;

    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    int8_t slotIdx = lvgl_pointer_get_slot_index(indev);
    if (slotIdx < 0) return;

    lv_obj_t* btn = lv_event_get_target_obj(e);
    int note = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (gs.pressedNote[slotIdx] >= 0) mc->sendMidi(0x80, (uint8_t)gs.pressedNote[slotIdx], 0);
    gs.pressedNote[slotIdx] = note;
    mc->sendMidi(0x90, (uint8_t)note, 100);
    refreshGridColors(mc);
    if (mc->grid_.noteLbl) {
        char buf[8]; midiNoteName(note, buf, sizeof(buf));
        lv_label_set_text(mc->grid_.noteLbl, buf);
    }
}

static void cbCellReleased(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& gs = mc->grid_;

    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    int8_t slotIdx = lvgl_pointer_get_slot_index(indev);
    if (slotIdx < 0) return;

    lv_obj_t* btn = lv_event_get_target_obj(e);
    int note = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (gs.pressedNote[slotIdx] == note) {
        mc->sendMidi(0x80, (uint8_t)note, 0);
        gs.pressedNote[slotIdx] = -1;
        refreshGridColors(mc);
        if (mc->grid_.noteLbl) lv_label_set_text(mc->grid_.noteLbl, "");
    }
}

static void releaseAllGridNotes(MidiController* mc) {
    auto& gs = mc->grid_;
    for (int slot = 0; slot < LVGL_POINTER_MAX_SLOTS; slot++) {
        if (gs.pressedNote[slot] >= 0) {
            mc->sendMidi(0x80, (uint8_t)gs.pressedNote[slot], 0);
            gs.pressedNote[slot] = -1;
        }
    }
    if (mc->grid_.noteLbl) lv_label_set_text(mc->grid_.noteLbl, "");
}

static void cbOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->grid_.octave > 1) {
        releaseAllGridNotes(mc);
        mc->grid_.octave--;
        refreshOctLabel(mc);
        refreshGridColors(mc);
    }
}

static void cbOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->grid_.octave < 6) {
        releaseAllGridNotes(mc);
        mc->grid_.octave++;
        refreshOctLabel(mc);
        refreshGridColors(mc);
    }
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildGridView(MidiController* mc, lv_obj_t* parent) {
    if (!mc) return;
    auto& gs = mc->grid_;
    for (auto& n : gs.pressedNote) n = -1;  // Ensure clean state when entering grid view

    lv_coord_t W = dispW();
    const lv_font_t* fontSmall = lvgl_get_text_font(FONT_SIZE_SMALL);
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

    mcBackBtn(ctrl, cbBack, mc, MC_W_S);
    lv_obj_t* titleLbl = lv_label_create(ctrl);
    lv_label_set_text(titleLbl, "GRID KEYS");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44BBFF), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    lv_obj_t* octDown = mcButton(ctrl);
    lv_obj_set_size(octDown, 112, btnH);
    lv_obj_add_event_cb(octDown, cbOctDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* odl = lv_label_create(octDown); lv_label_set_text(odl, "OCT-");
    lv_obj_set_style_text_font(odl, fontSmall, 0); lv_obj_center(odl);

    gs.octLbl = lv_label_create(ctrl);
    refreshOctLabel(mc);
    lv_obj_set_style_text_font(gs.octLbl, fontSmall, 0);
    lv_obj_set_style_text_color(gs.octLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_width(gs.octLbl, 176);
    lv_obj_set_style_text_align(gs.octLbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* octUp = mcButton(ctrl);
    lv_obj_set_size(octUp, 112, btnH);
    lv_obj_add_event_cb(octUp, cbOctUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* oul = lv_label_create(octUp); lv_label_set_text(oul, "OCT+");
    lv_obj_set_style_text_font(oul, fontSmall, 0); lv_obj_center(oul);

    gs.noteLbl = lv_label_create(ctrl);
    lv_label_set_text(gs.noteLbl, "");
    lv_obj_set_style_text_font(gs.noteLbl, fontSmall, 0);
    lv_obj_set_style_text_color(gs.noteLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_flex_grow(gs.noteLbl, 1);
    lv_obj_set_style_text_align(gs.noteLbl, LV_TEXT_ALIGN_RIGHT, 0);

    // -- Grid (5 rows × 8 cols, fills available space) -----------------------
    lv_obj_t* gridArea = mcContainer(root);
    lv_obj_set_width(gridArea, LV_PCT(100));
    lv_obj_set_flex_grow(gridArea, 1);
    lv_obj_set_style_bg_color(gridArea, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(gridArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gridArea, 0, 0);
    lv_obj_set_style_pad_top(gridArea, 8, 0);
    lv_obj_set_style_pad_bottom(gridArea, 16, 0);
    lv_obj_set_style_pad_left(gridArea, 16, 0);
    lv_obj_set_style_pad_right(gridArea, 16, 0);
    lv_obj_set_style_pad_gap(gridArea, 4, 0);
    lv_obj_set_scrollable(gridArea, false);
    lv_obj_set_flex_flow(gridArea, LV_FLEX_FLOW_COLUMN);

    int cellW = ((int)W - 16 * 2) / 8;

    // Build rows top to bottom: row 4 (highest) at top, row 0 at bottom
    for (int r = 4; r >= 0; r--) {
        lv_obj_t* rowObj = mcContainer(gridArea);
        lv_obj_set_width(rowObj, LV_PCT(100));
        lv_obj_set_flex_grow(rowObj, 1);
        lv_obj_set_flex_flow(rowObj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowObj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(rowObj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rowObj, 0, 0);
        lv_obj_set_style_pad_all(rowObj, 0, 0);
        lv_obj_set_style_pad_gap(rowObj, 4, 0);
        lv_obj_set_scrollable(rowObj, false);

        for (int c = 0; c < 8; c++) {
            lv_obj_t* btn = mcButton(rowObj);
            lv_obj_set_size(btn, cellW - 4, LV_PCT(100));
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3A50), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, cbCellPressed,  LV_EVENT_PRESSED,  mc);
            lv_obj_add_event_cb(btn, cbCellReleased, LV_EVENT_RELEASED, mc);

            // Note label (small, aligned center)
            int note = gridNote(r, c, (gs.octave + 1) * 12);
            lv_obj_set_user_data(btn, (void*)(intptr_t)note);
            {
                char buf[8]; midiNoteName(note, buf, sizeof(buf));
                lv_obj_t* lbl = lv_label_create(btn);
                lv_label_set_text(lbl, buf);
                lv_obj_set_style_text_font(lbl, fontSmall, 0);
                lv_obj_set_style_text_color(lbl, cellTextColor(note), 0);
                lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_clickable(lbl, false);
            }

            gs.cell[r][c] = btn;
        }
    }

    // Apply initial colors
    refreshGridColors(mc);
}
