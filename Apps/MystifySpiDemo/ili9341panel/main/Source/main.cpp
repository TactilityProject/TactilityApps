// SPDX-License-Identifier: Apache-2.0
/**
 * Headless terminal wrapper around the ILI9341 panel module's settings API.
 * The shell exposes this binary as the "ili9341panel" command.
 */
#include <ili9341panel.h>

#include "panel_display.h"

#include <app/install.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>
#include <tactility/module.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

constexpr const char* PACKAGE_ID = "tactility.mystifyspidemo";
constexpr const char* MODULE_NAME = "ili9341-panel";

struct PanelApi {
    decltype(&ili9341panel_settings_defaults) defaults = nullptr;
    decltype(&ili9341panel_settings_load) load = nullptr;
    decltype(&ili9341panel_settings_save) save = nullptr;
    decltype(&ili9341panel_apply) apply = nullptr;
    decltype(&ili9341panel_is_active) isActive = nullptr;
    decltype(&ili9341panel_get_device) getDevice = nullptr;
    Module* module = nullptr;
};

bool acquireApi(PanelApi& api) {
    Module* module = nullptr;

    auto resolve = [&module](const char* name, auto& function) {
        uintptr_t address = 0;
        Module* owner = nullptr;
        if (!module_resolve_symbol_global_get(name, &address, &owner)) {
            return false;
        }
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        if (module == nullptr) {
            module = owner;
        } else {
            module_put(owner);
        }
        return true;
    };

    const bool found = resolve("ili9341panel_settings_defaults", api.defaults) &&
        resolve("ili9341panel_settings_load", api.load) &&
        resolve("ili9341panel_settings_save", api.save) &&
        resolve("ili9341panel_apply", api.apply) &&
        resolve("ili9341panel_is_active", api.isActive) &&
        resolve("ili9341panel_get_device", api.getDevice);

    // Symbol lookup also finds stopped modules, so check the state as well.
    if (!found || !module_is_started(module)) {
        if (module != nullptr) {
            module_put(module);
        }
        api = PanelApi {};
        return false;
    }

    api.module = module;
    return true;
}

void releaseApi(PanelApi& api) {
    if (api.module != nullptr) {
        module_put(api.module);
    }
    api = PanelApi {};
}

int orientationIndex(const Ili9341PanelConfig& config) {
    if (config.swap_xy && !config.mirror_x && !config.mirror_y) return 0; // landscape
    if (!config.swap_xy && !config.mirror_x && config.mirror_y) return 1; // portrait
    if (config.swap_xy && config.mirror_x && config.mirror_y) return 2; // landscape 180
    if (!config.swap_xy && config.mirror_x && !config.mirror_y) return 3; // portrait 180
    return -1;
}

const char* orientationName(int index) {
    switch (index) {
        case 0:
            return "landscape";
        case 1:
            return "portrait";
        case 2:
            return "landscape180";
        case 3:
            return "portrait180";
        default:
            return "custom";
    }
}

bool applyOrientation(Ili9341PanelConfig& config, const char* name) {
    if (strcmp(name, "landscape") == 0) {
        config.swap_xy = true;
        config.mirror_x = false;
        config.mirror_y = false;
        config.width = 320;
        config.height = 240;
    } else if (strcmp(name, "portrait") == 0) {
        config.swap_xy = false;
        config.mirror_x = false;
        config.mirror_y = true;
        config.width = 240;
        config.height = 320;
    } else if (strcmp(name, "landscape180") == 0) {
        config.swap_xy = true;
        config.mirror_x = true;
        config.mirror_y = true;
        config.width = 320;
        config.height = 240;
    } else if (strcmp(name, "portrait180") == 0) {
        config.swap_xy = false;
        config.mirror_x = true;
        config.mirror_y = false;
        config.width = 240;
        config.height = 320;
    } else {
        return false;
    }
    return true;
}

