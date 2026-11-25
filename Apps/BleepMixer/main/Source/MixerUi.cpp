#include "Mixer.h"

#include <app/event.h>

#include <lvgl/devices/keyboard.h>
#include <lvgl/fonts.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/widgets/toolbar.h>
#include <src/misc/lv_event_private.h>
#include <lvgl_window_manager/dialog.h>

#include <cstdio>
#include <cstring>

using namespace bleep;

namespace {

// Tracker colours: the grid always has its own dark look, whatever the theme
const uint32_t GRID_BACKGROUND = 0x10131C;
const uint32_t GRID_BEAT = 0x1A1F2E;
const uint32_t GRID_PLAYING = 0x234A35;
const uint32_t GRID_CURSOR = 0x3A86FF;
const uint32_t GRID_TEXT = 0xCFD6E6;
const uint32_t GRID_DIM = 0x5B6379;
const uint32_t CHANNEL_COLOURS[CHANNEL_COUNT] = { 0xFFC94A, 0x9BE7A0, 0x4CC9F0, 0xFF6B8B };
const char* const CHANNEL_TITLES[CHANNEL_COUNT] = { "LEAD", "HARM", "BASS", "DRUM" };

// Tracker keyboard layout: the lower row plays one octave, the upper row the next
const char* const LOWER_KEYS = "zsxdcvgbhnjm";
const char* const UPPER_KEYS = "q2w3er5t6y7u";

// Keypad (for touch): notes, then octave, hold, rest and drums
char octaveText[8] = "Oct 4";
const char* KEYPAD_MAP[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "\n",
    "Oct-", octaveText, "Oct+", "---", "...", "Kick", "Snare", "Hat", "",
};
enum KeypadButton { KeyOctaveDown = 12, KeyOctaveLabel, KeyOctaveUp, KeyHold, KeyRest, KeyKick, KeySnare, KeyHat };

Context* fromEvent(lv_event_t* event) {
    return static_cast<Context*>(lv_event_get_user_data(event));
}

void focusGrid(Context& ctx) {
    lv_group_t* group = lv_group_get_default();
    if (group != nullptr && ctx.grid != nullptr) {
        lv_group_focus_obj(ctx.grid);
        lv_group_set_editing(group, true);
    }
}

/**
 * After a button was tapped, give the keys back to the pattern. A button chosen with the keys
 * keeps the focus, so the row of buttons can be used without a touch screen.
 */
void returnToGrid(Context& ctx) {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        focusGrid(ctx);
    }
}

// region Layout of the grid

struct GridLayout {
    const lv_font_t* font;
    int lineHeight;
    int numberWidth;
    int columnWidth;
    int visibleRows;
    lv_area_t area;
};

GridLayout gridLayout(const Context& ctx) {
    GridLayout layout {};
    layout.font = lvgl_get_text_font(FONT_SIZE_SMALL);
    layout.lineHeight = lv_font_get_line_height(layout.font) + 4;
    lv_obj_get_coords(ctx.grid, &layout.area);
    const int width = lv_area_get_width(&layout.area);
    const int height = lv_area_get_height(&layout.area);
    layout.numberWidth = lv_font_get_line_height(layout.font) * 3 / 2;
    layout.columnWidth = (width - layout.numberWidth) / CHANNEL_COUNT;
    layout.visibleRows = (height - layout.lineHeight) / layout.lineHeight;
    if (layout.visibleRows < 1) layout.visibleRows = 1;
    if (layout.visibleRows > ROWS_PER_BAR) layout.visibleRows = ROWS_PER_BAR;
    return layout;
}

void scrollToRow(Context& ctx, int row) {
    if (ctx.grid == nullptr) return;
    const GridLayout layout = gridLayout(ctx);
    if (row < ctx.scrollRow) ctx.scrollRow = row;
    if (row >= ctx.scrollRow + layout.visibleRows) ctx.scrollRow = row - layout.visibleRows + 1;
    if (ctx.scrollRow > ROWS_PER_BAR - layout.visibleRows) ctx.scrollRow = ROWS_PER_BAR - layout.visibleRows;
    if (ctx.scrollRow < 0) ctx.scrollRow = 0;
}

