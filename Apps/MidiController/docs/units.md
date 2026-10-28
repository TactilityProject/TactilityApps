# I2C Unit Drivers

The unit drivers live in `Libraries/M5UnitModules/`:

- `Include/UnitXxx.h`
- `Source/UnitXxx.cpp`

The app compiles them through `Apps/MidiController/main/CMakeLists.txt`, which globs both the app's `Source/*.c*` files and `../../../Libraries/M5UnitModules/Source/*.c*`.

Each I2C driver follows the same pattern:

- `begin(Device* dev, uint8_t addr = DEFAULT_ADDR)` - probes the device; returns `false` if not found
- Methods are no-ops (or return `false`) if `begin()` was not called or returned `false`
- No exceptions, no dynamic allocation

`UnitMidi` is the exception: it drives a SAM2695 over a UART at 31250 bps, so its `begin()` takes a UART controller instead of an I2C address.

---

## UnitCommon.h

Shared helpers for **STM32-based** M5Stack units (8Encoder, ByteButton, Joystick2, Scroll).

STM32 units use a register-pointer protocol: write the register address (STOP), wait 3 ms, then read the data. A repeated-START without the STOP will return stale or zero data because the STM32 HAL fires its receive callback on the STOP condition.

```cpp
// Write data to a register. len is limited to UNIT_MAX_WRITE_PAYLOAD (32 bytes).
bool unitWriteReg(Device* dev, uint8_t addr,
                  uint8_t reg, const uint8_t* buf, uint16_t len);

// Write just the register pointer (no data) - prepares for a subsequent read
bool unitWriteRegPtr(Device* dev, uint8_t addr, uint8_t reg);

// Full read sequence: writeRegPtr → vTaskDelay(3ms) → read
bool unitReadReg(Device* dev, uint8_t addr,
                 uint8_t reg, uint8_t* buf, uint16_t len);

// Perform a harmless identity read and validate the response
bool unitProbe(Device* dev, uint8_t addr);
```

`unitProbe` does **not** call `i2c_controller_has_device_at_address()`. On ESP-IDF 6.1 that helper uses `i2c_master_probe()`, which can report success for an absent address when the bus FSM is left in a non-error state. Behind a PaHub this made every known address appear to ACK on every channel, and the repeated probes could also wedge the bus so that a following `deselect()` failed. Instead, `unitProbe` performs a harmless real transaction and checks that it completes:

- PaHub (`0x70`): read the control register.
- 8Encoder (`0x41`), Scroll (`0x40`), ByteButton (`0x47`), Joystick2 (`0x63`): write register pointer `0xFE`, wait 3 ms, then read the firmware version. Only versions 1-15 are accepted.
- LCD (`0x3E`): write command `0x04` (READ_ID), then read four bytes. The first two must be `0x77 0x89`.
- CardKB2 (`0x5F`): write `0xF1`, wait, then read the firmware version (1-15). A plain read is deliberately avoided because it would consume a queued key.
- RFID2 (`0x28`): write `0x37`, wait, then read the `VersionReg`; `0x15` is the WS1850S in the RFID2 Unit, and `0x88`, `0x90`, `0x91`, `0x92`, or `0x12` are accepted for MFRC522/PN512-family readers.
- Other STM32 register-based units: write only a register pointer (`0x00`); no data follows, so no register value changes.

A NACK, timeout, or invalid identity value is a miss. Each failed probe is retried up to five times with a 2 ms gap, because an STM32 slave can briefly NACK while it re-arms its I2C listener. `begin()` still validates the device with a real register access. The 8Encoder checks both its firmware version (`0xFE`) and its own address register (`0xFF`); the LCD checks its four-byte ID; CardKB2 and RFID2 re-read their version registers. These checks are what prevent a floating PaHub channel from latching the scan onto an empty port: a write-only transaction can appear to ACK when the downstream bus has weak pull-ups, but an absent device cannot return the expected identity bytes.

