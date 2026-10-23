#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// TB3PO view - 303-style acid bassline generator
//
// Layout (flex column):
//   [ctrl row: < | PLAY | BPM- | N | BPM+  DENS- | D:N | DENS+  REGEN]
//   [canvas flex_grow: 4-row step visualization]
//   [status row: KEY- | key | KEY+   SCALE- | scale | SCALE+   Seed: XXXXXXXX]
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Scale data
// ---------------------------------------------------------------------------

static constexpr int TB3_NUM_SCALES = 6;
static const char* TB3_SCALE_NAMES[TB3_NUM_SCALES] = {
    "Major", "Minor", "Dorian", "Penta", "Blues", "Chrome"
};
static constexpr int TB3_MAX_SCALE_LEN = 12;
static const int TB3_SCALE_INTERVALS[TB3_NUM_SCALES][TB3_MAX_SCALE_LEN] = {
    {0, 2, 4, 5, 7, 9, 11},                  // Major (7)
    {0, 2, 3, 5, 7, 8, 10},                  // Minor (7)
    {0, 2, 3, 5, 7, 9, 10},                  // Dorian (7)
    {0, 2, 4, 7, 9},                         // Penta (5)
    {0, 3, 5, 6, 7, 10},                     // Blues (6)
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},  // Chrome (12)
};
static const int TB3_SCALE_LENS[TB3_NUM_SCALES] = {7, 7, 7, 5, 6, 12};

// KEY_NAMES already defined in MidiScales.h - use it directly

