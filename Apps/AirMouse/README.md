# Air Mouse

Turns the device into a Bluetooth LE mouse: rotate the device to move the pointer and tap the two
on-screen pads for the left and right buttons. The device pairs with a PC, phone or tablet like any
other Bluetooth mouse.

The app reads the gyroscope (not the accelerometer tilt), so the pointer follows rotation: turn the
device left and the pointer goes left, regardless of how you hold it. Gravity is used to work out how
the device is held, so it does not matter whether it is held upright or on its side.

## Requirements

- **Gyroscope.** Either an IMU in the devicetree (Tactility's drivers apply their own mount matrix),
  or a bare MPU6886 module on an exposed I2C bus, which the app finds at address 0x68. Without one
  the screen says "No IMU found".
- **Bluetooth.** The Bluetooth device has to be enabled in the devicetree. The radio itself may be
  off: switching the pointer on turns it on, and the app turns it back off when it exits if it was
  off before. If the radio is already on when the app opens, the pointer switches on by itself.
- **The window has to be visible.** The pointer only moves while the Air Mouse window is the one on
  screen. Put it on a device you hold in your hand, or on a device whose window stays on top.

## Pairing

The app advertises as `Tactility Air Mouse` with its own Bluetooth address and bond, so a host pairs
it as a separate device from the other Tactility Bluetooth apps (Media Keys, MIDI). Pair it once per
host. Switching apps does not need a new pairing, the host reconnects each device on sight.

## Main screen

The toolbar has the title, a switch and the settings button.

- **Switch:** turns the Bluetooth mouse on and off. On means: Bluetooth radio on, HID mouse started,
  advertising, and the pointer following the gyroscope. Switching it off stops the HID mouse and puts
  the radio back the way it was. Opening the app while the radio is already on switches the pointer
  on.
- **Left / Right pads:** pressing a pad holds that mouse button down, releasing it lets go. The pads
  are large so they can be used without looking. They do nothing when no host is connected.
- When the Bluetooth radio is turned off elsewhere (Tactility's own settings, or the host going
  away), the switch turns itself off.

### Clutch

Hold **both pads at once** to freeze the pointer, then move the device to a more comfortable position
and let go. The pointer picks up where it was - no flinging the cursor across the screen while you
reposition your arm.

That needs a touch screen, so devices without one get the **auto-clutch** instead (see below).

### Auto-clutch

While the auto-clutch is on, the app watches for the acceleration of the device being picked up or
carried - which is much stronger than the acceleration of steering - and freezes the pointer for as
long as that lasts, plus a quarter of a second. The status line in the settings screen says
"Repositioning - pointer frozen" while it is holding.

It is on by default on devices without a touch screen (where the manual clutch cannot be pressed at
all) and off by default on touch devices. The switch in the settings screen overrides the default.

## Settings screen

Reached with the settings button in the toolbar; the arrow in the toolbar goes back. Changes are
applied immediately and saved when you leave the screen or close the app.

- **Status line:** the current gyro bias in degrees per second and whether the axis mapping is
  following gravity (`tilt-aware`) or using the fixed one, or a progress text while calibrating.
- **Sensitivity (%):** 25 to 300 in steps of 5, default 100. Scales the pointer speed.
- **Pointer direction:** cycles through `Inv:off`, `Inv:X`, `Inv:Y` and `Inv:XY` - for when the
  pointer moves the wrong way (a device held upside down, or a swapped axis).
- **Tilt-aware orientation:** on by default. On, gravity decides which way is up, so the same
  movement moves the pointer the same way whether the device is held upright, on its left side or on
  its right side. Off, the axes are fixed to the device. Tactility's auto-rotate setting does not
  affect it.
- **Freeze pointer while repositioning:** the auto-clutch described above.
- **Calibrate gyro:** measures the gyro bias again. Keep the device still.

## Calibration

A gyro reads a small non-zero rate when it is not moving, and that offset drifts with temperature. A
bias measured while the device sits still is subtracted from every sample, and:

- The bias is measured automatically every time the pointer is switched on. Keep the device still for
  a moment; the settings screen shows "Keep still: calibrating..." until enough samples have been
  collected. The estimate is the median of a short window (~1.5 seconds), so the moments where your
  hand moved are ignored; if the device keeps being moved the window is started again a few times
  and after ~4.5 seconds the best estimate so far is used.
- The median is deliberate: it does not assume the sensor's own bias is small. Breakout IMU modules
  (a bare MPU6886, for example) commonly sit at several degrees per second, and the datasheet allows
  up to ±20 °/s, while the sensors built into the board are usually well under 1 °/s. Any scheme that
  discards "samples above X °/s" to reject movement fails completely on a module whose *bias* is
  above X, because then every sample is discarded.
- While the device is still, the bias is slowly tracked, so it does not creep over a long session -
  including the slow drift while the module warms up.
- The **Calibrate gyro** button starts a new measurement at any time.

Calibration only fixes the bias. If the pointer still drifts after a calibration, the bias readout on
the settings screen tells you which of the two it is: a reading near 0 while the pointer creeps means
the measurement itself is being lost, while a reading that keeps changing as the module warms up is
thermal drift (the tracking above follows it, and a fresh calibration pins it again).

## Filtering and pointer movement

The pointer is not the raw gyro reading:

- A One Euro filter removes jitter when the device is nearly still while keeping the response quick
  when it is moved.
- A small deadzone ignores the movement of a hand holding the device still.
- Movement is scaled by the time since the last sample, so the speed is the same at any update rate.
- Fractions of a pixel are carried over to the next sample, so slow movements still move the pointer
  instead of being rounded away, and a single sample cannot jump the pointer more than 40 pixels.

Horizontal movement is scaled more than vertical movement, because turning the device sideways covers
more angle than tilting it up or down.

## Stored data

The app keeps one file in its user data folder (`user/app/tactility.airmouse`):

- `settings.properties`: `sens` (25-300), `inv` (0-3: X, Y or both inverted), `orient` (tilt-aware
  mapping), `autoclutch` and `mm0`..`mm8` (the mount matrix below). The file is written when you
  leave the settings screen or close the app.

### Mount matrix

A bare MPU6886 module has no devicetree entry, so nothing tells the app how it is mounted. `mm0` to
`mm8` are a row-major 3x3 matrix in the usual Linux IIO `mount-matrix` convention:

```
x' = m0*x + m1*y + m2*z
y' = m3*x + m4*y + m5*z
z' = m6*x + m7*y + m8*z
```

It maps the module's axes onto the device frame (+X right, +Y to the top of the screen, +Z out of the
screen toward the user). The module frame is right-handed, so two of its axes fix the third
(Z = X x Y). For a module lying flat against the device, the four possibilities are:

