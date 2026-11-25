#include "Sound.h"

namespace hd {

SoundSystem::SoundSystem() = default;

// region Game thread

void SoundSystem::play(Sfx sfx) {
    const uint32_t head = queueHead.load(std::memory_order_relaxed);
    const uint32_t tail = queueTail.load(std::memory_order_acquire);
    if (head - tail >= QUEUE_SIZE) {
        return; // full: drop it, it's only a sound
    }
    queue[head % QUEUE_SIZE] = (uint8_t)sfx;
    queueHead.store(head + 1, std::memory_order_release);
}

void SoundSystem::setMusic(const Song* song) {
    musicRequest.store(song);
}

void SoundSystem::setEngine(float speed, bool throttle, bool boosting, bool on) {
    if (speed < 0.0f) speed = 0.0f;
    if (speed > 1.5f) speed = 1.5f;
    const uint32_t packed = (uint32_t)(speed * 40000.0f) | (throttle ? 1u << 16 : 0u) | (boosting ? 1u << 17 : 0u) | (on ? 1u << 18 : 0u);
    engineState.store(packed);
}

// endregion

// region Audio thread

void SoundSystem::startSfx(Sfx sfx) {
    const int voice = SFX_VOICE + nextSfxVoice;
    nextSfxVoice = (nextSfxVoice + 1) % SFX_VOICES;
    using bleep::Wave;
    auto sweep = [&](Wave wave, float fromHz, float toHz, int ms, int volume) {
        synth.sweep(voice, wave, fromHz, toHz, ms, volume);
    };
    switch (sfx) {
        case Sfx::MenuMove: sweep(Wave::Pulse25, 1320, 1320, 40, 1400); break;
        case Sfx::MenuSelect: sweep(Wave::Pulse25, 880, 1760, 110, 1800); break;
        case Sfx::MenuBack: sweep(Wave::Pulse25, 1100, 550, 110, 1600); break;
        case Sfx::CountdownBeep: sweep(Wave::Pulse50, 880, 880, 180, 2000); break;
        case Sfx::CountdownGo: sweep(Wave::Pulse50, 1760, 1760, 450, 2200); break;
        case Sfx::Boost: sweep(Wave::Noise, 6000, 1500, 350, 1800); break;
        case Sfx::WallHit: sweep(Wave::Noise, 900, 300, 140, 2600); break;
        case Sfx::Launch: sweep(Wave::Pulse12, 300, 900, 260, 1600); break;
        case Sfx::Land: sweep(Wave::Triangle, 160, 60, 120, 3000); break;
        case Sfx::Fall: sweep(Wave::Pulse25, 900, 90, 1100, 2000); break;
        case Sfx::Respawn: sweep(Wave::Triangle, 300, 1200, 320, 2600); break;
        case Sfx::Lap: sweep(Wave::Pulse25, 1046, 1568, 260, 1800); break;
        case Sfx::FinalLap: sweep(Wave::Pulse50, 784, 2093, 600, 2000); break;
        case Sfx::Finish: sweep(Wave::Pulse25, 523, 2093, 900, 2200); break;
        case Sfx::Count: break;
    }
}

void SoundSystem::updateEngine() {
    const uint32_t packed = engineState.load();
    const float speed = (float)(packed & 0xFFFF) / 40000.0f;
    const bool throttle = (packed >> 16) & 1;
    const bool boosting = (packed >> 17) & 1;
    const bool on = (packed >> 18) & 1;
    // Smoothed, so the hum glides rather than steps
    engineSpeed += (speed - engineSpeed) * 0.15f;
    const int targetVolume = on ? 260 + (throttle ? 200 : 0) + (boosting ? 220 : 0) : 0;
    engineVolume += (targetVolume - engineVolume) / 4;
    if (engineVolume > 4) {
        synth.tone(ENGINE_VOICE, boosting ? bleep::Wave::Pulse25 : bleep::Wave::Saw,
                   38.0f + engineSpeed * 120.0f + (boosting ? 25.0f : 0.0f), engineVolume);
    } else {
        synth.stop(ENGINE_VOICE);
    }
}

void SoundSystem::render(int16_t* out, int frames) {
    // Commands from the game
    uint32_t tail = queueTail.load(std::memory_order_relaxed);
    const uint32_t head = queueHead.load(std::memory_order_acquire);
    const AudioMode currentMode = mode();
    while (tail != head) {
        const uint8_t sfx = queue[tail % QUEUE_SIZE];
        tail++;
        if (currentMode != AudioMode::Off && sfx < (uint8_t)Sfx::Count) {
            startSfx((Sfx)sfx);
        }
    }
    queueTail.store(tail, std::memory_order_release);

    const Song* music = currentMode == AudioMode::All ? musicRequest.load() : nullptr;
    if (music != currentMusic) {
        currentMusic = music;
        sequencer.start(synth, music);
    }
    updateEngine();
    if (currentMode == AudioMode::Off) {
        synth.stop(ENGINE_VOICE);
    }

    for (int i = 0; i < frames; i++) {
        sequencer.tick(synth);
        int mix = synth.mix() * 12; // master gain: the busiest mix (music, engine, two effects) peaks around -5 dBFS
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }
}

// endregion

} // namespace hd
