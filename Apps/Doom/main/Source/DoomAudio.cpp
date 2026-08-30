#include "DoomAudio.h"

extern "C" {
#include "i_sound.h"
#include "opl.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"
}

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

constexpr auto* TAG = "DoomAudio";

constexpr int NUM_CHANNELS = 16;

// 512 frames per buffer. Smaller buffers were tried (256) on the theory that
// finer granularity would interleave better with the game task; it does not
// help, because the cost ratio per second of audio is identical either way -
// and the larger buffer leaves more slack queued in the DMA to absorb a late
// render, which matters because OPL synthesis runs close to the real-time
// limit on this chip.
constexpr int MIXBUFFER_SAMPLES = 512;

// The stream runs at 11025 Hz, matching DOOM's sound lumps exactly.
//
// 22050 was tried first, on the theory that OPL needs the extra bandwidth. It
// does not pay off here: the emulated chip always synthesises at its native
// 49716 Hz regardless of the output rate, so halving the output rate costs
// nothing in synthesis while halving the resampling and stream-write work.
// Profiling showed peaks of ~27600 us/buf at 22050 against a 23219 us budget
// (i.e. over budget); 11025 doubles the budget to ~46438 us for the same
// synthesis cost.
//
// Matching the lump rate also means sound effects are 1:1 again, so no
// resampling is needed on the SFX path.
constexpr uint32_t STREAM_SAMPLE_RATE = 11025;
constexpr uint32_t SFX_SAMPLE_RATE = 11025;
constexpr uint32_t SFX_UPSAMPLE = STREAM_SAMPLE_RATE / SFX_SAMPLE_RATE;

// The sample-and-hold upsampler in the mixer only works for whole-number ratios;
// the divide above would otherwise floor and detune every effect rather than
// failing visibly. Currently 1:1, so the phase accumulator is a no-op - it is
// kept so changing STREAM_SAMPLE_RATE stays a one-line change.
static_assert(STREAM_SAMPLE_RATE % SFX_SAMPLE_RATE == 0,
              "STREAM_SAMPLE_RATE must be a whole multiple of SFX_SAMPLE_RATE");

// Music make-up gain, as a fraction (see the mixer loop for the rationale).
//
// Full-scale value of snd_SfxVolume, which is what S_StartSound() passes down
// as the per-channel volume. DOOM's menu slider spans 0-15 and d_main.c scales
// it by 8, so the effective range here is 0-120 out of this 127.
constexpr int32_t SFX_VOLUME_MAX = 127;

// Headroom scale applied to the 8-bit sound lump before the volume divide.
// Sized against the music level so that the two fit together: with music at
// unity, one effect peaks around 4079 and three overlapping ones plus a dense
// music passage stay just inside full scale (~29200 of 32767). Raising this
// makes effects louder but starts clipping when several fire at once.
constexpr int32_t SFX_SCALE = 128;

// Music make-up gain, applied on top of the menu's "Music Volume" setting.
//
// Unity: earlier builds boosted here to compensate for DMX's attenuation, but
// that was masking the sfx volume divisor bug below - effects were being
// amplified ~8x, so music had to be pushed up to compete. With the divisor
// fixed, both sit at their natural levels and the menu sliders do the work.
constexpr int32_t MUSIC_GAIN_NUM = 1;
constexpr int32_t MUSIC_GAIN_DEN = 1;

struct AudioChannel {
    const uint8_t* data = nullptr;
    uint32_t length = 0;
    uint32_t pos = 0;       // index into the 11025 Hz lump
    uint32_t phase = 0;     // output frames emitted for the current lump sample
    int vol = 0;
    int sep = 0;
    bool playing = false;
};

AudioChannel channels[NUM_CHANNELS];

// Guards channels[]. The engine calls Sound_StartSound/UpdateSoundParams/
// StopSound from the doom task on core 1, while mixerTask reads and advances
// the same entries on core 0. Without this, the mixer could see a half-updated
// channel - e.g. a new data pointer with the previous sound's length - and read
// past the end of a lump.
//
// A FreeRTOS mutex rather than a portMUX spinlock: portENTER_CRITICAL expands to
// vPortEnterCriticalMultiCore on this dual-core target, which the firmware's ELF
// loader does not export (only the single-core vPortEnterCritical is), so the
// app would fail to load. The mutex is built from xQueueCreateMutex /
// xQueueSemaphoreTake / xQueueGenericSend, all of which are already exported.
//
// The held sections are a handful of stores, so contention is negligible.
SemaphoreHandle_t channelsMutex = nullptr;