`KNOWN_UNIT_ADDRS` lists the known unit addresses used by the M5UnitTest PaHub scanner: `0x41` (8Encoder), `0x3E` (LCD), `0x40` (Scroll), `0x47` (ByteButton), `0x63` (Joystick2), `0x28` (Rfid2), and `0x5F` (CardKB2). Every entry now has an identity check, so the scanner only lists a channel's devices when the expected version/ID bytes come back. If a scan finds nothing, it disconnects and re-selects the channel once before reporting a miss. MidiController does not scan the full list; it probes `0x41` for the 8Encoder and `0x3E` for the LCD on each of the six PaHub channels through their `begin()` methods.

---

## Unit8Encoder

**I2C address:** `0x41` (default, configurable)
**Protocol:** STM32-based (STOP+delay pattern)

### Register map

| Register | Description |
|----------|-------------|
| `0x00 + i*4` | Absolute counter for knob `i` (int32_t, LE), `i` = 0–7 |
| `0x20 + i*4` | Delta since last read for knob `i` (int32_t, LE), resets on read |
| `0x40 + i` | Write 1 to reset counter `i` |
| `0x50 + i` | Button `i` state (0 = released, 1 = pressed) |
| `0x60` | Toggle-switch state (0 = off, 1 = on) |
| `0x70 + i*3` | Encoder LED `i` RGB (3 bytes: R, G, B), `i` = 0–7 |
| `0x88` | Toggle-switch LED RGB (3 bytes) |
| `0xFE` | Firmware version |
| `0xFF` | I2C address (R/W) |

### API

```cpp
bool begin(Device* dev, uint8_t addr = 0x41);
bool isPresent() const;

// Read all 8 deltas and button states in one call.
// Deltas are returned in detents (hardware counts ÷4).
// Performs 16 individual register reads.
bool readAll(int32_t deltas[8], uint8_t buttons[8]);

// Read the toggle switch. Returns false on I2C error (state is then unchanged).
bool readSwitch(bool& state);

// Set one LED immediately. rgb = 0x00RRGGBB.
// idx 0-7 = encoder LEDs, idx 8 = switch LED.
void setLed(uint8_t idx, uint32_t rgb);
void setSwitchLed(uint32_t rgb);

// Set all 9 LEDs (8 encoders + switch) to the same colour.
void setAllLeds(uint32_t rgb);

// Compare pending[9] against cached state and write only the changed LEDs.
// pending[0-7] = encoder LEDs, pending[8] = switch LED.
void flushLeds(const uint32_t pending[LED_COUNT]);

// Cached colours, index 0-7 = encoders, 8 = switch.
const uint32_t* ledColors() const;

static constexpr uint8_t LED_COUNT = 9;
static constexpr uint8_t ENCODER_LED_COUNT = 8;
```

### Notes

- Delta registers accumulate movement between reads - no encoder ticks are lost even at a 100 ms poll rate.
- `begin()` performs the identity transaction, verifies the firmware version (`0xFE`, 1–15) and the address register (`0xFF`, must equal `0x41`), then retries the encoder-LED and switch-LED init writes up to three times before giving up. The identity check is what prevents a device that merely ACKs at `0x41` from latching the PaHub scan onto the wrong channel; the LED writes are only a final liveness check.
- `flushLeds` sends a 24-byte payload (25-byte transaction) for the eight encoder LEDs and a separate 3-byte payload (4-byte transaction) for the switch LED, but only when the corresponding cached colours changed.

---

## UnitPaHub

**I2C address:** `0x70` (PCA9548AP-based, fixed)
**Protocol:** Direct I2C write (no register pointer)

The module exposes **6 channels (CH0–CH5)**. The PCA9548AP chip itself supports 8 channels, but only six are wired to Grove ports on this unit.

A single-byte bitmask write selects which downstream channel is active. Only one channel should be active at a time.

