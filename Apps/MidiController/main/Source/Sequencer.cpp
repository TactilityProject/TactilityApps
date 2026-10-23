#include "Sequencer.h"

#include <cstring>
#include <mutex>
#include <esp_log.h>

static constexpr auto* TAG = "MidiSeq";

Sequencer::Sequencer() {
    memset(steps_, 0, sizeof(steps_));

    esp_timer_create_args_t args = {
        .callback              = timerCb,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "midi_seq_tick",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer");
        timer_ = nullptr;
    }
}

Sequencer::~Sequencer() {
    stop();
    if (timer_) {
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void Sequencer::start() {
    if (playing_.load()) return;
    currentStep.store(0);
    playing_.store(true);
    restartTimer();
}

void Sequencer::stop() {
    if (!playing_.load()) return;
    playing_.store(false);
    if (timer_) esp_timer_stop(timer_);
    currentStep.store(-1);
}

void Sequencer::setBpm(float bpm) {
    if (bpm < 40.0f)  bpm = 40.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    bpm_.store(bpm);
    if (playing_.load()) restartTimer();
}

void Sequencer::toggleStep(int track, int step) {
    if (track >= 0 && track < NUM_TRACKS && step >= 0 && step < NUM_STEPS) {
        std::lock_guard<std::mutex> lock(mutex_);
        steps_[track][step] = !steps_[track][step];
    }
}

bool Sequencer::getStep(int track, int step) const {
    if (track >= 0 && track < NUM_TRACKS && step >= 0 && step < NUM_STEPS) {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return steps_[track][step];
    }
    return false;
}

void Sequencer::clearAll() {
    // Stop playback first so the timer callback cannot observe a partially-cleared array
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    memset(steps_, 0, sizeof(steps_));
}

void Sequencer::restartTimer() {
    if (!timer_) return;
    esp_timer_stop(timer_);
    // 16th-note interval: 60s / BPM / 4 * 1e6 us = 15,000,000 / BPM
    uint64_t intervalUs = (uint64_t)(15000000.0f / bpm_.load());
    if (esp_timer_start_periodic(timer_, intervalUs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sequencer timer");
    }
}

void Sequencer::timerCb(void* arg) {
    static_cast<Sequencer*>(arg)->onTimerFired();
}

void Sequencer::onTimerFired() {
    if (!playing_.load()) return;

    int step = currentStep.load();

    if (onStep) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int t = 0; t < NUM_TRACKS; t++) {
            if (steps_[t][step]) {
                onStep(step, t);
            }
        }
    }

    currentStep.store((step + 1) % NUM_STEPS);

    if (onTick) onTick(step);
}
