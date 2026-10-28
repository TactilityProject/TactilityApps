#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// RAGA view - Indian classical raga Markov-chain phrase generator
//
// Layout (flex column):
//   [ctrl row: < | RAGA | status]
//   [raga row: Raga- | name | Raga+]
//   [tala row: Tala- | name | Tala+]
//   [root row: Root- | C3  | Root+]
//   [bpm  row: BPM-  | 120 | BPM+ ]
//   [spacer flex_grow]
//   [play row: PLAY/STOP | DRONE]
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Raga data
// ---------------------------------------------------------------------------

static constexpr int RAGA_COUNT = 8;
static constexpr int TALA_COUNT = 3;

static const char* RAGA_NAMES[RAGA_COUNT] = {
    "Bhairavi", "Lalit", "Bhupali", "Todi",
    "Madhuvanti", "Meghmalhar", "Yaman", "Malkauns"
};
static const char* TALA_NAMES[TALA_COUNT] = {
    "Teental", "Rupak", "Jhaptal"
};

static const uint8_t RAGA_INTERVALS[RAGA_COUNT][7] = {
    {0, 1, 3, 5, 7, 8, 10},   // Bhairavi
    {0, 1, 4, 5, 6, 9, 11},   // Lalit
    {0, 2, 4, 7, 9, 0, 0},    // Bhupali (5 notes)
    {0, 1, 3, 6, 7, 8, 11},   // Todi
    {0, 2, 3, 6, 7, 9, 11},   // Madhuvanti
    {0, 2, 5, 7, 10, 0, 0},   // Meghmalhar (5 notes)
    {0, 2, 4, 6, 7, 9, 11},   // Yaman
    {0, 3, 5, 8, 10, 0, 0},   // Malkauns (5 notes)
};
static const uint8_t RAGA_LENGTHS[RAGA_COUNT] = {7, 7, 5, 7, 7, 5, 7, 5};

// Tala accent patterns (2=sam/strong, 1=khali/medium, 0=weak)
static const uint8_t TEENTAL_ACCENTS[16] = {2,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0};
static const uint8_t RUPAK_ACCENTS[7]    = {2,0,0, 1,0,0,0};
static const uint8_t JHAPTAL_ACCENTS[10] = {2,0,0, 1,0,0, 0,1,0,0};

static const int  TALA_BEATS[TALA_COUNT]             = {16, 7, 10};
static const uint8_t* TALA_ACCENTS[TALA_COUNT]       = {
    TEENTAL_ACCENTS, RUPAK_ACCENTS, JHAPTAL_ACCENTS
};

// 2nd-order Markov weights for Bhairavi [prev_prev][prev][next]
static const uint8_t BHAIRAVI_WEIGHTS[7][7][7] = {
  {{40,30,15,25,10,5,5},{20,20,40,35,10,5,5},{15,25,20,50,10,5,5},
   {30,20,30,30,15,10,5},{25,15,20,40,20,15,10},{20,15,10,25,15,20,30},{50,20,10,15,10,10,15}},
  {{40,30,20,20,10,5,5},{25,35,30,20,10,5,5},{15,20,25,45,15,5,5},
   {25,20,35,30,15,10,5},{20,25,20,35,20,10,5},{15,20,15,25,15,25,20},{45,25,15,15,10,10,10}},
  {{35,30,20,25,10,5,5},{35,25,25,25,10,5,5},{20,25,25,35,15,10,5},
   {20,15,20,40,20,20,10},{15,20,25,40,20,15,10},{10,15,15,30,15,25,25},{40,25,15,15,10,10,15}},
  {{45,30,20,20,10,5,5},{30,25,30,25,10,5,5},{25,35,25,25,10,5,5},
   {25,20,25,40,20,15,10},{20,15,20,40,25,15,10},{10,10,15,30,15,25,35},{50,20,15,20,10,10,10}},
  {{40,25,20,25,15,5,5},{25,25,25,30,15,10,5},{20,25,25,40,15,10,5},
   {20,20,25,40,20,15,10},{25,15,20,40,25,20,10},{15,15,15,30,15,30,25},{45,20,15,20,15,10,10}},
  {{40,25,20,20,15,10,10},{25,30,25,25,15,15,10},{20,25,25,35,15,15,10},
   {20,20,25,35,20,20,15},{20,20,20,35,25,20,15},{15,15,15,25,15,30,30},{45,20,15,20,15,15,15}},
  {{50,25,15,20,10,10,10},{30,30,25,25,15,10,5},{25,25,25,30,15,10,10},
   {25,20,25,35,15,15,10},{25,20,20,35,20,15,10},{20,20,15,25,15,25,25},{45,25,15,20,10,10,15}},
};

