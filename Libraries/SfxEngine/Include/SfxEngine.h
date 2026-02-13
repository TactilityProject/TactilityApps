/**
 * @file SfxEngine.h
 * @brief Lightweight SFX-only audio engine for ESP32/Tactility apps (~1.5KB RAM)
 *
 * A stripped-down version of SoundEngine focused on sound effects only.
 * No background music, no advanced synthesis (FM, filters, pluck, delay).
 *
 * Features:
 *   - 4 simultaneous voices (polyphony)
 *   - Waveforms: Square, Pulse (25%/12.5%/75%), Triangle, Sawtooth, Sine, Noise, RetroNoise
 *   - ADSR envelope + special types (Punch, Flare, Swell, Twang, Decay)
 *   - Effects: Vibrato, Pitch Sweep
 *   - 52 curated predefined sound effects
 *   - Queue-based triggering (thread-safe)
 *   - Persistent audio task
 *
 * Usage:
 *   SfxEngine* engine = new SfxEngine();
 *   engine->start();
 *   engine->play(SfxId::Coin);
 *   engine->playNote(0, 72, 200);
 *   engine->stop();
 *   delete engine;
 */
#pragma once

#include <cstdint>
#include <tactility/device.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

//==============================================================================
// Configuration
//==============================================================================

#ifndef SFX_ENGINE_NUM_VOICES
#define SFX_ENGINE_NUM_VOICES 4
#endif

#ifndef SFX_ENGINE_SAMPLE_RATE
#define SFX_ENGINE_SAMPLE_RATE 16000
#endif

#ifndef SFX_ENGINE_BUFFER_SAMPLES
#define SFX_ENGINE_BUFFER_SAMPLES 256
#endif

//==============================================================================
// Types
//==============================================================================

enum class SfxWaveType : uint8_t {
    Square,      // 50% duty cycle
    Pulse25,     // 25% duty cycle (NES-style)
    Pulse12,     // 12.5% duty cycle
    Pulse75,     // 75% duty cycle (bright, hollow)
    Triangle,
    Sawtooth,
    Sine,
    Noise,       // White noise (random)
    RetroNoise   // NES-style LFSR noise
};

enum class SfxId : uint8_t {
    None = 0,

    // UI Sounds (9)
    Click, Confirm, Cancel, Error,
    MenuOpen, MenuClose, Toggle, Slider, Tab,

    // TamaTac Pet Sounds (8)
    Feed, Play, Medicine, Sleep, Clean, Evolve, Sick, Death,

    // Game Sounds (11)
    Coin, Powerup, Jump, Land, Laser, Explosion,
    Hurt, Warp, Pickup, OneUp, BrickHit,

    // Notifications (3)
    Alert, Notify, Success,

    // Drum Kit (4)
    Kick, Snare, HiHat, Crash,

    // Synth Percussion (5)
    SynthKick, SynthSnare, SynthHat, SynthTom, SynthRim,

    // Extra Effects (10)
    Zap, Blip, Chirp, Whoosh, Ding,
    RisingWhoosh, FallingWhoosh, GlitchHit, DigitalBurst, RetroBell,

    // Jingles (2)
    LevelUp, GameOver
};

enum class SfxEnvelopeType : uint8_t {
    ADSR,    // Standard attack-decay-sustain-release
    Punch,   // Constant volume with boost at start
    Flare,   // Exponential rise then decay
    Swell,   // Linear fade-in
    Twang,   // Extremely fast decay
    Decay    // Linear decay to zero
};

// Compact note for SFX sequences
struct SfxSequenceNote {
    uint8_t voice;
    uint8_t pitch;
    uint16_t durationMs;
    uint16_t delayMs;
    SfxWaveType wave;
    float volume;
    // Optional ADSR override (all zero = use defaults)
    uint16_t attackMs = 0;
    uint16_t decayMs = 0;
    float sustain = 0.0f;
    uint16_t releaseMs = 0;
    SfxEnvelopeType envType = SfxEnvelopeType::ADSR;

