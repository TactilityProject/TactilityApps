#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <esp_timer.h>

// 4-track 16-step drum sequencer using esp_timer for BPM-accurate step timing.

class Sequencer {
public:
    static constexpr int NUM_TRACKS = 4;
    static constexpr int NUM_STEPS  = 16;

    // Called from esp_timer task when a step fires for an active track.
    // onStep(step, track) - only fires for tracks that have that step enabled.
    using StepCallback = std::function<void(int step, int track)>;

    // Called each step tick regardless of active steps - for UI playhead advance.
    // Runs in esp_timer task - must be fast (store to atomic, no LVGL calls).
    using TickCallback = std::function<void(int step)>;

    Sequencer();
    ~Sequencer();

    void start();
    void stop();
    bool isPlaying() const { return playing_.load(); }

    void setBpm(float bpm);
    float getBpm() const { return bpm_.load(); }

    void toggleStep(int track, int step);
    bool getStep(int track, int step) const;
    void clearAll();

    // Currently playing step (-1 if stopped). Written from esp_timer task,
    // read from LVGL task - atomic for safe cross-task access.
    std::atomic<int> currentStep{-1};

    // Callbacks - must be set before start() is called; not safe to modify
    // while the sequencer is running.
    StepCallback onStep;
    TickCallback onTick;

private:
    bool  steps_[NUM_TRACKS][NUM_STEPS] = {};
    std::atomic<float> bpm_{120.0f};
    std::atomic<bool> playing_{false};
    std::mutex mutex_;  // guards steps_

    esp_timer_handle_t timer_ = nullptr;

    static void timerCb(void* arg);
    void        onTimerFired();
    void        restartTimer();
};
