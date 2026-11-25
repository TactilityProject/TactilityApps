#pragma once

#include <cstdint>

namespace bleep {

/** Something that produces mono audio at SAMPLE_RATE, called from the audio task. */
class AudioSource {
public:
    /** Fills out with the next frames of mono 16-bit audio. */
    virtual void render(int16_t* out, int frames) = 0;

protected:
    ~AudioSource() = default; // not deleted through this interface
};

} // namespace bleep