// ---------------------------------------------------------------------------
// Algorithm helpers (file-static, operate on RagaState&)
// ---------------------------------------------------------------------------

static int ragaSelectNextBhairavi(const int8_t hist[2]) {
    int h0 = ((int)hist[0] % 7 + 7) % 7;
    int h1 = ((int)hist[1] % 7 + 7) % 7;
    const uint8_t* w = BHAIRAVI_WEIGHTS[h0][h1];
    int total = 0;
    for (int i = 0; i < 7; i++) total += w[i];
    if (total <= 0) return 0;
    int sel = (int)((uint32_t)esp_random() % (uint32_t)total);
    int cum = 0;
    for (int i = 0; i < 7; i++) {
        cum += w[i];
        if (sel < cum) return i;
    }
    return 0;
}

static void ragaGeneratePhrase(MidiController::RagaState& rs) {
    int scaleLen = RAGA_LENGTHS[rs.raga];
    const uint8_t* scale = RAGA_INTERVALS[rs.raga];
    int total = 32;
    rs.phraseLen = total;

    if (rs.raga == 0) {
        // Bhairavi: 2nd-order Markov
        int8_t hist[2] = {rs.history[0], rs.history[1]};
        int octave = 0;
        for (int i = 0; i < total; i++) {
            int degree = ragaSelectNextBhairavi(hist);
            if ((uint32_t)(esp_random() % 100) < 8) {
                int shift = (int)((uint32_t)(esp_random() % 3)) - 1;
                octave = std::max(-1, std::min(1, octave + shift));
            }
            rs.phrase[i] = (int8_t)(scale[degree] + octave * 12);
            hist[0] = hist[1];
            hist[1] = (int8_t)degree;
        }
        rs.history[0] = hist[0];
        rs.history[1] = hist[1];
    } else {
        // Other ragas: random walk
        int degree = (int)((uint32_t)(esp_random() % (uint32_t)scaleLen));
        int octave = 0;
        for (int i = 0; i < total; i++) {
            int step = (int)((uint32_t)(esp_random() % 3)) - 1;
            degree = std::max(0, std::min(scaleLen - 1, degree + step));
            if ((uint32_t)(esp_random() % 100) < 10) {
                int shift = (int)((uint32_t)(esp_random() % 3)) - 1;
                octave = std::max(-1, std::min(1, octave + shift));
            }
            rs.phrase[i] = (int8_t)(scale[degree] + octave * 12);
        }
    }
    rs.phraseIdx = 0;
}

static void ragaUpdateTempo(MidiController::RagaState& rs) {
    if (rs.bpm < 1) rs.bpm = 120; // Safe default
    uint32_t interval = 60000u / (uint32_t)rs.bpm;
    rs.noteInterval = std::max((uint32_t)40, interval / 4);
    rs.noteDuration = std::max((uint32_t)30, rs.noteInterval * 8 / 10);
}

static void ragaStopNote(MidiController* mc) {
    auto& rs = mc->raga_;
    if (rs.noteActive) {
        mc->sendMidi(0x80, rs.currentNote, 0);
        rs.noteActive = false;
    }
}

static void ragaUpdateDrone(MidiController* mc) {
    auto& rs = mc->raga_;
    if (rs.droneOn) {
        if (!rs.droneActive || rs.droneNote != (uint8_t)rs.rootNote) {
            if (rs.droneActive) mc->sendMidi(0x80, rs.droneNote, 0);
            rs.droneNote   = (uint8_t)rs.rootNote;
            rs.droneActive = true;
            mc->sendMidi(0x90, rs.droneNote, 80);
        }
    } else if (rs.droneActive) {
        mc->sendMidi(0x80, rs.droneNote, 0);
        rs.droneActive = false;
    }
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static void cbRagaTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& rs = mc->raga_;

    ragaUpdateDrone(mc);
    if (!rs.playing) return;

    uint32_t now = lv_tick_get();

    // Note-off check
    if (rs.noteActive && now >= rs.noteOffMs) {
        ragaStopNote(mc);
    }

    // Schedule next note
    if (now >= rs.nextNoteMs) {
        if (rs.phraseLen == 0 || rs.phraseIdx >= rs.phraseLen)
            ragaGeneratePhrase(rs);

        ragaStopNote(mc);
        uint8_t note = (uint8_t)std::max(0, std::min(127, rs.rootNote + rs.phrase[rs.phraseIdx++]));
        // Ensure talaBeat is within bounds for current tala
        if (rs.talaBeat >= TALA_BEATS[rs.tala]) rs.talaBeat = 0;
        uint8_t accent = TALA_ACCENTS[rs.tala][rs.talaBeat];
        uint8_t vel = (uint8_t)std::min(127, 90 + (int)accent * 10);
        mc->sendMidi(0x90, note, vel);
        rs.currentNote = note;
        rs.noteActive  = true;
        rs.noteOffMs   = now + rs.noteDuration;

        // Update status label
        if (rs.statusLbl) {
            char buf[32];
            char noteName[8]; midiNoteName(note, noteName, sizeof(noteName));
            snprintf(buf, sizeof(buf), "%s | beat %d/%d",
                     noteName, rs.talaBeat + 1, TALA_BEATS[rs.tala]);
            lv_label_set_text(rs.statusLbl, buf);
        }
        rs.talaBeat    = (rs.talaBeat + 1) % TALA_BEATS[rs.tala];
        rs.nextNoteMs  = now + rs.noteInterval;
    }
}

