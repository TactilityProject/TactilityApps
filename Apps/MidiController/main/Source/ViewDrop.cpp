#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// DROP view - Gravity-drop physics + platform note triggering
//
// Layout (flex column):
//   [physics area - flex_grow, touch to drop/place]
//   [control row: < | MODE | OCT- | Oct N | OCT+ | CLEAR | SCALE | KEY- | KEY+ | status]
//
// Two modes:
//   DROP (default): touch spawns a ball at touch point
//   EDIT: touch places a horizontal platform (up to MAX_PLATFORMS); CLEAR removes all
// ---------------------------------------------------------------------------


static const uint32_t DROP_BALL_COLORS[] = {
    0xFF8800, 0xFF4444, 0x44FFAA, 0xAA44FF,
    0xFFFF44, 0x44AAFF, 0xFF44AA, 0x44FF44
};

static constexpr float GRAVITY_BASE  = 0.16f;
static constexpr float BOUNCE_COEFF  = 0.58f;
static constexpr float FRICTION      = 0.99f;
static constexpr float BALL_LIFE_MS  = 14000.0f;

// Platform geometry is expressed at the 220px reference width used by the
// physics scaling. The line in the draw callback and the collision half-height
// must stay in sync, or balls visibly sink into or float above a platform.
static constexpr float PLATFORM_HALF_H      = 4.0f;
static constexpr float PLATFORM_W_FRACTION  = 0.16f;

// ---------------------------------------------------------------------------
// Physics helpers
// ---------------------------------------------------------------------------

struct Vec2 { float x, y; };
static float dot(Vec2 a, Vec2 b) { return a.x*b.x + a.y*b.y; }

static void updatePlatformNote(MidiController* mc, int p) {
    auto& dr = mc->drop_;
    int degree = (p * 2) % 7;
    dr.platforms[p].note = getNoteInScale(dr.scale, degree, dr.octave, dr.key);
}

