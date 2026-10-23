#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// ARP view - Piano-triggered chord arpeggiator
//
// Layout (flex column):
//   [header: < | ARP]
//   [controls: Pattern | Chord | Octaves | Speed | BPM]
//   [status label]
//   [12-key chromatic piano row]
// ---------------------------------------------------------------------------

static const char* ARP_PATTERNS[] = { "UP", "DOWN", "UP/DN", "RAND", "CHANCE" };
static const char* CHORD_NAMES[]  = { "MAJOR", "MINOR", "7TH" };
// Chord intervals relative to root (in semitones)
static const int CHORD_INTERVALS[3][4] = {
    { 0,  4,  7, -1 },  // MAJ
    { 0,  3,  7, -1 },  // MIN
    { 0,  4,  7, 10 },  // 7TH
};
static const char* SPEED_NAMES[] = { "1/4", "1/8", "1/16", "1/32" };
static const int   SPEED_DIVS[]  = { 1,     2,     4,      8    };  // subdivisions of quarter note
static const bool  IS_SHARP[12]  = { false,true,false,true,false,false,true,false,true,false,true,false };

static inline lv_coord_t dispW() {
    return (lv_coord_t)lv_display_get_horizontal_resolution(nullptr);
}

// ---------------------------------------------------------------------------
// Arpeggio timer
// ---------------------------------------------------------------------------

static int getArpNoteCount(MidiController* mc) {
    auto& ap = mc->arp_;
    const int* iv = CHORD_INTERVALS[ap.chordType];
    int count = 0;
    while (count < 4 && iv[count] >= 0) count++;
    return count * ap.octaveRange;
}

static int getArpNote(MidiController* mc, int step) {
    auto& ap = mc->arp_;
    if (ap.triggeredNote < 0) return -1;

    const int* iv = CHORD_INTERVALS[ap.chordType];
    int numIntervals = 0;
    while (numIntervals < 4 && iv[numIntervals] >= 0) numIntervals++;
    int total = numIntervals * ap.octaveRange;
    if (total <= 0) return ap.triggeredNote;

    step = step % total;
    int octOffset  = step / numIntervals;
    int intOffset  = step % numIntervals;
    int note = ap.triggeredNote + iv[intOffset] + octOffset * 12;
    return std::max(0, std::min(127, note));
}

static void cbArpTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc || !mc->arp_.isPlaying) return;
    auto& ap = mc->arp_;

    if (ap.triggeredNote < 0) return;

    int total = getArpNoteCount(mc);
    if (total <= 0) return;

    // Note off for previous note
    if (ap.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ap.lastNote, 0);
        ap.lastNote = -1;
    }

    int note = -1;
    switch (ap.pattern) {
        case 0: // UP
            note = getArpNote(mc, ap.arpStep);
            ap.arpStep = (ap.arpStep + 1) % total;
            break;
        case 1: // DOWN
            note = getArpNote(mc, total - 1 - ap.arpStep);
            ap.arpStep = (ap.arpStep + 1) % total;
            break;
        case 2: // UP/DN
            note = getArpNote(mc, ap.arpStep);
            ap.arpStep += ap.arpDir;
            if (ap.arpStep >= total) { ap.arpStep = std::max(0, total - 2); ap.arpDir = -1; }
            if (ap.arpStep < 0)      { ap.arpStep = std::min(1, total - 1); ap.arpDir = 1; }
            break;
        case 3: // RAND
            note = getArpNote(mc, (int)(esp_random() % (uint32_t)total));
            break;
        case 4: // CHANCE - UP with 30% skip
            if ((esp_random() % 100) >= 30) {
                note = getArpNote(mc, ap.arpStep);
            }
            ap.arpStep = (ap.arpStep + 1) % total;
            break;
    }

    if (note >= 0) {
        mc->sendMidi(0x90, (uint8_t)note, 100);
        ap.lastNote = note;
    }

    // Update note display
    if (ap.noteLbl && note >= 0) {
        char buf[8];
        midiNoteName(note, buf, sizeof(buf));
        lv_label_set_text(ap.noteLbl, buf);
    }
}