// endregion

// region State changes

void updateTitle(Context& ctx) {
    if (ctx.toolbar == nullptr) return;
    char title[64];
    snprintf(title, sizeof(title), "%s%s", ctx.name.empty() ? "Untitled" : ctx.name.c_str(), ctx.dirty ? " *" : "");
    lvgl_toolbar_set_title(ctx.toolbar, title);
}

void updateLabels(Context& ctx) {
    if (ctx.barLabel != nullptr) {
        lv_label_set_text_fmt(ctx.barLabel, "Bar %d/%d", ctx.bar + 1, (int)ctx.song.bars.size());
    }
    if (ctx.bpmLabel != nullptr) {
        lv_label_set_text_fmt(ctx.bpmLabel, "%d BPM", ctx.song.bpm);
    }
    if (ctx.playLabel != nullptr) {
        lv_label_set_text(ctx.playLabel, ctx.player.playing() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    }
    if (ctx.loopButton != nullptr) {
        lv_obj_set_state(ctx.loopButton, LV_STATE_CHECKED, ctx.loopBar);
    }
    if (ctx.keypad != nullptr) {
        snprintf(octaveText, sizeof(octaveText), "Oct %d", ctx.octave);
        lv_buttonmatrix_set_map(ctx.keypad, KEYPAD_MAP);
    }
    updateTitle(ctx);
}

void changed(Context& ctx) {
    if (!ctx.dirty) {
        ctx.dirty = true;
        updateTitle(ctx);
    }
}

void applyLoop(Context& ctx) {
    ctx.player.setLoop(ctx.loopBar ? ctx.bar : -1, ctx.loopBar ? ctx.bar : -1);
}

void setBar(Context& ctx, int bar) {
    const int count = (int)ctx.song.bars.size();
    ctx.bar = (bar % count + count) % count;
    if (ctx.loopBar) applyLoop(ctx);
    updateLabels(ctx);
    if (ctx.grid != nullptr) lv_obj_invalidate(ctx.grid);
}

void moveCursor(Context& ctx, int rows, int channels) {
    ctx.channel = (ctx.channel + channels + CHANNEL_COUNT) % CHANNEL_COUNT;
    int row = ctx.row + rows;
    // Moving past the end of a bar continues in the next one
    if (row >= ROWS_PER_BAR) {
        row -= ROWS_PER_BAR;
        setBar(ctx, ctx.bar + 1);
    } else if (row < 0) {
        row += ROWS_PER_BAR;
        setBar(ctx, ctx.bar - 1);
    }
    ctx.row = row;
    scrollToRow(ctx, ctx.row);
    if (ctx.grid != nullptr) lv_obj_invalidate(ctx.grid);
}

/** Puts a cell at the cursor, plays it and moves down. */
void enterCell(Context& ctx, uint8_t cell) {
    if (!cellValid((Channel)ctx.channel, cell)) return;
    ctx.song.bars[ctx.bar].cells[ctx.channel][ctx.row] = cell;
    ctx.player.audition((Channel)ctx.channel, cell, ctx.song.bpm);
    changed(ctx);
    moveCursor(ctx, 1, 0);
}

void enterNote(Context& ctx, int semitone) {
    if (ctx.channel == Drums) return;
    int note = (ctx.octave + 1) * 12 + semitone;
    if (note < CellFirstNote) note = CellFirstNote;
    if (note > CellLastNote) note = CellLastNote;
    enterCell(ctx, (uint8_t)note);
}

void setOctave(Context& ctx, int octave) {
    if (octave < 0) octave = 0;
    if (octave > 7) octave = 7;
    ctx.octave = octave;
    updateLabels(ctx);
}

void togglePlay(Context& ctx) {
    if (ctx.player.playing()) {
        ctx.player.stop();
    } else {
        applyLoop(ctx);
        ctx.player.play(&ctx.song, ctx.bar);
    }
    ctx.shownPlayPosition = -1;
    updateLabels(ctx);
    if (ctx.grid != nullptr) lv_obj_invalidate(ctx.grid);
}

void editBars(Context& ctx, int action) {
    // Changing the bars must not race the audio task, which reads them
    ctx.player.lock();
    auto& bars = ctx.song.bars;
    if (action == 0) {
        bars.insert(bars.begin() + ctx.bar + 1, Bar());
    } else if (action == 1) {
        const Bar copy = bars[ctx.bar];
        bars.insert(bars.begin() + ctx.bar + 1, copy);
    } else if (bars.size() > 1) {
        bars.erase(bars.begin() + ctx.bar);
    }
    ctx.player.unlock();
    changed(ctx);
    setBar(ctx, action == 2 ? (ctx.bar < (int)bars.size() ? ctx.bar : (int)bars.size() - 1) : ctx.bar + 1);
}

void changeBpm(Context& ctx, int delta) {
    int bpm = ctx.song.bpm + delta;
    if (bpm < MIN_BPM) bpm = MIN_BPM;
    if (bpm > MAX_BPM) bpm = MAX_BPM;
    ctx.player.lock();
    ctx.song.bpm = bpm;
    ctx.player.unlock();
    changed(ctx);
    updateLabels(ctx);
}

// endregion

// region Grid drawing and input

void fillRect(lv_layer_t* layer, int x1, int y1, int x2, int y2, uint32_t colour) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(colour);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    const lv_area_t area = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &dsc, &area);
}