// Never blocks indefinitely: a failed take skips the update rather than stalling
// the mixer or the game loop. Returns false if the lock could not be taken.
inline bool lockChannels() {
    return channelsMutex != nullptr &&
           xSemaphoreTake(channelsMutex, pdMS_TO_TICKS(10)) == pdTRUE;
}

inline void unlockChannels() {
    xSemaphoreGive(channelsMutex);
}

Device* streamDevice = nullptr;
TaskHandle_t mixerTaskHandle = nullptr;
std::atomic<bool> stopRequested {false};
SemaphoreHandle_t stoppedSem = nullptr;

void mixerTask(void* /*arg*/) {
    static int16_t outBuffer[MIXBUFFER_SAMPLES * 2]; // stereo

    AudioStreamConfig config = {
        .sample_rate = STREAM_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channels = 2,
    };

    AudioStreamHandle outputHandle = nullptr;
    error_t error = audio_stream_open_output(streamDevice, &config, &outputHandle);
    if (error != ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to open output stream: %s", error_to_string(error));
        xSemaphoreGive(stoppedSem);
        vTaskDelete(nullptr);
        return;
    }

    uint32_t yieldCounter = 0;

    while (!stopRequested.load()) {
        // Music forms the base layer: DG_OPL_Render() fills the whole buffer
        // (writing silence when nothing is playing), so no memset is needed.
        // It also advances the MIDI timer queue by exactly MIXBUFFER_SAMPLES
        // frames, which is what keeps tempo locked to the output stream.
        DG_OPL_Render(outBuffer, MIXBUFFER_SAMPLES, STREAM_SAMPLE_RATE);

        // Optional music make-up gain. Compiled out entirely at unity, which is
        // the current setting - the menu's Music Volume slider is the intended
        // control. Saturating, since dense passages already approach full scale.
        if (MUSIC_GAIN_NUM != MUSIC_GAIN_DEN) {
            for (int i = 0; i < MIXBUFFER_SAMPLES * 2; i++) {
                int32_t boosted = (outBuffer[i] * MUSIC_GAIN_NUM) / MUSIC_GAIN_DEN;
                if (boosted > 32767) boosted = 32767;
                if (boosted < -32768) boosted = -32768;
                outBuffer[i] = static_cast<int16_t>(boosted);
            }
        }

        for (int c = 0; c < NUM_CHANNELS; c++) {
            // Take a consistent snapshot of the channel under the lock, so the
            // pointer, length and position below are guaranteed to belong to the
            // same sound even if the game task restarts this channel mid-buffer.
            AudioChannel ch;
            if (!lockChannels()) {
                continue;
            }
            ch = channels[c];
            unlockChannels();

            if (!ch.playing || ch.data == nullptr) {
                continue;
            }

            bool finished = false;

            for (int i = 0; i < MIXBUFFER_SAMPLES; i++) {
                if (ch.pos >= ch.length) {
                    finished = true;
                    break;
                }

                // Skip the 8-byte DMX header: format(2), rate(2), num_samples(4)
                int8_t sample = static_cast<int8_t>(ch.data[ch.pos + 8] - 128);

                // Scale 8-bit to 16-bit with headroom for overlapping sounds.
                int32_t sample16 = sample * SFX_SCALE;

                // ch.vol comes from S_StartSound() and is on the 0-127 scale of
                // snd_SfxVolume, i.e. it already carries the menu's "Sfx Volume"
                // setting. Dividing by SFX_VOLUME_MAX rather than a smaller
                // constant is what keeps the slider meaningful: the previous
                // divisor of 15 amplified effects roughly 8x instead of
                // attenuating them, which is why they drowned out the music.
                int32_t mixedL = outBuffer[i * 2] + ((sample16 * ch.vol * (255 - ch.sep)) / (SFX_VOLUME_MAX * 255));
                int32_t mixedR = outBuffer[i * 2 + 1] + ((sample16 * ch.vol * ch.sep) / (SFX_VOLUME_MAX * 255));

                if (mixedL > 32767) mixedL = 32767;
                if (mixedL < -32768) mixedL = -32768;
                if (mixedR > 32767) mixedR = 32767;
                if (mixedR < -32768) mixedR = -32768;

                outBuffer[i * 2] = static_cast<int16_t>(mixedL);
                outBuffer[i * 2 + 1] = static_cast<int16_t>(mixedR);

                // Sample-and-hold upsample from the lump's 11025 Hz to the
                // stream rate: only step the source once every SFX_UPSAMPLE
                // output frames, otherwise the effect plays back fast and
                // pitched up.
                if (++ch.phase >= SFX_UPSAMPLE) {
                    ch.phase = 0;
                    ch.pos++;
                }
            }

            // Publish the advanced position, but only if the game task has not
            // started a different sound on this channel in the meantime - in
            // which case its state wins and this buffer's output is discarded.
            if (lockChannels()) {
                if (channels[c].data == ch.data && channels[c].playing) {
                    if (finished) {
                        channels[c].playing = false;
                    } else {
                        channels[c].pos = ch.pos;
                        channels[c].phase = ch.phase;
                    }
                }
                unlockChannels();
            }
        }

        size_t bytesWritten = 0;
        error_t writeError = audio_stream_write(outputHandle, outBuffer, sizeof(outBuffer), &bytesWritten, pdMS_TO_TICKS(500));

        if (writeError != ERROR_NONE) {
            // A failing write returns immediately rather than blocking for the
            // buffer to drain. Without a delay here this loop spins flat out at
            // near-top priority and starves the idle task on this core, which
            // trips the task watchdog. Back off so the system stays alive.
            ESP_LOGW(TAG, "Mixer write failed: %s", error_to_string(writeError));
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (bytesWritten < sizeof(outBuffer)) {
            // Short write: the stream accepted the data without blocking, so
            // this iteration cost us no wait at all. taskYIELD() is not enough
            // here - it only switches to tasks of equal or higher priority, and
            // the idle task is the lowest. A real delay is required to let it run.
            vTaskDelay(1);
        } else if ((++yieldCounter & 0x3F) == 0) {
            // Guarantee the idle task runs occasionally even when every write
            // blocks normally. One tick every 64 buffers is ~3 s of audio at
            // 11025 Hz, far too infrequent to affect tempo, but enough to keep
            // the task watchdog satisfied if the stream ever drains faster than
            // real time.
            vTaskDelay(1);
        }

    }

    audio_stream_close(outputHandle);
    xSemaphoreGive(stoppedSem);
    vTaskDelete(nullptr);
}

//==============================================================================
// DG_sound_module
//==============================================================================

snddevice_t soundDevices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS };

