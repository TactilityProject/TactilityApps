#include "Bleep/AudioOutput.h"
#include "Bleep/Song.h"

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/semphr.h>
#include <tactility/freertos/task.h>
#include <tactility/log.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace bleep {

namespace {

constexpr auto* TAG = "BleepAudio";
constexpr uint32_t TASK_STACK_SIZE = 6 * 1024;
// High, so audio keeps flowing while the app is busy. Capped for FreeRTOS ports with fewer
// priority levels (the host port has 10).
constexpr UBaseType_t TASK_PRIORITY = configMAX_PRIORITIES > 10 ? 10 : configMAX_PRIORITIES - 1;

} // namespace

void AudioOutput::taskMain(void* self) {
    static_cast<AudioOutput*>(self)->run();
}

void AudioOutput::run() {
    auto* device = static_cast<Device*>(streamDevice);
    const AudioStreamConfig config = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channels = 2,
    };
    AudioStreamHandle handle = nullptr;
    if (audio_stream_open_output(device, &config, &handle) != ERROR_NONE) {
        LOG_E(TAG, "Failed to open the audio output");
        handle = nullptr;
    }
    while (!stopRequested.load()) {
        if (handle == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        source->render(mono, FRAMES_PER_CHUNK);
        for (int i = 0; i < FRAMES_PER_CHUNK; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        size_t written = 0;
        // Blocks until the codec has room: this paces the task
        if (audio_stream_write(handle, stereo, sizeof(stereo), &written, pdMS_TO_TICKS(500)) != ERROR_NONE) {
            // The codec may have changed (e.g. headphones attached): reopen
            audio_stream_close(handle);
            handle = nullptr;
            if (audio_stream_open_output(device, &config, &handle) != ERROR_NONE) {
                handle = nullptr;
            }
        }
    }
    if (handle != nullptr) {
        audio_stream_close(handle);
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(stopped));
    vTaskSuspend(nullptr); // deleted by stop(), which also frees the stack
}

bool AudioOutput::start(AudioSource* audioSource, const char* taskName) {
    if (task != nullptr) {
        return true;
    }
    Device* found = nullptr;
    device_for_each_of_type(&AUDIO_STREAM_TYPE, &found, [](Device* device, void* context) -> bool {
        if (device_is_ready(device)) {
            *static_cast<Device**>(context) = device;
            return false;
        }
        return true;
    });
    if (found == nullptr) {
        LOG_I(TAG, "No audio output");
        return false;
    }
    streamDevice = found;
    source = audioSource;
    stopRequested.store(false);
    SemaphoreHandle_t semaphore = xSemaphoreCreateBinary();
    if (semaphore == nullptr) {
        return false;
    }
    stopped = semaphore;
    TaskHandle_t handle = nullptr;
#ifdef ESP_PLATFORM
    // Core 0, away from apps that render on core 1 (on dual-core chips). Internal RAM if there's room.
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, taskName, TASK_STACK_SIZE, this, TASK_PRIORITY, &handle, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(taskMain, taskName, TASK_STACK_SIZE, this, TASK_PRIORITY, &handle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        handle = nullptr;
    }
#else
    if (xTaskCreate(taskMain, taskName, TASK_STACK_SIZE, this, TASK_PRIORITY, &handle) != pdPASS) {
        handle = nullptr;
    }
#endif
    if (handle == nullptr) {
        LOG_E(TAG, "Failed to start the audio task");
        vSemaphoreDelete(semaphore);
        stopped = nullptr;
        return false;
    }
    task = handle;
    return true;
}

void AudioOutput::stop() {
    if (task == nullptr) {
        return;
    }
    stopRequested.store(true);
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(stopped), portMAX_DELAY);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(static_cast<TaskHandle_t>(task));
#else
    vTaskDelete(static_cast<TaskHandle_t>(task));
#endif
    task = nullptr;
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(stopped));
    stopped = nullptr;
}

} // namespace bleep
