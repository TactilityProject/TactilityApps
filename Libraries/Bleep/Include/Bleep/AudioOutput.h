#pragma once

#include "AudioSource.h"

#include <atomic>
#include <cstdint>

namespace bleep {

/**
 * Plays an AudioSource through the device's audio stream (Tactility), on a task of its own, as
 * 16-bit stereo at SAMPLE_RATE. Does nothing on devices without audio output.
 */
class AudioOutput {
public:
    /**
     * @param taskName for the audio task
     * @return false when there's no audio output (or the task couldn't start)
     */
    bool start(AudioSource* source, const char* taskName = "bleep_audio");
    /** Stops and joins the audio task. Safe to call when not started. */
    void stop();
    bool running() const { return task != nullptr; }

private:
    static constexpr int FRAMES_PER_CHUNK = 256;

    void* streamDevice = nullptr; // Device*
    AudioSource* source = nullptr;
    void* task = nullptr; // TaskHandle_t
    void* stopped = nullptr; // SemaphoreHandle_t
    std::atomic<bool> stopRequested { false };
    int16_t mono[FRAMES_PER_CHUNK] = {};
    int16_t stereo[FRAMES_PER_CHUNK * 2] = {};

    static void taskMain(void* self);
    void run();
};

} // namespace bleep