boolean Sound_Init(boolean /*use_sfx_prefix*/) {
    // Reassert the mixer's rate. doomAudio_Init() sets snd_samplerate before
    // the engine boots, but M_LoadDefaults() then reads it back from the config
    // file - and it is written there on exit, so a config saved by an older
    // build could otherwise override the rate the mixer is actually running at
    // and detune every sound. This runs after the config load, so the code wins.
    snd_samplerate = static_cast<int>(STREAM_SAMPLE_RATE);
    return true;
}

void Sound_Shutdown() {}

int Sound_GetSfxLumpNum(sfxinfo_t* sfxinfo) {
    char namebuf[12];
    snprintf(namebuf, sizeof(namebuf), "DS%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

void Sound_Update() {}

void Sound_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel >= 0 && channel < NUM_CHANNELS && lockChannels()) {
        channels[channel].vol = vol;
        channels[channel].sep = sep;
        unlockChannels();
    }
}

int Sound_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep) {
    if (!sfxinfo->driver_data) {
        sfxinfo->driver_data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    }

    int c = channel;
    if (c < 0 || c >= NUM_CHANNELS) {
        c = 0;
        if (lockChannels()) {
            for (int i = 0; i < NUM_CHANNELS; i++) {
                if (!channels[i].playing) { c = i; break; }
            }
            unlockChannels();
        }
    }

    // The 8-byte DMX header must be present before any of it is read. A lump
    // this short is malformed, but W_CacheLumpNum happily returns it.
    const int lumpLength = W_LumpLength(sfxinfo->lumpnum);
    if (lumpLength < 8) {
        return c;
    }

    const auto* b = static_cast<const uint8_t*>(sfxinfo->driver_data);
    uint16_t format = b[0] | (b[1] << 8);
    uint32_t numSamples = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);

    if (format != 3) {
        // Not a standard DOOM sound format
        return c;
    }

    // The header's sample count is not trustworthy on its own - clamp it to what
    // the lump actually holds, or the mixer reads past the end of the cached
    // block. The payload starts after the 8-byte header.
    const uint32_t availableSamples = static_cast<uint32_t>(lumpLength) - 8u;
    if (numSamples > availableSamples) {
        numSamples = availableSamples;
    }
    if (numSamples == 0) {
        return c;
    }

    if (lockChannels()) {
        channels[c].data = b;
        channels[c].length = numSamples;
        channels[c].pos = 0;
        channels[c].phase = 0;
        channels[c].vol = vol;
        channels[c].sep = sep;
        channels[c].playing = true;
        unlockChannels();
    }

    return c;
}

