// SPDX-License-Identifier: GPL-3.0
#include "TonePlayback.h"

#include <tactility/device.h>
#include <tactility/freertos/freertos.h>
#include <tactility/log.h>

#include <cstdlib>

namespace {

constexpr auto* TAG = "tone_playback";

constexpr uint16_t CHUNK_FRAMES = 512;
constexpr uint16_t MAX_CHANNELS = 2;
constexpr uint32_t FADE_SAMPLES = tone_synth::SAMPLE_RATE * 5 / 1000;
constexpr float INVERSE_FADE_SAMPLES = 1.0f / FADE_SAMPLES;
constexpr int16_t AMPLITUDE = 8000; // ~25% of full scale

constexpr uint16_t PLAYBACK_TASK_STACK_BYTES = 4096;
constexpr uint16_t PLAYBACK_TASK_PRIORITY = 9;
constexpr char PLAYBACK_TASK_NAME[] = "tone-playback";

void playbackTask(void* argument) {
    auto* playback = static_cast<TonePlayback*>(argument);

    // Block until tone_playback_start() has published this task's handle. Without this,
    // the task can run on a second core and clear its own handle (or exit via an early
    // allocation failure) before the creator's store lands, leaving a stale handle that
    // tone_playback_stop() would wait on forever.
    if (xSemaphoreTake(playback->lifecycleMutex, portMAX_DELAY) != pdTRUE) {
        vTaskDelete(nullptr);
        return;
    }

    const uint8_t channels = playback->channels;
    const size_t samplesPerChunk = CHUNK_FRAMES * channels;
    auto* chunk = static_cast<int16_t*>(malloc(samplesPerChunk * sizeof(int16_t)));
    if (chunk == nullptr) {
        LOG_E(TAG, "Out of memory for chunk buffer");
        playback->playing.store(false);
        playback->currentHz.store(0);
        xSemaphoreGive(playback->lifecycleMutex);
        playback->task.store(nullptr);
        vTaskDelete(nullptr);
        return;
    }
    xSemaphoreGive(playback->lifecycleMutex);

    tone_synth::SynthState synthState;
    bool stopping = false;
    uint32_t fadeOutRemaining = 0;
    uint32_t lastFrequency = 0;

    while (true) {
        if (!playback->playing.load() && !stopping) {
            stopping = true;
            fadeOutRemaining = FADE_SAMPLES;
        }
        if (stopping && fadeOutRemaining == 0) {
            break;
        }

        const uint32_t sweepPeriodMs = playback->sweepPeriodMs.load();
        // SAMPLE_RATE is a multiple of 1000, so this is exact without an overflowing multiply.
        const uint32_t sweepPeriodSamples = sweepPeriodMs * (tone_synth::SAMPLE_RATE / 1000);
        const bool sweep = playback->sweep.load();
        const uint32_t fixedHz = playback->fixedHz.load();
        const uint32_t sweepMinHz = playback->sweepMinHz.load();
        const uint32_t sweepMaxHz = playback->sweepMaxHz.load();
        const auto waveform = static_cast<ToneWaveform>(playback->waveform.load());

        for (uint16_t frame = 0; frame < CHUNK_FRAMES; frame++) {
            const uint32_t frequency = tone_synth::frequency_at(
                fixedHz, sweep, sweepMinHz, sweepMaxHz, sweepPeriodSamples, synthState);
            const float sample = tone_synth::next_sample(waveform, frequency, synthState);
            lastFrequency = frequency;

            float gain = 1.0f;
            if (synthState.sampleIndex < FADE_SAMPLES) {
                gain = static_cast<float>(synthState.sampleIndex) * INVERSE_FADE_SAMPLES;
            }
            if (stopping) {
                gain *= static_cast<float>(fadeOutRemaining) * INVERSE_FADE_SAMPLES;
            }

            const int16_t value = static_cast<int16_t>(sample * AMPLITUDE * gain);
            for (uint8_t c = 0; c < channels; c++) {
                chunk[frame * channels + c] = value;
            }

            if (stopping && fadeOutRemaining > 0) {
                fadeOutRemaining--;
            }
        }

        playback->currentHz.store(lastFrequency);

        size_t bytesWritten = 0;
        const error_t result = audio_stream_write(
            playback->streamHandle, chunk, samplesPerChunk * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        if (result != ERROR_NONE) {
            LOG_E(TAG, "Stream write failed: %s", error_to_string(result));
            playback->playing.store(false);
            break;
        }
    }

    if (xSemaphoreTake(playback->lifecycleMutex, portMAX_DELAY) == pdTRUE) {
        if (playback->streamHandle != nullptr) {
            audio_stream_close(playback->streamHandle);
            playback->streamHandle = nullptr;
        }
        playback->playing.store(false);
        playback->currentHz.store(0);
        xSemaphoreGive(playback->lifecycleMutex);
    }
    playback->task.store(nullptr);
    free(chunk);
    vTaskDelete(nullptr);
}

} // namespace

error_t tone_playback_start(TonePlayback* playback) {
    if (playback->lifecycleMutex == nullptr) {
        return ERROR_INVALID_STATE;
    }
    // Hold the mutex from the busy-state check through task creation and handle
    // publication so the family of publications is atomic: the new task blocks on the
    // same mutex before it can clear its own handle, and a concurrent stop() cannot
    // observe a half-initialized start.
    if (xSemaphoreTake(playback->lifecycleMutex, portMAX_DELAY) != pdTRUE) {
        return ERROR_RESOURCE_BUSY;
    }
    if (playback->playing.load() || playback->task.load() != nullptr) {
        xSemaphoreGive(playback->lifecycleMutex);
        return ERROR_RESOURCE_BUSY;
    }

    Device* streamDevice = playback->streamDevice;
    if (streamDevice == nullptr) {
        if (device_get_first_by_type(&AUDIO_STREAM_TYPE, &streamDevice) != ERROR_NONE || streamDevice == nullptr) {
            xSemaphoreGive(playback->lifecycleMutex);
            return ERROR_NOT_FOUND;
        }
        playback->streamDevice = streamDevice;
    }

    const AudioStreamConfig config = {
        .sample_rate = tone_synth::SAMPLE_RATE,
        .bits_per_sample = 16,
        .channels = playback->channels,
    };

    AudioStreamHandle handle = nullptr;
    const error_t openResult = audio_stream_open_output(streamDevice, &config, &handle);
    if (openResult != ERROR_NONE) {
        LOG_E(TAG, "open_output failed: %s", error_to_string(openResult));
        xSemaphoreGive(playback->lifecycleMutex);
        return openResult;
    }
    playback->streamHandle = handle;

    playback->playing.store(true);
    playback->currentHz.store(0);

    TaskHandle_t playbackTaskHandle = nullptr;
    if (xTaskCreate(
            playbackTask, PLAYBACK_TASK_NAME, PLAYBACK_TASK_STACK_BYTES, playback,
            PLAYBACK_TASK_PRIORITY, &playbackTaskHandle) != pdPASS) {
        playback->playing.store(false);
        playback->streamHandle = nullptr;
        audio_stream_close(handle);
        xSemaphoreGive(playback->lifecycleMutex);
        return ERROR_OUT_OF_MEMORY;
    }
    playback->task.store(playbackTaskHandle);
    xSemaphoreGive(playback->lifecycleMutex);

    return ERROR_NONE;
}

void tone_playback_stop(TonePlayback* playback) {
    if (playback->lifecycleMutex == nullptr) {
        return;
    }
    // Clear the play flag under the mutex so a racing start() cannot republish the
    // task handle while we tear it down; release before waiting on the task itself.
    if (xSemaphoreTake(playback->lifecycleMutex, portMAX_DELAY) == pdTRUE) {
        playback->playing.store(false);
        xSemaphoreGive(playback->lifecycleMutex);
    }

    while (playback->task.load() != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool tone_playback_is_playing(const TonePlayback* playback) {
    return playback->playing.load();
}