// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ui.h"
#include "Tabs.h"
#include "Widgets.h"

#include <app/event.h>
#include <lvgl/fonts.h>
#include <lvgl/devices/keyboard.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/icons/shared.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/Memory.hpp>
#include <tactility/time.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gpsviewer {

namespace {

// lv_color_to_u32() is not exported to apps
uint32_t colorKey(lv_color_t color) {
    return (static_cast<uint32_t>(color.red) << 16) | (static_cast<uint32_t>(color.green) << 8) | color.blue;
}

constexpr int TAB_COUNT = 4;
constexpr uint32_t REFRESH_MS = 333;
constexpr lv_state_t NIGHT_STATE = LV_STATE_USER_4;
constexpr lv_style_selector_t NIGHT_SELECTOR = static_cast<lv_style_selector_t>(LV_PART_MAIN) | static_cast<lv_style_selector_t>(NIGHT_STATE);

// Tab order. The indices are also the keys 1 to 4.
enum TabIndex : int {
    TAB_POSITION = 0,
    TAB_SKY = 1,
    TAB_SIGNAL = 2,
    TAB_STATUS = 3,
};

// Character arrays, not string pointers: the app loader leaves pointers in data tables unrelocated
constexpr char TAB_NAMES[TAB_COUNT][9] = {"Position", "Sky", "Signal", "Status"};

enum class Overlay : uint8_t {
    None,
    Menu,
    Settings,
};

enum class MenuItem : uint8_t {
    ResetSpeed,
    Recording,
    NightMode,
    OpenSettings,
    Exit,
    Back,
};

enum class SettingItem : uint8_t {
    Units,
    Coordinates,
    Night,
    Back,
};

} // namespace

struct UiState {
    AppContext* context = nullptr;
    Theme theme {};
    lv_obj_t* root = nullptr;
    lv_obj_t* header = nullptr;
    lv_obj_t* headerBadge = nullptr;
    uint32_t badgeColor = 0;
    lv_obj_t* tabButtons[TAB_COUNT] {};
    lv_obj_t* tabIndicators[TAB_COUNT] {};
    lv_obj_t* content = nullptr;
    lv_obj_t* panels[TAB_COUNT] {};
    MessageCard messageCard;
    lv_obj_t* overlay = nullptr;
    // The keys go to this group while an overlay is open, so the focus can't leave it
    lv_group_t* overlayGroup = nullptr;
    lv_timer_t* timer = nullptr;
    Overlay overlayType = Overlay::None;
    int activeTab = 0;
    // Left and Right scroll the Signal bars instead of switching tabs
    bool signalScrolling = false;
    bool rebuildPending = false;
    SourceStatus status = SourceStatus::NoReceiver;
    char receiverName[32] = "";
    bool recording = false;
    uint32_t recordedLines = 0;
    Settings settings {};
    StatusTab statusTab;
    SignalTab signalTab;
    SkyTab skyTab;
    PositionTab positionTab;
    GnssSnapshot snapshot {};
};

namespace {

void build(UiState& ui);
void refresh(UiState& ui);
void setNightMode(UiState& ui, bool enable);

// Fix state in the toolbar: "3D FIX  3.5m", prefixed with REC
void updateHeader(UiState& ui) {
    if (ui.headerBadge == nullptr) {
        return;
    }
    const GnssSnapshot& s = ui.snapshot;
    const bool receiving = ui.status == SourceStatus::Receiving;
    const FixType fix = receiving ? s.fix : FixType::None;
    char text[40];
    size_t length = 0;
    text[0] = '\0';
    if (ui.recording) {
        length = static_cast<size_t>(snprintf(text, sizeof(text), "REC  "));
    }
    length += static_cast<size_t>(snprintf(text + length, sizeof(text) - length, "%s", fixText(fix)));
    if (fix != FixType::None && !std::isnan(s.hdop) && length < sizeof(text)) {
        char accuracy[24];
        formatAccuracy(estimatedAccuracyMeters(s.hdop), ui.settings.units, accuracy, sizeof(accuracy));
        // "3.5 m" -> "3.5m"
        size_t out = 0;
        for (size_t i = 0; accuracy[i] != '\0'; i++) {
            if (accuracy[i] != ' ') {
                accuracy[out++] = accuracy[i];
            }
        }
        accuracy[out] = '\0';
        snprintf(text + length, sizeof(text) - length, "  %s", accuracy);
    }
    lv_color_t color = fixColor(fix, ui.theme);
    if (ui.recording) {
        color = ui.theme.warning;
    }
    setLabelText(ui.headerBadge, text);
    const uint32_t colorValue = colorKey(color);
    if (colorValue != ui.badgeColor) {
        ui.badgeColor = colorValue;
        lv_obj_set_style_text_color(ui.headerBadge, color, LV_PART_MAIN);
    }
}

void styleTabButtons(UiState& ui) {
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t* button = ui.tabButtons[i];
        if (button == nullptr) {
            continue;
        }
        const bool active = i == ui.activeTab;
        setHidden(ui.tabIndicators[i], !active);
        lv_obj_t* label = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_color(label, active ? ui.theme.accent : ui.theme.dim, LV_PART_MAIN);
    }
}

