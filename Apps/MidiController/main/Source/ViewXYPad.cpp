#include "MidiController.h"

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <cstdio>

// ---------------------------------------------------------------------------
// XY PAD view - 2D touch pad for MIDI CC modulation
//
// Layout (flex column):
//   [header: < | XY PAD | X CC -/val/+ | | Y CC -/val/+]
//   [touch area - flex_grow, LV_EVENT_DRAW_MAIN]
//   [value readout footer]
// ---------------------------------------------------------------------------


static constexpr int XY_IND_RADIUS = 32;  // indicator circle radius (pixels)

// ---------------------------------------------------------------------------
// Draw event - renders BG, crosshair, indicator circle
// ---------------------------------------------------------------------------

static void cbXYDraw(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;

    lv_obj_t*   obj   = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t ox = area.x1, oy = area.y1;
    int32_t cW = lv_area_get_width(&area);
    int32_t cH = lv_area_get_height(&area);

    // Background
    lv_draw_fill_dsc_t bg;
    lv_draw_fill_dsc_init(&bg);
    bg.color = lv_color_hex(0x080C12);
    bg.opa   = LV_OPA_COVER;
    lv_draw_fill(layer, &bg, &area);

    // Crosshair lines
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0x1A3040);
    line.width = 1;
    line.opa   = LV_OPA_COVER;

    // Horizontal
    line.p1.x = (lv_value_precise_t)ox;
    line.p1.y = (lv_value_precise_t)(oy + cH / 2);
    line.p2.x = (lv_value_precise_t)(ox + cW - 1);
    line.p2.y = (lv_value_precise_t)(oy + cH / 2);
    lv_draw_line(layer, &line);

    // Vertical
    line.p1.x = (lv_value_precise_t)(ox + cW / 2);
    line.p1.y = (lv_value_precise_t)oy;
    line.p2.x = (lv_value_precise_t)(ox + cW / 2);
    line.p2.y = (lv_value_precise_t)(oy + cH - 1);
    lv_draw_line(layer, &line);

}

static void updateValLabel(MidiController* mc) {
    if (!mc->xy_.valLbl) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "X:%3d  Y:%3d", mc->xy_.xVal, mc->xy_.yVal);
    lv_label_set_text(mc->xy_.valLbl, buf);
}

// ---------------------------------------------------------------------------
// Touch input on canvas
// ---------------------------------------------------------------------------

static void cbCanvasTouch(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& xy = mc->xy_;
    if (xy.canvasW <= 1 || xy.canvasH <= 1) return;

    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    lv_area_t area;
    lv_obj_get_coords(xy.canvas, &area);
    int lx = (int)pt.x - (int)area.x1;
    int ly = (int)pt.y - (int)area.y1;
    lx = lx < 0 ? 0 : (lx >= xy.canvasW ? xy.canvasW - 1 : lx);
    ly = ly < 0 ? 0 : (ly >= xy.canvasH ? xy.canvasH - 1 : ly);

    int newX = (lx * 127) / (xy.canvasW - 1);
    int newY = 127 - (ly * 127) / (xy.canvasH - 1);

    bool changed = (newX != xy.xVal) || (newY != xy.yVal);
    if (!changed) return;

    xy.xVal = newX;
    xy.yVal = newY;

    mc->sendMidi(0xB0, (uint8_t)xy.xCC, (uint8_t)xy.xVal);
    mc->sendMidi(0xB0, (uint8_t)xy.yCC, (uint8_t)xy.yVal);

    // Move indicator widget immediately
    if (xy.indicator && xy.canvasW > 0 && xy.canvasH > 0) {
        int indR = XY_IND_RADIUS;
        int px = (xy.xVal * (xy.canvasW - 1)) / 127;
        int py = ((127 - xy.yVal) * (xy.canvasH - 1)) / 127;
        lv_obj_set_pos(xy.indicator, (lv_coord_t)(px - indR), (lv_coord_t)(py - indR));
    }

    lv_obj_invalidate(xy.canvas);
    updateValLabel(mc);
}

