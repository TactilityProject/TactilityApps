#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>

// ---------------------------------------------------------------------------
// BEATS view - 4-track 16-step drum sequencer
//
// Layout (flex column):
//   [transport row: BACK | PLAY/STOP | CLR | BPM- | BPM | BPM+ | "BPM"]
//   [step-marker header row (beat numbers 1/5/9/13)]
//   [4 × 16 step button grid - flex_grow, each row = track label + 16 buttons]
// ---------------------------------------------------------------------------

// Drum track names and colors
static const char* TRACK_NAMES[] = { "KICK", "SNRE", "HHAT", "OPEN" };

static lv_color_t trackColor(int t) {
    switch (t) {
        case 0:  return lv_color_hex(0xFF2020);  // red
        case 1:  return lv_color_hex(0xFFE000);  // yellow
        case 2:  return lv_color_hex(0x00DFFF);  // cyan
        case 3:  return lv_color_hex(0xCC44FF);  // purple
        default: return lv_color_hex(0x888888);
    }
}

static inline lv_coord_t dispW() {
    return (lv_coord_t)lv_display_get_horizontal_resolution(nullptr);
}

// Beat group separator: every 4 steps gets a slightly different shade
static bool isBeatBoundary(int step) { return (step % 4) == 0; }

static void refreshStepBtn(MidiController* mc, int track, int step) {
    lv_obj_t* btn = mc->beats_.stepBtn[track][step];
    if (!btn) return;
    bool active = mc->seq_ && mc->seq_->getStep(track, step);
    lv_obj_set_style_bg_color(btn,
        active ? trackColor(track) : lv_color_hex(0x1A2A40),
        LV_PART_MAIN);
    // Restore beat-boundary left border (overwritten by playback cursor highlight)
    if (isBeatBoundary(step)) {
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x334455), LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    }
}

static void refreshAllStepBtns(MidiController* mc) {
    for (int t = 0; t < Sequencer::NUM_TRACKS; t++) {
        for (int s = 0; s < Sequencer::NUM_STEPS; s++) {
            refreshStepBtn(mc, t, s);
        }
    }
}

static void refreshPlayBtn(MidiController* mc) {
    if (!mc->beats_.playLbl) return;
    lv_label_set_text(mc->beats_.playLbl,
        mc->beats_.playing ? "STOP" : "PLAY");
    lv_obj_set_style_bg_color(mc->beats_.playBtn,
        mc->beats_.playing ? lv_color_hex(0xFF2020) : lv_color_hex(0x00AA44),
        LV_PART_MAIN);
}

static void refreshBpmLbl(MidiController* mc) {
    if (!mc->beats_.bpmLbl) return;
    char buf[8]; snprintf(buf, sizeof(buf), "%d", mc->beats_.bpm);
    lv_label_set_text(mc->beats_.bpmLbl, buf);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbStepBtn(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || !mc->seq_) return;
    lv_obj_t* btn = lv_event_get_target_obj(e);
    int packed = (int)(intptr_t)lv_obj_get_user_data(btn);
    int track  = packed >> 8;
    int step   = packed & 0xFF;
    mc->seq_->toggleStep(track, step);
    refreshStepBtn(mc, track, step);
}

static void cbPlayStop(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || !mc->seq_) return;
    if (mc->beats_.playing) {
        mc->seq_->stop();
        mc->beats_.playing = false;
        mc->stopAllNotes();
        mc->seqPlayStep.store(-1);
        mc->seqLastStep = -1;
        refreshAllStepBtns(mc);  // self-heals borders and colors
    } else {
        mc->seqLastStep = -1;
        mc->seq_->start();
        mc->beats_.playing = true;
    }
    refreshPlayBtn(mc);
}

static void cbClear(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc || !mc->seq_) return;
    mc->seq_->clearAll();
    // clearAll() also stops the sequencer, so bring the transport UI back in sync.
    mc->beats_.playing = false;
    mc->seqPlayStep.store(-1);
    mc->seqLastStep = -1;
    mc->stopAllNotes();
    refreshPlayBtn(mc);
    refreshAllStepBtns(mc);
}

static void cbBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->beats_.bpm -= 5;
    if (mc->beats_.bpm < 40) mc->beats_.bpm = 40;
    if (mc->seq_) mc->seq_->setBpm((float)mc->beats_.bpm);
    refreshBpmLbl(mc);
}

