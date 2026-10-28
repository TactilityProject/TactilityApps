#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// ZEN view - Ambient bouncing ball physics + note triggering
//
// Layout (flex column):
//   [physics area - flex_grow, LV_EVENT_DRAW_MAIN]
//   [control row: < | ADD | RESET | SCALE | KEY- | KEY+ | OCT]
// ---------------------------------------------------------------------------


static constexpr int TOP_SEGS    = 8;
static constexpr int BOTTOM_SEGS = 8;
static constexpr int SIDE_SEGS   = 4;
static constexpr int TOTAL_WALL_SEGS = TOP_SEGS + BOTTOM_SEGS + SIDE_SEGS * 2;

struct WallSeg {
    float x1, y1, x2, y2;  // normalized 0..1
    int   noteIdx;
    int   scaleDegree;
    int   octaveOffset;
};

static void buildWalls(WallSeg walls[]) {
    float segW = 1.0f / TOP_SEGS;
    for (int i = 0; i < TOP_SEGS; i++) {
        walls[i] = { i * segW, 0.0f, (i+1) * segW, 0.0f, i, i, 0 };
    }
    for (int i = 0; i < BOTTOM_SEGS; i++) {
        walls[TOP_SEGS + i] = { i * segW, 1.0f, (i+1) * segW, 1.0f,
                                TOP_SEGS + i, BOTTOM_SEGS - 1 - i, 0 };
    }
    float segH = 1.0f / SIDE_SEGS;
    for (int i = 0; i < SIDE_SEGS; i++) {
        int idx = TOP_SEGS + BOTTOM_SEGS + i;
        walls[idx] = { 0.0f, i * segH, 0.0f, (i+1) * segH, idx, i, 1 };
    }
    for (int i = 0; i < SIDE_SEGS; i++) {
        int idx = TOP_SEGS + BOTTOM_SEGS + SIDE_SEGS + i;
        walls[idx] = { 1.0f, i * segH, 1.0f, (i+1) * segH,
                       idx, SIDE_SEGS - 1 - i, 1 };
    }
}

static const WallSeg* getWalls() {
    static WallSeg s_walls[TOTAL_WALL_SEGS];
    static bool s_built = false;
    if (!s_built) {
        buildWalls(s_walls);
        s_built = true;
    }
    return s_walls;
}

static constexpr uint32_t ZEN_COLORS[] = {
    0xFF4444, 0xFF8800, 0xFFFF00, 0x44FF44,
};

// The ambient mode reads better slightly slower than the original tuning.
static constexpr float ZEN_SPEED_FACTOR = 0.90f;

// Tracks whether any wall is currently flashing, so the canvas can be fully
// repainted once when the last flash ends (without repainting it every frame).
static bool s_zenFlashActive = false;

// ---------------------------------------------------------------------------
// Shared first-ball initializer (called from cbCanvasSize and cbZenTimer fallback)
// ---------------------------------------------------------------------------

// Note: does NOT update ballCntLbl - callers are responsible for label refresh.
// cbZenTimer updates it explicitly; cbCanvasSize relies on buildZenView initializing it to "Balls: x1".
static void initFirstBall(MidiController::ZenState& zn, int w, int h) {
    float speedScale = (float)w / 220.0f;
    zn.balls[0].x = w * 0.3f;
    zn.balls[0].y = h * 0.4f;
    zn.balls[0].vx = 2.0f * speedScale * ZEN_SPEED_FACTOR;
    zn.balls[0].vy = 1.5f * speedScale * ZEN_SPEED_FACTOR;
    zn.balls[0].radius = 20;
    zn.balls[0].color  = ZEN_COLORS[0];
    zn.balls[0].active = true;
    zn.numBalls = 1;
}

// ---------------------------------------------------------------------------
// Draw event - draws background and wall segments only; balls are lv_obj_t children
// ---------------------------------------------------------------------------

