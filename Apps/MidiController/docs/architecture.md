# Architecture

## Source layout

```
Apps/MidiController/main/Source/
├── MidiController.h          - mode enum, all state structs, class declaration
├── MidiController.cpp        - mode switching, HOME view, BLE/USB MIDI, app lifecycle
├── main.cpp                  - Tactility app entry point
├── MidiScales.h              - scale/key definitions and MIDI note helpers
├── UiKit.h                   - shared colours, metrics, and widget builders
├── GroveLookup.h / .cpp      - locate the Grove I2C controller device
├── Sequencer.h / .cpp        - 4-track × 16-step drum sequencer (used by BEATS)
├── EncoderInput.cpp          - 8Encoder poll timer, per-mode knob/button dispatch
├── LcdDisplay.h / .cpp       - LCD Unit display management, per-mode draw functions
├── ViewKeys.cpp              - KEYS mode UI
├── ViewBeats.cpp             - BEATS mode UI
├── ViewRaga.cpp              - RAGA mode UI + encoder callbacks
├── ViewDimensions.cpp        - DIMENSIONS mode UI + encoder callbacks
├── ViewBaby8.cpp             - BABY8 mode UI
├── ViewTb3po.cpp             - TB3PO mode UI + encoder callbacks
├── ViewEuclid.cpp            - EUCLID mode UI + encoder callbacks
├── ViewZen.cpp               - ZEN mode UI
├── ViewDrop.cpp              - DROP mode UI
├── ViewArp.cpp               - ARP mode UI
├── ViewChord.cpp             - CHORD mode UI
├── ViewLfo.cpp               - LFO mode UI
├── ViewRng.cpp               - RNG mode UI
├── ViewXYPad.cpp             - XY mode UI
└── ViewGrid.cpp              - GRID mode UI

Libraries/M5UnitModules/
├── Include/
│   ├── UnitCommon.h           - shared I2C helpers (STM32 STOP+delay pattern)
│   ├── Unit8Encoder.h         - 8Encoder driver
│   ├── UnitPaHub.h            - PaHub (PCA9548AP) multiplexer driver
│   ├── UnitLcd.h              - LCD Unit driver
│   ├── UnitByteButton.h       - ByteButton driver
│   ├── UnitJoystick2.h        - Joystick2 driver
│   ├── UnitScroll.h           - Scroll driver
│   └── UnitCardKB2/DualButton/Midi/Rfid2 headers
└── Source/
    └── matching Unit*.cpp implementations
```

`Apps/MidiController/main/CMakeLists.txt` uses `GLOB_RECURSE` for both the app's `Source/*.c*` files and `Libraries/M5UnitModules/Source/*.c*`, so every `.c`/`.cpp` file in those trees is compiled automatically.

---

## Mode lifecycle

```
createWidgets()  (Tactility window factory)
  └─ createWidgetsImpl()
       1. toolbar + content area
       2. create Sequencer and its step/tick callbacks
       3. detect an already-running BLE MIDI session
       4. buildHomeView(), or show the transport chooser on first launch
       5. encoderInit()   - probe PaHub → 8Encoder + LCD; start the
                            100 ms poll timer if either unit is present

The shared 20 ms UI timer is not kept alive for every mode. `switchToMode()`
creates it only when entering BEATS (step-cursor highlighting and drum
note-offs); leaving BEATS deletes it in `stopAllModeTimers()`.

user taps tile
  └─ switchToMode(mode)
       1. close the transport chooser overlay, if open
       2. stopAllModeTimers()
       3. stop the BEATS sequencer when leaving BEATS
       4. sendMidi(0xB0, 123, 0)        - All Notes Off
       5. null all widget pointers      - prevents callbacks seeing stale widgets
       6. encoderSetAllLeds(0); encoder_.btnSeedNext = true
       7. lv_indev_wait_release()       - discard in-flight touch (pre-build)
       8. lv_obj_del(currentViewRoot_)  - destroy old view tree
       9. currentMode_ = mode; lcdSetMode(mode) + lcdUpdate()
      10. build<Mode>View()             - create new LVGL tree
      11. start the 20 ms UI timer when mode == BEATS
      12. lv_indev_wait_release()       - discard any touch still in-flight

teardown()  (after the window is removed)
  └─ encoderStop()        - stop poll timer, LEDs off, lcdStop()
  └─ stopAllModeTimers()  - also deletes the BEATS UI timer
  └─ stop/reset the Sequencer
  └─ stop BLE/USB, unsubscribe BT events, release device references
  └─ resetAllState()      - reset all state structs and UI pointers
```

