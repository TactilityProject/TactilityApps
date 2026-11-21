# Screensaver

The screen timeout and the screensavers of the device. The package holds two binaries.

- `screensaver-module` is a kernel module. It starts at boot and watches the display for inactivity. After the timeout it shows the screensaver, and after 5 minutes on it the animation stops and the backlight turns off. Touching the screen brings the display back with the brightness it had.
- `screensaver` is the settings app. It turns the screensaver on and off, sets the timeout, chooses the screensaver and previews it.

Screensavers: a black screen (the backlight goes off right away), Bouncing Balls, Mystify, Matrix Rain and StackChan.

The settings are saved in the user data folder of the package, `screensaver.properties`. The screensaver starts disabled.

The module needs LVGL to be running when it starts. The firmware no longer has a screen timeout or screensavers of its own.