static void cbCanvasSize(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    auto& xy = mc->xy_;
    lv_obj_t* obj = lv_event_get_target_obj(e);
    xy.canvasW = (int)lv_obj_get_content_width(obj);
    xy.canvasH = (int)lv_obj_get_content_height(obj);
    // Place indicator at current value position
    if (xy.indicator && xy.canvasW > 0 && xy.canvasH > 0) {
        int indR = XY_IND_RADIUS;
        int px = (xy.xVal * (xy.canvasW - 1)) / 127;
        int py = ((127 - xy.yVal) * (xy.canvasH - 1)) / 127;
        lv_obj_set_pos(xy.indicator, (lv_coord_t)(px - indR), (lv_coord_t)(py - indR));
    }
}

// ---------------------------------------------------------------------------
// CC control callbacks
// ---------------------------------------------------------------------------

static void cbBack(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (mc) mc->switchToMode(MidiMode::HOME);
}

static void cbXCCDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->xy_.xCC = (mc->xy_.xCC - 1 + 128) % 128;
    if (mc->xy_.xCCLbl) {
        char buf[12]; snprintf(buf, sizeof(buf), "X CC:%d", mc->xy_.xCC);
        lv_label_set_text(mc->xy_.xCCLbl, buf);
    }
}
static void cbXCCUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->xy_.xCC = (mc->xy_.xCC + 1) % 128;
    if (mc->xy_.xCCLbl) {
        char buf[12]; snprintf(buf, sizeof(buf), "X CC:%d", mc->xy_.xCC);
        lv_label_set_text(mc->xy_.xCCLbl, buf);
    }
}
static void cbYCCDown(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->xy_.yCC = (mc->xy_.yCC - 1 + 128) % 128;
    if (mc->xy_.yCCLbl) {
        char buf[12]; snprintf(buf, sizeof(buf), "Y CC:%d", mc->xy_.yCC);
        lv_label_set_text(mc->xy_.yCCLbl, buf);
    }
}
static void cbYCCUp(lv_event_t* e) {
    auto* mc = static_cast<MidiController*>(lv_event_get_user_data(e));
    if (!mc) return;
    mc->xy_.yCC = (mc->xy_.yCC + 1) % 128;
    if (mc->xy_.yCCLbl) {
        char buf[12]; snprintf(buf, sizeof(buf), "Y CC:%d", mc->xy_.yCC);
        lv_label_set_text(mc->xy_.yCCLbl, buf);
    }
}

// ---------------------------------------------------------------------------
// Build function
// ---------------------------------------------------------------------------

