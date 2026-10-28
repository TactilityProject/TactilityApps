# Mystify SPI Demo

An app package that adds a 2.8" ILI9341 SPI panel to Tactility and renders a Mystify scene on it
(and mirrors it on the primary display).

Display used in this example: 2.8TFT- ILI9341 IC  
https://www.aliexpress.com/item/1005006323678902.html  
https://www.lcdwiki.com/2.8inch_SPI_Module_ILI9341_SKU%3AMSP2807

The package contains five binaries:

| Binary | Role |
|--------|------|
| `ili9341-panel-module` | Kernel module: owns the driver, the SPI bus and the display device, and exports a settings API |
| `mystifyspidemo` | The demo app: looks the panel up through the module's API and draws to it with Gfx |
| `ili9341-panel-settings` | The settings app ("ILI9341 Panel"): edits every panel setting and applies it live |
| `paneldashboard` | Panel Dashboard: draws a live clock and system stats on the panel's secondary `lv_display_t` |
| `ili9341panel` | Headless CLI, exposed by the shell as the `ili9341panel` command |

Because the module is loaded from the package (at install and at boot), the panel exists as a
normal `DisplayApi` device even when no app is running. LVGL binds it as a secondary display when
it starts; opening the demo stops LVGL, takes over the panel through `gfx::SinkTarget` +
`gfx::DisplaySink`, and restarts LVGL on exit.

## Wiring

| Panel | Tab5 M-BUS | Note |
|-------|------------|------|
| VCC | Ext5V | The module has its own 3.3 V regulator or bridge J1 and use 3.3V instead of Ext5V |
| GND | GND | |
| CS | GPIO51 | |
| RESET | GPIO4 | |
| DC | GPIO47 | |
| MOSI | GPIO18 | |
| SCK | GPIO5 | |
| MISO | GPIO19 | Only needed for register reads |
| LED | 3V3 | Do **not** drive it from a GPIO: that stops the board from booting |

## Module

`ili9341-panel-module` starts at boot:

1. Registers the `ili9341-spi` driver (`driver_construct_add`).
2. Loads `ili9341panel.properties` from the package's user data directory (defaults on first run).
3. Calls `spi_bus_initialize()` for the configured host/pins and creates the display device.
4. LVGL picks up the device when it starts and binds it as a second `lv_display_t`.

On stop it removes the device and frees the bus. If the panel fails to start, the module still
stays loaded so the configuration can be fixed and re-applied.

The driver itself (`Ili9341Spi.h`/`Ili9341Spi.cpp`, including the DisplayApi and MADCTL handling)
lives in the module too, next to `Panel.cpp`/`Settings.cpp`. It used to be the standalone
`Libraries/Ili9341Spi`; since the module is the only consumer and app-packaged drivers are proven,
keeping it there makes the package self-contained.

### Settings API

Exported by the module and resolved by apps at runtime (`module_resolve_symbol_global_get`):

```c
error_t ili9341panel_settings_defaults(struct Ili9341PanelConfig* out_config);
error_t ili9341panel_settings_load(struct Ili9341PanelConfig* out_config);
error_t ili9341panel_settings_save(const struct Ili9341PanelConfig* config);
error_t ili9341panel_apply(void);           // re-create the device from saved settings
bool    ili9341panel_is_active(void);
error_t ili9341panel_get_device(struct Device** out_device); // caller must device_put()
```

Defaults match the wiring above: SPI2, SCK=5, MOSI=18, MISO=19, DC=47, RESET=4, CS=51, landscape
320x240, 40 MHz, BGR, no inversion. `ili9341panel_apply()` destroys and re-creates the device, so
the panel's `lv_display_t` must be removed first (see "Settings app" below).

## Settings app

`ili9341-panel-settings` is a window-manager app that edits the module's saved configuration:

- **Panel**: enabled switch (starts/stops the module, so it matches Kernel settings), four
  orientation presets, mirror X/Y, invert colours, BGR order. The portraits use mirror Y for this
  panel, and the presets also set the matching 240×320/320×240 size.
- **Size and bus**: 320x240/240x320 presets with an active highlight plus free width/height,
  SPI2/SPI3, 10/20/40/80 MHz, and the maximum transfer size.
- **Pins**: SCK, MOSI, MISO, DC, RESET (None = unused) and CS.
- **Save** stores the settings for the next boot; **Apply** saves them, restarts the device and
  rebinds LVGL; **Revert** reloads the saved values into the form.

Numeric values (width, height, max transfer, pins) are mesh-settings style rows: the row shows the
current value and its change button opens the Input Dialog app for a proper, full-size text field
(leave it empty for RESET = None). Nothing uses a dropdown or an inline textarea.