    constexpr SfxSequenceNote()
        : voice(0), pitch(0), durationMs(0), delayMs(0),
          wave(SfxWaveType::Square), volume(0.0f) {}

    constexpr SfxSequenceNote(uint8_t v, uint8_t p, uint16_t dur, uint16_t del,
                              SfxWaveType w, float vol,
                              uint16_t att = 0, uint16_t dec = 0, float sus = 0.0f,
                              uint16_t rel = 0, SfxEnvelopeType et = SfxEnvelopeType::ADSR)
        : voice(v), pitch(p), durationMs(dur), delayMs(del), wave(w), volume(vol),
          attackMs(att), decayMs(dec), sustain(sus), releaseMs(rel), envType(et) {}
};

//==============================================================================
// SfxEngine Class
//==============================================================================

class SfxEngine {
public:
    SfxEngine() = default;
    ~SfxEngine() { stop(); }

    SfxEngine(const SfxEngine&) = delete;
    SfxEngine& operator=(const SfxEngine&) = delete;
    SfxEngine(SfxEngine&&) = delete;
    SfxEngine& operator=(SfxEngine&&) = delete;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const { return running_; }

    // SFX API
    void play(SfxId sound);
    void stopAllSounds();

    // Manual note API
    void playNote(uint8_t voice, uint8_t midiNote, uint16_t durationMs,
                  SfxWaveType wave = SfxWaveType::Square, float volume = 0.5f);
    void stopVoice(uint8_t voice);

    // Settings
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    void setVolume(float vol) { masterVolume_ = (vol < 0) ? 0 : (vol > 1.0f) ? 1.0f : vol; }
    float getVolume() const { return masterVolume_; }

    // Volume presets (consistent with SoundEngine naming)
    enum class VolumePreset { Quiet, Normal, Loud };
    void applyVolumePreset(VolumePreset preset);

    // Polyphonic gate (consistent with SoundEngine)
    void setPolyphonicGateEnabled(bool enabled) { polyphonicGateEnabled_ = enabled; }
    bool isPolyphonicGateEnabled() const { return polyphonicGateEnabled_; }
    void setSoftGateThreshold(float threshold) {
        softGateThreshold_ = (threshold < 0.5f) ? 0.5f : (threshold > 1.0f) ? 1.0f : threshold;
    }
    float getSoftGateThreshold() const { return softGateThreshold_; }

    // Auto-normalization (consistent with SoundEngine)
    void setAutoNormalization(bool enabled) { autoNormalize_ = enabled; }
    bool isAutoNormalizationEnabled() const { return autoNormalize_; }
    void setTargetRms(float rms) {
        targetRms_ = (rms < 0.1f) ? 0.1f : (rms > 0.8f) ? 0.8f : rms;
    }
    float getTargetRms() const { return targetRms_; }
    void setRmsSmoothing(float smoothing) {
        rmsSmoothing_ = (smoothing < 0.9f) ? 0.9f : (smoothing > 0.99999f) ? 0.99999f : smoothing;
    }
    float getRmsSmoothing() const { return rmsSmoothing_; }

    static constexpr int NUM_VOICES = SFX_ENGINE_NUM_VOICES;
    static constexpr int SAMPLE_RATE = SFX_ENGINE_SAMPLE_RATE;

private:
    static constexpr const char* TAG = "SfxEngine";
    static constexpr int BUFFER_SAMPLES = SFX_ENGINE_BUFFER_SAMPLES;
    static constexpr int MAX_SFX_NOTES = 64;

    //--------------------------------------------------------------------------
    // Voice State
    //--------------------------------------------------------------------------

    enum class EnvelopeStage { Off, Attack, Decay, Sustain, Release };

    struct Voice {
        // Oscillator
        SfxWaveType wave = SfxWaveType::Square;
        float phase = 0.0f;
        float baseFreq = 0.0f;
        float currentFreq = 0.0f;