static void refreshPlatformLabel(MidiController* mc, int p) {
    auto& dr = mc->drop_;
    if (!dr.canvas || p < 0 || p >= dr.numPlatforms) return;
    auto& plt = dr.platforms[p];

    // Create label widget if it doesn't exist yet
    if (!dr.platLbl[p]) {
        lv_obj_t* lbl = lv_label_create(dr.canvas);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
        lv_obj_set_style_bg_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
        lv_obj_set_style_border_width(lbl, 0, 0);
        lv_obj_set_style_pad_all(lbl, 4, 0);
        lv_obj_set_style_radius(lbl, 8, 0);
        lv_obj_set_scrollable(lbl, false);
        lv_obj_set_clickable(lbl, false);
        dr.platLbl[p] = lbl;
    }

    char noteName[8]; midiNoteName(plt.note, noteName, sizeof(noteName));
    lv_label_set_text(dr.platLbl[p], noteName);
    // Size to content so dark bg pill fits text exactly
    lv_obj_set_size(dr.platLbl[p], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // Let LVGL compute content size first, then center on platform
    lv_obj_update_layout(dr.platLbl[p]);
    lv_coord_t lblW = lv_obj_get_width(dr.platLbl[p]);
    lv_coord_t lblH = lv_obj_get_height(dr.platLbl[p]);
    lv_obj_set_pos(dr.platLbl[p],
        (lv_coord_t)(plt.x - lblW / 2),
        (lv_coord_t)(plt.y - lblH / 2));
}

static void rebuildPlatformNotes(MidiController* mc) {
    auto& dr = mc->drop_;
    for (int p = 0; p < dr.numPlatforms; p++) {
        updatePlatformNote(mc, p);
        refreshPlatformLabel(mc, p);
    }
    // Platform line colours are derived from their notes, so the canvas needs
    // a repaint even though only the labels changed.
    if (dr.canvas) lv_obj_invalidate(dr.canvas);
}

// ---------------------------------------------------------------------------
// Draw event
// ---------------------------------------------------------------------------

static void cbDropDraw(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& dr = mc->drop_;

    lv_obj_t*   obj   = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t ox = area.x1, oy = area.y1;

    // Background
    lv_draw_fill_dsc_t bg;
    lv_draw_fill_dsc_init(&bg);
    bg.color = lv_color_hex(0x080C12);
    bg.opa   = LV_OPA_COVER;
    lv_draw_fill(layer, &bg, &area);

    if (dr.numPlatforms == 0) return;

    // Note-based platform colors (hue cycle per semitone)
    static const uint32_t NOTE_COLORS[12] = {
        0xFF3333, 0xFF6600, 0xFFAA00, 0xFFFF00,
        0x88FF00, 0x00FF88, 0x00FFFF, 0x0088FF,
        0x0033FF, 0x8800FF, 0xFF00FF, 0xFF0088
    };

    // Draw platforms - line width matches physics platH (half-thickness × 2)
    float speedScale = (dr.canvasW > 0) ? (float)dr.canvasW / 220.0f : 1.0f;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = std::max(2, (int)(PLATFORM_HALF_H * 2.0f * speedScale));
    line.opa   = LV_OPA_COVER;

    for (int p = 0; p < dr.numPlatforms; p++) {
        auto& plt = dr.platforms[p];
        float cosA = cosf(plt.angle);
        float sinA = sinf(plt.angle);
        float hw   = plt.w * 0.5f;
        line.color = lv_color_hex(NOTE_COLORS[plt.note % 12]);
        line.p1.x = (lv_value_precise_t)(ox + plt.x - hw * cosA);
        line.p1.y = (lv_value_precise_t)(oy + plt.y - hw * sinA);
        line.p2.x = (lv_value_precise_t)(ox + plt.x + hw * cosA);
        line.p2.y = (lv_value_precise_t)(oy + plt.y + hw * sinA);
        lv_draw_line(layer, &line);
    }
}

// ---------------------------------------------------------------------------
// Physics timer
// ---------------------------------------------------------------------------

static void cbDropTimer(lv_timer_t* t) {
    auto* mc = static_cast<MidiController*>(lv_timer_get_user_data(t));
    if (!mc) return;
    auto& dr = mc->drop_;
    if (!dr.canvas || dr.canvasW <= 0 || dr.canvasH <= 0) return;

    float cW = (float)dr.canvasW;
    float cH = (float)dr.canvasH;
    float speedScale = cW / 220.0f;
    float gravity = GRAVITY_BASE * speedScale;
    // Platform half-thickness scales with display so balls hit reliably
    float platH = PLATFORM_HALF_H * speedScale;
    int   ballR = 16;
    uint32_t nowMs = lv_tick_get();

    for (int b = 0; b < MidiController::DropState::MAX_BALLS; b++) {
        auto& ball = dr.balls[b];
        if (!ball.active) continue;

        if ((nowMs - ball.spawnedMs) > (uint32_t)BALL_LIFE_MS) {
            ball.active = false;
            continue;
        }

        ball.vy += gravity;
        ball.vx *= FRICTION;
        ball.x  += ball.vx;
        ball.y  += ball.vy;

        // Floor bounce
        if (ball.y >= cH - ballR) {
            ball.y  = cH - ballR;
            ball.vy = -ball.vy * BOUNCE_COEFF;
            if (fabsf(ball.vy) < 0.5f * speedScale) ball.vy = 0;
        }
        // Wall clamp
        if (ball.x < ballR)       ball.x = ballR;
        if (ball.x > cW - ballR)  ball.x = cW - ballR;

        // Platform collisions
        for (int p = 0; p < dr.numPlatforms; p++) {
            auto& plt = dr.platforms[p];
            float cosA = cosf(plt.angle);
            float sinA = sinf(plt.angle);
            float dx = ball.x - plt.x;
            float dy = ball.y - plt.y;
            float localX =  dx * cosA + dy * sinA;
            float localY = -dx * sinA + dy * cosA;
            float halfW  = plt.w * 0.5f;

            if (fabsf(localX) < halfW && localY >= -platH && localY <= platH) {
                // Normal perpendicular to platform, pointing toward the ball
                Vec2 rawNormal = {-sinA, cosA};
                Vec2 vel       = {ball.vx, ball.vy};
                float rawVn    = dot(vel, rawNormal);
                // Pick the normal direction that opposes ball's velocity
                Vec2 normal = (rawVn < 0) ? rawNormal : Vec2{sinA, -cosA};
                float vn    = dot(vel, normal);
                if (vn < 0) {
                    ball.vx -= 2.0f * vn * normal.x;
                    ball.vy -= 2.0f * vn * normal.y;
                    ball.vx *= BOUNCE_COEFF;
                    ball.vy *= BOUNCE_COEFF;
                    // Push ball out along the normal so it clears the platform surface
                    ball.x = plt.x + (localX) * cosA + (platH + 1.0f) * normal.x;
                    ball.y = plt.y + (localX) * sinA + (platH + 1.0f) * normal.y;

                    if (fabsf(vn) > 1.0f * speedScale) {
                        int vel_midi = std::min(127, 70 + (int)(fabsf(vn) / speedScale * 5));
                        mc->sendMidi(0x90, (uint8_t)plt.note, vel_midi);
                        mc->sendMidi(0x80, (uint8_t)plt.note, 0);
                    }
                }
            }
        }
    }

    // Move the ball widgets. Moving/hiding a child invalidates the old and new
    // areas and LVGL repaints the canvas background there, so there is no need
    // to repaint the entire canvas at 60 fps. Full repaints are requested only
    // when the static scene (platforms/labels) changes.
    for (int b = 0; b < MidiController::DropState::MAX_BALLS; b++) {
        if (dr.ballObj[b]) {
            if (dr.balls[b].active) {
                lv_obj_set_pos(dr.ballObj[b],
                    (lv_coord_t)(dr.balls[b].x - ballR),
                    (lv_coord_t)(dr.balls[b].y - ballR));
                lv_obj_set_hidden(dr.ballObj[b], false);
            } else {
                lv_obj_set_hidden(dr.ballObj[b], true);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Canvas size event - just update dimensions, no auto-platform creation
// ---------------------------------------------------------------------------

static void cbCanvasSize(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& dr = mc->drop_;
    lv_obj_t* obj = lv_event_get_target_obj(e);
    int w = (int)lv_obj_get_content_width(obj);
    int h = (int)lv_obj_get_content_height(obj);
    if (w <= 0 || h <= 0) return;

    // If canvas size changed, scale existing platform positions
    if (dr.canvasW > 0 && dr.canvasH > 0 && (w != dr.canvasW || h != dr.canvasH)) {
        float sx = (float)w / dr.canvasW;
        float sy = (float)h / dr.canvasH;
        for (int p = 0; p < dr.numPlatforms; p++) {
            dr.platforms[p].x *= sx;
            dr.platforms[p].y *= sy;
            dr.platforms[p].w *= sx;
            refreshPlatformLabel(mc, p);
        }
    }
    dr.canvasW = w;
    dr.canvasH = h;
}

// ---------------------------------------------------------------------------
// Touch callback - spawns ball (DROP mode) or places platform (EDIT mode)
// ---------------------------------------------------------------------------

static void cbCanvasTouch(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& dr = mc->drop_;
    if (dr.canvasW <= 0 || dr.canvasH <= 0) return;

    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    lv_area_t area;
    lv_obj_get_coords(dr.canvas, &area);
    float lx = (float)((int)pt.x - (int)area.x1);
    float ly = (float)((int)pt.y - (int)area.y1);
    if (lx < 0 || ly < 0 || lx > dr.canvasW || ly > dr.canvasH) return;

    if (dr.editMode) {
        // Place a new horizontal platform at touch point (up to MAX_PLATFORMS)
        if (dr.numPlatforms >= MidiController::DropState::MAX_PLATFORMS) return;
        int p = dr.numPlatforms;
        dr.platforms[p].x     = lx;
        dr.platforms[p].y     = ly;
        dr.platforms[p].w     = (float)dr.canvasW * PLATFORM_W_FRACTION;
        dr.platforms[p].angle = 0.0f;  // always horizontal
        updatePlatformNote(mc, p);
        dr.numPlatforms++;
        refreshPlatformLabel(mc, p);
        lv_obj_invalidate(dr.canvas);
    } else {
        // Spawn a ball at touch point
        int ballR = 16;
        for (int b = 0; b < MidiController::DropState::MAX_BALLS; b++) {
            if (!dr.balls[b].active) {
                dr.balls[b].x = lx;
                dr.balls[b].y = ly;
                float vx = ((float)(esp_random() % 201) - 100.0f) * 0.012f * ((float)dr.canvasW / 220.0f);
                dr.balls[b].vx = vx;
                dr.balls[b].vy = 0.0f;
                dr.balls[b].active    = true;
                dr.balls[b].spawnedMs = lv_tick_get();
                if (!dr.ballObj[b]) {
                    lv_obj_t* w = mcContainer(dr.canvas);
                    lv_obj_set_size(w, ballR * 2, ballR * 2);
                    lv_obj_set_style_radius(w, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_color(w, lv_color_hex(DROP_BALL_COLORS[b % 8]), 0);
                    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(w, 0, 0);
                    lv_obj_set_style_pad_all(w, 0, 0);
                    lv_obj_set_scrollable(w, false);
                    lv_obj_set_clickable(w, false);
                    dr.ballObj[b] = w;
                }
                lv_obj_set_pos(dr.ballObj[b], (lv_coord_t)(lx - ballR), (lv_coord_t)(ly - ballR));
                lv_obj_set_hidden(dr.ballObj[b], false);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Control callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbModeToggle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& dr = mc->drop_;
    dr.editMode = !dr.editMode;
    // Button label shows what pressing again WILL do (toggle label)
    if (dr.modeLbl) lv_label_set_text(dr.modeLbl, dr.editMode ? "DROP" : "EDIT");
    if (dr.modeBtn) {
        lv_obj_set_style_bg_color(dr.modeBtn,
            dr.editMode ? lv_color_hex(0x008833) : lv_color_hex(0x003388), 0);
    }
    if (dr.canvas) lv_obj_invalidate(dr.canvas);
}

static void cbClear(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& dr = mc->drop_;
    // Clear balls
    for (int b = 0; b < MidiController::DropState::MAX_BALLS; b++)
        dr.balls[b].active = false;
    // Clear platform labels
    for (int p = 0; p < MidiController::DropState::MAX_PLATFORMS; p++) {
        if (dr.platLbl[p]) {
            lv_obj_delete(dr.platLbl[p]);
            dr.platLbl[p] = nullptr;
        }
    }
    dr.numPlatforms = 0;
    mc->stopAllNotes();
    if (dr.canvas) lv_obj_invalidate(dr.canvas);
}

static void cbScaleCycle(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->drop_.scale = (mc->drop_.scale + 1) % NUM_MIDI_SCALES;
    rebuildPlatformNotes(mc);
    if (mc->drop_.statusLbl) lv_label_set_text(mc->drop_.statusLbl, MIDI_SCALES[mc->drop_.scale].name);
}

static void cbKeyDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->drop_.key = (mc->drop_.key + 11) % 12;
    rebuildPlatformNotes(mc);
}
static void cbKeyUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->drop_.key = (mc->drop_.key + 1) % 12;
    rebuildPlatformNotes(mc);
}

static void cbOctDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->drop_.octave > 2) mc->drop_.octave--;
    rebuildPlatformNotes(mc);
    if (mc->drop_.octLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "Oct %d", mc->drop_.octave);
        lv_label_set_text(mc->drop_.octLbl, buf);
    }
}
static void cbOctUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    if (mc->drop_.octave < 7) mc->drop_.octave++;
    rebuildPlatformNotes(mc);
    if (mc->drop_.octLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "Oct %d", mc->drop_.octave);
        lv_label_set_text(mc->drop_.octLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildDropView(MidiController* mc, lv_obj_t* parent) {
    auto& dr = mc->drop_;

    // Reset volatile state on view entry. ballObj[]/platLbl[] were nulled by
    // switchToMode (previous root destroyed), null again here to be safe.
    dr.numPlatforms = 0;
    dr.editMode = false;
    for (auto& ball : dr.balls) ball.active = false;
    for (auto& obj  : dr.ballObj) obj = nullptr;
    for (auto& lbl  : dr.platLbl) lbl = nullptr;

    // Clamp octave to valid range [2,7]
    dr.octave = std::clamp(dr.octave, 2, 7);

    const lv_font_t* fontSmall = lvgl_get_text_font(FONT_SIZE_SMALL);
    int ctrlH = 88;

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

    auto makeBtn = [&](const char* lbl, int w, lv_event_cb_t cb) -> lv_obj_t* {
        return mcSmallBtn(ctrl, lbl, w, cb, mc);
    };

    // < (back)
    mcBackBtn(ctrl, cbBack, mc, 96);

    // MODE toggle: DROP / EDIT
    dr.modeBtn = makeBtn("EDIT", 176, cbModeToggle);
    lv_obj_set_style_bg_color(dr.modeBtn, lv_color_hex(0x003388), 0);
    dr.modeLbl = lv_obj_get_child(dr.modeBtn, 0);

    // OCT- label OCT+
    makeBtn("OCT-", 96, cbOctDown);
    char octBuf[8]; snprintf(octBuf, sizeof(octBuf), "Oct %d", dr.octave);
    dr.octLbl = lv_label_create(ctrl);
    lv_label_set_text(dr.octLbl, octBuf);
    lv_obj_set_style_text_font(dr.octLbl, fontSmall, 0);
    lv_obj_set_style_text_color(dr.octLbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(dr.octLbl, 64);
    lv_obj_set_style_text_align(dr.octLbl, LV_TEXT_ALIGN_CENTER, 0);
    makeBtn("OCT+", 96, cbOctUp);

    // CLR (clears balls and platforms)
    makeBtn("CLEAR", 128, cbClear);

    // SCALE | KEY- | KEY+
    lv_obj_t* scaleBtn = mcButton(ctrl);
    lv_obj_set_size(scaleBtn, 208, MC_BTN_H);
    lv_obj_add_event_cb(scaleBtn, cbScaleCycle, LV_EVENT_CLICKED, mc);
    dr.statusLbl = lv_label_create(scaleBtn);
    lv_label_set_text(dr.statusLbl, MIDI_SCALES[dr.scale].name);
    lv_obj_set_style_text_font(dr.statusLbl, fontSmall, 0);
    lv_obj_center(dr.statusLbl);

    makeBtn("KEY-", 96, cbKeyDown);
    makeBtn("KEY+", 96, cbKeyUp);

    // -- Physics area (fills remaining space below controls) -------------------
    lv_obj_t* physArea = mcContainer(root);
    lv_obj_set_width(physArea, LV_PCT(100));
    lv_obj_set_flex_grow(physArea, 1);
    lv_obj_set_style_bg_color(physArea, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(physArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(physArea, 0, 0);
    lv_obj_set_style_pad_all(physArea, 0, 0);
    lv_obj_set_scrollable(physArea, false);
    lv_obj_set_clickable(physArea, true);
    lv_obj_add_event_cb(physArea, cbDropDraw,    LV_EVENT_DRAW_MAIN,    mc);
    lv_obj_add_event_cb(physArea, cbCanvasSize,  LV_EVENT_SIZE_CHANGED, mc);
    lv_obj_add_event_cb(physArea, cbCanvasTouch, LV_EVENT_CLICKED,      mc);
    dr.canvas = physArea;

    // Start timer
    mc->dropTimer_ = lv_timer_create(cbDropTimer, 16, mc);
}