void Sound_StopSound(int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS && lockChannels()) {
        channels[channel].playing = false;
        unlockChannels();
    }
}

boolean Sound_SoundIsPlaying(int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS) {
        return channels[channel].playing;
    }
    return false;
}

void Sound_CacheSounds(sfxinfo_t* /*sounds*/, int /*num_sounds*/) {}

// DG_music_module is no longer defined here - it now lives in i_oplmusic.c,
// which drives the emulated OPL2/OPL3 chip. The mixer above renders it via
// DG_OPL_Render().

} // namespace

// i_sound.c's I_BindSoundVariables() binds these as config variables under
// FEATURE_SOUND, but they are normally defined by i_sdlsound.c / i_allegrosound.c,
// neither of which is built here.
extern "C" int use_libsamplerate = 0;
extern "C" float libsamplerate_scale = 1.0f;

extern "C" sound_module_t DG_sound_module = {
    soundDevices, 3,
    Sound_Init, Sound_Shutdown, Sound_GetSfxLumpNum,
    Sound_Update, Sound_UpdateSoundParams, Sound_StartSound,
    Sound_StopSound, Sound_SoundIsPlaying, Sound_CacheSounds
};

void doomAudio_Init() {
    // i_oplmusic.c opens the OPL chip at snd_samplerate, and the mixer renders
    // it at STREAM_SAMPLE_RATE. Keep the two in agreement.
    snd_samplerate = static_cast<int>(STREAM_SAMPLE_RATE);

    // Created before the mixer task, so the lock is always available to both
    // sides. Kept across runs - there is nothing to reinitialise.
    if (channelsMutex == nullptr) {
        channelsMutex = xSemaphoreCreateMutex();
        if (channelsMutex == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate channel mutex - sound disabled");
            return;
        }
    }

    for (auto& ch : channels) {
        ch.playing = false;
        ch.data = nullptr;
        ch.pos = 0;
        ch.phase = 0;
        ch.length = 0;
    }

    device_for_each_of_type(&AUDIO_STREAM_TYPE, &streamDevice, [](Device* dev, void* ctx) -> bool {
        if (device_is_ready(dev)) {
            *static_cast<Device**>(ctx) = dev;
            return false;
        }
        return true;
    });

    if (!streamDevice) {
        ESP_LOGW(TAG, "No audio stream device found - sound disabled");
        return;
    }

    if (!stoppedSem) {
        stoppedSem = xSemaphoreCreateBinary();
    }

    stopRequested.store(false);

    // Pinned to core 0 - the doom task itself runs at priority 5 on core 1, so keep
    // the mixer off that core to avoid contention with the watchdog-sensitive main loop.
    //
    // Priority 10 rather than near configMAX_PRIORITIES: the mixer now does real
    // work per buffer (OPL synthesis), so at near-top priority any iteration that
    // fails to block starves core 0's idle task and trips the watchdog. 10 is well
    // above the app and LVGL tasks - enough to keep audio glitch-free - while still
    // leaving the system schedulable.
    BaseType_t created = xTaskCreatePinnedToCore(mixerTask, "doom_audio", 8192, nullptr, 10, &mixerTaskHandle, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio mixer task");
        mixerTaskHandle = nullptr;
        streamDevice = nullptr;
    }
}

void doomAudio_Shutdown() {
    if (mixerTaskHandle) {
        stopRequested.store(true);
        xSemaphoreTake(stoppedSem, portMAX_DELAY);
        mixerTaskHandle = nullptr;
        streamDevice = nullptr;
    }

    // sfxinfo->driver_data (see Sound_StartSound) caches a pointer into zone memory, but S_sfx[]
    // is a global static array that outlives a single doomEsp_Run(). Z_Shutdown() frees the whole
    // zone on every exit path, so without this reset, the next run's Sound_StartSound would see
    // driver_data already set, skip re-caching, and read freed/reused memory. Must run before
    // Z_Shutdown() frees the zone these pointers point into.
    for (int i = 0; i < NUMSFX; i++) {
        S_sfx[i].driver_data = nullptr;
    }
}