// Forward declarations for public encoder entry points (defined at file end)
void ragaTogglePlay (MidiController* mc);
void ragaToggleDrone(MidiController* mc);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbPlayStop(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) ragaTogglePlay(mc);
}

static void cbDrone(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) ragaToggleDrone(mc);
}

static void cbRagaDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    rs.raga = (rs.raga + RAGA_COUNT - 1) % RAGA_COUNT;
    ragaGeneratePhrase(rs);
    if (rs.ragaLbl)    lv_label_set_text(rs.ragaLbl,    RAGA_NAMES[rs.raga]);
    if (rs.ragaBigLbl) lv_label_set_text(rs.ragaBigLbl, RAGA_NAMES[rs.raga]);
}
static void cbRagaUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    rs.raga = (rs.raga + 1) % RAGA_COUNT;
    ragaGeneratePhrase(rs);
    if (rs.ragaLbl)    lv_label_set_text(rs.ragaLbl,    RAGA_NAMES[rs.raga]);
    if (rs.ragaBigLbl) lv_label_set_text(rs.ragaBigLbl, RAGA_NAMES[rs.raga]);
}

static void cbTalaDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    rs.tala = (rs.tala + TALA_COUNT - 1) % TALA_COUNT;
    rs.talaBeat = 0;
    if (rs.talaLbl)    lv_label_set_text(rs.talaLbl,    TALA_NAMES[rs.tala]);
    if (rs.talaBigLbl) lv_label_set_text(rs.talaBigLbl, TALA_NAMES[rs.tala]);
}
static void cbTalaUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    rs.tala = (rs.tala + 1) % TALA_COUNT;
    rs.talaBeat = 0;
    if (rs.talaLbl)    lv_label_set_text(rs.talaLbl,    TALA_NAMES[rs.tala]);
    if (rs.talaBigLbl) lv_label_set_text(rs.talaBigLbl, TALA_NAMES[rs.tala]);
}

static void cbRootDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    if (rs.rootNote > 24) rs.rootNote--;
    if (rs.rootLbl) {
        char buf[8]; midiNoteName(rs.rootNote, buf, sizeof(buf));
        lv_label_set_text(rs.rootLbl, buf);
    }
}
static void cbRootUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    if (rs.rootNote < 96) rs.rootNote++;
    if (rs.rootLbl) {
        char buf[8]; midiNoteName(rs.rootNote, buf, sizeof(buf));
        lv_label_set_text(rs.rootLbl, buf);
    }
}

