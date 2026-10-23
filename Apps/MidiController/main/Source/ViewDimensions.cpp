#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// DIMENSIONS view - parametric equation sequencer (20 equations)
//
// Layout (flex column):
//   [row1: < | EQ- | Eq:N | EQ+ | PLAY | RESET]
//   [row2: A- | A:N | A+     B- | B:N | B+]
//   [row3: C- | C:N | C+     D- | D:N | D+]
//   [outArea flex_grow: px:N  py:N  pz:N  t:N]
//   [row5: OCT- | Oct N | OCT+   RND- | Rnd:N | RND+   BPM- | N | BPM+]
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Parametric equations
// ---------------------------------------------------------------------------

static inline float sq(float x) { return x * x; }

// Arduino random() compat - uniform int in [lo, hi)
static inline int rng_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)((uint32_t)(esp_random()) % (uint32_t)(hi - lo));
}

static void dimsEval(int eq, float t, float a, float b, float c, float d,
                     float rnd, float x, float y,
                     float& px, float& py, float& pz) {
    if (a == 0) a = 0.001f;
    if (b == 0) b = 0.001f;
    if (c == 0) c = 0.001f;
    if (d == 0) d = 0.001f;

    constexpr float PI = 3.14159265f;
    float px_f, py_f, pz_f;

    switch (eq) {
        case 1:
            px_f = x + t * sinf(a * t + PI/2.0f);
            py_f = y + t * sinf(b * t);
            pz_f = t * cosf(a * t / PI * c);
            break;
        case 2: {
            float denom2 = (a - b);
            if (fabsf(denom2) < 0.01f) denom2 = (denom2 >= 0.0f) ? 0.01f : -0.01f;
            px_f = x + t * sinf(a * t + PI/2.0f) + cosf(sq(a+b)/PI) * tanf(a);
            py_f = y + t * sinf(b * t) * (tanf((t * a)/denom2) / (b * 4.0f));
            pz_f = t * cosf(a * t / PI);
        }
            break;
        case 3:
            px_f = x + t * sinf(a * t + PI/(c + 1.0f)) + cosf(sq(a+b)/(c + 0.5f));
            py_f = y + t * sinf(c * t) * (tanf((t * a)/(c-b + 0.12f)) / ((b * 4.0f) + 0.5f));
            pz_f = t * sinf(a * t / PI);
            break;
        case 4:
            px_f = x + t * sinf(a * t + PI/c) + cosf(sq(PI/2.0f)/c);
            py_f = y + t * sinf(b * t/2.0f) * cosf(t/PI * a);
            pz_f = t * sinf(c * t / PI);
            break;
        case 5:
            px_f = x + t * sinf(a * t/2.0f) + (rnd * (PI / (float)rng_range(2,30) * 1.75f));
            py_f = y + t * sinf(b * t) + (rnd * (PI / (float)rng_range(5,25) * c));
            pz_f = t * cosf(a * t / PI * c);
            break;
        case 6:
            px_f = x + (float)rng_range(-64,64);
            py_f = y + (float)rng_range(-64,64);
            pz_f = t * (float)rng_range(0,127);
            break;
        case 7:
            px_f = x + t * sinf(a * t + PI/3.0f) * (PI / powf(b, 2.0f));
            py_f = y + t * sinf(b * t);
            pz_f = t * cosf(a * t / PI * c * t);
            break;
        case 8:
            px_f = x + t * sinf(a * t + PI/2.0f) - (t * cosf(a * t + PI/4.0f));
            py_f = y + t * sinf(b * t) + (y - t * cosf(0.25f * b * t));
            pz_f = t * cosf(a * t / PI * c);
            break;
        case 9:
            px_f = x + t * sinf(a * t + PI/25.0f) * (d * 0.5f);
            py_f = y + t * cosf(-b * t + PI/130.0f) * (d * 0.5f) + t * sinf(a * t + PI/25.0f);
            pz_f = t * sinf(t / PI * c);
            break;
        case 10:
            px_f = x + t * sinf(a * t + PI/2.0f) * logf(16.0f)/b * 2.0f;
            py_f = y + t * sinf(b * t) + sinf(d * t * 100.0f) + tanf(PI * t);
            pz_f = t * 0.5f * sinf(t / PI * c + 0.25f);
            break;
        case 11:
            px_f = x + t * sinf(a * t + PI/3.0f * 0.025f);
            py_f = y + t * sinf(b * 0.05f * t * cosf(b * t * 0.05f)) + rnd;
            pz_f = t * tanf(a * t / PI * c * 0.025f) + rnd * 1.0f + d;
            break;
        case 12:
            px_f = x + t * sinf(a * t + PI/2.0f);
            py_f = y + (float)rng_range(-64,64) * rnd * 1.0f/64.0f;
            pz_f = t * cosf(d * t / PI * c) * 1.0f + b * sinf(t * 2.25f);
            break;
        case 13: {
            float bmod = (b == 0) ? 1.0f : b;
            px_f = x + t * sinf(fmodf(a, bmod) * t) + sinf(PI/3.0f * c);
            py_f = y + t * sinf(t * c) + cosf(fmodf(a, bmod)) * 20.25f;
            pz_f = t * cosf(a * t / PI);
            break;
        }
        case 14:
            px_f = x + (float)rng_range(-64,64) * rnd * 1.0f/64.0f;
            py_f = y + t * sinf(a * t + PI/2.0f);
            pz_f = t * sinf(b * t / PI) * 2.0f + b * cosf(t * 20.0f);
            break;
        case 15:
            px_f = x + t * tanf(a * t + PI * sinf(b * t)) * 0.75f;
            py_f = y + t * tanf(b * t) - cosf(a * t * 0.5f) * 0.75f;
            pz_f = t * cosf(2.0f * a * t / PI * c);
            break;
        case 16:
            px_f = x + t * sinf(a * t + PI/3.0f) - (1.75f * t) * sinf(1.5f * a * t + PI/3.0f) * 1.0f/c * 0.5f;
            py_f = y + t * sinf(b * t + PI/3.0f) - (1.75f * t) * sinf(1.5f * b * t + PI/3.0f) * 1.0f/c * 0.5f;
            pz_f = y + t * sinf(d * t + PI/3.0f) - (1.75f * t) * cosf(1.5f * d * t + PI/2.0f) * 1.0f/c * 0.5f;
            break;
        case 17:
            px_f = x + t * sinf(a * t + PI);
            py_f = y + t * tanf(b * t - PI);
            pz_f = t * cosf(a * t / PI * c);
            break;
        case 18: {
            int r = (int)fabsf(rnd);
            if (r == 0) r = 1;
            px_f = x + (float)rng_range(-r, r);
            py_f = y + (float)rng_range(-r, r);
            pz_f = t * (float)rng_range(0, r);
            break;
        }
        case 19:
            px_f = x + t * sinf(a * t + PI/2.0f) + cosf(sq(a+b)/PI) * tanf(a) + rnd;
            py_f = y + t * sinf(b/c * t) * (tanf((t * a)/(a+b)) / (b * 4.0f)) + d/2.25f;
            pz_f = t * tanf(c * t / PI);
            break;
        case 20:
            px_f = x + t * sinf(a * t + (PI / powf(c, 3.0f)));
            py_f = y + (t * sinf(b * t/2.0f) * (tanf(t/PI * a) * 0.25f)) - d;
            pz_f = t/2.25f * tanf(c * t/4.0f) * rnd;
            break;
        default:
            px_f = x; py_f = y; pz_f = 64.0f;
            break;
    }

    // Clamp 0-127; guard NaN/Inf
    auto guard = [](float v) -> float {
        if (!std::isfinite(v)) return 64.0f;
        return std::max(0.0f, std::min(127.0f, v));
    };
    px = guard(px_f);
    py = guard(py_f);
    pz = guard(pz_f);
}