Apply runs on the app task, not in the LVGL event callback. It finds the panel's `lv_display_t` via
`LvglDeviceContext.device`, removes it under the LVGL lock, calls `ili9341panel_apply()`, and then
re-adds it with the same `LvglDisplayConfig` that `lvgl_devices_attach()` uses. A configuration that
fails to start is rolled back to the previous one, so the panel doesn't stay dead after a bad pin
value. While the switch is off, Save/Apply/Revert only report that the panel must be enabled first.
This requires the
firmware exports `lv_display_get_next` and `lv_display_get_driver_data` (see "Build and install").

The enabled switch controls the panel module's started state through `app_module_set_started()`, so
Kernel settings shows the same Started/Stopped state. While the module is stopped the form shows the
built-in defaults; turning it on loads the saved settings. If the module is started from Kernel
settings while this app is in the background, the form re-syncs when the window is rebuilt.

## Secondary display

The panel is a real secondary `lv_display_t` while LVGL runs, not a copy of the primary screen.
Apps can put widgets on its screen with `lv_display_get_screen_active()` (exported by the lvgl
module) and find the display through `LvglDeviceContext`, which is what `Panel Dashboard` does: it
shows a live clock, date, uptime, internal RAM and PSRAM on the panel while its window (status and
help) stays on the primary display. Closing the app deletes its widgets and the panel goes blank.

This model was chosen over mirroring the UI: the primary renders a 640x360 PPA-scaled frame, and
copying and scaling it to 320x240 on every refresh would cost more than the SPI panel can keep up
with. Apps that want raw pixels instead of LVGL widgets can still take the panel over with
`gfx::DisplaySink`, as the Mystify demo does.

## Terminal

`ili9341panel` is a headless app, so the shell exposes it as a command. It talks to the same
module API as the settings app:

```sh
ili9341panel status                 # module/panel state, SPI and display settings
ili9341panel get                    # all settings as key=value
ili9341panel on | off               # start/stop the module (same as the settings switch)
ili9341panel set orientation portrait
ili9341panel set clock 80
ili9341panel set sck 5              # keys: sck mosi miso dc cs reset; reset also accepts none
ili9341panel apply                  # re-create the panel from the saved settings
```

`set` saves immediately and reports that `apply` is needed; `apply` detaches the panel's LVGL
display, calls `ili9341panel_apply()` and re-binds it.

## Demo app

- Resolves `ili9341panel_get_device` to hold a module reference and get the panel device.
- Stops LVGL (done by the entry point), then creates `ExternalFirstDisplaySink` +
  `gfx::SinkTarget` at the device's own resolution. The sink subclass prefers PSRAM because
  `gfx::DisplaySink`'s default policy requires internal DMA memory; the driver sets
  `SPI_TRANS_DMA_USE_PSRAM` so the SPI engine reads directly from PSRAM.
- Mirrors the same normalised simulation on the primary display via `gfx::Target`, using
  `set_transform()` on portrait-native panels (Tab5) with a 640x360 logical frame that the PPA
  scales/rotates up.
- Clears both panels and restarts LVGL on exit.

## Build and install

```sh
tt app build --path Apps/MystifySpiDemo --architecture esp32p4
tt app bir   --path Apps/MystifySpiDemo --architecture esp32p4 --host <device>
```

The package installs all five binaries; the module starts when the package is installed and again
on every boot. After a kernel-module update, reboot once if the module was already running.

The settings app's live apply and the dashboard rely on exports in `Modules/lvgl-module/source/symbols.c`
(`lv_display_get_next`, `lv_display_get_driver_data`, `lv_display_get_screen_active`). A firmware
built before those were added makes those apps fail to load; rebuild and flash it, then re-run
`Scripts/check-app-symbols.py` against all five ELFs.

## Troubleshooting

- **`The ILI9341 panel module is not available`** — the package's module isn't loaded. Reboot, or
  check the module state in Kernel settings.
- **`The ILI9341 panel is not active`** — the module is loaded but the device failed to start. The
  module logs the driver's reason; check the wiring, then save/apply settings.
- **White screen with correct wiring** — toggle `invert_color` (`true` gives a negative image on
  this module), or lower `pixel_clock_hz` to 10-20 MHz.
- **App fails to load with a missing `spi_*` or `lv_display_get_next`/`lv_display_get_driver_data`
  symbol** — the firmware predates those exports in `Platforms/platform-esp32/source/module.cpp`
  and `Modules/lvgl-module/source/symbols.c`; rebuild and flash it, then re-run
  `Scripts/check-app-symbols.py`.
- The unit's controller doesn't implement RDDID (it returns zeros) but does answer register reads;
  the driver logs `Panel responds; RDDID is not implemented by this controller` and continues.
