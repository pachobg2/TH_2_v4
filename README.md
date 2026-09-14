# temp_humidity_sensor_v4 — self-provisioning ESP32-C3 SHTC3 sensor

A fork of [`temp_humidity_sensor`](../temp_humidity_sensor) with one core
difference: **no per-device `config.h` secrets**. WiFi, MQTT, and device
identity are set up through a small web portal the device broadcasts
itself, instead of being compiled in before you flash it. The goal is one
firmware image you can flash onto any unit and configure from a phone,
closer to how a commercial IoT device behaves, rather than editing and
rebuilding per device like the rest of this fleet.

**Not yet flashed to real hardware.** This was written without access to
a WiFiManager-equipped board to test against — treat the first bring-up
like any new sketch here: watch the Serial Monitor across a full boot,
and double-check `WiFiManagerParameter`/`autoConnect`/`startConfigPortal`
against whatever WiFiManager version Library Manager actually installs,
since its API has shifted across versions.

## Files

- `temp_humidity_sensor_v4.ino` — the sketch.
- `config.h.example` — copy to `config.h`. Unlike the rest of this fleet,
  this file has no per-device secrets in it (no WiFi/MQTT/device fields) —
  just hardware pins, firmware identity, and two fleet-wide values
  (`OTA_PASSWORD`, `AP_PASSWORD`) you might want to change from the shared
  default. Keep `config.h` out of git (already covered by `.gitignore`).

## How setup works

1. **First power-on** (or holding the setup button at boot on an
   already-configured unit): the device broadcasts its own WiFi network,
   `TempSensorV4-XXXXXX` (last 6 hex digits of its chip ID), protected by
   `AP_PASSWORD` from `config.h` (default `setup1234`).
2. Connect to that network from your phone or laptop. A captive-portal
   page should open automatically (or browse to `192.168.4.1`).
3. Pick your WiFi network from the scanned list (or enter one manually),
   plus fill in your MQTT broker host/port/username/password and a device
   name. Device ID defaults to an auto-generated `th4_XXXXXX` (stable,
   collision-free out of the box) — override it here if you want a
   memorable topic name instead.
4. Save. The device connects, stores everything to flash, opens a brief
   `ArduinoOTA` window (in case you want to push newer firmware in the
   same visit), then restarts into normal operation.

To reconfigure a unit later (new WiFi network, different broker), hold the
setup button while powering it on — same portal, pre-filled with its
current settings.

## Hardware

Same as `temp_humidity_sensor`: SHTC3 on I2C (SDA GPIO4, SCL GPIO5),
sensor power switch GPIO3, status LED GPIO7 (LEDC PWM, brightness via
`LED_BRIGHTNESS_PCT`), battery divider on GPIO1 (ADC), setup/OTA button on
GPIO0 (active-low, external ~10k pull-up recommended alongside the
internal one — see the wiring note at the top of the `.ino`).

## Before building

1. **Arduino IDE board package**: "esp32 by Espressif Systems", core 3.x+.
2. **Libraries** (Library Manager):
   - `espMqttClient` by bertmelis
   - `Adafruit SHTC3` (+ Adafruit BusIO, Adafruit Unified Sensor)
   - **`WiFiManager` by tzapu** — the setup portal itself. This is the one
     new dependency versus the rest of this fleet.
   - **`AsyncTCP` by ESP32Async** — not used directly, but Arduino IDE
     compiles every `.cpp` in a library's `src/` folder, and
     `espMqttClient` ships an async transport that unconditionally
     `#include`s it. Without it installed you'll hit a missing-header
     build error even though nothing here calls into it (same gotcha
     documented in `toshiba_ac_bridge`'s README).
   - `ArduinoOTA`, `Preferences` (bundled with the ESP32 core)
3. Select board **"ESP32C3 Dev Module"**.
4. Copy `config.h.example` to `config.h`. The defaults work as-is for a
   first flash — WiFi/MQTT are configured later, from the device itself.

## MQTT / Home Assistant

Base topic: `home/<device_id>/...` (device ID set during setup, default
`th4_XXXXXX`). Topic layout, HA discovery, and `expire_after` behavior are
identical to `temp_humidity_sensor` — see that project's README for the
full topic table. The only addition is that `<device_id>` and the "friendly
name" shown in Home Assistant are both set through the portal instead of
`config.h`.

## OTA updates

Two independent paths, same as the rest of this fleet's battery sensors:

- **Physical button**: hold at boot → opens the setup portal → on success,
  a normal `ArduinoOTA` window follows automatically before the restart.
  Use this even if you only want to push firmware, not change settings —
  just click through the portal with the existing values.
- **Remote (MQTT)**: flip the retained "OTA Request" switch in Home
  Assistant. This does *not* go through the portal — the device is
  already configured and connected, so it just opens an OTA window
  directly, exactly like `temp_humidity_sensor`.

## Config file

`config.h` holds only hardware pins, firmware identity
(`DEVICE_MANUFACTURER`/`MODEL`/`HW_VERSION`/`FIRMWARE_VERSION`), battery
calibration, timing, and the two fleet-wide passwords (`OTA_PASSWORD`,
`AP_PASSWORD`). Battery calibration (`BATT_CAL`, `BATT_DIVIDER_RATIO`) is
still genuinely per-board — measure your own unit's raw-vs-actual voltage
before trusting precise battery %, same as every other battery sensor
here.

## Version History

| Version | Date | Changes |
|---|---|---|
| v4.0.0 | 2026-09-14 | Initial fork of `temp_humidity_sensor`: WiFi/MQTT/device-identity moved from compiled `config.h` to a runtime WiFiManager-based setup portal (AP broadcast, network scan, custom MQTT/device-name fields), auto-generated stable device ID, combined setup+OTA button flow. All sensor/battery/LED/diagnostic behavior otherwise unchanged from `temp_humidity_sensor`. |
