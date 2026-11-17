// Record/playback loop on a dedicated task. shape: the audio task
// never touches lv_obj, it only publishes state the poll timer reads.

#include "AudioRecorder.h"

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t CHANNELS = 1;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr size_t AUDIO_BUFFER_SIZE = 4096;

#pragma pack(push, 1)
struct WavHeader {
    char riff_id[4];        // "RIFF"
    uint32_t riff_size;     // Total file size - 8
    char wave_id[4];        // "WAVE"
    char fmt_id[4];         // "fmt "
    uint32_t fmt_size;      // 16 for linear PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;     // sample_rate * num_channels * bits/8
    uint16_t block_align;   // num_channels * bits/8
    uint16_t bits_per_samp;
    char data_id[4];        // "data"
    uint32_t data_size;     // Size of the PCM audio data
};
#pragma pack(pop)

Device* findAudioStreamDevice() {
    Device* result = nullptr;
    device_for_each_of_type(&AUDIO_STREAM_TYPE, &result, [](Device* device, void* ctx) -> bool {
        if (device_is_ready(device)) {
            *static_cast<Device**>(ctx) = device;
            return false;
        }
        return true;
    });
    return result;
}

void buildRecordingFilename(const std::string& dir, char* out, size_t outLen) {
    struct tm tmInfo {};
    time_t now = time(nullptr);
    localtime_r(&now, &tmInfo);

    if (tmInfo.tm_year < 120) { // Year before 2020 (RTC not initialized)
        snprintf(out, outLen, "%s/REC_%08u.wav", dir.c_str(), (unsigned)now);
    } else {
        snprintf(out, outLen, "%s/REC_%04d%02d%02d_%02d%02d%02d.wav", dir.c_str(), tmInfo.tm_year + 1900,
                 tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    }
}

void publishActivePath(RecorderAudioState* audio, const std::string& path) {
    std::lock_guard lg(audio->pathMutex);
    audio->activeFilepath = path;
}

void doRecord(RecorderAudioState* audio, const std::string& filepath) {
    LOG_I(TAG, "Starting recording to: %s", filepath.c_str());

    FILE* fp = fopen(filepath.c_str(), "wb");
    if (fp == nullptr) {
        LOG_E(TAG, "Failed to create audio file: %s", filepath.c_str());
        audio->state.store(RecorderState::Idle);
        return;
    }

    WavHeader header{};
    memcpy(header.riff_id, "RIFF", 4);
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;
    header.num_channels = CHANNELS;
    header.sample_rate = SAMPLE_RATE;
    header.bits_per_samp = BITS_PER_SAMPLE;
    header.byte_rate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    header.block_align = CHANNELS * (BITS_PER_SAMPLE / 8);
    memcpy(header.data_id, "data", 4);
    header.data_size = 0;
    header.riff_size = sizeof(WavHeader) - 8;
    fwrite(&header, 1, sizeof(header), fp);

    uint32_t totalPcmBytes = 0;

    Device* streamDevice = findAudioStreamDevice();
    AudioStreamHandle handle = nullptr;
    if (streamDevice == nullptr) {
        LOG_E(TAG, "No audio-stream device found");
    } else {
        AudioStreamConfig cfg{};
        cfg.sample_rate = SAMPLE_RATE;
        cfg.bits_per_sample = BITS_PER_SAMPLE;
        cfg.channels = CHANNELS;
        error_t oerr = audio_stream_open_input(streamDevice, &cfg, &handle);
        if (oerr != ERROR_NONE) {
            LOG_E(TAG, "audio_stream_open_input failed: %d", oerr);
            handle = nullptr;
        }
    }

    if (handle != nullptr) {
        std::vector<uint8_t> recBuf(AUDIO_BUFFER_SIZE);
        const uint32_t maxBytes = RECORDER_MAX_SECONDS * header.byte_rate;

        while (!audio->stopRequested.load() && totalPcmBytes < maxBytes) {
            size_t bytesRead = 0;
            error_t rerr = audio_stream_read(handle, recBuf.data(), recBuf.size(), &bytesRead, pdMS_TO_TICKS(250));
            if (rerr == ERROR_NONE && bytesRead > 0) {
                size_t written = fwrite(recBuf.data(), 1, bytesRead, fp);
                totalPcmBytes += written;
                audio->currentTimeSec.store(totalPcmBytes / header.byte_rate);
                if (written < bytesRead) {
                    LOG_E(TAG, "Short write (%zu of %zu bytes), stopping recording", written,
                          bytesRead);
                    break;
                }
            } else if (rerr != ERROR_TIMEOUT) {
                LOG_W(TAG, "audio_stream_read returned: %d", rerr);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        audio_stream_close(handle);
    }

    header.data_size = totalPcmBytes;
    header.riff_size = sizeof(WavHeader) - 8 + totalPcmBytes;
    fseek(fp, 0, SEEK_SET);
    fwrite(&header, 1, sizeof(header), fp);
    fclose(fp);

    LOG_I(TAG, "Recording finished: %u bytes (%u sec)", (unsigned)totalPcmBytes,
          (unsigned)(header.byte_rate > 0 ? totalPcmBytes / header.byte_rate : 0));

    audio->currentTimeSec.store(header.byte_rate > 0 ? totalPcmBytes / header.byte_rate : 0);
    audio->totalTimeSec.store(audio->currentTimeSec.load());
    audio->recordingFinished.store(true);
}

void doPlayback(RecorderAudioState* audio, const std::string& filepath) {
    LOG_I(TAG, "Starting playback of: %s", filepath.c_str());

    FILE* fp = fopen(filepath.c_str(), "rb");
    if (fp == nullptr) {
        LOG_E(TAG, "Failed to open file for playback: %s", filepath.c_str());
        audio->state.store(RecorderState::Idle);
        return;
    }

    WavHeader header{};
    bool validWav = false;
    if (fread(&header, 1, sizeof(header), fp) == sizeof(header)) {
        if (memcmp(header.riff_id, "RIFF", 4) == 0 && memcmp(header.wave_id, "WAVE", 4) == 0 &&
            memcmp(header.fmt_id, "fmt ", 4) == 0) {
            validWav = true;
        }
    }

    uint32_t sampleRate = SAMPLE_RATE;
    uint16_t channels = CHANNELS;
    uint16_t bits = BITS_PER_SAMPLE;
    uint32_t dataBytes = 0;

    if (validWav) {
        sampleRate = header.sample_rate;
        channels = header.num_channels;
        bits = header.bits_per_samp;
        dataBytes = header.data_size;
        clearerr(fp);
    } else {
        clearerr(fp);
        if (fseek(fp, 0, SEEK_END) == 0) {
            long sz = ftell(fp);
            dataBytes = (sz > 0) ? (uint32_t)sz : 0;
            fseek(fp, 0, SEEK_SET);
        }
        clearerr(fp);
        LOG_W(TAG, "WAV header missing, assuming raw PCM: %u bytes", (unsigned)dataBytes);
    }

    uint32_t byteRate = sampleRate * channels * (bits / 8);
    if (byteRate == 0) {
        byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    }

    audio->totalTimeSec.store(dataBytes / byteRate);
    audio->currentTimeSec.store(0);

    uint32_t bytesPlayed = 0;

    Device* streamDevice = findAudioStreamDevice();
    AudioStreamHandle handle = nullptr;
    if (streamDevice == nullptr) {
        LOG_E(TAG, "No audio-stream device found");
    } else {
        AudioStreamConfig cfg{};
        cfg.sample_rate = sampleRate;
        cfg.bits_per_sample = static_cast<uint8_t>(bits);
        cfg.channels = static_cast<uint8_t>(channels);
        error_t oerr = audio_stream_open_output(streamDevice, &cfg, &handle);
        if (oerr != ERROR_NONE) {
            LOG_E(TAG, "audio_stream_open_output failed: %d", oerr);
            handle = nullptr;
        }
    }

    if (handle != nullptr) {
        std::vector<uint8_t> playBuf(AUDIO_BUFFER_SIZE);

        while (!audio->stopRequested.load() && (dataBytes == 0 || bytesPlayed < dataBytes)) {
            if (audio->pauseRequested.load()) {
                audio->state.store(RecorderState::Paused);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            if (audio->state.load() == RecorderState::Paused) {
                audio->state.store(RecorderState::Playing);
            }

            if (feof(fp) || ferror(fp)) {
                break;
            }

            size_t toRead = playBuf.size();
            if (dataBytes > 0 && (dataBytes - bytesPlayed) < toRead) {
                toRead = dataBytes - bytesPlayed;
            }

            size_t bytesRead = fread(playBuf.data(), 1, toRead, fp);
            if (bytesRead == 0) {
                break;
            }

            size_t bytesWritten = 0;
            error_t werr = audio_stream_write(handle, playBuf.data(), bytesRead, &bytesWritten, pdMS_TO_TICKS(250));
            if (werr == ERROR_NONE) {
                bytesPlayed += bytesRead;
                audio->currentTimeSec.store(bytesPlayed / byteRate);
            } else if (werr != ERROR_TIMEOUT) {
                LOG_W(TAG, "audio_stream_write failed: %d", werr);
                break;
            }
        }

        audio_stream_close(handle);
    }

    fclose(fp);
    LOG_I(TAG, "Playback finished: %u bytes played", (unsigned)bytesPlayed);
}

} // namespace

void recorderScanDirectory(Context* ctx) {
    std::vector<RecordingItem> scanned;

    // A recording that's still being written is a partial WAV file: leave it out of the
    // list until it's finished, so it can't be played or deleted mid-recording.
    std::string activePath;
    {
        std::lock_guard lg(ctx->audio.pathMutex);
        activePath = ctx->audio.activeFilepath;
    }
    const bool recording = ctx->audio.state.load() == RecorderState::Recording;

    DIR* d = opendir(ctx->appDataDir.c_str());
    if (d == nullptr) {
        mkdir(ctx->appDataDir.c_str(), 0755);
        d = opendir(ctx->appDataDir.c_str());
    }

    if (d != nullptr) {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            const char* dot = strrchr(entry->d_name, '.');
            if (dot != nullptr && strcasecmp(dot, ".wav") == 0) {
                std::string fullpath = ctx->appDataDir + "/" + entry->d_name;
                if (recording && fullpath == activePath) {
                    continue;
                }
                struct stat st{};
                RecordingItem item;
                item.filename = entry->d_name;
                item.fullpath = fullpath;
                if (stat(fullpath.c_str(), &st) == 0) {
                    item.sizeBytes = st.st_size;
                    item.mtime = st.st_mtime;
                }
                scanned.push_back(item);
            }
        }
        closedir(d);
    }

    std::sort(scanned.begin(), scanned.end(), [](const RecordingItem& a, const RecordingItem& b) {
        if (a.mtime != b.mtime) {
            return a.mtime > b.mtime;
        }
        return a.filename > b.filename;
    });

    std::lock_guard lg(ctx->recordingsMutex);
    ctx->recordings = std::move(scanned);
}

void recorderTaskEntry(void* param) {
    auto* audio = static_cast<RecorderAudioState*>(param);

    while (!audio->shutdown.load()) {
        auto request = audio->pending.exchange(RecorderRequest::None);

        switch (request) {
            case RecorderRequest::StartRecording: {
                char filepath[256];
                std::string dir;
                {
                    std::lock_guard lg(audio->pathMutex);
                    dir = audio->recordDir;
                }
                buildRecordingFilename(dir, filepath, sizeof(filepath));
                publishActivePath(audio, filepath);
                audio->stopRequested.store(false);
                audio->pauseRequested.store(false);
                audio->currentTimeSec.store(0);
                audio->totalTimeSec.store(RECORDER_MAX_SECONDS);
                audio->state.store(RecorderState::Recording);
                doRecord(audio, filepath);
                audio->state.store(RecorderState::Idle);
                break;
            }
            case RecorderRequest::StartPlayback: {
                std::string path;
                {
                    std::lock_guard lg(audio->pathMutex);
                    path = audio->requestedPlaybackPath;
                }
                publishActivePath(audio, path);
                audio->stopRequested.store(false);
                audio->pauseRequested.store(false);
                audio->state.store(RecorderState::Playing);
                doPlayback(audio, path);
                audio->state.store(RecorderState::Idle);
                break;
            }
            case RecorderRequest::None:
            default:
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
        }
    }

    audio->task = nullptr;
    vTaskDelete(nullptr);
}