static void updateArpTimer(MidiController* mc) {
    auto& ap = mc->arp_;
    if (mc->arpTimer_) {
        // Interval: quarter note at bpm / subdivision factor
        uint32_t intervalMs = (uint32_t)(60000.0f / ap.bpm / SPEED_DIVS[ap.speedIdx]);
        if (intervalMs < 20) intervalMs = 20;
        lv_timer_set_period(mc->arpTimer_, intervalMs);
    }
}

static void startArp(MidiController* mc, int rootNote) {
    auto& ap = mc->arp_;
    ap.triggeredNote = rootNote;
    ap.arpStep = 0;
    ap.arpDir  = 1;
    ap.isPlaying = true;

    uint32_t intervalMs = (uint32_t)(60000.0f / ap.bpm / SPEED_DIVS[ap.speedIdx]);
    if (intervalMs < 20) intervalMs = 20;

    if (!mc->arpTimer_) {
        mc->arpTimer_ = lv_timer_create(cbArpTimer, intervalMs, mc);
    } else {
        lv_timer_set_period(mc->arpTimer_, intervalMs);
        lv_timer_reset(mc->arpTimer_);
    }

    if (ap.statusLbl) {
        char buf[24];
        char noteName[8];
        midiNoteName(rootNote, noteName, sizeof(noteName));
        snprintf(buf, sizeof(buf), "Arping: %s %s", noteName, CHORD_NAMES[ap.chordType]);
        lv_label_set_text(ap.statusLbl, buf);
    }
}

static void stopArp(MidiController* mc) {
    auto& ap = mc->arp_;
    ap.isPlaying = false;
    ap.triggeredNote = -1;
    if (ap.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ap.lastNote, 0);
        ap.lastNote = -1;
    }
    if (ap.noteLbl) lv_label_set_text(ap.noteLbl, "");
    if (ap.statusLbl) lv_label_set_text(ap.statusLbl, "Tap a key to start");
}

// ---------------------------------------------------------------------------
// Control callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbPatternPrev(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->arp_.pattern = (mc->arp_.pattern + 4) % 5;
    if (mc->arp_.patternLbl) lv_label_set_text(mc->arp_.patternLbl, ARP_PATTERNS[mc->arp_.pattern]);
}
static void cbPatternNext(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->arp_.pattern = (mc->arp_.pattern + 1) % 5;
    if (mc->arp_.patternLbl) lv_label_set_text(mc->arp_.patternLbl, ARP_PATTERNS[mc->arp_.pattern]);
}

static void cbChordCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->arp_.chordType = (mc->arp_.chordType + 1) % 3;
    if (mc->arp_.chordLbl) lv_label_set_text(mc->arp_.chordLbl, CHORD_NAMES[mc->arp_.chordType]);
}

static void cbOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.octaveRange > 1) mc->arp_.octaveRange--;
    if (mc->arp_.octLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->arp_.octaveRange);
        lv_label_set_text(mc->arp_.octLbl, buf);
    }
}
static void cbOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.octaveRange < 4) mc->arp_.octaveRange++;
    if (mc->arp_.octLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->arp_.octaveRange);
        lv_label_set_text(mc->arp_.octLbl, buf);
    }
}

static void cbSpeedDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.speedIdx > 0) mc->arp_.speedIdx--;
    if (mc->arp_.speedLbl) lv_label_set_text(mc->arp_.speedLbl, SPEED_NAMES[mc->arp_.speedIdx]);
    updateArpTimer(mc);
}
static void cbSpeedUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.speedIdx < 3) mc->arp_.speedIdx++;
    if (mc->arp_.speedLbl) lv_label_set_text(mc->arp_.speedLbl, SPEED_NAMES[mc->arp_.speedIdx]);
    updateArpTimer(mc);
}

static void cbBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->arp_.bpm -= 5; if (mc->arp_.bpm < 40) mc->arp_.bpm = 40;
    if (mc->arp_.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", mc->arp_.bpm);
        lv_label_set_text(mc->arp_.bpmLbl, buf);
    }
    updateArpTimer(mc);
}
static void cbBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->arp_.bpm += 5; if (mc->arp_.bpm > 300) mc->arp_.bpm = 300;
    if (mc->arp_.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", mc->arp_.bpm);
        lv_label_set_text(mc->arp_.bpmLbl, buf);
    }
    updateArpTimer(mc);
}

