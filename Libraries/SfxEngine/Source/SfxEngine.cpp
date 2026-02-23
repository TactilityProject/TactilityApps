/**
 * @file SfxEngine.cpp
 * @brief Lightweight SFX-only audio engine implementation.
 *
 * Stripped-down version of SoundEngine — no BGM, no delay, no advanced synthesis.
 * Sound definitions are in SfxDefinitions.h (inline loadSound).
 */

#include "SfxEngine.h"
#include "SfxDefinitions.h"

#include <tactility/drivers/i2s_controller.h>
#include <cmath>
#include <cstring>
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

//==============================================================================
// Noise Generators
//==============================================================================

float SfxEngine::generateNoise() {
    uint16_t bit = ((lfsr_ >> 0) ^ (lfsr_ >> 2) ^ (lfsr_ >> 3) ^ (lfsr_ >> 5)) & 1;
    lfsr_ = (lfsr_ >> 1) | (bit << 15);
    return (lfsr_ & 1) ? 1.0f : -1.0f;
}

float SfxEngine::generateRetroNoise() {
    uint16_t bit = ((retroLfsr_ >> 0) ^ (retroLfsr_ >> 1)) & 1;
    retroLfsr_ = (retroLfsr_ >> 1) | (bit << 14);
    return (retroLfsr_ & 1) ? 1.0f : -1.0f;
}

//==============================================================================
// Utility
//==============================================================================

float SfxEngine::midiToFreq(uint8_t midi) {
    if (midi == 0) return 0.0f;
    if (midi > 127) midi = 127;
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}

float SfxEngine::oscillator(SfxWaveType wave, float phase) {
    switch (wave) {
        case SfxWaveType::Square:
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case SfxWaveType::Pulse25:
            return (phase < 0.25f) ? 1.0f : -1.0f;
        case SfxWaveType::Pulse12:
            return (phase < 0.125f) ? 1.0f : -1.0f;
        case SfxWaveType::Pulse75:
            return (phase < 0.75f) ? 1.0f : -1.0f;
        case SfxWaveType::Triangle:
            return 4.0f * fabsf(phase - 0.5f) - 1.0f;
        case SfxWaveType::Sawtooth:
            return 2.0f * phase - 1.0f;
        case SfxWaveType::Sine:
            return sinf(phase * 2.0f * M_PI);
        case SfxWaveType::Noise:
            return generateNoise();
        case SfxWaveType::RetroNoise:
            return generateRetroNoise();
        default:
            return 0.0f;
    }
}

//==============================================================================
// Envelope Processing
//==============================================================================