        // Timing
        int samplePos = 0;
        int totalSamples = 0;

        // Envelope
        SfxEnvelopeType envType = SfxEnvelopeType::ADSR;
        EnvelopeStage envStage = EnvelopeStage::Off;
        float envLevel = 0.0f;
        int envSamplePos = 0;
        int attackSamples = 0;
        int decaySamples = 0;
        float sustainLevel = 0.7f;
        int releaseSamples = 0;

        // Volume
        float volume = 0.5f;

        // Pitch effects
        float vibratoDepth = 0.0f;
        float vibratoRate = 0.0f;
        float vibratoPhase = 0.0f;
        float pitchSweep = 0.0f;

        bool active = false;
    };

    Voice voices_[NUM_VOICES] = {};

    //--------------------------------------------------------------------------
    // SFX Sequence
    //--------------------------------------------------------------------------

    SfxSequenceNote sequence_[MAX_SFX_NOTES] = {};
    int sequenceLength_ = 0;
    int sequenceIndex_ = 0;
    int sequenceDelaySamples_ = 0;

    //--------------------------------------------------------------------------
    // Queue Messages
    //--------------------------------------------------------------------------

    enum class MsgType : uint8_t {
        PlaySound, PlayNote, StopVoice, StopAll
    };

    struct PlayNoteMsg {
        uint8_t voice;
        uint8_t midiNote;
        uint16_t durationMs;
        SfxWaveType wave;
        float volume;
    };

    struct QueueMsg {
        MsgType type;
        union {
            SfxId sfxId;
            PlayNoteMsg note;
            uint8_t voiceIndex;
        };

        QueueMsg() : type(MsgType::StopAll), sfxId(SfxId::None) {}
    };

    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------

    Device* i2sDevice_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t stopSemaphore_ = nullptr;  // Signaled when audio task exits
    QueueHandle_t msgQueue_ = nullptr;

    volatile bool running_ = false;
    volatile bool enabled_ = true;
    volatile float masterVolume_ = 0.5f;

    // Polyphonic gate
    volatile bool polyphonicGateEnabled_ = true;
    volatile float softGateThreshold_ = 0.95f;

    // Auto-normalization
    volatile bool autoNormalize_ = true;
    volatile float targetRms_ = 0.35f;
    volatile float rmsSmoothing_ = 0.999f;

    // Noise generator state (per-instance, audio task only)
    uint16_t lfsr_ = 0xACE1;
    uint16_t retroLfsr_ = 0xACE1;

    // Auto-normalization working state (audio task only)
    float currentRms_ = 0.0f;
    int rmsCalcCounter_ = 0;
    float cachedNormGain_ = 1.0f;

    // Audio buffer (member to avoid stack pressure in audio task)
    int16_t audioBuffer_[BUFFER_SAMPLES * 2] = {};

    //--------------------------------------------------------------------------
    // Audio Generation
    //--------------------------------------------------------------------------

    static float midiToFreq(uint8_t midi);
    float generateNoise();
    float generateRetroNoise();
    float oscillator(SfxWaveType wave, float phase);
    void updateEnvelope(Voice& v);
    float generateVoiceSample(Voice& v);
    void fillStereoBuffer(int16_t* buf, int samples);
    float applyPolyphonicGate(float mix, int activeVoices);
    float applyAutoNormalization(float mix);
    float applyBrickWallLimiter(float sample);

    //--------------------------------------------------------------------------
    // Sound Loading
    //--------------------------------------------------------------------------

    void loadSound(SfxId sound);
    void triggerNote(Voice& v, uint8_t midiNote, uint16_t durationMs,
                     SfxWaveType wave, float volume, SfxEnvelopeType envType,
                     uint16_t attackMs = 0, uint16_t decayMs = 0,
                     float sustain = 0.0f, uint16_t releaseMs = 0);
    void processSequence();

    //--------------------------------------------------------------------------
    // Task
    //--------------------------------------------------------------------------

    static void audioTaskFunc(void* param);
};