static void updatePianoOctLabel(MidiController* mc) {
    if (mc->arp_.pianoOctLbl) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", mc->arp_.pianoOctave);
        lv_label_set_text(mc->arp_.pianoOctLbl, buf);
    }
}

static void rebuildPianoKeyNotes(MidiController* mc) {
    auto& ap = mc->arp_;
    // Reset all key highlights before re-assigning notes (octave change invalidates old highlight)
    for (int k = 0; k < 12; k++) {
        if (!ap.pianoKey[k]) continue;
        bool isSharp = IS_SHARP[k];
        lv_obj_set_style_bg_color(ap.pianoKey[k],
            isSharp ? lv_color_hex(0xDDDDCC) : lv_color_hex(0x111111),
            LV_PART_MAIN);
    }
    ap.triggeredNote = -1;  // old note no longer valid in new octave
    stopArp(mc);

    for (int k = 0; k < 12; k++) {
        if (!ap.pianoKey[k]) continue;
        int note = std::max(0, std::min(127, (ap.pianoOctave + 1) * 12 + k));
        lv_obj_set_user_data(ap.pianoKey[k], (void*)(intptr_t)note);
        // Update label (child 0)
        lv_obj_t* lbl = lv_obj_get_child(ap.pianoKey[k], 0);
        if (lbl) {
            char noteName[8]; midiNoteName(note, noteName, sizeof(noteName));
            lv_label_set_text(lbl, noteName);
        }
    }
    updatePianoOctLabel(mc);
}

static void cbPianoOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.pianoOctave > 1) mc->arp_.pianoOctave--;
    rebuildPianoKeyNotes(mc);
}
static void cbPianoOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->arp_.pianoOctave < 7) mc->arp_.pianoOctave++;
    rebuildPianoKeyNotes(mc);
}

