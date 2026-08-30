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
//     OPL interface - pull-based, per-context software driver.
//
//     This is a rewrite of the Chocolate Doom OPL front end (opl.c +
//     opl_sdl.c) for the JUCE plugin build.  All state lives in a
//     heap-allocated opl_context_t so that each plugin instance owns its
//     own OPL chip; there is no SDL and no dedicated audio thread.  The
//     host pulls audio with OPL_Render(), and the timer-callback queue is
//     advanced in lockstep with the samples that are generated.  The
//     Yamaha OPL2/OPL3 chip itself is emulated by Nuked OPL3 (opl3.c).
//

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"

#include "opl.h"
#include "opl_queue.h"

// ---------------------------------------------------------------------------
// Small cross-platform recursive mutex.
//
// Each context is touched from two threads: the game thread (which registers
// songs, writes registers and schedules callbacks) and the host audio thread
// (which calls OPL_Render()).  The lock is recursive so OPL_Lock() can nest
// inside OPL_Render() while a fired callback calls OPL_WriteRegister().
// ---------------------------------------------------------------------------

#if defined(ESP_PLATFORM)
  // The lock MUST be recursive: OPL_Render() holds it while firing timer
  // callbacks, and those callbacks call back into OPL_WriteRegister() /
  // OPL_SetCallback(), which take it again on the same task.
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  typedef SemaphoreHandle_t dg_mutex_t;
  static void dg_mutex_init (dg_mutex_t *m)   { *m = xSemaphoreCreateRecursiveMutex (); }
  static void dg_mutex_destroy (dg_mutex_t *m){ if (*m) { vSemaphoreDelete (*m); *m = NULL; } }
  static void dg_mutex_lock (dg_mutex_t *m)   { xSemaphoreTakeRecursive (*m, portMAX_DELAY); }
  static void dg_mutex_unlock (dg_mutex_t *m) { xSemaphoreGiveRecursive (*m); }
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  typedef CRITICAL_SECTION dg_mutex_t;
  static void dg_mutex_init (dg_mutex_t *m)   { InitializeCriticalSection (m); }
  static void dg_mutex_destroy (dg_mutex_t *m){ DeleteCriticalSection (m); }
  static void dg_mutex_lock (dg_mutex_t *m)   { EnterCriticalSection (m); }
  static void dg_mutex_unlock (dg_mutex_t *m) { LeaveCriticalSection (m); }
#else
  #include <pthread.h>
  typedef pthread_mutex_t dg_mutex_t;
  static void dg_mutex_init (dg_mutex_t *m)
  {
      pthread_mutexattr_t attr;
      pthread_mutexattr_init (&attr);
      pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init (m, &attr);
      pthread_mutexattr_destroy (&attr);
  }
  static void dg_mutex_destroy (dg_mutex_t *m){ pthread_mutex_destroy (m); }
  static void dg_mutex_lock (dg_mutex_t *m)   { pthread_mutex_lock (m); }
  static void dg_mutex_unlock (dg_mutex_t *m) { pthread_mutex_unlock (m); }
#endif

// ---------------------------------------------------------------------------
// Context.
// ---------------------------------------------------------------------------

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;

struct opl_context_s
{
    // The emulated chip.
    opl3_chip chip;
    int opl3mode;

    // Rate the chip is currently generating at, and the rate requested by
    // the host.  When they differ the chip is re-created at the start of the
    // next OPL_Render().
    unsigned int mixing_freq;
    unsigned int desired_rate;

    // Callback queue and virtual clock (microseconds since startup).
    opl_callback_queue_t *queue;
    uint64_t current_time;

    // Playback pause state and accumulated paused time.
    int paused;
    uint64_t pause_offset;

    // Last register number written via the port interface.
    int register_num;

    // Timers.  The chip does not track these itself.
    opl_timer_t timer1;
    opl_timer_t timer2;

    dg_mutex_t lock;
};

void OPL_Lock(opl_context_t *context)
{
    dg_mutex_lock (&context->lock);
}

void OPL_Unlock(opl_context_t *context)
{
    dg_mutex_unlock (&context->lock);
}

// ---------------------------------------------------------------------------
// Register writing.
// ---------------------------------------------------------------------------