void SfxEngine::updateEnvelope(Voice& v) {
    if (v.envStage == EnvelopeStage::Off) {
        v.envLevel = 0.0f;
        return;
    }

    v.envSamplePos++;

    // Special envelope types
    if (v.envType == SfxEnvelopeType::Punch) {
        if (v.envSamplePos < v.attackSamples) {
            v.envLevel = 1.0f + (1.0f - static_cast<float>(v.envSamplePos) / v.attackSamples);
        } else {
            v.envLevel = 1.0f;
        }
        if (v.samplePos >= v.totalSamples && v.totalSamples > 0) {
            v.envStage = EnvelopeStage::Off;
            v.active = false;
        }
        if (v.envLevel > 1.5f) v.envLevel = 1.5f;
        return;
    } else if (v.envType == SfxEnvelopeType::Flare) {
        if (v.envSamplePos < v.attackSamples) {
            float progress = static_cast<float>(v.envSamplePos) / v.attackSamples;
            v.envLevel = progress * progress;
        } else if (v.envSamplePos < v.attackSamples + v.decaySamples) {
            float progress = static_cast<float>(v.envSamplePos - v.attackSamples) / v.decaySamples;
            v.envLevel = 1.0f - progress;
        } else {
            v.envLevel = 0.0f;
            v.envStage = EnvelopeStage::Off;
            v.active = false;
        }
        return;
    } else if (v.envType == SfxEnvelopeType::Swell) {
        if (v.totalSamples > 0) {
            v.envLevel = static_cast<float>(v.samplePos) / v.totalSamples;
            if (v.envLevel > 1.0f) v.envLevel = 1.0f;
        } else {
            v.envLevel = 1.0f;
        }
        if (v.samplePos >= v.totalSamples && v.totalSamples > 0) {
            v.envStage = EnvelopeStage::Off;
            v.active = false;
        }
        return;
    } else if (v.envType == SfxEnvelopeType::Twang) {
        if (v.envSamplePos == 1) v.envLevel = 1.0f;
        v.envLevel *= 0.992f;
        if (v.envLevel < 0.001f) {
            v.envLevel = 0.0f;
            v.envStage = EnvelopeStage::Off;
            v.active = false;
        }
        return;
    } else if (v.envType == SfxEnvelopeType::Decay) {
        if (v.totalSamples > 0) {
            v.envLevel = 1.0f - (static_cast<float>(v.samplePos) / v.totalSamples);
            if (v.envLevel < 0.0f) v.envLevel = 0.0f;
        }
        if (v.samplePos >= v.totalSamples && v.totalSamples > 0) {
            v.envStage = EnvelopeStage::Off;
            v.active = false;
        }
        return;
    }

    // Standard ADSR
    switch (v.envStage) {
        case EnvelopeStage::Attack:
            if (v.attackSamples > 0) {
                v.envLevel = static_cast<float>(v.envSamplePos) / v.attackSamples;
            } else {
                v.envLevel = 1.0f;
            }
            if (v.envSamplePos >= v.attackSamples) {
                v.envStage = EnvelopeStage::Decay;
                v.envSamplePos = 0;
            }
            break;

        case EnvelopeStage::Decay:
            if (v.decaySamples > 0) {
                float progress = static_cast<float>(v.envSamplePos) / v.decaySamples;
                v.envLevel = 1.0f - progress * (1.0f - v.sustainLevel);
            } else {
                v.envLevel = v.sustainLevel;
            }
            if (v.envSamplePos >= v.decaySamples) {
                v.envStage = EnvelopeStage::Sustain;
                v.envSamplePos = 0;
            }
            break;

        case EnvelopeStage::Sustain:
            v.envLevel = v.sustainLevel;
            if (v.samplePos >= v.totalSamples && v.totalSamples > 0) {
                v.envStage = EnvelopeStage::Release;
                v.envSamplePos = 0;
            }
            break;

        case EnvelopeStage::Release:
            if (v.releaseSamples > 0) {
                float progress = static_cast<float>(v.envSamplePos) / v.releaseSamples;
                v.envLevel = v.sustainLevel * (1.0f - progress);
            } else {
                v.envLevel = 0.0f;
            }
            if (v.envSamplePos >= v.releaseSamples) {
                v.envStage = EnvelopeStage::Off;
                v.active = false;
            }
            break;

        default:
            break;
    }

    if (v.envLevel < 0.0f) v.envLevel = 0.0f;
    if (v.envLevel > 1.0f) v.envLevel = 1.0f;
}

//==============================================================================
// Advanced Mixing Functions
//==============================================================================

float SfxEngine::applyPolyphonicGate(float mix, int activeVoices) {
    if (!polyphonicGateEnabled_ || activeVoices <= 1) return mix;

    float absMix = fabsf(mix);
    if (absMix <= softGateThreshold_) return mix;

    float excess = absMix - softGateThreshold_;
    float gainReduction = 1.0f - (excess / (1.0f + activeVoices * 0.25f));
    gainReduction = fmaxf(gainReduction, 0.3f);

    return mix * gainReduction;
}

float SfxEngine::applyAutoNormalization(float mix) {
    float sampleSquared = mix * mix;
    currentRms_ = currentRms_ * rmsSmoothing_ + sampleSquared * (1.0f - rmsSmoothing_);

    if (!autoNormalize_) return mix;

    if (++rmsCalcCounter_ >= 16) {
        rmsCalcCounter_ = 0;

        if (currentRms_ >= 0.001f) {
            float currentRmsLinear = sqrtf(currentRms_);
            float gainAdjust = targetRms_ / currentRmsLinear;

            gainAdjust = fminf(gainAdjust, 1.5f);
            gainAdjust = fmaxf(gainAdjust, 0.5f);

            cachedNormGain_ = gainAdjust;
        } else {
            cachedNormGain_ = 1.0f;
        }
    }

    return mix * cachedNormGain_;
}

//==============================================================================
// Brick-Wall Limiter
//==============================================================================

