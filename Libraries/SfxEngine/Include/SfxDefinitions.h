/**
 * @file SfxDefinitions.h
 * @brief 52 curated SFX definitions for SfxEngine.
 *
 * Implements SfxEngine::loadSound() — mapping SfxId values to note sequences.
 */

#pragma once

#include "SfxEngine.h"
#include "esp_log.h"

// Safety macro to prevent buffer overflow
#define ADD_SFX_NOTE(seq, idx, ...) do { \
    if ((idx) < MAX_SFX_NOTES) { \
        SfxSequenceNote n = __VA_ARGS__; \
        (seq)[(idx)++] = n; \
    } else { \
        ESP_LOGE("SfxDef", "SFX note overflow at index %d", (idx)); \
    } \
} while(0)

inline void SfxEngine::loadSound(SfxId sound) {
    sequenceIndex_ = 0;
    sequenceDelaySamples_ = 0;
    sequenceLength_ = 0;

    // Stop all voices
    for (int i = 0; i < NUM_VOICES; i++) {
        voices_[i].active = false;
        voices_[i].envStage = EnvelopeStage::Off;
    }

    switch (sound) {

        // ── UI Sounds ────────────────────────────────────────────────────────

        case SfxId::Click:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 80, 30, 0, SfxWaveType::Square, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Confirm:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 80, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 120, 70, SfxWaveType::Square, 0.4f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Cancel:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 80, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 120, 70, SfxWaveType::Square, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Error:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 150, 0, SfxWaveType::Sawtooth, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {1, 51, 150, 0, SfxWaveType::Sawtooth, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::MenuOpen:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 40, 0, SfxWaveType::Pulse25, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 60, 30, SfxWaveType::Pulse25, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::MenuClose:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 40, 0, SfxWaveType::Pulse25, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 60, 30, SfxWaveType::Pulse25, 0.2f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Toggle:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 50, 0, SfxWaveType::Pulse12, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Slider:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 15, 0, SfxWaveType::Pulse25, 0.2f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Tab:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 67, 50, 0, SfxWaveType::Square, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 70, 40, SfxWaveType::Square, 0.3f});
            sequenceLength_ = i;
        }
        break;

        // ── TamaTac Pet Sounds ───────────────────────────────────────────────

        case SfxId::Feed:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 80, 0, SfxWaveType::Pulse25, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 80, 80, SfxWaveType::Pulse25, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 100, 80, SfxWaveType::Pulse25, 0.4f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Play:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 70, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {1, 60, 280, 0, SfxWaveType::Triangle, 0.2f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 70, 70, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 70, 70, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 100, 70, SfxWaveType::Square, 0.35f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Medicine:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 200, 0, SfxWaveType::Triangle, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {1, 79, 200, 50, SfxWaveType::Sine, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Sleep:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 79, 200, 0, SfxWaveType::Triangle, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 200, 180, SfxWaveType::Triangle, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 300, 180, SfxWaveType::Triangle, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Clean:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 50, 0, SfxWaveType::Pulse12, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 50, 40, SfxWaveType::Pulse12, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 80, 40, SfxWaveType::Pulse12, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Evolve:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 150, 0, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {1, 48, 600, 0, SfxWaveType::Triangle, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 64, 150, 130, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 67, 150, 130, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 400, 130, SfxWaveType::Square, 0.5f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Sick:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 100, 0, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 100, 150, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 100, 150, SfxWaveType::Square, 0.4f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Death:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 67, 250, 0, SfxWaveType::Triangle, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 64, 250, 220, SfxWaveType::Triangle, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 250, 220, SfxWaveType::Triangle, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 55, 350, 220, SfxWaveType::Triangle, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 48, 500, 300, SfxWaveType::Triangle, 0.2f});
            sequenceLength_ = i;
        }
        break;

        // ── Game Sounds ──────────────────────────────────────────────────────

        case SfxId::Coin:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 83, 60, 0, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 88, 150, 50, SfxWaveType::Square, 0.35f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Powerup:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 80, 0, SfxWaveType::Pulse25, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 64, 80, 60, SfxWaveType::Pulse25, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 67, 80, 60, SfxWaveType::Pulse25, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 80, 60, SfxWaveType::Pulse25, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 80, 60, SfxWaveType::Pulse25, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 80, 60, SfxWaveType::Pulse25, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 200, 60, SfxWaveType::Pulse25, 0.5f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Jump:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 30, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 65, 30, 25, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 70, 30, 25, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 75, 30, 25, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 80, 40, 25, SfxWaveType::Square, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 50, 30, SfxWaveType::Square, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Land:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 36, 80, 0, SfxWaveType::Triangle, 0.5f});
            ADD_SFX_NOTE(sequence_, i, {1, 40, 60, 0, SfxWaveType::Noise, 0.15f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Laser:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 96, 25, 0, SfxWaveType::Pulse25, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 90, 25, 20, SfxWaveType::Pulse25, 0.38f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 25, 20, SfxWaveType::Pulse25, 0.36f});
            ADD_SFX_NOTE(sequence_, i, {0, 78, 30, 20, SfxWaveType::Pulse25, 0.34f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 40, 25, SfxWaveType::Pulse25, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Explosion:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 50, 0, SfxWaveType::Noise, 0.5f});
            ADD_SFX_NOTE(sequence_, i, {1, 30, 300, 0, SfxWaveType::Triangle, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {0, 36, 150, 40, SfxWaveType::Noise, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {0, 30, 200, 100, SfxWaveType::Noise, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 24, 250, 150, SfxWaveType::Noise, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Hurt:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 80, 0, SfxWaveType::Sawtooth, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 48, 120, 60, SfxWaveType::Sawtooth, 0.35f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Warp:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 40, 0, SfxWaveType::Sine, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {1, 36, 350, 0, SfxWaveType::Noise, 0.12f});
            ADD_SFX_NOTE(sequence_, i, {0, 55, 40, 35, SfxWaveType::Sine, 0.32f});
            ADD_SFX_NOTE(sequence_, i, {0, 62, 40, 35, SfxWaveType::Sine, 0.34f});
            ADD_SFX_NOTE(sequence_, i, {0, 69, 40, 35, SfxWaveType::Sine, 0.36f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 40, 35, SfxWaveType::Sine, 0.38f});
            ADD_SFX_NOTE(sequence_, i, {0, 83, 50, 35, SfxWaveType::Sine, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 90, 60, 40, SfxWaveType::Sine, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 96, 80, 50, SfxWaveType::Sine, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Pickup:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 60, 0, SfxWaveType::Pulse12, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 88, 60, 50, SfxWaveType::Pulse12, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 91, 100, 50, SfxWaveType::Pulse12, 0.35f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::OneUp:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 100, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 76, 100, 80, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 100, 80, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 250, 80, SfxWaveType::Square, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {1, 60, 450, 0, SfxWaveType::Triangle, 0.2f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::BrickHit:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 76, 20, 0, SfxWaveType::Pulse25, 0.25f});
            sequenceLength_ = i;
        }
        break;

        // ── Notifications ────────────────────────────────────────────────────

        case SfxId::Alert:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 80, 0, SfxWaveType::Square, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 80, 120, SfxWaveType::Square, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 80, 120, SfxWaveType::Square, 0.45f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Notify:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 79, 100, 0, SfxWaveType::Sine, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 150, 90, SfxWaveType::Sine, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {1, 67, 250, 0, SfxWaveType::Triangle, 0.15f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Success:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 100, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 79, 100, 80, SfxWaveType::Square, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 200, 80, SfxWaveType::Square, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {1, 60, 400, 0, SfxWaveType::Triangle, 0.2f});
            sequenceLength_ = i;
        }
        break;

        // ── Drum Kit ─────────────────────────────────────────────────────────

        case SfxId::Kick:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 15, 0, SfxWaveType::Triangle, 0.6f});
            ADD_SFX_NOTE(sequence_, i, {0, 36, 60, 10, SfxWaveType::Triangle, 0.55f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Snare:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 50, 0, SfxWaveType::Noise, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {1, 48, 30, 0, SfxWaveType::Triangle, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::HiHat:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 80, 15, 0, SfxWaveType::Noise, 0.2f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Crash:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 250, 0, SfxWaveType::Noise, 0.3f});
            sequenceLength_ = i;
        }
        break;

        // ── Synth Percussion ─────────────────────────────────────────────────

        case SfxId::SynthKick:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 55, 10, 0, SfxWaveType::Sine, 0.6f});
            ADD_SFX_NOTE(sequence_, i, {0, 36, 120, 8, SfxWaveType::Sine, 0.55f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::SynthSnare:
            // MetalNoise -> Noise substitution
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 80, 0, SfxWaveType::Noise, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {1, 55, 40, 0, SfxWaveType::Triangle, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::SynthHat:
            // MetalNoise -> Noise substitution
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 30, 0, SfxWaveType::Noise, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::SynthTom:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 15, 0, SfxWaveType::Triangle, 0.5f});
            ADD_SFX_NOTE(sequence_, i, {0, 48, 100, 12, SfxWaveType::Triangle, 0.45f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::SynthRim:
            // MetalNoise -> Noise substitution
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 25, 0, SfxWaveType::Noise, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {1, 84, 15, 0, SfxWaveType::Pulse12, 0.25f});
            sequenceLength_ = i;
        }
        break;

        // ── Extra Effects ────────────────────────────────────────────────────

        case SfxId::Zap:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 96, 15, 0, SfxWaveType::Square, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 15, 10, SfxWaveType::Square, 0.30f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 15, 10, SfxWaveType::Square, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 20, 10, SfxWaveType::Square, 0.20f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Blip:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 90, 20, 0, SfxWaveType::Square, 0.3f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Chirp:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 20, 0, SfxWaveType::Pulse25, 0.3f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 30, 15, SfxWaveType::Pulse25, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Whoosh:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 30, 0, SfxWaveType::Noise, 0.15f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 30, 25, SfxWaveType::Noise, 0.20f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 30, 25, SfxWaveType::Noise, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 40, 25, SfxWaveType::Noise, 0.20f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::Ding:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 300, 0, SfxWaveType::Sine, 0.4f});
            ADD_SFX_NOTE(sequence_, i, {1, 91, 200, 0, SfxWaveType::Sine, 0.2f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::RisingWhoosh:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 36, 30, 0, SfxWaveType::Noise, 0.15f});
            ADD_SFX_NOTE(sequence_, i, {0, 48, 30, 25, SfxWaveType::Noise, 0.20f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 30, 25, SfxWaveType::Noise, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 40, 25, SfxWaveType::Noise, 0.30f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 50, 30, SfxWaveType::Noise, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::FallingWhoosh:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 30, 0, SfxWaveType::Noise, 0.30f});
            ADD_SFX_NOTE(sequence_, i, {0, 72, 30, 25, SfxWaveType::Noise, 0.25f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 30, 25, SfxWaveType::Noise, 0.20f});
            ADD_SFX_NOTE(sequence_, i, {0, 48, 40, 25, SfxWaveType::Noise, 0.15f});
            ADD_SFX_NOTE(sequence_, i, {0, 36, 50, 30, SfxWaveType::Noise, 0.10f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::GlitchHit:
            // MetalNoise -> Noise substitution
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 48, 20, 0, SfxWaveType::Noise, 0.45f});
            ADD_SFX_NOTE(sequence_, i, {1, 36, 80, 0, SfxWaveType::Sawtooth, 0.40f});
            ADD_SFX_NOTE(sequence_, i, {0, 96, 15, 15, SfxWaveType::Pulse12, 0.30f});
            ADD_SFX_NOTE(sequence_, i, {0, 60, 40, 10, SfxWaveType::Noise, 0.25f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::DigitalBurst:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 25, 0, SfxWaveType::Pulse12, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 84, 25, 20, SfxWaveType::Pulse12, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 96, 25, 20, SfxWaveType::Pulse12, 0.35f});
            ADD_SFX_NOTE(sequence_, i, {0, 108, 30, 20, SfxWaveType::Pulse12, 0.30f});
            sequenceLength_ = i;
        }
        break;

        case SfxId::RetroBell:
            // HarmonicSquare -> Square substitution
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 84, 200, 0, SfxWaveType::Square, 0.15f});
            ADD_SFX_NOTE(sequence_, i, {1, 91, 180, 0, SfxWaveType::Pulse25, 0.10f});
            sequenceLength_ = i;
        }
        break;

        // ── Jingles ──────────────────────────────────────────────────────────

        // Rising arpeggio (C4-E4-G4-C5-E5-G5-C6) with sustained bass (C3)
        case SfxId::LevelUp:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 60, 100, 0, SfxWaveType::Square, 0.28f});     // C4
            ADD_SFX_NOTE(sequence_, i, {1, 48, 800, 0, SfxWaveType::Triangle, 0.16f});    // C3 bass pad
            ADD_SFX_NOTE(sequence_, i, {0, 64, 100, 80, SfxWaveType::Square, 0.28f});     // E4
            ADD_SFX_NOTE(sequence_, i, {0, 67, 100, 80, SfxWaveType::Square, 0.28f});     // G4
            ADD_SFX_NOTE(sequence_, i, {0, 72, 150, 80, SfxWaveType::Square, 0.32f});     // C5
            ADD_SFX_NOTE(sequence_, i, {0, 76, 100, 130, SfxWaveType::Square, 0.32f});    // E5
            ADD_SFX_NOTE(sequence_, i, {0, 79, 100, 80, SfxWaveType::Square, 0.32f});     // G5
            ADD_SFX_NOTE(sequence_, i, {0, 84, 300, 80, SfxWaveType::Square, 0.40f});     // C6 (hold)
            sequenceLength_ = i;
        }
        break;

        // Descending chromatic line (C5-B4-A4-G4-F4-E4-C4) with bass drone
        case SfxId::GameOver:
        {
            int i = 0;
            ADD_SFX_NOTE(sequence_, i, {0, 72, 200, 0, SfxWaveType::Square, 0.30f});     // C5
            ADD_SFX_NOTE(sequence_, i, {1, 60, 800, 0, SfxWaveType::Triangle, 0.15f});    // C4 bass drone
            ADD_SFX_NOTE(sequence_, i, {0, 71, 200, 180, SfxWaveType::Square, 0.28f});    // B4
            ADD_SFX_NOTE(sequence_, i, {0, 69, 200, 180, SfxWaveType::Square, 0.27f});    // A4
            ADD_SFX_NOTE(sequence_, i, {0, 67, 200, 180, SfxWaveType::Square, 0.25f});    // G4
            ADD_SFX_NOTE(sequence_, i, {1, 55, 600, 0, SfxWaveType::Triangle, 0.15f});    // G3 bass shift
            ADD_SFX_NOTE(sequence_, i, {0, 65, 250, 180, SfxWaveType::Square, 0.24f});    // F4
            ADD_SFX_NOTE(sequence_, i, {0, 64, 400, 230, SfxWaveType::Square, 0.22f});    // E4
            ADD_SFX_NOTE(sequence_, i, {0, 60, 600, 380, SfxWaveType::Square, 0.18f});    // C4 (hold)
            sequenceLength_ = i;
        }
        break;

        case SfxId::None:
        default:
            sequenceLength_ = 0;
            break;
    }

    // Start sequence
    if (sequenceLength_ > 0) {
        sequenceDelaySamples_ = (sequence_[0].delayMs * SAMPLE_RATE) / 1000;
    }
}

#undef ADD_SFX_NOTE