void buildXYPadView(MidiController* mc, lv_obj_t* parent) {
    auto& xy = mc->xy_;

    const lv_font_t* fontSmall  = lvgl_get_text_font(FONT_SIZE_SMALL);
    const lv_font_t* fontNormal = lvgl_get_text_font(FONT_SIZE_DEFAULT);

    int hdrH  = 88;
    int footH = 64;

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

    // -- Header ----------------------------------------------------------------
    lv_obj_t* hdr = mcRow(root, hdrH);

    int btnH = MC_BTN_H;

    mcBackBtn(hdr, cbBack, mc, MC_W_S);

    lv_obj_t* titleLbl = lv_label_create(hdr);
    lv_label_set_text(titleLbl, "XY PAD");
    lv_obj_set_style_text_font(titleLbl, fontNormal, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    // X CC controls
    lv_obj_t* xDown = mcButton(hdr);
    lv_obj_set_size(xDown, 72, btnH);
    lv_obj_add_event_cb(xDown, cbXCCDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* xdl = lv_label_create(xDown); lv_label_set_text(xdl, "-");
    lv_obj_set_style_text_font(xdl, fontSmall, 0); lv_obj_center(xdl);

    xy.xCCLbl = lv_label_create(hdr);
    char xbuf[12]; snprintf(xbuf, sizeof(xbuf), "X CC:%d", xy.xCC);
    lv_label_set_text(xy.xCCLbl, xbuf);
    lv_obj_set_style_text_font(xy.xCCLbl, fontSmall, 0);
    lv_obj_set_style_text_color(xy.xCCLbl, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_width(xy.xCCLbl, 128);
    lv_obj_set_style_text_align(xy.xCCLbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* xUp = mcButton(hdr);
    lv_obj_set_size(xUp, 72, btnH);
    lv_obj_add_event_cb(xUp, cbXCCUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* xul = lv_label_create(xUp); lv_label_set_text(xul, "+");
    lv_obj_set_style_text_font(xul, fontSmall, 0); lv_obj_center(xul);

    lv_obj_t* divLbl = lv_label_create(hdr);
    lv_label_set_text(divLbl, "|");
    lv_obj_set_style_text_color(divLbl, lv_color_hex(0x334455), 0);

    // Y CC controls
    lv_obj_t* yDown = mcButton(hdr);
    lv_obj_set_size(yDown, 72, btnH);
    lv_obj_add_event_cb(yDown, cbYCCDown, LV_EVENT_CLICKED, mc);
    lv_obj_t* ydl = lv_label_create(yDown); lv_label_set_text(ydl, "-");
    lv_obj_set_style_text_font(ydl, fontSmall, 0); lv_obj_center(ydl);

    xy.yCCLbl = lv_label_create(hdr);
    char ybuf[12]; snprintf(ybuf, sizeof(ybuf), "Y CC:%d", xy.yCC);
    lv_label_set_text(xy.yCCLbl, ybuf);
    lv_obj_set_style_text_font(xy.yCCLbl, fontSmall, 0);
    lv_obj_set_style_text_color(xy.yCCLbl, lv_color_hex(0xFF8000), 0);
    lv_obj_set_width(xy.yCCLbl, 128);
    lv_obj_set_style_text_align(xy.yCCLbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* yUp = mcButton(hdr);
    lv_obj_set_size(yUp, 72, btnH);
    lv_obj_add_event_cb(yUp, cbYCCUp, LV_EVENT_CLICKED, mc);
    lv_obj_t* yul = lv_label_create(yUp); lv_label_set_text(yul, "+");
    lv_obj_set_style_text_font(yul, fontSmall, 0); lv_obj_center(yul);

    // -- Touch/Draw area -------------------------------------------------------
    lv_obj_t* padObj = mcContainer(root);
    lv_obj_set_width(padObj, LV_PCT(100));
    lv_obj_set_flex_grow(padObj, 1);
    lv_obj_set_style_bg_color(padObj, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(padObj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(padObj, 0, 0);
    lv_obj_set_style_pad_all(padObj, 0, 0);
    lv_obj_set_scrollable(padObj, false);
    lv_obj_set_clickable(padObj, true);
    lv_obj_add_event_cb(padObj, cbXYDraw,      LV_EVENT_DRAW_MAIN,    mc);
    lv_obj_add_event_cb(padObj, cbCanvasSize,  LV_EVENT_SIZE_CHANGED, mc);
    lv_obj_add_event_cb(padObj, cbCanvasTouch, LV_EVENT_PRESSING,     mc);
    xy.canvas = padObj;

    // Indicator circle widget (positioned absolutely within canvas)
    int indR = XY_IND_RADIUS;
    lv_obj_t* ind = mcContainer(padObj);
    lv_obj_set_size(ind, indR * 2, indR * 2);
    lv_obj_set_style_radius(ind, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ind, lv_color_hex(0x00DFFF), 0);
    lv_obj_set_style_bg_opa(ind, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ind, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(ind, 4, 0);
    lv_obj_set_style_pad_all(ind, 0, 0);
    lv_obj_set_scrollable(ind, false);
    lv_obj_set_clickable(ind, false);
    lv_obj_set_pos(ind, 0, 0);  // will be updated on first draw/touch
    xy.indicator = ind;

    // -- Footer: value readout ------------------------------------------------
    lv_obj_t* footer = mcContainer(root);
    lv_obj_set_size(footer, LV_PCT(100), footH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0A0C14), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 8, 0);
    lv_obj_set_scrollable(footer, false);

    xy.valLbl = lv_label_create(footer);
    lv_label_set_text(xy.valLbl, "X: 64  Y: 64");
    lv_obj_set_style_text_font(xy.valLbl, fontSmall, 0);
    lv_obj_set_style_text_color(xy.valLbl, lv_color_hex(0x666688), 0);
    lv_obj_center(xy.valLbl);

    updateValLabel(mc);
}