bool parseOnOff(const char* text, bool* out) {
    if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool parseNumber(const char* text, long min, long max, long* out) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

const char* yesNo(bool value) {
    return value ? "on" : "off";
}

void printUsage() {
    std::printf(
        "Usage: ili9341panel <command> [arguments]\n"
        "\n"
        "  status                       Module and panel state\n"
        "  get                          Print all settings\n"
        "  on | off                     Start/stop the panel module\n"
        "  apply                        Re-create the panel from the saved settings\n"
        "  set <key> <value>            Change and save a setting\n"
        "\n"
        "keys:\n"
        "  orientation landscape|portrait|landscape180|portrait180\n"
        "  swapxy|mirrorx|mirrory|invert|bgr on|off\n"
        "  width|height <1-1024>        clock <10|20|40|80> (MHz)\n"
        "  host <2|3>                   maxkb <1-64>\n"
        "  sck|mosi|miso|dc|cs <0-54>   reset <0-54|none>\n"
    );
}

int commandStatus(PanelApi& api) {
    struct Device* device = nullptr;
    const bool active = api.getDevice(&device) == ERROR_NONE && device != nullptr;

    Ili9341PanelConfig config {};
    if (api.load(&config) != ERROR_NONE) {
        std::printf("settings: could not be loaded\n");
        return 1;
    }

    std::printf("module:   started\n");
    if (active) {
        std::printf(
            "panel:    active, %ux%u, color format %d\n",
            static_cast<unsigned>(display_get_resolution_x(device)),
            static_cast<unsigned>(display_get_resolution_y(device)),
            static_cast<int>(display_get_color_format(device))
        );
        device_put(device);
    } else {
        std::printf("panel:    inactive\n");
    }

    std::printf("spi:      SPI%d at %u MHz\n", static_cast<int>(config.spi_host) + 1,
        static_cast<unsigned>(config.pixel_clock_hz / 1000000));
    std::printf(
        "pins:     sck=%d mosi=%d miso=%d dc=%d reset=%d cs=%d\n",
        static_cast<int>(config.pin_sck),
        static_cast<int>(config.pin_mosi),
        static_cast<int>(config.pin_miso),
        static_cast<int>(config.pin_dc),
        static_cast<int>(config.pin_reset),
        static_cast<int>(config.pin_cs)
    );
    std::printf(
        "display:  %ux%u, orientation %s, invert %s, bgr %s, max %u KB\n",
        static_cast<unsigned>(config.width),
        static_cast<unsigned>(config.height),
        orientationName(orientationIndex(config)),
        yesNo(config.invert_color),
        yesNo(config.bgr_order),
        static_cast<unsigned>((config.max_transfer_bytes + 512) / 1024)
    );
    return 0;
}

int commandGet(PanelApi& api) {
    Ili9341PanelConfig config {};
    const error_t result = api.load(&config);
    if (result != ERROR_NONE) {
        std::printf("error: %s\n", error_to_string(result));
        return 1;
    }

    std::printf("enabled=%s\n", yesNo(config.enabled));
    std::printf("spi_host=%d\n", static_cast<int>(config.spi_host));
    std::printf("sck=%d\n", static_cast<int>(config.pin_sck));
    std::printf("mosi=%d\n", static_cast<int>(config.pin_mosi));
    std::printf("miso=%d\n", static_cast<int>(config.pin_miso));
    std::printf("dc=%d\n", static_cast<int>(config.pin_dc));
    std::printf("reset=%d\n", static_cast<int>(config.pin_reset));
    std::printf("cs=%d\n", static_cast<int>(config.pin_cs));
    std::printf("width=%u\n", static_cast<unsigned>(config.width));
    std::printf("height=%u\n", static_cast<unsigned>(config.height));
    std::printf("swap_xy=%s\n", yesNo(config.swap_xy));
    std::printf("mirror_x=%s\n", yesNo(config.mirror_x));
    std::printf("mirror_y=%s\n", yesNo(config.mirror_y));
    std::printf("invert=%s\n", yesNo(config.invert_color));
    std::printf("bgr=%s\n", yesNo(config.bgr_order));
    std::printf("clock_mhz=%u\n", static_cast<unsigned>(config.pixel_clock_hz / 1000000));
    std::printf("max_transfer=%u\n", static_cast<unsigned>(config.max_transfer_bytes));
    return 0;
}

int commandOn() {
    error_t result = app_module_set_started(PACKAGE_ID, MODULE_NAME, true);
    if (result != ERROR_NONE) {
        std::printf("error: could not start the module: %s\n", error_to_string(result));
        return 1;
    }

    PanelApi api {};
    if (!acquireApi(api)) {
        std::printf("error: the module started but its settings API is unavailable\n");
        return 1;
    }

    // The switch owns "enabled" in the apps; normalize a config saved as disabled by an older build.
    Ili9341PanelConfig config {};
    if (api.load(&config) == ERROR_NONE && !config.enabled) {
        config.enabled = true;
        if (api.save(&config) == ERROR_NONE) {
            api.apply();
        }
    }

    struct Device* device = nullptr;
    if (api.getDevice(&device) == ERROR_NONE && device != nullptr) {
        if (ili9341panelui::findLvglDisplay(device) == nullptr) {
            ili9341panelui::attachToLvgl(device);
        }
        device_put(device);
        std::printf("panel started\n");
    } else {
        std::printf("module started; panel inactive (check the saved settings)\n");
    }

    releaseApi(api);
    return 0;
}

int commandOff() {
    PanelApi api {};
    if (acquireApi(api)) {
        struct Device* device = nullptr;
        if (api.getDevice(&device) == ERROR_NONE && device != nullptr) {
            ili9341panelui::detachFromLvgl(device, true);
            device_put(device);
        }
        releaseApi(api);
    }

    const error_t result = app_module_set_started(PACKAGE_ID, MODULE_NAME, false);
    if (result != ERROR_NONE) {
        std::printf("error: could not stop the module: %s\n", error_to_string(result));
        return 1;
    }
    std::printf("panel stopped\n");
    return 0;
}

int commandApply(PanelApi& api) {
    // Apply works on the saved settings, so normalize "enabled" first (the CLI has no off state of
    // its own; the module switch does).
    Ili9341PanelConfig config {};
    if (api.load(&config) == ERROR_NONE && !config.enabled) {
        config.enabled = true;
        api.save(&config);
    }

    struct Device* device = nullptr;
    if (api.getDevice(&device) == ERROR_NONE && device != nullptr) {
        ili9341panelui::detachFromLvgl(device);
        device_put(device);
    }

    error_t result = api.apply();
    if (result == ERROR_NONE && api.isActive()) {
        if (api.getDevice(&device) == ERROR_NONE && device != nullptr) {
            result = ili9341panelui::attachToLvgl(device);
            device_put(device);
        }
    }

    if (result != ERROR_NONE) {
        std::printf("error: apply failed: %s\n", error_to_string(result));
        return 1;
    }
    std::printf("settings applied\n");
    return 0;
}

int commandSet(PanelApi& api, int argc, char* argv[]) {
    if (argc < 4) {
        printUsage();
        return 1;
    }

    Ili9341PanelConfig config {};
    error_t result = api.load(&config);
    if (result != ERROR_NONE) {
        std::printf("error: %s\n", error_to_string(result));
        return 1;
    }

    const char* key = argv[2];
    const char* value = argv[3];
    long number = 0;
    bool flag = false;

    if (strcmp(key, "orientation") == 0) {
        if (!applyOrientation(config, value)) {
            std::printf("error: orientation must be landscape, portrait, landscape180 or portrait180\n");
            return 1;
        }
    } else if (strcmp(key, "swapxy") == 0 || strcmp(key, "mirrorx") == 0 ||
               strcmp(key, "mirrory") == 0 || strcmp(key, "invert") == 0 || strcmp(key, "bgr") == 0) {
        if (!parseOnOff(value, &flag)) {
            std::printf("error: %s must be on or off\n", key);
            return 1;
        }
        if (strcmp(key, "swapxy") == 0) config.swap_xy = flag;
        else if (strcmp(key, "mirrorx") == 0) config.mirror_x = flag;
        else if (strcmp(key, "mirrory") == 0) config.mirror_y = flag;
        else if (strcmp(key, "invert") == 0) config.invert_color = flag;
        else config.bgr_order = flag;
    } else if (strcmp(key, "width") == 0 || strcmp(key, "height") == 0) {
        if (!parseNumber(value, 1, 1024, &number)) {
            std::printf("error: %s must be 1-1024\n", key);
            return 1;
        }
        if (strcmp(key, "width") == 0) config.width = static_cast<uint16_t>(number);
        else config.height = static_cast<uint16_t>(number);
    } else if (strcmp(key, "clock") == 0) {
        if (!parseNumber(value, 10, 80, &number) ||
            (number != 10 && number != 20 && number != 40 && number != 80)) {
            std::printf("error: clock must be 10, 20, 40 or 80\n");
            return 1;
        }
        config.pixel_clock_hz = static_cast<uint32_t>(number) * 1000000;
    } else if (strcmp(key, "host") == 0) {
        if (!parseNumber(value, 1, 2, &number)) {
            std::printf("error: host must be 1 (SPI2) or 2 (SPI3)\n");
            return 1;
        }
        config.spi_host = static_cast<int32_t>(number);
    } else if (strcmp(key, "maxkb") == 0) {
        if (!parseNumber(value, 1, 64, &number)) {
            std::printf("error: maxkb must be 1-64\n");
            return 1;
        }
        const long bytes = number * 1024;
        config.max_transfer_bytes = static_cast<uint32_t>(bytes > 65535 ? 65535 : bytes);
    } else if (strcmp(key, "sck") == 0 || strcmp(key, "mosi") == 0 || strcmp(key, "miso") == 0 ||
               strcmp(key, "dc") == 0 || strcmp(key, "cs") == 0 || strcmp(key, "reset") == 0) {
        if (strcmp(key, "reset") == 0 && strcmp(value, "none") == 0) {
            config.pin_reset = -1;
        } else {
            if (!parseNumber(value, 0, 54, &number)) {
                std::printf("error: %s must be 0-54%s\n", key, strcmp(key, "reset") == 0 ? " or none" : "");
                return 1;
            }
            if (strcmp(key, "sck") == 0) config.pin_sck = static_cast<int32_t>(number);
            else if (strcmp(key, "mosi") == 0) config.pin_mosi = static_cast<int32_t>(number);
            else if (strcmp(key, "miso") == 0) config.pin_miso = static_cast<int32_t>(number);
            else if (strcmp(key, "dc") == 0) config.pin_dc = static_cast<int32_t>(number);
            else if (strcmp(key, "cs") == 0) config.pin_cs = static_cast<int32_t>(number);
            else config.pin_reset = static_cast<int32_t>(number);
        }
    } else {
        std::printf("error: unknown key '%s'\n", key);
        printUsage();
        return 1;
    }

    config.enabled = true;
    result = api.save(&config);
    if (result != ERROR_NONE) {
        std::printf("error: could not save: %s\n", error_to_string(result));
        return 1;
    }

    std::printf("%s saved; run 'ili9341panel apply' to use it\n", key);
    return 0;
}

} // namespace

extern "C" int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const char* command = argv[1];

    if (strcmp(command, "on") == 0) {
        return commandOn();
    }
    if (strcmp(command, "off") == 0) {
        return commandOff();
    }

    PanelApi api {};
    if (!acquireApi(api)) {
        std::printf("error: the panel module is not running (use 'ili9341panel on')\n");
        return 1;
    }

    int result = 1;
    if (strcmp(command, "status") == 0) {
        result = commandStatus(api);
    } else if (strcmp(command, "get") == 0) {
        result = commandGet(api);
    } else if (strcmp(command, "apply") == 0) {
        result = commandApply(api);
    } else if (strcmp(command, "set") == 0) {
        result = commandSet(api, argc, argv);
    } else {
        printUsage();
    }

    releaseApi(api);
    return result;
}
