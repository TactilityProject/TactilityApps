# Web Server

A web dashboard for the device: a file browser, an app manager, WiFi and system information.
The package holds two binaries.

- `webserver-module` is a kernel module with the HTTP server. It starts at boot and serves the dashboard when the server is enabled in the settings.
- `webserver` is the settings app. It turns the server on and off, shows the address and sets the authentication.

The server runs in one of two modes, chosen in the settings app:
- **Station:** the server uses the WiFi connection of the device. Connect to a network with the WiFi settings first.
- **Access point:** the device becomes a WiFi network and the server is at `http://192.168.4.1`. The network name (SSID) is `Tactility-XXXX` by default, with a generated password. The password has 8 to 63 characters, unless the network is open.

The settings are saved in the user data folder of the package, `webserver.properties`. They start disabled. A new mode, port or access point setting applies when the server starts the next time. The settings app restarts a running server when it closes.

## Endpoints

| Endpoint | |
|---|---|
| `GET /`, `GET /filebrowser` | Redirects to the dashboard |
| `GET /api/sysinfo` | System information, without authentication |
| `GET /api/apps`, `/api/wifi` | Installed apps and WiFi status |
| `POST /api/apps/run?id=`, `/api/apps/uninstall?id=` | Starts or uninstalls an app |
| `PUT /api/apps/install` | Installs an app from a multipart upload |
| `GET /fs/list`, `/fs/download`, `/fs/tree` | Reads files in `/data` and `/sdcard` |
| `POST /fs/upload`, `/fs/mkdir`, `/fs/delete`, `/fs/rename` | Changes files in `/data` and `/sdcard` |
| `POST /admin/reboot` | Restarts the device |
| `GET /dashboard.html`, `/version.json`, `/favicon.ico` | The files in `assets/` |
| `GET /*` | Files in `/sdcard/tactility/webserver` |

With authentication enabled, every endpoint except `/api/sysinfo` and the redirects needs HTTP basic authentication.

## Differences with the web server of the firmware

This package is a port of the web server service and settings of the firmware, which can be removed once this is in use.

- No screenshot endpoint. `/api/sysinfo` reports it as disabled.
- `/api/sysinfo` has no chip details, flash size, storage sizes or minimum free heap, and the firmware version is "unknown". Modules can't read these.
- The settings are separate from the firmware's, so the server starts disabled until it is enabled in this app.
- Both servers use port 80 by default. Only enable one of them at a time. They also can't both run an access point.
- The access point has no setting for the SSID or the channel in the app. They are in `webserver.properties` (`apSsid`, `apChannel`).

## Using the API from another app

The module exports the functions of [common/webserver.h](common/webserver.h) for apps in the same package: `webserver_settings_load()`, `webserver_settings_save()`, `webserver_is_enabled()` and `webserver_set_enabled()`.
The access point is started through the Wi-Fi API of the kernel (`wifi_access_point_start()` in `tactility/drivers/wifi.h`).
The settings app can only run while the module is running.