static void cbZenDraw(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& zn = mc->zen_;

    lv_obj_t*   obj   = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t ox = area.x1, oy = area.y1;
    int32_t cW = lv_area_get_width(&area);
    int32_t cH = lv_area_get_height(&area);

    // Update dimensions only if they look valid (never write 0 to physics state)
    if (cW > 0 && cH > 0) {
        zn.canvasW = (int)cW;
        zn.canvasH = (int)cH;
    }

    // Background fill
    lv_draw_fill_dsc_t bg;
    lv_draw_fill_dsc_init(&bg);
    bg.color = lv_color_hex(0x080C12);
    bg.opa   = LV_OPA_COVER;
    lv_draw_fill(layer, &bg, &area);

    uint32_t nowMs = lv_tick_get();

    const WallSeg* walls = getWalls();

    // Draw wall segments
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 12;
    line.opa   = LV_OPA_COVER;

    for (int w = 0; w < TOTAL_WALL_SEGS; w++) {
        bool flashing = (nowMs < zn.wallFlashUntilMs[w]);
        line.color = flashing ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x1A3050);
        line.p1.x = ox + (lv_value_precise_t)(walls[w].x1 * (float)(cW - 1));
        line.p1.y = oy + (lv_value_precise_t)(walls[w].y1 * (float)(cH - 1));
        line.p2.x = ox + (lv_value_precise_t)(walls[w].x2 * (float)(cW - 1));
        line.p2.y = oy + (lv_value_precise_t)(walls[w].y2 * (float)(cH - 1));
        lv_draw_line(layer, &line);
    }
}

// ---------------------------------------------------------------------------
// Physics timer - updates state, invalidates object to trigger redraw
// ---------------------------------------------------------------------------

