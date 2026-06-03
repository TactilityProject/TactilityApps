#include <UnitCardKB2.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr auto* TAG = "UnitCardKB2";
static constexpr uint32_t UART_BAUD = 115200;

// ---------------------------------------------------------------------------
// Key ID positions for modifier keys (row*11 + col)
// ---------------------------------------------------------------------------
static constexpr uint8_t ID_AA  = 2*11 + 0;  // 22 - caps lock
static constexpr uint8_t ID_FN  = 3*11 + 0;  // 33 - function key
static constexpr uint8_t ID_SYM = 3*11 + 1;  // 34 - symbol key

// ---------------------------------------------------------------------------
// Three translation tables - normal, caps, sym (44 entries each, 0=no output)
// Source: manual Table 3/4/5
// ---------------------------------------------------------------------------

// Normal/lowercase
static constexpr char KEY_NORMAL[44] = {
    // Row 0: 1 2 3 4 5 6 7 8 9 0  [col10 unused]
    '1','2','3','4','5','6','7','8','9','0', 0,
    // Row 1: q w e r t y u i o p Del
    'q','w','e','r','t','y','u','i','o','p', 0x08,
    // Row 2: Aa(mod) a s d f g h j k l Enter
      0, 'a','s','d','f','g','h','j','k','l', 0x0A,
    // Row 3: Fn(mod) Sym(mod) z x c v b n m Space [col10 unused]
      0,   0, 'z','x','c','v','b','n','m', 0x20, 0,
};

// Caps lock - letters uppercase, digits/space/del/enter unchanged
static constexpr char KEY_CAPS[44] = {
    '1','2','3','4','5','6','7','8','9','0', 0,
    'Q','W','E','R','T','Y','U','I','O','P', 0x08,
      0, 'A','S','D','F','G','H','J','K','L', 0x0A,
      0,   0, 'Z','X','C','V','B','N','M', 0x20, 0,
};

// Symbol mode - from manual Table 5
// Row 0: ! @ # $ % ^ & * ( )  [col10 unused]
// Row 1: ~ ` ? \ / | _ - + =  Del
// Row 2: Aa(mod) { } ^ [ ] " ' ; :  Enter
// Row 3: Fn(mod) Sym(mod) Z X C < > , .  Space  [col10 unused]
static constexpr char KEY_SYM[44] = {
    '!','@','#','$','%','^','&','*','(',')', 0,
    '~','`','?','\\','/','|','_','-','+','=', 0x08,
      0, '{','}','^','[',']','"','\'',';',':', 0x0A,
      0,   0, 'Z','X','C','<','>',',','.', 0x20, 0,
};

// Fn combos: key ID → ASCII (only for IDs that produce something with Fn)
// Fn+D(col3,row2=ID25)=up, Fn+X(col3,row3=ID36)=down,
// Fn+Z(col2,row3=ID35)=left, Fn+C(col4,row3=ID37)=right
// Fn+1(col0,row0=ID0)=Esc
static char fnCombo(uint8_t id) {
    switch (id) {
        case 0:  return 0x1B;  // Fn+1 = Esc
        case 25: return 0x1E;  // Fn+D = up
        case 36: return 0x1F;  // Fn+X = down
        case 35: return 0x1D;  // Fn+Z = left
        case 37: return 0x1C;  // Fn+C = right
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------

bool UnitCardKB2::begin(Device* dev, uint8_t addr) {
    end();
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "CardKB2 not found at 0x%02X", addr);
        return false;
    }
    i2cDev_ = dev;
    addr_   = addr;
    mode_   = Mode::I2C;
    ESP_LOGI(TAG, "CardKB2 ready (I2C) at 0x%02X", addr_);
    return true;
}

char UnitCardKB2::readFromI2C() {
    if (!i2cDev_) return 0;
    uint8_t val = 0;
    if (i2c_controller_read(i2cDev_, addr_, &val, 1, pdMS_TO_TICKS(UNIT_I2C_TIMEOUT_MS)) != ERROR_NONE)
        ESP_LOGD(TAG, "CardKB2 I2C read failed at 0x%02X", addr_);
    return (char)val;
}

// ---------------------------------------------------------------------------
// UART
// ---------------------------------------------------------------------------

bool UnitCardKB2::beginUart(Device* dev) {
    end();
    if (!dev) return false;
    UartConfig cfg = {
        UART_BAUD,
        UART_CONTROLLER_DATA_8_BITS,
        UART_CONTROLLER_PARITY_DISABLE,
        UART_CONTROLLER_STOP_BITS_1,
    };
    if (uart_controller_set_config(dev, &cfg) != ERROR_NONE) {
        ESP_LOGW(TAG, "CardKB2 UART set_config failed");
        return false;
    }
    if (uart_controller_open(dev) != ERROR_NONE) {
        ESP_LOGW(TAG, "CardKB2 UART open failed");
        return false;
    }
    uartDev_    = dev;
    frameState_ = FrameState::WaitAA;
    capsLock_   = false;
    symMode_    = false;
    fnHeld_     = false;
    oneShiftPending_ = false;
    mode_       = Mode::Uart;
    ESP_LOGI(TAG, "CardKB2 ready (UART) at %lu bps", (unsigned long)UART_BAUD);
    return true;
}

