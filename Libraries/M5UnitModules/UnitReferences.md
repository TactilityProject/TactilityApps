# M5Unit Module References

Quick reference for all M5Stack units supported by the M5UnitModules library.
Each entry links to the official M5Stack docs and the upstream Arduino library.

---

## PaHub v2.1 (I2C Hub)

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x70 (default), configurable |
| Channels | 6 (CH0-CH5) |
| Driver class | `UnitPaHub` |

PCA9548A-based I2C multiplexer. Allows up to 6 I2C devices with the same
address to share a single bus. Select a channel before talking to a device
behind it; deselect (mask = 0) when done.

- Docs: https://docs.m5stack.com/en/unit/Unit-PaHub%20v2.1
- Repo: https://github.com/m5stack/M5Unit-HUB

**Register map:**

| Addr | Operation | Description |
|------|-----------|-------------|
| 0x70 | W: bitmask | Enable channels (bit i = channel i). Write 0x00 to deselect all. |

---

## CardKB2 (Keyboard)

| Property | Value |
|---|---|
| Interface | I2C (default) or UART |
| I2C Address | 0x5F |
| UART | 115200-8N-1, via GROVE port |
| MCU | STM32 |
| Driver class | `UnitCardKB2` |

Full QWERTY keyboard in a credit-card form factor. The STM32 firmware
auto-repeats keys (300 ms initial delay, 50 ms repeat). Mode switching
(I2C/UART/ESP-NOW/BLE-HID) is done via Fn+Sym+[1-4] and persists across
reboots.

- Docs: https://docs.m5stack.com/en/unit/Unit_CardKB2
- Repo: https://github.com/m5stack/M5Unit-KEYBOARD

**I2C protocol:** single I2C read (no register write) returns current key ASCII
value. 0x00 = no key or I2C error (indistinguishable by design).

**UART protocol:** 5-byte frames: `AA 03 [KEY_ID] [KEY_STATE] [checksum]`
- `KEY_ID` = row × 11 + col (0–43, rows 0–3, cols 0–10)
- `KEY_STATE` = 0x01 pressed, 0x02 released
- `checksum` = (0x03 + KEY_ID + KEY_STATE) & 0xFF
- The driver tracks Aa/Fn/Sym modifier state internally and translates KEY_ID to ASCII.

**Special keys:**
- `Aa` key: tap = one-shot uppercase, double-tap = caps lock
- `Sym` key: toggles symbol mode (Aa ineffective while active)
- `Fn+D/X/Z/C` = cursor up/down/left/right (0x1E/0x1F/0x1D/0x1C), `Fn+1` = Esc (0x1B)

**Mode behaviour for Fn combos:**
- **UART mode:** Fully supported. The firmware sends the raw KEY_ID in the UART frame; our driver translates it via `fnCombo()` to the private ASCII codes above.
- **I2C mode:** No output. The firmware's I2C slave queue only holds regular ASCII; Fn-combo presses route to the BLE HID path and are not queued for I2C reads.
- **BLE HID mode:** Standard USB HID keycodes (ESC=0x29, Up=0x52, Down=0x51, Left=0x50, Right=0x4F), mapped to `LV_KEY_*` by `BluetoothHidHost`.

---

## LCD Unit (Color LCD)

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x3E |
| MCU | ESP32-PICO (inside) |
| Display | ST7789V2, 135x240 IPS |
| Driver class | `UnitLcd` |

Internal ESP32 bridges I2C commands to the ST7789V2. Commands are
fire-and-forget (the ESP32 processes them asynchronously). For bulk pixel
writes, poll `bufferRemaining()` (register 0x09) to avoid overflow.

- Docs: https://docs.m5stack.com/en/unit/lcd
- M5GFX repo: https://github.com/m5stack/M5GFX

**Key commands:**

| Cmd | Bytes | Description |
|-----|-------|-------------|
| 0x22 | [brightness] | Set backlight (0=off, 255=max) |
| 0x36 | [rotation] | Set rotation (0=portrait, 1=landscape, 2=portrait flip, 3=landscape flip) |
| 0x6A | [x0][y0][x1][y1][hi][lo] | Fill rect with RGB565 inline |
| 0x62 | [x][y][hi][lo] | Draw pixel RGB565 |
| 0x42 | [pixels...] | Write raw RGB565 stream |
| 0x09 | (read 1 byte) | Buffer bytes remaining |
| 0x68 | [x0][y0][x1][y1] | Fill rect with stored colour |

Physical dimensions: 135 wide x 240 tall (portrait). Rotation 1/3 swaps to 240 wide x 135 tall.
Max I2C clock: 400 kHz.

---