// Create or show a ball widget for slot b
static void ensureBallWidget(MidiController* mc, int b) {
    auto& zn = mc->zen_;
    if (zn.ballObj[b]) return;
    int r = zn.balls[b].radius;
    lv_obj_t* w = mcContainer(zn.canvas);
    lv_obj_set_size(w, r * 2, r * 2);
    lv_obj_set_style_radius(w, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(w, lv_color_hex(ZEN_COLORS[b % 4]), 0);
    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(w, 0, 0);
    lv_obj_set_style_pad_all(w, 0, 0);
    lv_obj_set_scrollable(w, false);
    lv_obj_set_clickable(w, false);
    lv_obj_set_pos(w, (lv_coord_t)(zn.balls[b].x - r), (lv_coord_t)(zn.balls[b].y - r));
    zn.ballObj[b] = w;
}

static void scheduleZenNoteOff(MidiController* mc, uint8_t note, uint32_t delayMs) {
    auto& zn = mc->zen_;
    uint32_t offAt = lv_tick_get() + delayMs;
    // Find a free slot
    for (int i = 0; i < MidiController::ZenState::MAX_PENDING_OFFS; i++) {
        if (zn.pendingOffMs[i] == 0) {
            zn.pendingOffNote[i] = note;
            zn.pendingOffMs[i]   = offAt ? offAt : 1;  // avoid 0 sentinel
            return;
        }
    }
    // No free slot - send immediately as fallback
    mc->sendMidi(0x80, note, 0);
}

static void cbZenTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& zn = mc->zen_;
    lv_obj_t* canvas = zn.canvas;  // snapshot - must not use zn.canvas after ensureBallWidget
    if (!canvas || zn.canvasW <= 0 || zn.canvasH <= 0) return;

    // Process pending note-offs
    uint32_t nowMs = lv_tick_get();
    for (int i = 0; i < MidiController::ZenState::MAX_PENDING_OFFS; i++) {
        if (zn.pendingOffMs[i] != 0 && (int32_t)(nowMs - zn.pendingOffMs[i]) >= 0) {
            mc->sendMidi(0x80, zn.pendingOffNote[i], 0);
            zn.pendingOffMs[i] = 0;
        }
    }

    // Fallback: initialize first ball if SIZE_CHANGED fired before state was ready
    if (zn.numBalls == 0) {
        initFirstBall(zn, zn.canvasW, zn.canvasH);
        if (zn.ballCntLbl) lv_label_set_text(zn.ballCntLbl, "Balls: x1");
    }

    float cW = (float)zn.canvasW;
    float cH = (float)zn.canvasH;
    float minSpd = 1.5f * cW / 220.0f * ZEN_SPEED_FACTOR;

    const WallSeg* walls = getWalls();

    for (int b = 0; b < MidiController::ZenState::MAX_BALLS; b++) {
        auto& ball = zn.balls[b];
        if (!ball.active) continue;

        ball.x += ball.vx;
        ball.y += ball.vy;

        // Top bounce
        if (ball.y - ball.radius <= 0) {
            ball.y = (float)ball.radius;
            ball.vy = fabsf(ball.vy);
            if (fabsf(ball.vy) < minSpd) ball.vy = minSpd;
            float fx = ball.x / cW;
            int seg = (int)(fx * TOP_SEGS);
            seg = seg < 0 ? 0 : (seg >= TOP_SEGS ? TOP_SEGS - 1 : seg);
            int note = std::max(0, std::min(127, getNoteInScale(zn.scale, walls[seg].scaleDegree, zn.octave + walls[seg].octaveOffset, zn.key)));
            mc->sendMidi(0x90, (uint8_t)note, 70 + (int)(esp_random() % 40));
            scheduleZenNoteOff(mc, (uint8_t)note, 150);
            zn.wallFlashUntilMs[walls[seg].noteIdx] = nowMs + 200;
            lv_obj_invalidate(canvas);
        }
        // Bottom bounce
        if (ball.y + ball.radius >= cH) {
            ball.y = cH - ball.radius;
            ball.vy = -fabsf(ball.vy);
            if (fabsf(ball.vy) < minSpd) ball.vy = -minSpd;
            float fx = ball.x / cW;
            int seg = (int)(fx * BOTTOM_SEGS);
            seg = seg < 0 ? 0 : (seg >= BOTTOM_SEGS ? BOTTOM_SEGS - 1 : seg);
            int wallIdx = TOP_SEGS + seg;
            int note = std::max(0, std::min(127, getNoteInScale(zn.scale, walls[wallIdx].scaleDegree, zn.octave + walls[wallIdx].octaveOffset, zn.key)));
            mc->sendMidi(0x90, (uint8_t)note, 70 + (int)(esp_random() % 40));
            scheduleZenNoteOff(mc, (uint8_t)note, 150);
            zn.wallFlashUntilMs[walls[wallIdx].noteIdx] = nowMs + 200;
            lv_obj_invalidate(canvas);
        }
        // Left bounce
        if (ball.x - ball.radius <= 0) {
            ball.x = (float)ball.radius;
            ball.vx = fabsf(ball.vx);
            if (fabsf(ball.vx) < minSpd) ball.vx = minSpd;
            float fy = ball.y / cH;
            int seg = (int)(fy * SIDE_SEGS);
            seg = seg < 0 ? 0 : (seg >= SIDE_SEGS ? SIDE_SEGS - 1 : seg);
            int wallIdx = TOP_SEGS + BOTTOM_SEGS + seg;
            int note = std::max(0, std::min(127, getNoteInScale(zn.scale, walls[wallIdx].scaleDegree, zn.octave + walls[wallIdx].octaveOffset, zn.key)));
            mc->sendMidi(0x90, (uint8_t)note, 70 + (int)(esp_random() % 40));
            scheduleZenNoteOff(mc, (uint8_t)note, 150);
            zn.wallFlashUntilMs[walls[wallIdx].noteIdx] = nowMs + 200;
            lv_obj_invalidate(canvas);
        }
        // Right bounce
        if (ball.x + ball.radius >= cW) {
            ball.x = cW - ball.radius;
            ball.vx = -fabsf(ball.vx);
            if (fabsf(ball.vx) < minSpd) ball.vx = -minSpd;
            float fy = ball.y / cH;
            int seg = (int)(fy * SIDE_SEGS);
            seg = seg < 0 ? 0 : (seg >= SIDE_SEGS ? SIDE_SEGS - 1 : seg);
            int wallIdx = TOP_SEGS + BOTTOM_SEGS + SIDE_SEGS + seg;
            int note = std::max(0, std::min(127, getNoteInScale(zn.scale, walls[wallIdx].scaleDegree, zn.octave + walls[wallIdx].octaveOffset, zn.key)));
            mc->sendMidi(0x90, (uint8_t)note, 70 + (int)(esp_random() % 40));
            scheduleZenNoteOff(mc, (uint8_t)note, 150);
            zn.wallFlashUntilMs[walls[wallIdx].noteIdx] = nowMs + 200;
            lv_obj_invalidate(canvas);
        }
    }

    // Update ball widget positions (create if needed, hide if inactive)
    for (int b = 0; b < MidiController::ZenState::MAX_BALLS; b++) {
        auto& ball = zn.balls[b];
        if (ball.active) {
            ensureBallWidget(mc, b);
            int r = ball.radius;
            lv_obj_set_pos(zn.ballObj[b], (lv_coord_t)(ball.x - r), (lv_coord_t)(ball.y - r));
            lv_obj_set_hidden(zn.ballObj[b], false);
        } else if (zn.ballObj[b]) {
            lv_obj_set_hidden(zn.ballObj[b], true);
        }
    }

    // Moving the ball widgets already invalidates their old and new areas, so
    // the canvas only needs a full repaint when a wall flash starts (handled at
    // collision time) or when the last flash expires.
    bool anyFlash = false;
    for (int w = 0; w < TOTAL_WALL_SEGS; w++) {
        if (nowMs < zn.wallFlashUntilMs[w]) { anyFlash = true; break; }
    }
    if (s_zenFlashActive && !anyFlash && canvas && canvas == zn.canvas) {
        lv_obj_invalidate(canvas);
    }
    s_zenFlashActive = anyFlash;
}