static void cbBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->beats_.bpm += 5;
    if (mc->beats_.bpm > 300) mc->beats_.bpm = 300;
    if (mc->seq_) mc->seq_->setBpm((float)mc->beats_.bpm);
    refreshBpmLbl(mc);
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildBeatsView(MidiController* mc, lv_obj_t* parent) {
    auto& bs = mc->beats_;
    if (mc->seq_) mc->seq_->setBpm((float)bs.bpm);

    lv_coord_t W  = dispW();
    const lv_font_t* fontSmall = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int labelW  = 152;  // track label column width
    int ctrlH   = 88;  // transport row height (matches KEYS)
    int btnH    = MC_BTN_H;

    // Available width for step grid (excluding track label and row padding)
    lv_coord_t gridW = W - labelW - 8 * 2;  // row has pad_all=4 each side
    // Step button width: account for 15 gaps between 16 steps (pad_gap=4)
    int stepW = (gridW - 4 * 15) / Sequencer::NUM_STEPS;

    // Root
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

    // -- Transport row (TOP) --------------------------------
    lv_obj_t* transport = mcRow(root, ctrlH);

    // Back button
    mcBackBtn(transport, cbBack, mc, MC_W_S);

    lv_obj_t* titleLbl = lv_label_create(transport);
    lv_label_set_text(titleLbl, "BEATS");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44BBFF), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    // PLAY/STOP
    lv_obj_t* playBtn = mcButton(transport);
    lv_obj_set_size(playBtn, 176, btnH);
    lv_obj_set_style_bg_color(playBtn, lv_color_hex(0x00AA44), 0);
    lv_obj_add_event_cb(playBtn, cbPlayStop, LV_EVENT_CLICKED, mc);
    bs.playBtn = playBtn;
    lv_obj_t* playLbl = lv_label_create(playBtn);
    lv_label_set_text(playLbl, "PLAY");
    lv_obj_set_style_text_font(playLbl, fontSmall, 0);
    lv_obj_center(playLbl);
    bs.playLbl = playLbl;

    // CLEAR
    lv_obj_t* clearBtn = mcButton(transport);
    lv_obj_set_size(clearBtn, 144, btnH);
    lv_obj_set_style_bg_color(clearBtn, lv_color_hex(0x331A00), 0);
    lv_obj_add_event_cb(clearBtn, cbClear, LV_EVENT_CLICKED, mc);
    lv_obj_t* clearLbl = lv_label_create(clearBtn);
    lv_label_set_text(clearLbl, "CLEAR");
    lv_obj_set_style_text_font(clearLbl, fontSmall, 0);
    lv_obj_center(clearLbl);

    // BPM -
    lv_obj_t* bpmDown = mcButton(transport);
    lv_obj_set_size(bpmDown, 80, btnH);
    lv_obj_add_event_cb(bpmDown, cbBpmDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* bdLbl = lv_label_create(bpmDown);
    lv_label_set_text(bdLbl, "-");
    lv_obj_set_style_text_font(bdLbl, fontSmall, 0);
    lv_obj_center(bdLbl);

    // BPM label
    bs.bpmLbl = lv_label_create(transport);
    char bpmBuf[8]; snprintf(bpmBuf, sizeof(bpmBuf), "%d", bs.bpm);
    lv_label_set_text(bs.bpmLbl, bpmBuf);
    lv_obj_set_style_text_font(bs.bpmLbl, fontSmall, 0);
    lv_obj_set_style_text_color(bs.bpmLbl, lv_color_hex(0xFFE000), 0);
    lv_obj_set_width(bs.bpmLbl, 128);
    lv_obj_set_style_text_align(bs.bpmLbl, LV_TEXT_ALIGN_CENTER, 0);

    // BPM +
    lv_obj_t* bpmUp = mcButton(transport);
    lv_obj_set_size(bpmUp, 80, btnH);
    lv_obj_add_event_cb(bpmUp, cbBpmUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* buLbl = lv_label_create(bpmUp);
    lv_label_set_text(buLbl, "+");
    lv_obj_set_style_text_font(buLbl, fontSmall, 0);
    lv_obj_center(buLbl);

    // BPM unit label
    lv_obj_t* bpmUnit = lv_label_create(transport);
    lv_label_set_text(bpmUnit, "BPM");
    lv_obj_set_style_text_font(bpmUnit, fontSmall, 0);
    lv_obj_set_style_text_color(bpmUnit, lv_color_hex(0x666688), 0);

    // -- Track header row (step markers only) --------------------------------
    lv_obj_t* hdrRow = mcContainer(root);
    lv_obj_set_size(hdrRow, LV_PCT(100), 64);
    lv_obj_set_flex_flow(hdrRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdrRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(hdrRow, lv_color_hex(0x0A0C14), 0);
    lv_obj_set_style_bg_opa(hdrRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdrRow, 0, 0);
    lv_obj_set_style_pad_all(hdrRow, 4, 0);
    lv_obj_set_style_pad_gap(hdrRow, 0, 0);
    lv_obj_set_scrollable(hdrRow, false);

    // Spacer to align step markers with step buttons below
    lv_obj_t* hdrSpacer = mcContainer(hdrRow);
    lv_obj_set_size(hdrSpacer, labelW + 4, 1);
    lv_obj_set_style_bg_opa(hdrSpacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdrSpacer, 0, 0);

    // Step index labels (beat markers: 1, 5, 9, 13)
    for (int s = 0; s < Sequencer::NUM_STEPS; s++) {
        if (s % 4 == 0) {
            lv_obj_t* marker = lv_label_create(hdrRow);
            char buf[4]; snprintf(buf, sizeof(buf), "%d", s + 1);
            lv_label_set_text(marker, buf);
            lv_obj_set_width(marker, stepW * 4 + 4 * 3);
            lv_obj_set_style_text_font(marker, fontSmall, 0);
            lv_obj_set_style_text_color(marker, lv_color_hex(0x666688), 0);
            lv_obj_set_style_text_align(marker, LV_TEXT_ALIGN_LEFT, 0);
        }
    }

    // -- Step grid (4 tracks × 16 steps) --------------------------------
    lv_obj_t* gridArea = mcContainer(root);
    lv_obj_set_width(gridArea, LV_PCT(100));
    lv_obj_set_flex_grow(gridArea, 1);
    lv_obj_set_style_bg_color(gridArea, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(gridArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gridArea, 0, 0);
    lv_obj_set_style_pad_all(gridArea, 0, 0);
    lv_obj_set_style_pad_gap(gridArea, 0, 0);
    lv_obj_set_scrollable(gridArea, false);
    lv_obj_set_flex_flow(gridArea, LV_FLEX_FLOW_COLUMN);

    for (int t = 0; t < Sequencer::NUM_TRACKS; t++) {
        // Track row
        lv_obj_t* trackRow = mcContainer(gridArea);
        lv_obj_set_width(trackRow, LV_PCT(100));
        lv_obj_set_flex_grow(trackRow, 1);
        lv_obj_set_flex_flow(trackRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(trackRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(trackRow, lv_color_hex(0x080C12), 0);
        lv_obj_set_style_bg_opa(trackRow, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(trackRow, 0, 0);
        lv_obj_set_style_pad_all(trackRow, 4, 0);
        lv_obj_set_style_pad_gap(trackRow, 4, 0);
        lv_obj_set_scrollable(trackRow, false);

        // Track label
        lv_obj_t* trackLbl = mcContainer(trackRow);
        lv_obj_set_size(trackLbl, labelW, LV_PCT(100));
        lv_obj_set_style_bg_color(trackLbl, lv_color_hex(0x0A0C14), 0);
        lv_obj_set_style_bg_opa(trackLbl, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(trackLbl, 2, 0);
        lv_obj_set_style_border_side(trackLbl, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(trackLbl, trackColor(t), 0);
        lv_obj_set_style_pad_all(trackLbl, 8, 0);
        lv_obj_set_scrollable(trackLbl, false);

        lv_obj_t* tNameLbl = lv_label_create(trackLbl);
        lv_label_set_text(tNameLbl, TRACK_NAMES[t]);
        lv_obj_set_style_text_font(tNameLbl, fontSmall, 0);
        lv_obj_set_style_text_color(tNameLbl, trackColor(t), 0);
        lv_obj_center(tNameLbl);

        // 16 step buttons
        for (int s = 0; s < Sequencer::NUM_STEPS; s++) {
            lv_obj_t* btn = mcButton(trackRow);
            lv_obj_set_size(btn, stepW, LV_PCT(90));
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2A40), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(btn, 2, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_set_style_border_width(btn, 0, 0);

            // Beat boundary: slight highlight on every 4th step column
            if (isBeatBoundary(s)) {
                lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
                lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
                lv_obj_set_style_border_color(btn, lv_color_hex(0x334455), LV_PART_MAIN);
            }

            // Pack track+step into user_data
            lv_obj_set_user_data(btn, (void*)(intptr_t)((t << 8) | s));
            lv_obj_add_event_cb(btn, cbStepBtn, LV_EVENT_CLICKED, mc);

            bs.stepBtn[t][s] = btn;
        }
    }

    // Sync UI state with current playback status
    refreshPlayBtn(mc);
    refreshAllStepBtns(mc);
}