// The content area gets an outline while the keys scroll the Signal bars
void setSignalScrolling(UiState& ui, bool scrolling) {
    ui.signalScrolling = scrolling;
    if (ui.content == nullptr) {
        return;
    }
    const int32_t width = scrolling ? 2 : 0;
    for (const lv_style_selector_t state : { LV_STATE_DEFAULT, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY }) {
        lv_obj_set_style_outline_color(ui.content, ui.theme.accent, state);
        lv_obj_set_style_outline_width(ui.content, width, state);
        lv_obj_set_style_outline_pad(ui.content, -2, state);
    }
}

void setTab(UiState& ui, int tab) {
    ui.activeTab = (tab + TAB_COUNT) % TAB_COUNT;
    if (ui.activeTab != TAB_SIGNAL && ui.signalScrolling) {
        setSignalScrolling(ui, false);
    }
    styleTabButtons(ui);
    refresh(ui);
}

void requestBits(UiState& ui, uint32_t bits) {
    ui.context->requests.fetch_or(bits);
}

void toggleRecording(UiState& ui) {
    ui.context->recordingToggles.fetch_add(1);
}

void applySettings(UiState& ui, const Settings& settings) {
    {
        ContextLock lock(*ui.context);
        ui.context->settings = settings;
    }
    ui.settings = settings;
    requestBits(ui, REQUEST_SAVE_SETTINGS);
}

// Stores the palette choice. The widget tree is rebuilt with the other palette on the next timer
// tick (or when the overlay is closed), as there are no shared styles to restyle in place.
void setNightMode(UiState& ui, bool enable) {
    Settings settings = ui.settings;
    if (settings.nightMode == enable) {
        return;
    }
    settings.nightMode = enable;
    applySettings(ui, settings);
    ui.rebuildPending = true;
}

void toggleNight(UiState& ui) {
    setNightMode(ui, !ui.settings.nightMode);
}

// Keys go to the content area, or to OPEN SETTINGS while a message card with that button is shown
void focusMain(UiState& ui) {
    lv_group_t* group = lv_group_get_default();
    if (group == nullptr) {
        return;
    }
    lv_obj_t* target = ui.content;
    if (ui.messageCard.shown() && ui.messageCard.focusTarget() != nullptr &&
        !lv_obj_has_flag(ui.messageCard.focusTarget(), LV_OBJ_FLAG_HIDDEN)) {
        target = ui.messageCard.focusTarget();
    }
    if (target != nullptr) {
        lv_group_focus_obj(target);
    }
}

// Hands the keys back to the app's group and drops the overlay's
void releaseOverlayGroup(UiState& ui) {
    if (ui.overlayGroup != nullptr) {
        lvgl_keyboard_set_active_group(nullptr);
        lv_group_delete(ui.overlayGroup);
        ui.overlayGroup = nullptr;
    }
}

void closeOverlay(UiState& ui) {
    releaseOverlayGroup(ui);
    if (ui.overlay != nullptr) {
        lv_obj_delete_async(ui.overlay);
        ui.overlay = nullptr;
    }
    ui.overlayType = Overlay::None;
    focusMain(ui);
}

// The next button in the list from index, wrapping around, past the title
lv_obj_t* nextOverlayButton(lv_obj_t* list, int index, int step) {
    const int count = static_cast<int>(lv_obj_get_child_count(list));
    for (int i = 1; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(list, ((index + step * i) % count + count) % count);
        // The buttons are clickable, the title is not
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            return child;
        }
    }
    return nullptr;
}