void drawText(lv_layer_t* layer, const lv_font_t* font, const char* text, int x1, int y1, int x2, int y2, uint32_t colour) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = lv_color_hex(colour);
    dsc.text = text;
    dsc.text_local = 1;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    const lv_area_t area = { x1, y1, x2, y2 };
    lv_draw_label(layer, &dsc, &area);
}

void onGridDraw(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    if (ctx.grid == nullptr || ctx.song.bars.empty()) return;
    lv_layer_t* layer = lv_event_get_layer(event);
    const GridLayout l = gridLayout(ctx);
    const int x0 = l.area.x1;
    const int y0 = l.area.y1;
    const int textOffset = 2;
    fillRect(layer, l.area.x1, l.area.y1, l.area.x2, l.area.y2, GRID_BACKGROUND);

    // Header: channel names (tap to mute)
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        const int x = x0 + l.numberWidth + c * l.columnWidth;
        const bool muted = ctx.muted & (1 << c);
        drawText(layer, l.font, muted ? "MUTE" : CHANNEL_TITLES[c], x, y0 + textOffset, x + l.columnWidth - 1, y0 + l.lineHeight,
                 muted ? GRID_DIM : CHANNEL_COLOURS[c]);
    }

    const bool playing = ctx.player.playing();
    const int playBar = ctx.player.bar();
    const int playRow = ctx.player.row();
    const Bar& bar = ctx.song.bars[ctx.bar];
    char text[16];
    for (int i = 0; i < l.visibleRows; i++) {
        const int row = ctx.scrollRow + i;
        if (row >= ROWS_PER_BAR) break;
        const int y = y0 + l.lineHeight * (i + 1);
        const int yEnd = y + l.lineHeight - 1;
        if (playing && playBar == ctx.bar && playRow == row) {
            fillRect(layer, x0, y, l.area.x2, yEnd, GRID_PLAYING);
        } else if (row % 4 == 0) {
            fillRect(layer, x0, y, l.area.x2, yEnd, GRID_BEAT);
        }
        snprintf(text, sizeof(text), "%02d", row + 1);
        drawText(layer, l.font, text, x0, y + textOffset, x0 + l.numberWidth - 1, yEnd, row % 4 == 0 ? GRID_TEXT : GRID_DIM);
        for (int c = 0; c < CHANNEL_COUNT; c++) {
            const int x = x0 + l.numberWidth + c * l.columnWidth;
            if (row == ctx.row && c == ctx.channel) {
                fillRect(layer, x + 1, y, x + l.columnWidth - 2, yEnd, GRID_CURSOR);
            }
            const uint8_t cell = bar.cells[c][row];
            cellText((Channel)c, cell, text);
            const bool empty = cell == CellRest || cell == CellHold;
            const uint32_t colour = (row == ctx.row && c == ctx.channel) ? 0xFFFFFF : empty ? GRID_DIM : CHANNEL_COLOURS[c];
            drawText(layer, l.font, text, x, y + textOffset, x + l.columnWidth - 1, yEnd, colour);
        }
    }
}