// Get MIDI note for scale degree + octave (root at MIDI 48 = C3)
static int tb3NoteInScale(int scaleIdx, int degree, int octave, int key) {
    if (scaleIdx < 0 || scaleIdx >= TB3_NUM_SCALES) scaleIdx = 0;
    int len = TB3_SCALE_LENS[scaleIdx];
    // Proper modular arithmetic for negative degrees
    while (degree < 0) degree += len;
    int deg = degree % len;
    int octShift = degree / len;
    int rootMidi = 48 + key;  // C3 + key offset
    int note = rootMidi + TB3_SCALE_INTERVALS[scaleIdx][deg] + (octave - 3 + octShift) * 12;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

// ---------------------------------------------------------------------------
// Pattern generation
// ---------------------------------------------------------------------------

static void tb3Regenerate(MidiController::Tb3poState& ts) {
    // Use seed-based PRNG
    uint32_t rng = ts.seed;
    auto rnd = [&](int lo, int hi) -> int {
        rng = rng * 1664525u + 1013904223u;
        if (hi <= lo) return lo;
        return lo + (int)((rng >> 16) % (uint32_t)(hi - lo));
    };
    auto rndBit = [&](int prob) -> bool {
        return rnd(0, 100) < prob;
    };

    int scaleIdx = ts.scale;
    if (scaleIdx < 0 || scaleIdx >= TB3_NUM_SCALES) scaleIdx = 0;
    int scaleLen = TB3_SCALE_LENS[scaleIdx];
    int pitchDens = std::min(ts.density, 8);
    int available;
    if (pitchDens > 7) available = scaleLen - 1;
    else if (pitchDens < 2) available = pitchDens;
    else {
        int fromScale = std::max(4, scaleLen - 3);
        available = 3 + (pitchDens - 3) * fromScale / 4;
        available = std::max(1, std::min(scaleLen - 1, available));
    }

    // Generate pitches, oct flags - use direct bit position (1u << i) to avoid inversion
    uint32_t oct_ups = 0, oct_downs = 0;
    int noteDegs[32] = {};
    for (int i = 0; i < 32; i++) {
        int forceRepeatProb = 50 - (pitchDens * 6);
        if (i > 0 && rndBit(forceRepeatProb)) {
            noteDegs[i] = noteDegs[i-1];
        } else {
            noteDegs[i] = rnd(0, available + 1);
            if (rndBit(40)) {
                if (rndBit(50)) oct_ups  |= (1u << i);
                else             oct_downs |= (1u << i);
            }
        }
    }

    // Generate gates, slides, accents - direct bit position, no shift accumulation
    rng = ts.seed + 1000u;
    // Gate probability rises monotonically with density (10% at 0, 90% at 16).
    int densProb = 10 + ts.density * 5;
    uint32_t gates = 0, slides = 0, accents = 0;
    int latestSlide = 0, latestAccent = 0;
    for (int i = 0; i < 32; i++) {
        if (rndBit(densProb))                    gates   |= (1u << i);
        latestSlide  = rndBit(latestSlide  ? 10 : 18);
        if (latestSlide)                         slides  |= (1u << i);
        latestAccent = rndBit(latestAccent ?  7 : 16);
        if (latestAccent)                        accents |= (1u << i);
    }

    // Fill steps array
    for (int i = 0; i < 32; i++) {
        auto& s = ts.steps[i];
        int scaleDeg = noteDegs[i];
        bool octUp   = (oct_ups   & (1u << i)) != 0;
        bool octDown = (oct_downs & (1u << i)) != 0;
        s.note     = scaleDeg;  // scale degree (resolved to MIDI in timer)
        s.oct_up   = octUp;
        s.oct_down = octDown;
        s.accent   = (accents & (1u << i)) != 0;
        s.slide    = (slides  & (1u << i)) != 0;
        s.gate     = (gates   & (1u << i)) != 0;
    }
}

static int tb3StepToMidi(const MidiController::Tb3poState& ts, int stepIdx) {
    const auto& s = ts.steps[stepIdx];
    int octOff = ts.octave + (s.oct_up ? 1 : (s.oct_down ? -1 : 0));
    return tb3NoteInScale(ts.scale, s.note, octOff, ts.key);
}

// ---------------------------------------------------------------------------
// Canvas draw (called from timer + after regen)
// ---------------------------------------------------------------------------

static void tb3DrawCanvas(MidiController::Tb3poState& ts) {
    if (!ts.canvas) return;
    lv_obj_clean(ts.canvas);

    int W = ts.canvasW;
    int H = ts.canvasH;
    if (W <= 0 || H <= 0) return;

    int stepW = W / 32;
    int rowH  = H / 4;

    for (int i = 0; i < 32; i++) {
        const auto& s = ts.steps[i];
        int x = i * stepW;
        bool isCurrent = (ts.playing && i == ts.currentStep);

        // Row 0: Note name
        {
            lv_obj_t* cell = mcContainer(ts.canvas);
            lv_obj_set_pos(cell, x, 0);
            lv_obj_set_size(cell, stepW - 1, rowH - 1);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_scrollable(cell, false);
            uint32_t bg = isCurrent ? 0x44BBFF : 0x1A2030;
            lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);

            lv_obj_t* lbl = lv_label_create(cell);
            int midi = tb3StepToMidi(ts, i);
            static const char* NOTES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            lv_label_set_text(lbl, NOTES[midi % 12]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(isCurrent ? 0x000000 : 0xDDDDDD), 0);
            lv_obj_center(lbl);
        }
        // Row 1: Oct up/down
        {
            lv_obj_t* cell = mcContainer(ts.canvas);
            lv_obj_set_pos(cell, x, rowH);
            lv_obj_set_size(cell, stepW - 1, rowH - 1);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_scrollable(cell, false);
            uint32_t bg = s.oct_up ? 0x006622 : (s.oct_down ? 0x662200 : 0x111820);
            lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            if (s.oct_up || s.oct_down) {
                lv_obj_t* lbl = lv_label_create(cell);
                lv_label_set_text(lbl, s.oct_up ? "+" : "-");
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
                lv_obj_center(lbl);
            }
        }
        // Row 2: Accent / Slide
        {
            lv_obj_t* cell = mcContainer(ts.canvas);
            lv_obj_set_pos(cell, x, rowH * 2);
            lv_obj_set_size(cell, stepW - 1, rowH - 1);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_scrollable(cell, false);
            uint32_t bg = s.accent ? 0x886600 : (s.slide ? 0x004488 : 0x111820);
            lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            if (s.accent || s.slide) {
                lv_obj_t* lbl = lv_label_create(cell);
                lv_label_set_text(lbl, (s.accent && s.slide) ? "AS" : (s.accent ? "A" : "S"));
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
                lv_obj_center(lbl);
            }
        }
        // Row 3: Gate dot
        {
            lv_obj_t* cell = mcContainer(ts.canvas);
            lv_obj_set_pos(cell, x, rowH * 3);
            lv_obj_set_size(cell, stepW - 1, rowH - 1);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_scrollable(cell, false);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x111820), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            if (s.gate) {
                lv_obj_t* dot = mcContainer(cell);
                int dotSz = std::min(stepW - 4, rowH - 4);
                lv_obj_set_size(dot, dotSz, dotSz);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, lv_color_hex(0x0088FF), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_scrollable(dot, false);
                lv_obj_center(dot);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static void cbTb3poTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    if (!ts.playing) return;

    uint32_t now = lv_tick_get();

    // Note-off. A slide flag on the step that owns lastNote holds it into the
    // next step, so suppress the normal 90%-gate Note Off until that slide ends.
    if (ts.lastNote >= 0 && now >= ts.noteOffMs) {
        bool heldBySlide = ts.lastNoteStep >= 0 && ts.steps[ts.lastNoteStep].slide;
        if (!heldBySlide) {
            mc->sendMidi(0x80, (uint8_t)ts.lastNote, 0);
            ts.lastNote = -1;
            ts.lastNoteStep = -1;
        }
    }

    if (now < ts.nextStepMs) return;

    // Advance step
    ts.currentStep = (ts.currentStep + 1) % 32;
    const auto& s = ts.steps[ts.currentStep];

    // BPM → 16th note duration
    uint32_t stepMs = 60000u / (uint32_t)ts.bpm / 4u;
    if (stepMs < 20) stepMs = 20;
    ts.nextStepMs = now + stepMs;

    if (s.gate) {
        int note = tb3StepToMidi(ts, ts.currentStep);
        int prevStep = (ts.currentStep + 31) % 32;
        bool incomingSlide = ts.steps[prevStep].slide;
        int oldNote = ts.lastNote;
        int vel = s.accent ? 127 : 100;

        if (oldNote < 0) {
            mc->sendMidi(0x90, (uint8_t)note, (uint8_t)vel);
        } else if (incomingSlide && oldNote != note) {
            // Start the new pitch while the old one is still on so a mono synth
            // hears a legato transition, then release the old pitch. This also
            // prevents a poly synth from leaving the old note hanging.
            mc->sendMidi(0x90, (uint8_t)note, (uint8_t)vel);
            mc->sendMidi(0x80, (uint8_t)oldNote, 0);
        } else if (!incomingSlide) {
            mc->sendMidi(0x80, (uint8_t)oldNote, 0);
            mc->sendMidi(0x90, (uint8_t)note, (uint8_t)vel);
        } else {
            // Same pitch: retrigger without an intervening Note Off.
            mc->sendMidi(0x90, (uint8_t)note, (uint8_t)vel);
        }

        ts.lastNote     = note;
        ts.lastNoteStep = ts.currentStep;
        ts.noteOffMs    = now + stepMs * 9 / 10;  // 90% gate
    } else {
        if (ts.lastNote >= 0) {
            mc->sendMidi(0x80, (uint8_t)ts.lastNote, 0);
            ts.lastNote = -1;
            ts.lastNoteStep = -1;
        }
    }

    tb3DrawCanvas(ts);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbTb3Back(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbTb3Play(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.playing = !ts.playing;
    if (!ts.playing && ts.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ts.lastNote, 0);
        ts.lastNote = -1;
        ts.lastNoteStep = -1;
    }
    if (ts.playing) {
        ts.currentStep = 31;  // will advance to 0 on first tick
        ts.nextStepMs = lv_tick_get();
    }
    if (ts.playLbl) lv_label_set_text(ts.playLbl, ts.playing ? "STOP" : "PLAY");
}

static void cbTb3BpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    if (ts.bpm > 40) ts.bpm -= 5;
    if (ts.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ts.bpm);
        lv_label_set_text(ts.bpmLbl, buf);
    }
}
static void cbTb3BpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    if (ts.bpm < 280) ts.bpm += 5;
    if (ts.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ts.bpm);
        lv_label_set_text(ts.bpmLbl, buf);
    }
}

