#include "Gpio.h"

#include <lvgl/fonts.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/lvgl.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/icon_button.h>
#include <lvgl/widgets/tabs.h>

#include <app/event.h>
#include <app/paths.h>
#include <lvgl_window_manager/dialog.h>

#include <tactility/device.h>
#include <tactility/drivers/gpio.h>
#include <tactility/log.h>
#include <tactility/preferences.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr auto* TAG = "GPIO";

constexpr uint32_t TAB_GRID = 0;
constexpr uint32_t TAB_TIMELINE = 1;

constexpr uint32_t INTERVALS_MS[] = { 20, 50, 100, 250, 500, 1000 };
constexpr uint32_t INTERVAL_COUNT = sizeof(INTERVALS_MS) / sizeof(INTERVALS_MS[0]);
/** Redrawing the timeline more often than this only costs time */
constexpr uint32_t MIN_DRAW_PERIOD_MS = 100;
/** Must match manifest.properties' app.id */
constexpr const char* APP_ID = "tactility.gpio";

enum Action : int32_t { ACTION_NONE, ACTION_ADD, ACTION_INTERVAL, ACTION_REMOVE };
/** How long a tile is marked after its pin changed */
constexpr uint32_t CHANGE_MARK_MS = 2000;

/** Black or white, whichever reads better on the colour */
static lv_color_t textColorOn(lv_color_t background) {
    const int brightness = (background.red * 3 + background.green * 6 + background.blue) / 10;
    return brightness > 128 ? lv_color_black() : lv_color_white();
}

/** High is filled, low is dimmed, a recent change has a coloured border and the selection an outline */
static void styleTile(Context* ctx, uint32_t index) {
    const Pin& pin = ctx->pins[index];
    lv_obj_t* tile = pin.tile;
    if (tile == nullptr) return;

    const bool mono = lvgl_theme_is_mono();
    lv_obj_t* parent = lv_obj_get_parent(tile);
    const lv_color_t text = lv_obj_get_style_text_color(parent, LV_PART_MAIN);
    const lv_color_t highColor = mono ? text : lv_theme_get_color_secondary(tile);
    const lv_color_t markColor = mono ? text : lv_palette_main(LV_PALETTE_LIGHT_BLUE);

    lv_obj_set_style_bg_color(tile, pin.high ? highColor : text, 0);
    lv_obj_set_style_bg_opa(tile, pin.high ? LV_OPA_COVER : (mono ? LV_OPA_TRANSP : LV_OPA_10), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(tile, 0),
        pin.high ? textColorOn(highColor) : text, 0);
    lv_obj_set_style_border_color(tile, markColor, 0);
    lv_obj_set_style_border_width(tile, pin.marked ? 2 : (mono ? 1 : 0), 0);
    lv_obj_set_style_outline_color(tile, text, 0);
    lv_obj_set_style_outline_width(tile, index == ctx->selected ? 2 : 0, 0);
    // A bar at the bottom marks a pin that is on the timeline
    lv_obj_t* marker = lv_obj_get_child(tile, 1);
    lv_obj_set_hidden(marker, !pin.pinned);
    lv_obj_set_style_bg_color(marker, pin.high ? textColorOn(highColor) : text, 0);
}

static void renderLanes(Context* ctx);
static void drawTimeline(Context* ctx);

static void updateLabels(Context* ctx) {
    if (ctx->detailName == nullptr) return;

    const Pin& pin = ctx->pins[ctx->selected];
    lv_label_set_text_fmt(ctx->detailName, "GPIO %u: %s", static_cast<unsigned>(ctx->selected), pin.high ? "HIGH" : "LOW");
    if (pin.changes == 0) {
        lv_label_set_text(ctx->detailChanges, "no change");
    } else {
        lv_label_set_text_fmt(ctx->detailChanges, "%u changes, %u s ago", static_cast<unsigned>(pin.changes),
            static_cast<unsigned>(lv_tick_elaps(pin.changedAt) / 1000));
    }
}