void handleGridKey(Context& ctx, uint32_t key) {
    switch (key) {
        case LV_KEY_UP:
            if (ctx.bar == 0 && ctx.row == 0) {
                // Up from the very first step: on to the buttons above
                lv_group_t* group = lv_group_get_default();
                if (group != nullptr) lv_group_focus_prev(group);
                return;
            }
            moveCursor(ctx, -1, 0);
            return;
        case LV_KEY_DOWN: moveCursor(ctx, 1, 0); return;
        case LV_KEY_LEFT: moveCursor(ctx, 0, -1); return;
        case LV_KEY_RIGHT: moveCursor(ctx, 0, 1); return;
        case LV_KEY_ENTER:
            ctx.player.audition((Channel)ctx.channel, ctx.song.bars[ctx.bar].cells[ctx.channel][ctx.row], ctx.song.bpm);
            return;
        case LV_KEY_DEL:
        case LV_KEY_BACKSPACE:
        case '.':
            enterCell(ctx, CellRest);
            return;
        case '-':
        case '`':
            enterCell(ctx, CellHold);
            return;
        case ' ': togglePlay(ctx); return;
        case '[': setOctave(ctx, ctx.octave - 1); return;
        case ']': setOctave(ctx, ctx.octave + 1); return;
        case '<': case ',': setBar(ctx, ctx.bar - 1); return;
        case '>': case '/': setBar(ctx, ctx.bar + 1); return;
        default: break;
    }
    const char ch = (char)(key >= 'A' && key <= 'Z' ? key + 32 : key);
    if (ctx.channel == Drums) {
        if (ch == 'z' || ch == 'k') enterCell(ctx, CellKick);
        else if (ch == 'x' || ch == 's') enterCell(ctx, CellSnare);
        else if (ch == 'c' || ch == 'h') enterCell(ctx, CellHat);
        return;
    }
    if (const char* p = strchr(LOWER_KEYS, ch); p != nullptr && ch != '\0') {
        enterNote(ctx, (int)(p - LOWER_KEYS));
    } else if (const char* q = strchr(UPPER_KEYS, ch); q != nullptr && ch != '\0') {
        enterNote(ctx, 12 + (int)(q - UPPER_KEYS));
    }
}

/**
 * Tactility's keyboard turns the up and down arrows into "focus the previous/next widget", except
 * for text areas and grid navigation containers. So the pattern is a grid navigation container
 * (see mixerCreateWidgets), and this pre-processing handler, which runs before grid navigation's
 * own, takes every key and stops the event there so grid navigation never moves the focus.
 */
void onGridKey(lv_event_t* event) {
    handleGridKey(*fromEvent(event), lv_event_get_key(event));
    // lv_event_stop_processing() isn't exported to apps: set its flag directly
    event->stop_processing = 1;
}

void onGridPressed(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    ctx.dragStartY = point.y;
    ctx.dragStartScroll = ctx.scrollRow;
    ctx.dragged = false;
}

void onGridPressing(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const GridLayout l = gridLayout(ctx);
    const int rows = (ctx.dragStartY - point.y) / l.lineHeight;
    if (rows != 0) ctx.dragged = true;
    if (ctx.dragged) {
        int scroll = ctx.dragStartScroll + rows;
        if (scroll > ROWS_PER_BAR - l.visibleRows) scroll = ROWS_PER_BAR - l.visibleRows;
        if (scroll < 0) scroll = 0;
        if (scroll != ctx.scrollRow) {
            ctx.scrollRow = scroll;
            lv_obj_invalidate(ctx.grid);
        }
    }
}