// px (0-127) → BPM-derived ms per step (10 clock-division tiers)
static uint32_t pxToMs(float px_val, int bpm) {
    // Tiers range from a whole note (16 sixteenths) to a 32nd note (0.5).
    static const float DIVIDERS[10] = {
        16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 4.0f/3.0f, 1.0f, 0.5f
    };
    int idx = (int)(px_val / 127.0f * 9.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    float beatMs = 60000.0f / (float)bpm;
    uint32_t ms = (uint32_t)(beatMs * DIVIDERS[idx] / 4.0f);
    if (ms < 20) ms = 20;
    return ms;
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static void cbDimTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (!ds.playing) return;

    uint32_t now = lv_tick_get();

    // Note-off
    if (ds.lastNote >= 0 && now >= ds.noteOffMs) {
        mc->sendMidi(0x80, (uint8_t)ds.lastNote, 0);
        ds.lastNote = -1;
    }

    if (now < ds.tickMs) return;

    // Evaluate equation
    dimsEval(ds.equation, ds.t, ds.a, ds.b, ds.c, ds.d, ds.rnd,
             ds.xOff, ds.yOff, ds.px, ds.py, ds.pz);

    // Advance t (wraps at t2)
    ds.t += ds.gran;
    if (ds.t > ds.t2) ds.t = 0.0f;

    // py → note (0-127 mapped to C2-C7, chromatic, before the octave offset)
    int note = (int)(ds.py / 127.0f * 60.0f) + 36;  // MIDI 36-96
    note += ds.octave * 12;
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    // pz → velocity
    int vel = std::max(1, std::min(127, (int)ds.pz));

    // Trigger if py above rest threshold (low gate: ~4%)
    if (ds.py >= 5.0f) {
        if (ds.lastNote >= 0) {
            mc->sendMidi(0x80, (uint8_t)ds.lastNote, 0);
        }
        mc->sendMidi(0x90, (uint8_t)note, (uint8_t)vel);
        ds.lastNote  = note;
        ds.noteOffMs = now + 80;  // 80ms gate
    }

    // Schedule next tick based on px
    uint32_t stepMs = pxToMs(ds.px, ds.bpm);
    ds.tickMs = now + stepMs;

    // Update output label
    if (ds.outLbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "px:%-3d  py:%-3d  pz:%-3d  t:%-5.1f",
                 (int)ds.px, (int)ds.py, (int)ds.pz, ds.t);
        lv_label_set_text(ds.outLbl, buf);
    }
}