/** Reads all pins, and updates the tiles that look different. Runs on the LVGL task. */
static void samplePins(Context* ctx) {
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        Pin& pin = ctx->pins[i];
        bool high = false;
        if (gpio_controller_get_level(ctx->gpioController, static_cast<gpio_pin_t>(i), &high) != ERROR_NONE) {
            high = false;
        }

        bool redraw = false;
        if (high != pin.high) {
            pin.high = high;
            redraw = true;
            if (ctx->sampled) {
                pin.changes++;
                pin.changedAt = lv_tick_get();
            }
        }
        const bool marked = pin.changes > 0 && lv_tick_elaps(pin.changedAt) < CHANGE_MARK_MS;
        if (marked != pin.marked) {
            pin.marked = marked;
            redraw = true;
        }
        if (redraw) styleTile(ctx, i);
    }
    ctx->sampled = true;
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        ctx->pins[i].history[ctx->historyEnd] = ctx->pins[i].high ? 1 : 0;
    }
    ctx->historyEnd = (ctx->historyEnd + 1) % HISTORY_LENGTH;
    if (ctx->historyCount < HISTORY_LENGTH) ctx->historyCount++;

    updateLabels(ctx);
    if (lv_tick_elaps(ctx->drawnAt) >= MIN_DRAW_PERIOD_MS) drawTimeline(ctx);
}

/* ── Timeline ─────────────────────────────────────────────────────── */

static void formatDuration(uint32_t ms, char* buffer, size_t size) {
    if (ms < 1000) {
        snprintf(buffer, size, "%u ms", static_cast<unsigned>(ms));
    } else if (ms % 1000 == 0) {
        snprintf(buffer, size, "%u s", static_cast<unsigned>(ms / 1000));
    } else {
        snprintf(buffer, size, "%u.%u s", static_cast<unsigned>(ms / 1000), static_cast<unsigned>(ms % 1000 / 100));
    }
}

/** Draws the lanes, with now at the right edge */
static void drawTimeline(Context* ctx) {
    if (ctx->laneList == nullptr || ctx->tab != TAB_TIMELINE) return;
    ctx->drawnAt = lv_tick_get();

    for (uint32_t i = 0; i < ctx->laneCount; ++i) {
        const Lane& lane = ctx->lanes[i];
        const Pin& pin = ctx->pins[lane.pin];
        lv_obj_set_style_bg_opa(lane.led, pin.high ? LV_OPA_COVER : LV_OPA_20, 0);

        lv_obj_t* box = lv_obj_get_parent(lane.line);
        const int32_t width = lv_obj_get_content_width(box);
        const int32_t height = lv_obj_get_content_height(box);
        if (width <= 1 || height <= 2) continue;

        uint32_t count = 0;
        const uint32_t first = HISTORY_LENGTH - ctx->historyCount;
        for (uint32_t k = 0; k < ctx->historyCount; ++k) {
            const uint32_t index = (ctx->historyEnd + HISTORY_LENGTH - ctx->historyCount + k) % HISTORY_LENGTH;
            const auto x = static_cast<lv_value_precise_t>((first + k) * (width - 1) / (HISTORY_LENGTH - 1));
            const auto y = static_cast<lv_value_precise_t>(pin.history[index] ? 1 : height - 2);
            // A vertical step at this x, from the previous level
            if (count > 0 && lane.points[count - 1].y != y) {
                lane.points[count] = { x, lane.points[count - 1].y };
                count++;
            }
            if (count < TRACE_POINTS) lane.points[count++] = { x, y };
        }
        lv_line_set_points(lane.line, lane.points, count);
    }
}

static void updateTimelineLabels(Context* ctx) {
    if (ctx->intervalLabel == nullptr) return;
    char duration[16];
    formatDuration(ctx->intervalMs, duration, sizeof(duration));
    lv_label_set_text_fmt(ctx->intervalLabel, "Every %s", duration);
    formatDuration(ctx->intervalMs * HISTORY_LENGTH, duration, sizeof(duration));
    lv_label_set_text_fmt(ctx->spanLabel, "Shows %s", duration);
}