void onGridClicked(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    focusGrid(ctx);
    if (ctx.dragged) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const GridLayout l = gridLayout(ctx);
    const int column = (point.x - l.area.x1 - l.numberWidth) / (l.columnWidth > 0 ? l.columnWidth : 1);
    if (point.x - l.area.x1 < l.numberWidth || column < 0 || column >= CHANNEL_COUNT) return;
    const int line = (point.y - l.area.y1) / l.lineHeight;
    if (line == 0) {
        // Header: mute or unmute the channel
        ctx.muted ^= (uint8_t)(1 << column);
        ctx.player.setMuted(ctx.muted);
    } else {
        const int row = ctx.scrollRow + line - 1;
        if (row >= ROWS_PER_BAR) return;
        if (row == ctx.row && column == ctx.channel) {
            ctx.player.audition((Channel)column, ctx.song.bars[ctx.bar].cells[column][row], ctx.song.bpm);
        }
        ctx.row = row;
        ctx.channel = column;
    }
    lv_obj_invalidate(ctx.grid);
}

// endregion

// region Buttons

void onKeypad(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    const uint32_t button = lv_buttonmatrix_get_selected_button(ctx.keypad);
    if (button < 12) {
        enterNote(ctx, (int)button);
    } else {
        switch (button) {
            case KeyOctaveDown: setOctave(ctx, ctx.octave - 1); break;
            case KeyOctaveUp: setOctave(ctx, ctx.octave + 1); break;
            case KeyHold: enterCell(ctx, CellHold); break;
            case KeyRest: enterCell(ctx, CellRest); break;
            case KeyKick: if (ctx.channel == Drums) enterCell(ctx, CellKick); break;
            case KeySnare: if (ctx.channel == Drums) enterCell(ctx, CellSnare); break;
            case KeyHat: if (ctx.channel == Drums) enterCell(ctx, CellHat); break;
            default: break;
        }
    }
    returnToGrid(ctx);
}

void onPlay(lv_event_t* event) { Context& ctx = *fromEvent(event); togglePlay(ctx); returnToGrid(ctx); }
void onPreviousBar(lv_event_t* event) { Context& ctx = *fromEvent(event); setBar(ctx, ctx.bar - 1); returnToGrid(ctx); }
void onNextBar(lv_event_t* event) { Context& ctx = *fromEvent(event); setBar(ctx, ctx.bar + 1); returnToGrid(ctx); }
void onInsertBar(lv_event_t* event) { Context& ctx = *fromEvent(event); editBars(ctx, 0); returnToGrid(ctx); }
void onDuplicateBar(lv_event_t* event) { Context& ctx = *fromEvent(event); editBars(ctx, 1); returnToGrid(ctx); }
void onDeleteBar(lv_event_t* event) { Context& ctx = *fromEvent(event); editBars(ctx, 2); returnToGrid(ctx); }
void onSlower(lv_event_t* event) { Context& ctx = *fromEvent(event); changeBpm(ctx, -2); returnToGrid(ctx); }
void onFaster(lv_event_t* event) { Context& ctx = *fromEvent(event); changeBpm(ctx, 2); returnToGrid(ctx); }

void onLoop(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    ctx.loopBar = !ctx.loopBar;
    applyLoop(ctx);
    updateLabels(ctx);
    returnToGrid(ctx);
}

void onKeypadToggle(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    ctx.keypadVisible = !ctx.keypadVisible;
    if (ctx.keypad != nullptr) lv_obj_set_hidden(ctx.keypad, !ctx.keypadVisible);
    returnToGrid(ctx);
}

void onSave(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    ctx.request.store(Request::Save);
}

void onSaveAs(lv_event_t* event) {
    askSaveAs(*fromEvent(event));
}

void onOpen(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    ctx.request.store(Request::ListForOpen);
}

void onCloseConfirmed(int32_t button, void* userData) {
    auto& ctx = *static_cast<Context*>(userData);
    if (button == 0) app_event_emit_close(ctx.appInstanceId);
}

void onClose(lv_event_t* event) {
    Context& ctx = *fromEvent(event);
    if (!ctx.dirty) {
        app_event_emit_close(ctx.appInstanceId);
        return;
    }
    static const char* const buttons[] = { "Close", "Cancel" };
    window_manager_alert_dialog(ctx.appInstanceId, "Unsaved changes", "Close without saving the song?", buttons, 2,
                                onCloseConfirmed, &ctx);
}