float SfxEngine::applyBrickWallLimiter(float sample) {
    constexpr float threshold = 0.98f;
    constexpr float knee = 0.02f;

    float absSample = fabsf(sample);
    if (absSample <= threshold - knee) {
        return sample;
    } else if (absSample >= threshold) {
        return (sample > 0.0f) ? threshold : -threshold;
    } else {
        float excess = absSample - (threshold - knee);
        float compression = knee * tanhf(excess / knee);
        float limited = (threshold - knee) + compression;
        return (sample > 0.0f) ? limited : -limited;
    }
}

//==============================================================================
// Sample Generation
//==============================================================================

float SfxEngine::generateVoiceSample(Voice& v) {
    if (!v.active && v.envStage == EnvelopeStage::Off) {
        return 0.0f;
    }

    // Apply vibrato
    float freq = v.currentFreq;
    if (v.vibratoDepth > 0.0f && v.vibratoRate > 0.0f) {
        float vibrato = sinf(v.vibratoPhase * 2.0f * M_PI) * v.vibratoDepth;
        freq = freq * powf(2.0f, vibrato / 12.0f);
        v.vibratoPhase += v.vibratoRate / SAMPLE_RATE;
        if (v.vibratoPhase >= 1.0f) v.vibratoPhase -= 1.0f;
    }

    // Apply pitch sweep
    if (v.pitchSweep != 0.0f) {
        float semitonesSwept = v.pitchSweep * (static_cast<float>(v.samplePos) / SAMPLE_RATE);
        freq = freq * powf(2.0f, semitonesSwept / 12.0f);
    }

    // Generate oscillator output
    float sample = oscillator(v.wave, v.phase);

    // Advance phase
    if (freq > 0.0f) {
        v.phase += freq / SAMPLE_RATE;
        while (v.phase >= 1.0f) v.phase -= 1.0f;
    }

    // Update envelope
    updateEnvelope(v);

    // Advance sample position
    v.samplePos++;

    // Apply envelope and volume
    return sample * v.envLevel * v.volume;
}