// Overlay lists get Left/Up and Right/Down as focus moves, and Esc/Backspace to close
void onOverlayItemKey(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    lv_obj_t* item = lv_event_get_current_target_obj(event);
    const uint32_t key = lv_event_get_key(event);
    lv_obj_t* list = lv_obj_get_parent(item);
    const int index = static_cast<int>(lv_obj_get_index(item));
    if (key == LV_KEY_LEFT || key == LV_KEY_UP || key == LV_KEY_RIGHT || key == LV_KEY_DOWN) {
        const int step = (key == LV_KEY_LEFT || key == LV_KEY_UP) ? -1 : 1;
        lv_obj_t* next = nextOverlayButton(list, index, step);
        if (next != nullptr) {
            lv_group_focus_obj(next);
        }
    } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        closeOverlay(*ui);
    }
}

void onOverlayBackdrop(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    if (lv_event_get_target_obj(event) == ui->overlay) {
        closeOverlay(*ui);
    }
}

lv_obj_t* createOverlay(UiState& ui, Overlay type, const char* title) {
    closeOverlay(ui);
    ui.overlayType = type;
    ui.overlay = lv_obj_create(ui.root);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(ui.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.overlay, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ui.overlay, onOverlayBackdrop, LV_EVENT_CLICKED, &ui);

    ui.overlayGroup = lv_group_create();
    if (ui.overlayGroup != nullptr) {
        lvgl_keyboard_set_active_group(ui.overlayGroup);
    }

    lv_obj_t* list = lv_list_create(ui.overlay);
    lv_obj_set_size(list, LV_PCT(80), LV_PCT(95));
    lv_obj_center(list);
    lv_obj_t* titleLabel = lv_list_add_text(list, title);
    if (ui.theme.night) {
        const Theme& theme = ui.theme;
        lv_obj_set_style_bg_color(list, theme.background, LV_PART_MAIN);
        lv_obj_set_style_border_color(list, theme.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(titleLabel, theme.background, LV_PART_MAIN);
        lv_obj_set_style_text_color(titleLabel, theme.dim, LV_PART_MAIN);
    }
    return list;
}

// Night mode: dark buttons with red text, and a dark red fill on the focused one
void paintOverlayButton(lv_obj_t* button, const Theme& theme) {
    lv_obj_set_style_bg_color(button, theme.background, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, theme.text, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, theme.border, LV_PART_MAIN);
    for (const lv_style_selector_t state : { LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_PRESSED }) {
        lv_obj_set_style_bg_color(button, theme.mono ? theme.text : theme.border, state);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, state);
        lv_obj_set_style_text_color(button, theme.mono ? theme.background : theme.accent, state);
    }
}

lv_obj_t* addOverlayItem(UiState& ui, lv_obj_t* list, const char* text, lv_event_cb_t onClick, uint32_t id) {
    lv_obj_t* button = lv_list_add_button(list, nullptr, text);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    lv_obj_add_event_cb(button, onClick, LV_EVENT_CLICKED, &ui);
    lv_obj_add_event_cb(button, onOverlayItemKey, LV_EVENT_KEY, &ui);
    if (ui.theme.night) {
        paintOverlayButton(button, ui.theme);
    }
    if (ui.overlayGroup != nullptr) {
        lv_group_add_obj(ui.overlayGroup, button);
    }
    return button;
}

uint32_t itemId(lv_event_t* event) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
}

void openSettings(UiState& ui);

void onMenuItem(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    const auto item = static_cast<MenuItem>(itemId(event));
    switch (item) {
        case MenuItem::ResetSpeed:
            requestBits(*ui, REQUEST_RESET_SPEED);
            closeOverlay(*ui);
            break;
        case MenuItem::Recording:
            toggleRecording(*ui);
            closeOverlay(*ui);
            break;
        case MenuItem::NightMode:
            closeOverlay(*ui);
            toggleNight(*ui);
            break;
        case MenuItem::OpenSettings:
            openSettings(*ui);
            break;
        case MenuItem::Exit:
            closeOverlay(*ui);
            app_event_emit_close(ui->context->appInstanceId);
            break;
        case MenuItem::Back:
            closeOverlay(*ui);
            break;
    }
}