static void cbPianoKey(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ap = mc->arp_;

    lv_obj_t* btn = lv_event_get_target_obj(e);
    int note = (int)(intptr_t)lv_obj_get_user_data(btn);

    // isSharp: sharps (C#,D#,F#,G#,A#) are light keys
    auto keyColor = [](int semitone) -> uint32_t {
        return IS_SHARP[semitone] ? 0xDDDDCC : 0x111111;
    };

    if (ap.triggeredNote == note && ap.isPlaying) {
        // Tap same key to stop
        stopArp(mc);
        lv_obj_set_style_bg_color(btn, lv_color_hex(keyColor(note % 12)), LV_PART_MAIN);
    } else {
        // Reset previously highlighted key
        if (ap.triggeredNote >= 0) {
            int prevKey = ap.triggeredNote % 12;
            if (ap.pianoKey[prevKey]) {
                lv_obj_set_style_bg_color(ap.pianoKey[prevKey],
                    lv_color_hex(keyColor(prevKey)),
                    LV_PART_MAIN);
            }
        }
        startArp(mc, note);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x00DFFF), LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

static void cbArpRootDelete(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ap = mc->arp_;

    // Stop and delete the arp timer so it cannot fire after the widgets are gone
    if (mc->arpTimer_) {
        lv_timer_delete(mc->arpTimer_);
        mc->arpTimer_ = nullptr;
    }
    stopArp(mc);

    // Null all stored LVGL pointers - objects are being destroyed
    ap.noteLbl    = nullptr;
    ap.statusLbl  = nullptr;
    ap.pianoOctLbl = nullptr;
    for (int k = 0; k < 12; k++) ap.pianoKey[k] = nullptr;
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildArpView(MidiController* mc, lv_obj_t* parent) {
    auto& ap = mc->arp_;

    lv_coord_t W = dispW();
    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int hdrH    = MC_HDR_H;
    int ctrlH   = 88;
    int statusH = 56;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_add_event_cb(root, cbArpRootDelete, LV_EVENT_DELETE, mc);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 1, 0);
    lv_obj_set_scrollable(root, false);
    mc->currentViewRoot_ = root;

    // Helper lambda to create a control row
    auto makeRow = [&](int h) -> lv_obj_t* { return mcRow(root, h); };

    auto makeSmallBtn = [&](lv_obj_t* parent_, const char* txt, lv_event_cb_t cb) -> lv_obj_t* {
        return mcSmallBtn(parent_, txt, MC_W_S, cb, mc);
    };

    auto makeLbl = [&](lv_obj_t* parent_, const char* txt, int w, lv_color_t col) -> lv_obj_t* {
        return mcValue(parent_, txt, w, col);
    };

    // -- Header row -----------------------------------------------------------
    lv_obj_t* hdr = makeRow(hdrH);
    mcBackBtn(hdr, cbBack, mc, MC_W_M);

    lv_obj_t* titleLbl = lv_label_create(hdr);
    lv_label_set_text(titleLbl, "ARPEGGIATOR");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xFF8000), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    // -- Pattern row ---------------------------------------------------------
    lv_obj_t* patRow = makeRow(ctrlH);
    lv_obj_t* patLblHead = lv_label_create(patRow);
    lv_label_set_text(patLblHead, "PTRN:");
    lv_obj_set_style_text_font(patLblHead, fontSmall, 0);
    lv_obj_set_style_text_color(patLblHead, lv_color_hex(0x666688), 0);
    makeSmallBtn(patRow, "<", cbPatternPrev);
    ap.patternLbl = makeLbl(patRow, ARP_PATTERNS[ap.pattern], 144, lv_color_hex(0x00DFFF));
    makeSmallBtn(patRow, ">", cbPatternNext);

    lv_obj_t* sep1 = lv_label_create(patRow);
    lv_label_set_text(sep1, " CHORD:");
    lv_obj_set_style_text_font(sep1, fontSmall, 0);
    lv_obj_set_style_text_color(sep1, lv_color_hex(0x666688), 0);
    lv_obj_t* chordBtn = mcButton(patRow);
    lv_obj_set_size(chordBtn, 144, MC_BTN_H);
    lv_obj_add_event_cb(chordBtn, cbChordCycle, LV_EVENT_CLICKED, mc);
    ap.chordLbl = lv_label_create(chordBtn);
    lv_label_set_text(ap.chordLbl, CHORD_NAMES[ap.chordType]);
    lv_obj_set_style_text_font(ap.chordLbl, fontSmall, 0);
    lv_obj_center(ap.chordLbl);

    // -- Octave / Speed / BPM row ---------------------------------------------
    lv_obj_t* paramRow = makeRow(ctrlH);
    lv_obj_t* octHead = lv_label_create(paramRow);
    lv_label_set_text(octHead, "OCT:");
    lv_obj_set_style_text_font(octHead, fontSmall, 0);
    lv_obj_set_style_text_color(octHead, lv_color_hex(0x666688), 0);
    makeSmallBtn(paramRow, "-", cbOctDown);
    char octBuf[4]; snprintf(octBuf, sizeof(octBuf), "%d", ap.octaveRange);
    ap.octLbl = makeLbl(paramRow, octBuf, 80, lv_color_hex(0xFFFFFF));
    makeSmallBtn(paramRow, "+", cbOctUp);

    lv_obj_t* spdHead = lv_label_create(paramRow);
    lv_label_set_text(spdHead, " SPD:");
    lv_obj_set_style_text_font(spdHead, fontSmall, 0);
    lv_obj_set_style_text_color(spdHead, lv_color_hex(0x666688), 0);
    makeSmallBtn(paramRow, "<", cbSpeedDown);
    ap.speedLbl = makeLbl(paramRow, SPEED_NAMES[ap.speedIdx], 144, lv_color_hex(0x00DFFF));
    makeSmallBtn(paramRow, ">", cbSpeedUp);

    lv_obj_t* bpmHead = lv_label_create(paramRow);
    lv_label_set_text(bpmHead, " BPM:");
    lv_obj_set_style_text_font(bpmHead, fontSmall, 0);
    lv_obj_set_style_text_color(bpmHead, lv_color_hex(0x666688), 0);
    makeSmallBtn(paramRow, "-", cbBpmDown);
    char bpmBuf[8]; snprintf(bpmBuf, sizeof(bpmBuf), "%d", ap.bpm);
    ap.bpmLbl = makeLbl(paramRow, bpmBuf, 160, lv_color_hex(0xFFE000));
    makeSmallBtn(paramRow, "+", cbBpmUp);

    // -- Status + note label row -----------------------------------------------
    lv_obj_t* statusRow = mcContainer(root);
    lv_obj_set_size(statusRow, LV_PCT(100), statusH);
    lv_obj_set_flex_flow(statusRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(statusRow, lv_color_hex(0x0A0C14), 0);
    lv_obj_set_style_bg_opa(statusRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(statusRow, 0, 0);
    lv_obj_set_style_pad_all(statusRow, 8, 0);
    lv_obj_set_scrollable(statusRow, false);

    ap.statusLbl = lv_label_create(statusRow);
    lv_label_set_text(ap.statusLbl, "Tap a key to start");
    lv_obj_set_style_text_font(ap.statusLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ap.statusLbl, lv_color_hex(0x666688), 0);

    ap.noteLbl = lv_label_create(statusRow);
    lv_label_set_text(ap.noteLbl, "");
    lv_obj_set_style_text_font(ap.noteLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ap.noteLbl, lv_color_hex(0x00DFFF), 0);

    // -- Piano octave controls + piano row --------------------------------------
    lv_obj_t* pianoHdrRow = makeRow(72);
    lv_obj_t* pOctHead = lv_label_create(pianoHdrRow);
    lv_label_set_text(pOctHead, "Piano Oct:");
    lv_obj_set_style_text_font(pOctHead, fontSmall, 0);
    lv_obj_set_style_text_color(pOctHead, lv_color_hex(0x666688), 0);
    lv_obj_t* pOctDown = mcButton(pianoHdrRow);
    lv_obj_set_size(pOctDown, 88, 56);
    lv_obj_add_event_cb(pOctDown, cbPianoOctDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* podl = lv_label_create(pOctDown); lv_label_set_text(podl, "-");
    lv_obj_set_style_text_font(podl, fontSmall, 0); lv_obj_center(podl);
    char pianoOctBuf[4]; snprintf(pianoOctBuf, sizeof(pianoOctBuf), "%d", ap.pianoOctave);
    ap.pianoOctLbl = lv_label_create(pianoHdrRow);
    lv_label_set_text(ap.pianoOctLbl, pianoOctBuf);
    lv_obj_set_style_text_font(ap.pianoOctLbl, fontSmall, 0);
    lv_obj_set_style_text_color(ap.pianoOctLbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(ap.pianoOctLbl, 72);
    lv_obj_set_style_text_align(ap.pianoOctLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* pOctUp = mcButton(pianoHdrRow);
    lv_obj_set_size(pOctUp, 88, 56);
    lv_obj_add_event_cb(pOctUp, cbPianoOctUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* poul = lv_label_create(pOctUp); lv_label_set_text(poul, "+");
    lv_obj_set_style_text_font(poul, fontSmall, 0); lv_obj_center(poul);

    // Piano row - flex_grow fills the rest
    lv_obj_t* pianoArea = mcContainer(root);
    lv_obj_set_width(pianoArea, LV_PCT(100));
    lv_obj_set_flex_grow(pianoArea, 1);
    lv_obj_set_style_bg_color(pianoArea, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(pianoArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pianoArea, 0, 0);
    lv_obj_set_style_pad_all(pianoArea, 0, 0);
    lv_obj_set_scrollable(pianoArea, false);

    // 12 chromatic keys - all same height, equal width.
    // Original style: naturals (C,D,E,F,G,A,B) = dark keys, sharps = light keys with labels.
    int keyW = (int)W / 12;

    for (int k = 0; k < 12; k++) {
        int note = (ap.pianoOctave + 1) * 12 + k;
        note = std::max(0, std::min(127, note));

        lv_obj_t* btn = mcButton(pianoArea);
        lv_obj_set_pos(btn, k * keyW, 0);
        bool isSharp = IS_SHARP[k];
        lv_obj_set_size(btn, keyW - 1, LV_PCT(100));
        // Sharps = light (cream), naturals = dark
        lv_obj_set_style_bg_color(btn, isSharp ? lv_color_hex(0xDDDDCC) : lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)note);
        lv_obj_add_event_cb(btn, cbPianoKey, LV_EVENT_CLICKED, mc);

        // Note name label on all keys
        char noteName[8];
        midiNoteName(note, noteName, sizeof(noteName));
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, noteName);
        lv_obj_set_style_text_font(lbl, fontSmall, 0);
        lv_obj_set_style_text_color(lbl, isSharp ? lv_color_hex(0x333333) : lv_color_hex(0x888888), 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_set_clickable(lbl, false);

        ap.pianoKey[k] = btn;
    }
}