```cpp
bool begin(Device* dev, uint8_t addr = 0x70);
bool isPresent() const;

// Activate one channel (0–5). Writes (1 << channel) to the device.
bool select(uint8_t channel);

// Deactivate all channels. Writes 0x00.
bool deselect();

uint8_t currentChannel() const;

static constexpr uint8_t NUM_CHANNELS = 6;
static constexpr uint8_t NO_CHANNEL   = 0xFF;
```

### Channel assignments in this app

The app scans **every** software channel (0–5) for the 8Encoder and LCD Unit and uses the first device that returns a valid identity (8Encoder firmware version plus address register; LCD four-byte ID). The identity reads and init writes are retried, because the unit can still be re-arming its I2C listener immediately after the probe. The scan starts at 0 and stops at the first match, so no channel is preferred.

Software channel numbers are zero-based (`select(channel)` writes `1 << channel`). Software CH0 is the first port.

| Software channel | Example unit |
|------------------|----------------------------------|
| CH0 | 8Encoder (0x41) |
| CH5 | LCD Unit (0x3E) |

Always call `select(channel)` before any transaction with a unit on that channel. `select()` skips the write when the requested channel is already cached as active; writing the same mask again briefly disconnects the downstream bus. After a real channel change it writes the mask, waits 3 ms, and reads the control register back for diagnostics. The readback is **not** a hard requirement: once a channel with an STM32 unit is selected, that unit can hold SCL low and block the upstream readback even though the mux latched correctly. `deselect()` writes `0x00` and does require a `0x00` readback, because with no channel selected the upstream bus should be free; that check is also what guards `UnitPaHub::begin()`. A failed write or deselect invalidates the cached channel, so the next `select()` always retries.

---

## UnitLcd

**I2C address:** `0x3E`
**Protocol:** ESP32-internal command buffer (no STOP+delay needed)

See [hardware.md](hardware.md) for the command reference.

```cpp
bool begin(Device* dev, uint8_t addr = 0x3E);
bool isPresent() const;

uint16_t width() const;       // current rotation's logical width
uint16_t height() const;      // current rotation's logical height
uint8_t  rotation() const;

void setBrightness(uint8_t brightness);   // 0=off, 255=max
void setRotation(uint8_t rotation);       // 0=portrait, 1=landscape, 2/3=flipped

void fillScreen(uint16_t rgb565);
void fillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t rgb565);
void drawPixel(uint8_t x, uint8_t y, uint16_t rgb565);

// Set active drawing window (CASET + RASET commands).
bool setWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

// Write raw RGB565 pixels into the active window.
// Sent in chunks of 126 pixels (253 bytes including the command byte).
void writePixels(const uint16_t* pixels, uint32_t len);

void drawHLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565);
void drawVLine(uint8_t x, uint8_t y, uint8_t len, uint16_t rgb565);
void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t rgb565);
void drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565);

void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565);
void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t rgb565);
void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t rgb565);
void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t rgb565);
void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  int16_t x2, int16_t y2, uint16_t rgb565);
void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  int16_t x2, int16_t y2, uint16_t rgb565);

// Angles in degrees (0=right, 90=down, 180=left, 270=up).
void fillArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
             float startDeg, float endDeg, uint16_t rgb565);
void drawArc(int16_t cx, int16_t cy, int16_t r0, int16_t r1,
             float startDeg, float endDeg, uint16_t rgb565);

void drawChar(uint8_t x, uint8_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale = 1);
void drawText(uint8_t x, uint8_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale = 1);

// Query remaining command buffer space (0–255).
// Returns UINT8_MAX on I2C error (treat as "plenty of space" / safe fallback).
uint8_t bufferRemaining();

// Convert 0x00RRGGBB → RGB565.
static uint16_t rgb888to565(uint32_t rgb888);
static uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
```

### Pixel chunking

The ESP32 I2C driver has a 256-byte buffer. `writePixels` splits the stream into chunks of **126 pixels** (252 bytes plus the `0x42` command byte = 253 bytes). Each chunk is a separate I2C transaction.

