//
// Nuked OPL3 API shim backed by DBOPL (the DOSBox OPL emulator).
//
// Two earlier cores were measured on the ESP32-P4 at 360 MHz, rendering a
// 512-frame buffer at 11025 Hz against a ~46439 us real-time budget:
//
//   Nuked OPL3  ~2x over budget - cycle-accurate, far too slow.
//   ymfm YMF262 ~108% of budget at 9 voices, and ~63% even rendering
//               silence, because its FM engine clocks all 36 operators
//               every native (49716 Hz) tick regardless of activity.
//               Batching, smaller buffers and voice caps made no difference;
//               the per-tick cost is inherent to cycle-accurate emulation.
//
// DBOPL is table-driven rather than cycle-accurate and measured ~19-30x
// faster than ymfm for identical workloads, which puts even full 18-voice
// OPL3 comfortably inside budget. It is also the core DOSBox shipped for
// years, so it is the closest match to how DOOM's music actually sounded on
// most people's machines.
//
// Crucially, DBOPL resamples internally: Chip::Setup(rate) configures it to
// emit directly at the host's rate, so the manual interpolation the ymfm shim
// needed is gone entirely.
//
// opl.c stores the chip by value, so opl3_chip stays a small struct holding an
// opaque pointer to the heap-allocated C++ object.
//

#include "opl3.h"

#include "dbopl/dbopl.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Local operator new/delete.
//
// The ELF loader resolves an app's undefined symbols against what the firmware
// exports, and the global C++ allocation operators are not among them - a build
// referencing them fails at load with "Can't find common _Znwj...". Defining
// them here keeps any C++ allocation inside the app. Deliberately simple: the
// app is built without exceptions, so a failed allocation returns null.
// ---------------------------------------------------------------------------

void* operator new(size_t size) noexcept { return malloc(size); }
void* operator new[](size_t size) noexcept { return malloc(size); }
void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

namespace {

struct ChipImpl {
    DBOPL::Chip chip;

    // Set when the player enables OPL3 mode via register 0x105. DBOPL needs to
    // know at construction time whether it is an OPL3, so the chip is built in
    // OPL3 mode and this only tracks what the player has asked for.
    bool opl3Mode = false;

    // Scratch for DBOPL, which emits 32-bit samples.
    int32_t scratch[1024 * 2] = {};

    ChipImpl() : chip(true) {}
};

constexpr uint32_t SCRATCH_FRAMES = 1024;

} // namespace

extern "C" {

void OPL3_Reset(opl3_chip* chip, uint32_t samplerate) {
    if (chip == nullptr) {
        return;
    }

    // DBOPL's lookup tables are global and must be built once before any chip
    // is created. Idempotent, so calling it on every reset is harmless.
    DBOPL::InitTables();

    auto* impl = static_cast<ChipImpl*>(chip->impl);
    if (impl == nullptr) {
        // malloc + placement new rather than plain `new`, to keep the global
        // allocation operators out of the picture entirely.
        void* storage = malloc(sizeof(ChipImpl));
        if (storage == nullptr) {
            chip->impl = nullptr;
            return;
        }
        impl = new (storage) ChipImpl();
        chip->impl = impl;
    }

    if (samplerate == 0) {
        samplerate = 49716;
    }
    chip->samplerate = samplerate;

    // Setup() both resets the chip state and configures its internal
    // resampling to emit at this rate.
    impl->chip.Setup(samplerate);
    impl->opl3Mode = false;
}

void OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v) {
    if (chip == nullptr || chip->impl == nullptr) {
        return;
    }
    auto* impl = static_cast<ChipImpl*>(chip->impl);

    // Track OPL3 "NEW" mode so the 0xC0 fix-up below knows which chip the
    // player believes it is talking to.
    if (reg == 0x105) {
        impl->opl3Mode = (v & 0x01) != 0;
    }

    // 0xC0-0xC8 are the per-channel feedback/algorithm registers. On a real
    // OPL2 they carry no output-routing bits and every voice is simply
    // audible; on an OPL3, bits 4 and 5 gate the left and right outputs and
    // reset to zero.
    //
    // i_oplmusic.c only sets those bits from SetChannelPan(), which is a no-op
    // unless opl_opl3mode is set - so in OPL2 mode it writes feedback with the
    // routing bits clear and the voice would be inaudible. Force both on to
    // get the unconditional OPL2 output behaviour the player expects.
    if (!impl->opl3Mode && (reg & 0xF0) == 0xC0 && (reg & 0x0F) <= 8) {
        v |= 0x30;
    }

    impl->chip.WriteReg(reg, v);
}

void OPL3_GenerateStream(opl3_chip* chip, int16_t* sndptr, uint32_t numsamples) {
    if (chip == nullptr || chip->impl == nullptr) {
        memset(sndptr, 0, numsamples * 2 * sizeof(int16_t));
        return;
    }
    auto* impl = static_cast<ChipImpl*>(chip->impl);

    while (numsamples > 0) {
        uint32_t block = numsamples > SCRATCH_FRAMES ? SCRATCH_FRAMES : numsamples;

        if (impl->opl3Mode) {
            // OPL3: stereo, two int32 per frame.
            impl->chip.GenerateBlock3(block, impl->scratch);
            for (uint32_t i = 0; i < block; i++) {
                int32_t left = impl->scratch[i * 2];
                int32_t right = impl->scratch[i * 2 + 1];
                if (left > 32767) left = 32767;
                if (left < -32768) left = -32768;
                if (right > 32767) right = 32767;
                if (right < -32768) right = -32768;
                sndptr[i * 2] = static_cast<int16_t>(left);
                sndptr[i * 2 + 1] = static_cast<int16_t>(right);
            }
        } else {
            // OPL2: mono, one int32 per frame - duplicated to both channels.
            impl->chip.GenerateBlock2(block, impl->scratch);
            for (uint32_t i = 0; i < block; i++) {
                int32_t sample = impl->scratch[i];
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                sndptr[i * 2] = static_cast<int16_t>(sample);
                sndptr[i * 2 + 1] = static_cast<int16_t>(sample);
            }
        }

        sndptr += block * 2;
        numsamples -= block;
    }
}

void OPL3_Destroy(opl3_chip* chip) {
    if (chip == nullptr) {
        return;
    }
    auto* impl = static_cast<ChipImpl*>(chip->impl);
    if (impl != nullptr) {
        impl->~ChipImpl();
        free(impl);
    }
    chip->impl = nullptr;
}

} // extern "C"