// ---------------------------------------------------------------------------
// Canvas size event - initialize balls after first layout
// ---------------------------------------------------------------------------

static void cbCanvasSize(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& zn = mc->zen_;
    lv_obj_t* obj = lv_event_get_target_obj(e);
    int w = (int)lv_obj_get_content_width(obj);
    int h = (int)lv_obj_get_content_height(obj);
    if (w <= 0 || h <= 0) return;

    zn.canvasW = w;
    zn.canvasH = h;

    // Initialize first ball on first valid layout
    if (zn.numBalls == 0) {
        initFirstBall(zn, w, h);
    }
}

// ---------------------------------------------------------------------------
// Control callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbAddBall(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& zn = mc->zen_;
    if (zn.numBalls >= MidiController::ZenState::MAX_BALLS) return;

    int b = zn.numBalls;
    zn.balls[b].x = zn.canvasW * 0.3f + (float)(esp_random() % 100) * 0.01f * zn.canvasW * 0.4f;
    zn.balls[b].y = zn.canvasH * 0.3f + (float)(esp_random() % 100) * 0.01f * zn.canvasH * 0.4f;
    float speedScale = (float)zn.canvasW / 220.0f;
    float spd = (1.5f + (float)(esp_random() % 14) * 0.1f) * speedScale * ZEN_SPEED_FACTOR;
    float ang = (float)(esp_random() % 628) * 0.01f;
    zn.balls[b].vx = cosf(ang) * spd;
    zn.balls[b].vy = sinf(ang) * spd;
    zn.balls[b].radius = 20;
    zn.balls[b].color  = ZEN_COLORS[b % 4];
    zn.balls[b].active = true;
    zn.numBalls++;
    if (zn.ballCntLbl) {
        char buf[24]; snprintf(buf, sizeof(buf), "Balls: x%d", zn.numBalls);
        lv_label_set_text(zn.ballCntLbl, buf);
    }
}

static void cbReset(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& zn = mc->zen_;
    for (int b = 1; b < MidiController::ZenState::MAX_BALLS; b++) zn.balls[b].active = false;
    zn.numBalls = 1;
    if (zn.ballCntLbl) lv_label_set_text(zn.ballCntLbl, "Balls: x1");
}

static void cbScaleCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->zen_.scale = (mc->zen_.scale + 1) % NUM_MIDI_SCALES;
    if (mc->zen_.statusLbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %s", KEY_NAMES[mc->zen_.key], MIDI_SCALES[mc->zen_.scale].name);
        lv_label_set_text(mc->zen_.statusLbl, buf);
    }
}

static void cbKeyDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->zen_.key = (mc->zen_.key + 11) % 12;
    if (mc->zen_.statusLbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %s", KEY_NAMES[mc->zen_.key], MIDI_SCALES[mc->zen_.scale].name);
        lv_label_set_text(mc->zen_.statusLbl, buf);
    }
}
static void cbKeyUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->zen_.key = (mc->zen_.key + 1) % 12;
    if (mc->zen_.statusLbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %s", KEY_NAMES[mc->zen_.key], MIDI_SCALES[mc->zen_.scale].name);
        lv_label_set_text(mc->zen_.statusLbl, buf);
    }
}