void openMenu(UiState& ui) {
    lv_obj_t* list = createOverlay(ui, Overlay::Menu, "GPS Viewer");
    lv_obj_t* first = addOverlayItem(ui, list, "Reset max/average speed", onMenuItem, static_cast<uint32_t>(MenuItem::ResetSpeed));
    char recordText[48];
    const bool canRecord = ui.context->dataDirectory[0] != '\0';
    if (ui.recording) {
        snprintf(recordText, sizeof(recordText), "Stop recording (%lu lines)", static_cast<unsigned long>(ui.recordedLines));
    } else if (canRecord) {
        snprintf(recordText, sizeof(recordText), "Start recording");
    } else {
        snprintf(recordText, sizeof(recordText), "No storage: can't record or save settings");
    }
    lv_obj_t* recordItem = addOverlayItem(ui, list, recordText, onMenuItem, static_cast<uint32_t>(MenuItem::Recording));
    if (!ui.recording && !canRecord) {
        lv_obj_add_state(recordItem, LV_STATE_DISABLED);
    }
    addOverlayItem(ui, list, ui.settings.nightMode ? "Night mode: off" : "Night mode: on", onMenuItem, static_cast<uint32_t>(MenuItem::NightMode));
    addOverlayItem(ui, list, "Settings", onMenuItem, static_cast<uint32_t>(MenuItem::OpenSettings));
    addOverlayItem(ui, list, "Exit", onMenuItem, static_cast<uint32_t>(MenuItem::Exit));
    addOverlayItem(ui, list, "< Back", onMenuItem, static_cast<uint32_t>(MenuItem::Back));
    if (first != nullptr) {
        lv_group_focus_obj(first);
    }
}

void settingText(const Settings& settings, SettingItem item, char* out, size_t outSize) {
    static const char units[3][9] = {"Metric", "Imperial", "Nautical"};
    static const char coordinates[3][18] = {"Degrees", "Deg. minutes", "Deg. min. seconds"};
    switch (item) {
        case SettingItem::Units:
            snprintf(out, outSize, "Units: %s", units[static_cast<int>(settings.units)]);
            break;
        case SettingItem::Coordinates:
            snprintf(out, outSize, "Coordinates: %s", coordinates[static_cast<int>(settings.coordinateFormat)]);
            break;
        case SettingItem::Night:
            snprintf(out, outSize, "Night mode: %s", settings.nightMode ? "on" : "off");
            break;
        case SettingItem::Back:
            snprintf(out, outSize, "< Back");
            break;
    }
}

void onSettingItem(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    const auto item = static_cast<SettingItem>(itemId(event));
    Settings settings = ui->settings;
    switch (item) {
        case SettingItem::Units:
            settings.units = static_cast<Units>((static_cast<int>(settings.units) + 1) % 3);
            break;
        case SettingItem::Coordinates:
            settings.coordinateFormat = static_cast<CoordinateFormat>((static_cast<int>(settings.coordinateFormat) + 1) % 3);
            break;
        case SettingItem::Night:
            settings.nightMode = !settings.nightMode;
            break;
        case SettingItem::Back:
            closeOverlay(*ui);
            return;
    }
    applySettings(*ui, settings);
    if (item == SettingItem::Night) {
        ui->rebuildPending = true;
    }
    char text[48];
    settingText(settings, item, text, sizeof(text));
    lv_obj_t* label = lv_obj_get_child(lv_event_get_current_target_obj(event), 0);
    if (label != nullptr) {
        lv_label_set_text(label, text);
    }
}

void openSettings(UiState& ui) {
    lv_obj_t* list = createOverlay(ui, Overlay::Settings, "Settings");
    lv_obj_t* first = nullptr;
    for (int i = 0; i <= static_cast<int>(SettingItem::Back); i++) {
        char text[48];
        settingText(ui.settings, static_cast<SettingItem>(i), text, sizeof(text));
        lv_obj_t* item = addOverlayItem(ui, list, text, onSettingItem, i);
        if (first == nullptr) {
            first = item;
        }
    }
    lv_group_focus_obj(first);
}

void onMenuButton(lv_event_t* event) {
    openMenu(*static_cast<UiState*>(lv_event_get_user_data(event)));
}

bool isKeyInput() {
    lv_indev_t* indev = lv_indev_active();
    return indev != nullptr && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER;
}