char UnitCardKB2::pollUart() {
    if (!uartDev_) return 0;
    char result = 0;
    uint8_t b;
    // Drain all available bytes this tick; non-blocking (timeout=0)
    while (uart_controller_read_byte(uartDev_, &b, 0) == ERROR_NONE) {
        switch (frameState_) {
            case FrameState::WaitAA:
                if (b == 0xAA) frameState_ = FrameState::WaitLen;
                break;
            case FrameState::WaitLen:
                frameState_ = (b == 0x03) ? FrameState::WaitId : FrameState::WaitAA;
                break;
            case FrameState::WaitId:
                frameId_    = b;
                frameState_ = FrameState::WaitState;
                break;
            case FrameState::WaitState:
                frameKs_    = b;
                frameState_ = FrameState::WaitCsum;
                break;
            case FrameState::WaitCsum: {
                frameState_ = FrameState::WaitAA;
                uint8_t expected = (0x03 + frameId_ + frameKs_) & 0xFF;
                if (b != expected) {
                    ESP_LOGD(TAG, "UART frame csum err: got 0x%02X exp 0x%02X", b, expected);
                    break;
                }

                bool pressed  = (frameKs_ == 0x01);
                bool released = (frameKs_ == 0x02);

                // --- Modifier tracking ---
                if (frameId_ == ID_FN) {
                    fnHeld_ = pressed;
                    break;
                }
                if (frameId_ == ID_SYM && pressed) {
                    symMode_ = !symMode_;
                    if (symMode_) capsLock_ = false;  // Aa ineffective in sym mode
                    break;
                }
                if (frameId_ == ID_AA && pressed && !symMode_) {
                    static constexpr uint32_t AA_DOUBLE_CLICK_MS = 400;
                    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                    if (!capsLock_) {
                        if (oneShiftPending_ && (now - lastAATimestamp_) <= AA_DOUBLE_CLICK_MS) {
                            // Quick double-tap - engage caps lock, clear one-shot
                            capsLock_        = true;
                            oneShiftPending_ = false;
                            lastAATimestamp_ = 0;
                        } else {
                            // First tap or too slow - start/restart one-shot
                            oneShiftPending_ = true;
                            lastAATimestamp_ = now;
                        }
                    } else {
                        // Already locked - release caps lock
                        capsLock_        = false;
                        oneShiftPending_ = false;
                        lastAATimestamp_ = 0;
                    }
                    break;
                }

                // --- Key press → ASCII ---
                if (!pressed) break;

                char ascii = 0;
                if (fnHeld_) {
                    ascii = fnCombo(frameId_);
                } else if (symMode_) {
                    if (frameId_ < 44) ascii = KEY_SYM[frameId_];
                } else if (capsLock_ || oneShiftPending_) {
                    if (frameId_ < 44) ascii = KEY_CAPS[frameId_];
                    if (oneShiftPending_) {
                        // Consume the one-shot only when a letter was actually shifted
                        if (ascii >= 'A' && ascii <= 'Z') oneShiftPending_ = false;
                    }
                } else {
                    if (frameId_ < 44) ascii = KEY_NORMAL[frameId_];
                }

                // First key press wins; stop draining once we have a result.
                if (ascii != 0) { result = ascii; return result; }
                break;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// end
// ---------------------------------------------------------------------------

void UnitCardKB2::end() {
    if (mode_ == Mode::Uart && uartDev_) {
        uart_controller_close(uartDev_);
        uartDev_ = nullptr;
    }
    i2cDev_           = nullptr;
    cachedKey_        = 0;
    frameState_       = FrameState::WaitAA;
    capsLock_         = false;
    symMode_          = false;
    fnHeld_           = false;
    oneShiftPending_  = false;
    lastAATimestamp_  = 0;
    mode_             = Mode::I2C;
}

// ---------------------------------------------------------------------------
// Public getKey / hasKey
// ---------------------------------------------------------------------------

char UnitCardKB2::getKey() {
    if (cachedKey_ != 0) {
        char k = cachedKey_;
        cachedKey_ = 0;
        return k;
    }
    if (mode_ == Mode::Uart) return pollUart();
    return readFromI2C();
}

bool UnitCardKB2::hasKey() {
    if (mode_ == Mode::Uart) {
        if (cachedKey_ == 0) cachedKey_ = pollUart();
        return cachedKey_ != 0;
    }
    if (cachedKey_ != 0) return true;
    cachedKey_ = readFromI2C();
    return cachedKey_ != 0;
}
