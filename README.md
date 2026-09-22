| Supported Targets | ESP32 |
| ----------------- | ----- |

# Security Dev

An ESP32 door-security monitor. It listens for BLE door sensors, shows status on a
small OLED, and pushes notifications to your phone through [ntfy.sh](https://ntfy.sh).

- **Wi-Fi provisioning** over SoftAP on first boot, with automatic reconnect afterwards
- **Configuration web page** for the device name, location, phone number, ntfy topic
  and up to 8 Bluetooth sensors
- **BLE door sensors** (IM24-BLE): open and close transitions are reported on the
  display and pushed to ntfy
- **Push notifications** on Wi-Fi connect, Wi-Fi reconnect attempts, and door events
- **Hold-to-erase** on GPIO0 to clear stored configuration and start over

## Hardware

- ESP32 development board
- SSD1306 128x64 OLED on I2C
- One or more IM24-BLE door sensors

The I2C pins are set in [`main/main.c`](main/main.c):

| Signal | GPIO |
| ------ | ---- |
| SDA    | 26   |
| SCL    | 25   |

These are hardcoded and currently override the `menuconfig` values, which sit
commented out just below them. The display address and I2C frequency do still come
from `menuconfig`.

## Build and flash

The toolchain lives under `C:\Espressif`, so activate it with the EIM profile script
rather than `export.ps1`:

```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
idf.py build
idf.py -p COM3 flash monitor
```

Bluetooth settings are in [`sdkconfig.defaults`](sdkconfig.defaults). Note that
NimBLE's central role has to stay enabled: on ESP32, `esp_nimble_cfg.h` forces host
based privacy on regardless of Kconfig, and the resulting `ble_hs_resolv.c` needs a
symbol that only the security manager provides. An observer-only build will not link.

## First run

1. On first boot the device advertises a SoftAP named `PROV_XXXXXX`. Connect to it
   with proof of possession `abcd1234` and supply your Wi-Fi credentials.
2. Once connected, the display shows the SSID and the DHCP address. Open that address
   in a browser to reach the configuration page.
3. Set a **Topic** (3 to 12 alphanumeric characters). This is your ntfy topic:
   subscribe to `https://ntfy.sh/<topic>` in the ntfy app to receive alerts.
4. Enter the Bluetooth address of each door sensor, with an optional friendly name
   used in notifications. Addresses accept 12 hex digits with or without separators.
5. Reboot so the BLE monitor picks up the new sensor list.

To wipe everything, hold GPIO0 (the BOOT button) for 5 seconds. This erases the whole
NVS partition, Wi-Fi credentials and device configuration alike, then restarts.

## How it works

```
app_main
  |
  +- display_mutex (recursive) -> u8g2 -> I2C -> SSD1306
  |
  +- erase_task ............ polls GPIO0, erases NVS on a 5s hold
  +- wifi_reconnect_task ... retries esp_wifi_connect every 5s while disconnected
  +- config_http task ...... serves the configuration page on port 80
  +- nimble host task ...... GAP callback: match MAC, decode, queue
  +- ble_sensor task ....... drains the queue, updates display, sends ntfy
  +- ntfy_http task ........ one per notification, POSTs and exits
```

Every task that draws to the screen takes `display_mutex` first. The BLE GAP callback
runs on NimBLE's host task, so it only matches and queues; the display write and the
HTTPS POST happen on `ble_sensor` where blocking is safe.

| File | Responsibility |
| ---- | -------------- |
| [`main/main.c`](main/main.c) | Display driver glue, Wi-Fi provisioning and events, ntfy notifications |
| [`main/ble_sensors.c`](main/ble_sensors.c) | BLE scanning, IM24-BLE decoding, door event reporting |
| [`main/http_server.c`](main/http_server.c) | Configuration web page and NVS-backed settings |
| [`main/eraseConfig.c`](main/eraseConfig.c) | GPIO0 hold-to-erase |

### IM24-BLE advertisement format

```
02 01 06 | 09 08 "iSensor " | 09 FF 10 49 7D DE 46 00 3B 35
      ^^   device type tag,    ^^^^^          ^^
           0x06 = door sensor  company 0x4910 state, bit 1 = open
```

The state byte is at offset 5 of the manufacturer data, counting from the company
identifier. Sensors re-advertise continuously, so only transitions are reported, with
a 1 second debounce against a rattling sensor.

To watch raw packets, the log level is already raised in `app_main`:

```c
esp_log_level_set("BLE_SENSOR", ESP_LOG_DEBUG);
```

## Troubleshooting

**Display blank** — check SDA/SCL wiring against the table above, confirm 3.3V, and try
display address 0x3D in `menuconfig` if 0x3C does not respond.

**No door events** — enable debug logging and confirm advertisements arrive from the
expected MAC. If packets arrive but nothing fires, check the device type tag is `0x06`
and re-check the state offset in `decode_door_state`.

**No notifications** — a Topic must be set on the configuration page. The device logs
`No ntfy Topic configured` when it is missing.

**Cannot reach the config page** — the provisioning server holds port 80 briefly after
handover; the server retries until it binds and logs `HTTP Server Started` when ready.

## References

- [ESP-IDF I2C API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html)
- [NimBLE API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/nimble/index.html)
- [U8G2 wiki](https://github.com/olikraus/u8g2/wiki)
- [ntfy documentation](https://docs.ntfy.sh/)