static void freeLanes(Context* ctx) {
    for (uint32_t i = 0; i < ctx->laneCount; ++i) {
        delete[] ctx->lanes[i].points;
    }
    ctx->laneCount = 0;
}

static void requestAction(Context* ctx, Action action, int32_t value) {
    ctx->actionValue = value;
    ctx->action = action;
    task_event_group_signal(ctx->eventGroup, ctx->actionBit);
}

static void onRemovePressed(lv_event_t* e) {
    // The lane can't be deleted from its own button's event
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    auto* button = lv_event_get_current_target_obj(e);
    requestAction(ctx, ACTION_REMOVE, static_cast<int32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button))));
}

/** Creates a lane for every pinned pin */
static void renderLanes(Context* ctx) {
    if (ctx->laneList == nullptr) return;
    freeLanes(ctx);
    lv_obj_clean(ctx->laneList);

    if (ctx->pinnedCount == 0) {
        auto* label = lv_label_create(ctx->laneList);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(label, "Add a GPIO, or long press one on the grid");
        return;
    }

    const int32_t lane_height = lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 2;
    const lv_color_t color = lvgl_theme_is_mono()
        ? lv_obj_get_style_text_color(lv_obj_get_parent(ctx->laneList), LV_PART_MAIN)
        : lv_theme_get_color_secondary(ctx->laneList);

    for (uint32_t i = 0; i < ctx->pinnedCount; ++i) {
        const uint32_t pin = ctx->pinnedPins[i];
        auto* row = lv_obj_create(ctx->laneList);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), lane_height);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_scrollable(row, false);

        auto* remove = lv_button_create(row);
        lv_obj_set_user_data(remove, reinterpret_cast<void*>(static_cast<uintptr_t>(pin)));
        lv_obj_add_event_cb(remove, onRemovePressed, LV_EVENT_SHORT_CLICKED, ctx);
        auto* label = lv_label_create(remove);
        lv_label_set_text_fmt(label, "%u " LV_SYMBOL_CLOSE, static_cast<unsigned>(pin));

        auto* box = lv_obj_create(row);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 0, lane_height - 6);
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_scrollable(box, false);
        auto* line = lv_line_create(box);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_color(line, color, 0);

        auto* led = lv_obj_create(row);
        lv_obj_remove_style_all(led);
        lv_obj_set_size(led, lane_height / 3, lane_height / 3);
        lv_obj_set_style_radius(led, 3, 0);
        lv_obj_set_style_bg_color(led, color, 0);

        ctx->lanes[ctx->laneCount++] = { pin, line, led, new lv_point_precise_t[TRACE_POINTS] };
    }

    lv_obj_update_layout(ctx->laneList);
    drawTimeline(ctx);
}

static void setPinned(Context* ctx, uint32_t pin, bool pinned) {
    if (pin >= ctx->pinCount || ctx->pins[pin].pinned == pinned) return;
    ctx->pins[pin].pinned = pinned;
    if (pinned) {
        ctx->pinnedPins[ctx->pinnedCount++] = pin;
    } else {
        uint32_t count = 0;
        for (uint32_t i = 0; i < ctx->pinnedCount; ++i) {
            if (ctx->pinnedPins[i] != pin) ctx->pinnedPins[count++] = ctx->pinnedPins[i];
        }
        ctx->pinnedCount = count;
    }
    styleTile(ctx, pin);
    renderLanes(ctx);
}

static void clearHistory(Context* ctx) {
    ctx->historyEnd = 0;
    ctx->historyCount = 0;
}

/* ── Dialogs ──────────────────────────────────────────────────────── */

static void onAddChoice(int32_t item, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (item >= 0) requestAction(ctx, ACTION_ADD, item);
}

