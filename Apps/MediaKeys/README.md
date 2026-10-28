# Media Keys

Turns the device into a Bluetooth LE media remote: six on-screen buttons for previous, play/pause,
next, mute, volume down and volume up, sent as HID Consumer Control reports. A host pairs it like any
other Bluetooth remote.

## Requirements

- **Bluetooth.** The Bluetooth device has to be enabled in the devicetree. The radio itself may be
  off: switching the remote on turns it on, and the app turns it back off when it exits if it was
  off before. If the radio is already on when the app opens, the remote switches on by itself.
- **A host.** Buttons are ignored while no host is connected.

## Pairing

The app advertises as `Tactility Media Keys` with its own Bluetooth identity (address and bond), so a
host pairs it as a separate device from the other Tactility Bluetooth apps (Air Mouse, MIDI). Pair it
once per host. Switching apps does not need a new pairing.

## Main screen

The toolbar has the title and a switch.

- **Switch:** turns the remote on and off. On means: Bluetooth radio on, HID keyboard/consumer
  profile started, advertising, and the buttons sending consumer reports. Switching it off stops the
  profile and puts the radio back the way it was. Opening the app while the radio is already on
  switches it on.
- **Buttons:** previous track, play/pause, next track, mute, volume down, volume up. They do nothing
  when no host is connected.
- When the Bluetooth radio is turned off elsewhere (Tactility's own settings), the switch turns
  itself off.

## Physical keyboard

When a keyboard device is active, the app also accepts keys while the remote is on:

| Key | Action |
| --- | --- |
| `b` | Previous track |
| `p` | Play/pause |
| `n` | Next track |
| `m` | Mute |
| `d` | Volume down |
| `u` | Volume up |

`Q` or `Esc` leaves keyboard mode so the keyboard can navigate the UI again. Switch the remote off
and on to re-enter it.

## Limitations

- Media keys only: no mouse, and the keyboard part of the HID profile is not used.
- The app cannot start while another Bluetooth device is connected to Tactility in host mode (for
  example a paired BLE keyboard). Disconnect that device first.
- Closing the app stops the HID profile, so the host shows the device as disconnected until the app
  is opened again.
- Devices without Bluetooth can open the app but cannot pair.
