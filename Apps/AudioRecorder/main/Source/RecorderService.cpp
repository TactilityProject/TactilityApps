// Record/playback loop on a dedicated task. shape: the audio task
// never touches lv_obj, it only publishes state the poll timer reads.

#include "AudioRecorder.h"

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/**
 * Reserves a unique pair of paths for a new recording: the final .wav and the .part file
 * that is written first. The .part extension keeps the unfinished file out of the list, and
 * the suffix keeps two recordings made in the same second from overwriting each other.
 */
bool reserveRecordingFiles(const std::string& dir, char* finalPath, size_t finalLen, char* tempPath, size_t tempLen) {
    char base[224];
    struct tm tmInfo {};
    time_t now = time(nullptr);
    localtime_r(&now, &tmInfo);

    if (tmInfo.tm_year < 120) { // Year before 2020 (RTC not initialized)
        snprintf(base, sizeof(base), "%s/REC_%08u.wav", dir.c_str(), (unsigned)now);
    } else {
        snprintf(base, sizeof(base), "%s/REC_%04d%02d%02d_%02d%02d%02d.wav", dir.c_str(), tmInfo.tm_year + 1900,
                 tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    }

    const size_t baseLen = strlen(base);
    if (baseLen < 5) {
        return false;
    }

    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            snprintf(finalPath, finalLen, "%s", base);
        } else {
            snprintf(finalPath, finalLen, "%.*s_%d.wav", (int)(baseLen - 4), base, suffix);
        }
        if (access(finalPath, F_OK) == 0) {
            continue;
        }

        snprintf(tempPath, tempLen, "%s.part", finalPath);
        int fd = open(tempPath, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            close(fd);
            return true;
        }
        if (errno != EEXIST) {
            LOG_E(TAG, "Failed to reserve %s: errno %d", tempPath, errno);
            return false;
        }
    }

    LOG_E(TAG, "Failed to find a unique recording filename");
    return false;
}

void publishActivePath(RecorderAudioState* audio, const std::string& path) {
    std::lock_guard lg(audio->pathMutex);
    audio->activeFilepath = path;
}

/** Publishes the result and its finalized path together, so the poll can consume a stable pair */
void publishRecordingResult(RecorderAudioState* audio, RecordingResult result, const std::string& path) {
    std::lock_guard lg(audio->pathMutex);
    audio->recordingResult = result;
    audio->completedRecordingPath = path;
}