static void onIntervalChoice(int32_t item, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (item >= 0) requestAction(ctx, ACTION_INTERVAL, item);
}

/** The names of the pins that aren't on the timeline, one after the other. @return how many */
static uint32_t listUnpinned(Context* ctx, char (*names)[16], const char** items, uint32_t* pins) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        if (ctx->pins[i].pinned) continue;
        if (names != nullptr) {
            snprintf(names[count], 16, "GPIO %u", static_cast<unsigned>(i));
            items[count] = names[count];
        }
        if (pins != nullptr) pins[count] = i;
        count++;
    }
    return count;
}

static void onAddPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    auto* names = new char[ctx->pinCount][16];
    auto** items = new const char*[ctx->pinCount];
    const uint32_t count = listUnpinned(ctx, names, items, nullptr);
    if (count == 1) {
        // A dialog with one item would pick it without asking, and this is what happens anyway
        uint32_t pin = 0;
        listUnpinned(ctx, nullptr, nullptr, &pin);
        setPinned(ctx, pin, true);
    } else if (count > 1) {
        window_manager_selection_dialog(ctx->appInstanceId, "Add GPIO", items, count, onAddChoice, ctx);
    }
    delete[] items;
    delete[] names;
}

static void onIntervalPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    static const char* const items[INTERVAL_COUNT] = { "20 ms", "50 ms", "100 ms", "250 ms", "500 ms", "1 s" };
    window_manager_selection_dialog(ctx->appInstanceId, "Update interval", items, INTERVAL_COUNT, onIntervalChoice, ctx);
}

static void onTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    if (!ctx->frozen) samplePins(ctx);
}

static void toggleFreeze(Context* ctx) {
    if (ctx->pinCount == 0) return;
    ctx->frozen = !ctx->frozen;
    // What changed while frozen wasn't seen, so it isn't counted
    if (!ctx->frozen) ctx->sampled = false;
    lv_label_set_text(ctx->freezeLabel, ctx->frozen ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    updateLabels(ctx);
}

static void onFreezePressed(lv_event_t* e) {
    toggleFreeze(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onResetPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx->pinCount == 0) return;
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        ctx->pins[i].changes = 0;
        ctx->pins[i].marked = false;
        styleTile(ctx, i);
    }
    clearHistory(ctx);
    updateLabels(ctx);
    drawTimeline(ctx);
}

static void onGridKey(lv_event_t* e) {
    if (lv_event_get_key(e) == ' ') {
        toggleFreeze(static_cast<Context*>(lv_event_get_user_data(e)));
    }
}

static void selectTile(Context* ctx, lv_obj_t* tile) {
    const uint32_t previous = ctx->selected;
    ctx->selected = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(tile)));
    styleTile(ctx, previous);
    styleTile(ctx, ctx->selected);
    updateLabels(ctx);
}

static void onTileLongPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    auto* tile = lv_event_get_current_target_obj(e);
    const auto pin = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(tile)));
    selectTile(ctx, tile);
    setPinned(ctx, pin, !ctx->pins[pin].pinned);
}

static void onTilePressed(lv_event_t* e) {
    selectTile(static_cast<Context*>(lv_event_get_user_data(e)), lv_event_get_current_target_obj(e));
}

/** The arrow keys select the tile they move to */
static void onTileFocused(lv_event_t* e) {
    // The first focus, when the widgets are created, has no input device and keeps the selection
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        onTilePressed(e);
    }
}

static void showTab(Context* ctx) {
    lv_obj_set_hidden(ctx->gridPage, ctx->tab != TAB_GRID);
    lv_obj_set_hidden(ctx->timelinePage, ctx->tab != TAB_TIMELINE);
    if (ctx->tab == TAB_TIMELINE && ctx->laneList != nullptr) {
        // Hidden widgets aren't laid out, so the sizes are only known now
        lv_obj_update_layout(ctx->timelinePage);
        drawTimeline(ctx);
    }
}