## Scroll Unit (Encoder + LED)

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x40 |
| MCU | STM32 |
| Driver class | `UnitScroll` |

Single rotary encoder with push button and one RGB LED.

- Docs: https://docs.m5stack.com/en/unit/UNIT-Scroll
- Repo: https://github.com/m5stack/M5Unit-Scroll

**Register map:**

| Addr | R/W | Width | Description |
|------|-----|-------|-------------|
| 0x10 | R/W | int16_t | Absolute encoder value |
| 0x20 | R | uint8_t | Button state (0=pressed, 1=released - inverted!) |
| 0x30 | R/W | 4 bytes | LED: [index(0), R, G, B] |
| 0x40 | W | uint8_t | Write 1 to reset encoder to 0 |
| 0x50 | R | int16_t | Delta since last read (resets after read) |
| 0xFE | R | uint8_t | Firmware version |
| 0xFF | R/W | uint8_t | I2C address |

---

## Joystick 2 Unit

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x63 |
| MCU | STM32 |
| Driver class | `UnitJoystick2` |

Dual-axis thumbstick with push button and one RGB LED. 12-bit ADC resolution.

- Docs: https://docs.m5stack.com/en/unit/Unit-JoyStick2
- Repo: https://github.com/m5stack/M5Unit-Joystick2

**Register map (key registers):**

| Addr | R/W | Width | Description |
|------|-----|-------|-------------|
| 0x00 | R | uint8_t | X axis (8-bit, 0-255) |
| 0x01 | R | uint8_t | Y axis (8-bit, 0-255) |
| 0x02 | R | uint8_t | Button (0=pressed, 1=released) |
| 0x10 | R | int16_t | X axis 12-bit signed |
| 0x12 | R | int16_t | Y axis 12-bit signed |
| 0x20 | R/W | 3 bytes | LED [R, G, B] |
| 0xFE | R | uint8_t | Firmware version |

---

## Dual Button Unit

| Property | Value |
|---|---|
| Interface | GPIO |
| Signals | 2x digital input (active LOW) |
| Colors | Red (Button A), Blue (Button B) |
| Driver class | `UnitDualButton` |

Simple two-button unit. Both buttons are active-LOW (pressed = 0V). The
default GPIO pins depend on the Grove port used:

- Docs: https://docs.m5stack.com/en/unit/dual_button
- Repo: https://github.com/m5stack/M5Stack/tree/master/examples/Unit/DUAL_BUTTON

**Pin assignment (Port B):** GPIO varies by device. The driver takes pin
numbers at `begin()` - pass the actual GPIO numbers for the target hardware.

---

## 8Encoder Unit

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x41 |
| MCU | STM32 |
| Driver class | `Unit8Encoder` |

8 rotary encoders with push buttons, 8 individual RGB LEDs, one toggle switch,
and one RGB LED for the switch. Total 9 addressable LEDs.

- Docs: https://docs.m5stack.com/en/unit/8Encoder
- Repo: https://github.com/m5stack/M5Unit-8Encoder

**Register map (V1 firmware, 2022-11-08):**

| Addr | R/W | Width | Description |
|------|-----|-------|-------------|
| 0x00 + i*4 | R/W | int32_t | Absolute counter for encoder i (0-3) |
| 0x10 + i*4 | R/W | int32_t | Absolute counter for encoder i (4-7) |
| 0x20 + i*4 | R | int32_t | Delta for encoder i (0-3), resets after read |
| 0x30 + i*4 | R | int32_t | Delta for encoder i (4-7), resets after read |
| 0x40 + i | W | uint8_t | Write 1 to reset counter i (i = 0-7) |
| 0x50 + i | R | uint8_t | Button state for encoder i (0=released, 1=pressed) |
| 0x60 | R | uint8_t | Toggle switch state (0=off, 1=on) |
| 0x70 + i*3 | R/W | 3 bytes | LED [R, G, B] for encoder i (i = 0-7) |
| 0x88 | R/W | 3 bytes | LED [R, G, B] for toggle switch (LED8) |
| 0xF0 | R | uint8_t | Firmware version |
| 0xFF | R/W | uint8_t | I2C address |

Hardware counts 4 pulses per detent; the driver divides by 4 to give
detent-level deltas. Counter range: -2,147,483,648 to +2,147,483,647.

**LED indices in `Unit8Encoder`:** 0-7 = encoder LEDs, 8 = switch LED.

---

## ByteButton Unit

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x47 |
| MCU | STM32 |
| Driver class | `UnitByteButton` |

8 momentary buttons with individual RGB LEDs, each with independently
controllable brightness.

- Docs: https://docs.m5stack.com/en/unit/Unit%20ByteButton
- Repo: https://github.com/m5stack/M5Unit-ByteButton

