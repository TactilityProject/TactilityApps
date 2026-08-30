//
// Nuked OPL3 API shim backed by ymfm.
//
// The original Nuked OPL3 emulator is cycle-accurate but far too expensive
// for this target: measured on an ESP32-P4 at 360 MHz it needed ~49000 us to
// render a 512-frame buffer against a ~23219 us real-time budget - over 200%
// of one core, with no song even playing.
//
// ymfm's YMF262 core measures ~3.65x faster for the same work, which brings
// rendering inside budget. This header keeps the three Nuked entry points
// that opl.c uses (OPL3_Reset, OPL3_WriteRegBuffered, OPL3_GenerateStream)
// so nothing above it had to change; the implementation in opl3.cpp forwards
// them to ymfm.
//
// opl.c stores the chip by value, so opl3_chip must be a complete type with
// a fixed size. It holds an opaque pointer to the heap-allocated C++ object.
//

#ifndef OPL_OPL3_H
#define OPL_OPL3_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _opl3_chip {
    void *impl;            // ymfm chip instance, owned by opl3.cpp
    uint32_t samplerate;   // rate requested by the host
} opl3_chip;

// Reset (and lazily create) the chip, configured for the given output rate.
void OPL3_Reset(opl3_chip *chip, uint32_t samplerate);

// Write a register. Nuked's "buffered" variant applied writes with emulated
// bus timing; ymfm applies them immediately, which is indistinguishable here
// because opl.c already schedules writes at the correct sample offsets.
void OPL3_WriteRegBuffered(opl3_chip *chip, uint16_t reg, uint8_t v);

// Render numsamples interleaved stereo frames at the configured rate.
void OPL3_GenerateStream(opl3_chip *chip, int16_t *sndptr, uint32_t numsamples);

// Release the ymfm instance. Not part of Nuked's API - opl.c's OPL_Destroy
// calls this so the heap object is not leaked when a context goes away.
void OPL3_Destroy(opl3_chip *chip);

#ifdef __cplusplus
}
#endif

#endif
