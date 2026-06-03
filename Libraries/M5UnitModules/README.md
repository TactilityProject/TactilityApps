# M5UnitModules

Drivers for M5Stack Unit peripheral modules, for use with [Tactility OS](https://github.com/tactility) on ESP32 devices.

## Available drivers

| Header | Unit | Interface | Address |
|--------|------|-----------|---------|
| `Unit8Encoder.h` | 8Encoder - 8 rotary encoders + LEDs | I2C | 0x41 |
| `UnitByteButton.h` | ByteButton - 8 buttons + RGB LEDs | I2C | 0x47 |
| `UnitJoystick2.h` | Joystick2 - XY joystick + button + LED | I2C | 0x63 |
| `UnitScroll.h` | Scroll - single encoder + button + LED | I2C | 0x40 |
| `UnitPaHub.h` | PaHub - TCA9548A 6-channel I2C mux | I2C | 0x70 |
| `UnitLcd.h` | LCD Unit - 135x240 IPS display (ESP32-PICO) | I2C | 0x3E |
| `UnitDualButton.h` | Dual Button - 2 GPIO buttons | GPIO | - |
| `UnitCardKB2.h` | CardKB2 - QWERTY keyboard | I2C | 0x5F |
| `UnitMidi.h` | MIDI Unit / Synth Unit - SAM2695 synth | UART | 31250 bps |
| `UnitRfid2.h` | RFID2 - WS1850S/MFRC522-compat RFID reader/writer | I2C | 0x28 |

## Usage

### Without CMake component

Source files are compiled directly into your app (same pattern as `TactilityCpp` and `SoundEngine`). In your app's `main/CMakeLists.txt`:

```cmake
file(GLOB_RECURSE SOURCE_FILES
    Source/*.c*
    ../../../Libraries/M5UnitModules/Source/*.c*
)

idf_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS
        ../../../Libraries/TactilityCpp/Include
        ../../../Libraries/M5UnitModules/Include
    REQUIRES TactilitySDK
)
```

Then include headers as:

```cpp
#include <UnitByteButton.h>
```

### Standalone (direct I2C)

All I2C units take a `Device*` from `device_find_by_name("i2c1")`. External peripherals are on `i2c1`; `i2c0` is reserved for internal device-tree hardware.

```cpp
#include <tactility/device.h>
#include <UnitByteButton.h>

Device* i2c = device_find_by_name("i2c1");

UnitByteButton bb;
if (bb.begin(i2c)) {
    uint8_t mask = bb.readButtons();  // bitmask, bit i = button i
    bb.setLed(0, 0x00FF00);           // set button 0 LED to green
}
```

### Through PaHub (I2C multiplexer)

When multiple units share the same I2C address, or more than 6 units are needed, connect them via a PaHub. Select the channel before each operation:

```cpp
#include <UnitPaHub.h>
#include <Unit8Encoder.h>

Device* i2c = device_find_by_name("i2c1");

UnitPaHub hub;
Unit8Encoder enc;

if (hub.begin(i2c)) {
    hub.select(0);       // channel 0
    enc.begin(i2c);      // device on channel 0 is now reachable
}

// In your poll loop:
hub.select(0);
int32_t deltas[8];
uint8_t buttons[8];
enc.readAll(deltas, buttons);
```

### GPIO unit (DualButton)

`UnitDualButton` uses the Tactility GPIO controller rather than raw ESP-IDF GPIO:

```cpp
#include <tactility/device.h>
#include <UnitDualButton.h>

Device* gpio = device_find_by_name("gpio0");

UnitDualButton db;
if (db.begin(gpio, GPIO_NUM_36, GPIO_NUM_26)) {
    bool a = db.isButtonAPressed();
    bool b = db.isButtonBPressed();
}
```

### UART unit (MIDI / Synth)

`UnitMidi` uses the `uart_controller` kernel driver. Pass a `Device*` from `device_find_by_name()` to `begin()`:

```cpp
#include <tactility/device.h>
#include <UnitMidi.h>

Device* uart = device_find_by_name("uart1");

UnitMidi midi;
if (midi.begin(uart)) {
    midi.programChange(0, 0);      // channel 0, program 0 (Grand Piano)
    midi.noteOn(0, 60, 100);       // middle C, velocity 100
    midi.noteOff(0, 60);
}
```

### CardKB2 in UART mode

After switching the CardKB2 to UART mode (Fn+Sym+2), use `beginUart()` instead of `begin()`:

```cpp
#include <tactility/device.h>
#include <UnitCardKB2.h>

Device* uart = device_find_by_name("uart1");

UnitCardKB2 kb;
if (kb.beginUart(uart)) {
    // Poll at ~50ms; returns ASCII of last pressed key, 0 if none
    char c = kb.getKey();
}
```

The driver tracks Aa (caps lock / one-shot shift), Sym (symbol mode), and Fn internally. Switch back to I2C mode on the keyboard with Fn+Sym+1.

**UART mode:** Fn+1 (Esc = 0x1B) and Fn+D/X/Z/C (cursor up/down/left/right = 0x1E/0x1F/0x1D/0x1C) are fully supported - the firmware sends the raw KEY_ID in the UART frame and the driver translates it.

**I2C mode:** Fn+1 and Fn+D/X/Z/C produce no output. The firmware's I2C slave queue only holds regular ASCII; the Fn-combo branch routes directly to BLE HID and does not push to the I2C queue.

**BLE HID mode:** Standard USB HID keycodes are sent (ESC=0x29, Up=0x52, Down=0x51, Left=0x50, Right=0x4F). The Tactility `BluetoothHidHost` maps all five to the correct `LV_KEY_*` values.

## I2C protocol note

Most M5Stack Units use an STM32 microcontroller internally. These require a STOP condition between the register-pointer write and the data read (STM32 HAL populates the tx buffer after STOP, before the next START). Using a repeated-START causes bus errors.

`UnitCommon.h` provides `unitReadReg` which implements the required STOP + 2ms delay pattern automatically. All STM32-based drivers use this helper.

The LCD Unit is an exception - it contains an ESP32-PICO and uses a command-based I2C protocol without the STOP+delay requirement.

## UnitRfid2 - RFID2 Unit

The RFID2 Unit uses a WS1850S chip (drop-in MFRC522 replacement) operating at 13.56 MHz. Read range is under 20 mm.

### Supported card types

| Card | SAK | Notes |
|------|-----|-------|
| MIFARE Classic Mini | 0x09 | 5 sectors, 20 blocks |
| MIFARE Classic 1K | 0x08 | 16 sectors, 64 blocks |
| MIFARE Classic 4K | 0x18 | 40 sectors, 256 blocks |
| MIFARE Ultralight | 0x00 | 16 user pages |
| NTAG213 | 0x00 | 45 pages; CC byte 0x12 at page 3 |
| NTAG215 | 0x00 | 135 pages; CC byte 0x3E at page 3 |
| NTAG216 | 0x00 | 231 pages; CC byte 0x6D at page 3 |

### Basic usage

```cpp
#include <UnitRfid2.h>

Device* i2c = device_find_by_name("i2c1");

UnitRfid2 rfid;
if (rfid.begin(i2c)) {
    UnitRfid2::Uid uid;
    if (rfid.readCard(&uid)) {
        auto type = rfid.getCardType(uid);
        // ...
        rfid.haltCard();
    }
}
```

`readCard()` runs the full ISO 14443-3 anticollision/SELECT sequence (including 7-byte and 10-byte cascaded UIDs) and uses WUPA so cards in HALT state are re-detected without needing to be removed.

### MIFARE Classic

```cpp
uint8_t block[16];

// Read with default key (0xFF×6)
rfid.mfReadBlock(4, uid, UnitRfid2::KEY_DEFAULT, block);

// Read trying both Key A and Key B
rfid.mfReadBlockKeyAB(4, uid, UnitRfid2::KEY_DEFAULT, block);

// Read trying all 15 built-in common keys automatically
UnitRfid2::MifareKey keyUsed;
rfid.mfReadBlockAuto(4, uid, block, &keyUsed);

// Write a block
rfid.mfWriteBlock(4, uid, UnitRfid2::KEY_DEFAULT, block);

// Read a full sector (4–16 blocks × 16 bytes; sectors 0-31 = 4 blocks, 32-39 = 16 blocks)
uint8_t sectorBuf[16 * 16];  // max 16 blocks for 4K large sectors
uint8_t blockCount = 0;
rfid.mfReadSector(1, uid, UnitRfid2::KEY_DEFAULT, sectorBuf, &blockCount);
```

`KNOWN_KEYS[15]` contains 15 widely-used default keys. `mfReadBlockAuto` tries each as Key A then Key B, which recovers data from most factory-default or commercially-programmed cards.

Sector trailers (block index % 4 == 3 for sectors 0-31; block index % 16 == 15 for sectors 32-39) contain key and access-condition data; they require the correct key to read and should not be blindly overwritten.

### MIFARE Ultralight / NTAG

```cpp
uint8_t page[4];
rfid.ulReadPage(4, page);    // pages 0-3 are UID/config - skip for user data

uint8_t pages[16];
rfid.ulReadPages(4, 4, pages);  // 4 consecutive pages starting at 4

rfid.ulWritePage(4, page);      // pages 0-3 blocked (pass force=true to override)
```

Use `ultralightPageCount(type)` to get the number of user pages for a given NTAG variant. The first user page is always 4.

### NDEF write (NTAG)

Write a URI NDEF record starting at page 4. The driver in `TestUnitRfid2` handles the full TLV framing, URI abbreviation prefix table (http://, https://, mailto:, tel:, etc.), and page-aligned writes via `ulWritePage`.

### Magic card UID clone (gen1a)

```cpp
// Clone the UID from `sourceUid` onto a gen1a magic MIFARE Classic card
rfid.mfWriteUid(sourceUid.bytes, targetUid);
```

`mfWriteUid` uses the gen1a backdoor sequence (0x40 7-bit frame + 0x43) to open direct write access to block 0, then builds the block with the new UID, computed BCC, and the original SAK/ATQA bytes. Only works on magic gen1a cards - regular MIFARE cards protect block 0 at the chip level.

NTAG21x UID bytes are read-only at the hardware level and cannot be cloned.

## M5UnitTest app

`Apps/M5UnitTest/` is a hardware test app that provides a live interactive test view for each unit. It auto-detects whether a unit is connected directly or via a PaHub, and scales its UI for both portrait and landscape orientations across all supported screen sizes.

| Test view | Unit tested | Notes |
|-----------|-------------|-------|
| `TestUnit8Encoder` | 8Encoder | Live delta/button readout; LED colour cycling on encoder press |
| `TestUnitByteButton` | ByteButton | 8 button indicators; tapping a button lights its LED |
| `TestUnitCardKB2` | CardKB2 | Keystroke echo in both I2C and UART modes |
| `TestUnitDualButton` | Dual Button | Configurable GPIO pin picker; red/blue circle indicators |
| `TestUnitJoystick2` | Joystick2 | XY position bar graphs + button state + LED colour picker |
| `TestUnitLcd` | LCD Unit | Displays a colour gradient and version info on the unit's screen |
| `TestUnitMidi` | MIDI / Synth | Channel + program picker; Note On/Off for middle C |
| `TestUnitPaHub` | PaHub | Per-channel I2C address scanner with periodic re-probe |
| `TestUnitRfid2` | RFID2 | Card detection with UID, type, SAK/ATQA display; Clear to re-scan |
| `TestUnitScroll` | Scroll | Encoder delta counter + button state + LED colour |

The RFID2 test view (`TestUnitRfid2`) shows a pulsing green circle while waiting, then displays card info on tap. Tapping **Clear** returns to idle.