| Module X | Module Y | Module Z | mm0..mm8 |
| --- | --- | --- | --- |
| right | up (to the top of the screen) | out of the screen | `1 0 0 / 0 1 0 / 0 0 1` |
| left | up | into the screen | `-1 0 0 / 0 1 0 / 0 0 -1` |
| right | down | into the screen | `1 0 0 / 0 -1 0 / 0 0 -1` |
| left | down | out of the screen | `-1 0 0 / 0 -1 0 / 0 0 1` |

The built-in default (`kDefaultExtMountMatrix` in `main/Source/AirMouse.cpp`) is the second row: the
module flat on the back (or bottom, on a device whose screen faces up, in which case the sticker ends
up facing the floor, so the module's Z goes through the device, away from the screen) with its Y
towards the top of the screen and its X to the device's left. A module mounted in one of the other
orientations needs either that constant changed or the `mm0`..`mm8` keys set.

`mm0` to `mm8` in the settings file win over the default, so a device that already has a settings file
keeps the matrix it saved. Remove the `mm*` keys (or the file) to fall back to the default. Edit the
file while the app is closed - it is read once, when the app starts.

If the axes are right but the movement is mirrored, use **Pointer direction** in the settings instead;
the matrix only has to get the three axes into the right place.

## Limitations

- One mouse, no keyboard, no scrolling wheel.
- The pointer only moves while the app's window is on screen.
- The auto-clutch is a threshold on how hard the device is being moved: brisk steering can freeze the
  pointer, and very gentle repositioning can move it. Turn it off if that gets in the way.
- Closing the app stops the HID mouse, so the host shows the device as disconnected until the app is
  opened again.
- The app cannot start while another Bluetooth device is connected to Tactility in host mode (for
  example a paired BLE keyboard). Disconnect that device first.
- Devices without a gyroscope cannot use the app; devices without Bluetooth can open it but not pair.
