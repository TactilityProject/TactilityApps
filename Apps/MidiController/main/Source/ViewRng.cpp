#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstdio>
#include <cmath>

// ---------------------------------------------------------------------------
// RNG view - Random melody generator
//
// All controls, no canvas.
// Layout (flex column):
//   [header row]
//   [play controls: PLAY/STOP | Key -/+/label | Scale]
//   [octave range: MinOct -/label/+ | MaxOct -/label/+]
//   [probability: Prob -/bar/+]
//   [BPM + subdivision]
//   [current note display - flex_grow]
// ---------------------------------------------------------------------------

static const char* SUBDIV_NAMES[] = { "1/4", "1/8", "1/16" };
static const int   SUBDIV_DIVS[]  = { 1,     2,     4     };


static uint64_t rngNoteIntervalUs(MidiController* mc) {
    auto& rn = mc->rng_;
    int idx = rn.subdivision % (sizeof(SUBDIV_DIVS) / sizeof(SUBDIV_DIVS[0]));
    float quarterUs = 60000000.0f / (float)rn.bpm;
    return (uint64_t)(quarterUs / (float)SUBDIV_DIVS[idx]);
}

static void cbRngTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc || !mc->rng_.playing) return;
    auto& rn = mc->rng_;

    uint64_t nowUs = (uint64_t)esp_timer_get_time();

    // Handle note-off
    if (rn.noteOffUs > 0 && nowUs >= rn.noteOffUs) {
        if (rn.currentNote >= 0) {
            mc->sendMidi(0x80, (uint8_t)rn.currentNote, 0);
            rn.currentNote = -1;
        }
        rn.noteOffUs = 0;
    }

    // Trigger next note
    if (nowUs >= rn.nextNoteUs) {
        uint64_t interval = rngNoteIntervalUs(mc);
        rn.nextNoteUs = nowUs + interval;

        if ((int)(esp_random() % 100) < rn.probability) {
            // Pick random degree within scale
            int numDegrees = MIDI_SCALES[rn.scaleIdx % NUM_MIDI_SCALES].numNotes;
            int degree = (int)(esp_random() % (uint32_t)numDegrees);
            int octRange = rn.maxOctave - rn.minOctave + 1;
            if (octRange < 1) octRange = 1;
            int octave = rn.minOctave + (int)(esp_random() % (uint32_t)octRange);
            int note = getNoteInScale(rn.scaleIdx, degree, octave, rn.rootKey);

            // Note off for previous if still playing
            if (rn.currentNote >= 0) {
                mc->sendMidi(0x80, (uint8_t)rn.currentNote, 0);
            }

            mc->sendMidi(0x90, (uint8_t)note, 80 + (int)(esp_random() % 40));
            rn.currentNote = note;
            rn.noteOffUs   = nowUs + interval * 3 / 4;  // note length = 75% of interval

            if (rn.noteLbl) {
                char buf[16];
                char noteName[8];
                midiNoteName(note, noteName, sizeof(noteName));
                snprintf(buf, sizeof(buf), "%s", noteName);
                lv_label_set_text(rn.noteLbl, buf);
            }
        } else {
            // Rest - no note
            if (rn.noteLbl) lv_label_set_text(rn.noteLbl, "( rest )");
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

static void cbPlayStop(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rn = mc->rng_;
    rn.playing = !rn.playing;
    if (rn.playing) {
        rn.nextNoteUs = (uint64_t)esp_timer_get_time();
        rn.noteOffUs  = 0;
        rn.currentNote = -1;
        if (!mc->rngTimer_) {
            mc->rngTimer_ = lv_timer_create(cbRngTimer, 10, mc);
        }
    } else {
        if (rn.currentNote >= 0) {
            mc->sendMidi(0x80, (uint8_t)rn.currentNote, 0);
            rn.currentNote = -1;
        }
        if (mc->rngTimer_) { lv_timer_delete(mc->rngTimer_); mc->rngTimer_ = nullptr; }
        if (rn.noteLbl) lv_label_set_text(rn.noteLbl, "");
    }
    if (rn.playLbl) lv_label_set_text(rn.playLbl, rn.playing ? "STOP" : "PLAY");
    if (rn.playBtn) {
        lv_obj_set_style_bg_color(rn.playBtn,
            rn.playing ? lv_color_hex(0xFF2020) : lv_color_hex(0x00AA44), 0);
    }
}

static void cbKeyDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.rootKey = (mc->rng_.rootKey + 11) % 12;
    if (mc->rng_.keyLbl) lv_label_set_text(mc->rng_.keyLbl, KEY_NAMES[mc->rng_.rootKey]);
}
static void cbKeyUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.rootKey = (mc->rng_.rootKey + 1) % 12;
    if (mc->rng_.keyLbl) lv_label_set_text(mc->rng_.keyLbl, KEY_NAMES[mc->rng_.rootKey]);
}