// Forward declarations for public encoder entry points (defined at file end)
void dimTogglePlay(MidiController* mc);
void dimReset      (MidiController* mc);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cbDimBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbDimPlay(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) dimTogglePlay(mc);
}

static void cbDimReset(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    dimReset(mc);
}

static void cbDimEqDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    ds.equation = (ds.equation > 1) ? ds.equation - 1 : 20;
    if (ds.eqLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "Eq:%d", ds.equation);
        lv_label_set_text(ds.eqLbl, buf);
    }
}
static void cbDimEqUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    ds.equation = (ds.equation < 20) ? ds.equation + 1 : 1;
    if (ds.eqLbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "Eq:%d", ds.equation);
        lv_label_set_text(ds.eqLbl, buf);
    }
}

// Generic param adjust helpers - user_data encodes which param + direction
// We use a small enum packing: low bit = direction (0=down 1=up), rest = param idx (0=a,1=b,2=c,3=d)
struct DimParamEvt { MidiController* mc; int param; int dir; };
static DimParamEvt s_paramEvt[8];  // 4 params × 2 directions

static void cbDimParam(lv_event_t* e) {
    auto* pe = static_cast<DimParamEvt*>(lv_event_get_user_data(e));
    if (!pe || !pe->mc) return;
    auto& ds = pe->mc->dim_;
    float* p[4] = {&ds.a, &ds.b, &ds.c, &ds.d};
    lv_obj_t** lbls[4] = {&ds.aLbl, &ds.bLbl, &ds.cLbl, &ds.dLbl};
    static const char* names[4] = {"A", "B", "C", "D"};
    float& v = *p[pe->param];
    v += (pe->dir > 0) ? 1.0f : -1.0f;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (*lbls[pe->param]) {
        char buf[12]; snprintf(buf, sizeof(buf), "%s:%.0f", names[pe->param], v);
        lv_label_set_text(*lbls[pe->param], buf);
    }
}

static void cbDimOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.octave > -4) ds.octave--;
    if (ds.octLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "Oct%+d", ds.octave);
        lv_label_set_text(ds.octLbl, buf);
    }
}
static void cbDimOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.octave < 4) ds.octave++;
    if (ds.octLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "Oct%+d", ds.octave);
        lv_label_set_text(ds.octLbl, buf);
    }
}

static void cbDimRndDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.rnd > 0) ds.rnd -= 5.0f;
    if (ds.rnd < 0) ds.rnd = 0;
    if (ds.rndLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "R:%.0f", ds.rnd);
        lv_label_set_text(ds.rndLbl, buf);
    }
}
static void cbDimRndUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.rnd < 64) ds.rnd += 5.0f;
    if (ds.rndLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "R:%.0f", ds.rnd);
        lv_label_set_text(ds.rndLbl, buf);
    }
}

