//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     OPL interface.
//
//     Reworked for the JUCE plugin build: instead of a single global OPL
//     chip driven by an SDL_mixer thread, the chip is a heap-allocated
//     context (opl_context_t) so that each plugin instance owns its own,
//     and audio is generated on demand with OPL_Render().
//


#ifndef OPL_OPL_H
#define OPL_OPL_H

#include <inttypes.h>

typedef struct opl_context_s opl_context_t;

typedef void (*opl_callback_t)(void *data);

// Result from OPL_Create(), indicating what type of OPL chip was detected,
// if any.
typedef enum
{
    OPL_INIT_NONE,
    OPL_INIT_OPL2,
    OPL_INIT_OPL3,
} opl_init_result_t;

typedef enum
{
    OPL_REGISTER_PORT = 0,
    OPL_DATA_PORT = 1,
    OPL_REGISTER_PORT_OPL3 = 2
} opl_port_t;

#define OPL_NUM_OPERATORS   21
#define OPL_NUM_VOICES      9

#define OPL_REG_WAVEFORM_ENABLE   0x01
#define OPL_REG_TIMER1            0x02
#define OPL_REG_TIMER2            0x03
#define OPL_REG_TIMER_CTRL        0x04
#define OPL_REG_FM_MODE           0x08
#define OPL_REG_NEW               0x105

// Operator registers (21 of each):

#define OPL_REGS_TREMOLO          0x20
#define OPL_REGS_LEVEL            0x40
#define OPL_REGS_ATTACK           0x60
#define OPL_REGS_SUSTAIN          0x80
#define OPL_REGS_WAVEFORM         0xE0

// Voice registers (9 of each):

#define OPL_REGS_FREQ_1           0xA0
#define OPL_REGS_FREQ_2           0xB0
#define OPL_REGS_FEEDBACK         0xC0

// Times

#define OPL_SECOND ((uint64_t) 1000 * 1000)
#define OPL_MS     ((uint64_t) 1000)
#define OPL_US     ((uint64_t) 1)

//
// Lifecycle.
//

// Create a new OPL context (chip + timer queue + lock).  Returns NULL on
// failure.  chip_type receives the detected chip type (always OPL3-capable
// with the software emulator).

opl_context_t *OPL_Create(opl_init_result_t *chip_type);

// Destroy an OPL context created by OPL_Create().

void OPL_Destroy(opl_context_t *context);

// Set the sample rate used for software emulation.  Takes effect on the
// next OPL_Render() (the chip is re-created on the audio thread).

void OPL_SetSampleRate(opl_context_t *context, unsigned int rate);

//
// Register access.
//

// Write to an OPL register.

void OPL_WriteRegister(opl_context_t *context, int reg, int value);

// Initialize all registers, performed on startup.

void OPL_InitRegisters(opl_context_t *context, int opl3);

//
// Timer callback functions.
//

// Set a timer callback.  After the specified number of microseconds
// have elapsed, the callback will be invoked.

void OPL_SetCallback(opl_context_t *context, uint64_t us,
                     opl_callback_t callback, void *data);

// Adjust callback times by the specified factor. For example, a value of
// 0.5 will halve all remaining times.

void OPL_AdjustCallbacks(opl_context_t *context, float factor);

// Clear all OPL callbacks that have been set.

void OPL_ClearCallbacks(opl_context_t *context);

// Begin critical section, during which, OPL callbacks will not be
// invoked.  The lock is recursive.

void OPL_Lock(opl_context_t *context);

// End critical section.

void OPL_Unlock(opl_context_t *context);

// Pause the OPL callbacks.

void OPL_SetPaused(opl_context_t *context, int paused);

//
// Rendering.
//
// Generate the requested number of stereo sample frames (interleaved L/R
// signed 16-bit) at the mixing sample rate, invoking any scheduled timer
// callbacks at the correct points in time as it goes.

void OPL_Render(opl_context_t *context, int16_t *buffer, unsigned int nsamples);

//
// Music rendering entry point (implemented in i_oplmusic.c).
//
// Called from the audio mixer task to render the currently-playing song into
// an interleaved stereo buffer of `nsamples` frames at `rate` Hz. The MIDI
// timer queue is advanced in lockstep with the samples produced, so tempo
// follows the audio device's actual consumption rate. Writes silence when no
// song is playing, so the caller never has to special-case that.
//

#ifdef __cplusplus
extern "C" {
#endif

void DG_OPL_Render(int16_t *buffer, unsigned int nsamples, unsigned int rate);

#ifdef __cplusplus
}
#endif

#endif
