# SfxEngine Library

Lightweight SFX-only audio engine for ESP32/Tactility apps. A stripped-down alternative to SoundEngine (~1.5KB RAM vs ~66KB).

## Features

- **4 voices** with polyphonic mixing
- **9 waveforms**: Square, Pulse (25%/12.5%/75%), Triangle, Sawtooth, Sine, Noise, RetroNoise
- **ADSR envelope** + special types (Punch, Flare, Swell, Twang, Decay)
- **Effects**: Vibrato, Pitch Sweep, Polyphonic Gate, Auto-Normalization
- **52 curated SFX**: UI sounds, pet sounds, game sounds, drums, percussion, effects, jingles
- **Thread-safe** queue-based API
- **ESP32 optimized**: ~1.5KB RAM, heap-allocated

## When to Use

| | SfxEngine | SoundEngine |
|---|-----------|-------------|
| **RAM** | ~1.5KB | ~66KB |
| **Voices** | 4 | 8 |
| **SFX** | 52 curated | 100+ |
| **BGM** | No | Yes (9 tracks) |
| **Synthesis** | Basic | FM, Filter, Pluck, Drums |
| **Effects** | Vibrato, Sweep, Gate, Norm | + Delay, Tremolo, Arp, Bitcrush |
| **Use case** | UI feedback, simple games | Rich audio, music apps |

## Quick Start

```cpp
#include "SfxEngine.h"

SfxEngine* engine = new SfxEngine();

if (!engine->start()) {
    ESP_LOGE(TAG, "Failed to start SfxEngine");
    return;
}
engine->applyVolumePreset(SfxEngine::VolumePreset::Normal);

engine->play(SfxId::Coin);                // Predefined SFX
engine->playNote(0, 60, 200);             // Manual: voice 0, C4, 200ms
engine->setVolume(0.7f);                   // Volume control

engine->stop();
delete engine;
```

## Available SFX

**UI** (9): Click, Confirm, Cancel, Error, MenuOpen, MenuClose, Toggle, Slider, Tab
**TamaTac Pet** (8): Feed, Play, Medicine, Sleep, Clean, Evolve, Sick, Death
**Game** (11): Coin, Powerup, Jump, Land, Laser, Explosion, Hurt, Warp, Pickup, OneUp, BrickHit
**Notifications** (3): Alert, Notify, Success
**Drums** (4): Kick, Snare, HiHat, Crash
**Synth Percussion** (5): SynthKick, SynthSnare, SynthHat, SynthTom, SynthRim
**Effects** (10): Zap, Blip, Chirp, Whoosh, Ding, RisingWhoosh, FallingWhoosh, GlitchHit, DigitalBurst, RetroBell
**Jingles** (2): LevelUp, GameOver

## CMake Integration

```cmake
file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE SFX_ENGINE_FILES ../../../Libraries/SfxEngine/Source/*.c*)

idf_component_register(
    SRCS ${SOURCE_FILES} ${SFX_ENGINE_FILES}
    INCLUDE_DIRS
        ../../../Libraries/TactilityCpp/Include
        ../../../Libraries/SfxEngine/Include
    REQUIRES TactilitySDK esp_driver_i2s esp_driver_gpio
)
```

## API Reference

### Lifecycle
- `bool start()` - Start audio engine
- `void stop()` - Stop audio engine
- `bool isRunning()` - Check status

### SFX
- `void play(SfxId)` - Play predefined sound
- `void stopAllSounds()` - Stop all voices

### Manual Notes
- `void playNote(voice, midiNote, durationMs, wave, volume)` - Play custom note
- `void stopVoice(voice)` - Stop specific voice

### Settings
- `void setVolume(float)` - Master volume (0.0-1.0, exponential curve)
- `void setEnabled(bool)` - Mute/unmute
- `void applyVolumePreset(VolumePreset)` - Apply Quiet/Normal/Loud preset (configures volume, gate, normalization)

### Mixing (consistent with SoundEngine)
- `void setPolyphonicGateEnabled(bool)` - Soft gate when multiple voices clip (default: on)
- `void setSoftGateThreshold(float)` - Gate threshold 0.5-1.0 (default: 0.95)
- `void setAutoNormalization(bool)` - RMS-based auto-gain (default: on)
- `void setTargetRms(float)` - Target RMS level 0.1-0.8 (default: 0.35)
- `void setRmsSmoothing(float)` - Smoothing factor 0.9-0.99999 (default: 0.999)

## File Structure

```text
Libraries/SfxEngine/
  Include/
    SfxEngine.h        # Core engine header
    SfxDefinitions.h   # 52 curated SFX definitions
  Source/
    SfxEngine.cpp      # Implementation
  LICENSE.md           # GPL v3
  README.md
```

## License

Licensed under the [GNU General Public License v3.0](LICENSE.md).