// Selecting the Signal tab with keys moves the keys to its bars, as Left and Right on the tab row only move the focus
void onTabButton(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    lv_obj_t* button = lv_event_get_current_target_obj(event);
    for (int i = 0; i < TAB_COUNT; i++) {
        if (ui->tabButtons[i] == button) {
            setTab(*ui, i);
            if (i == TAB_SIGNAL && isKeyInput() && !ui->messageCard.shown()) {
                lv_group_focus_obj(ui->content);
                setSignalScrolling(*ui, true);
            }
        }
    }
}

void moveTab(UiState& ui, int direction) {
    if (ui.signalScrolling) {
        ui.signalTab.scroll(direction);
        return;
    }
    setTab(ui, ui.activeTab + direction);
}

// Keys reach the content area while it has the focus. Letter keys are shortcuts only, every
// action is also in the Enter menu.
void onContentKey(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    const uint32_t key = lv_event_get_key(event);
    switch (key) {
        case LV_KEY_ESC:
        case LV_KEY_BACKSPACE:
            if (ui->signalScrolling) {
                setSignalScrolling(*ui, false);
            }
            break;
        case LV_KEY_LEFT:
        case ',':
            moveTab(*ui, -1);
            break;
        case LV_KEY_RIGHT:
        case '.':
        case ' ':
            moveTab(*ui, 1);
            break;
        case '1':
        case '2':
        case '3':
        case '4':
            setTab(*ui, static_cast<int>(key - '1'));
            break;
        case 'n':
        case 'N':
            toggleNight(*ui);
            break;
        case 'r':
        case 'R':
            requestBits(*ui, REQUEST_RESET_SPEED);
            break;
        case 's':
        case 'S':
            openSettings(*ui);
            break;
        case 'm':
        case 'M':
            openMenu(*ui);
            break;
        case 'l':
        case 'L':
            toggleRecording(*ui);
            break;
        default:
            break;
    }
}

// Enter from a keyboard, trackball or encoder opens the menu. Taps on the content don't.
// Enter opens the action menu, and on the Signal tab it switches between scrolling the bars and switching tabs
void onContentClicked(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    if (!isKeyInput()) {
        return;
    }
    if (ui->activeTab == TAB_SIGNAL && !ui->messageCard.shown()) {
        setSignalScrolling(*ui, !ui->signalScrolling);
        return;
    }
    openMenu(*ui);
}

void onContentDefocused(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    if (ui->signalScrolling) {
        setSignalScrolling(*ui, false);
    }
}

void onContentGesture(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr || ui->overlayType != Overlay::None) {
        return;
    }
    // Widgets with event bubbling pass their own gestures up too. One that turned off gesture bubbling,
    // like the dragged Signal bars, handles the swipe itself.
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (target != ui->content && !lv_obj_is_gesture_bubble(target)) {
        return;
    }
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_LEFT) {
        setTab(*ui, ui->activeTab + 1);
    } else if (direction == LV_DIR_RIGHT) {
        setTab(*ui, ui->activeTab - 1);
    }
}


void onTimer(lv_timer_t* timer) {
    auto* ui = static_cast<UiState*>(lv_timer_get_user_data(timer));
    if (ui->rebuildPending && ui->overlayType == Overlay::None) {
        ui->rebuildPending = false;
        build(*ui);
        return;
    }
    refresh(*ui);
}

lv_obj_t* createPanel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

void clearTabs(UiState& ui) {
    ui.statusTab.clear();
    ui.signalTab.clear();
    ui.skyTab.clear();
    ui.positionTab.clear();
}

void forgetWidgets(UiState& ui) {
    releaseOverlayGroup(ui);
    if (ui.timer != nullptr) {
        lv_timer_delete(ui.timer);
        ui.timer = nullptr;
    }
    clearTabs(ui);
    ui.messageCard.clear();
    ui.header = nullptr;
    ui.headerBadge = nullptr;
    ui.badgeColor = 0;
    ui.content = nullptr;
    ui.signalScrolling = false;
    ui.overlay = nullptr;
    ui.overlayType = Overlay::None;
    for (int i = 0; i < TAB_COUNT; i++) {
        ui.tabButtons[i] = nullptr;
        ui.tabIndicators[i] = nullptr;
        ui.panels[i] = nullptr;
    }
}

