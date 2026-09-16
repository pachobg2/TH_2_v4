# temp_humidity_sensor_v4 — self-provisioning ESP32-C3 SHTC3 sensor

A fork of [`temp_humidity_sensor`](../temp_humidity_sensor) with one core
difference: **no per-device `config.h` secrets**. WiFi, MQTT, and device
identity are set up through a small web portal the device broadcasts
itself, instead of being compiled in before you flash it. The goal is one
firmware image you can flash onto any unit and configure from a phone,
closer to how a commercial IoT device behaves, rather than editing and
rebuilding per device like the rest of this fleet.

Flashed and tested on real hardware since v4.0.0b; each Version History
entry below reflects what's actually been verified working (or fixed
after not working) on a physical unit. `WiFiManager`'s API has shifted
across versions historically -- `setCustomBodyHeader()` already turned out
to be missing on at least one installed version (see v4.2.3b) -- so if you
hit a build error on a method call here (`WiFiManagerParameter`,
`startConfigPortal`, `setConfigPortalBlocking`/`process`/
`getConfigPortalActive`, `setCustomHeadElement`, `setMenu`), double-check
it against whatever version Library Manager actually installs for you.

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
   e.g. `P@cho TH-2 XXXX` (`DEVICE_MANUFACTURER` + `DEVICE_MODEL` + the
   last 4 hex chars of the chip MAC), protected by `AP_PASSWORD` from
   `config.h` (default `setup1234`), LED blinking once a second for as
   long as the portal is open.
2. Connect to that network from your phone or laptop. A captive-portal
   landing page should open automatically (or browse to `192.168.4.1`),
   showing a small logo, a **"Device status"** box, then a short menu —
   **only one button matters here: "Configure"**. The status box is a live
   temperature/humidity/battery reading taken right as the portal opened,
   plus the last-known WiFi network and MQTT broker if this unit's been
   configured before (see [Logo and device
   status](#logo-and-device-status) below). The top of every portal page —
   including this landing one — also shows the device brand and model
   (e.g. "P@cho TH-2 Sensor") and the firmware version below it, so you
   can tell which unit and which build you're looking at without checking
   Serial or Home Assistant.
3. Tap **"Configure"**. This is the *only* page you need: pick your WiFi
   network from the scanned list (or enter one manually), then **keep
   scrolling** — a "MQTT & device settings" heading marks where the same
   form continues below the WiFi fields: broker host/port/username/
   password and a device name. Device ID defaults to an auto-generated
   `th4_XXXXXX` (stable, collision-free out of the box) — override it here
   if you want a memorable topic name instead. **MQTT broker host is a
   required field** — the browser won't let you submit the form with it
   blank, so there's no way to save the WiFi half only by mistake.
4. Save (once, for the whole form). The device connects, stores
   everything to flash, and restarts straight into normal operation. To
   also push new firmware in the same visit, hold the button 2-10s on the
   next boot (see below) instead of waiting through a separate OTA window
   here.

To reconfigure a unit later (new WiFi network, different broker), hold the
setup button past 10s while powering it on — same portal, pre-filled with
its current settings.

### Logo and device status

The portal's built-in "Info" page (generic ESP32 chip model, free heap,
uptime — none of it specific to this device) is hidden. In its place, the
landing menu page has, above the button list (via `setCustomMenuHTML()`
and a `"custom"` menu-position token — the supported way to add content
to that page, rather than the "Configure" form itself):

- A **lime-green "P@cho" logo** — a circular badge, inline SVG (a few
  hundred bytes as plain text, no separate image request or base64
  encoding needed). The landing page's own default header (the library's
  literal default title, "WiFiManager", over the AP name) is hidden in
  favor of this and the branded header text at the top of the page.
