#include "MusicPlayer.h"

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/esp_audio_simple_dec.h>
#include <tactility/log.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t INPUT_BUFFER_SIZE = 8192;
constexpr size_t OUTPUT_BUFFER_SIZE = 16384;
constexpr uint16_t PLAYBACK_TASK_STACK_BYTES = 8192;
constexpr uint8_t PLAYBACK_TASK_PRIORITY = 8;
constexpr char PLAYBACK_TASK_NAME[] = "music-player";

void setStatus(MusicPlayerContext* context, const std::string& status) {
    std::lock_guard lock(context->statusMutex);
    context->statusText = status;
}

bool getTrack(MusicPlayerContext* context, int32_t index, MusicTrack* output) {
    std::lock_guard lock(context->tracksMutex);
    if (index < 0 || static_cast<size_t>(index) >= context->tracks.size()) {
        return false;
    }
    *output = context->tracks[static_cast<size_t>(index)];
    return true;
}

bool shouldStop(const MusicPlayerContext* context) {
    return context->shutdown.load() || context->stopRequested.load();
}

bool waitWhilePaused(MusicPlayerContext* context) {
    while (context->pauseRequested.load() && !shouldStop(context)) {
        context->playbackState.store(MusicPlaybackState::Paused);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    if (!shouldStop(context)) {
        context->playbackState.store(MusicPlaybackState::Playing);
    }
    return !shouldStop(context);
}

bool writeDecoded(AudioStreamHandle stream, const uint8_t* data, size_t length, MusicPlayerContext* context,
                  uint32_t byteRate, uint64_t* bytesPlayed) {
    size_t offset = 0;
    uint8_t consecutiveTimeouts = 0;
    while (offset < length && waitWhilePaused(context)) {
        size_t bytesWritten = 0;
        const error_t error = audio_stream_write(stream, data + offset, length - offset, &bytesWritten, pdMS_TO_TICKS(250));
        if (error == ERROR_TIMEOUT) {
            if (++consecutiveTimeouts >= 20) {
                LOG_E(MUSIC_PLAYER_TAG, "Audio output kept timing out");
                setStatus(context, "Audio output timed out");
                return false;
            }
            continue;
        }
        if (error != ERROR_NONE || bytesWritten == 0) {
            LOG_E(MUSIC_PLAYER_TAG, "Audio output write failed: %d", error);
            setStatus(context, "Audio output write failed");
            return false;
        }
        consecutiveTimeouts = 0;
        offset += bytesWritten;
        *bytesPlayed += bytesWritten;
        context->currentTimeSeconds.store(byteRate == 0 ? 0 : static_cast<uint32_t>(*bytesPlayed / byteRate));
    }
    return offset == length;
}

bool playTrack(MusicPlayerContext* context, const MusicTrack& track) {
    FILE* file = fopen(track.path.c_str(), "rb");
    if (file == nullptr) {
        LOG_E(MUSIC_PLAYER_TAG, "Cannot open %s", track.path.c_str());
        setStatus(context, "Cannot open track");
        return false;
    }

    {
        std::lock_guard lock(context->outputMutex);
        if (context->selectedOutputName.empty()) {
            fclose(file);
            setStatus(context, "Select an audio output");
            return false;
        }
    }

    std::string outputName;
    {
        std::lock_guard lock(context->outputMutex);
        outputName = context->selectedOutputName;
    }

    Device* outputDevice = nullptr;
    if (device_get_by_name(outputName.c_str(), &outputDevice) != ERROR_NONE || outputDevice == nullptr) {
        fclose(file);
        setStatus(context, "Audio output is unavailable");
        return false;
    }

    esp_audio_simple_dec_cfg_t decoderConfig {};
    decoderConfig.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    decoderConfig.use_frame_dec = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t decoderError = esp_audio_simple_dec_open(&decoderConfig, &decoder);
    if (decoderError != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        device_put(outputDevice);
        fclose(file);
        LOG_E(MUSIC_PLAYER_TAG, "MP3 decoder open failed: %d", decoderError);
        setStatus(context, "MP3 decoder unavailable");
        return false;
    }

    std::vector<uint8_t> input(INPUT_BUFFER_SIZE);
    std::vector<uint8_t> decoded(OUTPUT_BUFFER_SIZE);
    size_t inputLength = 0;
    bool endOfFile = false;
    bool success = true;
    uint64_t bytesPlayed = 0;
    uint32_t byteRate = 0;
    AudioStreamHandle stream = nullptr;

    setStatus(context, "Playing");
    while (!shouldStop(context) && success) {
        if (!waitWhilePaused(context)) {
            break;
        }

        if (inputLength == 0 && !endOfFile) {
            const size_t bytesRead = fread(input.data(), 1, input.size(), file);
            inputLength = bytesRead;
            endOfFile = bytesRead < input.size();
            if (bytesRead == 0 && ferror(file)) {
                LOG_E(MUSIC_PLAYER_TAG, "Read failed for %s", track.path.c_str());
                setStatus(context, "Track read failed");
                success = false;
                break;
            }
        }

        esp_audio_simple_dec_raw_t raw {};
        raw.buffer = input.data();
        raw.len = static_cast<uint32_t>(inputLength);
        raw.eos = endOfFile;
        esp_audio_simple_dec_out_t frame {};
        frame.buffer = decoded.data();
        frame.len = static_cast<uint32_t>(decoded.size());

        const esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &frame);
        if (result != ESP_AUDIO_ERR_OK && result != ESP_AUDIO_ERR_CONTINUE && result != ESP_AUDIO_ERR_DATA_LACK) {
            LOG_E(MUSIC_PLAYER_TAG, "MP3 decode failed: %d", result);
            setStatus(context, "MP3 decode failed");
            success = false;
            break;
        }
        if (raw.consumed > inputLength) {
            LOG_E(MUSIC_PLAYER_TAG, "MP3 decoder consumed beyond input");
            setStatus(context, "MP3 decode failed");
            success = false;
            break;
        }
        if (raw.consumed > 0) {
            inputLength -= raw.consumed;
            if (inputLength > 0) {
                memmove(input.data(), input.data() + raw.consumed, inputLength);
            }
        }

        if (frame.decoded_size > decoded.size()) {
            LOG_E(MUSIC_PLAYER_TAG, "MP3 decoder returned an invalid output size");
            setStatus(context, "MP3 decode failed");
            success = false;
            break;
        }
        if (frame.decoded_size > 0) {
            esp_audio_simple_dec_info_t info {};
            if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK || info.sample_rate == 0 ||
                info.channel == 0 || info.bits_per_sample == 0) {
                setStatus(context, "Could not read MP3 format");
                success = false;
                break;
            }
            if (stream == nullptr) {
                AudioStreamConfig config {};
                config.sample_rate = info.sample_rate;
                config.bits_per_sample = info.bits_per_sample;
                config.channels = info.channel;
                const error_t openError = audio_stream_open_output(outputDevice, &config, &stream);
                if (openError != ERROR_NONE) {
                    LOG_E(MUSIC_PLAYER_TAG, "Audio output open failed: %d", openError);
                    setStatus(context, "Cannot open audio output");
                    success = false;
                    break;
                }
                byteRate = info.sample_rate * info.channel * (info.bits_per_sample / 8);
            }
            if (byteRate == 0 ||
                !writeDecoded(stream, decoded.data(), frame.decoded_size, context, byteRate, &bytesPlayed)) {
                success = false;
                break;
            }
        } else if (raw.consumed == 0 && inputLength == input.size() && !endOfFile) {
            LOG_E(MUSIC_PLAYER_TAG, "MP3 decoder cannot consume input buffer");
            setStatus(context, "MP3 decode stalled");
            success = false;
            break;
        } else if (raw.consumed == 0 && frame.decoded_size == 0 && !endOfFile) {
            const size_t bytesRead = fread(input.data() + inputLength, 1, input.size() - inputLength, file);
            inputLength += bytesRead;
            endOfFile = bytesRead == 0 || inputLength < input.size();
            if (bytesRead == 0 && ferror(file)) {
                LOG_E(MUSIC_PLAYER_TAG, "Read failed for %s", track.path.c_str());
                setStatus(context, "Track read failed");
                success = false;
                break;
            }
        } else if (raw.consumed == 0 && frame.decoded_size == 0 && endOfFile) {
            break;
        }
    }

    if (stream != nullptr) {
        const error_t closeError = audio_stream_close(stream);
        if (closeError != ERROR_NONE) {
            LOG_W(MUSIC_PLAYER_TAG, "Audio output close failed: %d", closeError);
        }
    }
    esp_audio_simple_dec_close(decoder);
    device_put(outputDevice);
    fclose(file);
    return success && !shouldStop(context);
}