void onCloseButton(lv_event_t* event) {
    auto* ui = static_cast<UiState*>(lv_event_get_user_data(event));
    app_event_emit_close(ui->context->appInstanceId);
}

void onOpenLocationSettings(lv_event_t* event) {
    requestBits(*static_cast<UiState*>(lv_event_get_user_data(event)), REQUEST_OPEN_LOCATION_SETTINGS);
}

void onBadgeButton(lv_event_t* event) {
    setTab(*static_cast<UiState*>(lv_event_get_user_data(event)), TAB_STATUS);
}

// Night mode paints the toolbar like the rest of the app
void paintNight(lv_obj_t* object, const Theme& theme) {
    lv_obj_set_style_text_color(object, theme.text, LV_PART_MAIN);
    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; i++) {
        paintNight(lv_obj_get_child(object, static_cast<int32_t>(i)), theme);
    }
}

void createHeader(UiState& ui) {
    ui.header = lvgl_toolbar_create(ui.root, "GPS Viewer");
    lvgl_toolbar_set_nav_action(ui.header, LV_SYMBOL_CLOSE, onCloseButton, &ui);
    // Fix state, filled in by updateHeader(). Opens the Status tab.
    lv_obj_t* badge = lvgl_toolbar_add_text_button_action(ui.header, "", onBadgeButton, &ui);
    // Action buttons are square, this one holds text
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, LV_DPX(4), LV_PART_MAIN);
    ui.headerBadge = lv_obj_get_child(badge, 0);
    if (ui.headerBadge != nullptr) {
        lv_obj_set_style_text_font(ui.headerBadge, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    }
    lv_obj_t* menu = lvgl_toolbar_add_text_button_action(ui.header, LVGL_ICON_SHARED_MORE_VERT, onMenuButton, &ui);
    lv_obj_set_style_text_font(menu, lvgl_get_shared_icon_default_font(), LV_PART_MAIN);
    if (ui.theme.night) {
        lv_obj_set_style_bg_color(ui.header, ui.theme.background, LV_PART_MAIN);
        paintNight(ui.header, ui.theme);
    }
    ui.badgeColor = 0;
}

