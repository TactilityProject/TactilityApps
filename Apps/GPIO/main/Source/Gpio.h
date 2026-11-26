#pragma once

#include <tactility/concurrent/task_event_group.h>
#include <tactility/drivers/gpio_controller.h>

#include <lvgl.h>
#include <stdint.h>


/** Samples that a timeline keeps */
constexpr uint32_t HISTORY_LENGTH = 100;
/** Corners of a timeline: a flat part and a step per sample */
constexpr uint32_t TRACE_POINTS = HISTORY_LENGTH * 2;

/** What is known about a pin since the app started */
struct Pin {
    bool high;
    uint32_t changes;
    /** lv_tick_get() of the last change, only valid when changes > 0 */
    uint32_t changedAt;
    /** Whether the tile is shown as recently changed */
    bool marked;
    lv_obj_t* tile;
    /** The levels of the last samples, as a ring that Context::historyEnd points into */
    uint8_t history[HISTORY_LENGTH];
    bool pinned; // has a lane on the timeline
};

/** A pinned pin's row on the timeline */
struct Lane {
    uint32_t pin;
    lv_obj_t* line;
    lv_obj_t* led;
    lv_point_precise_t* points; // TRACE_POINTS of them
};

constexpr uint32_t ACTION_QUEUE_LENGTH = 8;

/** Something that the app task has to do */
struct PendingAction {
    int32_t action;
    int32_t value;
};

struct Context {
    uint32_t appInstanceId;

    // The board's first active GPIO controller. Null if none is found.
    struct Device* gpioController = nullptr;
    uint32_t pinCount = 0;

    // new[] instead of std::vector: this app links with -nostdlib (elf_loader.cmake), so libstdc++
    // is never linked in. Size is pinCount. Guarded by the LVGL lock.
    Pin* pins = nullptr;
    bool sampled = false; // the first sample isn't a change
    uint32_t selected = 0;
    bool frozen = false;
    uint32_t tab = 0; // TAB_GRID or TAB_TIMELINE
    uint32_t intervalMs = 100;
    uint32_t historyEnd = 0;   // where the next sample goes
    uint32_t historyCount = 0;
    uint32_t drawnAt = 0;

    // The pins of the timeline, in the order they were added. Both are pinCount long.
    uint32_t* pinnedPins = nullptr;
    uint32_t pinnedCount = 0;
    Lane* lanes = nullptr;
    uint32_t laneCount = 0;

    // Dialog choices and lane removal are carried out on the app task, which actionBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t actionBit = 0;
    PendingAction actions[ACTION_QUEUE_LENGTH] = {}; // a ring, guarded by the LVGL lock
    uint32_t actionHead = 0;
    uint32_t actionCount = 0;

    // UI pointers (nulled when the widgets are destroyed)
    lv_obj_t* tabs = nullptr;
    lv_obj_t* gridPage = nullptr;
    lv_obj_t* timelinePage = nullptr;
    lv_obj_t* intervalLabel = nullptr;
    lv_obj_t* spanLabel = nullptr;
    lv_obj_t* laneList = nullptr;
    lv_obj_t* freezeLabel = nullptr;
    lv_obj_t* detailName = nullptr;
    lv_obj_t* detailChanges = nullptr;
    lv_timer_t* timer = nullptr;
};

/** Sets up state that must exist for the whole app instance lifetime. Call once, before
 *  window_manager_create_ext(). */
void gpioInit(Context* ctx);

/** window_manager_create_ext()'s callbacks - @a userData is the Context* for this instance. */
void gpioCreateWidgets(lv_obj_t* parent, void* userData);
void gpioDestroyWidgets(void* userData);

/** Releases the GPIO controller. Call once, after the window has been torn down. */
void gpioTeardown(Context* ctx);

/** Carries out the pending actions. Call on the app task, without the LVGL lock. */
void gpioRunAction(Context* ctx);