void onTimer(lv_timer_t* timer) {
    Context& ctx = *static_cast<Context*>(lv_timer_get_user_data(timer));
    if (ctx.grid == nullptr) return;
    const bool playing = ctx.player.playing();
    const int position = playing ? ctx.player.bar() << 8 | ctx.player.row() : -1;
    if (position == ctx.shownPlayPosition) return;
    ctx.shownPlayPosition = position;
    // Playing the whole song: follow it from bar to bar
    if (playing && !ctx.loopBar && ctx.player.bar() >= 0 && ctx.player.bar() != ctx.bar) {
        ctx.bar = ctx.player.bar();
        updateLabels(ctx);
    }
    if (playing && ctx.player.bar() == ctx.bar && ctx.player.row() >= 0) {
        scrollToRow(ctx, ctx.player.row());
    }
    lv_obj_invalidate(ctx.grid);
}

lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, Context* ctx, lv_obj_t** label = nullptr) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_style_pad_hor(button, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(button, 4, LV_STATE_DEFAULT);
    lv_obj_t* text_label = lv_label_create(button);
    lv_label_set_text(text_label, text);
    lv_obj_center(text_label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ctx);
    if (label != nullptr) *label = text_label;
    return button;
}

lv_obj_t* makeLabel(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    // Grid navigation would stop on it (invisibly) between the buttons
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_pad_hor(label, 4, LV_STATE_DEFAULT);
    return label;
}

// endregion

} // namespace

void mixerSongChanged(Context& ctx) {
    if (ctx.bar >= (int)ctx.song.bars.size()) ctx.bar = 0;
    updateLabels(ctx);
    if (ctx.grid != nullptr) lv_obj_invalidate(ctx.grid);
}