static void cbTb3DensDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    if (ts.density > 0) ts.density--;
    ts.seed = (uint32_t)esp_random();
    tb3Regenerate(ts);
    if (ts.densityLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "D:%d", ts.density);
        lv_label_set_text(ts.densityLbl, buf);
    }
    if (ts.seedLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08lX", (unsigned long)ts.seed);
        lv_label_set_text(ts.seedLbl, buf);
    }
    tb3DrawCanvas(ts);
}
static void cbTb3DensUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    if (ts.density < 16) ts.density++;
    ts.seed = (uint32_t)esp_random();
    tb3Regenerate(ts);
    if (ts.densityLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "D:%d", ts.density);
        lv_label_set_text(ts.densityLbl, buf);
    }
    if (ts.seedLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08lX", (unsigned long)ts.seed);
        lv_label_set_text(ts.seedLbl, buf);
    }
    tb3DrawCanvas(ts);
}

static void cbTb3Regen(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.seed = (uint32_t)esp_random();
    tb3Regenerate(ts);
    if (ts.seedLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08lX", (unsigned long)ts.seed);
        lv_label_set_text(ts.seedLbl, buf);
    }
    tb3DrawCanvas(ts);
}

static void cbTb3KeyDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.key = (ts.key + 11) % 12;
    tb3Regenerate(ts);
    if (ts.keyLbl) lv_label_set_text(ts.keyLbl, KEY_NAMES[ts.key]);
    tb3DrawCanvas(ts);
}
static void cbTb3KeyUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.key = (ts.key + 1) % 12;
    tb3Regenerate(ts);
    if (ts.keyLbl) lv_label_set_text(ts.keyLbl, KEY_NAMES[ts.key]);
    tb3DrawCanvas(ts);
}