void doRecord(RecorderAudioState* audio, const std::string& tempFilepath, const std::string& finalFilepath) {
    LOG_I(TAG, "Starting recording to: %s", finalFilepath.c_str());

    bool success = true;
    FILE* fp = fopen(tempFilepath.c_str(), "wb");
    if (fp == nullptr) {
        LOG_E(TAG, "Failed to create audio file: %s", tempFilepath.c_str());
        unlink(tempFilepath.c_str());
        publishRecordingResult(audio, RecordingResult::Failure, "");
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
    if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
        LOG_E(TAG, "Failed to write WAV header");
        success = false;
    }

    uint32_t totalPcmBytes = 0;

    Device* streamDevice = findAudioStreamDevice();
    AudioStreamHandle handle = nullptr;
    if (streamDevice == nullptr) {
        LOG_E(TAG, "No audio-stream device found");
        success = false;
    } else {
        AudioStreamConfig cfg{};
        cfg.sample_rate = SAMPLE_RATE;
        cfg.bits_per_sample = BITS_PER_SAMPLE;
        cfg.channels = CHANNELS;
        error_t oerr = audio_stream_open_input(streamDevice, &cfg, &handle);
        if (oerr != ERROR_NONE) {
            LOG_E(TAG, "audio_stream_open_input failed: %d", oerr);
            success = false;
        }
    }

    if (handle != nullptr) {
        std::vector<uint8_t> recBuf(AUDIO_BUFFER_SIZE);
        const uint32_t maxBytes = RECORDER_MAX_SECONDS * header.byte_rate;

        while (success && !audio->stopRequested.load() && !audio->shutdown.load() && totalPcmBytes < maxBytes) {
            size_t bytesRead = 0;
            error_t rerr = audio_stream_read(handle, recBuf.data(), recBuf.size(), &bytesRead, pdMS_TO_TICKS(250));
            if (rerr == ERROR_NONE && bytesRead > 0) {
                size_t written = fwrite(recBuf.data(), 1, bytesRead, fp);
                if (written < bytesRead) {
                    LOG_E(TAG, "Short write (%zu of %zu bytes), stopping recording", written, bytesRead);
                    success = false;
                    break;
                }
                totalPcmBytes += written;
                audio->currentTimeSec.store(totalPcmBytes / header.byte_rate);
            } else if (rerr != ERROR_TIMEOUT) {
                LOG_W(TAG, "audio_stream_read returned: %d", rerr);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        audio_stream_close(handle);
    }

    if (totalPcmBytes == 0) {
        LOG_W(TAG, "No audio data was recorded");
        success = false;
    }

    header.data_size = totalPcmBytes;
    header.riff_size = sizeof(WavHeader) - 8 + totalPcmBytes;
    if (success) {
        if (fseek(fp, 0, SEEK_SET) != 0) {
            success = false;
        } else if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
            success = false;
        }
    }
    if (fclose(fp) != 0) {
        LOG_E(TAG, "Failed to close the recording file");
        success = false;
    }

    if (success && rename(tempFilepath.c_str(), finalFilepath.c_str()) != 0) {
        LOG_E(TAG, "Failed to finalize recording %s: errno %d", finalFilepath.c_str(), errno);
        success = false;
    }
    if (!success) {
        unlink(tempFilepath.c_str());
    }

    audio->currentTimeSec.store(header.byte_rate > 0 ? totalPcmBytes / header.byte_rate : 0);
    audio->totalTimeSec.store(audio->currentTimeSec.load());
    if (success) {
        publishRecordingResult(audio, RecordingResult::Success, finalFilepath);
    } else {
        publishRecordingResult(audio, RecordingResult::Failure, "");
    }

    LOG_I(TAG, "Recording %s: %u bytes (%u sec)", success ? "finished" : "failed", (unsigned)totalPcmBytes,
          (unsigned)audio->currentTimeSec.load());
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

        while (!audio->stopRequested.load() && !audio->shutdown.load() && (dataBytes == 0 || bytesPlayed < dataBytes)) {
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

            // The stream may accept only part of a chunk: send the remainder before
            // reading more, and advance the progress by the accepted bytes only.
            size_t offset = 0;
            int timeouts = 0;
            while (offset < bytesRead && !audio->stopRequested.load() && !audio->shutdown.load()) {
                size_t bytesWritten = 0;
                error_t werr = audio_stream_write(handle, playBuf.data() + offset, bytesRead - offset, &bytesWritten,
                                                  pdMS_TO_TICKS(250));
                if (werr == ERROR_NONE) {
                    if (bytesWritten == 0) {
                        break;
                    }
                    offset += bytesWritten;
                    bytesPlayed += bytesWritten;
                    audio->currentTimeSec.store(bytesPlayed / byteRate);
                    timeouts = 0;
                } else if (werr == ERROR_TIMEOUT) {
                    if (++timeouts > 20) {
                        LOG_W(TAG, "audio_stream_write kept timing out");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    LOG_W(TAG, "audio_stream_write failed: %d", werr);
                    break;
                }
            }
            if (offset < bytesRead) {
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

    DIR* d = opendir(ctx->appDataDir.c_str());
    if (d == nullptr) {
        mkdir(ctx->appDataDir.c_str(), 0755);
        d = opendir(ctx->appDataDir.c_str());
    }

    if (d != nullptr) {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            const char* dot = strrchr(entry->d_name, '.');
            // Only finalized files have the .wav extension: the task writes a .wav.part file
            // and renames it after the header has been finalized.
            if (dot != nullptr && strcasecmp(dot, ".wav") == 0) {
                std::string fullpath = ctx->appDataDir + "/" + entry->d_name;
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
                if (audio->shutdown.load()) {
                    break;
                }
                std::string dir;
                {
                    std::lock_guard lg(audio->pathMutex);
                    dir = audio->recordDir;
                }

                char finalPath[256];
                char tempPath[260];
                if (!reserveRecordingFiles(dir, finalPath, sizeof(finalPath), tempPath, sizeof(tempPath))) {
                    publishRecordingResult(audio, RecordingResult::Failure, "");
                    audio->state.store(RecorderState::Idle);
                    break;
                }

                publishActivePath(audio, finalPath);
                audio->stopRequested.store(false);
                audio->pauseRequested.store(false);
                audio->currentTimeSec.store(0);
                audio->totalTimeSec.store(RECORDER_MAX_SECONDS);
                audio->state.store(RecorderState::Recording);
                doRecord(audio, tempPath, finalPath);
                audio->state.store(RecorderState::Idle);
                break;
            }
            case RecorderRequest::StartPlayback: {
                if (audio->shutdown.load()) {
                    break;
                }
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

    audio->task.store(nullptr);
    vTaskDelete(nullptr);
}
