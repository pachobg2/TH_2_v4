# temp_humidity_sensor_v4 — self-provisioning ESP32-C3 SHTC3 sensor

A fork of [`temp_humidity_sensor`](../temp_humidity_sensor) with one core
difference: **no per-device `config.h` secrets**. WiFi, MQTT, and device
identity are set up through a small web portal the device broadcasts
itself, instead of being compiled in before you flash it. The goal is one
firmware image you can flash onto any unit and configure from a phone,
closer to how a commercial IoT device behaves, rather than editing and
rebuilding per device like the rest of this fleet.

Flashed and tested on real hardware since v4.0.0; each Version History
entry below reflects what's actually been verified working (or fixed
after not working) on a physical unit. `WiFiManager`'s API has shifted
across versions historically, so if you hit a build error on a method
call here (`WiFiManagerParameter`, `startConfigPortal`,
`setConfigPortalBlocking`/`process`/`getConfigPortalActive`,
`setCustomBodyHeader`, `setMenu`), double-check it against whatever
version Library Manager actually installs for you.

## Files

- `temp_humidity_sensor_v4.ino` — the sketch.
- `config.h.example` — copy to `config.h`. Unlike the rest of this fleet,
  this file has no per-device secrets in it (no WiFi/MQTT/device fields) —
  just hardware pins, firmware identity, and two fleet-wide values
  (`OTA_PASSWORD`, `AP_PASSWORD`) you might want to change from the shared
  default. Keep `config.h` out of git (already covered by `.gitignore`).

## Setup button behavior

The button on `SETUP_PIN` is hold-duration sensitive:

- **Released quickly** (under `BUTTON_OTA_HOLD_MS`, 2s): not a deliberate
  hold, treated as no press at all — normal report cycle.
- **Held 2-10s**: opens a local OTA-only window (LED solid on) using the
  already-saved WiFi credentials — no portal, just a chance to push new
  firmware without walking over to a laptop. Same idea as the remote
  MQTT "OTA Request" switch, just triggered by the button instead.
- **Held past 10s**: opens the full setup portal (LED blinking once a
  second, `SETUP_LED_BLINK_MS`) — see below.

**First power-on** always goes straight to the full setup portal
regardless of hold duration, since there are no saved WiFi credentials yet
for the OTA-only path to use.

## How setup works

1. **First power-on** (or holding the setup button past 10s at boot on an
   already-configured unit): the device broadcasts its own WiFi network,
   `TempSensorV4-XXXXXX` (last 6 hex digits of its chip ID), protected by
   `AP_PASSWORD` from `config.h` (default `setup1234`), LED blinking once
   a second for as long as the portal is open.
2. Connect to that network from your phone or laptop. A captive-portal
   page should open automatically (or browse to `192.168.4.1`). The device
   model and firmware version are shown at the top of every portal page,
   including this first one, so you can tell which build a unit is running
   without checking Serial or Home Assistant.
3. Pick your WiFi network from the scanned list (or enter one manually),
   plus fill in your MQTT broker host/port/username/password and a device
   name. Device ID defaults to an auto-generated `th4_XXXXXX` (stable,
   collision-free out of the box) — override it here if you want a
   memorable topic name instead. **MQTT broker host is required** — saving
   with it blank connects to WiFi but leaves the device unconfigured (see
   below), rather than restarting into a state where it can never publish.
4. Save. The device connects, stores everything to flash, and restarts
   straight into normal operation. To also push new firmware in the same
   visit, hold the button 2-10s on the next boot (see below) instead of
   waiting through a separate OTA window here.

To reconfigure a unit later (new WiFi network, different broker), hold the
setup button past 10s while powering it on — same portal, pre-filled with
its current settings.

If the MQTT host field is left blank when you save, the device still
connects to WiFi (and keeps whatever else you entered) but does **not**
mark itself configured — the next boot goes straight back to the portal
on its own, no button hold needed, instead of restarting into a unit that
connects but can never publish anything.

### Factory reset

The portal has a **"Factory reset"** checkbox (in the Configure WiFi page,
alongside the MQTT/device fields). Checking it and saving wipes this
device's saved settings *and* the ESP32 radio's own persisted WiFi
credentials, then restarts into a fully unconfigured state — equivalent to
a fresh, never-set-up unit. Unlike the rest of that page, the reset fires
regardless of whether the WiFi fields on the same page are filled in or
manage to (re-)connect — you don't need to retype the WiFi password (the
page never pre-fills it) just to check the box and hit Save. This is
different from the portal's built-in **"Erase"** menu button (hidden in
this build to avoid the two being confused): that one only clears the
radio's WiFi credentials and leaves this project's own settings untouched,
which looks like a reset but isn't
one.

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
3. Select board **"ESP32C3 Dev Module"**, and under Tools set **USB CDC On
   Boot: Enabled** if your board uses native USB for Serial (e.g. a
   DevKitM-1) — without it, Serial Monitor shows nothing at all, no matter
   what the firmware does. If you see no serial output whatsoever after
   flashing, check this first.