- A **"Device status"** box:
  - **Temperature / humidity / battery** — a real reading, taken by
    powering on the SHTC3 right as the portal opens (same sensor, same
    code path as a normal report cycle). This is a one-time snapshot, not
    a live dashboard — it won't update again while the page sits open.
  - **WiFi** — the last network this unit successfully connected to, or
    "not yet configured" / "no WiFi saved yet" if it hasn't got one.
  - **MQTT broker** — the last-saved broker host:port, or "not yet
    configured" if none is saved.
  - **Boot count / connect fails** — the same diagnostic counters
    published to HA each cycle (today's count and the lifetime total).

### Factory reset

While the setup portal is open (LED blinking), hold the setup button again
for `FACTORY_RESET_HOLD_MS` (5s by default) — no browser needed. This
wipes both this device's saved settings and the ESP32 radio's own
persisted WiFi credentials, then restarts into a fully unconfigured
state, equivalent to a fresh, never-set-up unit.

This used to be a checkbox on the Configure page instead; it's a
button-hold now because the checkbox never reliably worked (see v4.2.0b
through v4.2.4b) — WiFiManager's custom-attribute mechanism for adding a
checkbox ends up emitting a duplicate HTML `value` attribute on the input,
so the "checked" value never actually made it into the submitted form
correctly, and separately, checking it still required the page's WiFi
fields to resolve one way or another (connect or fail) before the
checkbox was ever even looked at. The button hold has neither problem.

This is different from the portal's built-in **"Erase"** menu button
(hidden in this build to avoid the two being confused): that one only
clears the radio's WiFi credentials and leaves this project's own settings
untouched, which looks like a reset but isn't one.

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
full topic table, including the "Last Full Charge" diagnostic (shared
across every battery sensor in this fleet). The only addition is that
`<device_id>` and the "friendly name" shown in Home Assistant are both set
through the portal instead of `config.h`.

### LED brightness

Unlike the rest of this fleet, where `LED_BRIGHTNESS_PCT` in `config.h` is
fixed at compile time, v4 exposes it to Home Assistant as a **"LED
Brightness"** number entity (0-100%, `homeassistant/number/...`) —
`config.h`'s value is only the initial default for a never-configured
device. Changing the slider in HA is picked up on the device's next wake
(it applies the retained command, then echoes the new value back as
state), same latency as the "OTA Request" switch, since the device is
asleep the rest of the time. The value is saved to flash (NVS), so it
survives power loss and firmware updates, and is reset back to the
`config.h` default only by a factory reset.

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

**Still in beta.** `FIRMWARE_VERSION` carries a `b` suffix until this
project is declared stable, and the running firmware genuinely reports
that suffixed string (over MQTT, in HA, on the portal page) -- it isn't
just this table's bookkeeping. An earlier attempt to reset it to a clean
`4.0.0` (see the now-relabeled v4.4.1b-v4.4.3b below) turned out to be
premature, so versioning continues from v4.4.0b instead of restarting the
numbering.

| Version | Date | Changes |
|---|---|---|
| v4.0.0b | 2026-09-14 | Initial fork of `temp_humidity_sensor`: WiFi/MQTT/device-identity moved from compiled `config.h` to a runtime WiFiManager-based setup portal (AP broadcast, network scan, custom MQTT/device-name fields), auto-generated stable device ID, combined setup+OTA button flow. All sensor/battery/LED/diagnostic behavior otherwise unchanged from `temp_humidity_sensor`. |
| v4.0.1b | 2026-09-14 | Fixed the setup network silently never appearing when `AP_PASSWORD` is under 8 characters (a hard WPA2 minimum -- `WiFi.softAP()` just fails with no visible error). Now detects this and falls back to an open network instead of failing silently; README/config.h.example call out the requirement explicitly. Also documented the "USB CDC On Boot" board setting needed for Serial to work at all on native-USB boards. |
| v4.0.2b | 2026-09-14 | Removed the `wm.autoConnect()` step, which tried the ESP32 WiFi driver's own chip-wide last-saved network before falling back to the portal -- that's separate from and invisible to this project's own settings, so on a reused dev board it wasted a real ~60s connect-timeout trying a stale network from a completely different project before the setup AP ever appeared. Goes straight to `startConfigPortal()` now, since this function is only ever reached when there's no known-good config to try in the first place. |
| v4.1.0b | 2026-09-14 | Setup button is now hold-duration sensitive instead of a single on/off press: released quickly is a normal cycle, held 2-10s opens a local OTA-only window (no portal, LED solid on), held past 10s opens the full setup portal (LED now blinks once a second instead of sitting solid, via a new non-blocking `WiFiManager` loop). An unconfigured device still always goes straight to the portal regardless of hold duration. |
| v4.1.1b | 2026-09-14 | Fixed a build error from v4.1.0b (`'ButtonHoldMode' was not declared in this scope`): the Arduino IDE auto-generates prototypes for functions that don't already have one and inserts them near the top of the file, before custom types defined further down are visible. Moved the `ButtonHoldMode` enum itself up next to the `Settings` struct, right after the includes, so it's already declared by the time those prototypes are generated. No behavior change. |
| v4.1.2b | 2026-09-14 | Removed the automatic 5-minute `ArduinoOTA` window that ran after every successful setup-portal save -- redundant now that a 2-10s button hold opens a dedicated OTA-only window on its own, and it made every provisioning visit wait out an unused window before restarting. The portal now saves and restarts straight into normal operation. |
| v4.2.0b | 2026-09-14 | Two setup-portal changes: (1) saving with an empty MQTT host no longer marks the device "configured" -- it keeps WiFi/other fields and goes straight back to the portal next boot instead of silently becoming a unit that connects but can never publish. (2) Added a "Factory reset" checkbox to the portal that wipes both this project's saved settings and the ESP32 radio's own WiFi credentials, and hid the portal's built-in "Erase" menu button (which only clears the radio's WiFi credentials, not our settings, and read as a half-working reset). |
| v4.2.1b | 2026-09-14 | Fixed the v4.2.0b factory reset checkbox doing nothing on real hardware: it only ran after a successful WiFi (re)connect, but the portal never pre-fills the WiFi password field, so a save without retyping it fails to connect and silently skipped the reset too. Now fires as soon as the box is checked and Save is hit, independent of whether WiFi reconnects, and always restarts afterward. Also fixed a related bug where the checkbox's unsubmitted default value equaled its "checked" value, which could in principle have triggered a false-positive wipe on a portal timeout. |
| v4.2.2b | 2026-09-14 | Added the device model and firmware version to the top of every setup-portal page (`wm.setCustomBodyHeader()`), including the first page you land on -- no more guessing which build a unit is running without checking Serial or Home Assistant. |
| v4.2.3b | 2026-09-14 | Fixed a build error from v4.2.2b: `setCustomBodyHeader()` doesn't exist on the installed WiFiManager version (`'class WiFiManager' has no member named 'setCustomBodyHeader'`). Switched to `setCustomHeadElement()` -- a much older, more consistently-available API -- injecting the same version text via a CSS `body::before` instead. Same visible result, no behavior change. |
| v4.2.4b | 2026-09-14 | Fixed the v4.2.1b factory reset fix still not firing on real hardware: the checkbox value was only checked *after* the portal wait loop exited, but that loop doesn't exit promptly on a failed WiFi connect (e.g. an intentionally-blank password) -- WiFiManager just keeps the portal open and retries, so it could sit there for the full 10-minute portal timeout before the checkbox was ever looked at. Now polled every loop iteration, exiting immediately once the box is checked and Save is hit, regardless of WiFi outcome. |
| v4.3.0b | 2026-09-14 | Replaced the factory reset checkbox with a button hold: still not firing on real hardware after two rounds of fixes, and the actual root cause turned out to be WiFiManager's custom-attribute mechanism itself -- adding `value="1"` via a checkbox's custom-attribute string collides with the framework's own `value=''` attribute on the same generated `<input>` tag, so the checked state never made it into the submitted form correctly in the first place. Replaced entirely: holding the setup button again for `FACTORY_RESET_HOLD_MS` (5s) while the portal is open now triggers the reset directly, no web form involved. |
| v4.4.0b | 2026-09-14 | Added a "LED Brightness" number entity in Home Assistant (0-100%, persisted in NVS, applied on the device's next wake) -- `config.h`'s `LED_BRIGHTNESS_PCT` is now only the initial default for a never-configured unit rather than a fixed value. Also added a device brand/model line ("P@cho TH-2 Sensor") above the firmware version at the top of every setup-portal page. Confirmed the "Last Full Charge" diagnostic (shared with the rest of the battery-powered fleet) has been present since v4.0.0b. |
| v4.4.1b | 2026-09-14 | **`FIRMWARE_VERSION` briefly reset to a clean `4.0.0`, declaring the beta cycle over -- premature (see v4.4.4b); relabeled here to keep the `b` sequence unbroken.** No code change from v4.4.0b. |
| v4.4.2b | 2026-09-14 | Attempted fix for the setup portal reading as two separate steps, based on a wrong assumption that MQTT/device fields were already on the same "Configure WiFi" form as the WiFi picker -- added a heading and a `required` attribute on the MQTT host field to make that (supposedly) already-combined form clearer. Didn't fix anything: on real hardware these were genuinely two separate pages ("Configure WiFi" for WiFi only, a separate "Setup" menu entry for MQTT only), not one form with a scroll. See v4.4.3b. |
| v4.4.3b | 2026-09-14 | Actually fixed the two-page setup portal this time: WiFiManager's own docs warn that `setParamsPage()` and a custom `setMenu()` "should not be combined" -- our `setMenu()` list included `"param"` as its own menu entry (a leftover from before the factory reset checkbox was replaced by a button hold), which was overriding the library's default of rendering `addParameter()` fields directly on the "Configure WiFi" page. Removed `"param"` from the menu; the WiFi picker and MQTT/device fields are now genuinely one page, one form, one Save -- confirmed this was the real root cause, not a scrolling/visibility issue. |
| v4.4.4b | 2026-09-14 | Explicitly called `wm.setCaptivePortalEnable(true)` (already the library default, but now not relying on that default across versions) after a report of the captive-portal page not auto-opening on connecting to the AP. Confirmed via WiFiManager's own source that the DNS redirect responsible for that starts immediately in `startConfigPortal()` and is serviced frequently enough by our ~10ms portal loop either way -- the far more likely explanation is OS-side captive-portal-result caching for this exact AP name (same every boot, derived from the chip ID), not a firmware bug. Browsing to `192.168.4.1` manually always works regardless. Also: `FIRMWARE_VERSION` reset to `4.4.4b`, retracting the premature "stable" declaration at v4.4.1b. |
| v4.5.0b | 2026-09-15 | Replaced the portal's built-in "Info" page (generic ESP32 chip/heap/uptime diagnostics -- no public WiFiManager API to customize its content, only to hide its optional buttons) with a "Device status" section on the "Configure WiFi" page: a live temperature/humidity/battery reading taken as the portal opens (extracted the sensor read into a shared `readSensor()` helper, also used by the normal report cycle), plus last-known WiFi network and MQTT broker, plus boot/connect-fail counters. Snapshot only, not a live-updating dashboard. |
| v4.6.0b | 2026-09-15 | Two portal changes: (1) moved the "Device status" box off the "Configure WiFi" form and onto the landing menu page instead, below the button list -- via `setCustomMenuHTML()` and a `"custom"` menu-position token, the actual supported mechanism for this rather than a `WiFiManagerParameter`. (2) Relabeled the "Configure WiFi" button to just "Configure" (it covers WiFi and MQTT/device settings together, so the old label undersold it) -- WiFiManager has no button-label API, so this hides the button's real text via CSS and injects replacement text with `::after`, targeting it by its parent form's `action='/wifi'` (confirmed against the library's actual generated markup, not guessed). |
| v4.6.1b | 2026-09-16 | Added a small lime-green "P@cho" circular-badge logo (inline SVG, a few hundred bytes -- no base64 encoding or separate image request needed) to the landing menu page, and moved the "custom" menu-position token to the front of the menu order so the logo and "Device status" box now show up above the button list instead of below it. |
| v4.6.2b | 2026-09-16 | Enlarged the logo (64px to 120px) and hid the landing page's own default header. Confirmed via WiFiManager's source that only `handleRoot()` (the landing page) renders `<h1>{title}</h1><h3>{apName}</h3>` -- the literal text "WiFiManager" (the library's own default title, never changed here) over the AP name "TempSensorV4-XXXXXX" -- and that the "Configure" page builds its header differently, so hiding `h1`/`h3` via CSS is safe and doesn't affect it. |
| v4.6.3b | 2026-09-16 | Renamed the setup AP from `TempSensorV4-XXXXXX` (6 hex chars of the chip ID) to `<DEVICE_MANUFACTURER> <DEVICE_MODEL> XXXX` (e.g. `P@cho TH-2 XXXX`, last 4 hex chars of the chip MAC) -- built from the existing config.h identity constants rather than a hardcoded project name, and matches the branding already shown elsewhere in the portal. |