**Register map:**

| Addr | R/W | Width | Description |
|------|-----|-------|-------------|
| 0x00 | R | uint8_t | All 8 buttons as bitmask (bit i = button i, 1=pressed) |
| 0x10 + i | R/W | uint8_t | LED brightness for button i (0-255) |
| 0x19 | R/W | uint8_t | LED show mode (0=user-defined, 1=system default) |
| 0x20 + i*4 | R/W | uint32_t | LED RGB colour (LE: bytes = [B, G, R, 0x00]) |
| 0x50 + i | R/W | uint8_t | LED colour RGB233 compressed |
| 0x60 + i | R | uint8_t | Individual button state (0=released, non-zero=pressed) |
| 0x70 + i*4 | R/W | uint32_t | System "off" colour for button i |
| 0x90 + i*4 | R/W | uint32_t | System "on" colour for button i |
| 0xFE | R | uint8_t | Firmware version |
| 0xFF | R/W | uint8_t | I2C address |

**LED colour format:** write `uint32_t 0x00RRGGBB` as little-endian - wire
sees [BB, GG, RR, 0x00]. Set show mode to 0 (user-defined) before
`setLed`/`flushLeds` to take effect.

---

## MIDI Unit / Synth Unit

| Property | Value |
|---|---|
| Interface | UART |
| Baud rate | 31250 bps, 8N1 |
| IC | SAM2695 |
| Polyphony | 64 voices (38 with effects) |
| Channels | 16 MIDI channels |
| Driver class | `UnitMidi` |

Both the MIDI Unit and Synth Unit use the SAM2695 connected via UART. Pass a
pre-opened `Uart` instance to `begin()`. The driver issues a system reset
(0xFF) on init.

- Synth Unit docs: https://docs.m5stack.com/en/unit/Unit-Synth
- MIDI Unit docs: https://docs.m5stack.com/en/unit/Unit-MIDI
- Repo: https://github.com/m5stack/M5Unit-Synth

**MIDI 1.0 message formats:**

| Message | Bytes | Description |
|---------|-------|-------------|
| 0x80\|ch | note, 0 | Note Off |
| 0x90\|ch | note, velocity | Note On (velocity 0 = Note Off) |
| 0xB0\|ch | controller, value | Control Change |
| 0xC0\|ch | program | Program Change |
| 0xE0\|ch | LSB, MSB | Pitch Bend (-8192 to +8191, centre = 0x2000) |
| 0xFF | - | System Reset |
| 0xB0\|ch | 123, 0 | All Notes Off (CC 123) |

**General MIDI program numbers (partial):**

| Range | Category |
|-------|----------|
| 0-7 | Piano |
| 8-15 | Chromatic Percussion |
| 24-31 | Guitar |
| 32-39 | Bass |
| 40-47 | Strings |
| 56-63 | Brass |
| 73-79 | Flute/Recorder/Pipe |
| 80-87 | Synth Lead |
| 88-95 | Synth Pad |

---

## RFID 2 Unit

| Property | Value |
|---|---|
| Interface | I2C |
| I2C Address | 0x28 |
| IC | MFRC522 (I2C mode) |
| Standards | ISO/IEC 14443 A/B, MIFARE |
| Frequency | 13.56 MHz |
| Driver class | `UnitRfid2` |

MFRC522 contactless card reader in I2C mode. Supports UID reading for MIFARE
Classic, MIFARE Ultralight, and ISO 14443-A cards.

- Docs: https://docs.m5stack.com/en/unit/rfid2
- MFRC522 I2C lib: https://github.com/kkloesener/MFRC522_I2C

**Notes:**
- The MFRC522 supports SPI, I2C, and UART; this unit hardwires I2C mode.
- UID length: 4 bytes (single-size) or 7 bytes (double-size) depending on card.
- Always check `readUID()` return length; 0 = no card present or read error.

---

## Driver Common Notes

All drivers use `UnitCommon.h` helpers:

- `unitProbe(dev, addr)` - checks for device ACK at address
- `unitReadReg(dev, addr, reg, buf, len)` - STM32-style: write reg byte, then read
- `unitWriteReg(dev, addr, reg, buf, len)` - write reg byte followed by data
- `UNIT_I2C_TIMEOUT_MS` = 20 ms default timeout

The STM32-based units (8Encoder, ByteButton, Scroll, Joystick2, CardKB2)
require a 2 ms delay between write (reg select) and read in some cases. This
is handled internally by the I2C controller driver.

When using the PaHub, always call `selectIfNeeded()` **before** `isPresent()`
to ensure the correct channel is active for the ACK check.
