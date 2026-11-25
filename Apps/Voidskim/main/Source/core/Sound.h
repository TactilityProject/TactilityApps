#pragma once

#include <Bleep/AudioSource.h>
#include <Bleep/Sequencer.h>
#include <Bleep/Song.h>
#include <Bleep/Synth.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hd {

enum class Sfx : uint8_t {
    MenuMove,
    MenuSelect,
    MenuBack,
    CountdownBeep,
    CountdownGo,
    Boost,
    WallHit,
    Launch,
    Land,
    Fall,
    Respawn,
    Lap,
    FinalLap,
    Finish,
    Count
};

// Music is in the Bleep format (Libraries/Bleep): .bleep files
using bleep::Song;
using bleep::parseSong;

/** Which sounds play, chosen in the menu. */
enum class AudioMode : uint8_t { All, EffectsOnly, Off };

/**
 * The game's sound: music (a Bleep song on voices 0-3), sound effects (voices 4-5) and the player's
 * engine hum (voice 6), mixed to mono 16-bit samples by a Bleep synth.
 *
 * Two threads: the game calls play()/setMusic()/setEngine(), the audio output calls render().
 * They only share atomics and a single-producer/single-consumer command queue, so neither blocks.
 */
class SoundSystem : public bleep::AudioSource {
public:
    static constexpr int SAMPLE_RATE = bleep::SAMPLE_RATE;

    SoundSystem();

    // Game thread

    void play(Sfx sfx);
    /**
     * Plays a song in a loop (nullptr: silence). The song must stay alive and unchanged while it
     * may be playing: until another one is set and the next render() has run.
     */
    void setMusic(const Song* song);
    /**
     * @param speed 0..1 of top speed
     * @param on whether the engine is audible (driving, not paused)
     */
    void setEngine(float speed, bool throttle, bool boosting, bool on);
    void setMode(AudioMode mode) { mode_.store((uint8_t)mode); }
    AudioMode mode() const { return (AudioMode)mode_.load(); }

    // Audio thread

    /** Renders the next frames of mono audio. */
    void render(int16_t* out, int frames) override;

private:
    // Voices: 0-3 music, 4-5 effects, 6 engine
    static constexpr int SFX_VOICE = 4;
    static constexpr int SFX_VOICES = 2;
    static constexpr int ENGINE_VOICE = 6;
    bleep::Synth synth;
    bleep::Sequencer sequencer { 0 };
    int nextSfxVoice = 0;

    // Command queue (game -> audio)
    static constexpr int QUEUE_SIZE = 16;
    uint8_t queue[QUEUE_SIZE];
    std::atomic<uint32_t> queueHead { 0 };
    std::atomic<uint32_t> queueTail { 0 };

    std::atomic<const Song*> musicRequest { nullptr };
    std::atomic<uint8_t> mode_ { 0 };
    std::atomic<uint32_t> engineState { 0 }; // packed: speed (16 bits), throttle, boosting, on

    // Audio thread only
    const Song* currentMusic = nullptr;
    float engineSpeed = 0.0f;
    int engineVolume = 0;

    void startSfx(Sfx sfx);
    void updateEngine();
};

} // namespace hd