`begin()` first reads the four-byte ID (`0x04`, expected to start with `0x77 0x89`) and retries up to three times. This identity check stops a false ACK on a floating PaHub channel from being treated as an LCD. It then retries the brightness and rotation commands up to three times, because the LCD controller can still be busy immediately after a PaHub channel switch.

---

## UnitByteButton

**I2C address:** `0x47`
**Protocol:** STM32-based (STOP+delay pattern)

8 push-buttons with per-button RGB LEDs.

```cpp
bool begin(Device* dev, uint8_t addr = 0x47);
bool isPresent() const;

// Read all 8 buttons as a bitmask (bit i = button i, 1=pressed).
// Sets *ok=false on I2C error (ok may be nullptr).
uint8_t readButtons(bool* ok = nullptr);

// Read a single button (true = pressed).
bool readButton(uint8_t idx);

// Set one LED (0x00RRGGBB, idx 0-7) and send it immediately.
void setLed(uint8_t idx, uint32_t rgb);

// Flush pending[8] to hardware in one burst if anything changed.
void flushLeds(const uint32_t pending[8]);

// Set all 8 LEDs to one colour immediately.
void setAllLeds(uint32_t rgb);

const uint32_t* ledColors() const;
```

**Init note:** The show-mode register `0x19` is written `0x00` during `begin()`; otherwise LEDs will not update.

**Wire format:** each LED is sent as a little-endian `uint32_t` `[B, G, R, 0x00]`.

---

## UnitJoystick2

**I2C address:** `0x63`
**Protocol:** STM32-based (STOP+delay pattern)

Dual-axis thumbstick with push-button and RGB LED.

```cpp
bool begin(Device* dev, uint8_t addr = 0x63);
bool isPresent() const;

// Read offset-corrected 12-bit X/Y. Centre = 0, range ~±2048.
bool readXY12(int16_t* x, int16_t* y);

// Read raw 12-bit X/Y (0-4095, centre ~2048).
bool readXY12Raw(uint16_t* x, uint16_t* y);

// Read offset-corrected 8-bit X/Y. Centre = 0, range ~±128.
bool readXY8(int8_t* x, int8_t* y);

// True when the button is pressed (inverts the hardware's 0=pressed convention).
bool isPressed() const;

// Set LED colour (0x00RRGGBB). Returns false on I2C error.
bool setLed(uint32_t rgb);
```

---

## UnitScroll

**I2C address:** `0x40`
**Protocol:** STM32-based (STOP+delay pattern)

Single rotary encoder with push-button and RGB LED.

```cpp
bool begin(Device* dev, uint8_t addr = 0x40);
bool isPresent() const;

// Read accumulated delta since last call (int16_t, resets on read).
int16_t readDelta();

// Read the absolute encoder value.
int16_t readAbsolute() const;

// True when the button is pressed.
bool isPressed() const;

// Set LED colour (0x00RRGGBB).
// Wire format: [0x00, R, G, B] at register 0x30.
void setLed(uint32_t rgb);

// Reset the internal counter to zero.
void resetEncoder();
```

---

## Other drivers in the library

`Libraries/M5UnitModules/` also contains drivers for `UnitCardKB2`, `UnitDualButton`, `UnitMidi`, and `UnitRfid2`. They are not used by this app, but are compiled by the app's CMake glob. `UnitMidi` uses UART rather than I2C; the others follow the I2C driver pattern.

---

## Adding a new unit

1. Create `Libraries/M5UnitModules/Include/UnitXxx.h` and `Libraries/M5UnitModules/Source/UnitXxx.cpp`.
2. Include `UnitCommon.h` for the shared I2C helpers.
3. Implement `begin(Device*, uint8_t addr)` returning `false` if `unitProbe` fails.
4. Guard every method with `if (!dev_) return;` (or return `false`).
5. The file is picked up automatically by the `GLOB_RECURSE` rules in `Apps/MidiController/main/CMakeLists.txt`.
