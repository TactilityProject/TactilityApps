#include <UnitDualButton.h>
#include <esp_log.h>

static constexpr auto* TAG = "UnitDualButton";

UnitDualButton::~UnitDualButton() {
    end();
}

bool UnitDualButton::begin(Device* controller, gpio_pin_t pinA, gpio_pin_t pinB) {
    if (!controller) return false;

    gpio_flags_t flags = GPIO_FLAG_DIRECTION_INPUT | GPIO_FLAG_PULL_UP;

    descA_ = gpio_descriptor_acquire(controller, pinA, flags, GPIO_OWNER_GPIO);
    if (!descA_) {
        ESP_LOGW(TAG, "Failed to acquire pin %d", (int)pinA);
        return false;
    }

    descB_ = gpio_descriptor_acquire(controller, pinB, flags, GPIO_OWNER_GPIO);
    if (!descB_) {
        ESP_LOGW(TAG, "Failed to acquire pin %d", (int)pinB);
        gpio_descriptor_release(descA_);
        descA_ = nullptr;
        return false;
    }

    ready_ = true;
    ESP_LOGI(TAG, "DualButton ready on pins %d/%d", (int)pinA, (int)pinB);
    return true;
}

void UnitDualButton::end() {
    if (descA_) { gpio_descriptor_release(descA_); descA_ = nullptr; }
    if (descB_) { gpio_descriptor_release(descB_); descB_ = nullptr; }
    ready_ = false;
}

bool UnitDualButton::readPin(GpioDescriptor* desc) {
    bool high = true;
    if (gpio_descriptor_get_level(desc, &high) != ERROR_NONE) {
        // Read failed - treat as not pressed (safe fallback)
        ESP_LOGW(TAG, "gpio_descriptor_get_level failed");
        return false;
    }
    return !high;  // active-low: low = pressed
}

bool UnitDualButton::isButtonAPressed() const {
    if (!descA_) return false;
    return readPin(descA_);
}

bool UnitDualButton::isButtonBPressed() const {
    if (!descB_) return false;
    return readPin(descB_);
}
