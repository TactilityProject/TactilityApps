#pragma once

#include <Tactility/Thread.h>

#include <lvgl.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

constexpr uint32_t MIN_WORDS = 4;
constexpr uint32_t MAX_WORDS = 12;
constexpr uint32_t DEFAULT_WORDS = 6;
/** One meter segment per possible word count */
constexpr uint32_t METER_SEGMENTS = MAX_WORDS - MIN_WORDS + 1;

struct Context {
    uint32_t appInstanceId;

    // Guarded by the LVGL lock
    uint32_t wordCount = DEFAULT_WORDS;
    std::vector<std::string> words;
    bool failed = false; // the word list couldn't be read

    // UI pointers (nulled when the widgets are destroyed)
    lv_obj_t* phraseLabel = nullptr;
    lv_obj_t* meter[METER_SEGMENTS] = {};
    lv_obj_t* strengthLabel = nullptr;
    lv_obj_t* countLabel = nullptr;

    // Picking words reads the word list, so it runs on its own thread
    std::unique_ptr<tt::Thread> jobThread = nullptr;
    std::atomic<bool> busy { false };
};

/** window_manager_create_ext()'s callbacks - @a userData is the Context* for this instance. */
void dicewareCreateWidgets(lv_obj_t* parent, void* userData);
void dicewareDestroyWidgets(void* userData);

/** Joins the background word-picking job, if any. Call once the window is torn down. */
void dicewareTeardown(Context* ctx);