### Why `lv_indev_wait_release` is called twice

A tap on a home tile fires `switchToMode`. The finger-up event from that same tap can land on a button in the freshly built view if both share the same screen coordinates. The first `wait_release` runs before view construction but LVGL may still queue the event. A second call after construction ensures it is discarded before any button in the new view can process it.

---

## State management

Each mode has its own state struct nested inside `MidiController` (e.g. `RagaState raga_`). Data fields (bpm, notes, parameters) **persist** across mode switches; LVGL widget pointers are **nulled** before the old tree is destroyed and **re-populated** when the new view is built.

`resetAllState()` (called from `teardown()`) resets every per-mode state struct and its UI pointers to defaults.

---

## MIDI transport

MIDI is sent over either Bluetooth LE or USB. On launch the app shows a transport chooser (unless BLE MIDI is already running). The toolbar `USB`/`BLE` button reopens the chooser while sending is stopped. The choice is stored in `activeTransport_` for the session.

The toolbar switch calls `handleTransportToggle(true/false)`, which starts or stops the selected transport.

**BLE:** uses Tactility's `bluetooth_midi` driver.
1. If the BT radio is off, `bluetooth_set_radio_enabled(true)` is called and `startBleMidi()` runs from the `BT_RADIO_STATE_ON` event callback.
2. `bluetooth_midi_start()` enables the MIDI profile and the app advertises as **Tactility MIDI**. The name is set before the profile starts, so the radio switches to the MIDI identity (own address and bond) when it is already on under another app's identity.
3. `sendMidi(cmd, d1, d2)` calls `bluetooth_midi_send()`.

**USB:** `startUsbMidi()` calls `usb_midi_device_start()` and `sendMidi()` calls `usb_midi_device_send()`.

On stop or hide, `CC 123` (All Notes Off) is sent before the transport is stopped. Every `*_get()` helper returns a referenced `Device`, so each caller pairs it with `device_put()`.

---

## LVGL patterns used

### Canvas-style drawing
ZEN, DROP, LFO, XY, TB3PO, and EUCLID draw into a plain container via
`LV_EVENT_DRAW_MAIN`. No `lv_canvas` buffer is allocated - the container is
freed with the view tree.

TB3PO and EUCLID are sequencers, so their canvas is invalidated on every step.
They deliberately **draw the grid/rings directly** instead of deleting and
recreating child widgets: the earlier implementation churned through hundreds
of LVGL objects per beat, which could starve the idle task and trip the task
watchdog on slower targets. State changes only call `lv_obj_invalidate()`.

ZEN and DROP move a handful of ball widgets. Moving a child invalidates its old
and new areas, and LVGL repaints the canvas background there, so those timers
avoid full-canvas invalidation unless the static scene changes (a wall flash,
platform edit, or label/note change).

### Touch handling
KEYS uses `LV_EVENT_PRESSING` on the piano row plus per-key `PRESSED`/`RELEASED` handlers so a finger can slide across keys. XY uses `LV_EVENT_PRESSING` on the pad and derives CC values from finger X/Y. GRID uses per-cell `PRESSED`/`RELEASED` handlers with one held note per pointer slot.

### Encoder poll timer
`EncoderInput.cpp` runs a single 100 ms LVGL timer. On each tick:
1. Check `btnSeedNext` - if set, read buttons silently to seed `btnPrev` and return.
2. Route to the per-mode dispatch function based on `currentMode_`.
3. Dispatch reads deltas + buttons, updates state, refreshes LVGL labels, flushes LEDs, calls `lcdUpdate`.

### LED dirty-caching
`Unit8Encoder::flushLeds` maintains a 9-entry colour cache (8 encoder LEDs + 1 switch LED). It sends the 24-byte encoder-LED burst only if one of those eight changed, and writes the switch LED separately only if that entry changed.

---

## MIDI messages sent

| Command | Hex | Used for |
|---------|-----|----------|
| Note On | `0x90` | All melodic/drum note triggers |
| Note Off | `0x80` | All note releases |
| Control Change | `0xB0` | XY pad (CC 74/71 default), LFO modulation, All Notes Off (CC 123) |
| Pitch Bend | `0xE0` | LFO in pitch-bend mode |

All messages are sent on channel 1 (command bytes `0x90`/`0x80`/`0xB0`/`0xE0` without channel offset).