static void cbDimBpmDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.bpm > 40) ds.bpm -= 5;
    if (ds.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ds.bpm);
        lv_label_set_text(ds.bpmLbl, buf);
    }
}
static void cbDimBpmUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& ds = mc->dim_;
    if (ds.bpm < 280) ds.bpm += 5;
    if (ds.bpmLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ds.bpm);
        lv_label_set_text(ds.bpmLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void buildDimensionsView(MidiController* mc, lv_obj_t* parent) {
    auto& ds = mc->dim_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int smallBtnW = 88;
    int valW      = 152;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080C14), 0);
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

    auto makeSepLbl = [&](lv_obj_t* par) {
        lv_obj_t* sep = lv_label_create(par);
        lv_label_set_text(sep, "|");
        lv_obj_set_style_text_font(sep, fontSmall, 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(0x334455), 0);
    };

    // -- Row 1: [<] [EQ-] [Eq:N] [EQ+] | [PLAY] [RESET] -----------------------
    {
        lv_obj_t* row = makeRow();
        mcBackBtn(row, cbDimBack, mc, MC_W_S);
        lv_obj_t* titleLbl = lv_label_create(row);
        lv_label_set_text(titleLbl, "DIMENSIONS");
        lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
        lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x44BBFF), 0);
        lv_obj_set_flex_grow(titleLbl, 1);
        makeBtn(row, "<", smallBtnW, cbDimEqDown, mc);
        char buf[16]; snprintf(buf, sizeof(buf), "Eq:%d", ds.equation);
        ds.eqLbl = makeLbl(row, buf, 144, lv_color_hex(0xFF8844));
        makeBtn(row, ">", smallBtnW, cbDimEqUp, mc);
        makeSepLbl(row);
        lv_obj_t* playBtn = makeBtn(row, ds.playing ? "STOP" : "PLAY", 176, cbDimPlay, mc);
        ds.playLbl = lv_obj_get_child(playBtn, 0);
        makeBtn(row, "RESET", 192, cbDimReset, mc);
    }

    // -- Row 2: A and B params --------------------------------------------------
    {
        lv_obj_t* row = makeRow();
        // A
        s_paramEvt[0] = {mc, 0, -1};
        s_paramEvt[1] = {mc, 0, +1};
        makeBtn(row, "A-", smallBtnW, cbDimParam, &s_paramEvt[0]);
        char bufA[12]; snprintf(bufA, sizeof(bufA), "A:%.0f", ds.a);
        ds.aLbl = makeLbl(row, bufA, valW, lv_color_hex(0x88DDFF));
        makeBtn(row, "A+", smallBtnW, cbDimParam, &s_paramEvt[1]);
        makeSepLbl(row);
        // B
        s_paramEvt[2] = {mc, 1, -1};
        s_paramEvt[3] = {mc, 1, +1};
        makeBtn(row, "B-", smallBtnW, cbDimParam, &s_paramEvt[2]);
        char bufB[12]; snprintf(bufB, sizeof(bufB), "B:%.0f", ds.b);
        ds.bLbl = makeLbl(row, bufB, valW, lv_color_hex(0x88FFCC));
        makeBtn(row, "B+", smallBtnW, cbDimParam, &s_paramEvt[3]);
    }

    // -- Row 3: C and D params --------------------------------------------------
    {
        lv_obj_t* row = makeRow();
        // C
        s_paramEvt[4] = {mc, 2, -1};
        s_paramEvt[5] = {mc, 2, +1};
        makeBtn(row, "C-", smallBtnW, cbDimParam, &s_paramEvt[4]);
        char bufC[12]; snprintf(bufC, sizeof(bufC), "C:%.0f", ds.c);
        ds.cLbl = makeLbl(row, bufC, valW, lv_color_hex(0xFFCC88));
        makeBtn(row, "C+", smallBtnW, cbDimParam, &s_paramEvt[5]);
        makeSepLbl(row);
        // D
        s_paramEvt[6] = {mc, 3, -1};
        s_paramEvt[7] = {mc, 3, +1};
        makeBtn(row, "D-", smallBtnW, cbDimParam, &s_paramEvt[6]);
        char bufD[12]; snprintf(bufD, sizeof(bufD), "D:%.0f", ds.d);
        ds.dLbl = makeLbl(row, bufD, valW, lv_color_hex(0xFF88CC));
        makeBtn(row, "D+", smallBtnW, cbDimParam, &s_paramEvt[7]);
    }

    // -- Output area (flex_grow) ------------------------------------------------
    {
        lv_obj_t* outArea = mcContainer(root);
        lv_obj_set_width(outArea, LV_PCT(100));
        lv_obj_set_flex_grow(outArea, 1);
        lv_obj_set_style_bg_color(outArea, lv_color_hex(0x05080F), 0);
        lv_obj_set_style_bg_opa(outArea, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(outArea, 0, 0);
        lv_obj_set_style_pad_all(outArea, 24, 0);
        lv_obj_set_scrollable(outArea, false);
        lv_obj_set_flex_flow(outArea, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(outArea, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        ds.outLbl = lv_label_create(outArea);
        lv_label_set_text(ds.outLbl, "px: --   py: --   pz: --   t: --");
        lv_obj_set_style_text_font(ds.outLbl, fontNormal, 0);
        lv_obj_set_style_text_color(ds.outLbl, lv_color_hex(0x44BBEE), 0);
        lv_obj_set_width(ds.outLbl, LV_PCT(100));
        lv_obj_set_style_text_align(ds.outLbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    // -- Row 5: OCT  RND  BPM --------------------------------------------------
    {
        lv_obj_t* row = makeRow();
        // Octave
        makeBtn(row, "<", smallBtnW, cbDimOctDown, mc);
        char bufOct[10]; snprintf(bufOct, sizeof(bufOct), "Oct%+d", ds.octave);
        ds.octLbl = makeLbl(row, bufOct, 168, lv_color_hex(0xAAFF88));
        makeBtn(row, ">", smallBtnW, cbDimOctUp, mc);
        makeSepLbl(row);
        // Rnd
        makeBtn(row, "<", smallBtnW, cbDimRndDown, mc);
        char bufRnd[10]; snprintf(bufRnd, sizeof(bufRnd), "R:%.0f", ds.rnd);
        ds.rndLbl = makeLbl(row, bufRnd, 144, lv_color_hex(0xFFAA44));
        makeBtn(row, ">", smallBtnW, cbDimRndUp, mc);
        makeSepLbl(row);
        // BPM
        makeBtn(row, "<", smallBtnW, cbDimBpmDown, mc);
        char bufBpm[8]; snprintf(bufBpm, sizeof(bufBpm), "%d", ds.bpm);
        ds.bpmLbl = makeLbl(row, bufBpm, 144, lv_color_hex(0xFFE000));
        makeBtn(row, ">", smallBtnW, cbDimBpmUp, mc);
    }

    // -- Start timer (20ms poll) ------------------------------------------------
    mc->dimTimer_ = lv_timer_create(cbDimTimer, 20, mc);
    ds.tickMs = lv_tick_get();
}

// ---------------------------------------------------------------------------
// Public encoder entry points (called from EncoderInput.cpp)
// ---------------------------------------------------------------------------

void dimTogglePlay(MidiController* mc) {
    auto& ds = mc->dim_;
    ds.playing = !ds.playing;
    if (!ds.playing && ds.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ds.lastNote, 0);
        ds.lastNote = -1;
    }
    if (ds.playing) ds.tickMs = lv_tick_get();
    if (ds.playLbl) lv_label_set_text(ds.playLbl, ds.playing ? "STOP" : "PLAY");
}

void dimReset(MidiController* mc) {
    auto& ds = mc->dim_;
    ds.t = 0.0f;
    ds.a = 50; ds.b = 50; ds.c = 50; ds.d = 50;
    if (ds.aLbl) lv_label_set_text(ds.aLbl, "A:50");
    if (ds.bLbl) lv_label_set_text(ds.bLbl, "B:50");
    if (ds.cLbl) lv_label_set_text(ds.cLbl, "C:50");
    if (ds.dLbl) lv_label_set_text(ds.dLbl, "D:50");
    if (ds.lastNote >= 0) {
        mc->sendMidi(0x80, (uint8_t)ds.lastNote, 0);
        ds.lastNote = -1;
    }
}

void dimChangeOctave(MidiController* mc, int delta) {
    auto& ds = mc->dim_;
    int oct = ds.octave + delta;
    if (oct < -4) oct = -4;
    if (oct > 4)  oct = 4;
    ds.octave = oct;
    if (ds.octLbl) {
        char buf[10]; snprintf(buf, sizeof(buf), "Oct%+d", oct);
        lv_label_set_text(ds.octLbl, buf);
    }
}

void dimChangeRandom(MidiController* mc, int delta) {
    auto& ds = mc->dim_;
    float r = ds.rnd + (float)delta;
    if (r < 0)  r = 0;
    if (r > 64) r = 64;
    ds.rnd = r;
    if (ds.rndLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "R:%.0f", r);
        lv_label_set_text(ds.rndLbl, buf);
    }
}