static void cbScaleCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.scaleIdx = (mc->rng_.scaleIdx + 1) % NUM_MIDI_SCALES;
    if (mc->rng_.scaleLbl) lv_label_set_text(mc->rng_.scaleLbl, MIDI_SCALES[mc->rng_.scaleIdx].name);
}

static void cbMinOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.minOctave > 1) mc->rng_.minOctave--;
    if (mc->rng_.minOctLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->rng_.minOctave);
        lv_label_set_text(mc->rng_.minOctLbl, buf);
    }
}
static void cbMinOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.minOctave < mc->rng_.maxOctave) mc->rng_.minOctave++;
    if (mc->rng_.minOctLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->rng_.minOctave);
        lv_label_set_text(mc->rng_.minOctLbl, buf);
    }
}
static void cbMaxOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.maxOctave > mc->rng_.minOctave) mc->rng_.maxOctave--;
    if (mc->rng_.maxOctLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->rng_.maxOctave);
        lv_label_set_text(mc->rng_.maxOctLbl, buf);
    }
}
static void cbMaxOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.maxOctave < 8) mc->rng_.maxOctave++;
    if (mc->rng_.maxOctLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->rng_.maxOctave);
        lv_label_set_text(mc->rng_.maxOctLbl, buf);
    }
}

static void cbProbDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.probability -= 5; if (mc->rng_.probability < 0) mc->rng_.probability = 0;
    if (mc->rng_.probLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", mc->rng_.probability);
        lv_label_set_text(mc->rng_.probLbl, buf);
    }
}
static void cbProbUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.probability += 5; if (mc->rng_.probability > 100) mc->rng_.probability = 100;
    if (mc->rng_.probLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", mc->rng_.probability);
        lv_label_set_text(mc->rng_.probLbl, buf);
    }
}

static void cbBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.bpm -= 5; if (mc->rng_.bpm < 40) mc->rng_.bpm = 40;
    if (mc->rng_.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", mc->rng_.bpm);
        lv_label_set_text(mc->rng_.bpmLbl, buf);
    }
}
static void cbBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->rng_.bpm += 5; if (mc->rng_.bpm > 300) mc->rng_.bpm = 300;
    if (mc->rng_.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", mc->rng_.bpm);
        lv_label_set_text(mc->rng_.bpmLbl, buf);
    }
}