static void onTabChanged(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->tab = lvgl_tabs_get_selected(ctx->tabs);
    showTab(ctx);
}

static void onClosePressed(lv_event_t* e) {
    app_event_emit_close(static_cast<Context*>(lv_event_get_user_data(e))->appInstanceId);
}

/** A row of buttons that the arrow keys move along */
static lv_obj_t* createButtonGroup(lv_obj_t* parent) {
    auto* group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    return group;
}

static lv_obj_t* createHeaderButton(lv_obj_t* parent, const char* symbol, lv_event_cb_t callback, Context* ctx) {
    auto* button = lvgl_icon_button_create(parent);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, ctx);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    return button;
}

/** One compact row instead of a toolbar: close, the tabs, and the actions */
static void createHeader(lv_obj_t* parent, Context* ctx) {
    auto* header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const auto spacing = lvgl_theme_lerp(LV_DPX(2), LV_DPX(8), lvgl_theme_get_spacing());
    lv_obj_set_style_pad_left(header, spacing, 0);
    lv_obj_set_style_pad_right(header, spacing, 0);
    lv_obj_set_style_pad_column(header, spacing, 0);

    auto* close = createButtonGroup(header);
    createHeaderButton(close, LV_SYMBOL_CLOSE, onClosePressed, ctx);
    lvgl_grid_navigation_add(close);

    ctx->tabs = lvgl_tabs_create(header);
    lvgl_tabs_set_mode(ctx->tabs, LVGL_TABS_MODE_WRAP);
    lvgl_tabs_add_tab(ctx->tabs, "Grid");
    lvgl_tabs_add_tab(ctx->tabs, "Timeline");
    lvgl_tabs_set_selected(ctx->tabs, ctx->tab);
    lv_obj_add_event_cb(ctx->tabs, onTabChanged, LV_EVENT_VALUE_CHANGED, ctx);

    auto* spacer = lv_obj_create(header);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);

    auto* actions = createButtonGroup(header);
    auto* freeze = createHeaderButton(actions, ctx->frozen ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, onFreezePressed, ctx);
    ctx->freezeLabel = lv_obj_get_child(freeze, 0);
    createHeaderButton(actions, LV_SYMBOL_REFRESH, onResetPressed, ctx);
    lvgl_grid_navigation_add(actions);
}

static lv_obj_t* createPage(lv_obj_t* parent) {
    auto* page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 2, 0);
    return page;
}

static void createTimeline(Context* ctx) {
    auto* tools = lv_obj_create(ctx->timelinePage);
    lv_obj_remove_style_all(tools);
    lv_obj_set_size(tools, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tools, 6, 0);
    lv_obj_set_style_pad_right(tools, 6, 0);
    lv_obj_set_style_pad_column(tools, 6, 0);

    auto* add = lv_button_create(tools);
    lv_obj_add_event_cb(add, onAddPressed, LV_EVENT_SHORT_CLICKED, ctx);
    lv_label_set_text(lv_label_create(add), LV_SYMBOL_PLUS " GPIO");

    auto* interval = lv_button_create(tools);
    lv_obj_add_event_cb(interval, onIntervalPressed, LV_EVENT_SHORT_CLICKED, ctx);
    ctx->intervalLabel = lv_label_create(interval);

    ctx->spanLabel = lv_label_create(tools);
    lv_obj_set_style_text_font(ctx->spanLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->spanLabel, LV_OPA_70, 0);

    // The lanes scroll when there are more than fit
    ctx->laneList = lv_obj_create(ctx->timelinePage);
    lv_obj_remove_style_all(ctx->laneList);
    lv_obj_set_width(ctx->laneList, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->laneList, 1);
    lv_obj_set_flex_flow(ctx->laneList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(ctx->laneList, 6, 0);
    lv_obj_set_style_pad_right(ctx->laneList, 6, 0);
    lv_obj_set_style_pad_row(ctx->laneList, 2, 0);

    updateTimelineLabels(ctx);
    renderLanes(ctx);
}

static lv_obj_t* createContainer(lv_obj_t* parent, lv_flex_flow_t flow) {
    auto* container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, flow);
    lv_obj_set_style_pad_hor(container, 6, 0);
    return container;
}

void gpioCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 2, 0);

    createHeader(parent, ctx);
    ctx->gridPage = createPage(parent);
    ctx->timelinePage = createPage(parent);

    if (ctx->pinCount == 0) {
        auto* label = lv_label_create(ctx->gridPage);
        lv_obj_set_style_pad_all(label, 8, 0);
        lv_label_set_text(label, "No GPIO controller found");
        showTab(ctx);
        return;
    }

    const int32_t font_height = lvgl_get_text_font_height(FONT_SIZE_DEFAULT);
    const int32_t gap = 3;

    // The grid scrolls when the pins don't fit
    auto* grid = createContainer(ctx->gridPage, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_pad_ver(grid, 3, 0);
    lv_obj_set_style_pad_row(grid, gap + 1, 0);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_update_layout(parent);

    // As many columns as fit tiles that are wide enough for a two digit number and a finger
    const int32_t width = lv_obj_get_content_width(grid);
    const int32_t columns = LV_MAX((width + gap) / (font_height * 2 + gap), 1);
    const int32_t tile_width = (width - (columns - 1) * gap) / columns;

    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        auto* tile = lv_obj_create(grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tile_width, font_height + 10);
        lv_obj_set_style_radius(tile, 4, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_color(tile, lv_theme_get_color_primary(tile), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(tile, 3, LV_STATE_FOCUS_KEY);
        lv_obj_set_scrollable(tile, false);
        lv_obj_set_clickable(tile, true);
        lv_obj_set_click_focusable(tile, true);
        lv_obj_set_user_data(tile, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(tile, onTilePressed, LV_EVENT_SHORT_CLICKED, ctx);
        lv_obj_add_event_cb(tile, onTileFocused, LV_EVENT_FOCUSED, ctx);

        auto* label = lv_label_create(tile);
        lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
        lv_obj_center(label);

        auto* marker = lv_obj_create(tile);
        lv_obj_remove_style_all(marker);
        lv_obj_set_size(marker, tile_width / 2, 3);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
        lv_obj_set_clickable(marker, false);
        lv_obj_align(marker, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_add_event_cb(tile, onTileLongPressed, LV_EVENT_LONG_PRESSED, ctx);

        ctx->pins[i].tile = tile;
        styleTile(ctx, i);
    }
    // Arrow keys move between the tiles, Space freezes
    lvgl_grid_navigation_add(grid);
    lv_obj_add_event_cb(grid, onGridKey, LV_EVENT_KEY, ctx);
    if (lv_obj_get_group(grid) != nullptr) {
        lv_group_focus_obj(grid);
    }

    auto* detail = createContainer(ctx->gridPage, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_bottom(detail, 2, 0);
    lv_obj_set_flex_align(detail, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ctx->detailName = lv_label_create(detail);
    ctx->detailChanges = lv_label_create(detail);

    if (ctx->frozen) {
        updateLabels(ctx);
    } else {
        samplePins(ctx);
    }
    createTimeline(ctx);
    showTab(ctx);
    ctx->timer = lv_timer_create(onTimer, ctx->intervalMs, ctx);
}

void gpioDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (ctx->timer != nullptr) {
        lv_timer_delete(ctx->timer);
        ctx->timer = nullptr;
    }
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        ctx->pins[i].tile = nullptr;
    }
    freeLanes(ctx);
    ctx->intervalLabel = nullptr;
    ctx->spanLabel = nullptr;
    ctx->laneList = nullptr;
    ctx->freezeLabel = nullptr;
    ctx->tabs = nullptr;
    ctx->gridPage = nullptr;
    ctx->timelinePage = nullptr;
    ctx->detailName = nullptr;
    ctx->detailChanges = nullptr;
}

static bool getSettingsPath(char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, "gpio.properties", path, size) == ERROR_NONE;
}

static void loadSettings(Context* ctx) {
    char path[160];
    if (!getSettingsPath(path, sizeof(path))) return;
    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) return;

    int32_t interval = 0;
    if (preferences_opt_int32(preferences, "interval", &interval)) {
        for (const uint32_t option : INTERVALS_MS) {
            if (option == static_cast<uint32_t>(interval)) ctx->intervalMs = option;
        }
    }

    // E.g. "4,7,21"
    char pins[256] = {};
    if (preferences_opt_string(preferences, "pins", pins, sizeof(pins)) == ERROR_NONE) {
        const char* position = pins;
        while (*position != '\0') {
            char* end = nullptr;
            const unsigned long pin = strtoul(position, &end, 10);
            if (end == position) break;
            setPinned(ctx, static_cast<uint32_t>(pin), true);
            position = (*end == ',') ? end + 1 : end;
        }
    }
    preferences_close(preferences);
}

static void saveSettings(Context* ctx) {
    char path[160];
    if (ctx->pinCount == 0 || !getSettingsPath(path, sizeof(path))) return;
    for (char* p = path + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(path, 0755); *p = '/'; }
    }
    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) return;

    char pins[256] = {};
    size_t length = 0;
    for (uint32_t i = 0; i < ctx->pinnedCount && length + 8 < sizeof(pins); ++i) {
        length += snprintf(pins + length, sizeof(pins) - length, "%u,", static_cast<unsigned>(ctx->pinnedPins[i]));
    }
    preferences_put_int32(preferences, "interval", static_cast<int32_t>(ctx->intervalMs));
    preferences_put_string(preferences, "pins", pins);
    preferences_close(preferences);
}