void consumePendingStart(MusicPlayerContext* context, int32_t* trackIndex) {
    std::lock_guard lock(context->controlMutex);
    if (context->startPending) {
        *trackIndex = context->requestedTrackIndex;
        context->requestedTrackIndex = -1;
        context->startPending = false;
    }
}

} // namespace

void musicPlayerRequestStart(MusicPlayerContext* context, int32_t trackIndex) {
    {
        std::lock_guard lock(context->tracksMutex);
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= context->tracks.size()) {
            return;
        }
    }
    {
        std::lock_guard lock(context->controlMutex);
        context->requestedTrackIndex = trackIndex;
        context->startPending = true;
    }
    context->pauseRequested.store(false);
    context->stopRequested.store(true);
}

void musicPlayerRequestStop(MusicPlayerContext* context) {
    {
        std::lock_guard lock(context->controlMutex);
        context->startPending = false;
        context->requestedTrackIndex = -1;
    }
    context->pauseRequested.store(false);
    context->stopRequested.store(true);
}

void musicPlayerTaskEntry(void* param) {
    auto* context = static_cast<MusicPlayerContext*>(param);

    while (!context->shutdown.load()) {
        int32_t requestedIndex = -1;
        consumePendingStart(context, &requestedIndex);
        if (requestedIndex < 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        MusicTrack track;
        if (!getTrack(context, requestedIndex, &track)) {
            context->playbackState.store(MusicPlaybackState::Idle);
            setStatus(context, "Track is no longer available");
            continue;
        }

        context->stopRequested.store(false);
        context->pauseRequested.store(false);
        context->currentTrackIndex.store(requestedIndex);
        context->currentTimeSeconds.store(0);
        context->totalTimeSeconds.store(track.durationSeconds);
        context->playbackState.store(MusicPlaybackState::Opening);
        const bool finishedNormally = playTrack(context, track);
        context->playbackState.store(MusicPlaybackState::Idle);
        if (finishedNormally) {
            context->currentTimeSeconds.store(track.durationSeconds);
        } else if (context->stopRequested.load()) {
            context->currentTimeSeconds.store(0);
        }

        if (finishedNormally && !context->stopRequested.load() && !context->shutdown.load()) {
            int32_t nextIndex = -1;
            {
                std::lock_guard lock(context->tracksMutex);
                if (static_cast<size_t>(requestedIndex + 1) < context->tracks.size()) {
                    nextIndex = requestedIndex + 1;
                }
            }
            if (nextIndex >= 0) {
                musicPlayerRequestStart(context, nextIndex);
            } else {
                setStatus(context, "Finished");
            }
        } else if (context->stopRequested.load() && !context->shutdown.load()) {
            setStatus(context, "Stopped");
        }
    }

    context->playbackState.store(MusicPlaybackState::Idle);
    context->task.store(nullptr);
    vTaskDelete(nullptr);
}

bool musicPlayerStartTask(MusicPlayerContext* context) {
    TaskHandle_t task = nullptr;
    if (xTaskCreate(musicPlayerTaskEntry, PLAYBACK_TASK_NAME, PLAYBACK_TASK_STACK_BYTES, context, PLAYBACK_TASK_PRIORITY,
                    &task) != pdPASS) {
        return false;
    }
    context->task.store(task);
    return true;
}