static void cbSubdivDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.subdivision > 0) mc->rng_.subdivision--;
    if (mc->rng_.subdivLbl) lv_label_set_text(mc->rng_.subdivLbl, SUBDIV_NAMES[mc->rng_.subdivision]);
}
static void cbSubdivUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->rng_.subdivision < 2) mc->rng_.subdivision++;
    if (mc->rng_.subdivLbl) lv_label_set_text(mc->rng_.subdivLbl, SUBDIV_NAMES[mc->rng_.subdivision]);
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildRngView(MidiController* mc, lv_obj_t* parent) {
    auto& rn = mc->rng_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);
    const lv_font_t* fontLarge  = lvgl_get_text_font(FONT_SIZE_LARGE);

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 1, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    // Helper to make a row
    auto makeRow = [&]() -> lv_obj_t* { return mcRow(root); };

    auto makeBtn = [&](lv_obj_t* parent_, const char* txt, int w, lv_event_cb_t cb) {
        mcSmallBtn(parent_, txt, w, cb, mc);
    };

    auto makeLbl = [&](lv_obj_t* parent_, const char* txt, int w, lv_color_t col) -> lv_obj_t* {
        return mcValue(parent_, txt, w, col);
    };

    auto makeHdr = [&](lv_obj_t* parent_, const char* txt) {
        mcHead(parent_, txt);
    };

    // -- Row 1: header + play -------------------------------------------------
    lv_obj_t* row1 = makeRow();
    mcBackBtn(row1, cbBack, mc, MC_W_M);
    lv_obj_t* titleLbl = lv_label_create(row1);
    lv_label_set_text(titleLbl, "RNG JAMS");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44FF88), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    rn.playBtn = mcButton(row1);
    lv_obj_set_size(rn.playBtn, 224, MC_BTN_H);
    lv_obj_set_style_bg_color(rn.playBtn, lv_color_hex(0x00AA44), 0);
    lv_obj_add_event_cb(rn.playBtn, cbPlayStop, LV_EVENT_CLICKED, mc);
    rn.playLbl = lv_label_create(rn.playBtn);
    lv_label_set_text(rn.playLbl, "PLAY");
    lv_obj_set_style_text_font(rn.playLbl, fontSmall, 0);
    lv_obj_center(rn.playLbl);

    // -- Row 2: Key + Scale ---------------------------------------------------
    lv_obj_t* row2 = makeRow();
    makeHdr(row2, "Key:");
    makeBtn(row2, "-", 96, cbKeyDown);
    rn.keyLbl = makeLbl(row2, KEY_NAMES[rn.rootKey], 112, MC_VALUE());
    makeBtn(row2, "+", 96, cbKeyUp);

    makeHdr(row2, " Scale:");
    lv_obj_t* scaleBtn = mcButton(row2);
    lv_obj_set_size(scaleBtn, 240, MC_BTN_H);
    lv_obj_add_event_cb(scaleBtn, cbScaleCycle, LV_EVENT_CLICKED, mc);
    rn.scaleLbl = lv_label_create(scaleBtn);
    lv_label_set_text(rn.scaleLbl, MIDI_SCALES[rn.scaleIdx % NUM_MIDI_SCALES].name);
    lv_obj_set_style_text_font(rn.scaleLbl, fontSmall, 0);
    lv_obj_center(rn.scaleLbl);

    // -- Row 3: Oct range -----------------------------------------------------
    lv_obj_t* row3 = makeRow();
    makeHdr(row3, "Min:");
    makeBtn(row3, "-", 96, cbMinOctDown);
    char mob[4]; snprintf(mob, sizeof(mob), "%d", rn.minOctave);
    rn.minOctLbl = makeLbl(row3, mob, 96, lv_color_hex(0xFFFFFF));
    makeBtn(row3, "+", 96, cbMinOctUp);

    makeHdr(row3, " Max:");
    makeBtn(row3, "-", 96, cbMaxOctDown);
    char maxb[4]; snprintf(maxb, sizeof(maxb), "%d", rn.maxOctave);
    rn.maxOctLbl = makeLbl(row3, maxb, 96, lv_color_hex(0xFFFFFF));
    makeBtn(row3, "+", 96, cbMaxOctUp);

    // -- Row 4: Probability ---------------------------------------------------
    lv_obj_t* row4 = makeRow();
    makeHdr(row4, "Prob:");
    makeBtn(row4, "-", 96, cbProbDown);
    char pb[8]; snprintf(pb, sizeof(pb), "%d%%", rn.probability);
    rn.probLbl = makeLbl(row4, pb, 176, lv_color_hex(0xFFE000));
    makeBtn(row4, "+", 96, cbProbUp);

    // -- Row 5: BPM + Subdivision ---------------------------------------------
    lv_obj_t* row5 = makeRow();
    makeHdr(row5, "BPM:");
    makeBtn(row5, "-", 96, cbBpmDown);
    char bpmb[8]; snprintf(bpmb, sizeof(bpmb), "%d", rn.bpm);
    rn.bpmLbl = makeLbl(row5, bpmb, 176, lv_color_hex(0x00DFFF));
    makeBtn(row5, "+", 96, cbBpmUp);

    makeHdr(row5, " Beat:");
    makeBtn(row5, "<", 96, cbSubdivDown);
    rn.subdivLbl = makeLbl(row5, SUBDIV_NAMES[rn.subdivision], 144, lv_color_hex(0x00DFFF));
    makeBtn(row5, ">", 96, cbSubdivUp);

    // -- Note display (fills remaining space) --------------------------------
    lv_obj_t* noteArea = mcContainer(root);
    lv_obj_set_width(noteArea, LV_PCT(100));
    lv_obj_set_flex_grow(noteArea, 1);
    lv_obj_set_style_bg_color(noteArea, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(noteArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(noteArea, 0, 0);
    lv_obj_set_style_pad_all(noteArea, 0, 0);
    lv_obj_set_scrollable(noteArea, false);

    rn.noteLbl = lv_label_create(noteArea);
    lv_label_set_text(rn.noteLbl, "");
    lv_obj_set_style_text_font(rn.noteLbl, fontLarge, 0);
    lv_obj_set_style_text_color(rn.noteLbl, lv_color_hex(0x44FF88), 0);
    lv_obj_center(rn.noteLbl);
}