static void cbTb3ScaleDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.scale = (ts.scale + TB3_NUM_SCALES - 1) % TB3_NUM_SCALES;
    tb3Regenerate(ts);
    if (ts.scaleLbl) lv_label_set_text(ts.scaleLbl, TB3_SCALE_NAMES[ts.scale]);
    tb3DrawCanvas(ts);
}
static void cbTb3ScaleUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ts = mc->tb3po_;
    ts.scale = (ts.scale + 1) % TB3_NUM_SCALES;
    tb3Regenerate(ts);
    if (ts.scaleLbl) lv_label_set_text(ts.scaleLbl, TB3_SCALE_NAMES[ts.scale]);
    tb3DrawCanvas(ts);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void buildTb3poView(MidiController* mc, lv_obj_t* parent) {
    auto& ts = mc->tb3po_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);

    int rowH = 88;
    int sBtnW = 88;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x060A0F), 0);
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
        mcBackBtn(row, cbTb3Back, mc, MC_W_S);
        lv_obj_t* playBtn = makeBtn(row, ts.playing ? "STOP" : "PLAY", 176, cbTb3Play, mc);
        ts.playLbl = lv_obj_get_child(playBtn, 0);
        makeSep(row);
        makeBtn(row, "<", sBtnW, cbTb3BpmDown, mc);
        char bpmBuf[8]; snprintf(bpmBuf, sizeof(bpmBuf), "%d", ts.bpm);
        ts.bpmLbl = makeLbl(row, bpmBuf, 136, lv_color_hex(0xFFE000));
        makeBtn(row, ">", sBtnW, cbTb3BpmUp, mc);
        makeSep(row);
        makeBtn(row, "<", sBtnW, cbTb3DensDown, mc);
        char densBuf[6]; snprintf(densBuf, sizeof(densBuf), "D:%d", ts.density);
        ts.densityLbl = makeLbl(row, densBuf, 112, lv_color_hex(0xFF8844));
        makeBtn(row, ">", sBtnW, cbTb3DensUp, mc);
        makeSep(row);
        makeBtn(row, "REGEN", 200, cbTb3Regen, mc);
    }

    // -- Canvas (flex_grow) ----------------------------------------------------
    {
        lv_obj_t* canvasArea = mcContainer(root);
        lv_obj_set_width(canvasArea, LV_PCT(100));
        lv_obj_set_flex_grow(canvasArea, 1);
        lv_obj_set_style_bg_color(canvasArea, lv_color_hex(0x050810), 0);
        lv_obj_set_style_bg_opa(canvasArea, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(canvasArea, 0, 0);
        lv_obj_set_style_pad_all(canvasArea, 0, 0);
        lv_obj_set_scrollable(canvasArea, false);
        ts.canvas = canvasArea;

        // Estimates for the first draw, before LVGL has resolved flex sizes.
        // LV_EVENT_SIZE_CHANGED replaces these with the real canvas dimensions.
        ts.canvasW = lv_display_get_horizontal_resolution(nullptr);
        ts.canvasH = lv_display_get_vertical_resolution(nullptr) - rowH * 2 - 32;
        if (ts.canvasH < 240) ts.canvasH = 240;
    }

    // -- Status row: KEY  SCALE  Seed -----------------------------------------
    {
        lv_obj_t* row = makeRow();
        makeBtn(row, "<", sBtnW, cbTb3KeyDown, mc);
        ts.keyLbl = makeLbl(row, KEY_NAMES[ts.key], 88, lv_color_hex(0x88DDFF));
        makeBtn(row, ">", sBtnW, cbTb3KeyUp, mc);
        makeSep(row);
        makeBtn(row, "<", sBtnW, cbTb3ScaleDown, mc);
        ts.scaleLbl = makeLbl(row, TB3_SCALE_NAMES[ts.scale], 176, lv_color_hex(0xAAFF88));
        makeBtn(row, ">", sBtnW, cbTb3ScaleUp, mc);
        makeSep(row);
        lv_obj_t* seedHdr = lv_label_create(row);
        lv_label_set_text(seedHdr, "Seed:");
        lv_obj_set_style_text_font(seedHdr, fontSmall, 0);
        lv_obj_set_style_text_color(seedHdr, lv_color_hex(0x666688), 0);
        char seedBuf[16]; snprintf(seedBuf, sizeof(seedBuf), "%08lX", (unsigned long)ts.seed);
        ts.seedLbl = makeLbl(row, seedBuf, 256, lv_color_hex(0x8888AA));
    }

    // -- Initial pattern + draw -------------------------------------------------
    if (ts.seed == 0) ts.seed = (uint32_t)esp_random();
    tb3Regenerate(ts);

    // Start timer
    mc->tb3poTimer_ = lv_timer_create(cbTb3poTimer, 10, mc);
    ts.nextStepMs = lv_tick_get();

    // Draw immediately with the estimated size, then redraw once the layout pass
    // has given the canvas its real dimensions. Without this the four cell rows
    // can extend past the bottom of the content area and get clipped.
    tb3DrawCanvas(ts);
    lv_obj_add_event_cb(ts.canvas, [](lv_event_t* ev) {
        auto* mc2 = static_cast<MidiController*>(lv_event_get_user_data(ev));
        if (!mc2) return;
        auto& ts2 = mc2->tb3po_;
        int w = lv_obj_get_width(ts2.canvas);
        int h = lv_obj_get_height(ts2.canvas);
        if (w > 32 && h > 32) {
            ts2.canvasW = w;
            ts2.canvasH = h;
            tb3DrawCanvas(ts2);
        }
    }, LV_EVENT_SIZE_CHANGED, mc);
}

