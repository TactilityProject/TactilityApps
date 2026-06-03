#include <UnitMidi.h>
#include <tactility/drivers/uart_controller.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr auto* TAG            = "UnitMidi";
static constexpr uint32_t BAUD        = 31250;
static constexpr uint32_t SEND_TIMEOUT_MS = 20;

bool UnitMidi::begin(Device* dev) {
    if (!dev) return false;
    UartConfig cfg = {
        BAUD,
        UART_CONTROLLER_DATA_8_BITS,
        UART_CONTROLLER_PARITY_DISABLE,
        UART_CONTROLLER_STOP_BITS_1,
    };
    if (uart_controller_set_config(dev, &cfg) != ERROR_NONE) {
        ESP_LOGW(TAG, "uart_controller_set_config failed");
        return false;
    }
    if (uart_controller_open(dev) != ERROR_NONE) {
        ESP_LOGW(TAG, "uart_controller_open failed");
        return false;
    }
    dev_   = dev;
    ready_ = true;
    // Give SAM2695 time to power up, then reset to known state
    vTaskDelay(pdMS_TO_TICKS(100));
    reset();
    ESP_LOGI(TAG, "UnitMidi ready at %" PRIu32 " bps", BAUD);
    return true;
}

void UnitMidi::end() {
    if (dev_ && ready_) {
        allNotesOff(0xFF);
        uart_controller_close(dev_);
    }
    ready_ = false;
    dev_   = nullptr;
}

void UnitMidi::send(const uint8_t* data, size_t len) {
    if (!ready_ || !dev_) return;
    uart_controller_write_bytes(dev_, data, len, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
}

void UnitMidi::reset() {
    uint8_t msg[] = { 0xFF };
    send(msg, 1);
}

void UnitMidi::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    uint8_t msg[] = { (uint8_t)(0x90 | (channel & 0x0F)), (uint8_t)(note & 0x7F), (uint8_t)(velocity & 0x7F) };
    send(msg, 3);
}

void UnitMidi::noteOff(uint8_t channel, uint8_t note) {
    uint8_t msg[] = { (uint8_t)(0x80 | (channel & 0x0F)), (uint8_t)(note & 0x7F), 0x00 };
    send(msg, 3);
}

void UnitMidi::programChange(uint8_t channel, uint8_t program) {
    uint8_t msg[] = { (uint8_t)(0xC0 | (channel & 0x0F)), (uint8_t)(program & 0x7F) };
    send(msg, 2);
}

void UnitMidi::controlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    uint8_t msg[] = { (uint8_t)(0xB0 | (channel & 0x0F)), (uint8_t)(controller & 0x7F), (uint8_t)(value & 0x7F) };
    send(msg, 3);
}

void UnitMidi::pitchBend(uint8_t channel, int16_t value) {
    // value range -8192..+8191 → offset by 8192, split into 7-bit LSB/MSB
    uint16_t v = (uint16_t)(value + 8192);
    uint8_t msg[] = {
        (uint8_t)(0xE0 | (channel & 0x0F)),
        (uint8_t)(v & 0x7F),
        (uint8_t)((v >> 7) & 0x7F),
    };
    send(msg, 3);
}

void UnitMidi::allNotesOff(uint8_t channel) {
    if (channel == 0xFF) {
        for (uint8_t ch = 0; ch < 16; ch++)
            controlChange(ch, 123, 0);
    } else {
        controlChange(channel, 123, 0);
    }
}