static void cbBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    if (rs.bpm > 40) rs.bpm -= 5;
    ragaUpdateTempo(rs);
    if (rs.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", rs.bpm);
        lv_label_set_text(rs.bpmLbl, buf);
    }
}
static void cbBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& rs = mc->raga_;
    if (rs.bpm < 240) rs.bpm += 5;
    ragaUpdateTempo(rs);
    if (rs.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", rs.bpm);
        lv_label_set_text(rs.bpmLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void buildRagaView(MidiController* mc, lv_obj_t* parent) {
    auto& rs = mc->raga_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);
    const lv_font_t* fontLarge  = lvgl_get_text_font(FONT_SIZE_LARGE);

    static constexpr int rowH  = 88;
    static constexpr int btnH  = 64;
    static constexpr int pad   = 12;

    ragaUpdateTempo(rs);

    // -- Root: flex column -----------------------------------------------------
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

    // -- Top control row ------------------------------------------------------
    {
        lv_obj_t* ctrl = mcRow(root, rowH);

        // Back
        mcBackBtn(ctrl, cbBack, mc, MC_W_S);

        // Title
        lv_obj_t* title = lv_label_create(ctrl);
        lv_label_set_text(title, "RAGA");
        lv_obj_set_style_text_font(title, fontNormal, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFF8844), 0);
        lv_obj_set_flex_grow(title, 1);

        // Play / Stop button in the top bar - sync to current state
        rs.playBtn = mcButton(ctrl);
        lv_obj_set_size(rs.playBtn, 160, btnH);
        lv_obj_set_style_bg_color(rs.playBtn,
            rs.playing ? lv_color_hex(0xFF2020) : lv_color_hex(0x00AA44), 0);
        lv_obj_add_event_cb(rs.playBtn, cbPlayStop, LV_EVENT_CLICKED, mc);
        rs.playLbl = lv_label_create(rs.playBtn);
        lv_label_set_text(rs.playLbl, rs.playing ? "STOP" : "PLAY");
        lv_obj_set_style_text_font(rs.playLbl, fontSmall, 0);
        lv_obj_center(rs.playLbl);

        // Drone button - sync to current state
        rs.droneBtn = mcButton(ctrl);
        lv_obj_set_size(rs.droneBtn, 200, btnH);
        lv_obj_set_style_bg_color(rs.droneBtn,
            rs.droneOn ? lv_color_hex(0x00AA44) : lv_color_hex(0x1A2A40), LV_PART_MAIN);
        lv_obj_add_event_cb(rs.droneBtn, cbDrone, LV_EVENT_CLICKED, mc);
        rs.droneLbl = lv_label_create(rs.droneBtn);
        lv_label_set_text(rs.droneLbl, rs.droneOn ? "DRONE ON" : "DRONE OFF");
        lv_obj_set_style_text_font(rs.droneLbl, fontSmall, 0);
        lv_obj_center(rs.droneLbl);

        // Status
        rs.statusLbl = lv_label_create(ctrl);
        lv_label_set_text(rs.statusLbl, "IDLE");
        lv_obj_set_style_text_font(rs.statusLbl, fontSmall, 0);
        lv_obj_set_style_text_color(rs.statusLbl, lv_color_hex(0x666688), 0);
        lv_obj_set_width(rs.statusLbl, 260);
        lv_obj_set_style_text_align(rs.statusLbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    // -- Body: flex row - left params | right display --------------------------
    lv_obj_t* body = mcContainer(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_gap(body, 0, 0);
    lv_obj_set_scrollable(body, false);

    // -- LEFT: parameter column (flex column, 600px) ---------------------------
    {
        lv_obj_t* col = mcContainer(body);
        lv_obj_set_size(col, 600, LV_PCT(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_pad_gap(col, 0, 0);
        lv_obj_set_scrollable(col, false);

        // Each param gets flex_grow=1 so all 4 fill the column equally
        auto makeParamRow = [&](const char* name, lv_color_t nameCol) -> lv_obj_t* {
            lv_obj_t* row = mcContainer(col);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x0A0C14), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0x1A2030), 0);
            lv_obj_set_style_pad_all(row, pad, 0);
            lv_obj_set_style_pad_gap(row, pad, 0);
            lv_obj_set_scrollable(row, false);
            lv_obj_t* hdr = lv_label_create(row);
            lv_label_set_text(hdr, name);
            lv_obj_set_style_text_font(hdr, fontSmall, 0);
            lv_obj_set_style_text_color(hdr, nameCol, 0);
            lv_obj_set_width(hdr, 120);
            return row;
        };

        auto makeBtn = [&](lv_obj_t* par, const char* txt, int w, lv_event_cb_t cb) -> lv_obj_t* {
            return mcSmallBtn(par, txt, w, cb, mc);
        };

        // Value label uses flex_grow=1 so it takes remaining width - prevents > button overflow
        auto makeValLbl = [&](lv_obj_t* par, const char* txt, lv_color_t col) -> lv_obj_t* {
            lv_obj_t* lbl = lv_label_create(par);
            lv_label_set_text(lbl, txt);
            lv_obj_set_style_text_font(lbl, fontNormal, 0);
            lv_obj_set_style_text_color(lbl, col, 0);
            lv_obj_set_flex_grow(lbl, 1);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            return lbl;
        };

        // Raga
        {
            lv_obj_t* row = makeParamRow("Raga", lv_color_hex(0x888888));
            makeBtn(row, "<", 88, cbRagaDown);
            rs.ragaLbl = makeValLbl(row, RAGA_NAMES[rs.raga], lv_color_hex(0xFF8844));
            makeBtn(row, ">", 88, cbRagaUp);
        }
        // Tala
        {
            lv_obj_t* row = makeParamRow("Tala", lv_color_hex(0x888888));
            makeBtn(row, "<", 88, cbTalaDown);
            rs.talaLbl = makeValLbl(row, TALA_NAMES[rs.tala], lv_color_hex(0xAADDFF));
            makeBtn(row, ">", 88, cbTalaUp);
        }
        // Root
        {
            lv_obj_t* row = makeParamRow("Root", lv_color_hex(0x888888));
            makeBtn(row, "<", 88, cbRootDown);
            char rootBuf[8]; midiNoteName(rs.rootNote, rootBuf, sizeof(rootBuf));
            rs.rootLbl = makeValLbl(row, rootBuf, MC_VALUE());
            makeBtn(row, ">", 88, cbRootUp);
        }
        // BPM
        {
            lv_obj_t* row = makeParamRow("BPM", lv_color_hex(0x888888));
            makeBtn(row, "<", 88, cbBpmDown);
            char bpmBuf[8]; snprintf(bpmBuf, sizeof(bpmBuf), "%d", rs.bpm);
            rs.bpmLbl = makeValLbl(row, bpmBuf, lv_color_hex(0xFFE000));
            makeBtn(row, ">", 88, cbBpmUp);
        }
    }

    // -- RIGHT: status display (flex_grow) -------------------------------------
    {
        lv_obj_t* display = mcContainer(body);
        lv_obj_set_height(display, LV_PCT(100));
        lv_obj_set_flex_grow(display, 1);
        lv_obj_set_flex_flow(display, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(display, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(display, lv_color_hex(0x05080F), 0);
        lv_obj_set_style_bg_opa(display, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(display, 0, 0);
        lv_obj_set_style_pad_all(display, pad * 2, 0);
        lv_obj_set_style_pad_gap(display, pad, 0);
        lv_obj_set_scrollable(display, false);

        // Large raga name (updates when raga changes)
        rs.ragaBigLbl = lv_label_create(display);
        lv_label_set_text(rs.ragaBigLbl, RAGA_NAMES[rs.raga]);
        lv_obj_set_style_text_font(rs.ragaBigLbl, fontLarge, 0);
        lv_obj_set_style_text_color(rs.ragaBigLbl, lv_color_hex(0xFF8844), 0);
        lv_obj_set_style_text_align(rs.ragaBigLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(rs.ragaBigLbl, LV_PCT(100));

        // Tala info (updates when tala changes)
        rs.talaBigLbl = lv_label_create(display);
        lv_label_set_text(rs.talaBigLbl, TALA_NAMES[rs.tala]);
        lv_obj_set_style_text_font(rs.talaBigLbl, fontNormal, 0);
        lv_obj_set_style_text_color(rs.talaBigLbl, lv_color_hex(0xAADDFF), 0);
        lv_obj_set_style_text_align(rs.talaBigLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(rs.talaBigLbl, LV_PCT(100));
    }

    // -- Start timer ----------------------------------------------------------
    mc->ragaTimer_ = lv_timer_create(cbRagaTimer, 20, mc);
}

// ---------------------------------------------------------------------------
// Public encoder entry points (called from EncoderInput.cpp)
// ---------------------------------------------------------------------------

void ragaTogglePlay(MidiController* mc) {
    auto& rs = mc->raga_;
    rs.playing = !rs.playing;
    if (rs.playing) {
        ragaUpdateTempo(rs);
        ragaGeneratePhrase(rs);
        rs.talaBeat   = 0;
        rs.nextNoteMs = lv_tick_get();
    } else {
        ragaStopNote(mc);
        if (rs.statusLbl) lv_label_set_text(rs.statusLbl, "IDLE");
    }
    if (rs.playLbl) lv_label_set_text(rs.playLbl, rs.playing ? "STOP" : "PLAY");
    if (rs.playBtn)
        lv_obj_set_style_bg_color(rs.playBtn,
            rs.playing ? lv_color_hex(0xFF2020) : lv_color_hex(0x00AA44), 0);
}

void ragaToggleDrone(MidiController* mc) {
    auto& rs = mc->raga_;
    rs.droneOn = !rs.droneOn;
    if (rs.droneLbl) lv_label_set_text(rs.droneLbl, rs.droneOn ? "DRONE ON" : "DRONE OFF");
    if (rs.droneBtn) {
        lv_obj_set_style_bg_color(rs.droneBtn,
            rs.droneOn ? lv_color_hex(0x00AA44) : lv_color_hex(0x1A2A40),
            LV_PART_MAIN);
    }
}