static void OPLTimer_CalculateEndTime(opl_context_t *context, opl_timer_t *timer)
{
    int tics;

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = context->current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(opl_context_t *context,
                          unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            context->timer1.value = value;
            OPLTimer_CalculateEndTime(context, &context->timer1);
            break;

        case OPL_REG_TIMER2:
            context->timer2.value = value;
            OPLTimer_CalculateEndTime(context, &context->timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                context->timer1.enabled = 0;
                context->timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    context->timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(context, &context->timer1);
                }

                if ((value & 0x20) == 0)
                {
                    context->timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(context, &context->timer2);
                }
            }

            break;

        case OPL_REG_NEW:
            context->opl3mode = value & 0x01;
            // fall through - the value is also written to the chip.

        default:
            OPL3_WriteRegBuffered(&context->chip, (uint16_t) reg_num,
                                  (uint8_t) value);
            break;
    }
}

void OPL_WriteRegister(opl_context_t *context, int reg, int value)
{
    OPL_Lock(context);
    WriteRegister(context, (unsigned int) reg, (unsigned int) value);
    OPL_Unlock(context);
}

// Initialize registers on startup.  (Ported from the original opl.c; drives
// the chip through OPL_WriteRegister.)

void OPL_InitRegisters(opl_context_t *context, int opl3)
{
    int r;

    // Initialize level registers

    for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(context, r, 0x3f);
    }

    // Initialize other registers
    // These two loops write to registers that actually don't exist,
    // but this is what Doom does ...
    // Similarly, the <= is also intenational.

    for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(context, r, 0x00);
    }

    // More registers ...

    for (r=1; r < OPL_REGS_LEVEL; ++r)
    {
        OPL_WriteRegister(context, r, 0x00);
    }

    // Re-initialize the low registers:

    // Reset both timers and enable interrupts:
    OPL_WriteRegister(context, OPL_REG_TIMER_CTRL,      0x60);
    OPL_WriteRegister(context, OPL_REG_TIMER_CTRL,      0x80);

    // "Allow FM chips to control the waveform of each operator":
    OPL_WriteRegister(context, OPL_REG_WAVEFORM_ENABLE, 0x20);

    if (opl3)
    {
        OPL_WriteRegister(context, OPL_REG_NEW, 0x01);

        // Initialize level registers

        for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(context, r | 0x100, 0x3f);
        }

        // Initialize other registers
        // These two loops write to registers that actually don't exist,
        // but this is what Doom does ...
        // Similarly, the <= is also intenational.

        for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(context, r | 0x100, 0x00);
        }

        // More registers ...

        for (r=1; r < OPL_REGS_LEVEL; ++r)
        {
            OPL_WriteRegister(context, r | 0x100, 0x00);
        }
    }

    // Keyboard split point on (?)
    OPL_WriteRegister(context, OPL_REG_FM_MODE,         0x40);

    if (opl3)
    {
        OPL_WriteRegister(context, OPL_REG_NEW, 0x01);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void OPL_SetSampleRate(opl_context_t *context, unsigned int rate)
{
    // Just record the request; the chip is (re-)created lazily inside
    // OPL_Render() so that all chip access happens on the audio thread.

    if (rate > 0)
    {
        context->desired_rate = rate;
    }
}

opl_context_t *OPL_Create(opl_init_result_t *chip_type)
{
    opl_context_t *context;

    context = calloc(1, sizeof(opl_context_t));

    if (context == NULL)
    {
        if (chip_type != NULL)
        {
            *chip_type = OPL_INIT_NONE;
        }
        return NULL;
    }

    dg_mutex_init (&context->lock);

    context->queue = OPL_Queue_Create();

    // On a memory-constrained target either allocation can fail; bail out
    // rather than fault on the first lock or enqueue. Only the FreeRTOS lock
    // is a nullable handle - the Win32/pthread ones are structs.

#if defined(ESP_PLATFORM)
    if (context->lock == NULL || context->queue == NULL)
#else
    if (context->queue == NULL)
#endif
    {
        if (context->queue != NULL)
        {
            OPL_Queue_Destroy (context->queue);
        }
        dg_mutex_destroy (&context->lock);
        free (context);

        if (chip_type != NULL)
        {
            *chip_type = OPL_INIT_NONE;
        }
        return NULL;
    }
    context->current_time = 0;
    context->pause_offset = 0;
    context->paused = 0;
    context->register_num = 0;
    context->opl3mode = 0;

    context->timer1.rate = 12500;
    context->timer2.rate = 3125;

    context->desired_rate = 44100;
    OPL3_Reset(&context->chip, context->desired_rate);
    context->mixing_freq = context->desired_rate;

    if (chip_type != NULL)
    {
        *chip_type = OPL_INIT_OPL3;
    }

    return context;
}

void OPL_Destroy(opl_context_t *context)
{
    if (context == NULL)
    {
        return;
    }

    if (context->queue != NULL)
    {
        OPL_Queue_Destroy(context->queue);
        context->queue = NULL;
    }

    // The ymfm-backed chip holds a heap allocation behind an opaque pointer;
    // freeing the context alone would leak it.
    OPL3_Destroy(&context->chip);

    dg_mutex_destroy (&context->lock);

    free(context);
}

// ---------------------------------------------------------------------------
// Timer callbacks.
// ---------------------------------------------------------------------------

void OPL_SetCallback(opl_context_t *context, uint64_t us,
                     opl_callback_t callback, void *data)
{
    OPL_Lock(context);

    if (context->queue != NULL)
    {
        OPL_Queue_Push(context->queue, callback, data,
                       context->current_time - context->pause_offset + us);
    }

    OPL_Unlock(context);
}

void OPL_ClearCallbacks(opl_context_t *context)
{
    OPL_Lock(context);

    if (context->queue != NULL)
    {
        OPL_Queue_Clear(context->queue);
    }

    OPL_Unlock(context);
}

void OPL_AdjustCallbacks(opl_context_t *context, float factor)
{
    OPL_Lock(context);

    if (context->queue != NULL)
    {
        OPL_Queue_AdjustCallbacks(context->queue, context->current_time, factor);
    }

    OPL_Unlock(context);
}

void OPL_SetPaused(opl_context_t *context, int paused)
{
    OPL_Lock(context);
    context->paused = paused;
    OPL_Unlock(context);
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------

// Advance the virtual clock by nsamples samples, firing any timer callbacks
// that fall due.  The lock is assumed to already be held.

static void AdvanceTime(opl_context_t *context, unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    us = ((uint64_t) nsamples * OPL_SECOND) / context->mixing_freq;
    context->current_time += us;

    if (context->paused)
    {
        context->pause_offset += us;
    }

    // Invoke any callbacks that are now due.

    while (!OPL_Queue_IsEmpty(context->queue)
        && context->current_time
            >= OPL_Queue_Peek(context->queue) + context->pause_offset)
    {
        if (!OPL_Queue_Pop(context->queue, &callback, &callback_data))
        {
            break;
        }

        // The callback may itself schedule further callbacks and write
        // registers; that is fine as the lock is recursive.

        callback(callback_data);
    }
}

void OPL_Render(opl_context_t *context, int16_t *buffer, unsigned int nsamples)
{
    unsigned int filled = 0;

    OPL_Lock(context);

    // Re-create the chip if the host has changed the sample rate.  Resetting
    // wipes the register state, so the baseline registers are re-programmed;
    // any currently-sounding notes are re-triggered by subsequent events.

    if (context->desired_rate != context->mixing_freq && context->desired_rate != 0)
    {
        OPL3_Reset(&context->chip, context->desired_rate);
        context->mixing_freq = context->desired_rate;
        OPL_InitRegisters(context, context->opl3mode);
    }

    while (filled < nsamples)
    {
        uint64_t next_callback_time;
        uint64_t ns;

        // Work out how many samples we can generate before the next timer
        // callback is due.

        if (context->paused || OPL_Queue_IsEmpty(context->queue))
        {
            ns = nsamples - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(context->queue)
                               + context->pause_offset;

            if (next_callback_time <= context->current_time)
            {
                ns = 0;
            }
            else
            {
                ns = (next_callback_time - context->current_time)
                   * context->mixing_freq;
                ns = (ns + OPL_SECOND - 1) / OPL_SECOND;
            }

            if (ns > nsamples - filled)
            {
                ns = nsamples - filled;
            }
        }

        if (ns > 0)
        {
            OPL3_GenerateStream(&context->chip, buffer + filled * 2,
                                (uint32_t) ns);
            filled += (unsigned int) ns;
        }

        AdvanceTime(context, (unsigned int) ns);
    }

    OPL_Unlock(context);
}
