/*
 * LcdDisplay.h - per-mode LCD content for the M5Stack LCD Unit (135×240)
 *
 * The LCD unit usually sits on PaHub CH5, but lcdInit() probes every channel.
 * All public functions select the discovered channel, draw, then leave it
 * selected. EncoderInput re-selects the encoder channel before it reads, so
 * there is no conflict as long as they don't run concurrently - both are
 * called from the same LVGL timer thread.
 *
 * Layout per mode (portrait 135×240, white text on dark background):
 *
 *   HOME / idle:
 *     Full-screen mode name + "MIDI CTRL" branding
 *
 *   RAGA:
 *     Large raga name, tala, root note, BPM, play/drone status
 *
 *   DIMENSIONS:
 *     A B C D values as a 2×2 grid with colour bars, equation, BPM
 *
 *   TB3PO:
 *     32-step piano-roll mini view (current step highlighted), key, scale, BPM
 *
 *   BABY8:
 *     8-step note grid - note name per step, current step highlighted, gates
 *
 *   EUCLID:
 *     Selected voice name + params (steps/events/rotation/note), BPM
 */
#pragma once

#include <UnitPaHub.h>
#include <UnitLcd.h>
#include "MidiController.h"

// Initialise LCD subsystem. Pass the already-initialised PaHub.
// Returns false if the LCD unit is not found on any PaHub channel.
bool lcdInit(UnitPaHub* hub);

// Release resources (called from encoderStop).
void lcdStop();

bool lcdIsPresent();

// Called from switchToMode - clears screen and draws static mode chrome.
void lcdSetMode(MidiMode mode);

// Called from each dispatch function after state changes.
void lcdUpdate(MidiController* mc);