void gpioRunAction(Context* ctx) {
    const int32_t action = ctx->action.exchange(ACTION_NONE);
    const int32_t value = ctx->actionValue;

    lvgl_lock();
    if (action == ACTION_REMOVE) {
        setPinned(ctx, static_cast<uint32_t>(value), false);
    } else if (action == ACTION_ADD) {
        // The dialog listed the pins that weren't on the timeline
        auto* pins = new uint32_t[ctx->pinCount];
        const uint32_t count = listUnpinned(ctx, nullptr, nullptr, pins);
        if (static_cast<uint32_t>(value) < count) setPinned(ctx, pins[value], true);
        delete[] pins;
    } else if (action == ACTION_INTERVAL && static_cast<uint32_t>(value) < INTERVAL_COUNT) {
        ctx->intervalMs = INTERVALS_MS[value];
        // The samples so far are on another time scale
        clearHistory(ctx);
        if (ctx->timer != nullptr) lv_timer_set_period(ctx->timer, ctx->intervalMs);
        updateTimelineLabels(ctx);
        drawTimeline(ctx);
    }
    lvgl_unlock();
}

void gpioInit(Context* ctx) {
    if (device_get_first_active_by_type(&GPIO_CONTROLLER_TYPE, &ctx->gpioController) == ERROR_NONE) {
        gpio_controller_get_pin_count(ctx->gpioController, &ctx->pinCount);
    } else {
        LOG_E(TAG, "No active GPIO controller found");
    }
    ctx->pins = new Pin[ctx->pinCount]();
    ctx->pinnedPins = new uint32_t[ctx->pinCount]();
    ctx->lanes = new Lane[ctx->pinCount]();
    loadSettings(ctx);
}

void gpioTeardown(Context* ctx) {
    saveSettings(ctx);
    if (ctx->gpioController != nullptr) {
        device_put(ctx->gpioController);
        ctx->gpioController = nullptr;
    }
    delete[] ctx->pins;
    ctx->pins = nullptr;
    delete[] ctx->pinnedPins;
    delete[] ctx->lanes;
}