// Text tabs with a 2 px line under the active one. The strip is a grid navigation row, so the arrow keys and
// encoders reach the tabs, and Enter selects the focused one.
void createTabStrip(UiState& ui) {
    const Theme& theme = ui.theme;
    lv_obj_t* strip = createBox(ui.root);
    lv_obj_set_size(strip, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_color(strip, theme.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t* button = createBox(strip);
        lv_obj_set_size(button, LV_PCT(25), LV_DPX(24));
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_outline_color(button, theme.accent, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(button, -2, LV_STATE_FOCUS_KEY);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_t* label = createLabel(button, FONT_SIZE_SMALL, theme.dim);
        lv_label_set_text(label, TAB_NAMES[i]);
        lv_obj_center(label);
        lv_obj_t* indicator = createBox(button);
        lv_obj_set_size(indicator, LV_PCT(70), 2);
        lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(indicator, theme.accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_FLOATING);
        lv_obj_add_event_cb(button, onTabButton, LV_EVENT_CLICKED, &ui);
        ui.tabButtons[i] = button;
        ui.tabIndicators[i] = indicator;
    }
    lvgl_grid_navigation_add(strip);
}

void build(UiState& ui) {
    forgetWidgets(ui);
    lv_obj_clean(ui.root);
    {
        ContextLock lock(*ui.context);
        ui.settings = ui.context->settings;
    }
    ui.theme = makeTheme(ui.settings.nightMode);
    lv_obj_t* root = ui.root;

    // The night background is a style of a user state of the window's widget. Leaving night mode clears
    // the state, and the window's own background is back (lv_obj_remove_local_style_prop() is not exported).
    if (ui.theme.paintBackground) {
        lv_obj_set_style_bg_color(root, ui.theme.background, NIGHT_SELECTOR);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, NIGHT_SELECTOR);
        lv_obj_add_state(root, NIGHT_STATE);
    } else {
        lv_obj_remove_state(root, NIGHT_STATE);
    }
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    createHeader(ui);
    createTabStrip(ui);

    ui.content = lv_obj_create(root);
    lv_obj_set_width(ui.content, LV_PCT(100));
    lv_obj_set_flex_grow(ui.content, 1);
    lv_obj_set_style_bg_opa(ui.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.content, LV_DPX(4), LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui.content, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(ui.content, 0, LV_STATE_FOCUSED);
    lv_obj_remove_flag(ui.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui.content, onContentKey, LV_EVENT_KEY, &ui);
    lv_obj_add_event_cb(ui.content, onContentClicked, LV_EVENT_CLICKED, &ui);
    lv_obj_add_event_cb(ui.content, onContentDefocused, LV_EVENT_DEFOCUSED, &ui);
    lv_obj_add_event_cb(ui.content, onContentGesture, LV_EVENT_GESTURE, &ui);
    lv_obj_update_layout(root);

    for (int i = 0; i < TAB_COUNT; i++) {
        ui.panels[i] = createPanel(ui.content);
    }
    // The tabs size their widgets from the panel size
    lv_obj_update_layout(ui.content);
    ui.positionTab.create(ui.panels[TAB_POSITION], ui.theme);
    ui.skyTab.create(ui.panels[TAB_SKY], ui.theme);
    ui.signalTab.create(ui.panels[TAB_SIGNAL], ui.theme);
    ui.statusTab.create(ui.panels[TAB_STATUS], ui.theme);
    ui.messageCard.create(ui.content, ui.theme, onOpenLocationSettings, onContentKey, &ui);

    lv_group_t* group = lv_group_get_default();
    if (group != nullptr) {
        lv_group_add_obj(group, ui.content);
        lv_group_focus_obj(ui.content);
    }

    styleTabButtons(ui);
    ui.timer = lv_timer_create(onTimer, REFRESH_MS, &ui);
    refresh(ui);
}

void refresh(UiState& ui) {
    if (ui.content == nullptr) {
        return;
    }
    SourceStatus status;
    bool recording;
    {
        ContextLock lock(*ui.context);
        ui.context->model.snapshot(ui.snapshot, get_millis());
        status = ui.context->status;
        memcpy(ui.receiverName, ui.context->receiverName, sizeof(ui.receiverName));
        ui.settings = ui.context->settings;
        recording = ui.context->recording;
        ui.recordedLines = ui.context->recordedLines;
    }
    ui.status = status;
    ui.recording = recording;
    updateHeader(ui);

    const bool wasShown = ui.messageCard.shown();
    const bool hadButton = wasShown && ui.messageCard.focusTarget() != nullptr &&
                           !lv_obj_has_flag(ui.messageCard.focusTarget(), LV_OBJ_FLAG_HIDDEN);
    const bool showMessage = ui.messageCard.update(ui.status, ui.context->dataDirectory);
    const bool hasButton = showMessage && !lv_obj_has_flag(ui.messageCard.focusTarget(), LV_OBJ_FLAG_HIDDEN);
    if ((wasShown != showMessage || hadButton != hasButton) && ui.overlayType == Overlay::None) {
        focusMain(ui);
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        setHidden(ui.panels[i], i != ui.activeTab || showMessage);
    }
    if (showMessage) {
        return;
    }

    const TabView view {ui.snapshot, ui.settings, ui.status, ui.recording, ui.receiverName, ui.theme};
    switch (ui.activeTab) {
        case TAB_POSITION: ui.positionTab.update(view); break;
        case TAB_SKY: ui.skyTab.update(view); break;
        case TAB_SIGNAL: ui.signalTab.update(view); break;
        default: ui.statusTab.update(view); break;
    }
}

} // namespace

UiState* uiAllocate(AppContext& context) {
    auto* ui = tt::newObject<UiState>();
    if (ui != nullptr) {
        ui->context = &context;
    }
    return ui;
}

void uiFree(UiState* ui) {
    if (ui != nullptr) {
        tt::deleteObject(ui);
    }
}

void uiCreateWidgets(lv_obj_t* root, void* userData) {
    auto* context = static_cast<AppContext*>(userData);
    UiState& ui = *context->ui;
    ui.root = root;
    build(ui);
}

void setNightMode(AppContext& context, bool enable) {
    if (context.ui != nullptr) {
        setNightMode(*context.ui, enable);
    }
}

void uiDestroyWidgets(void* userData) {
    auto* context = static_cast<AppContext*>(userData);
    UiState& ui = *context->ui;
    forgetWidgets(ui);
    ui.root = nullptr;
}

} // namespace gpsviewer