void SfxEngine::fillStereoBuffer(int16_t* buf, int samples) {
    for (int i = 0; i < samples; i++) {
        processSequence();

        float mix = 0.0f;

        if (enabled_) {
            int activeVoices = 0;
            for (int v = 0; v < NUM_VOICES; v++) {
                if (voices_[v].active || voices_[v].envStage != EnvelopeStage::Off) {
                    mix += generateVoiceSample(voices_[v]);
                    activeVoices++;
                }
            }

            // Apply polyphonic soft gate (proportional reduction when clipping threatened)
            mix = applyPolyphonicGate(mix, activeVoices);

            // Apply master volume (exponential curve for perceptual linearity)
            float volCurve = masterVolume_ * masterVolume_;
            mix *= volCurve;

            // Apply auto-normalization (consistent volume across different SFX)
            mix = applyAutoNormalization(mix);

            // Brick-wall limiter (final safety net before soft clip)
            mix = applyBrickWallLimiter(mix);
        }

        // Cubic soft clip
        if (mix > 1.0f) mix = 1.0f;
        else if (mix < -1.0f) mix = -1.0f;
        else mix = (3.0f - mix * mix) * mix / 2.0f;

        // Convert to 16-bit stereo
        int16_t s = static_cast<int16_t>(mix * 28000);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
}

//==============================================================================
// Note Triggering
//==============================================================================

void SfxEngine::triggerNote(Voice& v, uint8_t midiNote, uint16_t durationMs,
                            SfxWaveType wave, float volume, SfxEnvelopeType envType,
                            uint16_t attackMs, uint16_t decayMs,
                            float sustain, uint16_t releaseMs) {
    v.wave = wave;
    v.baseFreq = midiToFreq(midiNote);
    v.currentFreq = v.baseFreq;
    v.phase = 0.0f;
    v.samplePos = 0;
    v.totalSamples = (durationMs * SAMPLE_RATE) / 1000;
    v.volume = volume;

    // Envelope
    v.envType = envType;
    v.envStage = EnvelopeStage::Attack;
    v.envLevel = 0.0f;
    v.envSamplePos = 0;

    if (attackMs > 0 || decayMs > 0 || releaseMs > 0 || envType != SfxEnvelopeType::ADSR) {
        v.attackSamples = (attackMs * SAMPLE_RATE) / 1000;
        v.decaySamples = (decayMs * SAMPLE_RATE) / 1000;
        v.sustainLevel = sustain;
        v.releaseSamples = (releaseMs * SAMPLE_RATE) / 1000;
    } else {
        // Default quick envelope for SFX
        v.attackSamples = (10 * SAMPLE_RATE) / 1000;
        v.decaySamples = (30 * SAMPLE_RATE) / 1000;
        v.sustainLevel = 0.8f;
        v.releaseSamples = (50 * SAMPLE_RATE) / 1000;
    }

    // Reset pitch effects
    v.vibratoDepth = 0.0f;
    v.vibratoRate = 0.0f;
    v.vibratoPhase = 0.0f;
    v.pitchSweep = 0.0f;

    v.active = true;
}

//==============================================================================
// Sequence Processing
//==============================================================================

void SfxEngine::processSequence() {
    if (sequenceIndex_ >= sequenceLength_) return;

    if (sequenceDelaySamples_ > 0) {
        sequenceDelaySamples_--;
        return;
    }

    const SfxSequenceNote& note = sequence_[sequenceIndex_];

    if (note.voice < NUM_VOICES && note.pitch > 0) {
        triggerNote(voices_[note.voice], note.pitch, note.durationMs,
                    note.wave, note.volume, note.envType,
                    note.attackMs, note.decayMs, note.sustain, note.releaseMs);
    }

    sequenceIndex_++;

    if (sequenceIndex_ < sequenceLength_) {
        sequenceDelaySamples_ = (sequence_[sequenceIndex_].delayMs * SAMPLE_RATE) / 1000;
    }
}

//==============================================================================
// Audio Task
//==============================================================================

void SfxEngine::audioTaskFunc(void* param) {
    auto* self = static_cast<SfxEngine*>(param);

    size_t written;
    QueueMsg msg;

    ESP_LOGI(TAG, "Audio task started");

    while (self->running_) {
        // Process queued messages (non-blocking)
        while (xQueueReceive(self->msgQueue_, &msg, 0) == pdTRUE) {
            switch (msg.type) {
                case MsgType::PlaySound:
                    self->loadSound(msg.sfxId);
                    break;

                case MsgType::PlayNote:
                    if (msg.note.voice < NUM_VOICES) {
                        self->triggerNote(self->voices_[msg.note.voice],
                                          msg.note.midiNote, msg.note.durationMs,
                                          msg.note.wave, msg.note.volume,
                                          SfxEnvelopeType::ADSR);
                    }
                    break;

                case MsgType::StopVoice:
                    if (msg.voiceIndex < NUM_VOICES) {
                        self->voices_[msg.voiceIndex].envStage = EnvelopeStage::Release;
                        self->voices_[msg.voiceIndex].envSamplePos = 0;
                    }
                    break;

                case MsgType::StopAll:
                    for (int i = 0; i < NUM_VOICES; i++) {
                        self->voices_[i].envStage = EnvelopeStage::Release;
                        self->voices_[i].envSamplePos = 0;
                    }
                    self->sequenceLength_ = 0;
                    break;
            }
        }

        // Fill audio buffer (member buffer to avoid stack pressure)
        self->fillStereoBuffer(self->audioBuffer_, BUFFER_SAMPLES);

        // Write to I2S
        error_t error = i2s_controller_write(self->i2sDevice_, self->audioBuffer_,
                                              sizeof(self->audioBuffer_), &written, pdMS_TO_TICKS(100));
        if (error != ERROR_NONE) {
            ESP_LOGE(TAG, "I2S write error");
            self->running_ = false;
            break;
        }
    }

    // Flush silence
    memset(self->audioBuffer_, 0, sizeof(self->audioBuffer_));
    i2s_controller_write(self->i2sDevice_, self->audioBuffer_, sizeof(self->audioBuffer_), &written, pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Audio task exiting");

    // Signal stop() that we're done
    if (self->stopSemaphore_ != nullptr) {
        xSemaphoreGive(self->stopSemaphore_);
    }

    vTaskDelete(NULL);
}

//==============================================================================
// Public API
//==============================================================================

bool SfxEngine::start() {
    if (running_) return true;

    // Find I2S device
    i2sDevice_ = nullptr;
    device_for_each_of_type(&I2S_CONTROLLER_TYPE, &i2sDevice_, [](Device* device, void* context) {
        if (!device_is_ready(device)) return true;
        Device** devicePtr = static_cast<Device**>(context);
        *devicePtr = device;
        return false;
    });

    if (i2sDevice_ == nullptr) {
        ESP_LOGW(TAG, "No I2S device found");
        return false;
    }

    // Configure I2S
    I2sConfig config = {
        .communication_format = I2S_FORMAT_STAND_I2S,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel_left = 0,
        .channel_right = 0
    };

    error_t error = i2s_controller_set_config(i2sDevice_, &config);
    if (error != ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to configure I2S: %s", error_to_string(error));
        i2sDevice_ = nullptr;
        return false;
    }

    // Create message queue
    msgQueue_ = xQueueCreate(8, sizeof(QueueMsg));
    if (msgQueue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create message queue");
        i2s_controller_reset(i2sDevice_);
        i2sDevice_ = nullptr;
        return false;
    }

    // Start audio task
    running_ = true;
    BaseType_t result = xTaskCreate(audioTaskFunc, "sfxeng", 4096, this, 5, &task_);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        running_ = false;
        vQueueDelete(msgQueue_);
        msgQueue_ = nullptr;
        i2s_controller_reset(i2sDevice_);
        i2sDevice_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "SfxEngine started (voices=%d, sampleRate=%d)", NUM_VOICES, SAMPLE_RATE);
    return true;
}

void SfxEngine::stop() {
    if (!running_) return;

    // Create semaphore for deterministic shutdown
    stopSemaphore_ = xSemaphoreCreateBinary();
    running_ = false;

    if (task_ != nullptr) {
        // Wait for audio task to signal completion (up to 500ms)
        if (stopSemaphore_ != nullptr) {
            xSemaphoreTake(stopSemaphore_, pdMS_TO_TICKS(500));
        }
        task_ = nullptr;
    }

    if (stopSemaphore_ != nullptr) {
        vSemaphoreDelete(stopSemaphore_);
        stopSemaphore_ = nullptr;
    }

    if (msgQueue_ != nullptr) {
        vQueueDelete(msgQueue_);
        msgQueue_ = nullptr;
    }

    if (i2sDevice_ != nullptr) {
        i2s_controller_reset(i2sDevice_);
        i2sDevice_ = nullptr;
    }

    ESP_LOGI(TAG, "SfxEngine stopped");
}

void SfxEngine::applyVolumePreset(VolumePreset preset) {
    switch (preset) {
        case VolumePreset::Quiet:
            masterVolume_ = 0.3f;
            autoNormalize_ = true;
            targetRms_ = 0.25f;
            polyphonicGateEnabled_ = true;
            softGateThreshold_ = 0.90f;
            ESP_LOGI(TAG, "Applied Quiet preset");
            break;
        case VolumePreset::Normal:
            masterVolume_ = 0.5f;
            autoNormalize_ = true;
            targetRms_ = 0.35f;
            polyphonicGateEnabled_ = true;
            softGateThreshold_ = 0.95f;
            ESP_LOGI(TAG, "Applied Normal preset");
            break;
        case VolumePreset::Loud:
            masterVolume_ = 0.75f;
            autoNormalize_ = true;
            targetRms_ = 0.45f;
            polyphonicGateEnabled_ = true;
            softGateThreshold_ = 0.98f;
            ESP_LOGI(TAG, "Applied Loud preset");
            break;
    }
}

void SfxEngine::play(SfxId sound) {
    if (!running_ || msgQueue_ == nullptr) return;

    QueueMsg msg;
    msg.type = MsgType::PlaySound;
    msg.sfxId = sound;
    xQueueSend(msgQueue_, &msg, 0);
}

void SfxEngine::stopAllSounds() {
    if (!running_ || msgQueue_ == nullptr) return;

    QueueMsg msg;
    msg.type = MsgType::StopAll;
    xQueueSend(msgQueue_, &msg, 0);
}

void SfxEngine::playNote(uint8_t voice, uint8_t midiNote, uint16_t durationMs,
                          SfxWaveType wave, float volume) {
    if (!running_ || msgQueue_ == nullptr || voice >= NUM_VOICES) return;

    QueueMsg msg;
    msg.type = MsgType::PlayNote;
    msg.note = {voice, midiNote, durationMs, wave, volume};
    xQueueSend(msgQueue_, &msg, 0);
}

void SfxEngine::stopVoice(uint8_t voice) {
    if (!running_ || msgQueue_ == nullptr || voice >= NUM_VOICES) return;

    QueueMsg msg;
    msg.type = MsgType::StopVoice;
    msg.voiceIndex = voice;
    xQueueSend(msgQueue_, &msg, 0);
}