void mixerCreateWidgets(lv_obj_t* root, void* userData) {
    Context& ctx = *static_cast<Context*>(userData);
    static bool firstShow = true;
    if (firstShow) {
        // A keyboard plays notes; without one, the on-screen keypad is the way to enter them
        ctx.keypadVisible = !lvgl_hardware_keyboard_is_available();
        firstShow = false;
    }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_pad_row(root, 0, LV_STATE_DEFAULT);

    ctx.toolbar = lvgl_toolbar_create(root, "BleepMixer");
    lvgl_toolbar_set_nav_action(ctx.toolbar, LV_SYMBOL_CLOSE, onClose, &ctx);
    lvgl_toolbar_add_text_button_action(ctx.toolbar, LV_SYMBOL_DIRECTORY, onOpen, &ctx);
    lvgl_toolbar_add_text_button_action(ctx.toolbar, LV_SYMBOL_SAVE, onSave, &ctx);
    lvgl_toolbar_add_text_button_action(ctx.toolbar, LV_SYMBOL_EDIT, onSaveAs, &ctx);
    lvgl_toolbar_add_text_button_action(ctx.toolbar, LV_SYMBOL_KEYBOARD, onKeypadToggle, &ctx);
    lv_obj_t* play = lvgl_toolbar_add_text_button_action(ctx.toolbar, LV_SYMBOL_PLAY, onPlay, &ctx);
    ctx.playLabel = lv_obj_get_child(play, 0);

    // Bar, tempo and loop controls
    lv_obj_t* controls = lv_obj_create(root);
    lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(controls, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(controls, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(controls, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(controls, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(controls, false);
    makeButton(controls, LV_SYMBOL_LEFT, onPreviousBar, &ctx);
    ctx.barLabel = makeLabel(controls);
    makeButton(controls, LV_SYMBOL_RIGHT, onNextBar, &ctx);
    makeButton(controls, "+Bar", onInsertBar, &ctx);
    makeButton(controls, "Copy", onDuplicateBar, &ctx);
    makeButton(controls, "Del", onDeleteBar, &ctx);
    ctx.loopButton = makeButton(controls, LV_SYMBOL_LOOP " Bar", onLoop, &ctx);
    lv_obj_add_flag(ctx.loopButton, LV_OBJ_FLAG_CHECKABLE);
    makeButton(controls, LV_SYMBOL_MINUS, onSlower, &ctx);
    ctx.bpmLabel = makeLabel(controls);
    makeButton(controls, LV_SYMBOL_PLUS, onFaster, &ctx);
    // Arrow keys move between the buttons; past the last row they go on to the pattern
    lvgl_grid_navigation_add(controls);
    lv_obj_add_event_cb(controls, [](lv_event_t*) {
        lv_group_t* focusGroup = lv_group_get_default();
        if (focusGroup != nullptr && lv_group_get_editing(focusGroup)) lv_group_set_editing(focusGroup, false);
    }, LV_EVENT_FOCUSED, nullptr);

    // The pattern of the bar
    ctx.grid = lv_obj_create(root);
    lv_obj_set_width(ctx.grid, LV_PCT(100));
    lv_obj_set_flex_grow(ctx.grid, 1);
    lv_obj_set_style_border_width(ctx.grid, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ctx.grid, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ctx.grid, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(ctx.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ctx.grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(ctx.grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ctx.grid, onGridDraw, LV_EVENT_DRAW_MAIN, &ctx);
    // Before grid navigation's handlers (added below), so it sees the keys first
    lv_obj_add_event_cb(ctx.grid, onGridKey, static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS), &ctx);
    lv_obj_add_event_cb(ctx.grid, onGridPressed, LV_EVENT_PRESSED, &ctx);
    lv_obj_add_event_cb(ctx.grid, onGridPressing, LV_EVENT_PRESSING, &ctx);
    lv_obj_add_event_cb(ctx.grid, onGridClicked, LV_EVENT_CLICKED, &ctx);

    // Note entry for touch screens
    ctx.keypad = lv_buttonmatrix_create(root);
    lv_buttonmatrix_set_map(ctx.keypad, KEYPAD_MAP);
    lv_obj_set_width(ctx.keypad, LV_PCT(100));
    lv_obj_set_height(ctx.keypad, lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 4 + 16);
    lv_obj_set_style_pad_all(ctx.keypad, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(ctx.keypad, 2, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ctx.keypad, onKeypad, LV_EVENT_VALUE_CHANGED, &ctx);
    lv_obj_set_hidden(ctx.keypad, !ctx.keypadVisible);

    lv_group_t* group = lv_group_get_default();
    if (group != nullptr) {
        lv_group_remove_obj(ctx.keypad); // it would take the keys from the grid
        lv_group_add_obj(group, ctx.grid); // after the buttons: Shift+Tab (or up from the first step) goes to them
        // Focused by the keys (e.g. coming back from the buttons): it takes the arrow keys again
        lv_obj_add_event_cb(ctx.grid, [](lv_event_t*) {
            lv_group_t* focusGroup = lv_group_get_default();
            if (focusGroup != nullptr) lv_group_set_editing(focusGroup, true);
        }, LV_EVENT_FOCUSED, nullptr);
        // Leaving it: the group's edit mode would keep sending the arrow keys to the focused button
        // instead of letting the button row move between its buttons
        lv_obj_add_event_cb(ctx.grid, [](lv_event_t*) {
            lv_group_t* focusGroup = lv_group_get_default();
            if (focusGroup != nullptr) lv_group_set_editing(focusGroup, false);
        }, LV_EVENT_DEFOCUSED, nullptr);
    }
    lvgl_grid_navigation_add(ctx.grid); // only so the arrows arrive as arrows: see onGridKey
    focusGrid(ctx);

    ctx.timer = lv_timer_create(onTimer, 40, &ctx);
    ctx.shownPlayPosition = -1;
    updateLabels(ctx);
}

void mixerDestroyWidgets(void* userData) {
    Context& ctx = *static_cast<Context*>(userData);
    if (ctx.timer != nullptr) {
        lv_timer_delete(ctx.timer);
        ctx.timer = nullptr;
    }
    lv_group_t* group = lv_group_get_default();
    if (group != nullptr) lv_group_set_editing(group, false);
    ctx.toolbar = nullptr;
    ctx.playLabel = nullptr;
    ctx.barLabel = nullptr;
    ctx.bpmLabel = nullptr;
    ctx.loopButton = nullptr;
    ctx.grid = nullptr;
    ctx.keypad = nullptr;
}
