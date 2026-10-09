// SPDX-License-Identifier: GPL-3.0
#pragma once

#include "ToneSynth.h"

#include <atomic>
#include <cstdint>

#include <tactility/drivers/audio_stream.h>
#include <tactility/freertos/task.h>

/**
 * Playback state shared between the UI thread and the playback task
 */
struct TonePlayback {
    Device* streamDevice = nullptr;
    AudioStreamHandle streamHandle = nullptr;
    uint8_t channels = 2; // 1 = mono, 2 = stereo; fixed at stream-open time

    std::atomic<bool> playing { false };
    std::atomic<bool> sweep { false };
    std::atomic<uint32_t> fixedHz { 440 };
    std::atomic<uint32_t> sweepMinHz { 200 };
    std::atomic<uint32_t> sweepMaxHz { 1000 };
    std::atomic<uint32_t> sweepPeriodMs { 4000 };
    std::atomic<uint32_t> waveform { static_cast<uint32_t>(ToneWaveform::Sine) };

    // Most recently produced frequency; the UI polls this to show live progress.
    std::atomic<uint32_t> currentHz { 0 };

    // Non-null while the playback task is alive; nulled by the task just before it deletes itself.
    std::atomic<TaskHandle_t> task { nullptr };
};

/**
 * Opens an output stream with the current channels setting and starts the playback
 * task. Rejects the call while playback is already active.
 */
error_t tone_playback_start(TonePlayback* playback);

/**
 * Requests the playback task to fade out and shut down, then blocks until it is gone.
 * Safe to call when playback is not running.
 */
void tone_playback_stop(TonePlayback* playback);

/** @return true while the playback task is producing audio. */
bool tone_playback_is_playing(const TonePlayback* playback);