static void cbOctCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->zen_.octave++;
    if (mc->zen_.octave > 6) mc->zen_.octave = 2;
    if (mc->zen_.octLbl) {
        char buf[6]; snprintf(buf, sizeof(buf), "O:%d", mc->zen_.octave);
        lv_label_set_text(mc->zen_.octLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

static void cbZenRootDelete(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& zn = mc->zen_;

    // Stop and delete the zen timer so it cannot fire after widgets are gone
    if (mc->zenTimer_) {
        lv_timer_delete(mc->zenTimer_);
        mc->zenTimer_ = nullptr;
    }

    // Flush all pending note-offs immediately so no notes get stuck
    for (int i = 0; i < MidiController::ZenState::MAX_PENDING_OFFS; i++) {
        if (zn.pendingOffMs[i] != 0) {
            mc->sendMidi(0x80, zn.pendingOffNote[i], 0);
            zn.pendingOffMs[i] = 0;
        }
    }

    // Null all stored LVGL pointers
    zn.canvas     = nullptr;
    zn.statusLbl  = nullptr;
    zn.ballCntLbl = nullptr;
    zn.octLbl     = nullptr;
    for (int b = 0; b < MidiController::ZenState::MAX_BALLS; b++)
        zn.ballObj[b] = nullptr;
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildZenView(MidiController* mc, lv_obj_t* parent) {
    auto& zn = mc->zen_;

    const lv_font_t* fontSmall = lvgl_get_text_font(FONT_SIZE_SMALL);
    int ctrlH = 88;
    int btnH  = MC_BTN_H;

    lv_obj_t* root = mcContainer(parent);
    lv_obj_add_event_cb(root, cbZenRootDelete, LV_EVENT_DELETE, mc);
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

    auto makeCtrlBtn = [&](const char* lbl, lv_event_cb_t cb) {
        lv_obj_t* btn = mcButton(ctrl);
        lv_obj_set_size(btn, 112, btnH);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, mc);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, lbl);
        lv_obj_set_style_text_font(l, fontSmall, 0);
        lv_obj_center(l);
    };

    mcBackBtn(ctrl, cbBack, mc, MC_W_XS);

    makeCtrlBtn("ADD",   cbAddBall);
    makeCtrlBtn("RESET", cbReset);
    makeCtrlBtn("SCALE", cbScaleCycle);
    makeCtrlBtn("KEY-",  cbKeyDown);
    makeCtrlBtn("KEY+",  cbKeyUp);
    makeCtrlBtn("OCT",   cbOctCycle);

    zn.octLbl = lv_label_create(ctrl);
    { char buf[6]; snprintf(buf, sizeof(buf), "O:%d", zn.octave); lv_label_set_text(zn.octLbl, buf); }
    lv_obj_set_style_text_font(zn.octLbl, fontSmall, 0);
    lv_obj_set_style_text_color(zn.octLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_width(zn.octLbl, 112);
    lv_obj_set_style_text_align(zn.octLbl, LV_TEXT_ALIGN_CENTER, 0);

    zn.ballCntLbl = lv_label_create(ctrl);
    lv_label_set_text(zn.ballCntLbl, "Balls: x1");
    lv_obj_set_style_text_font(zn.ballCntLbl, fontSmall, 0);
    lv_obj_set_style_text_color(zn.ballCntLbl, lv_color_hex(0x666688), 0);

    zn.statusLbl = lv_label_create(ctrl);
    { char sbuf[24]; snprintf(sbuf, sizeof(sbuf), "%s %s", KEY_NAMES[zn.key], MIDI_SCALES[zn.scale].name);
      lv_label_set_text(zn.statusLbl, sbuf); }
    lv_obj_set_style_text_font(zn.statusLbl, fontSmall, 0);
    lv_obj_set_style_text_color(zn.statusLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_flex_grow(zn.statusLbl, 1);
    lv_obj_set_style_text_align(zn.statusLbl, LV_TEXT_ALIGN_RIGHT, 0);

    // -- Physics area (fills remaining space below controls) -------------------
    lv_obj_t* physArea = mcContainer(root);
    lv_obj_set_width(physArea, LV_PCT(100));
    lv_obj_set_flex_grow(physArea, 1);
    lv_obj_set_style_bg_color(physArea, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(physArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(physArea, 0, 0);
    lv_obj_set_style_pad_all(physArea, 0, 0);
    lv_obj_set_scrollable(physArea, false);
    lv_obj_add_event_cb(physArea, cbZenDraw,    LV_EVENT_DRAW_MAIN,    mc);
    lv_obj_add_event_cb(physArea, cbCanvasSize, LV_EVENT_SIZE_CHANGED, mc);
    zn.canvas = physArea;

    // Start timer
    mc->zenTimer_ = lv_timer_create(cbZenTimer, 16, mc);
}