4. Copy `config.h.example` to `config.h`. The defaults work as-is for a
   first flash — WiFi/MQTT are configured later, from the device itself.
   **`AP_PASSWORD` must be 8-63 characters or left as `""`** — WPA2 rejects
   anything shorter and the setup network simply never appears (the
   firmware now falls back to an open network if you get this wrong, but
   don't rely on that).

## MQTT / Home Assistant

Base topic: `home/<device_id>/...` (device ID set during setup, default
`th4_XXXXXX`). Topic layout, HA discovery, and `expire_after` behavior are
identical to `temp_humidity_sensor` — see that project's README for the
full topic table. The only addition is that `<device_id>` and the "friendly
name" shown in Home Assistant are both set through the portal instead of
`config.h`.

## OTA updates

Three independent paths:

- **Physical button, held 2-10s**: OTA-only window (LED solid on), no
  portal — connects with already-saved WiFi credentials directly. The
  quickest way to push firmware to an already-configured unit.
- **Physical button, held past 10s**: opens the setup portal → on
  success, saves and restarts straight into normal operation (no OTA
  window here). Use this if you need to change settings or provision a
  brand-new unit; hold 2-10s on the next boot if you also want to push
  firmware.
- **Remote (MQTT)**: flip the retained "OTA Request" switch in Home
  Assistant. This does *not* go through the portal or need physical
  access — the device is already configured and connected, so it just
  opens an OTA window directly, exactly like `temp_humidity_sensor`.

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
| v4.0.1 | 2026-09-14 | Fixed the setup network silently never appearing when `AP_PASSWORD` is under 8 characters (a hard WPA2 minimum -- `WiFi.softAP()` just fails with no visible error). Now detects this and falls back to an open network instead of failing silently; README/config.h.example call out the requirement explicitly. Also documented the "USB CDC On Boot" board setting needed for Serial to work at all on native-USB boards. |
| v4.0.2 | 2026-09-14 | Removed the `wm.autoConnect()` step, which tried the ESP32 WiFi driver's own chip-wide last-saved network before falling back to the portal -- that's separate from and invisible to this project's own settings, so on a reused dev board it wasted a real ~60s connect-timeout trying a stale network from a completely different project before the setup AP ever appeared. Goes straight to `startConfigPortal()` now, since this function is only ever reached when there's no known-good config to try in the first place. |
| v4.1.0 | 2026-09-14 | Setup button is now hold-duration sensitive instead of a single on/off press: released quickly is a normal cycle, held 2-10s opens a local OTA-only window (no portal, LED solid on), held past 10s opens the full setup portal (LED now blinks once a second instead of sitting solid, via a new non-blocking `WiFiManager` loop). An unconfigured device still always goes straight to the portal regardless of hold duration. |
| v4.1.1 | 2026-09-14 | Fixed a build error from v4.1.0 (`'ButtonHoldMode' was not declared in this scope`): the Arduino IDE auto-generates prototypes for functions that don't already have one and inserts them near the top of the file, before custom types defined further down are visible. Moved the `ButtonHoldMode` enum itself up next to the `Settings` struct, right after the includes, so it's already declared by the time those prototypes are generated. No behavior change. |
| v4.1.2 | 2026-09-14 | Removed the automatic 5-minute `ArduinoOTA` window that ran after every successful setup-portal save -- redundant now that a 2-10s button hold opens a dedicated OTA-only window on its own, and it made every provisioning visit wait out an unused window before restarting. The portal now saves and restarts straight into normal operation. |
| v4.2.0 | 2026-09-14 | Two setup-portal changes: (1) saving with an empty MQTT host no longer marks the device "configured" -- it keeps WiFi/other fields and goes straight back to the portal next boot instead of silently becoming a unit that connects but can never publish. (2) Added a "Factory reset" checkbox to the portal that wipes both this project's saved settings and the ESP32 radio's own WiFi credentials, and hid the portal's built-in "Erase" menu button (which only clears the radio's WiFi credentials, not our settings, and read as a half-working reset). |
| v4.2.1 | 2026-09-14 | Fixed the v4.2.0 factory reset checkbox doing nothing on real hardware: it only ran after a successful WiFi (re)connect, but the portal never pre-fills the WiFi password field, so a save without retyping it fails to connect and silently skipped the reset too. Now fires as soon as the box is checked and Save is hit, independent of whether WiFi reconnects, and always restarts afterward. Also fixed a related bug where the checkbox's unsubmitted default value equaled its "checked" value, which could in principle have triggered a false-positive wipe on a portal timeout. |
| v4.2.2 | 2026-09-14 | Added the device model and firmware version to the top of every setup-portal page (`wm.setCustomBodyHeader()`), including the first page you land on -- no more guessing which build a unit is running without checking Serial or Home Assistant. |
