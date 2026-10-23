# Hardware

## Target device — M5Stack Tab5

| Property | Value |
|----------|-------|
| SoC | ESP32-P4 |
| Display | 1280×720 MIPI DSI (built-in) |
| OS | Tactility RTOS / FreeRTOS |
| Port A | Grove port in I2C mode — SDA=53, SCL=54 |

All external hardware hangs off **Port A** via the PaHub multiplexer.

---

## Wiring

```
Tab5 Port A
    └─ PaHub (TCA9548A, 0x70)
           ├─ CH4 ── 8Encoder Unit  (0x41)
           └─ CH5 ── LCD Unit       (0x3E)
```

The app probes all six PaHub channels and uses the first matching unit it finds, so CH4/CH5 are the usual wiring rather than a hard requirement.

Both units are **optional**. If the PaHub is not detected the app operates in touch-only mode.
If the PaHub is present but a unit is missing, that unit is silently disabled.

---

## PaHub channel map

| Channel | Unit | I2C address |
|---------|------|-------------|
| CH4 | 8Encoder V2 (typical) | 0x41 |
| CH5 | LCD Unit (135×240, typical) | 0x3E |
| CH0–3 | unused / available | — |

The app scans CH0–CH5 at startup, so either unit can be moved to another free channel.

---

## I2C unit summary

### 8Encoder V2 (0x41)
8 rotary encoders with push-buttons, individually addressable RGB LEDs (3-byte RGB), and one toggle switch with its own LED.

Key registers:

| Register | Description |
|----------|-------------|
| `0x20 + i*4` | Counter delta since last read (int32_t, 4 pulses/detent) |
| `0x50 + i` | Button state (0 = released, non-zero = pressed) |
| `0x60` | Toggle-switch state (0 = off, 1 = on) |
| `0x70 + i*3` | Encoder LED RGB (3-byte stride, `i` = 0–7) |
| `0x88` | Toggle-switch LED RGB (3 bytes) |

### PaHub v2 (0x70 — TCA9548A)
TCA9548A-based I2C multiplexer with **6 exposed channels (CH0–CH5)**; the chip itself supports 8, but only six ports are wired on the unit. Write a single bitmask byte to select a channel; write `0x00` to deselect all.

```
select(ch)  →  write(1 << ch)
deselect()  →  write(0x00)
```

### LCD Unit (0x3E — ESP32-PICO inside)
135×240 IPS display (portrait default). The internal ESP32 bridges I2C commands directly to an ST7789V2.
Unlike the STM32-based units it uses a **command-buffer protocol** — no STOP+delay pattern needed.

Key commands:

| Command | Description |
|---------|-------------|
| `0x22 [brightness]` | Set backlight (0=off, 255=max) |
| `0x36 [rotation]` | Set rotation (0–3 = 0°/90°/180°/270°) |
| `0x6A [x0][y0][x1][y1][hi][lo]` | Fill rectangle with inline RGB565 colour |
| `0x62 [x][y][hi][lo]` | Draw single pixel RGB565 |
| `0x2A [x0][x1]` / `0x2B [y0][y1]` | Set active window (column / row range) |
| `0x42 [pixels…]` | Write raw RGB565 stream into active window |
| `0x09` | Read buffer-remaining count (1-byte response) |

Used in portrait orientation (rotation 0): **135 px wide × 240 px tall**.

### ByteButton (0x47) — available, not wired
8 push-buttons with 4-byte LE RGB888 LEDs per button.

| Register | Description |
|----------|-------------|
| `0x00` | Button bitmask (bit i = button i) |
| `0x60 + i` | Individual button state |
| `0x20 + i*4` | LED colour [B, G, R, 0x00] |
| `0x19 = 0x00` | Show-mode register (must be written on init) |

### Joystick2 (0x63) — available, not wired
Dual-axis thumbstick with button and RGB LED.

| Register | Description |
|----------|-------------|
| `0x50` | XY 12-bit offset-corrected (2×int16) |
| `0x20` | Button (0 = pressed) |
| `0x30` | LED LE uint32 RGB |

### Scroll (0x40) — available, not wired
Single rotary encoder with button and RGB LED.

| Register | Description |
|----------|-------------|
| `0x50` | Delta since last read (int16_t, resets on read) |
| `0x20` | Button (0 = pressed) |
| `0x30` | LED [0x00, R, G, B] |
| `0x40` | Reset encoder counter (write 1) |

---

## I2C protocol notes

**STM32-based units** (ByteButton, Joystick2, Scroll, 8Encoder):
- Use a **write-then-read** pattern with a mandatory 2 ms delay between the address write (STOP) and the data read.
- A repeated-START without the STOP causes the STM32 HAL to return stale data.
- Helper: `unitReadReg()` in `UnitCommon.h` handles this automatically.

**ESP32-based units** (LCD Unit):
- Standard I2C — no delay needed. Each write is a self-contained command.
- Pixel streaming ends at the I2C STOP condition.