// ---------------------------------------------------------------------------
// Public encoder entry points (called from EncoderInput.cpp)
// ---------------------------------------------------------------------------

void tb3TogglePlay(MidiController* mc) {
    auto& ts = mc->tb3po_;
    ts.playing = !ts.playing;
    if (!ts.playing && ts.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ts.lastNote, 0);
        ts.lastNote = -1;
        ts.lastNoteStep = -1;
    }
    if (ts.playing) {
        ts.currentStep = 31;
        ts.nextStepMs  = lv_tick_get();
    }
    if (ts.playLbl) lv_label_set_text(ts.playLbl, ts.playing ? "STOP" : "PLAY");
}

void tb3ChangeScale(MidiController* mc, int delta) {
    auto& ts = mc->tb3po_;
    static constexpr int NUM_SCALES = 6;
    ts.scale = ((ts.scale + delta) % NUM_SCALES + NUM_SCALES) % NUM_SCALES;
    tb3Regenerate(ts);
    if (ts.scaleLbl) lv_label_set_text(ts.scaleLbl, TB3_SCALE_NAMES[ts.scale]);
    tb3DrawCanvas(ts);
}

void tb3Regen(MidiController* mc) {
    auto& ts = mc->tb3po_;
    ts.seed = (uint32_t)esp_random();
    tb3Regenerate(ts);
    if (ts.seedLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08lX", (unsigned long)ts.seed);
        lv_label_set_text(ts.seedLbl, buf);
    }
    tb3DrawCanvas(ts);
}

void tb3Redraw(MidiController* mc) {
    if (mc) tb3DrawCanvas(mc->tb3po_);
}
